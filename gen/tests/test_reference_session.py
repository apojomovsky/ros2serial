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

"""The session model against hand-computed bytes and boundaries.

These tests pin the model itself, without the C core: the HELLO payload is
written out by hand, and every timer fires on its specified millisecond. The
model-based tests then compare the core against this model.
"""

import struct

import pytest
from r2s_gen.reference import frames as frames_mod
from r2s_gen.reference import session as model
from session_harness import TOPIC_IDS
from session_harness import model_params
from session_harness import model_topics
from session_harness import wire

FLAGS = [0x00, 0x01, 0x00, 0x01]
TOPICS = model_topics(FLAGS)

# The boot HELLO for node "node" with the four topics above, written by
# hand from section 7.1: opcode 02, version 4, full mode, zero token,
# max_frame 512, rx_capacity 515, tick_hz 1000, features 0x02, count 4,
# then the varint length and "node".
HELLO_PAYLOAD = bytes.fromhex('02 04 00 00 00 00 00 00 02 03 02 e8 03 00 00 02 04 04 6e 6f 64 65')


def fresh() -> model.McuSession:
    """A model on the handwritten table, booted at tick 0 with TX drained."""
    session = model.McuSession(topics=list(TOPICS))
    session.reset(0)
    assert session.take_tx() == [model.encode_frame(0, 0, HELLO_PAYLOAD)]
    return session


def connect(session: model.McuSession, token: int = 0xAABBCCDD) -> None:
    """Run the handshake to CONNECTED, draining the TX after each step."""
    session.feed(wire(0, 0, bytes((model.OP_HELLO_REQ, model.PROTOCOL_VERSION))))
    session.spin()
    session.take_tx()
    for index in range(4):
        session.feed(wire(0, 1 + index, bytes((model.OP_TOPIC_REQ, index))))
        session.spin()
        session.take_tx()
        session.feed(wire(0, 5 + index, bytes((model.OP_TOPIC_STATUS, TOPIC_IDS[index], 0))))
        session.spin()
        assert session.take_tx() == []
    session.feed(wire(0, 9, bytes((model.OP_READY,)) + token.to_bytes(4, 'little')))
    session.spin()
    session.take_tx()


# covers: SES-1, SES-10
def test_boot_sends_hello_with_zero_token():
    fresh()


# covers: SES-2, SES-6, SES-17
def test_handshake_reaches_connected_with_token():
    session = fresh()
    connect(session)
    assert session.snapshot()['connected'] == model.CONNECTED
    assert session.snapshot()['token'] == 0xAABBCCDD
    assert session.snapshot()['status'] == [0, 0, 0, 0]


# The DECLARE for the led parameter, written by hand from section 7.6:
# opcode 02, index 0, bool, no flags, name "led", default true.
DECLARE_LED = bytes.fromhex('02 00 00 00 03 6c 65 64 01')


# covers: PAR-1, PAR-2, PAR-6
def test_declare_answers_param_req_with_handwritten_bytes():
    session = model.McuSession(topics=list(TOPICS), params=model_params())
    session.reset(0)
    session.take_tx()
    session.feed(wire(3, 0, bytes((model.OP_PARAM_REQ, 0))))
    session.spin()
    assert session.take_tx() == [model.encode_frame(3, 0, DECLARE_LED)]


# covers: SES-10
def test_ready_with_zero_token_is_refused():
    session = fresh()
    session.feed(wire(0, 0, bytes((model.OP_HELLO_REQ, model.PROTOCOL_VERSION))))
    session.spin()
    session.take_tx()
    session.feed(wire(0, 1, bytes((model.OP_READY, 0, 0, 0, 0))))
    session.spin()
    assert session.take_tx() == []
    assert session.snapshot()['connected'] == model.ANNOUNCING
    assert session.snapshot()['token'] == 0


# covers: SES-10
def test_hello_echoes_the_last_ready_token():
    session = fresh()
    connect(session, token=0x01020304)
    session.feed(wire(0, 9, bytes((model.OP_HELLO_REQ, model.PROTOCOL_VERSION))))
    session.spin()
    hello = session.take_tx()
    assert len(hello) == 1
    assert hello[0][5:9] == bytes.fromhex('04 03 02 01')


