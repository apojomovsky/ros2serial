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
from r2s_gen.reference import messages

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
