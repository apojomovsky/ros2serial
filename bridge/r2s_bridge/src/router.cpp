// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// The link between the MCU's topics and the ROS graph: generic publishers
// and subscriptions under the announced names, paced host-to-MCU traffic,
// and converted payloads both directions.
#include "r2s_bridge/router.hpp"

#include <algorithm>
#include <array>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <rcl_interfaces/msg/parameter_descriptor.hpp>
#include <rcl_interfaces/msg/set_parameters_result.hpp>
#include <rclcpp/create_generic_client.hpp>
#include <rclcpp/create_generic_publisher.hpp>
#include <rclcpp/create_generic_subscription.hpp>

#if __has_include(<rclcpp/generic_service.hpp>)
#include <rclcpp/create_generic_service.hpp>
#include <rclcpp/generic_service.hpp>
#define R2S_HAS_GENERIC_SERVICE 1
#else
#include <rcl/error_handling.h>
#include <rcl/service.h>
#endif

#include "r2s_bridge/converter.hpp"
#include "r2s_bridge/service_types.hpp"
#include "r2s_bridge/type_support.hpp"
extern "C" {
#include "r2s/r2s.h"
}

namespace r2s_bridge
{
namespace
{
constexpr std::uint8_t kReliableFlag = 0x08u;

// How often a poll retries a route missing despite an ok verdict, and how
// many poll attempts one handshake episode gets on top of the handshake's
// own: a transient creation failure heals within one spacing, while a
// deterministically uncreatable name stops retrying after its episode.
constexpr int kRepairSpacingMs = 100;
constexpr int kMaxRepairAttempts = 10;

// The MCU call timeout ([SVR-8]): a call with no answer in this long is
// reported to its caller, and its bookkeeping pruned.
constexpr std::int64_t kServiceCallTimeoutMs = 5000;

// How many ROS calls one service queues behind its in-flight one before the
// router answers the newest straight away.
constexpr std::size_t kMaxQueuedRosCalls = 3;

// One wire status the router answers an MCU call with when it cannot serve
// it ([SVR-6]).
constexpr std::uint8_t kSvcStatusOk = 0u;
constexpr std::uint8_t kSvcStatusBadPayload = 2u;
constexpr std::uint8_t kSvcStatusBusy = 3u;

// The ROS request identity the answer path carries: shared on the generic
// service, a copied struct on the rcl fallback.
#if defined(R2S_HAS_GENERIC_SERVICE)
using RosCallHeader = std::shared_ptr<rmw_request_id_t>;
#else
using RosCallHeader = rmw_request_id_t;
#endif

// The link state the H8 snapshot reports, one word per state. The table order
// follows the SessionState enumerators, so no branch can go untaken.
std::string link_state_name(SessionState state)
{
  static constexpr const char *names[] = {"disconnected", "boot_wait", "handshaking", "connected"};
  return names[static_cast<std::size_t>(state)];
}
const char * fail_name(ConvertFail fail)
{
  switch (fail) {
    case ConvertFail::kNone:
      return "ok";
    case ConvertFail::kUnknownType:
      return "unknown type";
    case ConvertFail::kUnsupportedField:
      return "unsupported field";
    case ConvertFail::kTruncated:
      return "truncated";
    case ConvertFail::kTrailing:
      return "trailing bytes";
    case ConvertFail::kBadVarint:
      return "bad varint";
    case ConvertFail::kBadBool:
      return "bad bool";
    case ConvertFail::kNulInString:
      return "NUL in string";
    case ConvertFail::kOverBound:
      return "over bound";
  }
  return "unknown";
}

// The parameters channel types ([PAR-2]) and the read-only flag ([PAR-4]).
constexpr std::uint8_t kParamBool = 0u;
constexpr std::uint8_t kParamI64 = 1u;
constexpr std::uint8_t kParamF64 = 2u;
constexpr std::uint8_t kParamString = 3u;
constexpr std::uint8_t kParamReadOnly = 0x01u;

// One walk type as its ROS counterpart. The walk keeps types 0 to 3
// ([PAR-3]), so every index below names a valid entry.
constexpr rclcpp::ParameterType kRosType[] = {
  rclcpp::ParameterType::PARAMETER_BOOL, rclcpp::ParameterType::PARAMETER_INTEGER,
  rclcpp::ParameterType::PARAMETER_DOUBLE, rclcpp::ParameterType::PARAMETER_STRING};

// One MCU STATUS code as its log name. Codes 1 to 4 reach here: applied
// stays quiet above, and the session drops anything past 4 ([PAR-5]).
const char * param_status_name(std::uint8_t status)
{
  static constexpr const char *kNames[] = {"applied", "unknown index", "bad type or length",
    "read-only",
    "out of range"};
  return kNames[status];
}

// One varint length in minimal form, as [ENC-7] lays a string out.
void put_varint(std::vector<std::uint8_t> & out, std::uint32_t value)
{
  do{
    std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7Fu);
    value >>= 7u;
    if (value != 0u) {
      byte = static_cast<std::uint8_t>(byte | 0x80u);
    }
    out.push_back(byte);
  } while (value != 0u);
}

// The MCU default as the ROS value to declare. The walk validated every
// shape before keeping the entry ([PAR-1], [PAR-3]), so this only lays the
// bytes out; the trailing throw names a type the walk should not have kept.
rclcpp::ParameterValue decode_param_default(const ParamInfo & info)
{
  switch (info.type) {
    case kParamBool:
      return rclcpp::ParameterValue(info.default_value[0] != 0u);
    case kParamI64:
    case kParamF64: {
        std::uint64_t bits = 0u;
        for (std::size_t at = 0; at < 8u; ++at) {
          bits |= static_cast<std::uint64_t>(info.default_value[at]) << (8u * at);
        }
        if (info.type == kParamI64) {
          return rclcpp::ParameterValue(static_cast<std::int64_t>(bits));
        }
        double number = 0.0;
        static_assert(sizeof(number) == sizeof(bits), "a host double carries 64 bits");
        std::memcpy(&number, &bits, sizeof(number));
        return rclcpp::ParameterValue(number);
      }
    case kParamString: {
        // The length prefix ends at the first byte without its top bit, as
        // the walk already checked.
        std::size_t at = 0;
        while ((info.default_value[at++] & 0x80u) != 0u) {
        }
        return rclcpp::ParameterValue(std::string(
            info.default_value.begin() + static_cast<std::ptrdiff_t>(at),
            info.default_value.end()));
      }
  }
  throw std::invalid_argument("unknown parameter type");
}

// One ROS value as its wire bytes ([PAR-1], [PAR-3]). The caller checked
// the ROS type against the MCU type first: the middleware validates every
// set, and the handshake re-push compares before encoding. The chain ends
// in the string case because the walk keeps types 0 to 3 ([PAR-3]).
void encode_param_value(
  std::uint8_t type, const rclcpp::Parameter & value,
  std::vector<std::uint8_t> & out)
{
  out.clear();
  if (type == kParamBool) {
    out.push_back(value.get_value<bool>() ? 1u : 0u);
  } else if (type == kParamI64) {
    const std::uint64_t bits = static_cast<std::uint64_t>(value.get_value<std::int64_t>());
    for (int at = 0; at < 8; ++at) {
      out.push_back(static_cast<std::uint8_t>((bits >> (8 * at)) & 0xFFu));
    }
  } else if (type == kParamF64) {
    const double number = value.get_value<double>();
    std::uint64_t bits = 0u;
    static_assert(sizeof(number) == sizeof(bits), "a host double carries 64 bits");
    std::memcpy(&bits, &number, sizeof(bits));
    for (int at = 0; at < 8; ++at) {
      out.push_back(static_cast<std::uint8_t>((bits >> (8 * at)) & 0xFFu));
    }
  } else {
    const std::string & text = value.get_value<std::string>();
    put_varint(out, static_cast<std::uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
  }
}

}  // namespace

std::map<std::string, double> parse_rate_overrides(
  const std::vector<std::string> & entries,
  const LogSink & sink)
{
  std::map<std::string, double> rates;
  for (const std::string & entry : entries) {
    const std::string::size_type at = entry.find('=');
    std::string why;
    double hz = 0.0;
    if (at == std::string::npos || at == 0u || at + 1u >= entry.size()) {
      why = "want <topic>=<hz>";
    } else {
      try {
        std::size_t used = 0;
        hz = std::stod(entry.substr(at + 1u), &used);
        if (used != entry.size() - at - 1u || !std::isfinite(hz) || hz < 0.0) {
          why = "the rate is not a non-negative number";
        }
      } catch (const std::exception &) {
        why = "the rate is not a non-negative number";
      }
    }
    if (!why.empty()) {
      if (sink) {
        sink(SessionLog::kError, "rate_limit.overrides: ignoring '" + entry + "': " + why);
      }
      continue;
    }
    rates[entry.substr(0u, at)] = hz;
  }
  return rates;
}

struct Router::Impl
{
  Impl(
    rclcpp::node_interfaces::NodeTopicsInterface::SharedPtr in_topics, TransportFactory in_open,
    const Clock & in_clock, const Clock & in_ros_clock, RouterParams in_params, LogSink in_sink,
    SessionConfig session_config, LogSink in_mcu_sink,
    rclcpp::node_interfaces::NodeParametersInterface::SharedPtr in_node_params,
    NodeServiceInterfaces in_node_services)
  : topics(in_topics), params(std::move(in_params)), sink(std::move(in_sink)), clock(in_clock),
    mcu_sink(std::move(in_mcu_sink)), node_params_(std::move(in_node_params)),
    node_services_(std::move(in_node_services)),
    session_(session_config, in_clock, in_ros_clock, std::move(in_open), resolve_topic_type,
      SessionEvents{log_event(), handshake_event(), user_event(), mcu_log_event(),
        param_status_event(), service_request_event(), service_response_event(),
        action_goal_response_event(), action_cancel_response_event(), action_feedback_event(),
        action_result_event()},
      resolve_service_type)
  {
    if (!sink) {
      throw std::invalid_argument("r2s_bridge::Router: a log sink is required");
    }
    if (params.qos_depth < 1u) {
      throw std::invalid_argument("r2s_bridge::Router: qos_depth must be at least 1");
    }
    if (params.tx_budget_pct < 0 || params.tx_budget_pct > 100) {
      throw std::invalid_argument("r2s_bridge::Router: tx_budget_pct must be 0 to 100");
    }
    if (params.baud <= 0) {
      throw std::invalid_argument("r2s_bridge::Router: baud must be positive");
    }
    if (!(params.rate_default_hz >= 0.0) || !std::isfinite(params.rate_default_hz)) {
      throw std::invalid_argument("r2s_bridge::Router: rate_default_hz must not be negative");
    }
    for (const auto &[name, hz] : params.rate_limits) {
      if (!(hz >= 0.0) || !std::isfinite(hz)) {
        throw std::invalid_argument("r2s_bridge::Router: a per-topic rate must not be negative");
      }
      (void)name;
    }
        // The on-set hook carries every ROS parameter set on the node; the
        // handler answers only the MCU's own. Kept alive here, or the node
        // unregisters it.
    if (node_params_ != nullptr) {
      param_hook_ = node_params_->add_on_set_parameters_callback(
        [this](const std::vector<rclcpp::Parameter> & params) {return on_mcu_param_set(params);});
    }
  }

