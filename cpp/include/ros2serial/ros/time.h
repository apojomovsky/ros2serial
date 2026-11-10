/* rosserial-compatible Time: seconds and nanoseconds since the epoch of the
 * shared clock. The arithmetic copies ros_lib, so ported sketches compute
 * the same values. Header-only, with the normalize step inline, since the
 * layer has no source of its own. */
#ifndef R2S_ROS_TIME_H
#define R2S_ROS_TIME_H

#include <math.h>
#include <stdint.h>

#include "ros2serial/ros/duration.h"

namespace ros
{

/// Folds whole seconds out of nsec into sec.
/**
 * \param[in,out] sec the seconds to add the fold into.
 * \param[in,out] nsec the nanoseconds to fold.
 */
inline void normalizeSecNSec(uint32_t & sec, uint32_t & nsec)
{
  uint32_t nsec_part = nsec % 1000000000u;
  uint32_t sec_part = nsec / 1000000000u;

  sec += sec_part;
  nsec = nsec_part;
}

/// A stamp of seconds and nanoseconds on the shared clock.
/**
 * The arithmetic copies ros_lib, so ported sketches compute the same
 * values.
 */
class Time
{
public:
  uint32_t sec, nsec;

  /// Zeroes the stamp.
  Time()
  : sec(0), nsec(0)
  {
  }

  /// Keeps the stamp normalized: nsec below one second.
  /**
   * \param[in] _sec the seconds to start from.
   * \param[in] _nsec the nanoseconds to start from.
   */
  Time(uint32_t _sec, uint32_t _nsec)
  : sec(_sec), nsec(_nsec)
  {
    normalizeSecNSec(sec, nsec);
  }

  /// The stamp in seconds, as a double.
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
   * \param[in] t the stamp in seconds.
   */
  void fromSec(double t)
  {
    sec = (uint32_t)floor(t);
    nsec = (uint32_t)round((t - (double)sec) * 1e9);
  }

  /// The stamp in nanoseconds, wrapping past about four seconds.
  /**
   * \return Seconds and nanoseconds as one count.
   */
  uint32_t toNsec()
  {
    return sec * 1000000000u + nsec;
  }

  /// Splits a nanosecond count across the fields.
  /**
   * \param[in] t the stamp in nanoseconds.
   * \return This stamp, normalized.
   */
  Time & fromNSec(int32_t t)
  {
    sec = (uint32_t)(t / 1000000000);
    nsec = (uint32_t)(t % 1000000000);
    normalizeSecNSec(sec, nsec);
    return *this;
  }

  /// Moves the stamp by a span.
  /**
   * Borrows across the fields as ros_lib does, so the unsigned
   * arithmetic never underflows.
   *
   * \param[in] rhs the span to move by.
   * \return This stamp, normalized.
   */
  Time & operator+=(const Duration & rhs)
  {
    sec = sec - 1u + (uint32_t)rhs.sec;
    nsec = nsec + 1000000000u + (uint32_t)rhs.nsec;
    normalizeSecNSec(sec, nsec);
    return *this;
  }

  /// Moves the stamp back by a span.
  /**
   * Borrows across the fields as ros_lib does, so the unsigned
   * arithmetic never underflows.
   *
   * \param[in] rhs the span to move back by.
   * \return This stamp, normalized.
   */
  Time & operator-=(const Duration & rhs)
  {
    sec = sec - 1u - (uint32_t)rhs.sec;
    nsec = nsec + 1000000000u - (uint32_t)rhs.nsec;
    normalizeSecNSec(sec, nsec);
    return *this;
  }

  /// The signed span between two stamps.
  /**
   * \param[in] rhs the stamp to subtract.
   * \return The span from rhs to this stamp.
   */
  Duration operator-(const Time & rhs) const
  {
    Duration d;

    d.sec = (int32_t)(sec > rhs.sec ? sec - rhs.sec : -(rhs.sec - sec));
    d.nsec = (int32_t)(nsec > rhs.nsec ? nsec - rhs.nsec : -(rhs.nsec - nsec));
    normalizeSecNSecSigned(d.sec, d.nsec);
    return d;
  }
};

} // namespace ros

#endif
