"""C emission: descriptor tables in the spec section 9 format, and their C99 compilation."""

import random
import struct
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
CORE_INCLUDE = Path(__file__).resolve().parents[2] / "core" / "include"
CORE_SUPPORT = Path(__file__).resolve().parents[2] / "core" / "tests" / "support"
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
    "sensor_msgs/msg/Imu": dict(HEADER_BOUNDS),
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
    (tmp_path / "r2s_generated.h").write_text(emit.emit_types(resolved, max_nest=table.max_nest), encoding="utf-8")
    (tmp_path / "r2s_descriptors.c").write_text(descriptors.emit_c(table), encoding="utf-8")
    dump = tmp_path / "dump.c"
    dump.write_text(
        "\n".join(
            [
                "#include <stdio.h>",
                "#include <stdint.h>",
                '#include "r2s/r2s_codec.h"',
                "extern const uint8_t R2S_FLASH r2s_type_records[];",
                "extern const uint8_t R2S_FLASH r2s_field_entries[];",
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
            "-I",
            str(CORE_INCLUDE),
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
    # covers: DSC-7, DSC-48
    repository = repository_for(tmp_path, {"test_msgs/msg/Empty": ""})
    resolved, table = build(["test_msgs/msg/Empty"], repository=repository)

    assert [(record.field_count, record.first_field) for record in table.records] == [(0, 0)]
    assert table.entries == ()
    (tmp_path / "r2s_generated.h").write_text(emit.emit_types(resolved, max_nest=table.max_nest), encoding="utf-8")
    (tmp_path / "r2s_descriptors.c").write_text(descriptors.emit_c(table), encoding="utf-8")
    link = tmp_path / "link.c"
    link.write_text(
        "\n".join(
            [
                "#include <stdint.h>",
                '#include "r2s/r2s_codec.h"',
                "extern const uint8_t R2S_FLASH r2s_type_records[];",
                "extern const uint8_t R2S_FLASH r2s_field_entries[];",
                "const uint8_t *tables[] = {r2s_type_records, r2s_field_entries};",
                "int main(void)",
                "{",
                "    return (int)(tables[0][0] + tables[1][0]);",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    linked = subprocess.run(
        [
            "cc",
            "-std=c99",
            "-pedantic",
            "-Werror",
            "-I",
            str(tmp_path),
            "-I",
            str(CORE_INCLUDE),
            str(link),
            str(tmp_path / "r2s_descriptors.c"),
            "-o",
            str(tmp_path / "link"),
        ],
        capture_output=True,
        text=True,
    )
    assert linked.returncode == 0, linked.stderr


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
    resolved, table = build(["test_msgs/msg/Cover"], repository=repository)
    fields = {(message.type_name, field.name): field for message in resolved.values() for field in message.fields}

    for entry in table.entries:
        assert 0 <= entry.kind <= 12
        assert entry.mode in (0, 1, 2, 0x82)
        assert (entry.count == 0) == (entry.mode == 0)
        assert (entry.mode == 0x82) == (entry.count > 255)
        if entry.kind == 11:
            assert entry.aux == fields[(entry.owner, entry.field)].string_bound
        elif entry.kind == 12:
            assert entry.aux == table.index_of[fields[(entry.owner, entry.field)].type_name]
            assert entry.aux < table.index_of[entry.owner]
        else:
            assert entry.aux == 0


def test_a_wide_sequence_counts_its_elements_in_two_bytes(tmp_path):
    # covers: DSC-14, DSC-15, DSC-20, DSC-48
    repository = repository_for(tmp_path, {"test_msgs/msg/Wide": "uint64[<=300] values"})
    resolved, table = build(["test_msgs/msg/Wide"], repository=repository)

    assert [(entry.mode, entry.count) for entry in table.entries] == [(0x82, 300)]
    text = descriptors.emit_c(table)
    assert "R2S_FIELD_CHECK(test_msgs__msg__Wide, values, values.size);" in text
    (tmp_path / "r2s_generated.h").write_text(emit.emit_types(resolved, max_nest=table.max_nest), encoding="utf-8")
    (tmp_path / "check.c").write_text(
        '#include "r2s_generated.h"\n' + text + "\nint main(void)\n{\n    return 0;\n}\n", encoding="utf-8"
    )
    for compiler, args, output in (("cc", [], "check_host.o"), ("avr-gcc", ["-mmcu=atmega328p"], "check_avr.o")):
        compiled = subprocess.run(
            [
                compiler,
                *args,
                "-std=c99",
                "-pedantic",
                "-Werror",
                "-I",
                str(tmp_path),
                "-I",
                str(CORE_INCLUDE),
                "-c",
                str(tmp_path / "check.c"),
                "-o",
                str(tmp_path / output),
            ],
            capture_output=True,
            text=True,
        )
        assert compiled.returncode == 0, f"{compiler}: {compiled.stderr}"
    symbols = subprocess.run(["avr-objdump", "-t", str(tmp_path / "check_avr.o")], capture_output=True, text=True)
    assert symbols.returncode == 0, symbols.stderr
    placed = [line for line in symbols.stdout.splitlines() if "r2s_type_records" in line or "r2s_field_entries" in line]
    assert len(placed) == 2 and all(".progmem" in line for line in placed), symbols.stdout


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


def test_the_generated_header_configures_the_nesting_stack():
    # covers: DSC-39
    resolved, table = build([FLOAT32, TWIST])
    header = emit.emit_types(resolved, max_nest=table.max_nest)

    assert "#ifndef R2S_MAX_NEST\n#define R2S_MAX_NEST 2u\n#endif" in header
    assert emit.emit_types(resolved).count("R2S_MAX_NEST") == 0


def test_the_descriptors_see_the_header_nesting_bound(tmp_path):
    # covers: DSC-39
    resolved, table = build([FLOAT32, TWIST])
    text = descriptors.emit_c(table)

    assert text.index('"r2s_generated.h"') < text.index('"r2s/r2s_codec.h"')
    (tmp_path / "r2s_generated.h").write_text(emit.emit_types(resolved, max_nest=table.max_nest), encoding="utf-8")
    proof = tmp_path / "nest_descriptors.c"
    proof.write_text(text + "typedef char r2s_nest_proof[(R2S_MAX_NEST == 2u) ? 1 : -1];\n", encoding="utf-8")
    compiled = subprocess.run(
        [
            "cc",
            "-std=c99",
            "-pedantic",
            "-Werror",
            "-I",
            str(tmp_path),
            "-I",
            str(CORE_INCLUDE),
            "-c",
            str(proof),
            "-o",
            str(tmp_path / "nest_descriptors.o"),
        ],
        capture_output=True,
        text=True,
    )
    assert compiled.returncode == 0, compiled.stderr


def test_the_core_sees_the_header_nesting_bound(tmp_path):
    # covers: DSC-39
    resolved, table = build([FLOAT32, TWIST])
    (tmp_path / "r2s_generated.h").write_text(emit.emit_types(resolved, max_nest=table.max_nest), encoding="utf-8")
    proof = tmp_path / "nest_proof.c"
    proof.write_text(
        "\n".join(
            [
                '#include "r2s_generated.h"',
                '#include "r2s/r2s_codec.h"',
                "typedef char r2s_nest_proof[(R2S_MAX_NEST == 2u) ? 1 : -1];",
                "int main(void)",
                "{",
                "    return 0;",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    compiled = subprocess.run(
        [
            "cc",
            "-std=c99",
            "-pedantic",
            "-Werror",
            "-I",
            str(tmp_path),
            "-I",
            str(CORE_INCLUDE),
            "-c",
            str(proof),
            "-o",
            str(tmp_path / "nest_proof.o"),
        ],
        capture_output=True,
        text=True,
    )
    assert compiled.returncode == 0, compiled.stderr


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


def test_an_unknown_root_has_no_descriptor_table():
    with pytest.raises(msgs.MessageError, match="unknown message type"):
        descriptors.build({}, ["test_msgs/msg/Missing"])


def test_an_unresolvable_nested_type_has_no_descriptor_table():
    resolved = {
        "test_msgs/msg/Outer": bounds.ResolvedMessage(
            "test_msgs/msg/Outer",
            (bounds.ResolvedField("inner", "test_msgs/msg/Missing", False, False, None, None),),
        )
    }

    with pytest.raises(msgs.MessageError, match="unknown field type"):
        descriptors.build(resolved, ["test_msgs/msg/Outer"])


def test_a_recursive_type_has_no_descriptor_table():
    # covers: DSC-12
    resolved = {
        "test_msgs/msg/Loop": bounds.ResolvedMessage(
            "test_msgs/msg/Loop",
            (bounds.ResolvedField("next", "test_msgs/msg/Loop", False, False, None, None),),
        )
    }

    with pytest.raises(msgs.MessageError, match="DSC-12"):
        descriptors.build(resolved, ["test_msgs/msg/Loop"])


def test_an_unbounded_sequence_has_no_descriptor_form_even_unresolved():
    # covers: DSC-22, DSC-41
    resolved = {
        "test_msgs/msg/Open": bounds.ResolvedMessage(
            "test_msgs/msg/Open",
            (bounds.ResolvedField("values", "uint8", True, True, None, None),),
        )
    }

    with pytest.raises(msgs.MessageError, match="DSC-22"):
        descriptors.build(resolved, ["test_msgs/msg/Open"])


def test_a_zero_length_array_has_no_descriptor_form():
    # covers: DSC-44, DSC-41
    resolved = {
        "test_msgs/msg/Empty": bounds.ResolvedMessage(
            "test_msgs/msg/Empty",
            (bounds.ResolvedField("values", "uint8", True, False, 0, None),),
        )
    }

    with pytest.raises(msgs.MessageError, match="DSC-44"):
        descriptors.build(resolved, ["test_msgs/msg/Empty"])


def test_a_struct_of_exactly_65535_bytes_has_a_descriptor_form(tmp_path):
    repository = repository_for(tmp_path, {"test_msgs/msg/Full": "uint8[65535] values"})

    _, table = build(["test_msgs/msg/Full"], repository=repository)

    assert table.entries[0].count == 65535


def test_no_roots_make_an_empty_table_with_a_nesting_bound_of_one():
    # covers: DSC-39
    table = descriptors.build({}, [])

    assert table.records == () and table.entries == ()
    assert table.max_nest == 1
    assert "#define R2S_MAX_NEST 1u" in emit.emit_types({}, max_nest=table.max_nest)


def test_wide_values_match_the_hand_derivation(tmp_path):
    # covers: DSC-4, DSC-5, DSC-13, DSC-14, DSC-15, DSC-16, DSC-17, DSC-18, DSC-19
    repository = repository_for(
        tmp_path,
        {"test_msgs/msg/Demo": "uint8[200] pad\nuint64[<=500] wide\nstring<=300 text"},
    )
    resolved, table = build(["test_msgs/msg/Demo"], repository=repository)
    records, entries = compiled_bytes(tmp_path, resolved, ["test_msgs/msg/Demo"])

    assert records.hex(" ") == "a0 11 00 00 03"
    assert entries.hex(" ") == "01 01 00 00 c8 00 00 00 07 82 c8 00 f4 01 00 00 0b 00 70 10 00 00 2c 01"


def test_generated_tables_match_c9_handwritten_tables(tmp_path):
    # covers: DSC-1, DSC-2, DSC-3, DSC-4
    demo = repository_for(tmp_path, {"std_msgs/msg/String": "string<=8 data"})
    resolved = resolve(msgs.MessageRepository(), [FLOAT32, TWIST])
    resolved.update(resolve(demo, ["std_msgs/msg/String"]))
    roots = [FLOAT32, TWIST, "std_msgs/msg/String"]
    table = descriptors.build(resolved, roots)
    assert [record.type_name for record in table.records] == [
        FLOAT32,
        VECTOR3,
        TWIST,
        "std_msgs/msg/String",
    ]
    (tmp_path / "r2s_generated.h").write_text(emit.emit_types(resolved, max_nest=table.max_nest), encoding="utf-8")
    (tmp_path / "r2s_descriptors.c").write_text(descriptors.emit_c(table), encoding="utf-8")
    compare = tmp_path / "compare.c"
    compare.write_text(
        "\n".join(
            [
                "#include <stdint.h>",
                "#include <stdio.h>",
                "#include <string.h>",
                '#include "handwritten.h"',
                '#include "r2s/r2s_codec.h"',
                "extern const uint8_t R2S_FLASH r2s_type_records[];",
                "extern const uint8_t R2S_FLASH r2s_field_entries[];",
                "r2s_status_t r2s_session_configure(const char *node_name,",
                "                                   const r2s_session_topic_t *topics,",
                "                                   uint8_t topic_count) { return R2S_OK; }",
                "int main(void)",
                "{",
                "    if (R2S_HANDWRITTEN_TABLE.type_count != 4u || R2S_HANDWRITTEN_TABLE.field_count != 7u) {",
                "        return 1;",
                "    }",
                "    if (memcmp(R2S_HANDWRITTEN_TABLE.types, r2s_type_records, 4u * 5u) != 0) {",
                "        return 1;",
                "    }",
                "    if (memcmp(R2S_HANDWRITTEN_TABLE.fields, r2s_field_entries, 7u * 8u) != 0) {",
                "        return 1;",
                "    }",
                '    printf("match\\n");',
                "    return 0;",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    binary = tmp_path / "compare"
    compiled = subprocess.run(
        [
            "cc",
            "-std=c99",
            "-pedantic",
            "-Werror",
            "-I",
            str(tmp_path),
            "-I",
            str(CORE_INCLUDE),
            "-I",
            str(CORE_SUPPORT),
            str(compare),
            str(tmp_path / "r2s_descriptors.c"),
            str(CORE_SUPPORT / "handwritten.c"),
            "-o",
            str(binary),
        ],
        capture_output=True,
        text=True,
    )
    assert compiled.returncode == 0, compiled.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0
    assert run.stdout == "match\n"


@pytest.mark.parametrize("narrow", [False, True])
def test_core_codec_round_trips_generated_tables(narrow, tmp_path):
    # covers: DSC-5, DSC-17, DSC-18, DSC-19, DSC-21
    repository = msgs.MessageRepository()
    merged = {}
    for bound_map in ROUND_TRIP_BOUNDS.values():
        merged.update(bound_map)
    roots = sorted(ROUND_TRIP_BOUNDS)
    resolved = resolve(repository, roots, merged)
    table = descriptors.build(resolved, roots)
    (tmp_path / "r2s_generated.h").write_text(emit.emit_types(resolved, max_nest=table.max_nest), encoding="utf-8")
    (tmp_path / "r2s_descriptors.c").write_text(descriptors.emit_c(table), encoding="utf-8")
    driver = tmp_path / "driver.c"
    driver.write_text(
        "\n".join(
            [
                "#include <stdint.h>",
                "#include <stdio.h>",
                "#include <string.h>",
                '#include "r2s/r2s_codec.h"',
                '#include "r2s/r2s_port.h"',
                "extern const uint8_t R2S_FLASH r2s_type_records[];",
                "extern const uint8_t R2S_FLASH r2s_field_entries[];",
                "static const r2s_codec_table_t TABLE = {",
                "    r2s_type_records, DRV_TYPES, r2s_field_entries, DRV_FIELDS,",
                "};",
                "static uint8_t msg[65535u];",
                "static uint8_t payload[65535u];",
                "static uint8_t encoded[65535u];",
                "uint16_t r2s_port_read(uint8_t *buf, uint16_t max)",
                "{",
                "    (void)buf;",
                "    (void)max;",
                "    return 0;",
                "}",
                "void r2s_port_write(const uint8_t *buf, uint16_t len)",
                "{",
                "    (void)buf;",
                "    (void)len;",
                "}",
                "uint16_t r2s_port_tx_free(void)",
                "{",
                "    return 0xffffu;",
                "}",
                "uint32_t r2s_port_tick(void)",
                "{",
                "    return 0;",
                "}",
                "int main(void)",
                "{",
                "    for (;;) {",
                "        uint8_t head[5];",
                "        size_t got = fread(head, 1, 5, stdin);",
                "        uint16_t type;",
                "        uint8_t narrow;",
                "        uint16_t len;",
                "        uint8_t field = 0;",
                "        uint8_t status = 0;",
                "        uint16_t outlen = 0;",
                "        r2s_codec_out_t out;",
                "        if (got == 0) {",
                "            return 0;",
                "        }",
                "        if (got != 5) {",
                "            return 1;",
                "        }",
                "        type = (uint16_t)(head[0] | ((uint16_t)head[1] << 8));",
                "        narrow = head[2];",
                "        len = (uint16_t)(head[3] | ((uint16_t)head[4] << 8));",
                "        if (len > sizeof(payload) || fread(payload, 1, len, stdin) != len) {",
                "            return 1;",
                "        }",
                "        memset(msg, 0, sizeof(msg));",
                "        out.buf = encoded;",
                "        out.cap = (uint16_t)sizeof(encoded);",
                "        out.to_tx = 0;",
                "        out.len = 0;",
                "        if (r2s_codec_decode(&TABLE, type, payload, len, narrow, msg, &field) != R2S_OK) {",
                "            status = 2;",
                "        } else if (r2s_codec_encode(&TABLE, type, msg, narrow, &out, &field) != R2S_OK) {",
                "            status = 1;",
                "        } else {",
                "            outlen = out.len;",
                "        }",
                "        if (fputc(status, stdout) == EOF) {",
                "            return 1;",
                "        }",
                "        if (fputc((int)(outlen & 0xffu), stdout) == EOF) {",
                "            return 1;",
                "        }",
                "        if (fputc((int)((outlen >> 8) & 0xffu), stdout) == EOF) {",
                "            return 1;",
                "        }",
                "        if (outlen > 0 && fwrite(encoded, 1, outlen, stdout) != outlen) {",
                "            return 1;",
                "        }",
                "    }",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    core = Path(__file__).resolve().parents[2] / "core" / "src"
    binary = tmp_path / "roundtrip"
    # The G8 (#75) firmware compiles the core with the generated header visible; -include
    # proves the header alone configures the interpreter, with no -D.

    compiled = subprocess.run(
        [
            "cc",
            "-std=c99",
            "-pedantic",
            "-Werror",
            "-I",
            str(tmp_path),
            "-I",
            str(CORE_INCLUDE),
            "-include",
            str(tmp_path / "r2s_generated.h"),
            f"-DDRV_TYPES={len(table.records)}u",
            f"-DDRV_FIELDS={len(table.entries)}u",
            str(driver),
            str(tmp_path / "r2s_descriptors.c"),
            str(core / "codec.c"),
            str(core / "varint.c"),
            str(core / "tx.c"),
            str(core / "crc.c"),
            str(core / "framer.c"),
            str(core / "rcobs.c"),
            "-o",
            str(binary),
        ],
        capture_output=True,
        text=True,
    )
    assert compiled.returncode == 0, compiled.stderr
    rng = random.Random(f"{SEED}-core-codec-{narrow}")
    asked = []
    wanted = []
    for type_name in roots:
        for _ in range(8):
            message = random_message(repository, type_name, rng)
            payload = messages.encode(repository, type_name, message, narrow=narrow)
            asked.append(
                struct.pack("<H", table.index_of[type_name])
                + bytes([1 if narrow else 0])
                + struct.pack("<H", len(payload))
                + payload
            )
            wanted.append((type_name, message, payload))
    run = subprocess.run([str(binary)], input=b"".join(asked), capture_output=True)
    assert run.returncode == 0
    pos = 0
    for type_name, message, payload in wanted:
        assert run.stdout[pos] == 0, (payload.hex(" "), run.stdout[pos])
        outlen = run.stdout[pos + 1] | (run.stdout[pos + 2] << 8)
        out = bytes(run.stdout[pos + 3 : pos + 3 + outlen])
        pos += 3 + outlen
        if narrow:
            # A narrow NaN stays a NaN with an implementation-defined payload,
            # so the bytes may differ where the value is a NaN.
            decoded = messages.decode(repository, type_name, out, narrow=True)
            assert matches(repository, type_name, message, decoded, narrow=True), (message, out.hex(" "))
        else:
            assert out == payload
    assert pos == len(run.stdout)
