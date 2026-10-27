"""C emission: descriptor tables in the spec section 9 format, and their C99 compilation."""

import random
import subprocess
from pathlib import Path

import pytest
from r2s_gen import bounds, descriptors, emit, msgs
from r2s_gen.reference import messages
from support import SEED, matches, random_message, walk_decode, walk_encode, write_message

GOLDEN = Path(__file__).parent / "golden" / "descriptors"
FLOAT32 = "std_msgs/msg/Float32"
TWIST = "geometry_msgs/msg/Twist"
VECTOR3 = "geometry_msgs/msg/Vector3"
HEADER_BOUNDS = {"std_msgs/msg/Header.frame_id": 130}
JOINT_STATE_BOUNDS = {
    **HEADER_BOUNDS,
    "sensor_msgs/msg/JointState.name": [130, 130],
    "sensor_msgs/msg/JointState.position": 130,
    "sensor_msgs/msg/JointState.velocity": 130,
    "sensor_msgs/msg/JointState.effort": 130,
}
ROUND_TRIP_BOUNDS = {
    FLOAT32: {},
    TWIST: {},
    VECTOR3: {},
    "builtin_interfaces/msg/Time": {},
    "geometry_msgs/msg/Quaternion": {},
    "sensor_msgs/msg/Imu": {},
    "std_msgs/msg/Header": HEADER_BOUNDS,
    "sensor_msgs/msg/JointState": JOINT_STATE_BOUNDS,
}
COVER_FIELDS = """bool flag
byte b
char c
int8 i8
uint8 u8
int16 i16
uint16 u16
int32 i32
uint32 u32
int64 i64
uint64 u64
float32 f32
float64 f64
string<=130 text
uint8[4] fixed
int16[<=130] narrow_seq
uint64[<=300] wide_seq
test_msgs/msg/Inner inner
test_msgs/msg/Inner[3] fixed_nested
test_msgs/msg/Inner[<=130] seq_nested
string<=130[4] fixed_strings
string<=130[<=130] seq_strings
"""


def repository_for(tmp_path, definitions):
    """A repository whose message path holds `definitions`, keyed by canonical type name."""
    root = tmp_path / "messages"
    for type_name, text in definitions.items():
        write_message(root, type_name, text)
    return msgs.MessageRepository(msg_paths=[root])


def resolve(repository, roots, bound_map=None):
    """The bounded messages for `roots`, ready to build a table from."""
    return bounds.resolve(repository, roots, bounds=bound_map or {})


def build(roots, bound_map=None, repository=None):
    """The descriptor table for `roots` with `bound_map` from the default repository."""
    resolved = resolve(repository or msgs.MessageRepository(), roots, bound_map)
    return resolved, descriptors.build(resolved, roots)


