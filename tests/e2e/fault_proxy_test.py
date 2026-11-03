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
    """Drain what arrives, stopping after a quiet gap; the host is shared.

    Fixed sleeps flake under other agents' builds, so this polls until the
    flow goes quiet instead, with a generous deadline for a loaded runner.
    """
    out = b""
    deadline = time.monotonic() + 30.0
    idle_since = None
    while time.monotonic() < deadline:
        readable, _, _ = select.select([fd], [], [], 0.05)
        if not readable:
            if out and idle_since is not None and time.monotonic() - idle_since > 0.5:
                return out
            continue
        chunk = os.read(fd, size)
        if chunk:
            out += chunk
            idle_since = time.monotonic()
    raise AssertionError(f"flow never went quiet, {len(out)} bytes so far")


def exchange(proxy, payload):
    """Send payload each way through a started proxy and return both outputs.

    Only for fault modes that never drop the delimiters: a dropped final
    delimiter would leave the frame buffered, so drop tests flush below.
    """
    mcu = open_raw(proxy.mcu_port)
    host = open_raw(proxy.host_port)
    try:
        os.write(mcu, payload)
        from_mcu = read_available(host)
        os.write(host, payload)
        from_host = read_available(mcu)
        return from_mcu, from_host
    finally:
        os.close(mcu)
        os.close(host)


def send_then_flush(proxy, source, target, payload):
    """Send payload under faults, then a clean delimiter that flushes it.

    A faulty final delimiter might itself drop and leave the frame buffered,
    so the flush delimiter waits until every payload byte is processed and
    goes out after disabling the faults.
    """
    os.write(source, payload)
    deadline = time.monotonic() + 30.0
    while proxy.counts()["mcu_to_host"].bytes_in < len(payload):
        assert time.monotonic() < deadline, "payload never reached the proxy"
        time.sleep(0.05)
    proxy.set_faults()
    os.write(source, b"\x00")
    return read_available(target)


def rcobs_encode(body):
    """Encode per spec/protocol.md COB-1: each group's code ends its data."""
    out = bytearray()
    n = 1
    for b in body:
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


def frame(topic, seq, payload):
    """One wire frame without a valid CRC: the proxy never checks it."""
    return rcobs_encode(bytes([topic, seq]) + payload) + b"\x00"


def data_positions(encoded):
    """Data offsets of one frame, through the proxy's own classifier."""
    positions = fault_proxy.FaultProxy._data_positions(list(encoded))
    assert positions is not None, "test frame is malformed"
    return positions


class TestCleanRelay(unittest.TestCase):
    def test_bytes_pass_both_ways_unchanged(self):
        proxy = fault_proxy.FaultProxy(seed=11)
        proxy.start()
        try:
            # The trailing delimiter flushes the buffered frame whole.
            payload = bytes(range(1, 256)) * 4 + b"\x00"
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
            mcu = open_raw(proxy.mcu_port)
            host = open_raw(proxy.host_port)
            try:
                os.write(mcu, b"hello")
                self._wait_for_drops(proxy, "mcu_to_host", 5)
                os.write(host, b"hello")
                self._wait_for_drops(proxy, "host_to_mcu", 5)
                for fd in (mcu, host):
                    readable, _, _ = select.select([fd], [], [], 2.0)
                    self.assertFalse(readable, "a dropped byte still arrived")
            finally:
                os.close(mcu)
                os.close(host)
        finally:
            proxy.stop()

    def _wait_for_drops(self, proxy, direction, count):
        deadline = time.monotonic() + 30.0
        while proxy.counts()[direction].dropped < count:
            self.assertLess(time.monotonic(), deadline, "dropped bytes never reached the proxy")
            time.sleep(0.05)

    def test_partial_drops_keep_order_and_count(self):
        payload = bytes(range(1, 128)) * 2 + b"\x00"
        outs = []
        for _ in range(2):
            proxy = fault_proxy.FaultProxy(seed=22)
            proxy.set_faults(drop_rate=0.5)
            proxy.start()
            try:
                mcu = open_raw(proxy.mcu_port)
                host = open_raw(proxy.host_port)
                try:
                    outs.append((send_then_flush(proxy, mcu, host, payload), proxy.counts()))
                finally:
                    os.close(mcu)
                    os.close(host)
            finally:
                proxy.stop()
        (out, counts), (repeat, _) = outs
        self.assertEqual(out, repeat)
        self.assertLess(len(out), len(payload) + 1)
        self.assertGreater(len(out), 0)
        # The last byte is the clean flush delimiter; the rest kept order.
        it = iter(payload)
        for byte in out[:-1]:
            for candidate in it:
                if candidate == byte:
                    break
            else:
                self.fail(f"byte {byte} out of order")
        counts = counts["mcu_to_host"]
        self.assertEqual(counts.dropped, len(payload) + 1 - len(out))
        self.assertEqual(len(out), len(payload) + 1 - counts.dropped + counts.duplicated)


