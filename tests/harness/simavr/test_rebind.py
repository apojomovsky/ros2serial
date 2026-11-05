"""The flash-to-RAM rebind over simavr: a PROGMEM table configures on the MCU,
the handshake announces it, a host frame on the trigger subscription rebinds
a RAM table with different content, and the next announce carries the RAM
table (#233, follow-up to the #189 bind rework).

The test ignores the boot HELLO like the announce test does: the firmware
transmits it in its first simulated microseconds, before simavr's UART
delivers every byte, so the host solicits each announce with HELLO_REQ. The
rebind needs no new handshake: the session keeps its token, so the second
HELLO carries the READY token and proves no reboot happened.

covers: SES-1, SES-2, CTL-4, CTL-12, CTL-15, CTL-16
"""

import pytest
import serial
from r2s_gen.reference import frames
from test_announce import drain, read_string, read_unit
from test_echo import Harness

NODE_NAME = "rebind"
PROTOCOL_VERSION = 3
READY_TOKEN = 0x2A
FLASH_TOPICS = (
    (16, 0x00, bytes(range(0x00, 0x08)), "std_msgs/msg/Float32", "battery"),
    (17, 0x01, bytes(range(0x10, 0x18)), "geometry_msgs/msg/Twist", "cmd_vel"),
)
RAM_TOPICS = ((18, 0x01, bytes(range(0x20, 0x28)), "geometry_msgs/msg/Twist", "rebound"),)
TRIGGER_TOPIC_ID = 17

# Simulated time outruns the wall clock unpaced, so the run limit is generous:
# the tests stop the harness themselves long before it expires.
RUN_MS = ("--run-ms", "60000")


def solicit_hello(port, seq, want_count, want_token):
    """The HELLO answering a HELLO_REQ, with its count, name and token."""
    port.write(frames.encode_wire(0, seq, bytes((0x01, PROTOCOL_VERSION))))
    topic_id, _, payload = read_unit(port)
    assert topic_id == 0
    assert payload[0] == 0x02  # OP_HELLO
    assert payload[1] == PROTOCOL_VERSION
    assert payload[2] == 0x00  # full handshake mode
    assert int.from_bytes(payload[3:7], "little") == want_token
    assert payload[16] == want_count
    name, _ = read_string(payload, 17)
    assert name == NODE_NAME


def read_topic(port, topics, index, seq):
    """The TOPIC at index, with every field the bound table configures."""
    port.write(frames.encode_wire(0, seq, bytes((0x03, index))))
    topic_id, _, payload = read_unit(port)
    want_id, want_flags, want_hash, want_type, want_name = topics[index]
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


def complete_handshake(port, seq=0):
    """The handshake the host runs: the announce, the topic walk, the READY.

    Every topic resolves with a status of zero, so a publish on the trigger
    subscription dispatches (SES-9).
    """
    solicit_hello(port, seq, len(FLASH_TOPICS), 0)
    for index in range(len(FLASH_TOPICS)):
        read_topic(port, FLASH_TOPICS, index, seq + index + 1)
    next_seq = seq + len(FLASH_TOPICS) + 1
    for topic_id, _, _, _, _ in FLASH_TOPICS:
        port.write(frames.encode_wire(0, next_seq, bytes((0x05, topic_id, 0x00))))
        next_seq += 1
    port.write(frames.encode_wire(0, next_seq, bytes((0x06, READY_TOKEN, 0x00, 0x00, 0x00))))
    return next_seq + 1


@pytest.mark.parametrize("mcu", ["atmega328p", "atmega2560"])
def test_a_trigger_frame_rebinds_the_ram_table(mcu):
    with Harness(mcu, *RUN_MS, image="rebind") as harness:
        with serial.Serial(harness.pty, 115200, timeout=2) as port:
            drain(port)
            next_seq = complete_handshake(port)
            port.write(frames.encode_wire(TRIGGER_TOPIC_ID, 0, bytes((0x00,))))
            solicit_hello(port, next_seq, len(RAM_TOPICS), READY_TOKEN)
            for index in range(len(RAM_TOPICS)):
                read_topic(port, RAM_TOPICS, index, next_seq + index + 1)
    assert harness.stop() == 0


def test_an_out_of_range_request_after_the_rebind_still_answers():
    with Harness("atmega328p", *RUN_MS, image="rebind") as harness:
        with serial.Serial(harness.pty, 115200, timeout=2) as port:
            drain(port)
            next_seq = complete_handshake(port)
            port.write(frames.encode_wire(TRIGGER_TOPIC_ID, 0, bytes((0x00,))))
            solicit_hello(port, next_seq, len(RAM_TOPICS), READY_TOKEN)
            port.write(frames.encode_wire(0, next_seq + 1, bytes((0x03, len(RAM_TOPICS)))))
            read_topic(port, RAM_TOPICS, 0, next_seq + 2)
    assert harness.stop() == 0
