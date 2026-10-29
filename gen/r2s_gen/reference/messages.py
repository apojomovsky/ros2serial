"""Payloads: the packed message encoding of spec/protocol.md, section 6.

The layout depends on the field types and on whether the topic is narrow, never
on the bounds a side was built with ([ENC-13]), so this reads the message
definitions and no config. A string decodes to its bytes: [ENC-7] passes them
through and checks no UTF-8. A float value is a number or the vector notation
`{bits: "<hex>"}`, the IEEE 754 pattern most significant bit first, which is the
only way to write a NaN payload (docs/01-testing.md, section 5.5).
"""

from __future__ import annotations

import math
import struct
from collections.abc import Mapping
from typing import Any

from r2s_gen.msgs import Field, Message, MessageRepository

from . import varint
from .errors import Rejected

_SIGNED = {"int8": 1, "int16": 2, "int32": 4, "int64": 8}
_UNSIGNED = {"byte": 1, "char": 1, "uint8": 1, "uint16": 2, "uint32": 4, "uint64": 8}
_INTEGERS = {**_SIGNED, **_UNSIGNED}
_FLOATS = {"float32": 4, "float64": 8}
_WIDE = frozenset({"wchar", "wstring"})


def encode(
    repository: MessageRepository,
    type_name: str,
    message: Mapping[str, Any],
    *,
    narrow: bool = False,
) -> bytes:
    """The payload of `message` for `type_name`, or an error naming what it cannot encode.

    A string field takes `str` (encoded as UTF-8) or `bytes`; a bool takes a
    `bool` or 0 and 1. A number is converted with binary32 rounding, so a vector
    must only write a number which is already exactly representable. A missing or
    unknown field is a `ValueError` naming it, as is a value the field cannot
    hold, while input a rule of the spec refuses is a `Rejected` naming that rule.
    """
    out = bytearray()
    _encode_message(repository, repository.load(type_name), message, narrow, out)
    return bytes(out)


def decode(
    repository: MessageRepository,
    type_name: str,
    payload: bytes,
    *,
    narrow: bool = False,
) -> dict[str, Any]:
    """The message `payload` carries for `type_name`, or a `Rejected` naming the rule it breaks.

    Strings come back as `bytes`, and a `float64` field on a narrow topic as the
    binary64 the binary32 on the wire widens to exactly ([ENC-11]).
    """
    reader = _Reader(payload)
    values = _decode_message(repository, repository.load(type_name), reader, narrow)
    if reader.pos != len(payload):
        raise Rejected("ENC-18", f"{len(payload) - reader.pos} bytes after the last field")
    return values


def narrow(value: float) -> float:
    """The binary64 that [ENC-11]'s roundTiesToEven makes of `value` in binary32."""
    return struct.unpack("<f", _binary32("<narrow>", value, overflow_to_infinity=True))[0]


def _encode_message(
    repository: MessageRepository,
    message: Message,
    values: Mapping[str, Any],
    narrow: bool,
    out: bytearray,
    trail: str = "",
) -> None:
    known = {field.name for field in message.fields}
    for key in values:
        if key not in known:
            raise ValueError(f"{trail}{message.type_name}.{key}: unknown field")
    for field in message.fields:
        where = f"{trail}{message.type_name}.{field.name}"
        if field.name not in values:
            raise ValueError(f"{where}: no value")
        value = values[field.name]
        if not field.is_array:
            _encode_element(repository, where, field, value, narrow, out)
            continue
        items = list(value)
        if field.array_length is not None:
            if len(items) != field.array_length:
                raise ValueError(f"{where}: a fixed array holds {field.array_length} elements, not {len(items)}")
        else:
            out += varint.encode(len(items))
        for index, item in enumerate(items):
            _encode_element(repository, f"{where}[{index}]", field, item, narrow, out)


def _encode_element(
    repository: MessageRepository,
    where: str,
    field: Field,
    value: Any,
    narrow: bool,
    out: bytearray,
) -> None:
    kind = field.type_name
    if kind == "string":
        data = _string_bytes(where, value)
        out += varint.encode(len(data))
        out += data
    elif kind == "bool":
        out.append(_bool_byte(where, value))
    elif kind in _INTEGERS:
        integer = _integer(where, value, _INTEGERS[kind], kind in _SIGNED)
        width = _INTEGERS[kind]
        out += integer.to_bytes(width, "little", signed=kind in _SIGNED)
    elif kind in _FLOATS:
        out += _float_bytes(where, value, _FLOATS[kind], narrow and kind == "float64")
    elif kind in _WIDE:
        raise Rejected("ENC-19", f"{where}: {kind} has no wire encoding")
    else:
        if not isinstance(value, Mapping):
            raise ValueError(f"{where}: expected a mapping of {kind}'s fields")
        _encode_message(repository, repository.load(kind), value, narrow, out, trail=f"{where}.")