    // One routed topic's endpoints. Declared up here because the route
    // builders below name it in their signatures.
  struct Route
  {
    TopicInfo info;
    rclcpp::GenericPublisher::SharedPtr pub;
    rclcpp::GenericSubscription::SharedPtr sub;
    std::size_t publish_failures = 0;
  };

    // One ROS request taken for an MCU-served service, packed and waiting
    // for its turn on the wire, or sent and waiting for the MCU.
  struct RosCall
  {
    RosCallHeader header;
    std::vector<std::uint8_t> packed;
  };

    // One ROS call in flight toward the MCU: the header answers the ROS
    // caller when the MCU RESPONSE arrives, and the deadline prunes it
    // ([SVR-8]).
  struct InflightRosCall
  {
    RosCallHeader header;
    std::int64_t sent_ms = 0;
  };

    // One MCU call in flight toward ROS: the future carries the ROS answer,
    // and the deadline answers busy when ROS stays silent ([SVR-8]).
  struct InflightMcuCall
  {
    InflightMcuCall(
      std::uint8_t req_in, ServiceTypes::Struct request_in,
      rclcpp::GenericClient::FutureAndRequestId future_in, std::int64_t seq_in,
      std::int64_t sent_in)
    : mcu_req(req_in), request(std::move(request_in)), future(std::move(future_in)),
      future_seq(seq_in), sent_ms(sent_in) {}
    std::uint8_t mcu_req = 0;
    ServiceTypes::Struct request;
    rclcpp::GenericClient::FutureAndRequestId future;
    std::int64_t future_seq = 0;
    std::int64_t sent_ms = 0;
  };