class TestDuplicates(unittest.TestCase):
    def test_dup_rate_one_doubles_every_byte(self):
        proxy = fault_proxy.FaultProxy(seed=31)
        proxy.set_faults(dup_rate=1.0)
        proxy.start()
        try:
            from_mcu, _ = exchange(proxy, b"\x01\x02\x00\x03\x00")
            self.assertEqual(from_mcu, b"\x01\x01\x02\x02\x00\x00\x03\x03\x00\x00")
            self.assertEqual(proxy.counts()["mcu_to_host"].duplicated, 5)
        finally:
            proxy.stop()


class TestCorruption(unittest.TestCase):
    def test_corrupt_rate_one_flips_one_bit_per_data_byte(self):
        wire = frame(16, 7, b"\x01AB") + frame(4, 9, b"x\x00y")
        proxy = fault_proxy.FaultProxy(seed=41)
        proxy.set_faults(corrupt_rate=1.0)
        proxy.start()
        try:
            out, _ = exchange(proxy, wire)
        finally:
            proxy.stop()
        self.assertEqual(len(out), len(wire))
        for before, after in zip(wire, out, strict=True):
            if before == 0x00:
                self.assertEqual(after, 0x00)
        plain_frames = wire.split(b"\x00")[:2]
        got_frames = out.split(b"\x00")[:2]
        total_data = 0
        for frame_index, (plain, got) in enumerate(zip(plain_frames, got_frames, strict=True)):
            data = data_positions(plain)
            total_data += sum(data)
            for position, is_data in enumerate(data):
                if is_data:
                    xor = plain[position] ^ got[position]
                    self.assertTrue(xor != 0 and xor & (xor - 1) == 0, f"frame {frame_index} byte {position}")
                    self.assertNotEqual(got[position], 0x00)
                else:
                    self.assertEqual(got[position], plain[position], f"frame {frame_index} code {position}")
        counts = proxy.counts()["mcu_to_host"]
        self.assertEqual(counts.frames, 2)
        self.assertEqual(counts.frames_touched, 2)
        self.assertEqual(counts.user_frames_touched, 1)
        self.assertEqual(counts.corrupted, total_data)

    def test_user_boundary_at_topic_16(self):
        wire = frame(15, 3, b"q") + frame(16, 3, b"q")
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
        payload = bytes(range(1, 200)) + b"\x00"
        outs = []
        for _ in range(2):
            proxy = fault_proxy.FaultProxy(seed=43)
            proxy.set_faults(corrupt_rate=0.3, drop_rate=0.1)
            proxy.start()
            try:
                mcu = open_raw(proxy.mcu_port)
                host = open_raw(proxy.host_port)
                try:
                    outs.append((send_then_flush(proxy, mcu, host, payload), proxy.counts()))
                finally:
                    os.close(mcu)
                    os.close(host)
            finally:
                proxy.stop()
        self.assertEqual(outs[0][0], outs[1][0])
        self.assertEqual(outs[0][1]["mcu_to_host"].__dict__, outs[1][1]["mcu_to_host"].__dict__)
        faults = outs[0][1]["mcu_to_host"].dropped + outs[0][1]["mcu_to_host"].corrupted
        self.assertGreater(faults, 0)

    def test_garbage_never_crashes_and_balances(self):
        payload = bytes((n * 37 + 11) % 256 for n in range(600)) + b"\x00"
        proxy = fault_proxy.FaultProxy(seed=44)
        proxy.set_faults(corrupt_rate=0.2, drop_rate=0.2, dup_rate=0.2)
        proxy.start()
        try:
            mcu = open_raw(proxy.mcu_port)
            host = open_raw(proxy.host_port)
            try:
                out = send_then_flush(proxy, mcu, host, payload)
            finally:
                os.close(mcu)
                os.close(host)
        finally:
            proxy.stop()
        counts = proxy.counts()["mcu_to_host"]
        self.assertEqual(len(out), len(payload) + 1 - counts.dropped + counts.duplicated)


