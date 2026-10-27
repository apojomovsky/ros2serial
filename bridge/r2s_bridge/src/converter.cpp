// Packed wire payloads to serialized CDR, through introspection with the plan
// per type cached.
#include "r2s_bridge/converter.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "rclcpp/typesupport_helpers.hpp"
#include "rcpputils/shared_library.hpp"
#include "rcutils/allocator.h"
#include "rmw/rmw.h"
#include "rmw/serialized_message.h"
#include "rosidl_runtime_cpp/message_initialization.hpp"
#include "rosidl_typesupport_introspection_cpp/field_types.hpp"
#include "rosidl_typesupport_introspection_cpp/message_introspection.hpp"

namespace r2s_bridge
{

using rosidl_runtime_cpp::MessageInitialization;
using rosidl_typesupport_introspection_cpp::MessageMember;
using rosidl_typesupport_introspection_cpp::MessageMembers;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_BOOLEAN;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_DOUBLE;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_FLOAT;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_INT16;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_INT32;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_INT64;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_INT8;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_MESSAGE;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_OCTET;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_STRING;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT16;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT32;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT64;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT8;

// The plan walk mirrors the MCU codec's visited counter (core/src/codec.c):
// every field definition takes one index in depth-first order, array elements
// share their field's index, and a nested array revisits its children per
// element, so indices past one depend on its runtime count.
constexpr std::size_t kMaxPlanDepth = 32;
constexpr std::size_t kMaxPlanFields = 255;
constexpr std::size_t kUnbounded = std::numeric_limits<std::size_t>::max();
constexpr const char *kIntrospectionId = "rosidl_typesupport_introspection_cpp";
constexpr const char *kCppId = "rosidl_typesupport_cpp";

struct Op {
    std::uint8_t type = 0;
    std::uint32_t offset = 0;
    bool is_array = false;
    bool is_sequence = false;
    std::size_t bound = 0;
    std::string path;
    void *(*get)(void *, std::size_t) = nullptr;
    void (*assign)(void *, std::size_t, const void *) = nullptr;
    void (*resize)(void *, std::size_t) = nullptr;
    std::vector<Op> nested;
};

struct Converter::Plan {
    std::shared_ptr<rcpputils::SharedLibrary> intro_lib;
    std::shared_ptr<rcpputils::SharedLibrary> cpp_lib;
    const rosidl_message_type_support_t *intro_ts = nullptr;
    const rosidl_message_type_support_t *cpp_ts = nullptr;
    const MessageMembers *members = nullptr;
    std::vector<Op> fields;
};

namespace
{

struct Reader {
    const std::uint8_t *at = nullptr;
    const std::uint8_t *end = nullptr;
};

bool take_byte(Reader &reader, std::uint8_t &byte)
{
    if (reader.at == reader.end) {
        return false;
    }
    byte = *reader.at++;
    return true;
}

bool take_u16(Reader &reader, std::uint16_t &value)
{
    if (static_cast<std::size_t>(reader.end - reader.at) < 2) {
        return false;
    }
    value = static_cast<std::uint16_t>(static_cast<std::uint16_t>(reader.at[0]) |
                                       (static_cast<std::uint16_t>(reader.at[1]) << 8));
    reader.at += 2;
    return true;
}

bool take_u32(Reader &reader, std::uint32_t &value)
{
    if (static_cast<std::size_t>(reader.end - reader.at) < 4) {
        return false;
    }
    value = static_cast<std::uint32_t>(reader.at[0]) | (static_cast<std::uint32_t>(reader.at[1]) << 8) |
            (static_cast<std::uint32_t>(reader.at[2]) << 16) |
            (static_cast<std::uint32_t>(reader.at[3]) << 24);
    reader.at += 4;
    return true;
}

bool take_u64(Reader &reader, std::uint64_t &value)
{
    if (static_cast<std::size_t>(reader.end - reader.at) < 8) {
        return false;
    }
    value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(reader.at[i]) << (8 * i);
    }
    reader.at += 8;
    return true;
}

enum class Varint : std::uint8_t {
    kOk,
    kTruncated,
    kMalformed,
};

// Reads one [VAR-1] length. Malformed covers [VAR-2] and [VAR-4]; truncated is
// [VAR-3], which wins when the payload ends first.
Varint take_varint(Reader &reader, std::uint32_t &value)
{
    value = 0;
    for (int shift = 0; shift < 21; shift += 7) {
        std::uint8_t byte = 0;
        if (!take_byte(reader, byte)) {
            return Varint::kTruncated;
        }
        value |= static_cast<std::uint32_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0u) {
            if (shift > 0 && value < (1u << shift)) {
                return Varint::kMalformed;
            }
            if (value > 0xFFFFu) {
                return Varint::kMalformed;
            }
            return Varint::kOk;
        }
    }
    return Varint::kMalformed;
}

