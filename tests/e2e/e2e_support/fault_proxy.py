"""A pty proxy between the posix firmware and the bridge that injects faults.

Bytes written on one slave come out of the other, as with the plain relay,
but each direction first passes a fault stage: per-byte drops, duplications
and corruption, driven by a seeded generator so runs replay. Drops and
duplications apply to the byte stream at once; corruption waits for the
frame's delimiter, since rCOBS carries each group's code after its data
(spec/protocol.md, COB-4) and only the whole frame tells code from data.
Scenario 10 uses corruption only: a single flipped bit in an encoded data
byte always fails the receiver's CRC check, so every touched frame is
dropped exactly once and the diagnostics counters match the proxy's
touched-frame counts. The framing variant instead corrupts code bytes
and delimiters: a shifted grouping fails the decode or the CRC, and a
corrupted delimiter merges two frames into one undecodable blob. Bytes
leave the proxy at frame boundaries, so a trailing partial frame waits
for its delimiter.
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
    """One direction's counters plus its buffered frame in progress."""

    def __init__(self):
        self.counts = DirectionCounts()
        self.rng = random.Random()
        # Post-drop [byte, duplicated] pairs of the segment in progress,
        # that is the bytes since the previous delimiter.
        self.pending = []
        # Earlier merged segments with their replacement delimiters.
        self.stashed = []
        self.delim_dup = False
        self.framing_touched = False


def _check_rate(name, value):
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise ValueError(f'{name} {value!r} is not a number')
    if math.isnan(value) or not 0.0 <= value <= 1.0:
        raise ValueError(f'{name} {value!r} is outside 0..1')


