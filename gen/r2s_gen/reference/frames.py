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

"""Frames: the decoded frame and its wire form (spec/protocol.md, section 2).

A frame unit is one frame and the delimiter that ends it, as a byte stream
delivers it, which is what a frame vector writes in its `wire` field.
"""

from . import crc, rcobs
from .errors import Rejected

HEADER = 2  # topic_id and seq, before the payload
MIN_DECODED = 4  # topic_id, seq and crc16, the shortest frame [FRM-7]


def encode(topic_id: int, seq: int, payload: bytes) -> bytes:
    """The decoded frame: `topic_id`, `seq`, the payload, then the CRC little-endian."""
    body = bytes((topic_id, seq)) + payload
    return body + crc.crc16(body).to_bytes(2, "little")


def encode_wire(topic_id: int, seq: int, payload: bytes) -> bytes:
    """The wire form: the decoded frame through rCOBS, then the delimiter [FRM-1]."""
    return rcobs.encode(encode(topic_id, seq, payload)) + b"\x00"


def decode(wire: bytes) -> tuple[int, int, bytes] | None:
    """The `(topic_id, seq, payload)` of one frame unit, or None for an empty one.

    A unit holding only the delimiter is an empty encoded frame, which [FRM-14]
    ignores without counting; every other unit is either a frame or a breach of
    a rule, which raises `Rejected` naming it: the rCOBS rules ([FRM-6]), a
    decoded frame shorter than 4 bytes ([FRM-7]), and the frame check ([FRM-8]).
    """
    if not wire.endswith(b"\x00"):
        raise Rejected("FRM-1", "the frame unit does not end with its delimiter")
    encoded = wire[:-1]
    if not encoded:
        return None
    frame = rcobs.decode(encoded)
    if len(frame) < MIN_DECODED:
        raise Rejected("FRM-7", f"the decoded frame is {len(frame)} bytes")
    if crc.crc16(frame) != 0:
        raise Rejected("FRM-8", "the frame check does not leave a zero residue")
    return frame[0], frame[1], frame[HEADER:-2]
