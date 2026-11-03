"""A pty proxy between the posix firmware and the bridge that injects faults.

Bytes written on one slave come out of the other, as with the plain relay,
but each direction first passes a fault stage: per-byte drops, duplications
and corruption, driven by a seeded generator so runs replay. Scenario 10
uses corruption only: a single flipped bit in an encoded data byte always
fails the receiver's CRC check, so every touched frame is dropped exactly
once and the diagnostics counters match the proxy's touched-frame counts.
Delimiters and rCOBS code bytes pass through untouched for the same reason:
changing the frame count would break that exact match.
"""

import math
import os
import pty
import random
import select
import threading
import time

# Decoded topic ids 0 to 15 are system channels (spec/protocol.md, FRM-3).
_FIRST_USER_TOPIC = 16


class DirectionCounts:
    """Cumulative fault counters for one direction of the proxy."""

    def __init__(self):
        self.bytes_in = 0
        self.bytes_out = 0
        self.dropped = 0
        self.duplicated = 0
        self.corrupted = 0
        self.frames = 0
        self.frames_touched = 0
        self.user_frames_touched = 0

    def copy(self):
        """An independent snapshot, so readers never see a half update."""
        out = DirectionCounts()
        out.__dict__.update(self.__dict__)
        return out


class _DirectionState:
    """One direction's counters plus its incremental frame parse."""

    def __init__(self):
        self.counts = DirectionCounts()
        self.rng = random.Random()
        self.offset = 0
        self.next_code = 0
        self.first_data = 0
        self.touched = False


def _check_rate(name, value):
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise ValueError(f"{name} {value!r} is not a number")
    if math.isnan(value) or not 0.0 <= value <= 1.0:
        raise ValueError(f"{name} {value!r} is outside 0..1")


class FaultProxy:
    """Two pty slaves tied master to master, faulting both ways."""

    def __init__(self, seed=0):
        self._mcu_master, mcu_slave = pty.openpty()
        self._host_master, host_slave = pty.openpty()
        self.mcu_port = os.ttyname(mcu_slave)
        self.host_port = os.ttyname(host_slave)
        os.close(mcu_slave)
        os.close(host_slave)
        self._lock = threading.Lock()
        self._drop_rate = 0.0
        self._dup_rate = 0.0
        self._corrupt_rate = 0.0
        self._seed = seed
        self._mcu_to_host = _DirectionState()
        self._host_to_mcu = _DirectionState()
        self._mcu_to_host.rng.seed(seed)
        self._host_to_mcu.rng.seed(seed)

    def set_faults(self, *, drop_rate=0.0, dup_rate=0.0, corrupt_rate=0.0, seed=None):
        """Replace the fault rates for both directions.

        A seed restarts both streams deterministically; without one the
        streams continue. Counts stay cumulative across calls.
        """
        _check_rate("drop_rate", drop_rate)
        _check_rate("dup_rate", dup_rate)
        _check_rate("corrupt_rate", corrupt_rate)
        with self._lock:
            self._drop_rate = drop_rate
            self._dup_rate = dup_rate
            self._corrupt_rate = corrupt_rate
            if seed is not None:
                self._seed = seed
                self._mcu_to_host.rng.seed(seed)
                self._host_to_mcu.rng.seed(seed)

    def counts(self):
        """A snapshot of both directions' counters, keyed by direction."""
        with self._lock:
            return {
                "mcu_to_host": self._mcu_to_host.counts.copy(),
                "host_to_mcu": self._host_to_mcu.counts.copy(),
            }

    def start(self):
        """Serve both directions until stop is called."""
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="fault-proxy", daemon=True)
        self._thread.start()

    def _fault_byte(self, state, byte):
        counts = state.counts
        counts.bytes_in += 1
        if state.rng.random() < self._drop_rate:
            counts.dropped += 1
            self._track_input(state, byte)
            return None
        out = byte
        if byte != 0x00 and state.offset != state.next_code:
            if state.rng.random() < self._corrupt_rate:
                out = byte ^ (0x02 if byte == 0x01 else 0x01)
                counts.corrupted += 1
                state.touched = True
        self._track_input(state, byte)
        copies = 1
        if state.rng.random() < self._dup_rate:
            copies = 2
            counts.duplicated += 1
        counts.bytes_out += copies
        return bytes([out]) * copies

    @staticmethod
    def _track_input(state, byte):
        # Forward parse of the rCOBS framing (spec/protocol.md, COB-1): the
        # byte at a group start is a code, the rest is data. Pure arithmetic,
        # so malformed input only misclassifies, never crashes.
        if byte == 0x00:
            counts = state.counts
            if state.offset > 0:
                counts.frames += 1
                if state.touched:
                    counts.frames_touched += 1
                    if state.offset >= 2 and state.first_data >= _FIRST_USER_TOPIC:
                        counts.user_frames_touched += 1
            state.offset = 0
            state.next_code = 0
            state.first_data = 0
            state.touched = False
            return
        if state.offset == state.next_code:
            state.next_code = state.offset + byte
        if state.offset == 1:
            state.first_data = byte
        state.offset += 1

    def _forward(self, source, target, state):
        # A master reads EIO until its slave is open and after it closes, so
        # a failure here only skips one turn: both processes open their slaves
        # just after this thread starts.
        try:
            chunk = os.read(source, 4096)
        except OSError:
            time.sleep(0.005)
            return
        if not chunk:
            return
        out = bytearray()
        with self._lock:
            for byte in chunk:
                piece = self._fault_byte(state, byte)
                if piece is not None:
                    out += piece
        view = memoryview(bytes(out))
        while view:
            try:
                written = os.write(target, view)
            except OSError:
                time.sleep(0.005)
                return
            view = view[written:]

    def _run(self):
        while not self._stop.is_set():
            readable, _, _ = select.select([self._mcu_master, self._host_master], [], [], 0.05)
            for source in readable:
                if source == self._mcu_master:
                    self._forward(source, self._host_master, self._mcu_to_host)
                else:
                    self._forward(source, self._mcu_master, self._host_to_mcu)

    def stop(self):
        """Join the thread and close both masters."""
        self._stop.set()
        self._thread.join(timeout=5.0)
        os.close(self._mcu_master)
        os.close(self._host_master)
