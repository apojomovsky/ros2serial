# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""A model of the MCU session: states, timers, counters and error reports.

The oracle the model-based tests compare the real C core against
(docs/01-testing.md, section 5.4). It is written from spec/protocol.md
sections 7.1, 7.4, 7.5, 7.6, 7.7 and 8.1, not from the C code, so a mismatch
names a bug in one of the two. It covers what the core implements now: the
control channel, the heartbeat, user dispatch, the error report channel,
the parameters channel and the reliable channel. The time and log channels
are accepted and ignored here, as in the core; P1 and P2 specify their
behavior.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from r2s_gen.reference import crc as crc_mod
from r2s_gen.reference import frames as frames_mod
from r2s_gen.reference import rcobs as rcobs_mod
from r2s_gen.reference import varint as varint_mod
from r2s_gen.reference.errors import Rejected

PROTOCOL_VERSION = 4
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

OP_PARAM_REQ = 0x01
OP_PARAM_DECLARE = 0x02
OP_PARAM_SET = 0x03
OP_PARAM_STATUS = 0x04

PARAM_BOOL = 0
PARAM_I64 = 1
PARAM_F64 = 2
PARAM_STRING = 3

PARAM_READONLY = 0x01

PARAM_APPLIED = 0
PARAM_UNKNOWN_INDEX = 1
PARAM_BAD_TYPE_OR_LENGTH = 2
PARAM_READONLY_STATUS = 3
PARAM_OUT_OF_RANGE = 4

PARAM_CHANNEL = 3
ACK_CHANNEL = 6

REL_RETRY_MS = 500
REL_RETRIES = 3

ERROR_TOPIC = 5

ERR_BOUND = 1
ERR_UNKNOWN_TOPIC = 2
ERR_RX_OVERFLOW = 3
ERR_FRAME_TOO_LARGE = 4
ERR_NOT_CONNECTED_CODE = 5
ERR_TX_BUSY = 6

BUSY_WINDOW_MS = 1000


@dataclass
class Topic:
    """One announced topic: the CTL-4 flags are the truth for the wire.

    A nonzero bound marks a bounded-string subscription, whose payloads
    the model checks against the bound instead of a fixed width.
    """

    topic_id: int
    flags: int
    type_hash: bytes
    type_name: str
    topic_name: str
    direction: str = 'pub'
    codec_len: int = 0
    bound: int = 0


@dataclass
class Param:
    """One parameter: the PAR-2 type, the PAR-4 flags, the string bound and
    the default the DECLARE carries. The value is the live one the last
    applied SET left.
    """

    name: str
    type: int
    flags: int
    bound: int
    initial: object
    value: object


def encode_frame(topic_id: int, seq: int, payload: bytes) -> bytes:
    """One frame without its delimiter, as the C TX capture holds it."""
    return frames_mod.encode_wire(topic_id, seq, payload)[:-1]


