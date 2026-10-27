"""The table announce over simavr: a topic table configures on the MCU and the
handshake announces exactly what it configures, from a PROGMEM table and from
a RAM table (#189).

The test ignores the boot HELLO: the firmware transmits it in its first
simulated microseconds, before simavr's UART delivers every byte, so its head
is truncated by a varying amount. A host that opens the serial port after boot
never sees it either; it solicits the announce with HELLO_REQ, which is what
the test does.

covers: SES-1, CTL-4, CTL-12, CTL-15, CTL-16
"""

import time

import pytest
import serial

from r2s_gen.reference import frames
from test_echo import Harness

NODE_NAME = "announce"
BOOT_ID = 0xA53C5A3C
PROTOCOL_VERSION = 1
TOPICS = (
    (16, 0x00, bytes(range(0x00, 0x08)), "std_msgs/msg/Float32", "battery"),
    (17, 0x01, bytes(range(0x10, 0x18)), "geometry_msgs/msg/Twist", "cmd_vel"),
)

# Simulated time outruns the wall clock unpaced, so the run limit is generous:
# the tests stop the harness themselves long before it expires.
RUN_MS = ("--run-ms", "60000")


def drain(port):
    """Drop the truncated boot bytes; everything after is solicited."""
    port.timeout = 0.5
    while port.read(256):
        pass
    port.timeout = 2


def read_unit(port, timeout=5.0):
    """One frame unit off the wire, decoded."""
    unit = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        unit += port.read(1)
        if unit.endswith(b"\x00") and len(unit) > 1:
            break
    assert unit.endswith(b"\x00"), f"no frame within {timeout}s: {unit.hex(' ')}"
    return frames.decode(bytes(unit))


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


def read_string(payload, at):
    """One varint-length-prefixed string and the offset past it."""
    length, at = read_varint(payload, at)
    return bytes(payload[at : at + length]).decode(), at + length


def solicit_hello(port, seq):
    """The HELLO answering a HELLO_REQ, with its count and node name."""
    port.write(frames.encode_wire(0, seq, bytes((0x01, PROTOCOL_VERSION))))
    topic_id, _, payload = read_unit(port)
    assert topic_id == 0
    assert payload[0] == 0x02  # OP_HELLO
    assert payload[1] == PROTOCOL_VERSION
    assert payload[2] == 0x00  # full handshake mode
    assert int.from_bytes(payload[3:7], "little") == BOOT_ID
    assert payload[16] == len(TOPICS)
    name, _ = read_string(payload, 17)
    assert name == NODE_NAME


def read_topic(port, index, seq):
    """The TOPIC at index, with every field the firmware configured."""
    port.write(frames.encode_wire(0, seq, bytes((0x03, index))))
    topic_id, _, payload = read_unit(port)
    want_id, want_flags, want_hash, want_type, want_name = TOPICS[index]
    assert topic_id == 0
    assert payload[0] == 0x04  # OP_TOPIC
    assert payload[1] == index
    assert payload[2] == want_id
    assert payload[3] == want_flags
    assert bytes(payload[4:12]) == want_hash
    got_type, at = read_string(payload, 12)
    assert got_type == want_type
    got_name, at = read_string(payload, at)
    assert got_name == want_name
    assert at == len(payload)


@pytest.mark.parametrize("image", ["announce", "announce-ram"])
@pytest.mark.parametrize("mcu", ["atmega328p", "atmega2560"])
def test_a_table_announces_its_topics_and_node_name(mcu, image):
    with Harness(mcu, *RUN_MS, image=image) as harness:
        with serial.Serial(harness.pty, 115200, timeout=2) as port:
            drain(port)
            solicit_hello(port, 0)
            for index in range(len(TOPICS)):
                read_topic(port, index, index + 1)
    assert harness.stop() == 0


@pytest.mark.parametrize("image", ["announce", "announce-ram"])
def test_an_out_of_range_topic_request_leaves_the_session_answering(image):
    with Harness("atmega328p", *RUN_MS, image=image) as harness:
        with serial.Serial(harness.pty, 115200, timeout=2) as port:
            drain(port)
            solicit_hello(port, 0)
            port.write(frames.encode_wire(0, 1, bytes((0x03, len(TOPICS)))))
            read_topic(port, 0, 2)
    assert harness.stop() == 0