# covers: SES-4, ERR-2, ERR-3
def test_publish_while_announcing_fails():
    session = fresh()
    assert session.publish(16, struct.pack('<f', 1.5)) == model.ERR_NOT_CONNECTED
    assert session.take_tx() == [model.encode_frame(5, 0, bytes((5, 16, 1, 0, 0xFF)))]


# covers: SES-9
def test_publish_after_connect_sends_a_frame():
    session = fresh()
    connect(session)
    payload = struct.pack('<f', 1.5)
    assert session.publish(16, payload) == 0
    assert session.take_tx() == [model.encode_frame(16, 0, payload)]
    assert session.snapshot()['tx_seq'] == [1, 0, 0, 0]


# covers: SES-7, SES-8
def test_link_timeout_returns_to_announcing():
    session = fresh()
    connect(session)
    session.advance_to(1999)
    assert session.snapshot()['connected'] == model.CONNECTED
    session.advance_to(2000)
    assert session.snapshot()['connected'] == model.ANNOUNCING
    assert session.snapshot()['status'] == [model.STATUS_UNKNOWN] * 4


# covers: SES-9, SES-18, HB-2, HB-4
def test_heartbeat_carries_the_counters():
    session = fresh()
    connect(session)
    session.advance_to(499)
    assert session.take_tx() == []
    session.advance_to(500)
    counters = struct.pack('<HHHH', 10, 0, 0, 0)
    assert session.take_tx() == [model.encode_frame(4, 0, counters)]


# covers: PAR-1, PAR-3
def test_declare_and_set_cover_float_and_writable_int():
    params = [
        model.Param('gain', model.PARAM_F64, 0, 0, 1.5, 1.5),
        model.Param('count', model.PARAM_I64, 0, 0, 7, 7),
    ]
    session = model.McuSession(topics=list(TOPICS), params=params)
    session.reset(0)
    session.take_tx()
    session.feed(wire(3, 0, bytes((model.OP_PARAM_REQ, 0))))
    session.spin()
    assert session.take_tx() == [
        model.encode_frame(
            3,
            0,
            bytes((model.OP_PARAM_DECLARE, 0, model.PARAM_F64, 0, 4))
            + b'gain'
            + struct.pack('<d', 1.5),
        )
    ]
    session.feed(wire(3, 1, bytes((model.OP_PARAM_REQ, 1))))
    session.spin()
    session.take_tx()
    connect(session)
    session.feed(wire(3, 0, bytes((model.OP_PARAM_SET, 0, 0)) + struct.pack('<d', 2.5)))
    session.spin()
    assert session.take_tx() == [
        model.encode_frame(6, 0, bytes((model.PARAM_CHANNEL, 0))),
        model.encode_frame(3, 0, bytes((model.OP_PARAM_STATUS, 0, 0, model.PARAM_APPLIED))),
    ]
    assert session.snapshot()['params'] == [(model.PARAM_F64, 2.5), (model.PARAM_I64, 7)]
    session.feed(
        wire(3, 1, bytes((model.OP_PARAM_SET, 1, 1)) + (8).to_bytes(8, 'little', signed=True))
    )
    session.spin()
    assert session.snapshot()['params'] == [(model.PARAM_F64, 2.5), (model.PARAM_I64, 8)]


# covers: SES-5
def test_hello_repeats_after_one_silent_second():
    session = fresh()
    session.advance_to(999)
    assert session.take_tx() == []
    session.advance_to(1000)
    assert session.take_tx() == [model.encode_frame(0, 1, HELLO_PAYLOAD)]


# covers: SES-3
def test_second_unexpected_frame_inside_100ms_sends_no_hello():
    session = fresh()
    session.feed(wire(17, 0, bytes(48)))
    session.spin()
    first = session.take_tx()
    assert len(first) == 1
    session.feed(wire(17, 1, bytes(48)))
    session.spin()
    assert session.take_tx() == []


# covers: FRM-8, HB-5
def test_bad_crc_counts_a_crc_error():
    session = fresh()
    good = bytearray(frames_mod.encode_wire(4, 0, b''))
    good[2] ^= 0xFF
    session.feed(bytes(good) + b'\x00')
    session.spin()
    assert session.snapshot()['counters'] == (0, 1, 0, 1)


# covers: FRM-12, HB-6, ERR-2, ERR-6
def test_oversized_unit_counts_an_overflow():
    session = fresh()
    session.feed(bytes([0x41]) * 516 + b'\x00')
    session.spin()
    assert session.snapshot()['counters'] == (0, 0, 1, 1)
    assert session.take_tx() == [model.encode_frame(5, 0, bytes((3, 0xFF, 1, 0, 0xFF)))]


