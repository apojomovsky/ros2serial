"""rCOBS encoding and decoding (spec/protocol.md, section 4), without the delimiter."""

from .errors import Rejected


def encode(data: bytes) -> bytes:
    out = bytearray()
    n = 1
    for b in data:
        if b == 0:
            out.append(n)
            n = 1
        else:
            out.append(b)
            n += 1
            if n == 0xFF:
                out.append(0xFF)
                n = 1
    out.append(n)
    return bytes(out)


def decode(encoded: bytes) -> bytes:
    """Decodes from the end, group by group, as COB-4 to COB-7 describe.

    The input is one frame without its delimiter, as the framer delivers it; the
    framer ignores an empty frame (FRM-14) before it ever gets here."""
    if encoded[-1:] == b"\xff":
        raise Rejected("COB-6", "the last code is 0xFF")
    if 0 in encoded:
        raise Rejected("COB-5", f"a 0x00 byte at offset {encoded.index(0)}")
    groups = []  # (code, data), from the end of the encoding towards its start
    end = len(encoded)
    while end > 0:
        code = encoded[end - 1]
        if code == 0:
            raise Rejected("COB-5", f"code 0x00 at offset {end - 1}")
        start = end - code
        if start < 0:
            raise Rejected("COB-5", f"code 0x{code:02x} at offset {end - 1} needs {code - 1} bytes before it")
        groups.append((code, encoded[start : end - 1]))
        end = start
    out = bytearray()
    for i, (code, data) in enumerate(reversed(groups)):
        out += data
        if i < len(groups) - 1 and code != 0xFF:
            out.append(0)
    return bytes(out)