// Fills one scalar from the wire. Every multi-byte integer is assembled
// little-endian into an exactly-sized value, so the host order never matters.
// ROS_TYPE_CHAR is refused with the rest below: the installed generator maps
// IDL char to UINT8, so no installed type carries it.
ConvertFail decode_scalar(std::uint8_t type, bool narrow, Reader &reader, std::uint8_t *dst)
{
    switch (type) {
    case ROS_TYPE_BOOLEAN: {
        std::uint8_t byte = 0;
        if (!take_byte(reader, byte)) {
            return ConvertFail::kTruncated;
        }
        if (byte > 1u) {
            return ConvertFail::kBadBool;
        }
        *dst = byte;
        return ConvertFail::kNone;
    }
    case ROS_TYPE_OCTET:
    case ROS_TYPE_UINT8:
    case ROS_TYPE_INT8: {
        std::uint8_t byte = 0;
        if (!take_byte(reader, byte)) {
            return ConvertFail::kTruncated;
        }
        *dst = byte;
        return ConvertFail::kNone;
    }
    case ROS_TYPE_UINT16:
    case ROS_TYPE_INT16: {
        std::uint16_t value = 0;
        if (!take_u16(reader, value)) {
            return ConvertFail::kTruncated;
        }
        std::memcpy(dst, &value, sizeof value);
        return ConvertFail::kNone;
    }
    case ROS_TYPE_UINT32:
    case ROS_TYPE_INT32: {
        std::uint32_t value = 0;
        if (!take_u32(reader, value)) {
            return ConvertFail::kTruncated;
        }
        std::memcpy(dst, &value, sizeof value);
        return ConvertFail::kNone;
    }
    case ROS_TYPE_UINT64:
    case ROS_TYPE_INT64: {
        std::uint64_t value = 0;
        if (!take_u64(reader, value)) {
            return ConvertFail::kTruncated;
        }
        std::memcpy(dst, &value, sizeof value);
        return ConvertFail::kNone;
    }
    case ROS_TYPE_FLOAT: {
        std::uint32_t raw = 0;
        if (!take_u32(reader, raw)) {
            return ConvertFail::kTruncated;
        }
        float value = 0;
        std::memcpy(&value, &raw, sizeof value);
        std::memcpy(dst, &value, sizeof value);
        return ConvertFail::kNone;
    }
    case ROS_TYPE_DOUBLE: {
        if (narrow) {
            std::uint32_t raw = 0;
            if (!take_u32(reader, raw)) {
                return ConvertFail::kTruncated;
            }
            float narrow_value = 0;
            std::memcpy(&narrow_value, &raw, sizeof narrow_value);
            const double value = narrow_value;
            std::memcpy(dst, &value, sizeof value);
        } else {
            std::uint64_t raw = 0;
            if (!take_u64(reader, raw)) {
                return ConvertFail::kTruncated;
            }
            double value = 0;
            std::memcpy(&value, &raw, sizeof value);
            std::memcpy(dst, &value, sizeof value);
        }
        return ConvertFail::kNone;
    }
    case ROS_TYPE_STRING: {
        std::uint32_t length = 0;
        switch (take_varint(reader, length)) {
        case Varint::kOk:
            break;
        case Varint::kTruncated:
            return ConvertFail::kTruncated;
        case Varint::kMalformed:
            return ConvertFail::kBadVarint;
        }
        if (static_cast<std::size_t>(reader.end - reader.at) < length) {
            return ConvertFail::kTruncated;
        }
        if (std::memchr(reader.at, 0, length) != nullptr) {
            return ConvertFail::kNulInString;
        }
        *reinterpret_cast<std::string *>(dst) =
            std::string(reinterpret_cast<const char *>(reader.at), length);
        reader.at += length;
        return ConvertFail::kNone;
    }
    }
    return ConvertFail::kUnsupportedField;
}