class McuSession:
    """The MCU side of the handshake and the link, in plain Python."""

    def __init__(
        self,
        node: str = 'node',
        topics: list[Topic] | None = None,
        max_frame: int = 512,
        tick_hz: int = 1000,
        features: int = 0x02,
        params: list[Param] | None = None,
    ) -> None:
        self.node = node
        self.topics: list[Topic] = list(topics) if topics is not None else []
        self.max_frame = max_frame
        self.tick_hz = tick_hz
        self.features = features
        self.params: list[Param] = list(params) if params is not None else []
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
        self.param_seq = 0
        self.ack_seq = 0
        self.rx_rel_expected = 0
        self.tx_rel_next = 0
        self.rel_outstanding: bytes | None = None
        self.rel_outstanding_rel = 0
        self.rel_deadline = 0
        self.rel_retries = 0
        self.rel_pending: bytes | None = None
        self.rel_pending_rel = 0
        self.counters = {'rx_frames': 0, 'crc_errors': 0, 'rx_overflows': 0, 'dropped': 0}
        self.err_pending = [0] * 7
        self.err_topic = [0xFF] * 7
        self.err_field = [0xFF] * 7
        self.err_conn_reported = False
        self.err_busy_last = 0
        self.err_seq = 0
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
        for param in self.params:
            param.value = param.initial
        self.ctrl_seq = 0
        self.hb_seq = 0
        self.param_seq = 0
        self.ack_seq = 0
        self._drop_reliable()
        self.counters = {'rx_frames': 0, 'crc_errors': 0, 'rx_overflows': 0, 'dropped': 0}
        self.err_pending = [0] * 7
        self.err_topic = [0xFF] * 7
        self.err_field = [0xFF] * 7
        self.err_conn_reported = False
        self.err_busy_last = 0
        self.err_seq = 0
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
        """Up to SPIN_MAX_FRAMES delivered frames, then the error flush and timers."""
        delivered = 0
        refused: list[bytes] = []
        while delivered < SPIN_MAX_FRAMES and self.pending:
            encoded, at = self.pending.pop(0)
            if self._accept(encoded, at):
                delivered += 1
            else:
                refused.append(encoded)
        self._note_framer_errors(refused)
        self._flush_errors()
        self._timers()

    def _note_framer_errors(self, refused: list[bytes]) -> None:
        """Note the refused units the framer reports at the end of a spin.

        An oversized unit never decodes, so it names no topic; a decoded
        unit past the frame limit names its topic, last one wins.
        """
        overflow = 0
        too_large = 0
        too_large_topic = 0xFF
        for encoded in refused:
            if len(encoded) > self.rx_capacity:
                overflow += 1
                continue
            try:
                decoded = rcobs_mod.decode(encoded)
            except Rejected:
                continue
            if len(decoded) > self.max_frame:
                too_large += 1
                too_large_topic = decoded[0]
        if overflow:
            self._note_error(ERR_RX_OVERFLOW, 0xFF, 0xFF, overflow)
        if too_large:
            self._note_error(ERR_FRAME_TOO_LARGE, too_large_topic, 0xFF, too_large)

    def advance_to(self, tick: int) -> None:
        """Move the clock, then spin once, as the fake port advance does."""
        self.tick = tick & MASK_32
        self.spin()

    def publish(self, topic_id: int, payload: bytes) -> int:
        """A publish wrapper: status and connection gate it (SES-4, SES-9)."""
        if self.state != CONNECTED:
            self._note_error(ERR_NOT_CONNECTED_CODE, topic_id, 0xFF, 1)
            return ERR_NOT_CONNECTED
        index = next((i for i, t in enumerate(self.topics) if t.topic_id == topic_id), -1)
        if index < 0 or self.status[index] != STATUS_OK:
            return ERR_TOPIC_REJECTED
        self.tx.append(encode_frame(topic_id, self.tx_seq[index], payload))
        self.tx_seq[index] = (self.tx_seq[index] + 1) & MASK_8
        return 0

    def _note_error(self, code: int, topic: int, field: int, count: int) -> None:
        """Count an error, then send what is due (spec/protocol.md, ERR-6)."""
        if not 1 <= code <= 6:
            return
        self.err_pending[code] = (self.err_pending[code] + count) & MASK_16
        self.err_topic[code] = topic
        self.err_field[code] = field
        self._flush_errors()

    def _flush_errors(self) -> None:
        """Send every pending report the rate limits allow (ERR-3)."""
        for code in range(1, 7):
            if not self.err_pending[code] or not self._error_due(code):
                continue
            payload = bytes(
                (
                    code,
                    self.err_topic[code],
                    self.err_pending[code] & 0xFF,
                    self.err_pending[code] >> 8,
                    self.err_field[code],
                )
            )
            self.tx.append(encode_frame(ERROR_TOPIC, self.err_seq, payload))
            self.err_seq = (self.err_seq + 1) & MASK_8
            self.err_pending[code] = 0
            if code == ERR_NOT_CONNECTED_CODE:
                self.err_conn_reported = True
            if code == ERR_TX_BUSY:
                self.err_busy_last = self.tick

    def _error_due(self, code: int) -> bool:
        """Whether a pending report for `code` may go out now (ERR-3)."""
        if code == ERR_TX_BUSY:
            window = BUSY_WINDOW_MS * self.tick_hz // 1000
            return (self.tick - self.err_busy_last) & MASK_32 >= window
        if code == ERR_NOT_CONNECTED_CODE:
            return not self.err_conn_reported
        return True

    def _note_connected(self) -> None:
        """Restart the report sequence; a new connection may report code 5 again."""
        self.err_seq = 0
        self.err_conn_reported = False
        self._flush_errors()

    def snapshot(self) -> dict[str, object]:
        """Every observable the tests compare after a step."""
        return {
            'connected': self.state,
            'token': self.token,
            'status': list(self.status),
            'counters': (
                self.counters['rx_frames'] & MASK_16,
                self.counters['crc_errors'] & MASK_16,
                self.counters['rx_overflows'] & MASK_16,
                self.counters['dropped'] & MASK_16,
            ),
            'tx_seq': list(self.tx_seq),
            'rx_lost': list(self.rx_lost),
            'params': [(param.type, param.value) for param in self.params],
        }

    def _accept(self, encoded: bytes, at: int) -> bool:
        """One unit through the framer rules; True when a frame is delivered."""
        if not encoded:
            return False
        if len(encoded) > self.rx_capacity:
            self.counters['rx_overflows'] += 1
            self.counters['dropped'] += 1
            return False
        try:
            decoded = rcobs_mod.decode(encoded)
        except Rejected:
            self.counters['dropped'] += 1
            return False
        if len(decoded) < 4 or len(decoded) > self.max_frame:
            self.counters['dropped'] += 1
            return False
        if crc_mod.crc16(decoded) != 0:
            self.counters['crc_errors'] += 1
            self.counters['dropped'] += 1
            return False
        self.counters['rx_frames'] += 1
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
        elif topic == PARAM_CHANNEL:
            self._on_params(payload)
        elif topic == ACK_CHANNEL:
            self._on_ack(payload)
        elif topic in (1, 2, 5):
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
                self._note_error(ERR_UNKNOWN_TOPIC, payload[1], 0xFF, 1)
                return
            self.status[index] = payload[2]
        elif op == OP_READY:
            if len(payload) != 5:
                self._refuse()
                return
            token = int.from_bytes(payload[1:5], 'little')
            if token == 0:
                self._refuse()
                return
            if self.state == ANNOUNCING:
                self.token = token
                self._enter_connected()
        else:
            self._refuse()

    def _dispatch(self, topic_id: int, seq: int, payload: bytes) -> None:
        sub = next(
            (t for t in self.topics if t.topic_id == topic_id and t.direction == 'sub'), None
        )
        if sub is None:
            if not any(t.topic_id == topic_id for t in self.topics):
                self._note_error(ERR_UNKNOWN_TOPIC, topic_id, 0xFF, 1)
            self._refuse()
            return
        index = self.topics.index(sub)
        if self.status[index] != STATUS_OK:
            self._refuse()
            return
        self.rx_lost[index] += (seq - self.rx_expected[index]) & MASK_8
        self.rx_expected[index] = (seq + 1) & MASK_8
        if sub.bound:
            self._dispatch_string(topic_id, sub.bound, payload)
        elif len(payload) != sub.codec_len:
            self._refuse()

    def _dispatch_string(self, topic_id: int, bound: int, payload: bytes) -> None:
        """Check a bounded-string payload: over the bound reports, else accepts.

        A length the varint cannot carry, short bytes or a zero byte breaks
        an encoding rule and refuses silently; only a length past the bound
        is a bound violation (ERR-1) with the string as field 0.
        """
        try:
            length, used = varint_mod.decode(payload)
        except Rejected:
            self._refuse()
            return
        if length > bound:
            self._note_error(ERR_BOUND, topic_id, 0, 1)
            self._refuse()
            return
        rest = payload[used:]
        if len(rest) != length or any(byte == 0 for byte in rest):
            self._refuse()

    def _default_bytes(self, param: Param) -> bytes:
        """The DECLARE default for one parameter (PAR-1, PAR-3)."""
        if param.type == PARAM_BOOL:
            return bytes((param.initial,))
        if param.type == PARAM_I64:
            return int(param.initial).to_bytes(8, 'little', signed=True)
        if param.type == PARAM_F64:
            return struct.pack('<d', param.initial)
        encoded = bytes(param.initial)
        return varint_mod.encode(len(encoded)) + encoded

    def _check_value(self, param: Param, value: bytes) -> int | None:
        """The STATUS verdict for a SET value, or None when the value breaks
        its type's rule and the frame is refused (PAR-3, PAR-5). Unknown
        index, length, read-only and range report 1, 2, 3 and 4 in order.
        """
        if param.type == PARAM_BOOL:
            if len(value) != 1:
                return PARAM_BAD_TYPE_OR_LENGTH
            if value[0] > 1:
                return None
        elif param.type in (PARAM_I64, PARAM_F64):
            if len(value) != 8:
                return PARAM_BAD_TYPE_OR_LENGTH
        else:
            try:
                length, used = varint_mod.decode(value)
            except Rejected:
                return None
            rest = value[used:]
            if len(rest) != length or any(byte == 0 for byte in rest):
                return None
            if length > param.bound:
                return PARAM_OUT_OF_RANGE
        if param.flags & PARAM_READONLY:
            return PARAM_READONLY_STATUS
        return PARAM_APPLIED

    def _store(self, index: int, value: bytes) -> None:
        """Keep the applied SET value as the live one."""
        param = self.params[index]
        if param.type == PARAM_BOOL:
            param.value = value[0]
        elif param.type == PARAM_I64:
            param.value = int.from_bytes(value, 'little', signed=True)
        elif param.type == PARAM_F64:
            param.value = struct.unpack('<d', value)[0]
        else:
            length, used = varint_mod.decode(value)
            param.value = bytes(value[used : used + length])

    def _send_declare(self, index: int) -> None:
        """One plain DECLARE answering a PARAM_REQ (PAR-7, PAR-8)."""
        param = self.params[index]
        name = param.name.encode()
        payload = (
            bytes((OP_PARAM_DECLARE, index, param.type, param.flags))
            + varint_mod.encode(len(name))
            + name
            + self._default_bytes(param)
        )
        self.tx.append(encode_frame(PARAM_CHANNEL, self.param_seq, payload))
        self.param_seq = (self.param_seq + 1) & MASK_8

    def _send_plain_status(self, index: int, status: int) -> None:
        """One plain STATUS answering a PARAM_REQ past the table (PAR-7,
        PAR-8): under the request retry, never a reliable frame, rel_seq 0.
        """
        payload = bytes((OP_PARAM_STATUS, 0, index, status))
        self.tx.append(encode_frame(PARAM_CHANNEL, self.param_seq, payload))
        self.param_seq = (self.param_seq + 1) & MASK_8

    def _send_ack(self, channel: int, rel: int) -> None:
        """One ack for the reliable frame received (REL-1, REL-3)."""
        self.tx.append(encode_frame(ACK_CHANNEL, self.ack_seq, bytes((channel, rel))))
        self.ack_seq = (self.ack_seq + 1) & MASK_8

    def _queue_status(self, index: int, status: int) -> None:
        """Hold the STATUS for the retransmit timer: at most one reliable
        payload is outstanding per direction (REL-2). The STATUS carries the
        next rel_seq of this direction's own stream, which moves in lockstep
        with the host's under stop-and-wait. A second applied SET waits
        behind the first.
        """
        rel = self.tx_rel_next
        self.tx_rel_next = (self.tx_rel_next + 1) & MASK_8
        payload = bytes((OP_PARAM_STATUS, rel, index, status))
        if self.rel_outstanding is None:
            self.rel_outstanding = payload
            self.rel_outstanding_rel = rel
            self._send_status()
        else:
            self.rel_pending = payload
            self.rel_pending_rel = rel

    def _send_status(self) -> None:
        """The outstanding STATUS goes out with the next frame seq (REL-4)."""
        assert self.rel_outstanding is not None
        self.tx.append(encode_frame(PARAM_CHANNEL, self.param_seq, self.rel_outstanding))
        self.param_seq = (self.param_seq + 1) & MASK_8
        self.rel_deadline = self.tick
        self.rel_retries = 0

    def _promote_status(self) -> None:
        """The pending STATUS becomes outstanding and goes out at once."""
        self.rel_outstanding = self.rel_pending
        self.rel_outstanding_rel = self.rel_pending_rel
        self.rel_pending = None
        if self.rel_outstanding is not None:
            self._send_status()

    def _on_params(self, payload: bytes) -> None:
        """One channel 3 frame: a plain REQ answer in ANNOUNCING (PAR-10),
        a reliable SET exchange while connected (PAR-8, REL-1 to REL-4). A
        SET in ANNOUNCING is ignored like any other non-handshake frame.
        """
        if self.state == ANNOUNCING:
            if len(payload) == 2 and payload[0] == OP_PARAM_REQ:
                index = payload[1]
                if index < len(self.params):
                    self._send_declare(index)
                else:
                    self._send_plain_status(index, PARAM_UNKNOWN_INDEX)
            else:
                self._answer_with_hello()
            return
        if self.state != CONNECTED:  # pragma: no cover - only ANNOUNCING and CONNECTED exist
            return
        if len(payload) >= 3 and payload[0] == OP_PARAM_SET:
            rel, index = payload[1], payload[2]
            if index >= len(self.params):
                self._send_ack(PARAM_CHANNEL, rel)
                if rel == self.rx_rel_expected:
                    self.rx_rel_expected = (self.rx_rel_expected + 1) & MASK_8
                    self._queue_status(index, PARAM_UNKNOWN_INDEX)
                return
            code = self._check_value(self.params[index], payload[3:])
            if code is None:
                self._refuse()
                return
            self._send_ack(PARAM_CHANNEL, rel)
            if rel != self.rx_rel_expected:
                return
            self.rx_rel_expected = (self.rx_rel_expected + 1) & MASK_8
            if code == PARAM_APPLIED:
                self._store(index, payload[3:])
            self._queue_status(index, code)
        else:
            self._refuse()

    def _on_ack(self, payload: bytes) -> None:
        """One channel 6 frame: a stale ack in ANNOUNCING is answered like
        any other non-handshake frame; a matching one while connected clears
        the outstanding STATUS (REL-1, REL-2, REL-4).
        """
        if self.state == ANNOUNCING:
            self._answer_with_hello()
            return
        if self.state != CONNECTED or len(payload) != 2:
            return
        channel, rel = payload[0], payload[1]
        if (
            channel == PARAM_CHANNEL
            and self.rel_outstanding is not None
            and rel == self.rel_outstanding_rel
        ):
            self.rel_outstanding = None
            self._promote_status()

    def _drop_reliable(self) -> None:
        """A re-handshake drops all reliable state (REL-5)."""
        self.rx_rel_expected = 0
        self.tx_rel_next = 0
        self.rel_outstanding = None
        self.rel_pending = None
        self.rel_retries = 0

    def _enter_announcing(self) -> None:
        self.state = ANNOUNCING
        self.heard = False
        self.status = [STATUS_UNKNOWN] * len(self.topics)
        for i in range(len(self.topics)):
            self.tx_seq[i] = 0
            self.rx_expected[i] = 0
        self.param_seq = 0
        self.ack_seq = 0
        self._drop_reliable()
        self.last_hello = self.tick
        self.last_nack = (self.tick - HELLO_NACK_MS) & MASK_32

    def _enter_connected(self) -> None:
        self.state = CONNECTED
        self.ctrl_seq = 0
        self.hb_seq = 0
        self.param_seq = 0
        self.ack_seq = 0
        self._drop_reliable()
        self.last_hb = self.tick
        self._note_connected()

    def _answer_with_hello(self) -> None:
        if (self.tick - self.last_nack) & MASK_32 >= HELLO_NACK_MS:
            self.last_nack = self.tick
            self._send_hello()

    def _refuse(self) -> None:
        self.counters['dropped'] += 1

    def _send_hello(self) -> None:
        name = self.node.encode()
        payload = (
            bytes((OP_HELLO, PROTOCOL_VERSION, HANDSHAKE_MODE_FULL))
            + self.token.to_bytes(4, 'little')
            + self.max_frame.to_bytes(2, 'little')
            + self.rx_capacity.to_bytes(2, 'little')
            + self.tick_hz.to_bytes(4, 'little')
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
            (self.counters['rx_frames'] & MASK_16).to_bytes(2, 'little')
            + (self.counters['crc_errors'] & MASK_16).to_bytes(2, 'little')
            + (self.counters['rx_overflows'] & MASK_16).to_bytes(2, 'little')
            + (self.counters['dropped'] & MASK_16).to_bytes(2, 'little')
        )
        self.tx.append(encode_frame(4, self.hb_seq, payload))
        self.hb_seq = (self.hb_seq + 1) & MASK_8

    def _retransmit_status(self) -> None:
        """The 500 ms timer: resend the outstanding STATUS, or drop it and
        count it after 3 retries (REL-2). A pending STATUS moves up either
        way, so an applied SET is never left unreported.
        """
        if (self.tick - self.rel_deadline) & MASK_32 < REL_RETRY_MS:
            return
        assert self.rel_outstanding is not None
        if self.rel_retries >= REL_RETRIES:
            self.rel_outstanding = None
            self.counters['dropped'] += 1
            self._promote_status()
            return
        self.rel_retries += 1
        self.rel_deadline = self.tick
        self.tx.append(encode_frame(PARAM_CHANNEL, self.param_seq, self.rel_outstanding))
        self.param_seq = (self.param_seq + 1) & MASK_8

    def _timers(self) -> None:
        if self.state == CONNECTED and (self.tick - self.last_host) & MASK_32 >= LINK_TIMEOUT_MS:
            self._enter_announcing()
        if self.state == ANNOUNCING:
            if not self.heard and (self.tick - self.last_hello) & MASK_32 >= HELLO_REPEAT_MS:
                self.last_hello = self.tick
                self._send_hello()
        else:
            if self.rel_outstanding is not None:
                self._retransmit_status()
            if (self.tick - self.last_hb) & MASK_32 >= HEARTBEAT_MS:
                self.last_hb = self.tick
                self._send_heartbeat()
