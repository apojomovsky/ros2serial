"""The MCU time arithmetic of spec/protocol.md section 7.2, in 32-bit integers alone."""

from r2s_gen.reference.errors import Rejected

MAX_ANCHOR_MS = 10000
MAX_DRIFT_PPM = 20000


def compute(tick_hz, case):
    """The ROS time of `tick` under the correction `case` holds, or None while unsynced.

    `case` carries `tick0`, `sec0`, `nanosec0`, `drift_ppm` and `tick`. Input the
    spec says to refuse raises Rejected naming the rule that refuses it.
    """
    tick0, sec0, nanosec0, drift, tick = (
        case["tick0"],
        case["sec0"],
        case["nanosec0"],
        case["drift_ppm"],
        case["tick"],
    )
    if tick_hz < 1000 or tick_hz % 1000 or 1000000 % (tick_hz // 1000):
        raise Rejected("CTL-9", f"tick_hz {tick_hz} is not 1000 times a divisor of 1000000")
    if abs(drift) > MAX_DRIFT_PPM:
        raise Rejected("TIM-5", f"drift {drift} ppm is outside +-20000")
    if not 0 <= nanosec0 < 1000000000:
        raise Rejected("TIM-11", f"nanosec {nanosec0} is not below 10^9")
    k = tick_hz // 1000
    e = (tick - tick0) % 2**32
    ms, r = divmod(e, k)
    if ms > MAX_ANCHOR_MS:
        return None
    sec = sec0 + ms // 1000
    # The inputs above bound this below 2 * 10^9, and the drift term below
    # 2 * 10^8 in magnitude, which is what [TIM-8] requires of each side.
    ns = nanosec0 + (ms % 1000) * 1000000 + r * (1000000 // k)
    carry, ns = divmod(ns, 1000000000)
    carry_drift, ns = divmod(ns + ms * drift, 1000000000)
    return (sec + carry + carry_drift, ns)