    // One routed service's endpoints. A server-role service is a ROS service
    // answered from the MCU; a client-role service is a ROS client fed by
    // MCU calls. Move-only: the rcl handle and the futures do not copy.
  struct ServiceRoute
  {
    ServiceRoute(ServiceInfo in_info, ServiceTypes in_types)
    : info(std::move(in_info)), types(std::move(in_types)) {}
#if !defined(R2S_HAS_GENERIC_SERVICE)
    ~ServiceRoute()
    {
      if (rcl_valid) {
        [[maybe_unused]] const rcl_ret_t fini = rcl_service_fini(&rcl_service, rcl_node.get());
        (void)fini;
      }
    }
#endif
    ServiceInfo info;
    ServiceTypes types;
#if defined(R2S_HAS_GENERIC_SERVICE)
    rclcpp::GenericService::SharedPtr service;
#else
    rcl_service_t rcl_service = rcl_get_zero_initialized_service();
    std::shared_ptr<rcl_node_t> rcl_node;
    bool rcl_valid = false;
    ServiceTypes::Struct take_storage;
#endif
    rclcpp::GenericClient::SharedPtr client;
    std::map<std::uint8_t, InflightRosCall> ros_inflight;
    std::uint8_t next_req = 0;
    std::deque<RosCall> ros_queue;
    std::map<std::uint8_t, InflightMcuCall> mcu_inflight;
  };

    // Poll-time repair attempts per topic missing despite an ok verdict: how
    // many ran, when the last ran, and why creation failed. A handshake
    // clears the table, renewing every missing route's episode.
  struct RepairState
  {
    int attempts = 0;
    std::chrono::nanoseconds last_attempt{0};
    std::string reason;
  };

  std::function<void(SessionLog, const std::string & )> log_event()
  {
    return [this](SessionLog level, const std::string & text) {emit(level, text);};
  }

    // The MCU's own lines go to their own sink when one is set, so the node
    // can log them under its child name; otherwise they join the log sink.
  std::function<void(SessionLog, const std::string & )> mcu_log_event()
  {
    return [this](SessionLog level, const std::string & text) {
             if (mcu_sink) {
               mcu_sink(level, text);
             } else {
               emit(level, text);
             }
           };
  }

  std::function<void(bool)> handshake_event()
  {
    return [this](bool identical) {on_handshake(identical);};
  }

  std::function<void(std::uint8_t, std::uint8_t)> param_status_event()
  {
    return [this](std::uint8_t index, std::uint8_t status) {on_param_status(index, status);};
  }

  std::function<void(std::uint8_t, const std::vector<std::uint8_t> & )> user_event()
  {
    return
      [this](std::uint8_t id, const std::vector<std::uint8_t> & payload) {
        on_user_frame(id, payload);
      };
  }

  std::function<void(std::uint8_t, std::uint8_t, const std::vector<std::uint8_t> &)>
  service_request_event()
  {
    return [this](std::uint8_t index, std::uint8_t req,
             const std::vector<std::uint8_t> & payload) {
             on_service_request(index, req, payload);
           };
  }

  std::function<void(std::uint8_t, std::uint8_t, const std::vector<std::uint8_t> &)>
  service_response_event()
  {
    return [this](std::uint8_t req, std::uint8_t status,
             const std::vector<std::uint8_t> & payload) {
             on_service_response(req, status, payload);
           };
  }

  std::function<void(std::uint8_t, std::uint8_t)> action_goal_response_event()
  {
    return [this](std::uint8_t, std::uint8_t) {};
  }

  std::function<void(std::uint8_t, std::uint8_t)> action_cancel_response_event()
  {
    return [this](std::uint8_t, std::uint8_t) {};
  }

  std::function<void(std::uint8_t, std::uint8_t, const std::vector<std::uint8_t> &)>
  action_feedback_event()
  {
    return [this](std::uint8_t, std::uint8_t, const std::vector<std::uint8_t> &) {};
  }

  std::function<void(std::uint8_t, std::uint8_t, const std::vector<std::uint8_t> &)>
  action_result_event()
  {
    return [this](std::uint8_t, std::uint8_t, const std::vector<std::uint8_t> &) {};
  }

  void emit(SessionLog level, const std::string & text)
  {
    sink(level, text);
  }

  rclcpp::QoS route_qos(const TopicInfo & info) const
  {
    rclcpp::QoS qos(params.qos_depth);
    if ((info.flags & kReliableFlag) != 0u) {
      qos.reliable();
    } else {
      qos.best_effort();
    }
    return qos;
  }

    // Creates one route's endpoint, reporting the failure through the sink at
    // the caller's level. Empty when the middleware refuses the endpoint, and
    // the error text explains why for the diagnostics snapshot.
  std::optional<Route> make_route(
    const TopicInfo & info, SessionLog failure_level,
    std::string & error)
  {
    Route route;
    route.info = info;
    try {
      if (info.device_to_host()) {
        route.pub = rclcpp::create_generic_publisher(topics, info.topic_name, info.type_name,
                                                             route_qos(info));
      } else {
        route.sub = rclcpp::create_generic_subscription(
                    topics, info.topic_name, info.type_name, route_qos(info),
          [this, id = info.topic_id](std::shared_ptr<rclcpp::SerializedMessage> msg) {
            on_ros_message(id, std::move(msg));
                    });
      }
    } catch (const std::exception & failure) {
      error = failure.what();
      emit(failure_level,
          "topic '" + info.topic_name + "': no publisher or subscription: " + error);
      return std::nullopt;
    }
    error.clear();
    return route;
  }

    // The pacer entry the handshake registers for one routed topic.
  SchedulerTopic pacing_for(const TopicInfo & info) const
  {
    SchedulerTopic paced;
    paced.topic_id = info.topic_id;
    paced.keep_latest = info.keep_latest();
    const auto limited = params.rate_limits.find(info.topic_name);
    paced.rate_hz = limited == params.rate_limits.end() ? params.rate_default_hz : limited->second;
    paced.queue_bound = paced.keep_latest ? 1u : params.qos_depth;
    return paced;
  }

    // Gives one ok topic its route and pacing, creating the endpoint when
    // missing. Records a failed attempt for the repair schedule, so the
    // handshake and the polls share one creation path.
  void ensure_route(const TopicInfo & info, SessionLog failure_level)
  {
    std::string error;
    auto route = make_route(info, failure_level, error);
    if (!route) {
      repairs_[info.topic_id].reason = error;
      return;
    }
    scheduler_.add_topic(pacing_for(info));
    routes[info.topic_id] = std::move(*route);
  }

    // Retries routes missing despite an ok verdict: one transient creation
    // failure must not lose the topic forever while the handshake keeps
    // reporting it ok. Only connected polls repair, so a walk in progress
    // never races the handshake completing it; a healed subscription joins
    // the pacing the way the handshake would have registered it.
  void repair_missing_routes(std::chrono::nanoseconds now)
  {
    if (session_.state() != SessionState::kConnected) {
      return;
    }
    for (const TopicInfo & info : session_.topics()) {
      if (info.status != kTopicStatusOk || routes.count(info.topic_id) != 0u) {
        continue;
      }
      RepairState & state = repairs_[info.topic_id];
      if (state.attempts >= kMaxRepairAttempts) {
        continue;
      }
      if (state.attempts > 0 &&
        now - state.last_attempt < std::chrono::milliseconds(kRepairSpacingMs))
      {
        continue;
      }
      state.last_attempt = now;
      ++state.attempts;
      ensure_route(info, SessionLog::kDebug);
    }
  }