ConvertFail decode_ops(const std::vector<Op> &ops, std::uint8_t *base, Reader &reader, bool narrow,
                       std::uint32_t &counter, std::uint8_t &field);

ConvertFail decode_element(const Op &op, std::uint8_t *base, std::uint32_t i, Reader &reader, bool narrow,
                           std::uint32_t here, std::uint32_t &counter, std::uint8_t &field)
{
    if (op.type == ROS_TYPE_MESSAGE) {
        // Only addressable elements reach here; a null getter means boolean
        // scalars, handled below.
        return decode_ops(op.nested, static_cast<std::uint8_t *>(op.get(base, i)), reader, narrow, counter,
                          field);
    }
    if (op.get != nullptr) {
        const ConvertFail fail =
            decode_scalar(op.type, narrow, reader, static_cast<std::uint8_t *>(op.get(base, i)));
        if (fail != ConvertFail::kNone) {
            field = static_cast<std::uint8_t>(here);
        }
        return fail;
    }
    bool value = false;
    const ConvertFail fail = decode_scalar(op.type, narrow, reader, reinterpret_cast<std::uint8_t *>(&value));
    if (fail != ConvertFail::kNone) {
        field = static_cast<std::uint8_t>(here);
        return fail;
    }
    op.assign(base, i, &value);
    return ConvertFail::kNone;
}

ConvertFail decode_ops(const std::vector<Op> &ops, std::uint8_t *base, Reader &reader, bool narrow,
                       std::uint32_t &counter, std::uint8_t &field)
{
    for (const Op &op : ops) {
        const std::uint32_t here = counter++;
        if (op.is_array) {
            std::uint32_t count = op.bound;
            if (op.is_sequence) {
                switch (take_varint(reader, count)) {
                case Varint::kOk:
                    break;
                case Varint::kTruncated:
                    field = static_cast<std::uint8_t>(here);
                    return ConvertFail::kTruncated;
                case Varint::kMalformed:
                    field = static_cast<std::uint8_t>(here);
                    return ConvertFail::kBadVarint;
                }
                if (count > op.bound) {
                    field = static_cast<std::uint8_t>(here);
                    return ConvertFail::kOverBound;
                }
                op.resize(base + op.offset, count);
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                const ConvertFail fail =
                    decode_element(op, base + op.offset, i, reader, narrow, here, counter, field);
                if (fail != ConvertFail::kNone) {
                    return fail;
                }
            }
        } else if (op.type == ROS_TYPE_MESSAGE) {
            const ConvertFail fail = decode_ops(op.nested, base + op.offset, reader, narrow, counter, field);
            if (fail != ConvertFail::kNone) {
                return fail;
            }
        } else {
            const ConvertFail fail = decode_scalar(op.type, narrow, reader, base + op.offset);
            if (fail != ConvertFail::kNone) {
                field = static_cast<std::uint8_t>(here);
                return fail;
            }
        }
    }
    return ConvertFail::kNone;
}

