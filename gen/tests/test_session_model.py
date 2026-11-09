# Copyright (c) 2026 Alexis Pojomovsky
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

"""The session state machine: random event sequences against libr2s_host.

Each rule feeds one event to the C core and to the Python model and compares
every observable after it. The event set covers the implemented session: the
handshake, heartbeats, user traffic, corrupted input, the timers, publish
calls, the error reports they can trigger, the parameter walk answers and
the reliable SET and ack exchange with its retransmit timers. Time and log
traffic stays out: P1 and P2 specify that behavior when they land.
"""

import struct

from hypothesis import HealthCheck, settings
from hypothesis import strategies as st
from hypothesis.stateful import RuleBasedStateMachine, initialize, invariant, rule, run_state_machine_as_test
from r2s_gen.reference import session as model
from session_harness import TOPIC_IDS, Pair, wire

MAX_EXAMPLES = 30

VALID_MSS = [0, 1, 99, 100, 499, 500, 999, 1000, 1500, 1999, 2000, 2001, 5000]


def valid_twist(narrow: bool) -> bytes:
    """A decodable cmd_vel payload for the announced width."""
    return bytes(24 if narrow else 48)


PARAM_REQ = 0x01
PARAM_SET = 0x03


class SessionMachine(RuleBasedStateMachine):
    """Random sequences through the pair; every rule ends in a comparison."""

    def __init__(self) -> None:
        super().__init__()
        self.pair = Pair()
        self.host_seq = {0: 0, 3: 0, 4: 0, 6: 0, 17: 0}
        self.set_rel = 0
        self.status_rel = 0
        self.led = 1
        self.name_value = b"hi"
        self.last_set: bytes | None = None

    @initialize()
    def boot(self) -> None:
        self.pair.reset()
        self.host_seq = {0: 0, 3: 0, 4: 0, 6: 0, 17: 0}
        self.set_rel = 0
        self.status_rel = 0
        self.led = 1
        self.name_value = b"hi"
        self.last_set = None

    @rule()
    def hello_req(self) -> None:
        self.pair.feed_frame(0, self._seq(0), bytes((model.OP_HELLO_REQ, model.PROTOCOL_VERSION)))

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

    @rule(index=st.sampled_from([0, 1, 2, 3, 9]))
    def param_req(self, index: int) -> None:
        self.pair.feed_frame(3, self._seq(3), bytes((PARAM_REQ, index)))

    def _send_set(self, payload: bytes) -> None:
        """One new-rel SET through both sides, tracking both rel streams."""
        self.pair.feed_frame(3, self._seq(3), payload)
        self.last_set = payload
        self.set_rel = (self.set_rel + 1) & 0xFF
        self.status_rel = (self.status_rel + 1) & 0xFF

    @rule()
    def param_set_led(self) -> None:
        self.led = 1 - self.led
        self._send_set(bytes((PARAM_SET, self.set_rel, 0, self.led)))

    @rule()
    def param_set_name(self) -> None:
        self.name_value = b"xy" if self.name_value == b"hi" else b"hi"
        value = self.name_value
        self._send_set(bytes((PARAM_SET, self.set_rel, 2, len(value))) + value)

    @rule()
    def param_set_readonly(self) -> None:
        self._send_set(bytes((PARAM_SET, self.set_rel, 1)) + (7).to_bytes(8, "little"))

    @rule()
    def param_set_unknown_index(self) -> None:
        self._send_set(bytes((PARAM_SET, self.set_rel, 9, 0)))

    @rule()
    def param_set_duplicate(self) -> None:
        """The last SET again: acked by both, applied by neither."""
        if self.last_set is None:
            return
        self.pair.feed_frame(3, self._seq(3), self.last_set)

    @rule()
    def param_set_future(self) -> None:
        """A rel seq neither side expects: acked by both, applied by neither."""
        self.pair.feed_frame(3, self._seq(3), bytes((PARAM_SET, (self.set_rel + 5) & 0xFF, 0, 0)))

    @rule()
    def param_ack_status(self) -> None:
        """Ack the newest STATUS both sides sent, when one went out."""
        if self.status_rel == 0:
            return
        self.pair.feed_frame(6, self._seq(6), bytes((3, (self.status_rel - 1) & 0xFF)))

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
        frame = wire(0, self._seq(0), bytes((model.OP_HELLO_REQ, model.PROTOCOL_VERSION)))
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
        self.host_seq = {0: 0, 3: 0, 4: 0, 6: 0, 17: 0}
        self.set_rel = 0
        self.status_rel = 0
        self.led = 1
        self.name_value = b"hi"
        self.last_set = None

    @invariant()
    def encoded_tx_holds_no_zero(self) -> None:
        """The rCOBS output of the last step carries no delimiter (COB-2)."""
        for frame in self.pair.last_host_tx + self.pair.last_model_tx:
            assert all(byte != 0 for byte in frame)

    def _seq(self, topic: int) -> int:
        seq = self.host_seq[topic]
        self.host_seq[topic] = (seq + 1) & 0xFF
        return seq


