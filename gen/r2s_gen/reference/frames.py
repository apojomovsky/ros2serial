# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""
Frames: the decoded frame and its wire form (spec/protocol.md, section 2).

A frame unit is one frame and the delimiter that ends it, as a byte stream
delivers it, which is what a frame vector writes in its `wire` field.
"""

from . import crc
from . import rcobs
from .errors import Rejected

HEADER = 2  # topic_id and seq, before the payload
MIN_DECODED = 4  # topic_id, seq and crc16, the shortest frame [FRM-7]


def encode(topic_id: int, seq: int, payload: bytes) -> bytes:
    """
    Encode the decoded frame: `topic_id`, `seq`, the payload, then the CRC.

    :param topic_id: the topic to send on.
    :param seq: the sequence number.
    :param payload: the message payload.
    :return: the decoded frame with the CRC little-endian.
    """
    body = bytes((topic_id, seq)) + payload
    return body + crc.crc16(body).to_bytes(2, 'little')


def encode_wire(topic_id: int, seq: int, payload: bytes) -> bytes:
    """
    Encode the wire form: the decoded frame through rCOBS, then the delimiter.

    :param topic_id: the topic to send on.
    :param seq: the sequence number.
    :param payload: the message payload.
    :return: the frame unit [FRM-1].
    """
    return rcobs.encode(encode(topic_id, seq, payload)) + b'\x00'


def decode(wire: bytes) -> tuple[int, int, bytes] | None:
    """
    Decode one frame unit into `(topic_id, seq, payload)`, or None if empty.

    A unit holding only the delimiter is an empty encoded frame, which [FRM-14]
    ignores without counting; every other unit is either a frame or a breach of
    a rule, which raises `Rejected` naming it: the rCOBS rules ([FRM-6]), a
    decoded frame shorter than 4 bytes ([FRM-7]), and the frame check ([FRM-8]).

    :param wire: the frame unit bytes, delimiter included.
    :return: the `(topic_id, seq, payload)`, or None for an empty unit.
    :raises Rejected: if the unit breaks a rule named above.
    """
    if not wire.endswith(b'\x00'):
        raise Rejected('FRM-1', 'the frame unit does not end with its delimiter')
    encoded = wire[:-1]
    if not encoded:
        return None
    frame = rcobs.decode(encoded)
    if len(frame) < MIN_DECODED:
        raise Rejected('FRM-7', f'the decoded frame is {len(frame)} bytes')
    if crc.crc16(frame) != 0:
        raise Rejected('FRM-8', 'the frame check does not leave a zero residue')
    return frame[0], frame[1], frame[HEADER:-2]
