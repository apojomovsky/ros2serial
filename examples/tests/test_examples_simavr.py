"""Hello and blink on an emulated Uno: the two task sketches that need no hardware.

The firmware registers its topics while it runs, the handshake announces
exactly those, hello publishes strings within its bound and a Bool the host
sends reaches blink without an error report. Assertions stay
bounds-with-margin: lengths against the bound less a margin, never exact
golden bytes, so the test proves the link works without pinning content.

The handshake tolerates the session's own traffic: HELLO repeats while
unconnected (SES-5), error reports for the sketch's not-connected publishes,
and heartbeats once connected (SES-9). A drain to silence would hang on the
first, so the test reads until the unit it asked for and fails on anything
unexpected instead. Garbled units are skipped with a deadline: on unpaced
simavr the solicited answers race spontaneous traffic, and a corrupt unit
is a countable link event the heartbeat reports, not a sketch failure.
Strict byte validation lives in the vector, contract and e2e suites.

covers: SES-1, SES-2, SES-5, SES-9
"""

import time

import serial
from r2s_gen.reference import frames
from test_announce import read_string
from test_echo import Harness

PROTOCOL_VERSION = 3
RUN_MS = ("--run-ms", "60000")
HELLO_NODE = "hello"
BLINK_NODE = "blink"
HELLO_TOPICS = ((16, 0x02, "std_msgs/msg/String", "chatter"),)
BLINK_TOPICS = ((16, 0x03, "std_msgs/msg/Bool", "led"),)
STRING_BOUND = 16
STRING_MARGIN = 4
OP_HELLO = 0x02
OP_TOPIC = 0x04
HEARTBEAT_TOPIC = 4
ERROR_TOPIC = 5
READ_TIMEOUT = 10.0


def next_unit(port, deadline):
    """The next decodable unit, skipping garbled ones until the deadline."""
    while True:
        remaining = deadline - time.monotonic()
        assert remaining > 0, "no valid unit within the wait"
        unit = bytearray()
        unit_deadline = time.monotonic() + min(remaining, READ_TIMEOUT)
        while time.monotonic() < unit_deadline:
            unit += port.read(1)
            if unit.endswith(b"\x00") and len(unit) > 1:
                break
        if not unit.endswith(b"\x00"):
            continue
        try:
            return frames.decode(bytes(unit))
        except Exception:  # noqa: BLE001 - a garbled unit is skipped, not diagnosed here
            continue


def read_hello(port, deadline):
    """The next HELLO unit, skipping the session's own repeats (SES-5)."""
    while True:
        topic_id, _, payload = next_unit(port, deadline)
        if topic_id == ERROR_TOPIC:
            continue  # the sketch publishes before it connects
        assert topic_id == 0, f"unexpected topic {topic_id} while waiting for HELLO"
        if payload[0] == OP_HELLO:
            return payload
        raise AssertionError(f"unexpected op {payload[0]} while waiting for HELLO")


def solicit_hello(port, seq, node, topics):
    """The HELLO answering a HELLO_REQ, with its count and node name."""
    port.write(frames.encode_wire(0, seq, bytes((0x01, PROTOCOL_VERSION))))
    payload = read_hello(port, time.monotonic() + READ_TIMEOUT)
    assert payload[1] == PROTOCOL_VERSION
    assert payload[2] == 0x00  # full handshake mode
    assert int.from_bytes(payload[3:7], "little") == 0, "no token before the first READY"
    assert payload[16] == len(topics)
    name, _ = read_string(payload, 17)
    assert name == node


def read_topic(port, index, seq, topics, strict=False):
    """The TOPIC at index, skipping HELLO repeats and heartbeats on the way."""
    port.write(frames.encode_wire(0, seq, bytes((0x03, index))))
    deadline = time.monotonic() + READ_TIMEOUT
    while True:
        topic_id, _, payload = next_unit(port, deadline)
        if topic_id == HEARTBEAT_TOPIC:
            continue  # SES-9: the connected heartbeat shares the link
        if topic_id == ERROR_TOPIC and not strict:
            continue  # the sketch publishes before it connects
        assert topic_id == 0, f"unexpected topic {topic_id} while waiting for TOPIC"
        if payload[0] == OP_HELLO:
            continue  # SES-5: the unconnected repeat answers nothing
        assert payload[0] == OP_TOPIC, f"unexpected op {payload[0]} while waiting for TOPIC"
        break
    want_id, want_flags, want_type, want_name = topics[index]
    assert payload[1] == index
    assert payload[2] == want_id
    assert payload[3] == want_flags
    got_type, at = read_string(payload, 12)
    assert got_type == want_type
    got_name, at = read_string(payload, at)
    assert got_name == want_name
    assert at == len(payload)


def complete_handshake(port, node, topics, seq=0):
    """The handshake the host runs: the announce, the topic walk, the READY."""
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
            solicit_hello(port, 0, HELLO_NODE, HELLO_TOPICS)
            read_topic(port, 0, 1, HELLO_TOPICS)
    assert harness.stop() == 0


def read_varint(payload, at):
    """One LEB128 value and the offset past it."""
    value = 0
    shift = 0
    while True:
        byte = payload[at]
        at += 1
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            return value, at
        shift += 7


def test_hello_publishes_strings_within_its_bound():
    with Harness("atmega328p", *RUN_MS, image="hello") as harness:
        with serial.Serial(harness.pty, 115200, timeout=2) as port:
            complete_handshake(port, HELLO_NODE, HELLO_TOPICS)
            seen = 0
            deadline = time.monotonic() + READ_TIMEOUT
            while seen < 3:
                topic_id, _, payload = next_unit(port, deadline)
                if topic_id == HEARTBEAT_TOPIC:
                    continue
                if topic_id == ERROR_TOPIC:
                    continue  # a stale not-connected report from the backlog
                assert topic_id == HELLO_TOPICS[0][0], f"unexpected topic {topic_id} after READY"
                length, at = read_varint(payload, 0)
                assert length <= STRING_BOUND - STRING_MARGIN
                assert at + length == len(payload)
                seen += 1
    assert harness.stop() == 0


def test_blink_answers_bool_without_an_error():
    with Harness("atmega328p", *RUN_MS, image="blink") as harness:
        with serial.Serial(harness.pty, 115200, timeout=2) as port:
            complete_handshake(port, BLINK_NODE, BLINK_TOPICS)
            port.write(frames.encode_wire(BLINK_TOPICS[0][0], 0, bytes((0x01,))))
            port.write(frames.encode_wire(BLINK_TOPICS[0][0], 1, bytes((0x00,))))
            # A TOPIC walk proves the dispatch survived: the answer arrives
            # while heartbeats share the link, and an error report fails the
            # wait naming the bound the firmware hit. The blink sketch never
            # publishes, so no stale report can beat the dispatch here.
            read_topic(port, 0, 2, BLINK_TOPICS, strict=True)
    assert harness.stop() == 0