def compiled_bytes(tmp_path, resolved, roots):
    """The type records and field entries the emitted C holds, read from a host run."""
    table = descriptors.build(resolved, roots)
    (tmp_path / "r2s_generated.h").write_text(emit.emit_types(resolved), encoding="utf-8")
    (tmp_path / "r2s_descriptors.c").write_text(descriptors.emit_c(table), encoding="utf-8")
    dump = tmp_path / "dump.c"
    dump.write_text(
        "\n".join(
            [
                "#include <stdio.h>",
                "#include <stdint.h>",
                "extern const uint8_t r2s_type_records[];",
                "extern const uint8_t r2s_field_entries[];",
                "int main(void)",
                "{",
                f"    fwrite(r2s_type_records, 1, {len(table.records) * 5}, stdout);",
                f"    fwrite(r2s_field_entries, 1, {len(table.entries) * 8}, stdout);",
                "    return 0;",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    binary = tmp_path / "dump"
    compiled = subprocess.run(
        [
            "cc",
            "-std=c99",
            "-pedantic",
            "-Werror",
            "-I",
            str(tmp_path),
            str(dump),
            str(tmp_path / "r2s_descriptors.c"),
            "-o",
            str(binary),
        ],
        capture_output=True,
        text=True,
    )
    assert compiled.returncode == 0, compiled.stderr
    run = subprocess.run([str(binary)], capture_output=True)
    assert run.returncode == 0
    split = len(table.records) * 5
    return run.stdout[:split], run.stdout[split:]


def test_float32_descriptor_bytes_match_the_hand_derivation(tmp_path):
    # covers: DSC-1, DSC-2, DSC-3, DSC-4, DSC-5
    resolved, _ = build([FLOAT32])
    records, entries = compiled_bytes(tmp_path, resolved, [FLOAT32])

    assert records.hex(" ") == "04 00 00 00 01"
    assert entries.hex(" ") == "09 00 00 00 00 00 00 00"


def test_twist_descriptor_bytes_match_the_hand_derivation(tmp_path):
    # covers: DSC-6, DSC-9, DSC-11, DSC-12, DSC-17, DSC-18, DSC-19, DSC-21, DSC-47
    resolved, _ = build([FLOAT32, TWIST])
    records, entries = compiled_bytes(tmp_path, resolved, [FLOAT32, TWIST])

    assert records.hex(" ") == "04 00 00 00 01 18 00 01 00 03 30 00 04 00 02"
    assert entries.hex(" ") == (
        "09 00 00 00 00 00 00 00"
        " 0a 00 00 00 00 00 00 00"
        " 0a 00 08 00 00 00 00 00"
        " 0a 00 10 00 00 00 00 00"
        " 0c 00 00 00 00 00 01 00"
        " 0c 00 18 00 00 00 01 00"
    )


def test_an_empty_message_has_a_record_with_no_entries(tmp_path):
    # covers: DSC-7
    repository = repository_for(tmp_path, {"test_msgs/msg/Empty": ""})
    _, table = build(["test_msgs/msg/Empty"], repository=repository)

    assert [(record.field_count, record.first_field) for record in table.records] == [(0, 0)]
    assert table.entries == ()


def test_entries_follow_declaration_order_with_nested_types_before_their_owners(tmp_path):
    # covers: DSC-8, DSC-10
    _, table = build([FLOAT32, TWIST])

    assert [record.type_name for record in table.records] == [FLOAT32, VECTOR3, TWIST]
    assert [entry.field for entry in table.entries] == ["data", "x", "y", "z", "linear", "angular"]
    assert [entry.owner for entry in table.entries] == [FLOAT32, VECTOR3, VECTOR3, VECTOR3, TWIST, TWIST]

    _, repeated = build([TWIST, TWIST])

    assert [record.type_name for record in repeated.records] == [VECTOR3, TWIST]


def test_every_entry_states_a_valid_kind_mode_count_and_aux(tmp_path):
    # covers: DSC-13, DSC-42, DSC-43, DSC-44, DSC-45, DSC-46
    repository = repository_for(
        tmp_path, {"test_msgs/msg/Inner": "uint8 tag\nfloat64 value", "test_msgs/msg/Cover": COVER_FIELDS}
    )
    _, table = build(["test_msgs/msg/Cover"], repository=repository)

    for entry in table.entries:
        assert 0 <= entry.kind <= 12
        assert entry.mode in (0, 1, 2, 0x82)
        assert (entry.count == 0) == (entry.mode == 0)
        assert (entry.mode == 0x82) == (entry.count > 255)
        assert (entry.aux == 0) == (entry.kind not in (11, 12))
        if entry.kind == 12:
            assert entry.aux < table.index_of[entry.owner]


def test_a_wide_sequence_counts_its_elements_in_two_bytes(tmp_path):
    # covers: DSC-14, DSC-15, DSC-20
    repository = repository_for(tmp_path, {"test_msgs/msg/Wide": "uint64[<=300] values"})
    resolved, table = build(["test_msgs/msg/Wide"], repository=repository)

    assert [(entry.mode, entry.count) for entry in table.entries] == [(0x82, 300)]
    (tmp_path / "r2s_generated.h").write_text(emit.emit_types(resolved), encoding="utf-8")
    (tmp_path / "check.c").write_text(
        '#include "r2s_generated.h"\n' + descriptors.emit_c(table) + "\nint main(void)\n{\n    return 0;\n}\n",
        encoding="utf-8",
    )
    for compiler, args in (("cc", []), ("avr-gcc", ["-mmcu=atmega328p"])):
        compiled = subprocess.run(
            [
                compiler,
                *args,
                "-std=c99",
                "-pedantic",
                "-Werror",
                "-I",
                str(tmp_path),
                "-c",
                str(tmp_path / "check.c"),
                "-o",
                str(tmp_path / "check.o"),
            ],
            capture_output=True,
            text=True,
        )
        assert compiled.returncode == 0, f"{compiler}: {compiled.stderr}"


def test_a_narrow_sequence_counts_its_elements_in_one_byte(tmp_path):
    # covers: DSC-14, DSC-15
    repository = repository_for(tmp_path, {"test_msgs/msg/Narrow": "int16[<=130] values"})
    _, table = build(["test_msgs/msg/Narrow"], repository=repository)

    assert [(entry.mode, entry.count) for entry in table.entries] == [(2, 130)]


def test_an_unbounded_sequence_has_no_descriptor_form(tmp_path):
    # covers: DSC-22, DSC-41
    repository = repository_for(tmp_path, {"test_msgs/msg/Open": "uint8[] values"})

    with pytest.raises(msgs.MessageError):
        resolve(repository, ["test_msgs/msg/Open"])


def test_an_unbounded_string_has_no_descriptor_form(tmp_path):
    # covers: DSC-22, DSC-41
    repository = repository_for(tmp_path, {"test_msgs/msg/Open": "string text"})

    with pytest.raises(msgs.MessageError):
        resolve(repository, ["test_msgs/msg/Open"])


def test_a_wide_character_has_no_descriptor_form(tmp_path):
    # covers: DSC-22, DSC-41
    repository = repository_for(tmp_path, {"test_msgs/msg/Wide": "wstring text"})

    with pytest.raises(msgs.MessageError):
        resolve(repository, ["test_msgs/msg/Wide"])


def test_a_message_with_more_than_255_fields_has_no_descriptor_form(tmp_path):
    # covers: DSC-22, DSC-41
    fields = "\n".join(f"uint8 f{i}" for i in range(256))
    repository = repository_for(tmp_path, {"test_msgs/msg/Crowded": fields})

    with pytest.raises(msgs.MessageError, match="DSC-22"):
        build(["test_msgs/msg/Crowded"], repository=repository)


def test_a_table_with_more_than_65535_entries_has_no_descriptor_form(tmp_path):
    # covers: DSC-22, DSC-41
    definitions = {f"test_msgs/msg/Type{i}": "\n".join(f"uint8 f{j}" for j in range(255)) for i in range(300)}
    repository = repository_for(tmp_path, definitions)

    with pytest.raises(msgs.MessageError, match="DSC-22"):
        build(list(definitions), repository=repository)


def test_a_struct_over_65535_bytes_has_no_descriptor_form(tmp_path):
    # covers: DSC-22, DSC-41
    repository = repository_for(tmp_path, {"test_msgs/msg/Huge": "uint64[8192] values"})

    with pytest.raises(msgs.MessageError, match="DSC-22"):
        build(["test_msgs/msg/Huge"], repository=repository)


def test_a_scalar_with_a_bound_has_no_descriptor_form():
    # covers: DSC-44, DSC-41
    resolved = {
        "test_msgs/msg/Broken": bounds.ResolvedMessage(
            "test_msgs/msg/Broken",
            (bounds.ResolvedField("a", "uint8", False, False, 5, None),),
        )
    }

    with pytest.raises(msgs.MessageError, match="DSC-44"):
        descriptors.build(resolved, ["test_msgs/msg/Broken"])


def test_nesting_depth_counts_nested_messages_but_not_arrays(tmp_path):
    # covers: DSC-38, DSC-39, DSC-49
    repository = repository_for(
        tmp_path,
        {
            "test_msgs/msg/Inner": "uint8 tag",
            "test_msgs/msg/Middle": "test_msgs/msg/Inner inner\ntest_msgs/msg/Inner[<=4] rest",
            "test_msgs/msg/Outer": "test_msgs/msg/Middle middle",
        },
    )

    assert build(["test_msgs/msg/Outer"], repository=repository)[1].max_nest == 3
    assert build(["test_msgs/msg/Inner"], repository=repository)[1].max_nest == 1
    assert build([FLOAT32])[1].max_nest == 1


def test_the_emitted_descriptors_match_the_golden():
    resolved, table = build([FLOAT32, TWIST])

    assert descriptors.emit_c(table) == (GOLDEN / "float32_twist.c").read_text(encoding="utf-8")


@pytest.mark.parametrize("narrow", [False, True])
@pytest.mark.parametrize("type_name", sorted(ROUND_TRIP_BOUNDS))
def test_descriptors_round_trip_against_the_reference(type_name, narrow):
    # covers: DSC-13, DSC-14, DSC-15, DSC-16
    repository = msgs.MessageRepository()
    resolved = resolve(repository, [type_name], ROUND_TRIP_BOUNDS[type_name])
    table = descriptors.build(resolved, [type_name])
    rng = random.Random(f"{SEED}-descriptors-{type_name}-{narrow}")

    for _ in range(8):
        message = random_message(repository, type_name, rng)
        payload = messages.encode(repository, type_name, message, narrow=narrow)

        assert walk_encode(table, resolved, type_name, message, narrow=narrow) == payload
        decoded = walk_decode(table, resolved, type_name, payload, narrow=narrow)
        assert matches(repository, type_name, message, decoded, narrow=narrow), (message, payload.hex(" "))


@pytest.mark.parametrize("narrow", [False, True])
def test_descriptors_cover_every_kind_and_mode(narrow, tmp_path):
    # covers: DSC-13, DSC-14, DSC-15, DSC-16
    repository = repository_for(
        tmp_path, {"test_msgs/msg/Inner": "uint8 tag\nfloat64 value", "test_msgs/msg/Cover": COVER_FIELDS}
    )
    resolved = resolve(repository, ["test_msgs/msg/Cover"])
    table = descriptors.build(resolved, ["test_msgs/msg/Cover"])
    rng = random.Random(f"{SEED}-descriptors-cover-{narrow}")

    for _ in range(8):
        message = random_message(repository, "test_msgs/msg/Cover", rng)
        payload = messages.encode(repository, "test_msgs/msg/Cover", message, narrow=narrow)

        assert walk_encode(table, resolved, "test_msgs/msg/Cover", message, narrow=narrow) == payload
        decoded = walk_decode(table, resolved, "test_msgs/msg/Cover", payload, narrow=narrow)
        assert matches(repository, "test_msgs/msg/Cover", message, decoded, narrow=narrow), message
