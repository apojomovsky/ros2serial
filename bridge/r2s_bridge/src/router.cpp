// The link between the MCU's topics and the ROS graph: generic publishers
// and subscriptions under the announced names, paced host-to-MCU traffic,
// and converted payloads both directions.
#include "r2s_bridge/router.hpp"

#include <array>
#include <cmath>
#include <mutex>
#include <optional>
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

// How often a poll retries a route missing despite an ok verdict, and how
// many poll attempts one handshake episode gets on top of the handshake's
// own: a transient creation failure heals within one spacing, while a
// deterministically uncreatable name stops retrying after its episode.
constexpr int kRepairSpacingMs = 100;
constexpr int kMaxRepairAttempts = 10;

// The link state the H8 snapshot reports, one word per state. The table order
// follows the SessionState enumerators, so no branch can go untaken.
std::string link_state_name(SessionState state)
{
    static constexpr const char *names[] = {"disconnected", "boot_wait", "handshaking", "connected"};
    return names[static_cast<std::size_t>(state)];
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
         const Clock &in_clock, const Clock &in_ros_clock, RouterParams in_params, LogSink in_sink,
         SessionConfig session_config, LogSink in_mcu_sink)
        : topics(in_topics), params(std::move(in_params)), sink(std::move(in_sink)), clock(in_clock),
          mcu_sink(std::move(in_mcu_sink)),
          session_(session_config, in_clock, in_ros_clock, std::move(in_open), resolve_topic_type,
                   SessionEvents{log_event(), handshake_event(), user_event(), mcu_log_event(), {}})
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

    // One routed topic's endpoints. Declared up here because the route
    // builders below name it in their signatures.
    struct Route {
        TopicInfo info;
        rclcpp::GenericPublisher::SharedPtr pub;
        rclcpp::GenericSubscription::SharedPtr sub;
        std::size_t publish_failures = 0;
    };

    // Poll-time repair attempts per topic missing despite an ok verdict: how
    // many ran, when the last ran, and why creation failed. A handshake
    // clears the table, renewing every missing route's episode.
    struct RepairState {
        int attempts = 0;
        std::chrono::nanoseconds last_attempt{0};
        std::string reason;
    };

    std::function<void(SessionLog, const std::string &)> log_event()
    {
        return [this](SessionLog level, const std::string &text) { emit(level, text); };
    }

    // The MCU's own lines go to their own sink when one is set, so the node
    // can log them under its child name; otherwise they join the log sink.
    std::function<void(SessionLog, const std::string &)> mcu_log_event()
    {
        return [this](SessionLog level, const std::string &text) {
            if (mcu_sink) {
                mcu_sink(level, text);
            } else {
                emit(level, text);
            }
        };
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

    // Creates one route's endpoint, reporting the failure through the sink at
    // the caller's level. Empty when the middleware refuses the endpoint, and
    // the error text explains why for the diagnostics snapshot.
    std::optional<Route> make_route(const TopicInfo &info, SessionLog failure_level, std::string &error)
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
        } catch (const std::exception &failure) {
            error = failure.what();
            emit(failure_level, "topic '" + info.topic_name + "': no publisher or subscription: " + error);
            return std::nullopt;
        }
        error.clear();
        return route;
    }

    // The pacer entry the handshake registers for one routed topic.
    SchedulerTopic pacing_for(const TopicInfo &info) const
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
    void ensure_route(const TopicInfo &info, SessionLog failure_level)
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
        for (const TopicInfo &info : session_.topics()) {
            if (info.status != kTopicStatusOk || routes.count(info.topic_id) != 0u) {
                continue;
            }
            RepairState &state = repairs_[info.topic_id];
            if (state.attempts >= kMaxRepairAttempts) {
                continue;
            }
            if (state.attempts > 0 &&
                now - state.last_attempt < std::chrono::milliseconds(kRepairSpacingMs)) {
                continue;
            }
            state.last_attempt = now;
            ++state.attempts;
            ensure_route(info, SessionLog::kDebug);
        }
    }

    void on_handshake(bool identical)
    {
        std::lock_guard<std::mutex> guard(mutex);
        // A new handshake renews the repair attempts: a still-missing route
        // retries from scratch instead of inheriting a spent episode.
        repairs_.fill(RepairState{});
        if (identical && !routes.empty()) {
            // The endpoints stay, but the MCU's new limits may exceed the old
            // bucket: refresh the burst without touching the queues.
            scheduler_.set_max_burst(
                static_cast<double>(Scheduler::encoded_frame_bound(session_.mcu_max_frame())));
            return;
        }
        routes.clear();
        const double budget_bytes_per_sec =
            static_cast<double>(params.baud) / 10.0 * static_cast<double>(params.tx_budget_pct) / 100.0;
        const double burst = static_cast<double>(Scheduler::encoded_frame_bound(session_.mcu_max_frame()));
        scheduler_.configure({}, budget_bytes_per_sec, burst);
        for (const TopicInfo &info : session_.topics()) {
            if (info.status != kTopicStatusOk) {
                continue;
            }
            ensure_route(info, SessionLog::kError);
        }
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
        ConvertResult result;
        try {
            result = converter_.to_cdr(route.info.type_name, route.info.narrow(), payload.data(),
                                       payload.size(), cdr);
            // GCOVR_EXCL_START: the middleware fails only when it cannot
            // allocate or serialize, which a unit test cannot inject.
        } catch (const std::exception &error) {
            // A failed serialize (the middleware short under load) is a
            // dropped frame, never an abort: it names its topic and counts.
            emit(SessionLog::kWarning,
                 "topic '" + route.info.topic_name + "': failed to serialize (" + error.what() + ")");
            // The route exists: only the poll thread mutates the table, so
            // the entry looked up above is still there.
            std::lock_guard<std::mutex> guard(mutex);
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
        rcl_serialized_message_t &rcl = out.get_rcl_serialized_message();
        std::memcpy(rcl.buffer, cdr.data(), cdr.size());
        rcl.buffer_length = cdr.size();
        try {
            route.pub->publish(out);
            // GCOVR_EXCL_BR_START: a unit test cannot throw out of the logging,
            // locking, or counting calls below, nor inject a foreign exception
            // type, so the unwind edges never run.
        } catch (const std::exception &error) {
            // A failed publish (a context shutting down under the poll loop)
            // is a dropped frame, never an abort: it names its topic and counts.
            emit(SessionLog::kWarning,
                 "topic '" + route.info.topic_name + "': failed to publish (" + error.what() + ")");
            // The route exists: only the poll thread mutates the table, so
            // the entry looked up above is still there.
            std::lock_guard<std::mutex> guard(mutex);
            ++routes[topic_id].publish_failures;
            return;
            // GCOVR_EXCL_BR_STOP
        }
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

    rclcpp::node_interfaces::NodeTopicsInterface::SharedPtr topics;
    RouterParams params;
    LogSink sink;
    const Clock &clock;
    LogSink mcu_sink;
    Converter converter_;
    Scheduler scheduler_;
    std::map<std::uint8_t, Route> routes;
    // Repair attempts by topic id: array indexing cannot miss, so the
    // diagnostics snapshot below needs no fallback branch.
    std::array<RepairState, 256> repairs_;
    std::mutex mutex;
    Session session_;
};

Router::Router(rclcpp::node_interfaces::NodeTopicsInterface::SharedPtr topics, TransportFactory open,
               const Clock &clock, const Clock &ros_clock, RouterParams params, LogSink sink,
               SessionConfig session_config, LogSink mcu_sink)
    : impl_(std::make_unique<Impl>(std::move(topics), std::move(open), clock, ros_clock, std::move(params),
                                   std::move(sink), std::move(session_config), std::move(mcu_sink)))
{
}

Router::~Router() = default;

void Router::poll()
{
    impl_->session_.poll();
    const std::chrono::nanoseconds now = impl_->clock.now();
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->repair_missing_routes(now);
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
    out.control_coalesced = impl_->session_.control_coalesced();
    out.protocol_version = R2S_PROTOCOL_VERSION;
    out.mcu_counters = impl_->session_.mcu_counters();
    out.mcu_error_sums = impl_->session_.error_sums();
    for (const TopicInfo &info : impl_->session_.topics()) {
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

} // namespace r2s_bridge
