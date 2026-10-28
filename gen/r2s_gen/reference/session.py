"""A model of the MCU session: states, timers and counters.

The oracle the model-based tests compare the real C core against
(docs/01-testing.md, section 5.4). It is written from spec/protocol.md
sections 7.1, 7.4 and 8.1, not from the C code, so a mismatch names a bug
in one of the two. It covers what the core implements now: the control
channel, the heartbeat and user dispatch. The time, log, parameters, error
report and reliable-ack channels are accepted and ignored here, as in the
core; P1, P2 and P5 specify their behavior.
"""

from __future__ import annotations

from dataclasses import dataclass

from r2s_gen.reference import crc as crc_mod
from r2s_gen.reference import frames as frames_mod
from r2s_gen.reference import rcobs as rcobs_mod
from r2s_gen.reference import varint as varint_mod
from r2s_gen.reference.errors import Rejected

PROTOCOL_VERSION = 2
HANDSHAKE_MODE_FULL = 0

HELLO_REPEAT_MS = 1000
HELLO_NACK_MS = 100
HEARTBEAT_MS = 500
LINK_TIMEOUT_MS = 2000
SPIN_MAX_FRAMES = 8

ANNOUNCING = 0
CONNECTED = 1

MASK_8 = 0xFF
MASK_16 = 0xFFFF
MASK_32 = 0xFFFFFFFF

OP_HELLO_REQ = 0x01
OP_HELLO = 0x02
OP_TOPIC_REQ = 0x03
OP_TOPIC = 0x04
OP_TOPIC_STATUS = 0x05
OP_READY = 0x06

ERR_NOT_CONNECTED = -1
ERR_TOPIC_REJECTED = -4

STATUS_OK = 0
STATUS_UNKNOWN = 0xFF


@dataclass
class Topic:
    """One announced topic: the CTL-4 flags are the truth for the wire."""

    topic_id: int
    flags: int
    type_hash: bytes
    type_name: str
    topic_name: str
    direction: str = "pub"
    codec_len: int = 0


def encode_frame(topic_id: int, seq: int, payload: bytes) -> bytes:
    """One frame without its delimiter, as the C TX capture holds it."""
    return frames_mod.encode_wire(topic_id, seq, payload)[:-1]


