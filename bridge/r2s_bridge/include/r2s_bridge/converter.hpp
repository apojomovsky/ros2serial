// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

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

/// Introspection type support, named without its header.
struct rosidl_message_type_support_t;

namespace r2s_bridge
{

/// Why a packed payload was dropped.
/**
 * Truncated and Trailing are [ENC-17] and [ENC-18]; BadVarint covers
 * [VAR-2], [VAR-3] and [VAR-4]; BadBool is [ENC-6] and NulInString is
 * [ENC-14]. UnknownType names a type the installed packages do not
 * provide, and UnsupportedField one whose layout has no wire form, so
 * the caller refuses the topic instead of dropping its frames. OverBound
 * is a bounded sequence longer than the host type holds.
 */
enum class ConvertFail : std::uint8_t
{
  kNone = 0,  ///< No failure; the conversion succeeded.
  kUnknownType,  ///< The installed packages do not provide the type.
  kUnsupportedField,  ///< A field layout has no wire form.
  kTruncated,  ///< The payload ended early [ENC-17].
  kTrailing,  ///< Bytes followed the message [ENC-18].
  kBadVarint,  ///< A varint breaks [VAR-2], [VAR-3] or [VAR-4].
  kBadBool,  ///< A bool is not 0 or 1 [ENC-6].
  kNulInString,  ///< A string holds a NUL [ENC-14].
  kOverBound,  ///< A sequence is longer than the host type holds.
};

/// The outcome of one conversion.
/**
 * On success fail is kNone and cdr holds the serialized message; otherwise
 * field is the depth-first index of the offending field definition, or
 * 0xFF when no field is named.
 */
struct ConvertResult
{
  ConvertFail fail = ConvertFail::kNone;  ///< Why the conversion failed.
  std::uint8_t field = 0xFFu;  ///< The depth-first index of the bad field.
};

/// Converts packed wire payloads to serialized CDR.
/**
 * Byte-identical to rclcpp serialization of the same message. Plans are
 * built once per type from the installed introspection type support and
 * cached; the narrow flag is a per-call input, never part of the plan
 * ([ENC-10], [CTL-5]). Not copyable; safe to share between threads.
 */
class Converter
{
public:
  /// Builds an empty converter.
  Converter();

  /// Destroys the converter and its cached plans.
  ~Converter();

  /// Not copyable.
  Converter(const Converter &) = delete;

  /// Not assignable.
  Converter & operator=(const Converter &) = delete;

  /// Converts one packed payload of type_name to serialized CDR in cdr.
  /**
   * Clears cdr first. narrow selects binary32 float64s on the wire.
   *
   * \param[in] type_name the message type to convert.
   * \param[in] narrow true when float64s travel as binary32.
   * \param[in] data the packed payload.
   * \param[in] size how many bytes data holds.
   * \param[out] cdr the serialized message.
   * \return The outcome, with the bad field index on failure.
   * \throws std::bad_alloc when a plan or buffer cannot grow.
   * \throws std::runtime_error when the middleware cannot serialize.
   */
  ConvertResult to_cdr(
    const std::string & type_name, bool narrow, const std::uint8_t *data,
    std::size_t size, std::vector<std::uint8_t> & cdr);

  /// Converts one packed payload of type_name into the caller's struct.
  /**
   * Into storage, which the caller initialized for the same type. The cached
   * plan is shared with to_cdr, and narrow is the same per-call input; no
   * middleware handle is needed, so hand-installed plans convert too.
   *
   * \param[in] type_name the message type to convert.
   * \param[in] narrow true when float64s travel as binary32.
   * \param[in] data the packed payload.
   * \param[in] size how many bytes data holds.
   * \param[out] storage the initialized introspection struct.
   * \return The outcome, with the bad field index on failure.
   */
  ConvertResult unpack_to_struct(
    const std::string & type_name, bool narrow, const std::uint8_t *data,
    std::size_t size, void *storage);

  /// Converts one caller-owned introspection struct of type_name to packed.
  /**
   * Into packed, clearing packed first. The cached plan is shared with
   * from_cdr, and narrow is the same per-call input; no middleware handle
   * is needed, so hand-installed plans convert too.
   *
   * \param[in] type_name the message type to convert.
   * \param[in] narrow true when float64s travel as binary32.
   * \param[in] storage the caller-owned introspection struct.
   * \param[out] packed the packed payload.
   * \return The outcome, with the bad field index on failure.
   */
  ConvertResult pack_from_struct(
    const std::string & type_name, bool narrow, const void *storage,
    std::vector<std::uint8_t> & packed);

    /// Converts one serialized CDR message of type_name to a packed payload.
    /**
     * Clears packed first. narrow packs float64s as binary32. The cached plan
     * is shared with to_cdr. A CDR the middleware cannot read back, and a
     * string or sequence longer than the wire varint holds, fail as
     * kUnsupportedField and kOverBound; field names the same depth-first
     * index to_cdr reports.
     *
     * \param[in] type_name the message type to convert.
     * \param[in] narrow true when float64s travel as binary32.
     * \param[in] data the serialized message.
     * \param[in] size how many bytes data holds.
     * \param[out] packed the packed payload.
     * \return The outcome, with the bad field index on failure.
     * \throws std::bad_alloc when a plan or buffer cannot grow.
     */
    ConvertResult from_cdr(
      const std::string & type_name, bool narrow, const std::uint8_t *data,
      std::size_t size, std::vector<std::uint8_t> & packed);

  /// The dotted path of a depth-first field index ("linear.y").
  /**
   * Empty when the type does not load, the index names no field, or the
   * index lies inside or after a nested-message sequence, whose offset
   * depends on the runtime count. Callers fall back to the numeric index
   * then.
   *
   * \param[in] type_name the message type to name through.
   * \param[in] index the depth-first field index.
   * \return The dotted path, or empty when none names the field.
   */
  std::string field_path(const std::string & type_name, std::uint8_t index);

  /// Registers a plan from caller-owned members.
  /**
   * For tests of types no package installs. The members must outlive the
   * converter. True when the layout has a wire form; false names the
   * reason through fail.
   *
   * \param[in] type_name the message type to register.
   * \param[in] intro_ts the members to build the plan from.
   * \return True when the layout has a wire form.
   */
  bool install_plan_for_test(
    const std::string & type_name,
    const rosidl_message_type_support_t *intro_ts);

  /// The number of cached plans, for tests and diagnostics.
  /**
   * \return How many plans the converter holds.
   */
  std::size_t cached_plan_count() const;

private:
  struct Plan;
  std::shared_ptr<Plan> get_plan(const std::string & type_name, ConvertFail & why);

  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<Plan>> plans_;
};

}  // namespace r2s_bridge

#endif  // R2S_BRIDGE__CONVERTER_HPP_
