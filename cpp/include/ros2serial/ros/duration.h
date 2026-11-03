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

/* Moves whole seconds between the fields until nsec fits in one second. */
inline void normalizeSecNSecSigned(int32_t &sec, int32_t &nsec)
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

class Duration
{
  public:
    int32_t sec, nsec;

    Duration() : sec(0), nsec(0)
    {
    }

    /* Keeps the span normalized: nsec inside one second either way. */
    Duration(int32_t _sec, int32_t _nsec) : sec(_sec), nsec(_nsec)
    {
        normalizeSecNSecSigned(sec, nsec);
    }

    /* The span in seconds, as a double. */
    double toSec() const
    {
        return (double)sec + 1e-9 * (double)nsec;
    }

    /* Splits seconds across the fields, rounding the rest to nanoseconds. */
    void fromSec(double t)
    {
        sec = (int32_t)floor(t);
        nsec = (int32_t)round((t - (double)sec) * 1e9);
    }

    Duration &operator+=(const Duration &rhs)
    {
        sec += rhs.sec;
        nsec += rhs.nsec;
        normalizeSecNSecSigned(sec, nsec);
        return *this;
    }

    Duration &operator-=(const Duration &rhs)
    {
        sec += -rhs.sec;
        nsec += -rhs.nsec;
        normalizeSecNSecSigned(sec, nsec);
        return *this;
    }

    /* Scales both fields, truncating toward zero as ros_lib does. */
    Duration &operator*=(double scale)
    {
        sec = (int32_t)((double)sec * scale);
        nsec = (int32_t)((double)nsec * scale);
        normalizeSecNSecSigned(sec, nsec);
        return *this;
    }
};

} // namespace ros

#endif
