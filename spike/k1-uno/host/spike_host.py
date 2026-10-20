#!/usr/bin/env python3
"""K1 spike host: handshake with the spike firmware and measure the link.

No ROS involved. Reports battery-frame loss, Twist echo correctness, round
trip times, MCU clock drift against the host clock, the MCU's own counters,
and any reset it detects. Throwaway, like the firmware.
"""

import argparse
import json
import statistics
import struct
import sys
import time

import serial


def crc16_mcrf4xx(data, crc=0xFFFF):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc


def rcobs_encode(data):
    out, n = bytearray(), 1
    for b in data:
        if b == 0:
            out.append(n)
            n = 1
        else:
            out.append(b)
            n += 1
            if n == 0xFF:
                out.append(0xFF)
                n = 1
    out.append(n)
    return bytes(out)


def rcobs_decode(enc):
    buf = bytearray(enc)
    i = w = len(buf)
    last = True
    while i > 0:
        c = buf[i - 1]
        if c == 0 or c > i:
            raise ValueError("bad rCOBS code")
        if not last and c != 0xFF:
            w -= 1
            buf[w] = 0
        for j in range(i - 2, i - c - 1, -1):
            w -= 1
            buf[w] = buf[j]
        i -= c
        last = False
    return bytes(buf[w:])


def read_str(p, off):
    n = p[off]
    return p[off + 1:off + 1 + n].decode(), off + 1 + n


class Link:
    def __init__(self, port, baud):
        self.ser = serial.Serial(port, baud, timeout=0)
        self.rx = bytearray()
        self.seq = {}
        self.stats = {"crc_errors": 0, "bad_frames": 0}

    def send(self, topic, payload):
        s = self.seq.get(topic, 0)
        self.seq[topic] = (s + 1) & 0xFF
        body = bytes([topic, s]) + payload
        body += struct.pack("<H", crc16_mcrf4xx(body))
        self.ser.write(rcobs_encode(body) + b"\0")

    def frames(self):
        """Yield (arrival time, topic, seq, payload) for every complete frame."""
        data = self.ser.read(4096)
        now = time.monotonic()
        self.rx += data
        while b"\0" in self.rx:
            enc, _, rest = self.rx.partition(b"\0")
            self.rx = bytearray(rest)
            if not enc:
                continue
            try:
                f = rcobs_decode(enc)
            except ValueError:
                self.stats["bad_frames"] += 1
                continue
            if len(f) < 4:
                self.stats["bad_frames"] += 1
            elif crc16_mcrf4xx(f) != 0:
                self.stats["crc_errors"] += 1
            else:
                yield now, f[0], f[1], f[2:-2]


def parse_hello(p):
    ver, mode, boot_id, max_frame, rx_cap, tick_hz, feat, ntopics = struct.unpack_from("<BBIHHIBB", p, 1)
    name, _ = read_str(p, 1 + 16)
    return {"version": ver, "mode": mode, "boot_id": boot_id, "max_frame": max_frame,
            "rx_capacity": rx_cap, "tick_hz": tick_hz, "features": feat,
            "topic_count": ntopics, "node": name}


def handshake(link, boot_wait, hello=None):
    deadline = time.monotonic() + boot_wait
    last_req = 0.0
    while hello is None:
        now = time.monotonic()
        if now >= deadline and now - last_req > 0.5:
            link.send(0, bytes([1, 1]))  # HELLO_REQ, protocol 1
            last_req = now
        for _, topic, _, p in link.frames():
            if topic == 0 and p and p[0] == 2:
                hello = parse_hello(p)
        time.sleep(0.005)
    topics = []
    for idx in range(hello["topic_count"]):
        entry = None
        for _attempt in range(3):
            link.send(0, bytes([3, idx]))
            t_end = time.monotonic() + 0.2
            while entry is None and time.monotonic() < t_end:
                for _, topic, _, p in link.frames():
                    if topic == 0 and p[0] == 4 and p[1] == idx:
                        tid, flags = p[2], p[3]
                        ty, off = read_str(p, 12)
                        name, _ = read_str(p, off)
                        entry = {"id": tid, "flags": flags, "type": ty, "name": name}
                time.sleep(0.002)
            if entry:
                break
        if entry is None:
            sys.exit(f"TOPIC {idx} never arrived")
        topics.append(entry)
        link.send(0, bytes([5, entry["id"], 0]))  # TOPIC_STATUS ok
    link.send(0, bytes([6]))  # READY
    link.seq = {}
    return hello, topics