  void on_handshake(bool identical)
  {
    std::lock_guard<std::recursive_mutex> guard(mutex);
        // A new handshake renews the repair attempts: a still-missing route
        // retries from scratch instead of inheriting a spent episode.
    repairs_.fill(RepairState{});
    if (identical && !routes.empty()) {
            // The endpoints stay, but the MCU's new limits may exceed the old
            // bucket: refresh the burst without touching the queues.
      scheduler_.set_max_burst(
                static_cast<double>(Scheduler::encoded_frame_bound(session_.mcu_max_frame())));
    } else {
      sync_routes();
    }
        // Every handshake re-syncs the parameters: a reboot resets the MCU
        // to its defaults, so the current values go out again ([PAR-9]).
    sync_params();
        // Services rebuild the same way: a re-handshake drops every
        // in-flight call on both sides ([SVR-8], [REL-5]), so no ROS caller
        // survives it.
    sync_services();
  }

  void sync_routes()
  {
    routes.clear();
    const double budget_bytes_per_sec =
      static_cast<double>(params.baud) / 10.0 * static_cast<double>(params.tx_budget_pct) / 100.0;
    const double burst =
      static_cast<double>(Scheduler::encoded_frame_bound(session_.mcu_max_frame()));
    scheduler_.configure({}, budget_bytes_per_sec, burst);
    for (const TopicInfo & info : session_.topics()) {
      if (info.status != kTopicStatusOk) {
        continue;
      }
      ensure_route(info, SessionLog::kError);
    }
  }

    // One ROS server for an MCU-served service: the distro's generic
    // service where it has one, the rcl handle with introspection support
    // where it does not (research ticket R4). Throws naming the service on
    // any failure, so the caller refuses the route instead of serving it.
  void make_ros_server(ServiceRoute & route)
  {
    if (node_services_.base == nullptr || node_services_.services == nullptr) {
      throw std::invalid_argument("no node interfaces for the service server");
    }
#if defined(R2S_HAS_GENERIC_SERVICE)
    auto callback = [this, index = route.info.index](
                      std::shared_ptr<rmw_request_id_t> header,
                      rclcpp::GenericService::SharedRequest request) {
        std::lock_guard<std::recursive_mutex> guard(mutex);
        on_ros_request(index, std::move(header), std::move(request));
      };
    route.service = rclcpp::create_generic_service(node_services_.base, node_services_.services,
        route.info.service_name, route.info.type_name, std::move(callback), rclcpp::ServicesQoS(),
        nullptr);
#else
    route.rcl_node = node_services_.base->get_shared_rcl_node_handle();
        // rcl takes the name as given, so expand and remap it the way the
        // node would for a managed service.
    const std::string expanded =
      node_services_.base->resolve_topic_or_service_name(route.info.service_name, true);
    rcl_service_options_t options = rcl_service_get_default_options();
    // GCOVR_EXCL_START: rcl_service_init fails only on a middleware fault
    // which a unit test cannot inject; the success path is tested.
    if (rcl_service_init(&route.rcl_service, route.rcl_node.get(),
          route.types.cpp_service_handle(), expanded.c_str(), &options) != RCL_RET_OK)
    {
      rcl_reset_error();
      throw std::runtime_error("rcl_service_init failed");
    }
    // GCOVR_EXCL_STOP
    route.rcl_valid = true;
    route.take_storage = route.types.make_request();
#endif
  }

    // One ROS client for an MCU-called service: the generic client on every
    // distro (research ticket R4). Throws naming the service on failure.
  void make_ros_client(ServiceRoute & route)
  {
    if (node_services_.base == nullptr || node_services_.graph == nullptr ||
      node_services_.services == nullptr)
    {
      throw std::invalid_argument("no node interfaces for the service client");
    }
    route.client = rclcpp::create_generic_client(node_services_.base, node_services_.graph,
        node_services_.services, route.info.service_name, route.info.type_name);
  }

    // Gives one ok service its route: the introspection handles, then the
    // ROS endpoints its role needs. A service whose type does not load
    // never reaches here: the walk verdict refused it ([SVR-7]).
  void ensure_service_route(const ServiceInfo & info)
  {
    std::unique_ptr<ServiceRoute> route;
    try {
      route = std::make_unique<ServiceRoute>(info, ServiceTypes(info.type_name));
      // GCOVR_EXCL_START: resolve_service_type reports ok only after the
      // same library and service handle load, so the constructor cannot
      // throw for a routed service; a unit test cannot stage a failure
      // between the verdict and this lookup.
    } catch (const std::exception & failure) {
      service_repairs_[info.index].reason = failure.what();
      emit(SessionLog::kError,
        "service '" + info.service_name + "': no route (" + failure.what() + ")");
      return;
    }
    // GCOVR_EXCL_STOP
    try {
      if (info.role == kServiceRoleServer) {
        make_ros_server(*route);
      } else {
        make_ros_client(*route);
      }
    } catch (const std::exception & failure) {
      service_repairs_[info.index].reason = failure.what();
      emit(SessionLog::kError,
        "service '" + info.service_name + "': no endpoint (" + failure.what() + ")");
      return;
    }
    service_repairs_.erase(info.index);
    service_routes_[info.index] = std::move(route);
  }

  void sync_services()
  {
    service_routes_.clear();
    service_repairs_.clear();
    for (const ServiceInfo & info : session_.services()) {
      if (info.status != kServiceStatusOk) {
        continue;
      }
      ensure_service_route(info);
    }
  }