class TestFraming(unittest.TestCase):
    def test_code_bytes_corrupt_while_data_survives(self):
        encoded = rcobs_encode(bytes([16, 7]) + b"\x01AB")
        proxy = fault_proxy.FaultProxy(seed=61)
        proxy.set_faults(framing_rate=1.0)
        proxy.start()
        try:
            mcu = open_raw(proxy.mcu_port)
            host = open_raw(proxy.host_port)
            try:
                out = send_then_flush(proxy, mcu, host, encoded)
            finally:
                os.close(mcu)
                os.close(host)
        finally:
            proxy.stop()
        self.assertEqual(len(out), len(encoded) + 1)
        self.assertEqual(out[-1], 0x00)
        data = data_positions(encoded)
        for position, is_data in enumerate(data):
            if is_data:
                self.assertEqual(out[position], encoded[position], f"data byte {position}")
            else:
                xor = encoded[position] ^ out[position]
                self.assertTrue(xor != 0 and xor & (xor - 1) == 0, f"code byte {position}")
                self.assertNotEqual(out[position], 0x00)
        counts = proxy.counts()["mcu_to_host"]
        self.assertEqual(counts.frames, 1)
        self.assertEqual(counts.frames_touched, 1)
        self.assertEqual(counts.user_frames_touched, 1)
        self.assertEqual(counts.corrupted, sum(not flag for flag in data))

    def test_corrupted_delimiter_merges_two_frames(self):
        first = frame(16, 7, b"AB")
        second = frame(4, 9, b"CD")
        proxy = fault_proxy.FaultProxy(seed=62)
        proxy.set_faults(framing_rate=1.0)
        proxy.start()
        try:
            mcu = open_raw(proxy.mcu_port)
            host = open_raw(proxy.host_port)
            try:
                out = send_then_flush(proxy, mcu, host, first + second)
            finally:
                os.close(mcu)
                os.close(host)
        finally:
            proxy.stop()
        first_enc, second_enc = first[:-1], second[:-1]
        self.assertEqual(len(out), len(first_enc) + 1 + len(second_enc) + 1)
        self.assertEqual(out[-1], 0x00)
        self.assertNotEqual(out[len(first_enc)], 0x00)
        self.assertNotIn(b"\x00", out[:-1])
        for offset, enc in ((0, first_enc), (len(first_enc) + 1, second_enc)):
            for position, is_data in enumerate(data_positions(enc)):
                if is_data:
                    self.assertEqual(out[offset + position], enc[position])
                else:
                    xor = enc[position] ^ out[offset + position]
                    self.assertTrue(xor != 0 and xor & (xor - 1) == 0)
                    self.assertNotEqual(out[offset + position], 0x00)
        counts = proxy.counts()["mcu_to_host"]
        self.assertEqual(counts.frames, 1)
        self.assertEqual(counts.frames_touched, 1)
        self.assertEqual(counts.user_frames_touched, 1)
        codes = sum(not flag for flag in data_positions(first_enc))
        codes += sum(not flag for flag in data_positions(second_enc))
        self.assertEqual(counts.corrupted, codes + 2)

    def test_same_seed_replays_framing_faults(self):
        wire = frame(16, 7, b"\x01AB") + frame(20, 3, b"x\x00y") + frame(17, 4, b"zz")
        outs = []
        for _ in range(2):
            proxy = fault_proxy.FaultProxy(seed=63)
            proxy.set_faults(framing_rate=0.5, corrupt_rate=0.1)
            proxy.start()
            try:
                mcu = open_raw(proxy.mcu_port)
                host = open_raw(proxy.host_port)
                try:
                    outs.append((send_then_flush(proxy, mcu, host, wire), proxy.counts()))
                finally:
                    os.close(mcu)
                    os.close(host)
            finally:
                proxy.stop()
        self.assertEqual(outs[0][0], outs[1][0])
        self.assertEqual(outs[0][1]["mcu_to_host"].__dict__, outs[1][1]["mcu_to_host"].__dict__)
        faults = outs[0][1]["mcu_to_host"].corrupted
        self.assertGreater(faults, 0)

    def test_bad_framing_rate_is_rejected(self):
        proxy = fault_proxy.FaultProxy(seed=64)
        for bad in (-0.1, 1.1, float("nan")):
            with self.assertRaises(ValueError, msg=str(bad)):
                proxy.set_faults(framing_rate=bad)


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
                dirty = read_available(host)
                before = proxy.counts()["mcu_to_host"].frames_touched
                proxy.set_faults()
                os.write(mcu, frame(16, 2, b"AB"))
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
