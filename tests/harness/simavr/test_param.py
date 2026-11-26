# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The float64 parameter over simavr: binary64 on the wire, binary32 in the MCU.

The firmware carries one topic and a gain parameter defaulting to 1.5. On
this 32-bit-double build the DECLARE still carries the 8 bytes of
binary64, the value widened exactly from the binary32 the MCU holds. A
SET arrives as binary64 and the store narrows it with roundTiesToEven:
the firmware reports the stored bits after each applied SET, so the
largest finite value, the rounding midpoint above it, values past it in
both signs, and subnormal boundaries all show what was kept. A value
that only fits at 4 bytes earns a bad-length STATUS instead.

covers: DSC-27, ENC-12, PAR-1, PAR-3
"""

import struct
import time

import pytest
import serial
from r2s_gen.reference import frames
from test_echo import Harness

PROTOCOL_VERSION = 4
MAX_FRAME = 84
DECLARE_GAIN = bytes.fromhex('02 00 02 00 04 67 61 69 6e 00 00 00 00 00 00 f8 3f')
RUN_MS = ('--run-ms', '60000')

# One applied SET per row: the binary64 bytes the host sends and the
# binary32 bits the MCU stores, worked out from [ENC-11] by hand. The
# midpoint sits half an ulp above the largest finite binary32: a value
# there rounds to infinity, since the largest mantissa is odd.
FLT_MAX = struct.unpack('<f', b'\xff\xff\x7f\x7f')[0]
ABOVE_MAX = FLT_MAX + 2.0**100
MIDPOINT = FLT_MAX + 2.0**103
SETTLES = [
    (2.5, 0x40200000),
    (ABOVE_MAX, 0x7F7FFFFF),
    (MIDPOINT, 0x7F800000),
    (-ABOVE_MAX, 0xFF7FFFFF),
    (-MIDPOINT, 0xFF800000),
    (MIDPOINT + 2.0**100, 0x7F800000),
    (2.0**-149, 0x00000001),
    (2.0**-150, 0x00000000),
    (3 * 2.0**-150, 0x00000002),
    (-(2.0**-150), 0x80000000),
]


def settle_payload(value):
    """One binary64 SET value as its 8 little-endian bytes."""
    return struct.pack('<d', value)


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


def expect_stored(port, bits):
    """The firmware's raw report carries exactly these stored bits."""
    port.timeout = 2
    report = port.read(5)
    assert report == b'V' + struct.pack('<I', bits), f'stored {report.hex(" ")}'


@pytest.mark.parametrize('mcu', ['atmega328p', 'atmega2560'])
def test_f64_parameter_declares_wide_and_narrows_on_set(mcu):
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
            for rel, (value, bits) in enumerate(SETTLES):
                payload = bytes((0x03, rel, 0)) + settle_payload(value)
                port.write(frames.encode_wire(3, rel, payload))
                expect(port, 6, bytes((3, rel)))
                expect(port, 3, bytes((0x04, rel, 0, 0)))
                expect_stored(port, bits)
                port.write(frames.encode_wire(6, rel, bytes((3, rel))))
            # A 4-byte SET is the wrong width for a parameter on any link.
            rel = len(SETTLES)
            port.write(
                frames.encode_wire(
                    3, rel, bytes.fromhex('03') + bytes((rel, 0)) + bytes.fromhex('00 00 20 40')
                )
            )
            expect(port, 6, bytes((3, rel)))
            expect(port, 3, bytes((0x04, rel, 0, 2)))
            port.write(frames.encode_wire(6, rel, bytes((3, rel))))
    assert harness.stop() == 0
