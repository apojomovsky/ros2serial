# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Python references for the P9 framing benchmark's alternative framers.

rCOBS mirrors spec/protocol.md section 4 (the shipped core is the product;
this model cross-checks the test vectors). COBS is the standard
Stuart Cheshire byte stuffing, SLIP follows RFC 1055, and the rosserial-style
model is a length-prefixed sync framing in the shape of rosserial's wire
format (sync byte, version, length plus additive checksums), reimplemented
here from its description. All four wrap the same user bytes so the
benchmark compares framing, not payloads.
"""

RS_SYNC = 0xFF
RS_VERSION = 0x01
SLIP_END = 0xC0
SLIP_ESC = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD


def crc16_mcrf4xx_bitwise(data: bytes) -> int:
    """CRC-16/MCRF4XX bit by bit, the shipped algorithm (CHK-1)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0x8408
            else:
                crc >>= 1
    return crc & 0xFFFF


def crc16_mcrf4xx_table(data: bytes) -> int:
    """CRC-16/MCRF4XX through a 256 entry table, same wire result."""
    table = []
    for value in range(256):
        crc = value
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0x8408
            else:
                crc >>= 1
        table.append(crc & 0xFFFF)
    crc = 0xFFFF
    for byte in data:
        crc = ((crc >> 8) ^ table[(crc ^ byte) & 0xFF]) & 0xFFFF
    return crc


def rcobs_encode(data: bytes) -> bytes:
    """rCOBS encoding after COB-1: codes follow the block they describe."""
    out = bytearray()
    run = 1
    for byte in data:
        if byte == 0x00:
            out.append(run)
            run = 1
        else:
            out.append(byte)
            run += 1
            if run == 0xFF:
                out.append(0xFF)
                run = 1
    out.append(run)
    return bytes(out)


def rcobs_decode(enc: bytes) -> bytes:
    """rCOBS decoding after COB-4, naming COB-5 and COB-6 like the core."""
    if len(enc) == 0:
        return b''
    if enc[-1] == 0xFF:
        raise ValueError('COB-6: encoding ends in 0xFF')
    if any(byte == 0x00 for byte in enc):
        raise ValueError('COB-5: encoding holds 0x00')
    groups = []
    read = len(enc)
    while read > 0:
        code = enc[read - 1]
        if code > read:
            raise ValueError('COB-5: code overruns the input')
        groups.append((code, bytes(enc[read - code : read - 1])))
        read -= code
    out = bytearray()
    for index in range(len(groups) - 1, -1, -1):
        code, data = groups[index]
        out += data
        if index != 0 and code != 0xFF:
            out.append(0x00)
    return bytes(out)


def cobs_encode(data: bytes) -> bytes:
    """Standard COBS: each code byte opens the block it describes."""
    out = bytearray([0])
    code_index = 0
    code = 1
    for byte in data:
        if byte == 0x00:
            out[code_index] = code
            code_index = len(out)
            out.append(0)
            code = 1
        else:
            out.append(byte)
            code += 1
            if code == 0xFF:
                out[code_index] = code
                code_index = len(out)
                out.append(0)
                code = 1
    out[code_index] = code
    return bytes(out)


def cobs_decode(enc: bytes) -> bytes:
    """Standard COBS decoding, front to back."""
    if any(byte == 0x00 for byte in enc):
        raise ValueError('COBS: encoding holds 0x00')
    out = bytearray()
    index = 0
    size = len(enc)
    while index < size:
        code = enc[index]
        if code == 0x00 or index + code > size:
            raise ValueError('COBS: code overruns the input')
        out += enc[index + 1 : index + code]
        index += code
        if code < 0xFF and index < size:
            out.append(0x00)
    return bytes(out)


def slip_encode(data: bytes) -> bytes:
    """SLIP after RFC 1055: escape END and ESC, then append END."""
    out = bytearray()
    for byte in data:
        if byte == SLIP_END:
            out += bytes((SLIP_ESC, SLIP_ESC_END))
        elif byte == SLIP_ESC:
            out += bytes((SLIP_ESC, SLIP_ESC_ESC))
        else:
            out.append(byte)
    out.append(SLIP_END)
    return bytes(out)


def slip_decode(segment: bytes) -> bytes:
    """SLIP decoding of one segment between END markers."""
    if any(byte == SLIP_END for byte in segment):
        raise ValueError('SLIP: segment holds END')
    out = bytearray()
    index = 0
    while index < len(segment):
        byte = segment[index]
        if byte == SLIP_ESC:
            if index + 1 >= len(segment):
                raise ValueError('SLIP: trailing ESC')
            follower = segment[index + 1]
            if follower == SLIP_ESC_END:
                out.append(SLIP_END)
            elif follower == SLIP_ESC_ESC:
                out.append(SLIP_ESC)
            else:
                raise ValueError('SLIP: bad escape')
            index += 2
        else:
            out.append(byte)
            index += 1
    return bytes(out)


def rs_encode(topic: int, seq: int, payload: bytes) -> bytes:
    """Rosserial-style frame: sync, version, length plus checks, payload."""
    body = bytes((topic & 0xFF, (topic >> 8) & 0xFF, seq & 0xFF)) + bytes(payload)
    length = len(body) + 1
    length_check = (-(length & 0xFF) - ((length >> 8) & 0xFF)) & 0xFF
    message_check = (-sum(body)) & 0xFF
    return (
        bytes((RS_SYNC, RS_VERSION, length & 0xFF, (length >> 8) & 0xFF, length_check))
        + body
        + bytes((message_check,))
    )


RS_BODY_MAX = 256


def rs_scan(stream: bytes):
    """Scan a byte stream for rosserial-style frames.

    Returns (frames, first_offset, last_end) where frames holds
    (topic, seq, payload) and the offsets bound the recovered span.
    """
    frames = []
    first_offset = None
    last_end = 0
    index = 0
    while index + 5 <= len(stream):
        if stream[index] != RS_SYNC or stream[index + 1] != RS_VERSION:
            index += 1
            continue
        length = stream[index + 2] | (stream[index + 3] << 8)
        if (
            length < 4
            or length > RS_BODY_MAX
            or (stream[index + 2] + stream[index + 3] + stream[index + 4]) & 0xFF != 0
        ):
            index += 1
            continue
        end = index + 5 + length
        if end > len(stream):
            break
        body = stream[index + 5 : end - 1]
        if (sum(body) + stream[end - 1]) & 0xFF != 0:
            index += 1
            continue
        topic = body[0] | (body[1] << 8)
        frames.append((topic, body[2], bytes(body[3:])))
        if first_offset is None:
            first_offset = index
        last_end = end
        index = end
    return frames, first_offset, last_end


def delimiter_of(framer: str) -> int:
    """The wire byte that ends a frame: 0x00, except SLIP which uses END."""
    return SLIP_END if framer == 'slip' else 0x00


def delimited_split(stream: bytes, delimiter: int = 0x00):
    """Split a delimited stream on its end marker."""
    segments = []
    current = bytearray()
    for byte in stream:
        if byte == delimiter:
            segments.append(bytes(current))
            current = bytearray()
        else:
            current.append(byte)
    return segments, len(stream) - len(current)