    // One packed ROS request from the take path: queued behind the
    // in-flight one, or answered empty when the queue is full. The packed
    // bytes are already converted; the header answers the ROS caller.
  void enqueue_ros_call(std::uint8_t index, RosCall call)
  {
    const auto found = service_routes_.find(index);
    // GCOVR_EXCL_START: only the generic-service callback can hand over an
    // index the routes dropped; the rcl take path always names a live route,
    // and a unit test cannot feed the unbuilt callback.
    if (found == service_routes_.end()) {
      emit(SessionLog::kWarning,
        "service index " + std::to_string(index) + ": no route for the ROS request");
      return;
    }
    // GCOVR_EXCL_STOP
    ServiceRoute & route = *found->second;
    if (route.ros_queue.size() >= kMaxQueuedRosCalls) {
      emit(SessionLog::kWarning,
        "service '" + route.info.service_name + "': overloaded, a ROS call is dropped");
      answer_ros_call(route, call.header, route.types.make_response());
      return;
    }
    route.ros_queue.push_back(std::move(call));
  }

#if defined(R2S_HAS_GENERIC_SERVICE)
    // One ROS request the generic service took: pack it now, so a failure
    // answers its caller straight away instead of queueing garbage.
  void on_ros_request(
    std::uint8_t index, std::shared_ptr<rmw_request_id_t> header,
    rclcpp::GenericService::SharedRequest request)
  {
    const auto found = service_routes_.find(index);
    if (found == service_routes_.end()) {
      return;
    }
    ServiceRoute & route = *found->second;
    RosCall call;
    call.header = std::move(header);
    const ConvertResult result = converter_.pack_from_struct(route.types.request_type_name(), false,
        request.get(), call.packed);
    if (result.fail != ConvertFail::kNone) {
      emit(SessionLog::kWarning, "service '" + route.info.service_name +
        "': dropped ROS request (" + fail_name(result.fail) + " at '" +
        field_text(route.types.request_type_name(), result.field) + "')");
      answer_ros_call(route, call.header, route.types.make_response());
      return;
    }
    enqueue_ros_call(index, std::move(call));
  }
#endif

    // One MCU call while connected: unpack it into a ROS call, or answer
    // busy when it does not unpack ([SVR-6]). A call for an index with no
    // client is a caller bug; busy tells it to back off.
  void on_service_request(
    std::uint8_t index, std::uint8_t req, const std::vector<std::uint8_t> & payload)
  {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    const auto found = service_routes_.find(index);
    // GCOVR_EXCL_START: the session drops REQUESTs for unknown indexes
    // before the event fires, so this guard cannot trip; a unit test cannot
    // fire the event past the session.
    if (found == service_routes_.end() || found->second->client == nullptr) {
      emit(SessionLog::kWarning,
        "service index " + std::to_string(index) + ": no ROS client for the MCU call");
      session_.send_service_response(req, kSvcStatusBusy, {});
      return;
    }
    // GCOVR_EXCL_STOP
    ServiceRoute & route = *found->second;
    // GCOVR_EXCL_START: the session holds one MCU call per slot and drops
    // the second before the event fires, so this guard cannot trip; a unit
    // test cannot fire the event past the session.
    if (route.mcu_inflight.count(req) != 0u) {
      return;
    }
    // GCOVR_EXCL_STOP
    ServiceTypes::Struct request = route.types.make_request();
    const ConvertResult result = converter_.unpack_to_struct(route.types.request_type_name(), false,
        payload.data(), payload.size(), request.storage.data());
    if (result.fail != ConvertFail::kNone) {
      emit(SessionLog::kWarning, "service '" + route.info.service_name +
        "': dropped MCU call (" + fail_name(result.fail) + " at '" +
        field_text(route.types.request_type_name(), result.field) + "')");
      session_.send_service_response(req, kSvcStatusBadPayload, {});
      return;
    }
    try {
      auto sent = route.client->async_send_request(request.storage.data());
      const std::int64_t seq = sent.request_id;
      route.mcu_inflight.emplace(req, InflightMcuCall(req, std::move(request), std::move(sent),
        seq, now_ms()));
      // GCOVR_EXCL_START: the request above is converted and the client is
      // live, so async_send_request cannot throw here; a unit test cannot
      // stage a middleware failure underneath it.
    } catch (const std::exception & failure) {
      emit(SessionLog::kWarning,
        "service '" + route.info.service_name + "': no ROS call (" + failure.what() + ")");
      session_.send_service_response(req, kSvcStatusBusy, {});
      return;
    }
    // GCOVR_EXCL_STOP
  }

    // One MCU answer while connected: the req names the ROS caller it
    // belongs to. A nonzero status answers empty; anything for an unknown
    // req already timed out and is ignored ([SVR-5], [SVR-6], [SVR-8]).
  void on_service_response(
    std::uint8_t req, std::uint8_t status, const std::vector<std::uint8_t> & payload)
  {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    for (auto & entry : service_routes_) {
      ServiceRoute & route = *entry.second;
      const auto found = route.ros_inflight.find(req);
      if (found == route.ros_inflight.end()) {
        continue;
      }
      InflightRosCall call = std::move(found->second);
      route.ros_inflight.erase(found);
      if (status == kSvcStatusOk) {
        ServiceTypes::Struct response = route.types.make_response();
        const ConvertResult result = converter_.unpack_to_struct(
            route.types.response_type_name(), false, payload.data(), payload.size(),
            response.storage.data());
        if (result.fail != ConvertFail::kNone) {
          emit(SessionLog::kWarning, "service '" + route.info.service_name +
            "': dropped MCU answer (" + fail_name(result.fail) + " at '" +
            field_text(route.types.response_type_name(), result.field) + "')");
          answer_ros_call(route, call.header, route.types.make_response());
        } else {
          answer_ros_call(route, call.header, std::move(response));
        }
      } else {
        emit(SessionLog::kWarning, "service '" + route.info.service_name +
          "': the MCU answered status " + std::to_string(status));
        answer_ros_call(route, call.header, route.types.make_response());
      }
      pump_ros_queue(route);
      return;
    }
  }

    // Answers one taken ROS request from a filled response struct. A zero
    // response is the honest answer when the MCU never produced one.
  void answer_ros_call(
    ServiceRoute & route, const RosCallHeader & header, ServiceTypes::Struct response)
  {
#if defined(R2S_HAS_GENERIC_SERVICE)
    // Single owner for the response bytes: the shared pointer aliases the
    // bridge-owned struct, so its fini runs once and nothing is copied
    // between owners with separate finis.
    auto owned = std::make_shared<ServiceTypes::Struct>(std::move(response));
    std::shared_ptr<void> shared(owned, owned->storage.data());
    route.service->send_response(*header, shared);
#else
    rmw_request_id_t id = header;
    // GCOVR_EXCL_START: the header comes from a live take and the response
    // is filled, so rcl_send_response only fails on a middleware fault which
    // a unit test cannot inject.
    if (rcl_send_response(&route.rcl_service, &id, response.storage.data()) != RCL_RET_OK) {
      rcl_reset_error();
      emit(SessionLog::kWarning,
        "service '" + route.info.service_name + "': no ROS answer (" +
        std::to_string(id.sequence_number) + ")");
    }
    // GCOVR_EXCL_STOP
#endif
  }

