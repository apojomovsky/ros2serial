"""C emission: golden headers per type, the mapping table, and C99 compilation."""

import subprocess
from pathlib import Path

import pytest
from r2s_gen import bounds, emit, msgs
from support import write_message

GOLDEN = Path(__file__).parent / "golden" / "types"
HEADER_BOUNDS = {"std_msgs/msg/Header.frame_id": 24}
JOINT_STATE_BOUNDS = {
    **HEADER_BOUNDS,
    "sensor_msgs/msg/JointState.name": [4, 12],
    "sensor_msgs/msg/JointState.position": 4,
    "sensor_msgs/msg/JointState.velocity": 4,
    "sensor_msgs/msg/JointState.effort": 4,
}

GOLDEN_CASES = [
    pytest.param("std_msgs/msg/Float32", {}, "float32.h", id="float32"),
    pytest.param("geometry_msgs/msg/Twist", {}, "twist.h", id="twist"),
    pytest.param("std_msgs/msg/Header", HEADER_BOUNDS, "header.h", id="header"),
    pytest.param("sensor_msgs/msg/JointState", JOINT_STATE_BOUNDS, "joint_state.h", id="joint_state"),
]


def repository_for(tmp_path: Path, definitions: dict[str, str]) -> msgs.MessageRepository:
    """A repository whose message path holds `definitions`, keyed by canonical type name."""
    root = tmp_path / "messages"
    for type_name, text in definitions.items():
        write_message(root, type_name, text)
    return msgs.MessageRepository(msg_paths=[root])


def emit_type(
    type_name: str,
    bound_map: dict | None = None,
    default_string_bound: int | None = None,
    repository: msgs.MessageRepository | None = None,
) -> str:
    """The generated header for one root type."""
    resolved = bounds.resolve(
        repository or msgs.MessageRepository(),
        [type_name],
        bounds=bound_map or {},
        default_string_bound=default_string_bound,
    )
    return emit.emit_types(resolved)


@pytest.mark.parametrize(("type_name", "bound_map", "golden"), GOLDEN_CASES)
def test_the_golden_types_are_emitted(type_name, bound_map, golden):
    assert emit_type(type_name, bound_map) == (GOLDEN / golden).read_text(encoding="utf-8")


@pytest.mark.parametrize(("type_name", "bound_map", "golden"), GOLDEN_CASES)
def test_the_golden_types_compile_as_c99(tmp_path, type_name, bound_map, golden):
    (tmp_path / "r2s_generated.h").write_text(emit_type(type_name, bound_map), encoding="utf-8")
    source = tmp_path / "main.c"
    source.write_text('#include "r2s_generated.h"\n\nint main(void)\n{\n    return 0;\n}\n', encoding="utf-8")

    compiled = subprocess.run(
        [
            "cc",
            "-std=c99",
            "-pedantic",
            "-Werror",
            "-I",
            str(tmp_path),
            "-c",
            str(source),
            "-o",
            str(tmp_path / "main.o"),
        ],
        capture_output=True,
        text=True,
    )

    assert compiled.returncode == 0, compiled.stderr


@pytest.mark.parametrize(
    ("declaration", "expected"),
    [
        ("bool flag", "uint8_t flag;"),
        ("byte raw", "uint8_t raw;"),
        ("char letter", "uint8_t letter;"),
        ("int8 a", "int8_t a;"),
        ("uint8 a", "uint8_t a;"),
        ("int16 a", "int16_t a;"),
        ("uint16 a", "uint16_t a;"),
        ("int32 a", "int32_t a;"),
        ("uint32 a", "uint32_t a;"),
        ("int64 a", "int64_t a;"),
        ("uint64 a", "uint64_t a;"),
        ("float32 a", "float a;"),
        ("float64 a", "double a;"),
    ],
)
def test_the_primitive_types_map_to_their_c_types(tmp_path, declaration, expected):
    text = emit_type("test_msgs/msg/Values", repository=repository_for(tmp_path, {"test_msgs/msg/Values": declaration}))

    assert expected in text


def test_a_scalar_string_is_a_nul_terminated_char_array(tmp_path):
    text = emit_type(
        "test_msgs/msg/Label",
        default_string_bound=8,
        repository=repository_for(tmp_path, {"test_msgs/msg/Label": "string name"}),
    )

    assert "char name[9];" in text


def test_a_fixed_array_keeps_its_length_and_gets_no_count(tmp_path):
    text = emit_type(
        "test_msgs/msg/Fixed",
        repository=repository_for(tmp_path, {"test_msgs/msg/Fixed": "uint8[4] values"}),
    )

    assert "uint8_t values[4];" in text
    assert "size;" not in text


@pytest.mark.parametrize(
    ("declaration", "expected"),
    [
        ("uint8[<=4] values", "    struct {\n        uint8_t data[4];\n        uint8_t size;\n    } values;"),
        ("uint8[<=256] values", "    struct {\n        uint8_t data[256];\n        uint16_t size;\n    } values;"),
    ],
)
def test_a_bounded_sequence_is_a_struct_with_its_count(tmp_path, declaration, expected):
    text = emit_type(
        "test_msgs/msg/Sequence",
        repository=repository_for(tmp_path, {"test_msgs/msg/Sequence": declaration}),
    )

    assert expected in text


def test_a_bounded_sequence_of_strings_holds_a_char_array_per_element(tmp_path):
    text = emit_type(
        "test_msgs/msg/Names",
        default_string_bound=5,
        repository=repository_for(tmp_path, {"test_msgs/msg/Names": "string[<=3] names"}),
    )

    assert "char data[3][6];" in text