def drift_ppm(samples, tick_hz):
    """Slope of MCU ticks against host time, from the lowest-RTT half of the samples."""
    if len(samples) < 6:
        return None
    best = sorted(samples, key=lambda s: s[2])[: max(6, len(samples) // 2)]
    xs, ys = [], []
    wraps, prev = 0, None
    for host_t, tick, _rtt in sorted(best):
        if prev is not None and tick < prev:
            wraps += 1
        prev = tick
        xs.append(host_t)
        ys.append(tick + wraps * 2**32)
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    return (slope / tick_hz - 1.0) * 1e6


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--twist-hz", type=float, default=20)
    ap.add_argument("--boot-wait", type=float, default=2.5, help="wait for an unsolicited HELLO first (Uno resets on open)")
    ap.add_argument("--json", action="store_true", help="print the summary as one JSON line")
    args = ap.parse_args()

    link = Link(args.port, args.baud)
    hello, topics = handshake(link, args.boot_wait)
    print("HELLO", hello)
    for t in topics:
        print("TOPIC", t)

    tick_hz = hello["tick_hz"]
    boot_ids = [hello["boot_id"]]
    t_start = time.monotonic()
    next_hb = next_twist = t_start
    next_ping = t_start
    burst = 8
    ping_id = 0
    pings = {}
    rtts, samples = [], []
    sent_twists = {}
    twist_i = 0
    echo_ok = echo_bad = 0
    battery_last = None
    battery_rx = battery_lost = 0
    mcu_counters = None
    resets = 0

    while time.monotonic() - t_start < args.seconds:
        now = time.monotonic()
        if now >= next_hb:
            link.send(4, b"")
            next_hb += 0.5
        if now >= next_twist:
            v = (float(twist_i), 0.0, 0.0, 0.0, 0.0, -float(twist_i))
            sent_twists[v] = now
            link.send(17, struct.pack("<6f", *v))
            twist_i += 1
            next_twist += 1.0 / args.twist_hz
        if now >= next_ping:
            ping_id = (ping_id + 1) & 0xFF
            pings[ping_id] = time.monotonic()
            link.send(1, bytes([1, ping_id]))
            if burst:
                burst -= 1
                next_ping = now + 0.05
            else:
                next_ping = now + 1.0
        for t_rx, topic, _seq, p in link.frames():
            if topic == 16 and len(p) == 4:
                (val,) = struct.unpack("<f", p)
                battery_rx += 1
                if battery_last is not None and val > battery_last + 1:
                    battery_lost += int(val - battery_last - 1)
                battery_last = val
            elif topic == 18 and len(p) == 24:
                v = struct.unpack("<6f", p)
                if v in sent_twists:
                    echo_ok += 1
                    del sent_twists[v]
                else:
                    echo_bad += 1
            elif topic == 1 and p and p[0] == 2:
                pid, tick = struct.unpack_from("<BI", p, 1)
                if pid in pings:
                    t_tx = pings.pop(pid)
                    rtt = t_rx - t_tx
                    rtts.append(rtt)
                    samples.append(((t_tx + t_rx) / 2, tick, rtt))
            elif topic == 4 and len(p) >= 10:
                mcu_counters = dict(zip(("rx_frames", "crc_errors", "rx_overflows", "dropped", "tx_busy"),
                                        struct.unpack_from("<5H", p)))
            elif topic == 0 and p and p[0] == 2:
                resets += 1
                print(f"MCU reset detected at {now - t_start:.2f} s, re-handshaking")
                hello, topics = handshake(link, 0, parse_hello(p))
                boot_ids.append(hello["boot_id"])
                battery_last = None
                samples, pings = [], {}  # the MCU tick restarted with the reset
        time.sleep(0.001)

    sent = twist_i
    drift = drift_ppm(samples, tick_hz)
    summary = {
        "seconds": args.seconds, "baud": args.baud,
        "battery_rx": battery_rx, "battery_lost": battery_lost,
        "twist_sent": sent, "echo_ok": echo_ok, "echo_bad": echo_bad,
        "rtt_ms_min": round(min(rtts) * 1e3, 2) if rtts else None,
        "rtt_ms_median": round(statistics.median(rtts) * 1e3, 2) if rtts else None,
        "drift_ppm": None if drift is None else round(drift, 1),
        "host_crc_errors": link.stats["crc_errors"], "host_bad_frames": link.stats["bad_frames"],
        "mcu": mcu_counters, "resets": resets, "boot_ids": boot_ids,
    }
    print(json.dumps(summary) if args.json else json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
