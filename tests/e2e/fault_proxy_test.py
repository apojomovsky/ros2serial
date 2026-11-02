"""Unit tests for the fault-injecting pty proxy, with no ROS in the loop.

Run directly: `python3 tests/e2e/fault_proxy_test.py`. The launch suite runs
it before any scenario, so a broken proxy fails fast instead of mid-handshake.
"""

import os
import select
import time
import tty
import unittest

from e2e_support import fault_proxy


def open_raw(port):
    """Open a pty slave with no line processing, so every byte passes through."""
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd)
    return fd


def read_available(fd, size=65536):
    """Drain what is waiting, stopping after a short idle gap."""
    out = b""
    deadline = time.monotonic() + 2.0
    idle_since = None
    while time.monotonic() < deadline:
        readable, _, _ = select.select([fd], [], [], 0.05)
        if not readable:
            if out and idle_since is not None and time.monotonic() - idle_since > 0.3:
                return out
            continue
        chunk = os.read(fd, size)
        if chunk:
            out += chunk
            idle_since = time.monotonic()
    return out


def exchange(proxy, payload, pause=0.3):
    """Send payload each way through a started proxy and return both outputs."""
    mcu = open_raw(proxy.mcu_port)
    host = open_raw(proxy.host_port)
    try:
        os.write(mcu, payload)
        time.sleep(pause)
        from_mcu = read_available(host)
        os.write(host, payload)
        time.sleep(pause)
        from_host = read_available(mcu)
        return from_mcu, from_host
    finally:
        os.close(mcu)
        os.close(host)


def frame(topic, seq, payload):
    """One wire frame without a valid CRC: the proxy never checks it.

    The payload must hold no zero bytes, so the encoding is one group and
    the first byte is its code.
    """
    assert 0 not in payload
    body = bytes([topic, seq]) + payload
    return bytes([len(body) + 1]) + body + b"\x00"


class TestCleanRelay(unittest.TestCase):
    def test_bytes_pass_both_ways_unchanged(self):
        proxy = fault_proxy.FaultProxy(seed=11)
        proxy.start()
        try:
            payload = bytes(range(1, 256)) * 4
            from_mcu, from_host = exchange(proxy, payload)
            self.assertEqual(from_mcu, payload)
            self.assertEqual(from_host, payload)
            counts = proxy.counts()
            self.assertEqual(counts["mcu_to_host"].corrupted, 0)
            self.assertEqual(counts["host_to_mcu"].corrupted, 0)
            self.assertEqual(counts["mcu_to_host"].dropped, 0)
            self.assertEqual(counts["host_to_mcu"].duplicated, 0)
        finally:
            proxy.stop()


class TestDrops(unittest.TestCase):
    def test_drop_rate_one_drops_everything(self):
        proxy = fault_proxy.FaultProxy(seed=21)
        proxy.set_faults(drop_rate=1.0)
        proxy.start()
        try:
            from_mcu, from_host = exchange(proxy, b"hello")
            self.assertEqual(from_mcu, b"")
            self.assertEqual(from_host, b"")
            self.assertEqual(proxy.counts()["mcu_to_host"].dropped, 5)
            self.assertEqual(proxy.counts()["host_to_mcu"].dropped, 5)
        finally:
            proxy.stop()

    def test_partial_drops_keep_order_and_count(self):
        payload = bytes(range(1, 128)) * 2
        first = fault_proxy.FaultProxy(seed=22)
        first.set_faults(drop_rate=0.5)
        first.start()
        try:
            out, _ = exchange(first, payload)
        finally:
            first.stop()
        second = fault_proxy.FaultProxy(seed=22)
        second.set_faults(drop_rate=0.5)
        second.start()
        try:
            repeat, _ = exchange(second, payload)
        finally:
            second.stop()
        self.assertEqual(out, repeat)
        self.assertLess(len(out), len(payload))
        self.assertGreater(len(out), 0)
        it = iter(payload)
        for byte in out:
            for candidate in it:
                if candidate == byte:
                    break
        counts = second.counts()["mcu_to_host"]
        self.assertEqual(counts.dropped, len(payload) - len(out))


class TestDuplicates(unittest.TestCase):
    def test_dup_rate_one_doubles_every_byte(self):
        proxy = fault_proxy.FaultProxy(seed=31)
        proxy.set_faults(dup_rate=1.0)
        proxy.start()
        try:
            from_mcu, _ = exchange(proxy, b"\x01\x02\x00\x03")
            self.assertEqual(from_mcu, b"\x01\x01\x02\x02\x00\x00\x03\x03")
            self.assertEqual(proxy.counts()["mcu_to_host"].duplicated, 4)
        finally:
            proxy.stop()


