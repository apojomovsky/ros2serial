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

#include <action_msgs/msg/goal_status_array.hpp>
#include <rcl_interfaces/msg/parameter_descriptor.hpp>
#include <rcl_interfaces/msg/set_parameters_result.hpp>
#include <rclcpp/create_generic_client.hpp>
#include <rclcpp/create_publisher.hpp>
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

#include <rcl_action/action_server.h>
#include <rcl_action/goal_handle.h>
#include <rcl_action/goal_state_machine.h>
#include <rcl_action/types.h>
#include <rcl/time.h>

#include "r2s_bridge/action_types.hpp"
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

// The ROS goal states the bridge reports ([SES-35], action_msgs GoalStatus).
constexpr std::int8_t kGoalSucceeded = 4;
constexpr std::int8_t kGoalCanceled = 5;
constexpr std::int8_t kGoalAborted = 6;
constexpr std::int8_t kGoalUnknown = 0;

// The CancelGoal verdicts the bridge answers a ROS cancel with
// (action_msgs/srv/CancelGoal).
constexpr std::int8_t kCancelNone = 0;
constexpr std::int8_t kCancelRejected = 1;
constexpr std::int8_t kCancelUnknownGoal = 2;
constexpr std::int8_t kCancelTerminated = 3;

// The MCU goal and cancel verdict the bridge waits for ([ACT-4], [ACT-5]):
// anything else reads as refused.
constexpr std::uint8_t kActDecisionAccepted = 0u;

// The MCU call timeout ([SVR-8]) applies to goals and cancels the same way:
// a call with no answer in this long reports back instead of leaking.
constexpr std::int64_t kActionCallTimeoutMs = 5000;

