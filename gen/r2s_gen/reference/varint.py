"""Length varints (spec/protocol.md, section 5): unsigned LEB128, at most 65535."""

from .errors import Rejected

MAX = 0xFFFF


def encode(value: int) -> bytes:
    if not 0 <= value <= MAX:
        raise ValueError(f"a length is 0 to {MAX}, not {value}")
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
    """Returns the value and the number of bytes it used."""
    value = 0
    for i in range(3):
        if i == len(data):
            raise Rejected("VAR-3", "the varint is truncated")
        value |= (data[i] & 0x7F) << (7 * i)
        if data[i] & 0x80 == 0:
            if value > MAX:
                raise Rejected("VAR-2", f"value {value} is larger than {MAX}")
            if i > 0 and data[i] == 0:
                raise Rejected("VAR-4", "the encoding is not the shortest")
            return value, i + 1
    raise Rejected("VAR-2", "the third byte has its high bit set")