    // Sends the next queued ROS call while none is in flight: the session
    // holds one outstanding frame per channel, so one in flight per service
    // keeps every caller answerable ([SVR-4], [SVR-10]).
  void pump_ros_queue(ServiceRoute & route)
  {
    if (!route.ros_inflight.empty() || route.ros_queue.empty()) {
      return;
    }
    RosCall call = std::move(route.ros_queue.front());
    route.ros_queue.pop_front();
    std::uint8_t req = route.next_req++;
    // GCOVR_EXCL_START: the id wraps only after 256 sends, and the queue
    // holds 3 with one in flight, so the in-flight set can never collide in
    // a unit test; the fuzz corpus cannot stage 256 answered calls either.
    while (route.ros_inflight.count(req) != 0u) {
      req = route.next_req++;
    }
    // GCOVR_EXCL_STOP
    if (!session_.send_service_request(route.info.index, req, call.packed)) {
      emit(SessionLog::kWarning,
        "service '" + route.info.service_name + "': the link refused the ROS call");
      answer_ros_call(route, call.header, route.types.make_response());
      pump_ros_queue(route);
      return;
    }
    InflightRosCall inflight;
    inflight.header = std::move(call.header);
    inflight.sent_ms = now_ms();
    route.ros_inflight[req] = std::move(inflight);
  }

    // The session clock in milliseconds, for the call deadlines ([SVR-8]).
  std::int64_t now_ms() const
  {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock.now()).count();
  }

    // Takes pending ROS requests on the rcl fallback: the generic service
    // feeds its own callback instead. A take that finds nothing resets the
    // middleware error and moves on; the 1 ms poll takes again.
  void take_ros_requests()
  {
#if !defined(R2S_HAS_GENERIC_SERVICE)
    for (auto & entry : service_routes_) {
      ServiceRoute & route = *entry.second;
      if (!route.rcl_valid || route.info.role != kServiceRoleServer) {
        continue;
      }
      rmw_service_info_t header{};
      const rcl_ret_t taken =
        rcl_take_request_with_info(&route.rcl_service, &header, route.take_storage.storage.data());
      if (taken != RCL_RET_OK) {
        rcl_reset_error();
        continue;
      }
      RosCall call;
      call.header = header.request_id;
      const ConvertResult result = converter_.pack_from_struct(
          route.types.request_type_name(), false, route.take_storage.storage.data(), call.packed);
      if (result.fail != ConvertFail::kNone) {
        emit(SessionLog::kWarning, "service '" + route.info.service_name +
          "': dropped ROS request (" + fail_name(result.fail) + " at '" +
          field_text(route.types.request_type_name(), result.field) + "')");
        answer_ros_call(route, call.header, route.types.make_response());
        continue;
      }
      enqueue_ros_call(route.info.index, std::move(call));
    }
#endif
  }

    // Moves every service route: queued ROS calls go out, answered MCU
    // calls complete, and calls older than the call timeout report back
    // ([SVR-8]) instead of leaking (research ticket R4).
  void poll_services()
  {
    if (session_.state() != SessionState::kConnected) {
      return;
    }
    take_ros_requests();
    const std::int64_t now = now_ms();
    for (auto & entry : service_routes_) {
      ServiceRoute & route = *entry.second;
      pump_ros_queue(route);
      for (auto it = route.ros_inflight.begin(); it != route.ros_inflight.end();) {
        if (now - it->second.sent_ms < kServiceCallTimeoutMs) {
          ++it;
          continue;
        }
        emit(SessionLog::kWarning, "service '" + route.info.service_name +
          "': the MCU never answered, a ROS call is dropped");
        answer_ros_call(route, it->second.header, route.types.make_response());
        it = route.ros_inflight.erase(it);
        pump_ros_queue(route);
      }
      for (auto it = route.mcu_inflight.begin(); it != route.mcu_inflight.end();) {
        InflightMcuCall & call = it->second;
        if (call.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
          if (now - call.sent_ms >= kServiceCallTimeoutMs) {
            emit(SessionLog::kWarning, "service '" + route.info.service_name +
              "': no ROS answer, the MCU call is busy");
            route.client->remove_pending_request(call.future_seq);
            session_.send_service_response(call.mcu_req, kSvcStatusBusy, {});
            it = route.mcu_inflight.erase(it);
          } else {
            ++it;
          }
          continue;
        }
        std::vector<std::uint8_t> packed;
        const ConvertResult result = converter_.pack_from_struct(
            route.types.response_type_name(), false, call.future.get().get(), packed);
        const std::uint8_t mcu_req = call.mcu_req;
        if (result.fail != ConvertFail::kNone) {
          emit(SessionLog::kWarning, "service '" + route.info.service_name +
            "': dropped ROS answer (" + fail_name(result.fail) + " at '" +
            field_text(route.types.response_type_name(), result.field) + "')");
          session_.send_service_response(mcu_req, kSvcStatusBusy, {});
        } else if (!session_.send_service_response(mcu_req, kSvcStatusOk, packed)) {
          emit(SessionLog::kWarning,
            "service '" + route.info.service_name + "': the link refused the ROS answer");
        }
        it = route.mcu_inflight.erase(it);
      }
    }
  }

    // Retries service routes missing despite an ok verdict, the way the
    // topic repair does: a transient creation failure must not lose the
    // service while the handshake keeps reporting it ok.
  void repair_missing_services(std::chrono::nanoseconds now)
  {
    if (session_.state() != SessionState::kConnected) {
      return;
    }
    for (const ServiceInfo & info : session_.services()) {
      if (info.status != kServiceStatusOk || service_routes_.count(info.index) != 0u) {
        continue;
      }
      RepairState & state = service_repairs_[info.index];
      if (state.attempts >= kMaxRepairAttempts) {
        continue;
      }
      if (state.attempts > 0 &&
        now - state.last_attempt < std::chrono::milliseconds(kRepairSpacingMs))
      {
        continue;
      }
      state.last_attempt = now;
      ++state.attempts;
      ensure_service_route(info);
    }
  }

    // One refused SET the MCU reported while connected ([PAR-5]): applied
    // ones need no log, the reliable channel already acked them.
  void on_param_status(std::uint8_t index, std::uint8_t status)
  {
    if (status == 0u) {
      return;
    }
    std::lock_guard<std::recursive_mutex> guard(mutex);
    std::string name = "#" + std::to_string(index);
    for (const auto & entry : mcu_params_) {
      if (entry.second.index == index) {
        name = "'" + entry.first + "'";
        break;
      }
    }
    emit(SessionLog::kWarning,
             "parameter " + name + ": the MCU refused the set (" + param_status_name(status) + ")");
  }