def test_a_fixed_array_of_strings_holds_a_char_array_per_element(tmp_path):
    text = emit_type(
        "test_msgs/msg/Names",
        default_string_bound=5,
        repository=repository_for(tmp_path, {"test_msgs/msg/Names": "string[3] names"}),
    )

    assert "char names[3][6];" in text


def test_nested_types_are_defined_before_the_types_that_use_them(tmp_path):
    repository = repository_for(
        tmp_path,
        {"test_msgs/msg/Inner": "uint8 value", "test_msgs/msg/Outer": "Inner inner"},
    )

    text = emit_type("test_msgs/msg/Outer", repository=repository)

    assert text.index("} test_msgs__msg__Inner;") < text.index("test_msgs__msg__Inner inner;")


def test_a_shared_nested_type_is_defined_once():
    resolved = bounds.resolve(
        msgs.MessageRepository(),
        ["std_msgs/msg/Header", "sensor_msgs/msg/JointState"],
        bounds=JOINT_STATE_BOUNDS,
    )

    text = emit.emit_types(resolved)

    assert text.count("typedef struct std_msgs__msg__Header {") == 1


def test_the_order_of_the_resolved_types_does_not_change_the_output():
    resolved = bounds.resolve(msgs.MessageRepository(), ["geometry_msgs/msg/Twist"])

    assert emit.emit_types(resolved) == emit.emit_types(dict(reversed(list(resolved.items()))))


def test_an_empty_message_gets_the_member_c99_requires(tmp_path):
    text = emit_type(
        "test_msgs/msg/Empty",
        repository=repository_for(tmp_path, {"test_msgs/msg/Empty": ""}),
    )

    assert "uint8_t structure_needs_at_least_one_member;" in text


def test_a_wide_bounded_sequence_of_odd_storage_is_padded_before_its_count(tmp_path):
    text = emit_type(
        "test_msgs/msg/Sequence",
        repository=repository_for(tmp_path, {"test_msgs/msg/Sequence": "uint8[<=257] values"}),
    )

    assert (
        "    struct {\n        uint8_t data[257];\n        uint8_t _pad;\n        uint16_t size;\n    } values;" in text
    )


def test_a_wide_bounded_sequence_of_even_storage_has_no_padding(tmp_path):
    text = emit_type(
        "test_msgs/msg/Sequence",
        repository=repository_for(tmp_path, {"test_msgs/msg/Sequence": "uint8[<=256] values"}),
    )

    assert "        uint8_t data[256];\n        uint16_t size;" in text


def test_a_wide_bounded_sequence_of_odd_nested_messages_is_padded(tmp_path):
    repository = repository_for(
        tmp_path,
        {"test_msgs/msg/Inner": "uint8[3] data", "test_msgs/msg/Outer": "Inner[<=257] values"},
    )

    text = emit_type("test_msgs/msg/Outer", repository=repository)

    assert "        test_msgs__msg__Inner data[257];\n        uint8_t _pad;\n        uint16_t size;" in text


def test_a_wide_bounded_sequence_of_even_nested_messages_has_no_padding(tmp_path):
    repository = repository_for(
        tmp_path,
        {"test_msgs/msg/Inner": "uint16 value", "test_msgs/msg/Outer": "Inner[<=257] values"},
    )

    text = emit_type("test_msgs/msg/Outer", repository=repository)

    assert "_pad" not in text


def test_the_count_of_a_wide_bounded_sequence_is_padded_on_avr(tmp_path):
    header = emit_type(
        "test_msgs/msg/Sequence",
        repository=repository_for(tmp_path, {"test_msgs/msg/Sequence": "uint8[<=257] values"}),
    )
    (tmp_path / "r2s_generated.h").write_text(header, encoding="utf-8")
    source = tmp_path / "offset.c"
    source.write_text(
        "#include <stddef.h>\n"
        '#include "r2s_generated.h"\n'
        "\n"
        "typedef char count_at_258["
        "(offsetof(test_msgs__msg__Sequence, values.size) == 258) ? 1 : -1];\n",
        encoding="utf-8",
    )

    compiled = subprocess.run(
        [
            "avr-gcc",
            "-mmcu=atmega328p",
            "-std=c99",
            "-pedantic",
            "-Werror",
            "-I",
            str(tmp_path),
            "-c",
            str(source),
            "-o",
            str(tmp_path / "offset.o"),
        ],
        capture_output=True,
        text=True,
    )

    assert compiled.returncode == 0, compiled.stderr


def test_a_wide_bounded_sequence_of_odd_strings_is_padded(tmp_path):
    text = emit_type(
        "test_msgs/msg/Names",
        default_string_bound=4,
        repository=repository_for(tmp_path, {"test_msgs/msg/Names": "string[<=301] names"}),
    )

    assert "        char data[301][5];\n        uint8_t _pad;\n        uint16_t size;" in text


def test_a_wide_bounded_sequence_inside_a_nested_message_takes_even_storage(tmp_path):
    repository = repository_for(
        tmp_path,
        {"test_msgs/msg/Inner": "uint8[<=300] data", "test_msgs/msg/Outer": "Inner[<=257] values"},
    )

    text = emit_type("test_msgs/msg/Outer", repository=repository)

    assert "        test_msgs__msg__Inner data[257];\n        uint16_t size;" in text


def test_a_wide_bounded_sequence_of_empty_messages_is_padded(tmp_path):
    repository = repository_for(
        tmp_path,
        {"test_msgs/msg/Inner": "", "test_msgs/msg/Outer": "Inner[<=257] values"},
    )

    text = emit_type("test_msgs/msg/Outer", repository=repository)

    assert "        test_msgs__msg__Inner data[257];\n        uint8_t _pad;\n        uint16_t size;" in text