# covers: SES-1, SES-2, SES-3, SES-4, SES-5, SES-6, SES-7, SES-8, SES-9, SES-10, SES-11
# covers: FRM-6, FRM-7, FRM-8, FRM-12, FRM-14, FRM-15
# covers: SYS-5, CTL-16, HB-2, HB-3, HB-4, HB-5, HB-6, HB-7
# covers: REL-1, REL-2, REL-3, REL-4, REL-5, PAR-1, PAR-2, PAR-3, PAR-4, PAR-5, PAR-6, PAR-8
def test_session_matches_model():
    run_state_machine_as_test(
        SessionMachine,
        settings=settings(
            max_examples=MAX_EXAMPLES,
            deadline=None,
            database=None,
            derandomize=True,
            suppress_health_check=[HealthCheck.too_slow],
        ),
    )


def handshake(pair: Pair, token: int = 1) -> None:
    """The full handshake to CONNECTED, comparing after every step."""
    pair.feed_frame(0, 0, bytes((model.OP_HELLO_REQ, model.PROTOCOL_VERSION)))
    for index in range(4):
        pair.feed_frame(0, 1 + index, bytes((model.OP_TOPIC_REQ, index)))
        pair.feed_frame(0, 5 + index, bytes((model.OP_TOPIC_STATUS, TOPIC_IDS[index], 0)))
    pair.feed_frame(0, 9, bytes((model.OP_READY,)) + token.to_bytes(4, "little"))


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
    pair.feed_frame(0, 10, bytes((model.OP_HELLO_REQ, model.PROTOCOL_VERSION)))
    pair.publish(1.5)
    pair.check("publish after re-handshake reuses seq 0")


# covers: ERR-3, ERR-6
def test_not_connected_count_wraps_past_65535():
    pair = Pair()
    payload = struct.pack("<f", 1.5)
    for _ in range(70000):
        assert pair.host.publish_battery(1.5) == model.ERR_NOT_CONNECTED
        assert pair.model.publish(16, payload) == model.ERR_NOT_CONNECTED
    # The first failure sends count 1; the other 69999 wait, and 69999
    # past the wrap reads back as 4463 in the next connection.
    pair.feed_frame(0, 0, bytes((model.OP_HELLO_REQ, 2)))
    for index in range(4):
        pair.feed_frame(0, 1 + index, bytes((model.OP_TOPIC_REQ, index)))
        pair.feed_frame(0, 5 + index, bytes((model.OP_TOPIC_STATUS, TOPIC_IDS[index], 0)))
    pair.host.feed(wire(0, 9, bytes((model.OP_READY,)) + (1).to_bytes(4, "little")))
    pair.model.feed(wire(0, 9, bytes((model.OP_READY,)) + (1).to_bytes(4, "little")))
    pair.host.spin_now()
    pair.model.spin()
    expected = [model.encode_frame(5, 0, bytes((5, 16, 0x6F, 0x11, 0xFF)))]
    assert pair.host.take_tx() == expected
    assert pair.model.take_tx() == expected
    pair.check("wrapped count flushes on READY")


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
    assert pair.host.snapshot()["counters"][0] == 18
    pair.host.spin_now()
    pair.model.spin()
    pair.check("the last two units processed")
    assert pair.host.snapshot()["counters"][0] == 20


# covers: FRM-4, FRM-5
def test_tx_seq_wraps_at_256():
    pair = Pair()
    handshake(pair)
    for _ in range(256):
        pair.publish(1.5)
    assert pair.host.snapshot()["tx_seq"] == [0, 0, 0, 0]
    pair.publish(1.5)
    pair.check("publish after the seq wraps reuses seq 0")


# covers: SES-1, FRM-14
def test_spin_budget_skips_dropped_units():
    pair = Pair()
    handshake(pair)
    pair.feed_raw(b"\x00" * 8 + wire(4, 0, b""))
    assert pair.host.snapshot()["counters"] == (11, 0, 0, 0)


# covers: PAR-1, PAR-2, PAR-6, PAR-7
def test_declare_answers_every_configured_type():
    pair = Pair()
    pair.feed_frame(3, 0, bytes((PARAM_REQ, 0)))
    pair.feed_frame(3, 1, bytes((PARAM_REQ, 1)))
    pair.feed_frame(3, 2, bytes((PARAM_REQ, 2)))
    assert pair.last_model_tx == [
        model.encode_frame(
            3, 2, bytes((model.OP_PARAM_DECLARE, 2, model.PARAM_STRING, 0, 4)) + b"name" + bytes((2,)) + b"ab"
        )
    ]
    pair.feed_frame(3, 3, bytes((PARAM_REQ, 3)))
    assert pair.last_model_tx == [
        model.encode_frame(3, 3, bytes((model.OP_PARAM_STATUS, 0, 3, model.PARAM_UNKNOWN_INDEX)))
    ]


