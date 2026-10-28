"""The session state machine: random event sequences against libr2s_host.

Each rule feeds one event to the C core and to the Python model and compares
every observable after it. The event set covers the implemented session: the
handshake, heartbeats, user traffic, corrupted input, the timers and publish
calls. Time, log, parameters, error report and reliable-ack traffic stays
out: P1, P2 and P5 specify that behavior when they land.
"""

import os

from hypothesis import HealthCheck, settings
from hypothesis import strategies as st
from hypothesis.stateful import RuleBasedStateMachine, initialize, invariant, rule, run_state_machine_as_test
from r2s_gen.reference import session as model
from session_harness import TOPIC_IDS, Pair, wire

EXAMPLES = int(os.environ.get("R2S_MODEL_EXAMPLES", "30"))

VALID_MSS = [0, 1, 99, 100, 499, 500, 999, 1000, 1500, 1999, 2000, 2001, 5000]


def valid_twist(narrow: bool) -> bytes:
    """A decodable cmd_vel payload for the announced width."""
    return bytes(24 if narrow else 48)


class SessionMachine(RuleBasedStateMachine):
    """Random sequences through the pair; every rule ends in a comparison."""

    def __init__(self) -> None:
        super().__init__()
        self.pair = Pair()
        self.host_seq = {0: 0, 4: 0, 17: 0}

    @initialize()
    def boot(self) -> None:
        self.pair.reset()
        self.host_seq = {0: 0, 4: 0, 17: 0}

    @rule()
    def hello_req(self) -> None:
        self.pair.feed_frame(0, self._seq(0), bytes((model.OP_HELLO_REQ, 2)))

    @rule(index=st.sampled_from([0, 1, 2, 3, 9]))
    def topic_req(self, index: int) -> None:
        self.pair.feed_frame(0, self._seq(0), bytes((model.OP_TOPIC_REQ, index)))

    @rule(topic_id=st.sampled_from([16, 17, 18, 0x42]), status=st.sampled_from([0, 0, 0, 1, 2, 3, 4]))
    def topic_status(self, topic_id: int, status: int) -> None:
        self.pair.feed_frame(0, self._seq(0), bytes((model.OP_TOPIC_STATUS, topic_id, status)))

    @rule(token=st.sampled_from([1, 0xAABBCCDD, 0, 0xFFFFFFFF]))
    def ready(self, token: int) -> None:
        self.pair.feed_frame(0, self._seq(0), bytes((model.OP_READY,)) + token.to_bytes(4, "little"))

    @rule(empty=st.booleans())
    def heartbeat(self, empty: bool) -> None:
        self.pair.feed_frame(4, self._seq(4), b"" if empty else b"\x01")

    @rule(kind=st.sampled_from(["valid", "short", "long", "gap"]))
    def cmd_vel(self, kind: str) -> None:
        narrow = bool(self.pair.model.topics[1].flags & 0x02)
        if kind == "valid":
            payload = valid_twist(narrow)
        elif kind == "short":
            payload = bytes(3)
        elif kind == "long":
            payload = valid_twist(narrow) + b"\x00" * 4
        else:
            payload = valid_twist(narrow)
            self.host_seq[17] = (self.host_seq[17] + 7) & 0xFF
        self.pair.feed_frame(17, self._seq(17), payload)

    @rule(topic_id=st.sampled_from([16, 18]))
    def pub_topic_from_host(self, topic_id: int) -> None:
        """Host frames on MCU-published topics are refused, not dispatched."""
        self.pair.feed_frame(topic_id, 0, bytes(8))

    @rule()
    def corrupt_crc(self) -> None:
        frame = bytearray(wire(4, self._seq(4), b""))
        frame[2] ^= 0xFF
        self.pair.feed_raw(bytes(frame))

    @rule()
    def corrupt_rcobs(self) -> None:
        self.pair.feed_raw(b"\x05\x01\x00")

    @rule()
    def truncated_frame(self) -> None:
        frame = wire(0, self._seq(0), bytes((model.OP_HELLO_REQ, 2)))
        self.pair.feed_raw(frame[: len(frame) // 2])

    @rule()
    def oversized_frame(self) -> None:
        self.pair.feed_raw(bytes([0x41]) * 516 + b"\x00")

    @rule()
    def empty_frame(self) -> None:
        self.pair.feed_raw(b"\x00")

    @rule(ms=st.sampled_from(VALID_MSS))
    def advance(self, ms: int) -> None:
        self.pair.advance(ms)

    @rule(value=st.sampled_from([1.5, -0.0, 100.25]))
    def publish(self, value: float) -> None:
        self.pair.publish(value)

    @rule()
    def reboot(self) -> None:
        self.pair.reset()
        self.host_seq = {0: 0, 4: 0, 17: 0}

    @invariant()
    def encoded_tx_holds_no_zero(self) -> None:
        """The rCOBS output the pair drains carries no delimiter (COB-2)."""
        for frame in self.pair.host.take_tx():
            assert all(byte != 0 for byte in frame)
        assert self.pair.model.take_tx() == []

    def _seq(self, topic: int) -> int:
        seq = self.host_seq[topic]
        self.host_seq[topic] = (seq + 1) & 0xFF
        return seq


# covers: SES-1, SES-2, SES-3, SES-4, SES-5, SES-6, SES-7, SES-8, SES-9, SES-10, SES-11
# covers: FRM-6, FRM-7, FRM-8, FRM-12, FRM-14, FRM-15
# covers: SYS-5, CTL-16, HB-2, HB-3, HB-4, HB-5, HB-6, HB-7
def test_session_matches_model():
    run_state_machine_as_test(
        SessionMachine,
        settings=settings(
            max_examples=EXAMPLES, deadline=None, database=None, suppress_health_check=[HealthCheck.too_slow]
        ),
    )


def handshake(pair: Pair, token: int = 1) -> None:
    """The full handshake to CONNECTED, comparing after every step."""
    pair.feed_frame(0, 0, bytes((model.OP_HELLO_REQ, 2)))
    for index in range(3):
        pair.feed_frame(0, 1 + index, bytes((model.OP_TOPIC_REQ, index)))
        pair.feed_frame(0, 4 + index, bytes((model.OP_TOPIC_STATUS, TOPIC_IDS[index], 0)))
    pair.feed_frame(0, 7, bytes((model.OP_READY,)) + token.to_bytes(4, "little"))


# covers: SES-8, SES-9
def test_timer_boundaries_against_the_library():
    pair = Pair()
    handshake(pair)
    pair.advance(499)
    assert pair.host.take_tx() == []
    assert pair.model.take_tx() == []
    pair.advance(1)
    pair.check("heartbeat at exactly 500 ms")
    pair.feed_frame(4, 0, b"")
    pair.advance(1999)
    assert pair.host.snapshot()["connected"] == model.CONNECTED
    pair.feed_frame(4, 1, b"")
    pair.advance(1999)
    assert pair.host.snapshot()["connected"] == model.CONNECTED
    pair.advance(1)
    pair.check("link timeout at exactly 2000 ms")


# covers: SES-11
def test_seq_restarts_after_rehandshake():
    pair = Pair()
    handshake(pair)
    pair.publish(1.5)
    pair.feed_frame(0, 8, bytes((model.OP_HELLO_REQ, 2)))
    pair.publish(1.5)
    pair.check("publish after re-handshake reuses seq 0")


# covers: SES-1
def test_spin_limit_holds_backlog_frames():
    pair = Pair()
    handshake(pair, token=9)
    for seq in range(10):
        pair.host.queue(wire(4, seq, b""))
        pair.model.feed(wire(4, seq, b""))
    pair.host.spin_now()
    pair.model.spin()
    pair.check("eight of ten queued units processed")
    assert pair.host.snapshot()["counters"][0] == 16
    pair.host.spin_now()
    pair.model.spin()
    pair.check("the last two units processed")
    assert pair.host.snapshot()["counters"][0] == 18
