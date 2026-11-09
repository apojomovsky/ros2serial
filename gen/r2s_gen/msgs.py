"""
Message loading: .msg and .action definitions from the search paths and snapshots.

A repository resolves a canonical `package/msg/Type` or `package/action/Name`
name to the first matching file and parses it into fields and constants.
Nested types are loaded on demand, and the search order is the message paths,
the sourced ROS prefixes, then the snapshot for the configured distro.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

PRIMITIVES = frozenset(
    {
        'bool',
        'byte',
        'char',
        'float32',
        'float64',
        'int8',
        'uint8',
        'int16',
        'uint16',
        'int32',
        'uint32',
        'int64',
        'uint64',
        'string',
        'wchar',
        'wstring',
    }
)
WIDE_TYPES = frozenset({'wchar', 'wstring'})

SNAPSHOTS = Path(__file__).resolve().parent / 'snapshots'

_TYPE = re.compile(
    r'^(?P<base>[A-Za-z][A-Za-z0-9_]*(?:/[A-Za-z][A-Za-z0-9_]*){0,2})'
    r'(?:<=(?P<string_bound>\d+))?'
    r'(?:\[(?P<array>[^\]]*)\])?$'
)
_CONSTANT = re.compile(r'^(?P<type>\S+)\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*(?P<value>.*)$')
_FIELD_NAME = re.compile(r'^(?!.*__)(?!.*_$)[a-z][a-z0-9_]*$')


class MessageError(Exception):
    """
    A definition that cannot be loaded or resolved.

    `where` names the file, line, type or field.
    """

    def __init__(self, where: str, problem: str, fix: str):
        """
        Record where the definition broke and how to fix it.

        :param where: the file, line, type, or field.
        :param problem: what is wrong.
        :param fix: what to do about it.
        """
        super().__init__(f'{where}: {problem}; {fix}')
        self.where = where
        self.problem = problem
        self.fix = fix


@dataclass(frozen=True)
class Constant:
    """One named constant of a message definition."""

    name: str
    type_name: str
    value: str


@dataclass(frozen=True)
class Field:
    """One field of a message definition."""

    name: str
    type_name: str
    is_array: bool = False
    array_length: int | None = None
    array_bound: int | None = None
    string_bound: int | None = None
    default: str | None = None


@dataclass(frozen=True)
class Message:
    """One parsed .msg definition."""

    type_name: str
    fields: tuple[Field, ...]
    constants: tuple[Constant, ...]
    path: Path

    def field(self, name: str) -> Field:
        """
        Return the field called `name`.

        :param name: the field name to look up.
        :return: the field.
        :raises KeyError: if no field is called `name`.
        """
        for field in self.fields:
            if field.name == name:
                return field
        raise KeyError(name)


@dataclass(frozen=True)
class Action:
    """One parsed .action definition: goal, result, and feedback."""

    type_name: str
    goal: Message
    result: Message
    feedback: Message
    path: Path


ACTION_SUFFIXES = ('_Goal', '_Result', '_Feedback')


def _action_base(type_name: str) -> str | None:
    for suffix in ACTION_SUFFIXES:
        if type_name.endswith(suffix):
            return type_name[: -len(suffix)]
    return None


def canonical_type_name(type_name: str) -> str:
    """
    Return the canonical `package/msg/Type` or `package/action/Name` form of a type.

    :param type_name: the type in any accepted form.
    :return: the canonical form.
    :raises MessageError: if the type has no accepted form.
    """
    parts = type_name.split('/')
    if len(parts) == 2:
        return f'{parts[0]}/msg/{parts[1]}'
    if len(parts) == 3 and parts[1] in ('msg', 'action'):
        return type_name
    raise MessageError(
        type_name,
        "expected a type as 'package/Type', 'package/msg/Type' or 'package/action/Type'",
        'check the type name',
    )


def _canonical_reference(reference: str, package: str, where: str) -> str:
    if '/' not in reference:
        return f'{package}/msg/{reference}'
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
        raise MessageError(
            where, f"'{text}' is not a length", 'write a whole number of at least 1'
        ) from None
    if value < 1:
        raise MessageError(
            where, f"'{text}' is not a length", 'write a whole number of at least 1'
        )
    return value


def _parse_type(token: str, package: str, where: str) -> dict:
    match = _TYPE.fullmatch(token)
    if match is None:
        raise MessageError(
            where, f"'{token}' is not a type", 'write a primitive type or package/Type'
        )
    base = match['base']
    string_bound = (
        _count(match['string_bound'], where) if match['string_bound'] is not None else None
    )
    array = match['array']
    if string_bound is not None and base not in ('string', 'wstring'):
        raise MessageError(where, f"'<=' bound on '{base}'", 'bound only a string this way')
    is_array = array is not None
    array_length = None
    array_bound = None
    if array is not None:
        if array.startswith('<='):
            array_bound = _count(array[2:], where)
        elif array:
            array_length = _count(array, where)
    type_name = base if base in PRIMITIVES else _canonical_reference(base, package, where)
    return {
        'type_name': type_name,
        'is_array': is_array,
        'array_length': array_length,
        'array_bound': array_bound,
        'string_bound': string_bound,
    }


def _parse_section(text: str, type_name: str, path: Path, base: int) -> Message:
    """One .action section as a message; line numbers count from the file start."""
    package = type_name.split('/')[0]
    fields: list[Field] = []
    constants: list[Constant] = []
    names: set[str] = set()
    for offset, raw in enumerate(text.splitlines()):
        number = base + offset
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        where = f'{path}:{number}'
        constant = _CONSTANT.fullmatch(line)
        if constant is not None:
            parsed = _parse_type(constant['type'], package, where)
            if parsed['is_array'] or parsed['string_bound'] is not None:
                raise MessageError(
                    where,
                    'a constant cannot be an array or a bounded string',
                    'declare it without a bound',
                )
            name = constant['name']
            if name in names:
                raise MessageError(where, f"'{name}' is declared twice", 'rename one of them')
            names.add(name)
            constants.append(Constant(name, parsed['type_name'], constant['value'].strip()))
            continue
        tokens = line.split(maxsplit=2)
        if len(tokens) < 2:
            raise MessageError(
                where, 'expected a type and a field name', f"write it as '{line} value'"
            )
        parsed = _parse_type(tokens[0], package, where)
        name = tokens[1]
        if _FIELD_NAME.fullmatch(name) is None:
            raise MessageError(
                where,
                f"'{name}' is not a field name",
                'use lowercase letters, digits and single underscores, starting with a letter',
            )
        if name in names:
            raise MessageError(where, f"'{name}' is declared twice", 'rename one of them')
        names.add(name)
        fields.append(Field(name=name, default=tokens[2] if len(tokens) == 3 else None, **parsed))
    return Message(
        type_name=type_name, fields=tuple(fields), constants=tuple(constants), path=path
    )


def parse_action(text: str, type_name: str, path: Path) -> Action:
    """Parse one .action file into its goal, result and feedback messages."""
    package, _, name = type_name.partition('/action/')
    lines = text.splitlines()
    marks = [index for index, raw in enumerate(lines, start=1) if raw.strip() == '---']
    if len(marks) != 2:
        raise MessageError(
            str(path),
            f"expected 2 '---' separators, found {len(marks)}",
            "write goal, '---', result, '---', feedback",
        )
    goal_text = '\n'.join(lines[: marks[0] - 1])
    result_text = '\n'.join(lines[marks[0] : marks[1] - 1])
    feedback_text = '\n'.join(lines[marks[1] :])
    goal = _parse_section(goal_text, f'{package}/action/{name}_Goal', path, 1)
    result = _parse_section(result_text, f'{package}/action/{name}_Result', path, marks[0] + 1)
    feedback = _parse_section(
        feedback_text, f'{package}/action/{name}_Feedback', path, marks[1] + 1
    )
    return Action(type_name=type_name, goal=goal, result=result, feedback=feedback, path=path)


def parse_message(text: str, type_name: str, path: Path) -> Message:
    """Parse one .msg file into its fields and constants; the first malformed line is the error."""
    return _parse_section(text, type_name, path, 1)


class MessageRepository:
    """
    Resolves message and action type names through the paths and a distro snapshot.

    A message path holds packages as `<package>/msg/<Type>.msg` and
    `<package>/action/<Name>.action`; a ROS prefix is read as
    `<prefix>/share/<package>/...` in the same layout.
    """

    def __init__(
        self,
        msg_paths: tuple[Path, ...] | list[Path] = (),
        ament_prefix_path: tuple[Path, ...] | list[Path] = (),
        distro: str = 'jazzy',
        sourced_distro: str | None = None,
        snapshot_root: Path | None = None,
    ):
        """
        Point the repository at message paths, ROS prefixes, and a snapshot distro.

        :param msg_paths: directories holding message packages.
        :param ament_prefix_path: sourced ROS prefixes to search.
        :param distro: the snapshot distro to fall back to.
        :param sourced_distro: the sourced ROS distro, if any.
        :param snapshot_root: the snapshots directory, defaulting to the shipped one.
        """
        self._msg_paths = tuple(Path(path) for path in msg_paths)
        self._ament_prefix_path = tuple(Path(path) for path in ament_prefix_path)
        self._distro = distro
        self._snapshot_root = Path(snapshot_root) if snapshot_root is not None else SNAPSHOTS
        self._cache: dict[str, Message] = {}
        self._actions: dict[str, Action] = {}
        self.warnings: tuple[str, ...] = ()
        if sourced_distro is not None and sourced_distro != distro:
            self.warnings = (
                f"the sourced ROS distro is '{sourced_distro}' but '{distro}' is configured; "
                'search paths may resolve different definitions',
            )

    def _files(self, package: str, name: str):
        relative = Path(package) / 'msg' / f'{name}.msg'
        for root in self._msg_paths:
            yield root / relative
        for prefix in self._ament_prefix_path:
            yield prefix / 'share' / relative
        yield self._snapshot_root / self._distro / relative

    def _action_files(self, package: str, name: str):
        relative = Path(package) / 'action' / f'{name}.action'
        for root in self._msg_paths:
            yield root / relative
        for prefix in self._ament_prefix_path:
            yield prefix / 'share' / relative
        yield self._snapshot_root / self._distro / relative

    def load_action(self, type_name: str) -> Action:
        """Load the action definition of `type_name`, from the first file that has it."""
        canonical = canonical_type_name(type_name)
        if '/action/' not in canonical:
            raise MessageError(canonical, 'not an action type', "name it as 'package/action/Name'")
        base = _action_base(canonical)
        action_name = canonical if base is None else base
        package, _, name = action_name.partition('/action/')
        cached = self._actions.get(action_name)
        if cached is not None:
            return cached
        for path in self._action_files(package, name):
            if path.is_file():
                action = parse_action(path.read_text(encoding='utf-8'), action_name, path)
                self._actions[action_name] = action
                self._cache[action.goal.type_name] = action.goal
                self._cache[action.result.type_name] = action.result
                self._cache[action.feedback.type_name] = action.feedback
                return action
        raise MessageError(
            action_name,
            'no action definition found',
            'add its package to --msg-path or check the type name',
        )

    def load(self, type_name: str) -> Message:
        """Load the definition of `type_name`, from the first file that has it."""
        canonical = canonical_type_name(type_name)
        cached = self._cache.get(canonical)
        if cached is not None:
            return cached
        if '/action/' in canonical:
            base = _action_base(canonical)
            if base is None:
                raise MessageError(
                    canonical,
                    'an action has no message of its own',
                    "name its goal, result or feedback as 'package/action/Name_Goal'",
                )
            action = self.load_action(base)
            if canonical == action.goal.type_name:
                return action.goal
            if canonical == action.result.type_name:
                return action.result
            if canonical == action.feedback.type_name:
                return action.feedback
            raise MessageError(  # pragma: no cover - every action suffix returns above
                canonical,
                'not a goal, result or feedback type',
                "name it as 'package/action/Name_Goal', '_Result' or '_Feedback'",
            )
        package, _, name = canonical.partition('/msg/')
        for path in self._files(package, name):
            if path.is_file():
                message = parse_message(path.read_text(encoding='utf-8'), canonical, path)
                self._cache[canonical] = message
                return message
        raise MessageError(
            canonical,
            'no message definition found',
            'add its package to --msg-path or check the type name',
        )
