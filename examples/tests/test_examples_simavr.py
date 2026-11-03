"""Hello and blink on an emulated Uno: the two task sketches that need no hardware.

The firmware registers its topics while it runs, the handshake announces
exactly those, hello publishes strings within its bound and a Bool the host
sends reaches blink without an error report. Assertions stay
bounds-with-margin: lengths against the bound less a margin, never exact
golden bytes, so the test proves the link works without pinning content.

covers: SES-1, SES-2
"""

import time

import pytest
import serial
from r2s_gen.reference import frames
from test_announce import drain, read_string, read_unit, read_varint
from test_echo import Harness

PROTOCOL_VERSION = 2
RUN_MS = ("--run-ms", "60000")
HELLO_NODE = "hello"
BLINK_NODE = "blink"
HELLO_TOPICS = ((16, 0x02, "std_msgs/msg/String", "chatter"),)
BLINK_TOPICS = ((16, 0x03, "std_msgs/msg/Bool", "led"),)
STRING_BOUND = 16
STRING_MARGIN = 4


def solicit_hello(port, seq, node, topics):
    """The HELLO answering a HELLO_REQ, with its count and node name."""
    port.write(frames.encode_wire(0, seq, bytes((0x01, PROTOCOL_VERSION))))
    topic_id, _, payload = read_unit(port)
    assert topic_id == 0
    assert payload[0] == 0x02  # OP_HELLO
    assert payload[1] == PROTOCOL_VERSION
    assert payload[2] == 0x00  # full handshake mode
    assert int.from_bytes(payload[3:7], "little") == 0, "no token before the first READY"
    assert payload[16] == len(topics)
    name, _ = read_string(payload, 17)
    assert name == node


def read_topic(port, index, seq, topics):
    """The TOPIC at index, with the fields the sketch registered."""
    port.write(frames.encode_wire(0, seq, bytes((0x03, index))))
    topic_id, _, payload = read_unit(port)
    want_id, want_flags, want_type, want_name = topics[index]
    assert topic_id == 0
    assert payload[0] == 0x04  # OP_TOPIC
    assert payload[1] == index
    assert payload[2] == want_id
    assert payload[3] == want_flags
    got_type, at = read_string(payload, 12)
    assert got_type == want_type
    got_name, at = read_string(payload, at)
    assert got_name == want_name
    assert at == len(payload)


def complete_handshake(port, node, topics, seq=0):
    """The handshake the host runs: announce, topic walk, READY."""
    solicit_hello(port, seq, node, topics)
    for index in range(len(topics)):
        read_topic(port, index, seq + index + 1, topics)
    next_seq = seq + len(topics) + 1
    for topic_id, _, _, _ in topics:
        port.write(frames.encode_wire(0, next_seq, bytes((0x05, topic_id, 0x00))))
        next_seq += 1
    port.write(frames.encode_wire(0, next_seq, bytes((0x06, 0x2A, 0x00, 0x00, 0x00))))


def test_hello_announces_its_chatter_topic():
    with Harness("atmega328p", *RUN_MS, image="hello") as harness:
        with serial.Serial(harness.pty, 115200, timeout=2) as port:
            drain(port)
            solicit_hello(port, 0, HELLO_NODE, HELLO_TOPICS)
            read_topic(port, 0, 1, HELLO_TOPICS)
    assert harness.stop() == 0


def test_hello_publishes_strings_within_its_bound():
    with Harness("atmega328p", *RUN_MS, image="hello") as harness:
        with serial.Serial(harness.pty, 115200, timeout=2) as port:
            drain(port)
            complete_handshake(port, HELLO_NODE, HELLO_TOPICS)
            for _ in range(3):
                topic_id, _, payload = read_unit(port, timeout=10.0)
                assert topic_id == HELLO_TOPICS[0][0]
                length, at = read_varint(payload, 0)
                assert length <= STRING_BOUND - STRING_MARGIN
                assert at + length == len(payload)
    assert harness.stop() == 0


def test_blink_answers_bool_without_an_error():
    with Harness("atmega328p", *RUN_MS, image="blink") as harness:
        with serial.Serial(harness.pty, 115200, timeout=2) as port:
            drain(port)
            complete_handshake(port, BLINK_NODE, BLINK_TOPICS)
            port.write(frames.encode_wire(BLINK_TOPICS[0][0], 0, bytes((0x01,))))
            port.write(frames.encode_wire(BLINK_TOPICS[0][0], 1, bytes((0x00,))))
            deadline = time.monotonic() + 2.0
            port.timeout = max(0.1, deadline - time.monotonic())
            assert port.read(1) == b"", "the sketch answered a subscription it should only obey"
            solicit_hello(port, 2, BLINK_NODE, BLINK_TOPICS)
    assert harness.stop() == 0
