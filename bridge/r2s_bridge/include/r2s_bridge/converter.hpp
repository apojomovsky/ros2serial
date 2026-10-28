// Packed wire payloads to serialized CDR, through introspection with the plan
// per type cached (docs/00-design.md, E4; spec/protocol.md, sections 6 and 7.5).
// The walk is depth-first over the field definitions, so the index reported
// for a bad payload matches the MCU codec's error report index ([ERR-4]).
#ifndef R2S_BRIDGE__CONVERTER_HPP_
#define R2S_BRIDGE__CONVERTER_HPP_

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct rosidl_message_type_support_t;

namespace r2s_bridge
{

// Why a packed payload was dropped. Truncated and Trailing are [ENC-17] and
// [ENC-18]; BadVarint covers [VAR-2], [VAR-3] and [VAR-4]; BadBool is [ENC-6]
// and NulInString is [ENC-14]. UnknownType names a type the installed packages
// do not provide, and UnsupportedField one whose layout has no wire form, so
// the caller refuses the topic instead of dropping its frames. OverBound is a
// bounded sequence longer than the host type holds.
enum class ConvertFail : std::uint8_t {
    kNone = 0,
    kUnknownType,
    kUnsupportedField,
    kTruncated,
    kTrailing,
    kBadVarint,
    kBadBool,
    kNulInString,
    kOverBound,
};

// The outcome of one conversion. On success fail is kNone and cdr holds the
// serialized message; otherwise field is the depth-first index of the
// offending field definition, or 0xFF when no field is named.
struct ConvertResult {
    ConvertFail fail = ConvertFail::kNone;
    std::uint8_t field = 0xFFu;
};

// Converts packed wire payloads to serialized CDR, byte-identical to rclcpp
// serialization of the same message. Plans are built once per type from the
// installed introspection type support and cached; the narrow flag is a
// per-call input, never part of the plan ([ENC-10], [CTL-5]). Not copyable;
// safe to share between threads. May throw std::bad_alloc, and
// std::runtime_error when the middleware cannot serialize.
class Converter
{
  public:
    Converter();
    ~Converter();
    Converter(const Converter &) = delete;
    Converter &operator=(const Converter &) = delete;

    // Converts one packed payload of type_name to serialized CDR in cdr,
    // clearing cdr first. narrow selects binary32 float64s on the wire.
    ConvertResult to_cdr(const std::string &type_name, bool narrow, const std::uint8_t *data,
                         std::size_t size, std::vector<std::uint8_t> &cdr);

    // Converts one serialized CDR message of type_name to a packed payload in
    // packed, clearing packed first. narrow packs float64s as binary32. The
    // cached plan is shared with to_cdr. A CDR the middleware cannot read
    // back, and a string or sequence longer than the wire varint holds, fail
    // as kUnsupportedField and kOverBound; field names the same depth-first
    // index to_cdr reports.
    ConvertResult from_cdr(const std::string &type_name, bool narrow, const std::uint8_t *data,
                           std::size_t size, std::vector<std::uint8_t> &packed);

    // The dotted path of a depth-first field index ("linear.y"), or empty
    // when the type does not load or the index names no field.
    std::string field_path(const std::string &type_name, std::uint8_t index);

    // Registers a plan from caller-owned members, for tests of types no
    // package installs. The members must outlive the converter. True when the
    // layout has a wire form; false names the reason through fail.
    bool install_plan_for_test(const std::string &type_name, const rosidl_message_type_support_t *intro_ts);

    // The number of cached plans, for tests and diagnostics.
    std::size_t cached_plan_count() const;

  private:
    struct Plan;
    std::shared_ptr<Plan> get_plan(const std::string &type_name, ConvertFail &why);

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Plan>> plans_;
};

} // namespace r2s_bridge

#endif // R2S_BRIDGE__CONVERTER_HPP_
