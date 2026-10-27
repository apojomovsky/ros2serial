// The link between the MCU's topics and the ROS graph. Scaffolding for the
// tests-first commit; the routing lands with the change.
#include "r2s_bridge/router.hpp"

#include <stdexcept>

namespace r2s_bridge
{

std::map<std::string, double> parse_rate_overrides(const std::vector<std::string> &, const LogSink &)
{
    return {};
}

struct Router::Impl {
    Impl(rclcpp::node_interfaces::NodeTopicsInterface::SharedPtr, rclcpp::Logger, TransportFactory,
         const Clock &, RouterParams params, LogSink)
    {
        if (params.qos_depth < 1u || params.tx_budget_pct < 0 || params.tx_budget_pct > 100 ||
            params.baud <= 0 || params.rate_default_hz < 0.0) {
            throw std::invalid_argument("r2s_bridge::Router: a pacing parameter is out of range");
        }
    }
};

Router::Router(rclcpp::node_interfaces::NodeTopicsInterface::SharedPtr topics, rclcpp::Logger logger,
               TransportFactory open, const Clock &clock, RouterParams params, LogSink sink)
    : impl_(
          std::make_unique<Impl>(std::move(topics), logger, std::move(open), clock, params, std::move(sink)))
{
}

Router::~Router() = default;

void Router::poll()
{
}

std::size_t Router::publisher_count() const
{
    return 0u;
}

std::size_t Router::subscription_count() const
{
    return 0u;
}

std::size_t Router::dropped(std::uint8_t) const
{
    return 0u;
}

} // namespace r2s_bridge