// The dotted path of a depth-first index. A fixed nested array holds exactly
// its bound elements, so the walk passes through it to the siblings after
// it. A sequence holds a runtime count the index alone cannot name: indices
// past one map into its elements in order, which is exact while every outer
// element carries the same count and otherwise names the first layout that
// covers the index.
bool path_of(const std::vector<Op> &ops, std::uint32_t &counter, std::uint8_t want, std::string &out)
{
    for (const Op &op : ops) {
        const std::uint32_t here = counter++;
        if (here == want) {
            out = op.path;
            return true;
        }
        if (op.type != ROS_TYPE_MESSAGE) {
            continue;
        }
        if (!op.is_array) {
            if (path_of(op.nested, counter, want, out)) {
                return true;
            }
        } else if (!op.nested.empty()) {
            if (!op.is_sequence) {
                for (std::size_t k = 0; k < op.bound; ++k) {
                    if (path_of(op.nested, counter, want, out)) {
                        return true;
                    }
                }
            } else {
                while (counter <= want) {
                    if (path_of(op.nested, counter, want, out)) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool build_ops(const MessageMembers *members, const std::string &prefix, std::vector<Op> &ops,
               std::size_t depth, std::size_t &count)
{
    // Storage is constructed and destroyed through the top type's functions;
    // a plan without them cannot convert. Nested types never construct.
    if (members == nullptr || depth > kMaxPlanDepth) {
        return false;
    }
    if (depth == 0 && (members->init_function == nullptr || members->fini_function == nullptr)) {
        return false;
    }
    for (std::uint32_t i = 0; i < members->member_count_; ++i) {
        if (count >= kMaxPlanFields) {
            return false;
        }
        ++count;
        const MessageMember &member = members->members_[i];
        Op op;
        op.type = member.type_id_;
        op.offset = member.offset_;
        op.path = prefix.empty() ? member.name_ : prefix + "." + member.name_;
        if (member.type_id_ == ROS_TYPE_MESSAGE) {
            const auto *nested_ts = static_cast<const rosidl_message_type_support_t *>(member.members_);
            if (nested_ts == nullptr || nested_ts->data == nullptr) {
                return false;
            }
            if (!build_ops(static_cast<const MessageMembers *>(nested_ts->data), op.path, op.nested,
                           depth + 1, count)) {
                return false;
            }
        }
        if (member.is_array_) {
            // A null getter means a boolean sequence, whose std::vector<bool>
            // has no addressable elements; its assign callback fills elements
            // through a temporary instead. Anything else without a getter
            // cannot be read back, so refusing beats mis-converting.
            if (member.get_function == nullptr) {
                if (member.type_id_ != ROS_TYPE_BOOLEAN || member.assign_function == nullptr) {
                    return false;
                }
                op.assign = member.assign_function;
            } else {
                op.get = member.get_function;
            }
            op.is_array = true;
            if (member.resize_function != nullptr) {
                op.is_sequence = true;
                op.resize = member.resize_function;
                op.bound = member.is_upper_bound_ ? member.array_size_ : kUnbounded;
            } else {
                op.bound = member.array_size_;
            }
        }
        ops.push_back(std::move(op));
    }
    return true;
}

struct MessageGuard {
    const MessageMembers *members;
    std::uint8_t *data;
    ~MessageGuard()
    {
        members->fini_function(data);
    }
};

struct SerializedGuard {
    rmw_serialized_message_t msg = rmw_get_zero_initialized_serialized_message();
    bool owned = false;
    ~SerializedGuard()
    {
        if (owned) {
            rmw_serialized_message_fini(&msg);
        }
    }
};

// The middleware leaves CDR alignment padding unwritten, so the padding holds
// whatever was in the buffer. Zeroing an oversized buffer first makes the
// bytes deterministic. When the message outgrows the initial buffer, rmw
// reallocates, and the fresh bytes hold garbage again, so the buffer is
// zeroed and serialized a second time; the grown capacity suffices then.
constexpr std::size_t kSerializeCapacity = 4096;

void serialize_message(const rosidl_message_type_support_t *cpp_ts, const std::uint8_t *storage,
                       std::vector<std::uint8_t> &cdr)
{
    SerializedGuard guard;
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    // GCOVR_EXCL_START: the middleware fails only when it cannot allocate,
    // which a unit test cannot inject.
    if (rmw_serialized_message_init(&guard.msg, kSerializeCapacity, &allocator) != RMW_RET_OK) {
        throw std::runtime_error("r2s_bridge::Converter: cannot initialize serialized message");
    }
    // GCOVR_EXCL_STOP
    guard.owned = true;
    const auto run = [&]() { return rmw_serialize(storage, cpp_ts, &guard.msg); };
    std::memset(guard.msg.buffer, 0, guard.msg.buffer_capacity);
    // GCOVR_EXCL_START: rmw fails only when it cannot grow the buffer.
    if (run() != RMW_RET_OK) {
        throw std::runtime_error("r2s_bridge::Converter: rmw_serialize failed");
    }
    // GCOVR_EXCL_STOP
    if (guard.msg.buffer_capacity > kSerializeCapacity) {
        std::memset(guard.msg.buffer, 0, guard.msg.buffer_capacity);
        // GCOVR_EXCL_START: the second run cannot grow the buffer again.
        if (run() != RMW_RET_OK) {
            throw std::runtime_error("r2s_bridge::Converter: rmw_serialize failed");
        }
        // GCOVR_EXCL_STOP
    }
    cdr.assign(guard.msg.buffer, guard.msg.buffer + guard.msg.buffer_length);
}

} // namespace

Converter::Converter() = default;
Converter::~Converter() = default;

std::shared_ptr<Converter::Plan> Converter::get_plan(const std::string &type_name, ConvertFail &why)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = plans_.find(type_name);
    if (it != plans_.end()) {
        return it->second;
    }
    auto plan = std::make_shared<Plan>();
    try {
        // The libraries stay loaded inside the cached plan, which is what the
        // handles point into; type_support.hpp loads the same way and closes
        // its library again because it keeps no plan.
        plan->intro_lib = rclcpp::get_typesupport_library(type_name, kIntrospectionId);
        plan->intro_ts =
            rclcpp::get_message_typesupport_handle(type_name, kIntrospectionId, *plan->intro_lib);
        plan->cpp_lib = rclcpp::get_typesupport_library(type_name, kCppId);
        plan->cpp_ts = rclcpp::get_message_typesupport_handle(type_name, kCppId, *plan->cpp_lib);
    } catch (const std::exception &) {
        // A malformed name, a missing package and an unloadable library all
        // refuse the topic the same way ([CTL-13]).
        why = ConvertFail::kUnknownType;
        return nullptr;
    }
    plan->members = static_cast<const MessageMembers *>(plan->intro_ts->data);
    // GCOVR_EXCL_START: installed generators reject unbuildable layouts, so
    // no installed type fails here; hand-built failures go through
    // install_plan_for_test, and unknown names through the load above.
    std::size_t count = 0;
    if (!build_ops(plan->members, "", plan->fields, 0, count)) {
        why = ConvertFail::kUnsupportedField;
        return nullptr;
    }
    // GCOVR_EXCL_STOP
    plans_[type_name] = plan;
    return plan;
}

ConvertResult Converter::to_cdr(const std::string &type_name, bool narrow, const std::uint8_t *data,
                                std::size_t size, std::vector<std::uint8_t> &cdr)
{
    cdr.clear();
    ConvertFail why = ConvertFail::kNone;
    const std::shared_ptr<Plan> plan = get_plan(type_name, why);
    if (plan == nullptr) {
        return ConvertResult{why, 0xFFu};
    }
    std::vector<std::uint8_t> storage(plan->members->size_of_);
    plan->members->init_function(storage.data(), MessageInitialization::ALL);
    const MessageGuard guard{plan->members, storage.data()};
    Reader reader{data, data + size};
    std::uint32_t counter = 0;
    std::uint8_t field = 0xFFu;
    const ConvertFail fail = decode_ops(plan->fields, storage.data(), reader, narrow, counter, field);
    if (fail != ConvertFail::kNone) {
        return ConvertResult{fail, field};
    }
    if (reader.at != reader.end) {
        return ConvertResult{ConvertFail::kTrailing, 0xFFu};
    }
    if (plan->cpp_ts == nullptr) {
        // Hand-installed plans carry no middleware handle: they decode far
        // enough to report errors, never to CDR.
        return ConvertResult{ConvertFail::kUnsupportedField, 0xFFu};
    }
    serialize_message(plan->cpp_ts, storage.data(), cdr);
    return ConvertResult{};
}

std::string Converter::field_path(const std::string &type_name, std::uint8_t index)
{
    ConvertFail why = ConvertFail::kNone;
    const std::shared_ptr<Plan> plan = get_plan(type_name, why);
    if (plan == nullptr) {
        return "";
    }
    std::uint32_t counter = 0;
    std::string out;
    if (path_of(plan->fields, counter, index, out)) {
        return out;
    }
    return "";
}

bool Converter::install_plan_for_test(const std::string &type_name,
                                      const rosidl_message_type_support_t *intro_ts)
{
    if (intro_ts == nullptr || intro_ts->data == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto plan = std::make_shared<Plan>();
    plan->intro_ts = intro_ts;
    plan->members = static_cast<const MessageMembers *>(intro_ts->data);
    std::size_t count = 0;
    if (!build_ops(plan->members, "", plan->fields, 0, count)) {
        return false;
    }
    plans_[type_name] = plan;
    return true;
}

std::size_t Converter::cached_plan_count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return plans_.size();
}

} // namespace r2s_bridge
