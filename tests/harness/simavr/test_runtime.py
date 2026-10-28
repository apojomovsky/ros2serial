"""The runtime API on an emulated AVR: the firmware registers its topics while
it runs, the handshake announces exactly those, and a Twist the host sends
comes back as a battery frame.

The test reuses the announce test's frame readers, so both firmware shapes
answer the same control frames the same way.

covers: CTL-4, CTL-12, CTL-15, CTL-16, SES-1, SES-2, SES-9, FRM-4
"""

import struct

import pytest
import serial
from r2s_gen.reference import frames
from test_announce import drain, read_string, read_unit
from test_echo import Harness

NODE_NAME = "runtime_base"
PROTOCOL_VERSION = 2
BATTERY_TOPIC_ID = 16
CMD_VEL_TOPIC_ID = 17
TOPICS = (
    # The emulated AVR's double is 32 bits, so every topic carries the
    # narrow flag and a float64 field takes four bytes on the wire
    # (ENC-11, ENC-12).
    (BATTERY_TOPIC_ID, 0x02, "std_msgs/msg/Float32", "battery"),
    (CMD_VEL_TOPIC_ID, 0x03, "geometry_msgs/msg/Twist", "cmd_vel"),
)

# Simulated time outruns the wall clock unpaced, so the run limit is
# generous: the tests stop the harness themselves long before it expires.
RUN_MS = ("--run-ms", "60000")


def solicit_hello(port, seq):
    """The HELLO answering a HELLO_REQ, with its count and node name."""
    port.write(frames.encode_wire(0, seq, bytes((0x01, PROTOCOL_VERSION))))
    topic_id, _, payload = read_unit(port)
    assert topic_id == 0
    assert payload[0] == 0x02  # OP_HELLO
    assert payload[1] == PROTOCOL_VERSION
    assert payload[2] == 0x00  # full handshake mode
    assert int.from_bytes(payload[3:7], "little") == 0, "no token before the first READY"
    assert payload[16] == len(TOPICS)
    name, _ = read_string(payload, 17)
    assert name == NODE_NAME


def read_topic(port, index, seq):
    """The TOPIC at index, with the fields the runtime registered."""
    port.write(frames.encode_wire(0, seq, bytes((0x03, index))))
    topic_id, _, payload = read_unit(port)
    want_id, want_flags, want_type, want_name = TOPICS[index]
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


def twist_payload(linear, angular):
    """The packed payload of a Twist on a narrow topic: six float32 fields."""
    return struct.pack("<6f", linear, 0.0, 0.0, angular, 0.0, 0.0)


def complete_handshake(port, seq=0):
    """The handshake the host runs: the announce, the topic walk, the READY.

    Every topic is resolved with a status of zero, so a publish on it works
    (SES-9).
    """
    solicit_hello(port, seq)
    for index in range(len(TOPICS)):
        read_topic(port, index, seq + index + 1)
    next_seq = seq + len(TOPICS) + 1
    for topic_id, _, _, _ in TOPICS:
        port.write(frames.encode_wire(0, next_seq, bytes((0x05, topic_id, 0x00))))
        next_seq += 1
    port.write(frames.encode_wire(0, next_seq, bytes((0x06, 0x2A, 0x00, 0x00, 0x00))))


@pytest.mark.parametrize("mcu", ["atmega328p", "atmega2560"])
def test_the_runtime_announces_its_registered_topics(mcu):
    with Harness(mcu, *RUN_MS, image="runtime") as harness:
        with serial.Serial(harness.pty, 115200, timeout=2) as port:
            drain(port)
            solicit_hello(port, 0)
            for index in range(len(TOPICS)):
                read_topic(port, index, index + 1)
    assert harness.stop() == 0


def test_a_host_twist_comes_back_as_a_battery_frame():
    with Harness("atmega328p", *RUN_MS, image="runtime") as harness:
        with serial.Serial(harness.pty, 115200, timeout=2) as port:
            drain(port)
            complete_handshake(port)
            port.write(frames.encode_wire(CMD_VEL_TOPIC_ID, 0, twist_payload(1.5, 0.25)))
            topic_id, _, payload = read_unit(port)
    assert topic_id == BATTERY_TOPIC_ID
    assert struct.unpack("<f", payload)[0] == pytest.approx(1.5, abs=1e-6)
    assert harness.stop() == 0