# covers: SES-10
def test_reserved_channel_in_announcing_answers_hello():
    pair = Pair()
    pair.feed_frame(2, 0, b"")
    assert len(pair.last_model_tx) == 1


# covers: ERR-2
def test_unknown_topic_status_reports_code_2():
    pair = Pair()
    handshake(pair)
    pair.feed_frame(0, 10, bytes((model.OP_TOPIC_STATUS, 0x42, 0)))
    assert pair.last_model_tx == [model.encode_frame(5, 0, bytes((2, 0x42, 1, 0, 0xFF)))]


def _send_set(pair: Pair, seq: int, rel: int, index: int, value: bytes) -> None:
    """One SET through both sides; the check after it compares everything."""
    pair.feed_frame(3, seq, bytes((PARAM_SET, rel, index)) + value)


def _send_ack(pair: Pair, seq: int, rel: int) -> None:
    """One channel 6 ack through both sides."""
    pair.feed_frame(6, seq, bytes((model.PARAM_CHANNEL, rel)))


# covers: PAR-3, PAR-4, PAR-5, PAR-8, REL-1, REL-3, REL-4
def test_set_verdicts_cover_every_type_rule():
    pair = Pair()
    handshake(pair)
    seq3, seq6 = 0, 0
    _send_set(pair, seq3, 0, 0, b"\x01")
    seq3 += 1
    assert pair.model.snapshot()["params"][0] == (model.PARAM_BOOL, 1)
    _send_ack(pair, seq6, 0)
    seq6 += 1
    _send_set(pair, seq3, 1, 0, b"\x02")
    seq3 += 1
    _send_set(pair, seq3, 1, 0, b"")
    seq3 += 1
    _send_ack(pair, seq6, 1)
    seq6 += 1
    _send_set(pair, seq3, 2, 1, b"\x01\x02\x03\x04")
    seq3 += 1
    _send_ack(pair, seq6, 2)
    seq6 += 1
    _send_set(pair, seq3, 3, 1, (7).to_bytes(8, "little"))
    seq3 += 1
    _send_ack(pair, seq6, 3)
    seq6 += 1
    _send_set(pair, seq3, 4, 2, b"\x80")
    seq3 += 1
    _send_set(pair, seq3, 4, 2, b"\x03ab\x00")
    seq3 += 1
    _send_set(pair, seq3, 4, 2, b"\x05ab")
    seq3 += 1
    _send_set(pair, seq3, 4, 2, b"\x08" + b"12345678")
    seq3 += 1
    _send_ack(pair, seq6, 4)
    seq6 += 1
    _send_set(pair, seq3, 5, 2, b"\x02xy")
    seq3 += 1
    assert pair.model.snapshot()["params"][2] == (model.PARAM_STRING, b"xy")
    _send_ack(pair, seq6, 5)
    seq6 += 1
    _send_set(pair, seq3, 6, 9, b"\x00")
    seq3 += 1
    _send_ack(pair, seq6, 6)
    seq6 += 1
    _send_set(pair, seq3, 6, 9, b"\x00")
    _send_set(pair, seq3 + 1, 11, 0, b"\x00")


# covers: REL-1, REL-2, REL-4
def test_pending_retransmit_and_giveup():
    pair = Pair()
    handshake(pair)
    pair.feed_frame(3, 0, bytes((PARAM_SET, 0, 0, 0)))
    pair.feed_frame(3, 1, bytes((PARAM_SET, 1, 0, 1)))
    assert pair.last_model_tx == [model.encode_frame(6, 1, bytes((model.PARAM_CHANNEL, 1)))]
    pair.feed_frame(6, 0, bytes((model.PARAM_CHANNEL, 9)))
    pair.feed_frame(6, 1, bytes((model.PARAM_CHANNEL + 1, 0)))
    pair.feed_frame(6, 2, bytes((model.PARAM_CHANNEL,)))
    pair.feed_frame(6, 3, bytes((model.PARAM_CHANNEL, 0)))
    assert pair.last_model_tx == [model.encode_frame(3, 1, bytes((model.OP_PARAM_STATUS, 1, 0, 0)))]
    pair.advance(499)
    assert pair.last_model_tx == []
    pair.advance(1)
    assert model.encode_frame(3, 2, bytes((model.OP_PARAM_STATUS, 1, 0, 0))) in pair.last_model_tx
    seq4 = 0
    for _ in range(3):
        pair.feed_frame(4, seq4, b"")
        seq4 += 1
        pair.advance(500)
    assert pair.model.snapshot()["counters"][3] == 1
    pair.advance(500)
    assert pair.model.snapshot()["counters"][3] == 1