class TestCorruption(unittest.TestCase):
    def test_corrupt_rate_one_flips_one_bit_per_data_byte(self):
        wire = frame(16, 7, b"\x01AB") + frame(4, 9, b"xy")
        proxy = fault_proxy.FaultProxy(seed=41)
        proxy.set_faults(corrupt_rate=1.0)
        proxy.start()
        try:
            out, _ = exchange(proxy, wire)
        finally:
            proxy.stop()
        self.assertEqual(len(out), len(wire))
        for before, after in zip(wire, out):
            if before == 0x00:
                self.assertEqual(after, 0x00)
        first, second = wire.split(b"\x00")[0], wire.split(b"\x00")[1]
        got_first, got_second = out.split(b"\x00")[0], out.split(b"\x00")[1]
        self.assertEqual(got_first[0], first[0])
        self.assertEqual(got_second[0], second[0])
        for frame_index, (plain, got) in enumerate(((first, got_first), (second, got_second))):
            for position in range(1, len(plain)):
                xor = plain[position] ^ got[position]
                self.assertTrue(xor != 0 and xor & (xor - 1) == 0, f"frame {frame_index} byte {position}")
                self.assertNotEqual(got[position], 0x00)
        counts = proxy.counts()["mcu_to_host"]
        self.assertEqual(counts.frames, 2)
        self.assertEqual(counts.frames_touched, 2)
        self.assertEqual(counts.user_frames_touched, 1)
        self.assertEqual(counts.corrupted, len(first) + len(second) - 2)

    def test_user_boundary_at_topic_16(self):
        wire = frame(15, 0, b"q") + frame(16, 0, b"q")
        proxy = fault_proxy.FaultProxy(seed=42)
        proxy.set_faults(corrupt_rate=1.0)
        proxy.start()
        try:
            exchange(proxy, wire)
        finally:
            proxy.stop()
        counts = proxy.counts()["mcu_to_host"]
        self.assertEqual(counts.frames_touched, 2)
        self.assertEqual(counts.user_frames_touched, 1)

    def test_same_seed_replays_same_faults(self):
        payload = bytes(range(1, 200))
        outs = []
        for _ in range(2):
            proxy = fault_proxy.FaultProxy(seed=43)
            proxy.set_faults(corrupt_rate=0.3, drop_rate=0.1)
            proxy.start()
            try:
                out, _ = exchange(proxy, payload)
                outs.append(out)
            finally:
                proxy.stop()
        self.assertEqual(outs[0], outs[1])
        self.assertNotEqual(outs[0], payload)

    def test_garbage_never_crashes_and_balances(self):
        payload = bytes((n * 37 + 11) % 256 for n in range(600))
        proxy = fault_proxy.FaultProxy(seed=44)
        proxy.set_faults(corrupt_rate=0.2, drop_rate=0.2, dup_rate=0.2)
        proxy.start()
        try:
            out, _ = exchange(proxy, payload)
        finally:
            proxy.stop()
        counts = proxy.counts()["mcu_to_host"]
        self.assertEqual(len(out), len(payload) - counts.dropped + counts.duplicated)


class TestControl(unittest.TestCase):
    def test_bad_rates_are_rejected(self):
        proxy = fault_proxy.FaultProxy(seed=51)
        for kwargs in ({"drop_rate": -0.1}, {"dup_rate": 1.1}, {"corrupt_rate": 2.0}):
            with self.assertRaises(ValueError, msg=str(kwargs)):
                proxy.set_faults(**kwargs)

    def test_disabling_faults_recovers_clean_flow(self):
        proxy = fault_proxy.FaultProxy(seed=52)
        proxy.set_faults(corrupt_rate=1.0)
        proxy.start()
        try:
            mcu = open_raw(proxy.mcu_port)
            host = open_raw(proxy.host_port)
            try:
                os.write(mcu, frame(16, 1, b"AB"))
                time.sleep(0.3)
                dirty = read_available(host)
                before = proxy.counts()["mcu_to_host"].frames_touched
                proxy.set_faults()
                os.write(mcu, frame(16, 2, b"AB"))
                time.sleep(0.3)
                clean = read_available(host)
                after = proxy.counts()["mcu_to_host"].frames_touched
            finally:
                os.close(mcu)
                os.close(host)
            self.assertNotEqual(dirty, frame(16, 1, b"AB"))
            self.assertEqual(clean, frame(16, 2, b"AB"))
            self.assertEqual(before, 1)
            self.assertEqual(after, 1)
        finally:
            proxy.stop()


if __name__ == "__main__":
    unittest.main()
