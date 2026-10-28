// The link between the MCU's topics and the ROS graph: generic publishers
// and subscriptions under the announced names, paced host-to-MCU traffic,
// and converted payloads both directions.
#include "r2s_bridge/router.hpp"

#include <cmath>
#include <mutex>
#include <stdexcept>
#include <utility>

#include <rclcpp/create_generic_publisher.hpp>
#include <rclcpp/create_generic_subscription.hpp>

#include "r2s_bridge/converter.hpp"
#include "r2s_bridge/type_support.hpp"
extern "C" {
#include "r2s/r2s.h"
}

namespace r2s_bridge
{
namespace
{
constexpr std::uint8_t kReliableFlag = 0x08u;

// The link state the H8 snapshot reports, one word per state.
std::string link_state_name(SessionState state)
{
    switch (state) {
    case SessionState::kDisconnected:
        return "disconnected";
    case SessionState::kBootWait:
        return "boot_wait";
    case SessionState::kHandshaking:
        return "handshaking";
    case SessionState::kConnected:
        return "connected";
    }
    return "unknown";
}
const char *fail_name(ConvertFail fail)
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

} // namespace

std::map<std::string, double> parse_rate_overrides(const std::vector<std::string> &entries,
                                                   const LogSink &sink)
{
    std::map<std::string, double> rates;
    for (const std::string &entry : entries) {
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

struct Router::Impl {
    Impl(rclcpp::node_interfaces::NodeTopicsInterface::SharedPtr in_topics, TransportFactory in_open,
         const Clock &in_clock, RouterParams in_params, LogSink in_sink, SessionConfig session_config)
        : topics(in_topics), params(std::move(in_params)), sink(std::move(in_sink)), clock(in_clock),
          session_(session_config, in_clock, std::move(in_open), resolve_topic_type,
                   SessionEvents{log_event(), handshake_event(), user_event()})
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
    }

    std::function<void(SessionLog, const std::string &)> log_event()
    {
        return [this](SessionLog level, const std::string &text) { emit(level, text); };
    }

    std::function<void(bool)> handshake_event()
    {
        return [this](bool identical) { on_handshake(identical); };
    }

    std::function<void(std::uint8_t, const std::vector<std::uint8_t> &)> user_event()
    {
        return
            [this](std::uint8_t id, const std::vector<std::uint8_t> &payload) { on_user_frame(id, payload); };
    }

    void emit(SessionLog level, const std::string &text)
    {
        sink(level, text);
    }

    rclcpp::QoS route_qos(const TopicInfo &info) const
    {
        rclcpp::QoS qos(params.qos_depth);
        if ((info.flags & kReliableFlag) != 0u) {
            qos.reliable();
        } else {
            qos.best_effort();
        }
        return qos;
    }

    void on_handshake(bool identical)
    {
        std::lock_guard<std::mutex> guard(mutex);
        if (identical && !routes.empty()) {
            // The endpoints stay, but the MCU's new limits may exceed the old
            // bucket: refresh the burst without touching the queues.
            scheduler_.set_max_burst(
                static_cast<double>(Scheduler::encoded_frame_bound(session_.mcu_max_frame())));
            return;
        }
        routes.clear();
        std::vector<SchedulerTopic> pacing;
        const double budget_bytes_per_sec =
            static_cast<double>(params.baud) / 10.0 * static_cast<double>(params.tx_budget_pct) / 100.0;
        for (const TopicInfo &info : session_.topics()) {
            if (info.status != kTopicStatusOk) {
                continue;
            }
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
            } catch (const std::exception &error) {
                emit(SessionLog::kError,
                     "topic '" + info.topic_name + "': no publisher or subscription: " + error.what());
                continue;
            }
            SchedulerTopic paced;
            paced.topic_id = info.topic_id;
            paced.keep_latest = info.keep_latest();
            const auto limited = params.rate_limits.find(info.topic_name);
            paced.rate_hz = limited == params.rate_limits.end() ? params.rate_default_hz : limited->second;
            paced.queue_bound = paced.keep_latest ? 1u : params.qos_depth;
            pacing.push_back(paced);
            routes[info.topic_id] = std::move(route);
        }
        const double burst = static_cast<double>(Scheduler::encoded_frame_bound(session_.mcu_max_frame()));
        scheduler_.configure(std::move(pacing), budget_bytes_per_sec, burst);
    }

    void on_user_frame(std::uint8_t topic_id, const std::vector<std::uint8_t> &payload)
    {
        Route route;
        {
            std::lock_guard<std::mutex> guard(mutex);
            const auto found = routes.find(topic_id);
            if (found == routes.end() || found->second.pub == nullptr) {
                return;
            }
            route = found->second;
        }
        std::vector<std::uint8_t> cdr;
        const ConvertResult result =
            converter_.to_cdr(route.info.type_name, route.info.narrow(), payload.data(), payload.size(), cdr);
        if (result.fail != ConvertFail::kNone) {
            emit(SessionLog::kWarning, "topic '" + route.info.topic_name + "': dropped packed payload (" +
                                           fail_name(result.fail) + " at '" +
                                           field_text(route.info.type_name, result.field) + "')");
            return;
        }
        rclcpp::SerializedMessage out(cdr.size());
        rcl_serialized_message_t &rcl = out.get_rcl_serialized_message();
        std::memcpy(rcl.buffer, cdr.data(), cdr.size());
        rcl.buffer_length = cdr.size();
        route.pub->publish(out);
    }

    void on_ros_message(std::uint8_t topic_id, std::shared_ptr<rclcpp::SerializedMessage> msg)
    {
        Route route;
        {
            std::lock_guard<std::mutex> guard(mutex);
            const auto found = routes.find(topic_id);
            if (found == routes.end() || found->second.sub == nullptr) {
                return;
            }
            route = found->second;
        }
        const rcl_serialized_message_t &rcl = msg->get_rcl_serialized_message();
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
                                           " bytes, past the MCU max_frame " + std::to_string(limit));
            return;
        }
        std::lock_guard<std::mutex> guard(mutex);
        scheduler_.submit(topic_id, std::move(packed));
    }

    std::string field_text(const std::string &type, std::uint8_t field)
    {
        const std::string path = converter_.field_path(type, field);
        return path.empty() ? "#" + std::to_string(field) : path;
    }

    struct Route {
        TopicInfo info;
        rclcpp::GenericPublisher::SharedPtr pub;
        rclcpp::GenericSubscription::SharedPtr sub;
    };

    rclcpp::node_interfaces::NodeTopicsInterface::SharedPtr topics;
    RouterParams params;
    LogSink sink;
    const Clock &clock;
    Converter converter_;
    Scheduler scheduler_;
    std::map<std::uint8_t, Route> routes;
    std::mutex mutex;
    Session session_;
};

