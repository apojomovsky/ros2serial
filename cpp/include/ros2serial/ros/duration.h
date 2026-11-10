/* rosserial-compatible Duration: a signed span of seconds and nanoseconds.
 * The arithmetic copies ros_lib, so ported sketches compute the same values.
 * Header-only, with the normalize step inline, since the layer has no source
 * of its own. */
#ifndef R2S_ROS_DURATION_H
#define R2S_ROS_DURATION_H

#include <math.h>
#include <stdint.h>

namespace ros
{

/// Moves whole seconds between the fields until nsec fits in one second.
/**
 * \param[in,out] sec the seconds to normalize.
 * \param[in,out] nsec the nanoseconds to fold into sec.
 */
inline void normalizeSecNSecSigned(int32_t & sec, int32_t & nsec)
{
  int32_t nsec_part = nsec;
  int32_t sec_part = sec;

  while (nsec_part > 1000000000) {
    nsec_part -= 1000000000;
    ++sec_part;
  }
  while (nsec_part < 0) {
    nsec_part += 1000000000;
    --sec_part;
  }
  sec = sec_part;
  nsec = nsec_part;
}

/// A signed span of seconds and nanoseconds.
/**
 * The arithmetic copies ros_lib, so ported sketches compute the same
 * values.
 */
class Duration
{
public:
  int32_t sec, nsec;

  /// Zeroes the span.
  Duration()
  : sec(0), nsec(0)
  {
  }

  /// Keeps the span normalized: nsec inside one second either way.
  /**
   * \param[in] _sec the seconds to start from.
   * \param[in] _nsec the nanoseconds to start from.
   */
  Duration(int32_t _sec, int32_t _nsec)
  : sec(_sec), nsec(_nsec)
  {
    normalizeSecNSecSigned(sec, nsec);
  }

  /// The span in seconds, as a double.
  /**
   * \return Seconds plus nanoseconds as a fraction.
   */
  double toSec() const
  {
    return (double)sec + 1e-9 * (double)nsec;
  }

  /// Splits seconds across the fields.
  /**
   * Rounds the rest to nanoseconds.
   *
   * \param[in] t the span in seconds.
   */
  void fromSec(double t)
  {
    sec = (int32_t)floor(t);
    nsec = (int32_t)round((t - (double)sec) * 1e9);
  }

  /// Adds a span to this one.
  /**
   * \param[in] rhs the span to add.
   * \return This span, normalized.
   */
  Duration & operator+=(const Duration & rhs)
  {
    sec += rhs.sec;
    nsec += rhs.nsec;
    normalizeSecNSecSigned(sec, nsec);
    return *this;
  }

  /// Subtracts a span from this one.
  /**
   * \param[in] rhs the span to subtract.
   * \return This span, normalized.
   */
  Duration & operator-=(const Duration & rhs)
  {
    sec += -rhs.sec;
    nsec += -rhs.nsec;
    normalizeSecNSecSigned(sec, nsec);
    return *this;
  }

  /// Scales both fields, truncating toward zero as ros_lib does.
  /**
   * \param[in] scale the factor to scale by.
   * \return This span, normalized.
   */
  Duration & operator*=(double scale)
  {
    sec = (int32_t)((double)sec * scale);
    nsec = (int32_t)((double)nsec * scale);
    normalizeSecNSecSigned(sec, nsec);
    return *this;
  }
};

} // namespace ros

#endif
