# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Length varints (spec/protocol.md, section 5): unsigned LEB128, at most 65535."""

from .errors import Rejected

MAX = 0xFFFF


def encode(value: int) -> bytes:
    """
    Encode a length as unsigned LEB128, at most 65535.

    :param value: the length to encode.
    :return: the varint bytes.
    :raises ValueError: if the value is outside 0 to 65535.
    """
    if not 0 <= value <= MAX:
        raise ValueError(f'a length is 0 to {MAX}, not {value}')
    out = bytearray()
    while True:
        low = value & 0x7F
        value >>= 7
        if value:
            out.append(low | 0x80)
        else:
            out.append(low)
            return bytes(out)


def decode(data: bytes) -> tuple[int, int]:
    """
    Return the value and the number of bytes it used.

    :param data: the bytes to read the varint from.
    :return: the value and the number of bytes consumed.
    :raises Rejected: if the varint is truncated, too large, or not shortest.
    """
    value = 0
    for i in range(3):
        if i == len(data):
            raise Rejected('VAR-3', 'the varint is truncated')
        value |= (data[i] & 0x7F) << (7 * i)
        if data[i] & 0x80 == 0:
            if value > MAX:
                raise Rejected('VAR-2', f'value {value} is larger than {MAX}')
            if i > 0 and data[i] == 0:
                raise Rejected('VAR-4', 'the encoding is not the shortest')
            return value, i + 1
    raise Rejected('VAR-2', 'the third byte has its high bit set')