    // The parameters layer: the walk's DECLAREs become ROS parameters with
    // the MCU defaults, and every current value that differs goes out with
    // SET, on this handshake and every re-handshake ([PAR-9]).
  void sync_params()
  {
    if (node_params_ == nullptr) {
      return;
    }
    const std::vector<ParamInfo> announced = session_.params();
    for (auto & entry : mcu_params_) {
      const bool kept =
        std::any_of(announced.begin(), announced.end(),
          [&entry](const ParamInfo & info) {return info.name == entry.first;});
      if (!kept && !entry.second.stale) {
        entry.second.stale = true;
        emit(SessionLog::kWarning,
                     "parameter '" + entry.first +
            "': no longer announced, its sets fail until it returns");
      }
    }
    for (const ParamInfo & info : announced) {
      if (info.name.empty()) {
        emit(SessionLog::kWarning, "parameter index " + std::to_string(info.index) + ": no name");
        continue;
      }
      if (mcu_params_.count(info.name) == 0u && node_params_->has_parameter(info.name)) {
        emit(SessionLog::kWarning,
                     "parameter '" + info.name + "': the node already has it, not tracked");
        continue;
      }
      if (!node_params_->has_parameter(info.name)) {
        try {
          rcl_interfaces::msg::ParameterDescriptor descriptor;
          descriptor.read_only = (info.flags & kParamReadOnly) != 0u;
          node_params_->declare_parameter(info.name, decode_param_default(info), descriptor);
        } catch (const std::exception & error) {
          emit(SessionLog::kWarning,
                         "parameter '" + info.name + "': not declared (" + error.what() + ")");
          continue;
        }
      }
            // A returning name unstales: the entry below overwrites the flag.
      mcu_params_[info.name] = McuParam{info.index, info.type, false};
            // Declared above or already present: the get cannot miss.
      const rclcpp::Parameter current = node_params_->get_parameter(info.name);
            // The middleware fixes a ROS type at declare, so a mismatch
            // means the MCU reflashed the type: nothing goes out ([PAR-9]).
      if (current.get_type() != kRosType[info.type]) {
        emit(SessionLog::kWarning, "parameter '" + info.name + "': type changed, not pushed");
        continue;
      }
      std::vector<std::uint8_t> wire;
      encode_param_value(info.type, current, wire);
      if (wire != info.default_value) {
        session_.send_param_set(info.index, wire);
      }
    }
  }

    // One ROS parameter set on the node: only the MCU's own parameters go
    // out as SETs, everything else belongs to the node's own callbacks. The
    // middleware already refused read-only and wrong-typed sets before
    // calling, so only a stale entry or a down link still fails here. A
    // rejection names its parameter; a set while disconnected fails, so
    // `ros2 param set` reports the link instead of vanishing.
  rcl_interfaces::msg::SetParametersResult on_mcu_param_set(
    const std::vector<rclcpp::Parameter> & params)
  {
    rcl_interfaces::msg::SetParametersResult result;
    result.successful = true;
    std::lock_guard<std::recursive_mutex> guard(mutex);
    for (const rclcpp::Parameter & param : params) {
      const auto found = mcu_params_.find(param.get_name());
      if (found == mcu_params_.end()) {
        continue;
      }
      if (found->second.stale) {
        result.successful = false;
        result.reason = "parameter '" + param.get_name() + "' is no longer announced by the MCU";
        return result;
      }
      std::vector<std::uint8_t> wire;
      encode_param_value(found->second.type, param, wire);
      if (!session_.send_param_set(found->second.index, wire)) {
        result.successful = false;
        result.reason = "parameter '" + param.get_name() + "': the link is down";
        return result;
      }
    }
    return result;
  }

  void on_user_frame(std::uint8_t topic_id, const std::vector<std::uint8_t> & payload)
  {
    Route route;
    {
      std::lock_guard<std::recursive_mutex> guard(mutex);
      const auto found = routes.find(topic_id);
      if (found == routes.end() || found->second.pub == nullptr) {
        return;
      }
      route = found->second;
    }
    std::vector<std::uint8_t> cdr;
    ConvertResult result;
    try {
      result = converter_.to_cdr(route.info.type_name, route.info.narrow(), payload.data(),
                                       payload.size(), cdr);
            // GCOVR_EXCL_START: the middleware fails only when it cannot
            // allocate or serialize, which a unit test cannot inject.
    } catch (const std::exception & error) {
            // A failed serialize (the middleware short under load) is a
            // dropped frame, never an abort: it names its topic and counts.
      emit(SessionLog::kWarning,
                 "topic '" + route.info.topic_name + "': failed to serialize (" + error.what() +
          ")");
            // The route exists: only the poll thread mutates the table, so
            // the entry looked up above is still there.
      std::lock_guard<std::recursive_mutex> guard(mutex);
      ++routes[topic_id].publish_failures;
      return;
    }
        // GCOVR_EXCL_STOP: the brace above runs only with the catch.
    if (result.fail != ConvertFail::kNone) {
      emit(SessionLog::kWarning, "topic '" + route.info.topic_name + "': dropped packed payload (" +
                                           fail_name(result.fail) + " at '" +
                                           field_text(route.info.type_name, result.field) + "')");
      return;
    }
    rclcpp::SerializedMessage out(cdr.size());
    rcl_serialized_message_t & rcl = out.get_rcl_serialized_message();
    std::memcpy(rcl.buffer, cdr.data(), cdr.size());
    rcl.buffer_length = cdr.size();
    try {
      route.pub->publish(out);
            // GCOVR_EXCL_BR_START: a unit test cannot throw out of the logging,
            // locking, or counting calls below, nor inject a foreign exception
            // type, so the unwind edges never run.
    } catch (const std::exception & error) {
            // A failed publish (a context shutting down under the poll loop)
            // is a dropped frame, never an abort: it names its topic and counts.
      emit(SessionLog::kWarning,
                 "topic '" + route.info.topic_name + "': failed to publish (" + error.what() + ")");
            // The route exists: only the poll thread mutates the table, so
            // the entry looked up above is still there.
      std::lock_guard<std::recursive_mutex> guard(mutex);
      ++routes[topic_id].publish_failures;
      return;
            // GCOVR_EXCL_BR_STOP
    }
  }

  void on_ros_message(std::uint8_t topic_id, std::shared_ptr<rclcpp::SerializedMessage> msg)
  {
    Route route;
    {
      std::lock_guard<std::recursive_mutex> guard(mutex);
      const auto found = routes.find(topic_id);
      if (found == routes.end() || found->second.sub == nullptr) {
        return;
      }
      route = found->second;
    }
    const rcl_serialized_message_t & rcl = msg->get_rcl_serialized_message();
    std::vector<std::uint8_t> packed;
    const ConvertResult result = converter_.from_cdr(route.info.type_name, route.info.narrow(),
                                                         rcl.buffer, rcl.buffer_length, packed);
    if (result.fail != ConvertFail::kNone) {
      emit(SessionLog::kWarning, "topic '" + route.info.topic_name + "': dropped ROS message (" +
                                           fail_name(result.fail) + " at '" +
                                           field_text(route.info.type_name, result.field) + "')");
      return;
    }
    const std::uint16_t limit = session_.mcu_max_frame();
    if (packed.size() + 4u > limit) {
      emit(SessionLog::kWarning, "topic '" + route.info.topic_name + "': ROS message packs to " +
                                           std::to_string(packed.size() + 4u) +
                                           " bytes, past the MCU max_frame " +
          std::to_string(limit));
      return;
    }
    std::lock_guard<std::recursive_mutex> guard(mutex);
    scheduler_.submit(topic_id, std::move(packed));
  }