Router::Router(rclcpp::node_interfaces::NodeTopicsInterface::SharedPtr topics, TransportFactory open,
               const Clock &clock, RouterParams params, LogSink sink, SessionConfig session_config)
    : impl_(std::make_unique<Impl>(std::move(topics), std::move(open), clock, std::move(params),
                                   std::move(sink), std::move(session_config)))
{
}

Router::~Router() = default;

void Router::poll()
{
    impl_->session_.poll();
    const std::chrono::nanoseconds now = impl_->clock.now();
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->scheduler_.poll(now, [this](std::uint8_t id, const std::vector<std::uint8_t> &payload) {
        return impl_->session_.send_user(id, payload);
    });
}

std::size_t Router::publisher_count() const
{
    std::lock_guard<std::mutex> guard(impl_->mutex);
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
    std::lock_guard<std::mutex> guard(impl_->mutex);
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
    std::lock_guard<std::mutex> guard(impl_->mutex);
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
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->scheduler_.dropped(topic_id);
}

std::size_t Router::queued(std::uint8_t topic_id) const
{
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->scheduler_.queued(topic_id);
}

Router::Diagnostics Router::diagnostics() const
{
    std::lock_guard<std::mutex> guard(impl_->mutex);
    Diagnostics out;
    out.link_state = link_state_name(impl_->session_.state());
    out.session_token = impl_->session_.token();
    out.resets = impl_->session_.resets();
    out.protocol_version = R2S_PROTOCOL_VERSION;
    out.mcu_counters = impl_->session_.mcu_counters();
    for (const auto &[id, route] : impl_->routes) {
        TopicDiagnostics topic;
        topic.topic_id = id;
        topic.topic_name = route.info.topic_name;
        topic.device_to_host = route.info.device_to_host();
        topic.seq_gaps = impl_->session_.seq_gaps(id);
        topic.dropped = impl_->scheduler_.dropped(id);
        out.topics.push_back(topic);
    }
    return out;
}

} // namespace r2s_bridge
