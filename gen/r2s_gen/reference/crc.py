"""CRC-16/MCRF4XX, bit by bit, exactly as CHK-1 states it."""

POLY_REFLECTED = 0x8408  # 0x1021 with its bits reversed, for reflected input and output


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ POLY_REFLECTED if crc & 1 else crc >> 1
    return crc
