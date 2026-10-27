"""Helpers the generator tests share: throwaway message packages, vectors and random messages."""

import math
import random
import struct
from collections.abc import Mapping
from pathlib import Path
from typing import Any

import pytest
import yaml
from r2s_gen import msgs
from r2s_gen.reference import messages, varint

VECTORS = Path(__file__).resolve().parents[2] / "spec" / "vectors"
SEED = 20261026

_INTEGER_RANGES = {
    "int8": (-128, 127),
    "uint8": (0, 255),
    "byte": (0, 255),
    "char": (0, 255),
    "int16": (-(2**15), 2**15 - 1),
    "uint16": (0, 2**16 - 1),
    "int32": (-(2**31), 2**31 - 1),
    "uint32": (0, 2**32 - 1),
    "int64": (-(2**63), 2**63 - 1),
    "uint64": (0, 2**64 - 1),
}
_FLOAT_WIDTHS = {"float32": 4, "float64": 8}


def write_message(root: Path, type_name: str, text: str) -> Path:
    """Write `text` as the definition of `type_name` under a message root, and return its path."""
    package, _, name = type_name.partition("/msg/")
    path = root / package / "msg" / f"{name}.msg"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def h(text: str) -> bytes:
    return bytes.fromhex(text)


def vector_cases(kind: str, reject: bool = False) -> list:
    """Every case of the `kind` vector files, as pytest parameters named by file and index."""
    params = []
    for path in sorted((VECTORS / kind).glob("*.yaml")):
        doc = yaml.safe_load(path.read_text())
        for i, case in enumerate(doc.get("cases") or []):
            if ("reject" in case) == reject:
                params.append(pytest.param(case, id=f"{path.stem}-{i}"))
    return params


def vector_files(kind: str) -> list:
    """Every `kind` vector file, as pytest parameters named by the file."""
    paths = sorted((VECTORS / kind).glob("*.yaml"))
    return [pytest.param(yaml.safe_load(path.read_text()), id=path.stem) for path in paths]


def random_message(repository: msgs.MessageRepository, type_name: str, rng: random.Random) -> dict[str, Any]:
    """A message of `type_name` whose every field holds a random value, boundaries included."""
    return _random_message(repository, repository.load(type_name), rng)


def _random_message(repository: msgs.MessageRepository, message: msgs.Message, rng: random.Random) -> dict[str, Any]:
    values = {}
    for field in message.fields:
        if field.is_array:
            # A count over 127 crosses the two-byte varint, which is worth making every time.
            count = field.array_length if field.array_length is not None else rng.choice([0, 1, 2, 3, 130])
            values[field.name] = [_random_element(repository, field, rng) for _ in range(count)]
        else:
            values[field.name] = _random_element(repository, field, rng)
    return values


def _random_element(repository: msgs.MessageRepository, field: msgs.Field, rng: random.Random) -> Any:
    kind = field.type_name
    if kind == "bool":
        return rng.choice([True, False])
    if kind in _INTEGER_RANGES:
        low, high = _INTEGER_RANGES[kind]
        return rng.choice([low, high, 0 if low <= 0 else low, rng.randrange(low, high + 1)])
    if kind in _FLOAT_WIDTHS:
        # Bit patterns reach every float there is, subnormals and NaNs included.
        width = 8 * _FLOAT_WIDTHS[kind]
        pattern = rng.choice(
            [
                rng.getrandbits(width),
                0,
                1 << (width - 1),
                1,
                (1 << width) - 1,
                (1 << (width - 1)) | (1 << (width - 2)),
            ]
        )
        return {"bits": f"{pattern:0{width // 4}x}"}
    if kind == "string":
        return bytes(rng.choice([rng.randrange(1, 0x100), 0x7F, 0x80, 0xFF]) for _ in range(rng.choice([0, 1, 5, 130])))
    return _random_message(repository, repository.load(kind), rng)


def matches(
    repository: msgs.MessageRepository,
    type_name: str,
    expected: Mapping[str, Any],
    decoded: Mapping[str, Any],
    *,
    narrow: bool = False,
) -> bool:
    """Whether `decoded` holds the message `expected` writes, with NaN equal to NaN.

    A vector writes a float as a number or as `{bits: "<hex>"}`; a narrow topic
    carries a binary32 that [ENC-11] makes of the binary64 the vector states.
    """
    return _message_matches(repository, repository.load(type_name), expected, decoded, narrow)


def _message_matches(
    repository: msgs.MessageRepository,
    message: msgs.Message,
    expected: Mapping[str, Any],
    decoded: Mapping[str, Any],
    narrow: bool,
) -> bool:
    for field in message.fields:
        if field.name not in expected or field.name not in decoded:
            return False
        if field.is_array:
            want, got = list(expected[field.name]), list(decoded[field.name])
            if len(want) != len(got) or not all(
                _element_matches(repository, field, a, b, narrow) for a, b in zip(want, got, strict=True)
            ):
                return False
        elif not _element_matches(repository, field, expected[field.name], decoded[field.name], narrow):
            return False
    return True


def _element_matches(
    repository: msgs.MessageRepository,
    field: msgs.Field,
    expected: Any,
    decoded: Any,
    narrow: bool,
) -> bool:
    kind = field.type_name
    if kind in _FLOAT_WIDTHS:
        width = _FLOAT_WIDTHS[kind]
        n = narrow and kind == "float64"
        wanted, got = _field_value(expected, width, n), _field_value(decoded, width, n)
        # A signalling NaN does not survive a pass through binary64, so NaN
        # equals NaN whatever its payload; every other value compares as bits.
        if math.isnan(wanted) or math.isnan(got):
            return math.isnan(wanted) and math.isnan(got)
        return struct.pack("<f" if width == 4 or n else "<d", wanted) == struct.pack(
            "<f" if width == 4 or n else "<d", got
        )
    if kind == "string":
        return _string_bytes(expected) == decoded
    if kind == "bool":
        return bool(expected) == decoded
    if kind not in _INTEGER_RANGES:
        return _message_matches(repository, repository.load(kind), expected, decoded, narrow)
    return expected == decoded


