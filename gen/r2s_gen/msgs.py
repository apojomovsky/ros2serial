"""Message loading: .msg definitions from the search paths and the bundled snapshots.

A repository resolves a canonical `package/msg/Type` name to the first matching
file and parses it into fields and constants. Nested types are loaded on demand,
and the search order is the message paths, the sourced ROS prefixes, then the
snapshot for the configured distro.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

PRIMITIVES = frozenset(
    {
        "bool",
        "byte",
        "char",
        "float32",
        "float64",
        "int8",
        "uint8",
        "int16",
        "uint16",
        "int32",
        "uint32",
        "int64",
        "uint64",
        "string",
        "wchar",
        "wstring",
    }
)
WIDE_TYPES = frozenset({"wchar", "wstring"})

SNAPSHOTS = Path(__file__).resolve().parent / "snapshots"

_TYPE = re.compile(
    r"^(?P<base>[A-Za-z][A-Za-z0-9_]*(?:/[A-Za-z][A-Za-z0-9_]*){0,2})"
    r"(?:<=(?P<string_bound>\d+))?"
    r"(?:\[(?P<array>[^\]]*)\])?$"
)
_CONSTANT = re.compile(r"^(?P<type>\S+)\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*(?P<value>.*)$")
_FIELD_NAME = re.compile(r"^(?!.*__)(?!.*_$)[a-z][a-z0-9_]*$")


class MessageError(Exception):
    """A definition that cannot be loaded or resolved; `where` names the file, line, type or field."""

    def __init__(self, where: str, problem: str, fix: str):
        super().__init__(f"{where}: {problem}; {fix}")
        self.where = where
        self.problem = problem
        self.fix = fix


@dataclass(frozen=True)
class Constant:
    name: str
    type_name: str
    value: str


@dataclass(frozen=True)
class Field:
    name: str
    type_name: str
    is_array: bool = False
    array_length: int | None = None
    array_bound: int | None = None
    string_bound: int | None = None
    default: str | None = None


@dataclass(frozen=True)
class Message:
    type_name: str
    fields: tuple[Field, ...]
    constants: tuple[Constant, ...]
    path: Path

    def field(self, name: str) -> Field:
        for field in self.fields:
            if field.name == name:
                return field
        raise KeyError(name)


def canonical_type_name(type_name: str) -> str:
    """`package/Type` or `package/msg/Type` to the canonical `package/msg/Type`."""
    parts = type_name.split("/")
    if len(parts) == 2:
        return f"{parts[0]}/msg/{parts[1]}"
    if len(parts) == 3 and parts[1] == "msg":
        return type_name
    raise MessageError(
        type_name,
        "expected a type as 'package/Type' or 'package/msg/Type'",
        "check the type name",
    )


def _canonical_reference(reference: str, package: str, where: str) -> str:
    if "/" not in reference:
        return f"{package}/msg/{reference}"
    try:
        return canonical_type_name(reference)
    except MessageError:
        raise MessageError(
            where,
            f"'{reference}' is not a type",
            "write a primitive type, a same-package type, or 'package/Type'",
        ) from None


def _count(text: str, where: str) -> int:
    try:
        value = int(text)
    except ValueError:
        raise MessageError(where, f"'{text}' is not a length", "write a whole number of at least 1") from None
    if value < 1:
        raise MessageError(where, f"'{text}' is not a length", "write a whole number of at least 1")
    return value


def _parse_type(token: str, package: str, where: str) -> dict:
    match = _TYPE.fullmatch(token)
    if match is None:
        raise MessageError(where, f"'{token}' is not a type", "write a primitive type or package/Type")
    base = match["base"]
    string_bound = _count(match["string_bound"], where) if match["string_bound"] is not None else None
    array = match["array"]
    if string_bound is not None and base not in ("string", "wstring"):
        raise MessageError(where, f"'<=' bound on '{base}'", "bound only a string this way")
    is_array = array is not None
    array_length = None
    array_bound = None
    if array is not None:
        if array.startswith("<="):
            array_bound = _count(array[2:], where)
        elif array:
            array_length = _count(array, where)
    type_name = base if base in PRIMITIVES else _canonical_reference(base, package, where)
    return {
        "type_name": type_name,
        "is_array": is_array,
        "array_length": array_length,
        "array_bound": array_bound,
        "string_bound": string_bound,
    }


def parse_message(text: str, type_name: str, path: Path) -> Message:
    """Parse one .msg file into its fields and constants; the first malformed line is the error."""
    package = type_name.split("/")[0]
    fields: list[Field] = []
    constants: list[Constant] = []
    names: set[str] = set()
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        where = f"{path}:{number}"
        constant = _CONSTANT.fullmatch(line)
        if constant is not None:
            parsed = _parse_type(constant["type"], package, where)
            if parsed["is_array"] or parsed["string_bound"] is not None:
                raise MessageError(
                    where, "a constant cannot be an array or a bounded string", "declare it without a bound"
                )
            name = constant["name"]
            if name in names:
                raise MessageError(where, f"'{name}' is declared twice", "rename one of them")
            names.add(name)
            constants.append(Constant(name, parsed["type_name"], constant["value"].strip()))
            continue
        tokens = line.split(maxsplit=2)
        if len(tokens) < 2:
            raise MessageError(where, "expected a type and a field name", f"write it as '{line} value'")
        parsed = _parse_type(tokens[0], package, where)
        name = tokens[1]
        if _FIELD_NAME.fullmatch(name) is None:
            raise MessageError(
                where,
                f"'{name}' is not a field name",
                "use lowercase letters, digits and single underscores, starting with a letter",
            )
        if name in names:
            raise MessageError(where, f"'{name}' is declared twice", "rename one of them")
        names.add(name)
        fields.append(Field(name=name, default=tokens[2] if len(tokens) == 3 else None, **parsed))
    return Message(type_name=type_name, fields=tuple(fields), constants=tuple(constants), path=path)


class MessageRepository:
    """Resolves message type names through the message paths, the ROS prefixes and a distro snapshot.

    A message path is a directory of packages (`<package>/msg/<Type>.msg`); a ROS
    prefix is read as `<prefix>/share/<package>/msg/<Type>.msg`.
    """

    def __init__(
        self,
        msg_paths: tuple[Path, ...] | list[Path] = (),
        ament_prefix_path: tuple[Path, ...] | list[Path] = (),
        distro: str = "jazzy",
        sourced_distro: str | None = None,
        snapshot_root: Path | None = None,
    ):
        self._msg_paths = tuple(Path(path) for path in msg_paths)
        self._ament_prefix_path = tuple(Path(path) for path in ament_prefix_path)
        self._distro = distro
        self._snapshot_root = Path(snapshot_root) if snapshot_root is not None else SNAPSHOTS
        self._cache: dict[str, Message] = {}
        self.warnings: tuple[str, ...] = ()
        if sourced_distro is not None and sourced_distro != distro:
            self.warnings = (
                f"the sourced ROS distro is '{sourced_distro}' but '{distro}' is configured; "
                "search paths may resolve different definitions",
            )

    def _files(self, package: str, name: str):
        relative = Path(package) / "msg" / f"{name}.msg"
        for root in self._msg_paths:
            yield root / relative
        for prefix in self._ament_prefix_path:
            yield prefix / "share" / relative
        yield self._snapshot_root / self._distro / relative

    def load(self, type_name: str) -> Message:
        """The definition of `type_name`, from the first file that has it."""
        canonical = canonical_type_name(type_name)
        cached = self._cache.get(canonical)
        if cached is not None:
            return cached
        package, _, name = canonical.partition("/msg/")
        for path in self._files(package, name):
            if path.is_file():
                message = parse_message(path.read_text(encoding="utf-8"), canonical, path)
                self._cache[canonical] = message
                return message
        raise MessageError(
            canonical,
            "no message definition found",
            "add its package to --msg-path or check the type name",
        )
