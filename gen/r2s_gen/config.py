"""Config loading: r2s.yaml into a validated model, with golden error messages.

Validation runs against the shipped JSON Schema, and every rejection becomes a
MessageError of file, key path, what is wrong and how to fix it. The command
line prints those unchanged, so the golden tests pin both layers at once.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from r2s_gen.msgs import MessageError
from r2s_gen.topics import check_collisions, check_non_empty

SCHEMA_PATH = Path(__file__).resolve().parent / "schema" / "r2s.schema.json"

_REQUIRED_FIX = {
    "node": "add node: <name>",
    "topics": "add topics: with one entry per topic",
    "name": "add name: <topic>",
    "type": "add type: <package/msg/Type>",
    "direction": "add direction: pub or direction: sub",
}

_TYPE_PROBLEM_FIX = {
    "node": ("expected a string", "write the node name, such as node: base_controller"),
    "features": ("expected a list", "write one feature per line, such as features: [time, log]"),
    "topics": ("expected a list", "write one entry per topic, starting each with - name: <topic>"),
    "name": ("expected a string", "write the topic name, such as name: battery"),
    "type": ("expected a string", "write package/msg/Type, such as std_msgs/msg/Float32"),
    "direction": ("expected a string", "write direction: pub or direction: sub"),
    "qos": ("expected a string", "use one of: best_effort, keep_latest, reliable"),
    "narrow_float64": (
        "expected true or false",
        "write narrow_float64: true to send float64 fields as binary32",
    ),
    "bounds": ("expected a mapping", "write bounds: with one package/msg/Type.field per line"),
    "default_string_bound": ("expected an integer", "give a whole number of characters"),
    "limits": ("expected a mapping", "write limits: with max_frame: <bytes>"),
    "max_frame": ("expected an integer", "give a whole number of bytes"),
}

_ENUM_KIND = {"distro": "a known distro", "features": "a known feature"}

_ARTICLE = {
    "array": "a list",
    "object": "a mapping",
    "string": "a string",
    "integer": "an integer",
    "boolean": "a boolean",
}


@dataclass(frozen=True)
class Topic:
    name: str
    type_name: str
    direction: str
    qos: str = "best_effort"
    narrow_float64: bool = False


@dataclass(frozen=True)
class Config:
    node: str
    topics: tuple[Topic, ...] = ()
    distro: str = "jazzy"
    handshake: str = "full"
    tier: str = "M"
    features: tuple[str, ...] = ("time", "log")
    bounds: dict[str, Any] = field(default_factory=dict)
    default_string_bound: int | None = None
    max_frame: int | None = None


def load(path: str | Path) -> Config:
    """Read and validate the config file; every problem is a MessageError."""
    raw = _read(path)
    data = _parse(path, raw)
    _string_keys(path, data)
    _check_schema(path, data)
    return _model(path, data)


def _string_keys(path: str | Path, data: dict) -> None:
    """Every mapping key must be a string; schema validation assumes string keys."""
    _string_keys_at(str(path), data)
    topics = data.get("topics")
    if isinstance(topics, list):
        for index, entry in enumerate(topics):
            if isinstance(entry, dict):
                _string_keys_at(f"{path}: topics[{index}]", entry)
    for section in ("bounds", "limits"):
        mapping = data.get(section)
        if isinstance(mapping, dict):
            _string_keys_at(f"{path}: {section}", mapping)


def _string_keys_at(where: str, mapping: dict) -> None:
    for key in mapping:
        if not isinstance(key, str):
            raise MessageError(where, f"key {key} must be a string", "quote the key")


def add_parser(subparsers: argparse._SubParsersAction) -> argparse.ArgumentParser:
    """Add the `check` subcommand to the command line."""
    parser = subparsers.add_parser(
        "check",
        help="validate r2s.yaml and report the first problem",
        description="Validate the generator config against its schema; exit 1 naming the first problem.",
    )
    parser.add_argument("--config", required=True, help="the config file to validate")
    parser.add_argument(
        "--msg-path", action="append", default=[], metavar="DIR", help="search DIR for message packages; repeatable"
    )
    parser.set_defaults(run=run)
    return parser


def run(args: argparse.Namespace) -> int:
    """Validate the config file; a bad config is exit 1 with its golden message."""
    # Message paths feed type resolution, which lands with G3; the schema check ignores them.
    try:
        load(Path(args.config))
    except MessageError as error:
        print(f"r2s-gen: error: {error}", file=sys.stderr)
        return 1
    return 0


def _read(path: str | Path) -> str:
    try:
        return Path(path).read_text(encoding="utf-8")
    except UnicodeDecodeError as error:
        raise MessageError(str(path), f"cannot read the config ({error})", "save the file as UTF-8") from error
    except OSError as error:
        raise MessageError(str(path), f"cannot read the config ({error})", "check the path") from error


def _parse(path: str | Path, raw: str) -> Any:
    try:
        import yaml
    except ImportError as error:
        raise MessageError(
            str(path), "cannot read the config without PyYAML", "install it with pip install pyyaml"
        ) from error
    try:
        data = yaml.safe_load(raw)
    except yaml.YAMLError as error:
        raise MessageError(str(path), "not valid YAML", "fix the indentation and quoting") from error
    if not isinstance(data, dict):
        raise MessageError(str(path), "the config must be a mapping of keys", "start the file with node: <name>")
    return data


def _check_schema(path: str | Path, data: dict) -> None:
    try:
        import jsonschema
    except ImportError as error:
        raise MessageError(
            str(path), "cannot validate the config without jsonschema", "install it with pip install jsonschema"
        ) from error
    schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    validator = jsonschema.validators.validator_for(schema)(schema)
    errors = sorted(validator.iter_errors(data), key=lambda e: (_location(e.absolute_path), e.validator))
    if errors:
        raise _describe(str(path), data, errors[0])


def _location(parts: Any) -> str:
    """A key path such as topics[0].name, or bounds['pkg/msg/Type.field'] for dotted keys."""
    out = ""
    for part in parts:
        if isinstance(part, int):
            out += f"[{part}]"
        elif "." in part:
            out += f"['{part}']"
        else:
            out += f".{part}" if out else part
    return out


def _describe(path: str, data: dict, error: Any) -> MessageError:
    """The golden message for a schema rejection: file, key path, what is wrong, how to fix it."""
    parts = list(error.absolute_path)
    location = _location(parts)
    where = path if not location else f"{path}: {location}"
    if len(parts) == 2 and parts[0] == "bounds" and parts[1] != "default_string_bound":
        return _describe_bound(where, data["bounds"][parts[1]])
    if error.validator == "required":
        missing = sorted(key for key in error.validator_value if key not in error.instance)[0]
        return MessageError(where, f"missing required key '{missing}'", _REQUIRED_FIX[missing])
    if error.validator == "additionalProperties":
        return _describe_unknown(where, error)
    if error.validator == "type":
        return _describe_type(where, error)
    if error.validator == "enum":
        return _describe_enum(where, error)
    if error.validator == "pattern":
        return _describe_pattern(where, error)
    if error.validator == "minimum":
        return _describe_minimum(where, error)
    return MessageError(where, error.message, "fix the value")


def _describe_bound(where: str, value: Any) -> MessageError:
    """The golden message for a bounds value, read from the value itself."""
    if isinstance(value, int) and not isinstance(value, bool):
        return MessageError(where, "the bound must be at least 0", "raise it")
    if not isinstance(value, list):
        return MessageError(
            where,
            "expected an integer bound or [count, string_bound]",
            "write 24 or [4, 12]",
        )
    if len(value) != 2:
        return MessageError(
            where,
            "a list bound is [count, string_bound]",
            "give the count and the string bound",
        )
    count, element = value
    if isinstance(count, bool) or not isinstance(count, int):
        return MessageError(where, "the sequence bound must be an integer", "give a whole number")
    if count < 1:
        return MessageError(where, "the sequence bound must be at least 1", "raise it")
    if isinstance(element, bool) or not isinstance(element, int):
        return MessageError(where, "the string bound must be an integer", "give a whole number of characters")
    return MessageError(where, "the string bound must be at least 0", "raise it")


def _describe_unknown(path: str, error: Any) -> MessageError:
    properties = error.schema.get("properties", {})
    patterns = error.schema.get("patternProperties", {})
    unknown = sorted(
        key
        for key in error.instance
        if key not in properties and not any(re.fullmatch(pattern, str(key)) for pattern in patterns)
    )[0]
    if _location(error.absolute_path) == "bounds":
        return MessageError(
            path,
            f"'{unknown}' is not a field path",
            "write package/msg/Type.field, such as std_msgs/msg/Header.frame_id",
        )
    expected = ", ".join(sorted(properties))
    return MessageError(path, f"unknown key '{unknown}'", f"remove it (expected one of: {expected})")


def _describe_type(path: str, error: Any) -> MessageError:
    parts = list(error.absolute_path)
    if len(parts) == 2 and parts[0] == "topics" and isinstance(parts[1], int):
        return MessageError(path, "expected a topic entry", "write - name: <topic> with type: and direction:")
    key = parts[-1] if parts and isinstance(parts[-1], str) else ""
    problem, fix = _TYPE_PROBLEM_FIX.get(
        key, (f"expected {_ARTICLE.get(error.validator_value, 'a')} {error.validator_value}", "fix the value")
    )
    return MessageError(path, problem, fix)


def _describe_enum(path: str, error: Any) -> MessageError:
    parts = list(error.absolute_path)
    key = parts[-2] if parts and isinstance(parts[-1], int) else (parts[-1] if parts else "")
    kind = _ENUM_KIND.get(key, "valid")
    return MessageError(path, f"{error.instance!r} is not {kind}", f"use one of: {', '.join(error.validator_value)}")


def _describe_pattern(path: str, error: Any) -> MessageError:
    key = error.absolute_path[-1]
    value = error.instance
    if key == "node":
        return MessageError(
            path,
            f"'{value}' is not a valid ROS node name",
            "use letters, digits and underscores, starting with a letter",
        )
    if key == "name":
        if isinstance(value, str) and value.startswith("/"):
            return MessageError(
                path,
                f"'{value}' is not a relative topic name",
                "drop the leading slash, such as battery",
            )
        return MessageError(
            path,
            f"'{value}' is not a valid topic name",
            "use letters, digits, underscores and slashes, starting with a letter or underscore",
        )
    return MessageError(
        path,
        f"'{value}' is not a message type",
        "write package/msg/Type, such as std_msgs/msg/Float32",
    )


def _describe_minimum(path: str, error: Any) -> MessageError:
    key = error.absolute_path[-1] if error.absolute_path else ""
    if key == "default_string_bound":
        return MessageError(path, f"must be at least {error.validator_value}", "raise it")
    if key == "max_frame":
        return MessageError(path, f"must be at least {error.validator_value}", "raise it")
    return MessageError(path, f"the bound must be at least {error.validator_value}", "raise it")


def _model(path: str | Path, data: dict) -> Config:
    """The validated mapping as a Config; the schema already pins every shape but uniqueness."""
    topics = []
    seen = set()
    for index, entry in enumerate(data["topics"]):
        if entry["name"] in seen:
            raise MessageError(
                f"{path}: topics[{index}].name",
                f"duplicate topic name '{entry['name']}'",
                "give each topic its own name",
            )
        seen.add(entry["name"])
        topics.append(
            Topic(
                name=entry["name"],
                type_name=entry["type"],
                direction=entry["direction"],
                qos=entry.get("qos", "best_effort"),
                narrow_float64=entry.get("narrow_float64", False),
            )
        )
    bounds = {key: value for key, value in data.get("bounds", {}).items() if key != "default_string_bound"}
    check_non_empty([topic.name for topic in topics], str(path))
    check_collisions([topic.name for topic in topics], str(path))
    return Config(
        node=data["node"],
        topics=tuple(topics),
        distro=data.get("distro", "jazzy"),
        handshake=data.get("handshake", "full"),
        tier=data.get("tier", "M"),
        features=tuple(data.get("features", ["time", "log"])),
        bounds=bounds,
        default_string_bound=data.get("bounds", {}).get("default_string_bound"),
        max_frame=data.get("limits", {}).get("max_frame"),
    )
