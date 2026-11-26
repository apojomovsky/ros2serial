# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The float64 parameter over simavr: a 32-bit-double MCU declares and sets it narrow.

The firmware carries one topic and a gain parameter defaulting to 1.5. On
this build the DECLARE carries the 4 bytes of binary32, a narrow SET of
2.5 applies and the firmware reports the stored value, and the same value
at 8 bytes is refused as a bad length. A core that travelled wide would
declare 8 bytes the bridge drops, and this test would never see them.

covers: DSC-27, ENC-12, PAR-1, PAR-3
"""

import time

import pytest
import serial
from r2s_gen.reference import frames
from test_echo import Harness

PROTOCOL_VERSION = 4
MAX_FRAME = 84
DECLARE_GAIN = bytes.fromhex('02 00 02 00 04 67 61 69 6e 00 00 c0 3f')
RUN_MS = ('--run-ms', '60000')


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
        if unit.endswith(b'\x00') and len(unit) > 1:
            break
    assert unit.endswith(b'\x00'), f'no frame within {timeout}s: {unit.hex(" ")}'
    return frames.decode(bytes(unit))


def expect(port, topic_id, payload):
    """The next framed unit is exactly this topic and payload."""
    got_topic, _, got_payload = read_unit(port)
    assert got_topic == topic_id, f'topic {got_topic}, want {topic_id}'
    assert got_payload == payload, f'{got_payload.hex(" ")}, want {payload.hex(" ")}'


@pytest.mark.parametrize('mcu', ['atmega328p', 'atmega2560'])
def test_f64_parameter_declares_and_sets_narrow(mcu):
    with Harness(mcu, *RUN_MS, image='param') as harness:
        with serial.Serial(harness.pty, 115200, timeout=2) as port:
            drain(port)
            port.write(frames.encode_wire(0, 0, bytes((0x01, PROTOCOL_VERSION))))
            topic_id, _, hello = read_unit(port)
            assert topic_id == 0
            assert hello[0] == 0x02
            assert hello[15] & 0x10, 'the HELLO sets the 32-bit-double feature bit'
            assert hello[16] == 1, 'one topic'
            port.write(frames.encode_wire(0, 1, bytes((0x03, 0))))
            topic_id, _, topic = read_unit(port)
            assert topic_id == 0
            assert topic[0] == 0x04
            assert topic[2] == 16, 'the battery topic id'
            port.write(frames.encode_wire(0, 2, bytes((0x05, 16, 0))))
            port.write(frames.encode_wire(3, 0, bytes((0x01, 0))))
            expect(port, 3, DECLARE_GAIN)
            port.write(frames.encode_wire(3, 1, bytes((0x01, 1))))
            expect(port, 3, bytes((0x04, 0, 1, 1)))
            port.write(frames.encode_wire(0, 3, bytes((0x06, 1, 0, 0, 0))))
            # A narrow SET of 2.5 applies; the firmware then reports the
            # stored value as raw marker bytes after the framed STATUS.
            port.write(frames.encode_wire(3, 0, bytes.fromhex('03 00 00 00 00 20 40')))
            expect(port, 6, bytes((3, 0)))
            expect(port, 3, bytes((0x04, 0, 0, 0)))
            port.timeout = 2
            assert port.read(7) == b'GAIN25!'
            port.write(frames.encode_wire(6, 0, bytes((3, 0))))
            # The same value at 8 bytes earns a bad-length STATUS, not an apply.
            port.write(frames.encode_wire(3, 1, bytes.fromhex('03 01 00 00 00 00 00 00 00 04 40')))
            expect(port, 6, bytes((3, 1)))
            expect(port, 3, bytes((0x04, 1, 0, 2)))
            port.write(frames.encode_wire(6, 1, bytes((3, 1))))
    assert harness.stop() == 0