def _decode_message(
    repository: MessageRepository,
    message: Message,
    reader: _Reader,
    narrow: bool,
    trail: str = "",
) -> dict[str, Any]:
    values: dict[str, Any] = {}
    for field in message.fields:
        where = f"{trail}{message.type_name}.{field.name}"
        if field.is_array:
            count = field.array_length if field.array_length is not None else reader.varint()
            values[field.name] = [
                _decode_element(repository, f"{where}[{index}]", field, reader, narrow) for index in range(count)
            ]
        else:
            values[field.name] = _decode_element(repository, where, field, reader, narrow)
    return values


def _decode_element(
    repository: MessageRepository,
    where: str,
    field: Field,
    reader: _Reader,
    narrow: bool,
) -> Any:
    kind = field.type_name
    if kind == "string":
        data = reader.take(reader.varint())
        if b"\x00" in data:
            raise Rejected("ENC-14", f"{where}: the string holds a 0x00 byte")
        return data
    if kind == "bool":
        byte = reader.u8()
        if byte not in (0, 1):
            raise Rejected("ENC-6", f"{where}: the bool byte is {byte}")
        return bool(byte)
    if kind in _INTEGERS:
        return int.from_bytes(reader.take(_INTEGERS[kind]), "little", signed=kind in _SIGNED)
    if kind in _FLOATS:
        if _FLOATS[kind] == 4 or narrow:
            return struct.unpack("<f", reader.take(4))[0]
        return struct.unpack("<d", reader.take(8))[0]
    if kind in _WIDE:
        raise Rejected("ENC-19", f"{where}: {kind} has no wire encoding")
    return _decode_message(repository, repository.load(kind), reader, narrow, trail=f"{where}.")


def _string_bytes(where: str, value: Any) -> bytes:
    if isinstance(value, str):
        data = value.encode("utf-8")
    elif isinstance(value, (bytes, bytearray)):
        data = bytes(value)
    else:
        raise ValueError(f"{where}: expected a string, not {value!r}")
    if b"\x00" in data:
        raise Rejected("ENC-14", f"{where}: a string holds no 0x00 byte")
    return data


def _bool_byte(where: str, value: Any) -> int:
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int) and value in (0, 1):
        return value
    raise ValueError(f"{where}: a bool is true or false, not {value!r}")


def _integer(where: str, value: Any, width: int, signed: bool) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{where}: expected an integer, not {value!r}")
    low = -(1 << (8 * width - 1)) if signed else 0
    high = (1 << (8 * width - (1 if signed else 0))) - 1
    if not low <= value <= high:
        raise ValueError(f"{where}: {value} does not fit {width * 8} bits")
    return value


def _float_bytes(where: str, value: Any, width: int, narrow: bool) -> bytes:
    if isinstance(value, Mapping):
        pattern = _pattern(where, value, width)
        if narrow:
            return _binary32(where, struct.unpack("<d", pattern)[0], overflow_to_infinity=True)
        return pattern
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{where}: expected a number or {{bits: ...}}, not {value!r}")
    try:
        number = float(value)
    except OverflowError:
        raise ValueError(f"{where}: {value!r} does not fit binary64") from None
    if width == 4 or narrow:
        # A float32 field holds a binary32 already, so a value too large for one
        # is a mistake in the input; [ENC-11] makes a wide value an infinity.
        return _binary32(where, number, overflow_to_infinity=narrow)
    return struct.pack("<d", number)


def _pattern(where: str, value: Mapping[str, Any], width: int) -> bytes:
    """The little-endian bytes of a `{bits: "<hex>"}`, written most significant bit first."""
    bits = value.get("bits")
    if (
        not isinstance(bits, str)
        or len(bits) != 2 * width
        or any(digit not in "0123456789abcdefABCDEF" for digit in bits)
    ):
        raise ValueError(f"{where}: bits are {2 * width} hex digits of a binary{8 * width} pattern")
    return bytes.fromhex(bits)[::-1]


def _binary32(where: str, number: float, *, overflow_to_infinity: bool) -> bytes:
    try:
        return struct.pack("<f", number)
    except OverflowError:
        if not overflow_to_infinity:
            raise ValueError(f"{where}: {number!r} does not fit binary32") from None
        return struct.pack("<f", math.copysign(math.inf, number))


class _Reader:
    """The payload bytes and how many have been read, naming ENC-17 when one is missing."""

    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def take(self, count: int) -> bytes:
        if self.pos + count > len(self.data):
            raise Rejected("ENC-17", f"the payload ends after {self.pos} bytes, {count} short")
        out = self.data[self.pos : self.pos + count]
        self.pos += count
        return out

    def u8(self) -> int:
        return self.take(1)[0]

    def varint(self) -> int:
        value, used = varint.decode(self.data[self.pos :])
        self.pos += used
        return value