  std::string field_text(const std::string & type, std::uint8_t field)
  {
    const std::string path = converter_.field_path(type, field);
    return path.empty() ? "#" + std::to_string(field) : path;
  }

  rclcpp::node_interfaces::NodeTopicsInterface::SharedPtr topics;
  RouterParams params;
  LogSink sink;
  const Clock & clock;
  LogSink mcu_sink;
  rclcpp::node_interfaces::NodeParametersInterface::SharedPtr node_params_;
  NodeServiceInterfaces node_services_;
  rclcpp::node_interfaces::OnSetParametersCallbackHandle::SharedPtr param_hook_;
    // One MCU parameter by ROS name: the SETs route by index. Only names
    // the layer itself declared are tracked, so a colliding node
    // parameter never sends MCU traffic.
  struct McuParam
  {
    std::uint8_t index = 0;
    std::uint8_t type = 0;
        // A stale entry left the announced table. The ROS parameter stays:
        // a standard node cannot undeclare it, so its sets fail instead.
    bool stale = false;
  };
  std::map<std::string, McuParam> mcu_params_;
  Converter converter_;
  Scheduler scheduler_;
  std::map<std::uint8_t, Route> routes;
    // Repair attempts by topic id: array indexing cannot miss, so the
    // diagnostics snapshot below needs no fallback branch.
  std::array<RepairState, 256> repairs_;
    // One route per ok service by walk index, with repair attempts by index
    // for routes missing despite an ok verdict.
  std::map<std::uint8_t, std::unique_ptr<ServiceRoute>> service_routes_;
  std::map<std::uint8_t, RepairState> service_repairs_;
    // Recursive: declaring a ROS parameter reenters through the on-set
    // hook on the same thread, which takes the guard again.
  std::recursive_mutex mutex;
  Session session_;
};

Router::Router(
  rclcpp::node_interfaces::NodeTopicsInterface::SharedPtr topics, TransportFactory open,
  const Clock & clock, const Clock & ros_clock, RouterParams params, LogSink sink,
  SessionConfig session_config, LogSink mcu_sink,
  rclcpp::node_interfaces::NodeParametersInterface::SharedPtr node_params,
  NodeServiceInterfaces node_services)
: impl_(std::make_unique<Impl>(std::move(topics), std::move(open), clock, ros_clock,
    std::move(params),
                                   std::move(sink), std::move(session_config), std::move(mcu_sink),
                                   std::move(node_params), std::move(node_services)))
{
}

Router::~Router() = default;

void Router::poll()
{
  impl_->session_.poll();
  const std::chrono::nanoseconds now = impl_->clock.now();
  std::lock_guard<std::recursive_mutex> guard(impl_->mutex);
  impl_->repair_missing_routes(now);
  impl_->repair_missing_services(now);
  impl_->poll_services();
  impl_->scheduler_.poll(now, [this](std::uint8_t id, const std::vector<std::uint8_t> & payload) {
      return impl_->session_.send_user(id, payload);
    });
}

std::size_t Router::publisher_count() const
{
  std::lock_guard<std::recursive_mutex> guard(impl_->mutex);
  std::size_t count = 0;
  for (const auto &[id, route] : impl_->routes) {
    (void)id;
    if (route.pub != nullptr) {
      ++count;
    }
  }
  return count;
}

std::size_t Router::subscription_count() const
{
  std::lock_guard<std::recursive_mutex> guard(impl_->mutex);
  std::size_t count = 0;
  for (const auto &[id, route] : impl_->routes) {
    (void)id;
    if (route.sub != nullptr) {
      ++count;
    }
  }
  return count;
}

std::optional<rclcpp::QoS> Router::endpoint_qos(std::uint8_t topic_id) const
{
  std::lock_guard<std::recursive_mutex> guard(impl_->mutex);
  const auto found = impl_->routes.find(topic_id);
  if (found == impl_->routes.end()) {
    return std::nullopt;
  }
  if (found->second.pub != nullptr) {
    return found->second.pub->get_actual_qos();
  }
  if (found->second.sub != nullptr) {
    return found->second.sub->get_actual_qos();
  }
  return std::nullopt;
}

std::size_t Router::dropped(std::uint8_t topic_id) const
{
  std::lock_guard<std::recursive_mutex> guard(impl_->mutex);
  return impl_->scheduler_.dropped(topic_id);
}

std::size_t Router::queued(std::uint8_t topic_id) const
{
  std::lock_guard<std::recursive_mutex> guard(impl_->mutex);
  return impl_->scheduler_.queued(topic_id);
}

Router::Diagnostics Router::diagnostics() const
{
  std::lock_guard<std::recursive_mutex> guard(impl_->mutex);
  Diagnostics out;
  out.link_state = link_state_name(impl_->session_.state());
  out.session_token = impl_->session_.token();
  out.resets = impl_->session_.resets();
  out.control_coalesced = impl_->session_.control_coalesced();
  out.protocol_version = R2S_PROTOCOL_VERSION;
  out.mcu_counters = impl_->session_.mcu_counters();
  out.mcu_error_sums = impl_->session_.error_sums();
  for (const TopicInfo & info : impl_->session_.topics()) {
    TopicDiagnostics topic;
    topic.topic_id = info.topic_id;
    topic.topic_name = info.topic_name;
    topic.device_to_host = info.device_to_host();
    topic.status = info.status;
    topic.seq_gaps = impl_->session_.seq_gaps(info.topic_id);
    topic.dropped = impl_->scheduler_.dropped(info.topic_id);
    const auto route = impl_->routes.find(info.topic_id);
    topic.publish_failures = route == impl_->routes.end() ? 0u : route->second.publish_failures;
    topic.route_missing = info.status == kTopicStatusOk && route == impl_->routes.end();
    if (topic.route_missing) {
      topic.route_error = impl_->repairs_[info.topic_id].reason;
    }
    topic.mcu_errors = impl_->session_.error_counts(info.topic_id);
    out.topics.push_back(topic);
  }
  out.time_sync_available = impl_->session_.time_available();
  out.time_offset_ns = impl_->session_.time_offset_ns();
  out.time_drift_ppm = impl_->session_.time_drift_ppm();
  out.time_round_trip_ns = impl_->session_.time_round_trip_ns();
  return out;
}

}  // namespace r2s_bridge