def _field_value(value: Any, width: int, narrow: bool) -> float:
    """The value a float field holds, from a number, a `{bits}` or a decoded value."""
    if isinstance(value, Mapping):
        number = struct.unpack("<f" if width == 4 else "<d", int(value["bits"], 16).to_bytes(width, "little"))[0]
    else:
        number = float(value)
    return messages.narrow(number) if narrow and width == 8 else number


def _string_bytes(value: Any) -> bytes:
    return value.encode("utf-8") if isinstance(value, str) else bytes(value)


_WALK_SIZES = {0: 1, 1: 1, 2: 1, 3: 2, 4: 2, 5: 4, 6: 4, 7: 8, 8: 8}
_WALK_SIGNED = frozenset({2, 4, 6, 8})


def walk_encode(table, resolved, type_name, values, narrow=False):
    """The payload the descriptor table makes of `values`, walking entries the way the interpreter does."""
    out = bytearray()
    _walk_encode_message(table, resolved, type_name, values, narrow, out)
    return bytes(out)


def _walk_encode_message(table, resolved, type_name, values, narrow, out):
    record = table.records[table.index_of[type_name]]
    fields = resolved[type_name].fields
    entries = table.entries[record.first_field : record.first_field + record.field_count]
    if len(fields) != len(entries):
        raise AssertionError(f"{type_name}: {len(fields)} fields but {len(entries)} entries")
    for field, entry in zip(fields, entries, strict=True):
        items = values[field.name] if entry.mode != 0 else [values[field.name]]
        if entry.mode == 1 and len(items) != entry.count:
            raise AssertionError(f"{type_name}.{field.name}: {len(items)} elements but {entry.count} entries")
        if entry.mode in (2, 0x82):
            out += varint.encode(len(items))
        for item in items:
            _walk_encode_element(table, resolved, entry, item, narrow, out)


def _walk_encode_element(table, resolved, entry, value, narrow, out):
    kind = entry.kind
    if kind == 12:
        names = {index: name for name, index in table.index_of.items()}
        _walk_encode_message(table, resolved, names[entry.aux], value, narrow, out)
    elif kind == 11:
        data = bytes(value) if isinstance(value, (bytes, bytearray)) else value.encode("utf-8")
        out += varint.encode(len(data)) + data
    elif kind == 0:
        out.append(int(value))
    elif kind in _WALK_SIZES:
        out += value.to_bytes(_WALK_SIZES[kind], "little", signed=kind in _WALK_SIGNED)
    elif kind in (9, 10):
        out += _walk_float_bytes(entry, value, narrow)


def _walk_float_bytes(entry, value, narrow):
    """The wire bytes of a float field: a `{bits}` pattern direct, a number packed, narrow in binary32."""
    width = 4 if entry.kind == 9 else 8
    if isinstance(value, Mapping):
        raw = bytes.fromhex(value["bits"])[::-1]
        if entry.kind == 9 or not narrow:
            return raw
        value = struct.unpack("<d", raw)[0]
    if width == 4 or narrow:
        try:
            return struct.pack("<f", value)
        except OverflowError:
            return struct.pack("<f", math.copysign(math.inf, value))
    return struct.pack("<d", value)


def walk_decode(table, resolved, type_name, payload, narrow=False):
    """The message the descriptor table reads from `payload`, walking entries the way the interpreter does."""
    return _walk_decode_message(table, resolved, type_name, payload, 0, narrow)[0]


def _walk_decode_message(table, resolved, type_name, payload, pos, narrow):
    record = table.records[table.index_of[type_name]]
    fields = resolved[type_name].fields
    entries = table.entries[record.first_field : record.first_field + record.field_count]
    if len(fields) != len(entries):
        raise AssertionError(f"{type_name}: {len(fields)} fields but {len(entries)} entries")
    values = {}
    for field, entry in zip(fields, entries, strict=True):
        if entry.mode == 0:
            item, pos = _walk_decode_element(table, resolved, entry, payload, pos, narrow)
            values[field.name] = item
        else:
            count = entry.count
            if entry.mode in (2, 0x82):
                count, used = varint.decode(payload[pos:])
                pos += used
            items = []
            for _ in range(count):
                item, pos = _walk_decode_element(table, resolved, entry, payload, pos, narrow)
                items.append(item)
            values[field.name] = items
    return values, pos


def _walk_decode_element(table, resolved, entry, payload, pos, narrow):
    kind = entry.kind
    if kind == 12:
        names = {index: name for name, index in table.index_of.items()}
        return _walk_decode_message(table, resolved, names[entry.aux], payload, pos, narrow)
    if kind == 11:
        length, used = varint.decode(payload[pos:])
        pos += used
        return bytes(payload[pos : pos + length]), pos + length
    if kind == 0:
        return bool(payload[pos]), pos + 1
    if kind in _WALK_SIZES:
        width = _WALK_SIZES[kind]
        return int.from_bytes(payload[pos : pos + width], "little", signed=kind in _WALK_SIGNED), pos + width
    width = 4 if kind == 9 or narrow else 8
    return struct.unpack("<f" if width == 4 else "<d", payload[pos : pos + width])[0], pos + width