# covers: FRM-14
def test_empty_frame_is_ignored():
    session = fresh()
    session.feed(b'\x00')
    session.spin()
    assert session.snapshot()['counters'] == (0, 0, 0, 0)
    assert session.take_tx() == []


# covers: FRM-15
def test_seq_gap_counts_lost_frames():
    session = fresh()
    connect(session)
    session.feed(wire(17, 5, bytes(48)))
    session.spin()
    assert session.snapshot()['rx_lost'] == [0, 5, 0, 0]
    session.feed(wire(17, 6, bytes(48)))
    session.spin()
    assert session.snapshot()['rx_lost'] == [0, 5, 0, 0]


# covers: ERR-2, ERR-6
def test_unknown_topic_dispatch_reports_code_2():
    session = fresh()
    connect(session)
    session.feed(wire(20, 0, b'\x00'))
    session.spin()
    assert session.take_tx() == [model.encode_frame(5, 0, bytes((2, 20, 1, 0, 0xFF)))]


# covers: ERR-2
def test_published_topic_frame_is_refused_without_a_report():
    session = fresh()
    connect(session)
    session.feed(wire(16, 0, bytes(8)))
    session.spin()
    assert session.snapshot()['counters'] == (11, 0, 0, 1)
    assert session.take_tx() == []


# covers: ERR-1, ERR-2, ERR-4, ERR-6
def test_bound_violation_reports_code_1_with_its_field():
    session = fresh()
    connect(session)
    session.feed(wire(19, 0, bytes((9,)) + b'abcdefghi'))
    session.spin()
    assert session.take_tx() == [model.encode_frame(5, 0, bytes((1, 19, 1, 0, 0)))]


# covers: ERR-3, ERR-6
def test_not_connected_reports_once_per_connection():
    session = fresh()
    payload = struct.pack('<f', 1.5)
    assert session.publish(16, payload) == model.ERR_NOT_CONNECTED
    assert session.take_tx() == [model.encode_frame(5, 0, bytes((5, 16, 1, 0, 0xFF)))]
    assert session.publish(16, payload) == model.ERR_NOT_CONNECTED
    assert session.take_tx() == []
    session.feed(wire(0, 10, bytes((model.OP_HELLO_REQ, 2))))
    session.spin()
    session.take_tx()
    for index in range(4):
        session.feed(wire(0, 11 + index, bytes((model.OP_TOPIC_REQ, index))))
        session.spin()
        session.take_tx()
        session.feed(wire(0, 15 + index, bytes((model.OP_TOPIC_STATUS, TOPIC_IDS[index], 0))))
        session.spin()
        session.take_tx()
    session.feed(wire(0, 19, bytes((model.OP_READY,)) + (0xAABBCCDD).to_bytes(4, 'little')))
    session.spin()
    assert session.take_tx() == [model.encode_frame(5, 0, bytes((5, 16, 1, 0, 0xFF)))]


# covers: ERR-2, ERR-6
def test_frame_too_large_reports_code_4_with_its_topic():
    session = fresh()
    connect(session)
    session.feed(wire(17, 0, bytes(509)))
    session.spin()
    assert session.snapshot()['counters'] == (10, 0, 0, 1)
    assert session.take_tx() == [model.encode_frame(5, 0, bytes((4, 17, 1, 0, 0xFF)))]


# covers: SES-8
def test_timeout_survives_the_tick_wraparound():
    session = fresh()
    connect(session)
    session.tick = 0xFFFFFF00
    session.last_host = 0xFFFFFF00
    session.last_hb = 0xFFFFFF00
    session.advance_to(0x100)
    assert session.snapshot()['connected'] == model.CONNECTED
    session.advance_to(0xFFFFFF00 + 2000)
    assert session.snapshot()['connected'] == model.ANNOUNCING


@pytest.mark.parametrize('opcode', [0x00, 0x07, 0x09, 0xFF])
# covers: SYS-5
def test_unknown_control_opcode_is_refused(opcode):
    session = fresh()
    session.feed(wire(0, 0, bytes((opcode, 0x01))))
    session.spin()
    assert session.snapshot()['counters'] == (1, 0, 0, 1)
    assert session.take_tx() == []