class FaultProxy:
    """Two pty slaves tied master to master, faulting both ways."""

    def __init__(self, seed=0):
        self._mcu_master, mcu_slave = pty.openpty()
        self._host_master, host_slave = pty.openpty()
        self.mcu_port = os.ttyname(mcu_slave)
        self.host_port = os.ttyname(host_slave)
        os.close(mcu_slave)
        os.close(host_slave)
        # Non-blocking masters: select can report a master readable while its
        # slave is still closed, and a slave opened between select and read
        # would turn that blocking read into a wait for data that never comes.
        os.set_blocking(self._mcu_master, False)
        os.set_blocking(self._host_master, False)
        self._lock = threading.Lock()
        self._drop_rate = 0.0
        self._dup_rate = 0.0
        self._corrupt_rate = 0.0
        self._framing_rate = 0.0
        self._seed = seed
        self._mcu_to_host = _DirectionState()
        self._host_to_mcu = _DirectionState()
        self._mcu_to_host.rng.seed(seed)
        self._host_to_mcu.rng.seed(seed)

    def set_faults(
        self, *, drop_rate=0.0, dup_rate=0.0, corrupt_rate=0.0, framing_rate=0.0, seed=None
    ):
        """Replace the fault rates for both directions.

        A seed restarts both streams deterministically; without one the
        streams continue. Counts stay cumulative across calls. framing_rate
        corrupts code bytes and delimiters instead of data bytes, merging
        frames whose delimiter is replaced by a nonzero byte.
        """
        _check_rate('drop_rate', drop_rate)
        _check_rate('dup_rate', dup_rate)
        _check_rate('corrupt_rate', corrupt_rate)
        _check_rate('framing_rate', framing_rate)
        with self._lock:
            self._drop_rate = drop_rate
            self._dup_rate = dup_rate
            self._corrupt_rate = corrupt_rate
            self._framing_rate = framing_rate
            if seed is not None:
                self._seed = seed
                self._mcu_to_host.rng.seed(seed)
                self._host_to_mcu.rng.seed(seed)

    def counts(self):
        """A snapshot of both directions' counters, keyed by direction."""
        with self._lock:
            return {
                'mcu_to_host': self._mcu_to_host.counts.copy(),
                'host_to_mcu': self._host_to_mcu.counts.copy(),
            }

    def start(self):
        """Serve both directions until stop is called."""
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name='fault-proxy', daemon=True)
        self._thread.start()

    @staticmethod
    def _data_positions(frame):
        """Encoded offsets holding data, parsed back to front per COB-4.

        Returns None for a malformed frame, which carries no corruption: the
        receiver drops it however it arrives, so the proxy must not invent a
        touched frame the counters cannot match.
        """
        data = [False] * len(frame)
        end = len(frame)
        while end > 0:
            code = frame[end - 1]
            if code == 0 or code > end:
                return None
            for i in range(end - code, end - 1):
                data[i] = True
            end -= code
        return data

    def _corrupt_segment(self, state):
        """Corrupt one delimiter-terminated segment in place.

        Only the bytes since the previous delimiter are parsed, so a merge
        never hides a segment's code bytes from the fault draw. The segment
        carries no corruption when it is malformed: the receiver drops it
        however it arrives, so the proxy must not invent a touched frame
        the counters cannot match.
        """
        counts = state.counts
        pending = state.pending
        data = self._data_positions([byte for byte, _ in pending])
        if data is None:
            return
        for i, (byte, _) in enumerate(pending):
            if data[i]:
                if state.rng.random() < self._corrupt_rate:
                    # A one-bit flip outside 0x00: the grouping survives,
                    # so the decoded frame differs in exactly this bit.
                    flipped = byte ^ (0x02 if byte == 0x01 else 0x01)
                    pending[i][0] = flipped
                    counts.corrupted += 1
                    state.framing_touched = True
            elif self._framing_rate > 0.0 and state.rng.random() < self._framing_rate:
                # A one-bit flip outside 0x00 shifts the grouping, so the
                # decode fails or yields different bytes than the CRC covers.
                flipped = byte ^ (0x02 if byte == 0x01 else 0x01)
                pending[i][0] = flipped
                counts.corrupted += 1
                state.framing_touched = True

    def _flush_frame(self, state, out):
        counts = state.counts
        full = state.stashed + state.pending
        state.stashed = []
        state.pending = []
        touched = state.framing_touched
        state.framing_touched = False
        delim_dup = state.delim_dup
        state.delim_dup = False
        if full:
            counts.frames += 1
            topic = full[0][0]
            if touched:
                counts.frames_touched += 1
                # The first group's data starts the frame, so its first byte
                # is the topic id whatever groups follow.
                if topic >= _FIRST_USER_TOPIC:
                    counts.user_frames_touched += 1
            for byte, duplicated in full:
                out.append(byte)
                counts.bytes_out += 1
                if duplicated:
                    out.append(byte)
                    counts.bytes_out += 1
                    counts.duplicated += 1
        out.append(0x00)
        counts.bytes_out += 1
        if delim_dup:
            out.append(0x00)
            counts.bytes_out += 1
            counts.duplicated += 1

    def _fault_byte(self, state, byte, out):
        counts = state.counts
        counts.bytes_in += 1
        if state.rng.random() < self._drop_rate:
            counts.dropped += 1
            return
        duplicated = state.rng.random() < self._dup_rate
        if byte == 0x00:
            self._corrupt_segment(state)
            if self._framing_rate > 0.0 and state.rng.random() < self._framing_rate:
                # A nonzero delimiter merges this segment with the next one
                # instead of flushing it, desynchronising the receiver.
                replacement = state.rng.randint(1, 0xFF)
                state.stashed.extend(state.pending)
                state.pending = []
                state.stashed.append([replacement, duplicated])
                counts.corrupted += 1
                state.framing_touched = True
            else:
                state.delim_dup = duplicated
                self._flush_frame(state, out)
        else:
            state.pending.append([byte, duplicated])

    def _forward(self, source, target, state):
        # A master reads EIO until its slave is open and after it closes, and
        # the non-blocking read raises when select raced reality, so a failure
        # here only skips one turn: both processes open their slaves just
        # after this thread starts.
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
                self._fault_byte(state, byte, out)
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