class McuSession:
    """The MCU side of the handshake and the link, in plain Python."""

    def __init__(
        self,
        node: str = "node",
        topics: list[Topic] | None = None,
        max_frame: int = 512,
        tick_hz: int = 1000,
        features: int = 0x02,
    ) -> None:
        self.node = node
        self.topics: list[Topic] = list(topics) if topics is not None else []
        self.max_frame = max_frame
        self.tick_hz = tick_hz
        self.features = features
        self.rx_capacity = max_frame + max_frame // 254 + 1
        self.tick = 0
        self.rx_buf = bytearray()
        self.pending: list[tuple[bytes, int]] = []
        self.tx: list[bytes] = []
        self.state = ANNOUNCING
        self.token = 0
        self.status: list[int] = []
        self.tx_seq: list[int] = []
        self.rx_expected: list[int] = []
        self.rx_lost: list[int] = []
        self.ctrl_seq = 0
        self.hb_seq = 0
        self.counters = {"rx_frames": 0, "crc_errors": 0, "rx_overflows": 0, "dropped": 0}
        self.last_host = 0
        self.last_hello = 0
        self.last_hb = 0
        self.last_nack = 0
        self.heard = False
        self.reset(0)

    def reset(self, tick: int) -> None:
        """A reboot: the token clears and a HELLO goes out (SES-1, SES-10)."""
        self.tick = tick & MASK_32
        self.rx_buf = bytearray()
        self.pending = []
        self.tx = []
        self.state = ANNOUNCING
        self.token = 0
        self.status = [STATUS_UNKNOWN] * len(self.topics)
        self.tx_seq = [0] * len(self.topics)
        self.rx_expected = [0] * len(self.topics)
        self.rx_lost = [0] * len(self.topics)
        self.ctrl_seq = 0
        self.hb_seq = 0
        self.counters = {"rx_frames": 0, "crc_errors": 0, "rx_overflows": 0, "dropped": 0}
        self.last_host = 0
        self.last_hello = self.tick
        self.last_hb = 0
        self.last_nack = (self.tick - HELLO_NACK_MS) & MASK_32
        self.heard = False
        self._send_hello()

    def take_tx(self) -> list[bytes]:
        """The frames queued since the last call, clearing the capture."""
        out = list(self.tx)
        self.tx = []
        return out

    def feed(self, data: bytes) -> None:
        """Bytes from the host; complete units wait for the next spin."""
        for byte in data:
            if byte == 0:
                self.pending.append((bytes(self.rx_buf), self.tick))
                self.rx_buf = bytearray()
            else:
                self.rx_buf.append(byte)

    def spin(self) -> None:
        """Up to SPIN_MAX_FRAMES delivered frames, then the timers."""
        delivered = 0
        while delivered < SPIN_MAX_FRAMES and self.pending:
            encoded, at = self.pending.pop(0)
            if self._accept(encoded, at):
                delivered += 1
        self._timers()

    def advance_to(self, tick: int) -> None:
        """Move the clock, then spin once, as the fake port advance does."""
        self.tick = tick & MASK_32
        self.spin()

    def publish(self, topic_id: int, payload: bytes) -> int:
        """A publish wrapper: status and connection gate it (SES-4, SES-9)."""
        if self.state != CONNECTED:
            return ERR_NOT_CONNECTED
        index = next((i for i, t in enumerate(self.topics) if t.topic_id == topic_id), -1)
        if index < 0 or self.status[index] != STATUS_OK:
            return ERR_TOPIC_REJECTED
        self.tx.append(encode_frame(topic_id, self.tx_seq[index], payload))
        self.tx_seq[index] = (self.tx_seq[index] + 1) & MASK_8
        return 0

    def snapshot(self) -> dict[str, object]:
        """Every observable the tests compare after a step."""
        return {
            "connected": self.state,
            "token": self.token,
            "status": list(self.status),
            "counters": (
                self.counters["rx_frames"] & MASK_16,
                self.counters["crc_errors"] & MASK_16,
                self.counters["rx_overflows"] & MASK_16,
                self.counters["dropped"] & MASK_16,
            ),
            "tx_seq": list(self.tx_seq),
            "rx_lost": list(self.rx_lost),
        }

    def _accept(self, encoded: bytes, at: int) -> bool:
        """One unit through the framer rules; True when a frame is delivered."""
        if not encoded:
            return False
        if len(encoded) > self.rx_capacity:
            self.counters["rx_overflows"] += 1
            self.counters["dropped"] += 1
            return False
        try:
            decoded = rcobs_mod.decode(encoded)
        except Rejected:
            self.counters["dropped"] += 1
            return False
        if len(decoded) < 4 or len(decoded) > self.max_frame:
            self.counters["dropped"] += 1
            return False
        if crc_mod.crc16(decoded) != 0:
            self.counters["crc_errors"] += 1
            self.counters["dropped"] += 1
            return False
        self.counters["rx_frames"] += 1
        self._on_frame(decoded[0], decoded[1], decoded[2:-2], at)
        return True

    def _on_frame(self, topic: int, seq: int, payload: bytes, at: int) -> None:
        self.last_host = at & MASK_32
        if topic == 0:
            self._on_control(payload)
        elif topic == 4:
            if payload:
                self._refuse()
            elif self.state == ANNOUNCING:
                self._answer_with_hello()
        elif topic in (1, 2, 3, 5, 6):
            if self.state == ANNOUNCING:
                self._answer_with_hello()
        elif topic >= 16:
            if self.state == CONNECTED:
                self._dispatch(topic, seq, payload)
            else:
                self._answer_with_hello()
        if self.state == ANNOUNCING:
            self.heard = True

    def _on_control(self, payload: bytes) -> None:
        if not payload:
            self._refuse()
            return
        op = payload[0]
        if op == OP_HELLO_REQ:
            if len(payload) != 2:
                self._refuse()
                return
            self._enter_announcing()
            self._send_hello()
        elif op == OP_TOPIC_REQ:
            if len(payload) != 2 or payload[1] >= len(self.topics):
                self._refuse()
                return
            self._send_topic(payload[1])
        elif op == OP_TOPIC_STATUS:
            if len(payload) != 3 or payload[2] > 3:
                self._refuse()
                return
            index = next((i for i, t in enumerate(self.topics) if t.topic_id == payload[1]), -1)
            if index < 0:
                self._refuse()
                return
            self.status[index] = payload[2]
        elif op == OP_READY:
            if len(payload) != 5:
                self._refuse()
                return
            token = int.from_bytes(payload[1:5], "little")
            if token == 0:
                self._refuse()
                return
            if self.state == ANNOUNCING:
                self.token = token
                self._enter_connected()
        else:
            self._refuse()

    def _dispatch(self, topic_id: int, seq: int, payload: bytes) -> None:
        sub = next((t for t in self.topics if t.topic_id == topic_id and t.direction == "sub"), None)
        if sub is None:
            self._refuse()
            return
        index = self.topics.index(sub)
        if self.status[index] != STATUS_OK:
            self._refuse()
            return
        self.rx_lost[index] += (seq - self.rx_expected[index]) & MASK_8
        self.rx_expected[index] = (seq + 1) & MASK_8
        if len(payload) != sub.codec_len:
            self._refuse()

    def _enter_announcing(self) -> None:
        self.state = ANNOUNCING
        self.heard = False
        self.status = [STATUS_UNKNOWN] * len(self.topics)
        for i in range(len(self.topics)):
            self.tx_seq[i] = 0
            self.rx_expected[i] = 0
        self.last_hello = self.tick
        self.last_nack = (self.tick - HELLO_NACK_MS) & MASK_32

    def _enter_connected(self) -> None:
        self.state = CONNECTED
        self.ctrl_seq = 0
        self.hb_seq = 0
        self.last_hb = self.tick

    def _answer_with_hello(self) -> None:
        if (self.tick - self.last_nack) & MASK_32 >= HELLO_NACK_MS:
            self.last_nack = self.tick
            self._send_hello()

    def _refuse(self) -> None:
        self.counters["dropped"] += 1

    def _send_hello(self) -> None:
        name = self.node.encode()
        payload = (
            bytes((OP_HELLO, PROTOCOL_VERSION, HANDSHAKE_MODE_FULL))
            + self.token.to_bytes(4, "little")
            + self.max_frame.to_bytes(2, "little")
            + self.rx_capacity.to_bytes(2, "little")
            + self.tick_hz.to_bytes(4, "little")
            + bytes((self.features, len(self.topics)))
            + varint_mod.encode(len(name))
            + name
        )
        self.tx.append(encode_frame(0, self.ctrl_seq, payload))
        self.ctrl_seq = (self.ctrl_seq + 1) & MASK_8

    def _send_topic(self, index: int) -> None:
        topic = self.topics[index]
        type_name = topic.type_name.encode()
        topic_name = topic.topic_name.encode()
        payload = (
            bytes((OP_TOPIC, index, topic.topic_id, topic.flags))
            + topic.type_hash
            + varint_mod.encode(len(type_name))
            + type_name
            + varint_mod.encode(len(topic_name))
            + topic_name
        )
        self.tx.append(encode_frame(0, self.ctrl_seq, payload))
        self.ctrl_seq = (self.ctrl_seq + 1) & MASK_8

    def _send_heartbeat(self) -> None:
        payload = (
            (self.counters["rx_frames"] & MASK_16).to_bytes(2, "little")
            + (self.counters["crc_errors"] & MASK_16).to_bytes(2, "little")
            + (self.counters["rx_overflows"] & MASK_16).to_bytes(2, "little")
            + (self.counters["dropped"] & MASK_16).to_bytes(2, "little")
        )
        self.tx.append(encode_frame(4, self.hb_seq, payload))
        self.hb_seq = (self.hb_seq + 1) & MASK_8

    def _timers(self) -> None:
        if self.state == CONNECTED and (self.tick - self.last_host) & MASK_32 >= LINK_TIMEOUT_MS:
            self._enter_announcing()
        if self.state == ANNOUNCING:
            if not self.heard and (self.tick - self.last_hello) & MASK_32 >= HELLO_REPEAT_MS:
                self.last_hello = self.tick
                self._send_hello()
        elif (self.tick - self.last_hb) & MASK_32 >= HEARTBEAT_MS:
            self.last_hb = self.tick
            self._send_heartbeat()