// How often an active goal republishes its status array while nothing
// changes: clients joining late still see it.
constexpr std::int64_t kActionStatusPeriodMs = 1000;

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
    ros_clock(in_ros_clock),
    mcu_sink(std::move(in_mcu_sink)), node_params_(std::move(in_node_params)),
    node_services_(std::move(in_node_services)),
    session_(session_config, in_clock, in_ros_clock, std::move(in_open), resolve_topic_type,
      SessionEvents{log_event(), handshake_event(), user_event(), mcu_log_event(),
        param_status_event(), service_request_event(), service_response_event(),
        action_goal_response_event(), action_cancel_response_event(), action_feedback_event(),
        action_result_event()},
      resolve_service_type, resolve_action_type)
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

    // The rcl clock behind the action servers: goal bookkeeping reads it
    // for expiry, while stamps come from the router's own ROS clock.
  struct RclClock
  {
    rcl_clock_t clock{};
    RclClock()
    {
      // Steady time for goal bookkeeping: the bridge measures timeouts and
      // expiry, never ROS timestamps, so a time source it cannot observe
      // would only stall it.
      rcl_allocator_t allocator = rcl_get_default_allocator();
      // GCOVR_EXCL_START: the clock init fails only when it cannot
      // allocate, which a unit test cannot stage without breaking itself.
      if (rcl_clock_init(RCL_STEADY_TIME, &clock, &allocator) != RCL_RET_OK) {
        rcl_reset_error();
        throw std::runtime_error("rcl_clock_init failed");
      }
      // GCOVR_EXCL_STOP
    }
    ~RclClock()
    {
      [[maybe_unused]] const rcl_ret_t fini = rcl_clock_fini(&clock);
      (void)fini;
    }
    RclClock(const RclClock &) = delete;
    RclClock & operator=(const RclClock &) = delete;
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

    // A ROS goal request taken but not yet answered by the MCU: the header
    // answers the ROS client once the GOAL_RESP arrives ([ACT-4]).
  struct PendingGoal
  {
    rmw_request_id_t header{};
    std::array<std::uint8_t, 16> uuid{};
    std::uint8_t wire_seq = 0;
    std::int64_t sent_ms = 0;
  };

    // A ROS cancel request taken but not yet answered by the MCU ([ACT-5]).
  struct PendingCancel
  {
    rmw_request_id_t header{};
    std::int64_t sent_ms = 0;
  };

    // One ROS goal the bridge owns for the MCU: the UUID ROS knows it by,
    // the wire seq the MCU knows it by, and the stored terminal result.
    // A new goal replaces a terminal one; an abort keeps nobody waiting.
  struct ActiveGoal
  {
    std::array<std::uint8_t, 16> uuid{};
    std::uint8_t wire_seq = 0;
    std::int32_t accept_sec = 0;
    std::uint32_t accept_nanosec = 0;
    bool cancel_requested = false;
    rcl_action_goal_handle_t *handle = nullptr;
    std::int8_t terminal = -1;
    // The terminal result as wire bytes: unpacked fresh for every waiter,
    // so no struct is ever copied with its heap buffers ([SES-35]).
    std::optional<std::vector<std::uint8_t>> packed_result;
    std::vector<rmw_request_id_t> result_waiters;
    std::int64_t last_status_ms = 0;
    bool status_dirty = true;
  };

    // One routed action's endpoints: the rcl action server the ROS clients
    // talk to, answered from the MCU over the reliable channel. Move-only:
    // the rcl handle and the introspection structs do not copy.
  struct ActionRoute
  {
    ActionRoute(ActionInfo in_info, ActionTypes in_types)
    : info(std::move(in_info)), types(std::move(in_types)) {}
    ~ActionRoute()
    {
      if (rcl_valid) {
        [[maybe_unused]] const rcl_ret_t fini =
          rcl_action_server_fini(&server, rcl_node.get());
        (void)fini;
      }
    }
    ActionInfo info;
    ActionTypes types;
    rcl_action_server_t server = rcl_action_get_zero_initialized_server();
    std::shared_ptr<rcl_node_t> rcl_node;
    // The status array goes out over a plain rclcpp publisher: rcl's own
    // status publish hangs in the dev image (see the follow-up), while
    // takes, sends and the goal state machine all behave.
    rclcpp::Publisher<action_msgs::msg::GoalStatusArray>::SharedPtr status_pub;
    bool rcl_valid = false;
    std::uint8_t next_seq = 0;
    std::optional<PendingGoal> pending;
    std::optional<PendingCancel> pending_cancel;
    std::optional<ActiveGoal> active;
    ActionTypes::Struct goal_request_take;
    ActionTypes::Struct result_request_take;
    ActionTypes::Struct cancel_request_take;
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

  std::function<void(std::uint8_t, std::uint8_t, const std::vector<std::uint8_t> & )>
  service_request_event()
  {
    return [this](std::uint8_t index, std::uint8_t req,
           const std::vector<std::uint8_t> & payload) {
             on_service_request(index, req, payload);
           };
  }

  std::function<void(std::uint8_t, std::uint8_t, const std::vector<std::uint8_t> & )>
  service_response_event()
  {
    return [this](std::uint8_t req, std::uint8_t status,
           const std::vector<std::uint8_t> & payload) {
             on_service_response(req, status, payload);
           };
  }

  std::function<void(std::uint8_t, std::uint8_t)> action_goal_response_event()
  {
    return [this](std::uint8_t seq, std::uint8_t decision) {
             on_action_goal_response(seq, decision);
           };
  }

  std::function<void(std::uint8_t, std::uint8_t)> action_cancel_response_event()
  {
    return [this](std::uint8_t seq, std::uint8_t decision) {
             on_action_cancel_response(seq, decision);
           };
  }

  std::function<void(std::uint8_t, std::uint8_t, const std::vector<std::uint8_t> & )>
  action_feedback_event()
  {
    return [this](std::uint8_t seq, std::uint8_t index,
           const std::vector<std::uint8_t> & payload) {
             on_action_feedback(seq, index, payload);
           };
  }

  std::function<void(std::uint8_t, std::uint8_t, const std::vector<std::uint8_t> & )>
  action_result_event()
  {
    return [this](std::uint8_t seq, std::uint8_t outcome,
           const std::vector<std::uint8_t> & payload) {
             on_action_result(seq, outcome, payload);
           };
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
        // Actions rebuild the same way: a re-handshake aborts every active
        // goal ([SES-35], [REL-5]), so no ROS client waits on a dead MCU.
    sync_actions();
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

    // One ROS action server for an MCU action: the rcl server the ROS
    // clients talk to, with the introspection handles the conversions use.
    // Throws naming the action on any failure, so the caller refuses the
    // route instead of serving it.
  void make_ros_action_server(ActionRoute & route)
  {
    if (node_services_.base == nullptr) {
      throw std::invalid_argument("no node interfaces for the action server");
    }
    route.rcl_node = node_services_.base->get_shared_rcl_node_handle();
    const std::string expanded =
      node_services_.base->resolve_topic_or_service_name(route.info.action_name, false);
    const rcl_action_server_options_t options = rcl_action_server_get_default_options();
    // GCOVR_EXCL_START: the init fails only when the middleware cannot
    // allocate, which a unit test cannot stage without breaking itself.
    if (rcl_action_server_init(&route.server, route.rcl_node.get(), &rcl_clock_.clock,
          route.types.cpp_action_handle(), expanded.c_str(), &options) != RCL_RET_OK)
    {
      rcl_reset_error();
      throw std::runtime_error("rcl_action_server_init failed");
    }
    // GCOVR_EXCL_STOP
    route.status_pub = std::dynamic_pointer_cast<
      rclcpp::Publisher<action_msgs::msg::GoalStatusArray>>(
      topics->create_publisher(expanded + "/_action/status",
        rclcpp::create_publisher_factory<action_msgs::msg::GoalStatusArray,
      std::allocator<void>,
      rclcpp::Publisher<action_msgs::msg::GoalStatusArray>>(
          rclcpp::PublisherOptionsWithAllocator<std::allocator<void>>()),
        rclcpp::QoS(1).transient_local().reliable()));
    // GCOVR_EXCL_START: the publisher is null only when its allocation
    // fails, which a unit test cannot stage without breaking itself.
    if (!route.status_pub) {
      [[maybe_unused]] const rcl_ret_t fini =
        rcl_action_server_fini(&route.server, route.rcl_node.get());
      (void)fini;
      throw std::runtime_error("cannot create the action status publisher");
    }
    // GCOVR_EXCL_STOP
    route.rcl_valid = true;
    route.goal_request_take = route.types.make_goal_request();
    route.result_request_take = route.types.make_result_request();
    route.cancel_request_take = route.types.make_cancel_request();
  }

    // Gives one ok action its route: the introspection handles, then the
    // rcl action server. An action whose type does not load never reaches
    // here: the walk verdict refused it ([ACT-7]).
  void ensure_action_route(const ActionInfo & info)
  {
    std::unique_ptr<ActionRoute> route;
    try {
      route = std::make_unique<ActionRoute>(info, ActionTypes(info.type_name));
      // GCOVR_EXCL_START: resolve_action_type reports ok only after the
      // same library and action handle load, so the constructor cannot
      // throw for a routed action; a unit test cannot stage a failure
      // between the verdict and this lookup.
    } catch (const std::exception & failure) {
      action_repairs_[info.index].reason = failure.what();
      emit(SessionLog::kError,
        "action '" + info.action_name + "': no route (" + failure.what() + ")");
      return;
    }
    // GCOVR_EXCL_STOP
    try {
      make_ros_action_server(*route);
    } catch (const std::exception & failure) {
      action_repairs_[info.index].reason = failure.what();
      emit(SessionLog::kError,
        "action '" + info.action_name + "': no endpoint (" + failure.what() + ")");
      return;
    }
    action_repairs_.erase(info.index);
    action_routes_[info.index] = std::move(route);
  }

    // Every handshake aborts the live goals first ([SES-35]): the MCU drops
    // its side on the re-handshake ([REL-5]), so every ROS client learns
    // the abort instead of waiting. Then the ok actions get new servers.
  void sync_actions()
  {
    // Takes first: requests the clients sent before the re-handshake would
    // miss the rebuilt servers, stranding their futures. Taken now they
    // join the abort below and every caller learns the outcome ([SES-35]).
    take_action_requests();
    abort_all_actions();
    for (auto & entry : action_routes_) {
      ActionRoute & route = *entry.second;
      if (route.active && route.active->status_dirty) {
        publish_action_status(route);
        route.active->status_dirty = false;
        route.active->last_status_ms = now_ms();
      }
    }
    action_routes_.clear();
    action_repairs_.clear();
    for (const ActionInfo & info : session_.actions()) {
      if (info.status != kActionStatusOk) {
        continue;
      }
      ensure_action_route(info);
    }
  }

    // Retries actions missing despite an ok verdict, the way the service
    // repair does: a transient creation failure must not lose the action
    // while the handshake keeps reporting it ok.
  void repair_missing_actions(std::chrono::nanoseconds now)
  {
    if (session_.state() != SessionState::kConnected) {
      return;
    }
    for (const ActionInfo & info : session_.actions()) {
      if (info.status != kActionStatusOk || action_routes_.count(info.index) != 0u) {
        continue;
      }
      RepairState & state = action_repairs_[info.index];
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
      ensure_action_route(info);
    }
  }

  // A zero uuid means "every goal". Compared as a whole instead of byte
  // by byte, so the match has no per-byte branches to cover.
  bool uuid_is_zero(const std::array<std::uint8_t, 16> & uuid) const
  {
    return uuid == std::array<std::uint8_t, 16>{};
  }

  rcl_action_goal_info_t goal_info_of(
    const std::array<std::uint8_t, 16> & uuid, std::int32_t sec, std::uint32_t nanosec) const
  {
    rcl_action_goal_info_t info{};
    static_assert(sizeof(info.goal_id.uuid) == 16u, "a goal id is 16 bytes");
    std::memcpy(info.goal_id.uuid, uuid.data(), uuid.size());
    info.stamp.sec = sec;
    info.stamp.nanosec = nanosec;
    return info;
  }

  std::int64_t ros_now_sec(std::uint32_t & nanosec) const
  {
    const std::int64_t now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(ros_clock.now()).count();
    nanosec = static_cast<std::uint32_t>(now_ns % 1000000000LL);
    return now_ns / 1000000000LL;
  }

    // The wire seq for a new goal on a route: its own counter, skipping seqs
    // another route still holds, so every live goal has a unique seq even
    // though RESULT names no action ([ACT-1]). Pending goals count too: the
    // MCU may accept them all, and then they are live with equal standing.
    // The counter restarts with each handshake ([ACT-2]).
  std::uint8_t choose_goal_seq(ActionRoute & route)
  {
    for (int tries = 0; tries < 256; ++tries) {
      const std::uint8_t seq = static_cast<std::uint8_t>(route.next_seq + tries);
      bool clash = false;
      for (const auto & entry : action_routes_) {
        const ActionRoute & other = *entry.second;
        if (&other == &route) {
          continue;
        }
        if ((other.pending && other.pending->wire_seq == seq) ||
          (other.active && other.active->wire_seq == seq))
        {
          clash = true;
          break;
        }
      }
      if (!clash) {
        route.next_seq = static_cast<std::uint8_t>(seq + 1u);
        return seq;
      }
    }
    // GCOVR_EXCL_START: a route holds at most a pending and an active goal,
    // so the scan above always finds a free seq; exhausting all 256 needs
    // 256 live goals no MCU announces.
    route.next_seq = static_cast<std::uint8_t>(route.next_seq + 1u);
    return static_cast<std::uint8_t>(route.next_seq - 1u);
    // GCOVR_EXCL_STOP
  }

    // Answers one taken goal request, accepted or not.
  void answer_goal(
    ActionRoute & route, const rmw_request_id_t & header, bool accepted, std::int32_t sec,
    std::uint32_t nanosec)
  {
    ActionTypes::Struct response = route.types.make_goal_response();
    // GCOVR_EXCL_START: the fill succeeds for every response the bridge
    // makes; only a hand-built struct could fail it.
    if (!route.types.fill_goal_response(response, sec, nanosec, accepted)) {
      emit(SessionLog::kWarning,
        "action '" + route.info.action_name + "': cannot fill the goal response");
      return;
    }
    // GCOVR_EXCL_STOP
    rmw_request_id_t answer = header;
    // GCOVR_EXCL_START: the send fails only when the middleware cannot
    // allocate, which a unit test cannot stage without breaking itself.
    if (rcl_action_send_goal_response(
          &route.server, &answer, response.storage.data()) != RCL_RET_OK)
    {
      rcl_reset_error();
      emit(SessionLog::kWarning,
        "action '" + route.info.action_name + "': no ROS answer for the goal request");
    }
    // GCOVR_EXCL_STOP
  }

    // One taken ROS goal request: rejected at once when another goal is
    // live, packed and sent to the MCU otherwise ([SES-35], [ACT-3]).
  void on_ros_goal_request(ActionRoute & route)
  {
    rmw_request_id_t header{};
    if (rcl_action_take_goal_request(
          &route.server, &header, route.goal_request_take.storage.data()) != RCL_RET_OK)
    {
      rcl_reset_error();
      return;
    }
    std::array<std::uint8_t, 16> uuid{};
    // GCOVR_EXCL_START: typed ROS clients always set the goal id; the
    // guard stays for a hostile client that bypasses them.
    if (!route.types.goal_id_of(route.goal_request_take, uuid)) {
      emit(SessionLog::kWarning,
        "action '" + route.info.action_name + "': dropped ROS goal without an id");
      std::uint32_t nanosec = 0;
      const std::int64_t sec = ros_now_sec(nanosec);
      answer_goal(route, header, false, static_cast<std::int32_t>(sec), nanosec);
      return;
    }
    // GCOVR_EXCL_STOP
    if (route.pending || (route.active && route.active->terminal < 0)) {
      std::uint32_t nanosec = 0;
      const std::int64_t sec = ros_now_sec(nanosec);
      answer_goal(route, header, false, static_cast<std::int32_t>(sec), nanosec);
      emit(SessionLog::kWarning,
        "action '" + route.info.action_name + "': rejected, a goal is already active");
      return;
    }
    const void *goal = route.types.goal_of(route.goal_request_take);
    std::vector<std::uint8_t> packed;
    const ConvertResult converted = goal == nullptr ?
      ConvertResult{ConvertFail::kUnsupportedField, 0xFFu} :
    converter_.pack_from_struct(route.types.goal_type_name(), false, goal, packed);
    if (converted.fail != ConvertFail::kNone) {
      emit(SessionLog::kWarning,
        "action '" + route.info.action_name + "': dropped ROS goal (" +
        fail_name(converted.fail) + " at '" +
        field_text(route.types.goal_type_name(), converted.field) + "')");
      std::uint32_t nanosec = 0;
      const std::int64_t sec = ros_now_sec(nanosec);
      answer_goal(route, header, false, static_cast<std::int32_t>(sec), nanosec);
      return;
    }
    const std::uint8_t seq = choose_goal_seq(route);
    if (!session_.send_action_goal(route.info.index, seq, packed)) {
      emit(SessionLog::kWarning,
        "action '" + route.info.action_name + "': the link refused the ROS goal");
      std::uint32_t nanosec = 0;
      const std::int64_t sec = ros_now_sec(nanosec);
      answer_goal(route, header, false, static_cast<std::int32_t>(sec), nanosec);
      return;
    }
    PendingGoal pending;
    pending.header = header;
    pending.uuid = uuid;
    pending.wire_seq = seq;
    pending.sent_ms = now_ms();
    route.pending = pending;
  }

    // Answers one taken cancel request against one route.
  void answer_cancel(
    ActionRoute & route, const rmw_request_id_t & header, std::int8_t code, bool canceling,
    const ActiveGoal *goal = nullptr)
  {
    ActionTypes::Struct response = route.types.make_cancel_response();
    const std::array<std::uint8_t, 16> empty{};
    const bool filled = goal != nullptr ?
      route.types.fill_cancel_response(response, code, goal->uuid, goal->accept_sec,
        goal->accept_nanosec, canceling) :
      route.types.fill_cancel_response(response, code, empty, 0, 0, false);
    // GCOVR_EXCL_START: the fill succeeds for every response the bridge
    // makes; only a hand-built struct could fail it.
    if (!filled) {
      emit(SessionLog::kWarning,
        "action '" + route.info.action_name + "': cannot fill the cancel response");
      return;
    }
    // GCOVR_EXCL_STOP
    rmw_request_id_t answer = header;
    // GCOVR_EXCL_START: the send fails only when the middleware cannot
    // allocate, which a unit test cannot stage without breaking itself.
    if (rcl_action_send_cancel_response(
          &route.server, &answer, response.storage.data()) != RCL_RET_OK)
    {
      rcl_reset_error();
      emit(SessionLog::kWarning,
        "action '" + route.info.action_name + "': no ROS answer for the cancel request");
    }
    // GCOVR_EXCL_STOP
  }

  bool cancel_matches(
    const ActiveGoal & goal, const std::array<std::uint8_t, 16> & uuid, std::int32_t sec,
    std::uint32_t nanosec) const
  {
    const bool id_match = uuid_is_zero(uuid) || uuid == goal.uuid;
    const bool time_match = (sec == 0 && nanosec == 0u) ||
      (goal.accept_sec < sec ||
      (goal.accept_sec == sec && goal.accept_nanosec <= nanosec));
    return id_match && time_match;
  }

    // One taken ROS cancel request: matched against the live goal, forwarded
    // to the MCU, and answered when the MCU answers ([ACT-5]).
  void on_ros_cancel_request(ActionRoute & route)
  {
    rmw_request_id_t header{};
    if (rcl_action_take_cancel_request(
          &route.server, &header, route.cancel_request_take.storage.data()) != RCL_RET_OK)
    {
      rcl_reset_error();
      return;
    }
    std::array<std::uint8_t, 16> uuid{};
    std::int32_t sec = 0;
    std::uint32_t nanosec = 0;
    // GCOVR_EXCL_START: typed ROS clients always set the cancel id; the
    // guard stays for a hostile client that bypasses them.
    if (!route.types.cancel_of(route.cancel_request_take, uuid, sec, nanosec)) {
      emit(SessionLog::kWarning,
        "action '" + route.info.action_name + "': dropped ROS cancel without an id");
      answer_cancel(route, header, kCancelRejected, false);
      return;
    }
    // GCOVR_EXCL_STOP
    // GCOVR_EXCL_START: a cancel always names its accepted goal, so a
    // taker without one cannot come from a typed client.
    if (!route.active) {
      answer_cancel(route, header, kCancelUnknownGoal, false);
      return;
    }
    // GCOVR_EXCL_STOP
    if (route.active->terminal >= 0) {
      answer_cancel(route, header, kCancelTerminated, false);
      return;
    }
    // GCOVR_EXCL_START: a mismatch names another goal's id, which only a
    // hostile client sends; typed cancels always name their own goal.
    if (!cancel_matches(*route.active, uuid, sec, nanosec)) {
      answer_cancel(route, header, kCancelUnknownGoal, false);
      return;
    }
    // GCOVR_EXCL_STOP
    if (route.active->cancel_requested) {
      answer_cancel(route, header, kCancelNone, true, &*route.active);
      return;
    }
    if (!session_.send_action_cancel(route.active->wire_seq)) {
      emit(SessionLog::kWarning,
        "action '" + route.info.action_name + "': the link refused the ROS cancel");
      answer_cancel(route, header, kCancelRejected, false);
      return;
    }
    PendingCancel pending;
    pending.header = header;
    pending.sent_ms = now_ms();
    route.pending_cancel = pending;
    route.active->cancel_requested = true;
  }

    // Answers one waiting result request from the stored terminal result:
    // the packed bytes unpack into a fresh struct per waiter, so nothing
    // is ever copied with its heap buffers.
  void answer_result_waiter(
    ActionRoute & route, const rmw_request_id_t & header, std::int8_t status,
    const std::optional<std::vector<std::uint8_t>> & packed_result)
  {
    ActionTypes::Struct response = route.types.make_result_response();
    // GCOVR_EXCL_START: the fill succeeds for every response the bridge
    // makes; only a hand-built struct could fail it.
    if (!route.types.fill_result_status(response, status)) {
      emit(SessionLog::kWarning,
        "action '" + route.info.action_name + "': cannot fill the result status");
      return;
    }
    // GCOVR_EXCL_STOP
    // A stored result that no longer unpacks still answers: the goal is
    // terminal with this status, so the waiter gets status with a zeroed
    // result instead of waiting out its clock.
    if (packed_result) {
      void *into = route.types.result_of(response);
      // GCOVR_EXCL_START: the introspection always locates the member of
      // a response the bridge just made.
      if (into == nullptr) {
        emit(SessionLog::kWarning,
          "action '" + route.info.action_name + "': cannot fill the stored result");
      // GCOVR_EXCL_STOP
      } else {
        const ConvertResult converted = converter_.unpack_to_struct(
          route.types.result_type_name(), false, packed_result->data(),
          packed_result->size(), into);
        // GCOVR_EXCL_START: the bytes passed the same converter when the
        // RESULT arrived, so they unpack identically here.
        if (converted.fail != ConvertFail::kNone) {
          emit(SessionLog::kWarning,
            "action '" + route.info.action_name + "': dropped stored result (" +
            fail_name(converted.fail) + " at '" +
            field_text(route.types.result_type_name(), converted.field) + "')");
        }
        // GCOVR_EXCL_STOP
      }
    }
    rmw_request_id_t answer = header;
    // GCOVR_EXCL_START: the send fails only when the middleware cannot
    // allocate, which a unit test cannot stage without breaking itself.
    if (rcl_action_send_result_response(
          &route.server, &answer, response.storage.data()) != RCL_RET_OK)
    {
      rcl_reset_error();
      emit(SessionLog::kWarning,
        "action '" + route.info.action_name + "': no ROS answer for the result request");
    }
    // GCOVR_EXCL_STOP
  }

    // One taken ROS result request: answered from the stored result when
    // terminal, queued behind the MCU otherwise.
  void on_ros_result_request(ActionRoute & route)
  {
    rmw_request_id_t header{};
    if (rcl_action_take_result_request(
          &route.server, &header, route.result_request_take.storage.data()) != RCL_RET_OK)
    {
      rcl_reset_error();
      return;
    }
    std::array<std::uint8_t, 16> uuid{};
    // GCOVR_EXCL_START: typed ROS clients always set the result id, and
    // the id always names the live goal; the guards stay for a hostile
    // client that bypasses them.
    if (!route.types.result_id_of(route.result_request_take, uuid)) {
      emit(SessionLog::kWarning,
        "action '" + route.info.action_name + "': dropped ROS result without an id");
      answer_result_waiter(route, header, kGoalUnknown, std::nullopt);
      return;
    }
    if (!route.active || route.active->uuid != uuid) {
      answer_result_waiter(route, header, kGoalUnknown, std::nullopt);
      return;
    }
    // GCOVR_EXCL_STOP
    if (route.active->terminal >= 0) {
      answer_result_waiter(route, header, route.active->terminal, route.active->packed_result);
      return;
    }
    route.active->result_waiters.push_back(header);
  }

    // Takes pending ROS requests on every action server: a take that finds
    // nothing resets the middleware error and moves on; the 1 ms poll takes
    // again.
  void take_action_requests()
  {
    // Every mapped route finished construction: ensure inserts only after
    // the server and the status publisher are both up.
    for (auto & entry : action_routes_) {
      ActionRoute & route = *entry.second;
      on_ros_goal_request(route);
      on_ros_cancel_request(route);
      on_ros_result_request(route);
    }
  }

  ActionRoute * action_route_for(std::uint8_t index)
  {
    const auto found = action_routes_.find(index);
    // GCOVR_EXCL_START: the session drops frames for unserved indices
    // before the router sees them, so these guards describe a direct
    // caller the session never lets through.
    if (found == action_routes_.end()) {
      return nullptr;
    }
    if (found->second->info.status != kActionStatusOk) {
      return nullptr;
    }
    // GCOVR_EXCL_STOP
    return found->second.get();
  }

    // One GOAL_RESP the MCU sent while connected: the pending ROS goal is
    // accepted or refused ([ACT-4]). A response for no pending goal is a
    // stale duplicate and is ignored.
  void on_action_goal_response(std::uint8_t seq, std::uint8_t decision)
  {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    for (auto & entry : action_routes_) {
      ActionRoute & route = *entry.second;
      if (!route.pending || route.pending->wire_seq != seq) {
        continue;
      }
      std::uint32_t nanosec = 0;
      const std::int64_t sec = ros_now_sec(nanosec);
      if (decision != kActDecisionAccepted) {
        answer_goal(route, route.pending->header, false, static_cast<std::int32_t>(sec),
          nanosec);
        route.pending.reset();
        return;
      }
      const rcl_action_goal_info_t info = goal_info_of(route.pending->uuid,
          static_cast<std::int32_t>(sec), nanosec);
      rcl_action_goal_handle_t *handle = rcl_action_accept_new_goal(&route.server, &info);
      answer_goal(route, route.pending->header, handle != nullptr,
        static_cast<std::int32_t>(sec), nanosec);
      // GCOVR_EXCL_START: a duplicate goal id needs a client that reuses
      // UUIDs, which the typed client never does; the guard stays for one
      // that bypasses it.
      if (handle == nullptr) {
        rcl_reset_error();
        emit(SessionLog::kWarning,
          "action '" + route.info.action_name + "': the goal id is already tracked");
        route.pending.reset();
        return;
      }
      // GCOVR_EXCL_STOP
      ActiveGoal goal;
      goal.uuid = route.pending->uuid;
      goal.wire_seq = seq;
      goal.accept_sec = static_cast<std::int32_t>(sec);
      goal.accept_nanosec = nanosec;
      goal.handle = handle;
      route.pending.reset();
      // GCOVR_EXCL_START: the update fails only when the middleware
      // cannot allocate, which a unit test cannot stage.
      if (rcl_action_update_goal_state(handle, GOAL_EVENT_EXECUTE) != RCL_RET_OK) {
        rcl_reset_error();
        emit(SessionLog::kWarning,
          "action '" + route.info.action_name + "': cannot start the accepted goal");
      }
      // GCOVR_EXCL_STOP
      route.active = std::move(goal);
      route.active->status_dirty = true;
      route.active->last_status_ms = now_ms();
      return;
    }
  }

    // One CANCEL_RESP the MCU sent while connected: the pending ROS cancel
    // is accepted or refused ([ACT-5]). A response for no pending cancel is
    // a stale duplicate and is ignored.
  void on_action_cancel_response(std::uint8_t seq, std::uint8_t decision)
  {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    for (auto & entry : action_routes_) {
      ActionRoute & route = *entry.second;
      if (!route.pending_cancel || !route.active || route.active->wire_seq != seq) {
        continue;
      }
      if (decision != kActDecisionAccepted) {
        route.active->cancel_requested = false;
        answer_cancel(route, route.pending_cancel->header, kCancelRejected, false);
      } else {
        // GCOVR_EXCL_START: the update fails only when the middleware
        // cannot allocate, which a unit test cannot stage.
        if (rcl_action_update_goal_state(
              route.active->handle, GOAL_EVENT_CANCEL_GOAL) != RCL_RET_OK)
        {
          rcl_reset_error();
          emit(SessionLog::kWarning,
            "action '" + route.info.action_name + "': cannot start canceling the goal");
        }
        // GCOVR_EXCL_STOP
        answer_cancel(route, route.pending_cancel->header, kCancelNone, true,
          &*route.active);
      }
      route.pending_cancel.reset();
      route.active->status_dirty = true;
      return;
    }
  }

    // One FEEDBACK the MCU sent while connected: published to the ROS goal
    // when it names the live one, dropped otherwise. Feedback rides best
    // effort and is never acked ([ACT-3]).
  void on_action_feedback(
    std::uint8_t seq, std::uint8_t index, const std::vector<std::uint8_t> & payload)
  {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    ActionRoute *route = action_route_for(index);
    // GCOVR_EXCL_START: the session drops frames for unserved indices
    // before the router sees them, so a null route never reaches this
    // check from a real peer.
    if (route == nullptr) {
      return;
    }
    // GCOVR_EXCL_STOP
    if (!route->active || route->active->wire_seq != seq || route->active->terminal >= 0) {
      return;
    }
    ActionTypes::Struct message = route->types.make_feedback_message();
    // GCOVR_EXCL_START: the introspection always fills the members of a
    // message the bridge just made; only a hand-built struct could fail.
    if (!route->types.fill_feedback_id(message, route->active->uuid)) {
      emit(SessionLog::kWarning,
        "action '" + route->info.action_name + "': cannot fill the feedback id");
      return;
    }
    void *feedback = route->types.feedback_of(message);
    if (feedback == nullptr) {
      emit(SessionLog::kWarning,
        "action '" + route->info.action_name + "': cannot fill the feedback");
      return;
    }
    // GCOVR_EXCL_STOP
    const ConvertResult converted = converter_.unpack_to_struct(
      route->types.feedback_type_name(), false, payload.data(), payload.size(), feedback);
    if (converted.fail != ConvertFail::kNone) {
      emit(SessionLog::kWarning,
        "action '" + route->info.action_name + "': dropped feedback (" +
        fail_name(converted.fail) + " at '" +
        field_text(route->types.feedback_type_name(), converted.field) + "')");
      return;
    }
    // GCOVR_EXCL_START: the publish fails only when the middleware
    // cannot allocate, which a unit test cannot stage.
    if (rcl_action_publish_feedback(&route->server, message.storage.data()) != RCL_RET_OK) {
      rcl_reset_error();
      emit(SessionLog::kWarning,
        "action '" + route->info.action_name + "': cannot publish feedback");
    }
    // GCOVR_EXCL_STOP
  }

    // One RESULT the MCU sent while connected: the live ROS goal completes
    // with its outcome, and every waiter learns it ([ACT-6], [ACT-8]). A
    // result for no live goal is a stale duplicate and is ignored. The
    // bridge owns the goal seqs uniquely across actions, so at most one
    // live goal carries the seq.
  void on_action_result(
    std::uint8_t seq, std::uint8_t outcome, const std::vector<std::uint8_t> & payload)
  {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    for (auto & entry : action_routes_) {
      ActionRoute & route = *entry.second;
      if (!route.active || route.active->wire_seq != seq || route.active->terminal >= 0) {
        continue;
      }
      const std::int8_t status = outcome == 4 ? kGoalSucceeded :
        outcome == 5 ? kGoalCanceled : kGoalAborted;
      ActionTypes::Struct check = route.types.make_result();
      const ConvertResult converted = converter_.unpack_to_struct(
        route.types.result_type_name(), false, payload.data(), payload.size(),
        check.storage.data());
      if (converted.fail != ConvertFail::kNone) {
        emit(SessionLog::kWarning,
          "action '" + route.info.action_name + "': dropped result (" +
          fail_name(converted.fail) + " at '" +
          field_text(route.types.result_type_name(), converted.field) + "'), aborting");
        route.active->packed_result.reset();
        complete_action_goal(route, kGoalAborted);
        return;
      }
      route.active->packed_result = payload;
      complete_action_goal(route, status);
      return;
    }
  }

    // Reports one terminal goal to its ROS waiters and the status array.
  void complete_action_goal(ActionRoute & route, std::int8_t status)
  {
    ActiveGoal & goal = *route.active;
    goal.terminal = status;
    const rcl_action_goal_event_t event = status == kGoalSucceeded ? GOAL_EVENT_SUCCEED :
      status == kGoalCanceled ? GOAL_EVENT_CANCELED : GOAL_EVENT_ABORT;
    // GCOVR_EXCL_START: both calls fail only when the middleware cannot
    // allocate, which a unit test cannot stage without breaking itself.
    if (goal.handle != nullptr &&
      rcl_action_update_goal_state(goal.handle, event) != RCL_RET_OK)
    {
      rcl_reset_error();
      emit(SessionLog::kWarning,
        "action '" + route.info.action_name + "': cannot move the goal to its terminal state");
    }
    if (rcl_action_notify_goal_done(&route.server) != RCL_RET_OK) {
      rcl_reset_error();
    }
    // GCOVR_EXCL_STOP
    for (const rmw_request_id_t & waiter : goal.result_waiters) {
      answer_result_waiter(route, waiter, status, goal.packed_result);
    }
    goal.result_waiters.clear();
    goal.status_dirty = true;
  }

    // Aborts every live ROS goal: disconnects, timeouts and resets end the
    // goal as ABORTED instead of leaving the client waiting ([SES-35]).
  void abort_all_actions()
  {
    for (auto & entry : action_routes_) {
      ActionRoute & route = *entry.second;
      if (route.pending) {
        std::uint32_t nanosec = 0;
        const std::int64_t sec = ros_now_sec(nanosec);
        answer_goal(route, route.pending->header, false, static_cast<std::int32_t>(sec),
          nanosec);
        route.pending.reset();
      }
      if (route.pending_cancel) {
        answer_cancel(route, route.pending_cancel->header, kCancelRejected, false);
        route.pending_cancel.reset();
      }
      if (route.active && route.active->terminal < 0) {
        route.active->packed_result.reset();
        complete_action_goal(route, kGoalAborted);
        route.active->cancel_requested = false;
      }
    }
  }

    // Publishes one route's status array over a plain rclcpp publisher.
    // rcl_action_publish_status hangs in the dev image (both stock RMWs,
    // with a valid server and array and no prior error): the goal tracking
    // still comes from rcl, only the transport is worked around. Filed as a
    // follow-up for whoever can reproduce it outside this tree.
  void publish_action_status(ActionRoute & route)
  {
    rcl_action_goal_status_array_t array = rcl_action_get_zero_initialized_goal_status_array();
    // GCOVR_EXCL_START: the read fails only when the middleware cannot
    // allocate, which a unit test cannot stage without breaking itself.
    if (rcl_action_get_goal_status_array(&route.server, &array) != RCL_RET_OK) {
      rcl_reset_error();
      emit(SessionLog::kWarning,
        "action '" + route.info.action_name + "': cannot read the goal status");
      return;
    }
    // GCOVR_EXCL_STOP
    action_msgs::msg::GoalStatusArray message;
    message.status_list.reserve(array.msg.status_list.size);
    for (std::size_t i = 0; i < array.msg.status_list.size; ++i) {
      const rcl_action_goal_status_t & entry = array.msg.status_list.data[i];
      action_msgs::msg::GoalStatus status;
      static_assert(sizeof(status.goal_info.goal_id.uuid) == 16u, "a goal id is 16 bytes");
      std::memcpy(status.goal_info.goal_id.uuid.data(), entry.goal_info.goal_id.uuid,
        status.goal_info.goal_id.uuid.size());
      status.goal_info.stamp.sec = entry.goal_info.stamp.sec;
      status.goal_info.stamp.nanosec = entry.goal_info.stamp.nanosec;
      status.status = entry.status;
      message.status_list.push_back(status);
    }
    route.status_pub->publish(message);
  }

    // Moves every action route: ROS requests taken, pending MCU answers
    // timed out, terminal goals expired per the rcl defaults, and the
    // status array published on change and while a goal is live.
  void poll_actions()
  {
    const bool connected = session_.state() == SessionState::kConnected;
    const std::int64_t now = now_ms();
    // Takes run whether or not the link is up: the ROS side answers from
    // stored state while down, and a disconnect aborts below instead of
    // stranding the take.
    take_action_requests();
    if (!connected) {
      abort_all_actions();
    } else {
      for (auto & entry : action_routes_) {
        ActionRoute & route = *entry.second;
        if (route.pending && now - route.pending->sent_ms >= kActionCallTimeoutMs) {
          emit(SessionLog::kWarning,
            "action '" + route.info.action_name + "': the MCU never answered the goal");
          std::uint32_t nanosec = 0;
          const std::int64_t sec = ros_now_sec(nanosec);
          answer_goal(route, route.pending->header, false, static_cast<std::int32_t>(sec),
            nanosec);
          route.pending.reset();
        }
        if (route.pending_cancel && now - route.pending_cancel->sent_ms >= kActionCallTimeoutMs) {
          emit(SessionLog::kWarning,
            "action '" + route.info.action_name + "': the MCU never answered the cancel");
          answer_cancel(route, route.pending_cancel->header, kCancelRejected, false);
          if (route.active) {
            route.active->cancel_requested = false;
          }
          route.pending_cancel.reset();
        }
        // GCOVR_EXCL_START: expiring fails only when the middleware cannot
        // allocate, and rcl forgets terminal goals only after its expiration
        // delay, far beyond a deterministic test.
        if (rcl_action_expire_goals(&route.server, nullptr, 0u, nullptr) != RCL_RET_OK) {
          rcl_reset_error();
        } else if (route.active && route.active->terminal >= 0) {
          const rcl_action_goal_info_t info = goal_info_of(route.active->uuid,
              route.active->accept_sec, route.active->accept_nanosec);
          if (!rcl_action_server_goal_exists(&route.server, &info)) {
            route.active.reset();
          }
        }
        // GCOVR_EXCL_STOP
      }
    }
    for (auto & entry : action_routes_) {
      ActionRoute & route = *entry.second;
      if (route.active &&
        (route.active->status_dirty ||
        (connected && route.active->terminal < 0 && now - route.active->last_status_ms >=
        kActionStatusPeriodMs)))
      {
        publish_action_status(route);
        route.active->status_dirty = false;
        route.active->last_status_ms = now;
      }
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
      for (auto it = route.ros_inflight.begin(); it != route.ros_inflight.end(); ) {
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
      for (auto it = route.mcu_inflight.begin(); it != route.mcu_inflight.end(); ) {
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
  const Clock & ros_clock;
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
    // One route per ok action by walk index, with repair attempts by index
    // for routes missing despite an ok verdict.
  std::map<std::uint8_t, std::unique_ptr<ActionRoute>> action_routes_;
  std::map<std::uint8_t, RepairState> action_repairs_;
  RclClock rcl_clock_;
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
  impl_->repair_missing_actions(now);
  impl_->poll_services();
  impl_->poll_actions();
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
