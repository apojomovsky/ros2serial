"""The bundle subcommand: the Arduino library file set, golden bytes and the X-macro expansion.

Covers the bundle path end to end: the exact file set, the banner in every
file, determinism across runs and distros, the distro error, the command
itself, and the golden bytes. The host proof compiles the X-macro expansion
with the real core codec, then checks the table, the publish wrappers and
dispatch against the Python reference; an unknown type in the list must fail
naming it.
"""

import re
import subprocess
from pathlib import Path

import pytest
from r2s_gen import bundle, cli, cpp, msgs, typehash
from r2s_gen.reference import messages
from r2s_gen.version import __version__

GOLDEN = Path(__file__).parent / "golden" / "bundle"
CORE = Path(__file__).resolve().parents[2] / "core"
CORE_INCLUDE = CORE / "include"
CORE_SRC = CORE / "src"

SKETCH_TOPICS = """\
#define R2S_NODE_NAME "test_bundle"
#define R2S_TOPICS(X) \\
    X(Publisher, std_msgs::msg::Float32, battery_pub, "battery") \\
    X(Publisher, std_msgs::msg::String, greeting_pub, "greeting") \\
    X(Subscription, geometry_msgs::msg::Twist, cmd_vel_sub, "cmd_vel", on_cmd_vel) \\
    X(Subscription, sensor_msgs::msg::JointState, joints_sub, "joints", on_joints)
"""


def files(distro="jazzy"):
    return bundle.bundle(distro)


def test_files_for_names_the_exact_bundle_set():
    assert bundle.files_for() == (
        "library.properties",
        "src/ros2serial.h",
        "src/r2s_bundled.h",
        "src/r2s_bundled.c",
        "src/builtin_interfaces/msg/time.hpp",
        "src/geometry_msgs/msg/quaternion.hpp",
        "src/geometry_msgs/msg/twist.hpp",
        "src/geometry_msgs/msg/vector3.hpp",
        "src/sensor_msgs/msg/imu.hpp",
        "src/sensor_msgs/msg/joint_state.hpp",
        "src/std_msgs/msg/float32.hpp",
        "src/std_msgs/msg/header.hpp",
        "src/std_msgs/msg/string.hpp",
    )


def test_bundle_returns_exactly_the_named_files():
    assert sorted(files()) == sorted(bundle.files_for())


def test_every_file_carries_the_version_distro_and_digest():
    for name, text in files().items():
        assert __version__ in text, name
        assert "distro jazzy" in text, name
        assert re.search(r"bundle [0-9a-f]{16}", text), name


def test_bundle_is_byte_identical_run_to_run():
    assert files() == files()


def test_each_distro_bundles_its_own_snapshot():
    assert files("jazzy") != files("lyrical")
    assert files("jazzy") != files("rolling")


def test_every_bundled_type_resolves_from_the_snapshot():
    repository = msgs.MessageRepository(distro="jazzy")
    for type_name in bundle.BUNDLED_TYPES:
        assert repository.load(type_name).type_name == type_name


def test_an_unknown_distro_names_the_supported_ones():
    with pytest.raises(bundle.MessageError) as caught:
        files("humble")

    assert str(caught.value) == "--distro: unknown distro 'humble'; use one of: jazzy, lyrical, rolling"


def test_the_golden_library_properties_are_emitted():
    assert files()["library.properties"] == (GOLDEN / "library.properties").read_text(encoding="utf-8")


def test_the_golden_sketch_header_is_emitted():
    assert files()["src/ros2serial.h"] == (GOLDEN / "ros2serial.h").read_text(encoding="utf-8")


def test_the_golden_bundled_header_is_emitted():
    assert files()["src/r2s_bundled.h"] == (GOLDEN / "r2s_bundled.h").read_text(encoding="utf-8")


def test_the_golden_bundled_source_is_emitted():
    assert files()["src/r2s_bundled.c"] == (GOLDEN / "r2s_bundled.c").read_text(encoding="utf-8")


def test_the_sketch_header_serves_the_runtime_api_without_a_topic_list():
    header = files()["src/ros2serial.h"]

    assert "#if defined(__cplusplus) && !defined(R2S_TOPICS)" in header
    assert '#include "r2s/runtime.hpp"' in header


def test_the_sketch_header_refuses_a_c_sketch_without_a_topic_list():
    header = files()["src/ros2serial.h"]

    assert "#if !defined(R2S_TOPICS) && !defined(__cplusplus)" in header
    assert "#error" in header


def test_the_sketch_header_keeps_the_x_macro_expansion_for_a_topic_list():
    header = files()["src/ros2serial.h"]

    assert "#ifdef R2S_TOPICS" in header
    assert "enum r2s_topic_slot { R2S_TOPICS(R2S_X_SLOT) R2S_SLOT_COUNT };" in header
    assert "void r2s_codec_dispatch(uint8_t topic_id, uint8_t seq, const uint8_t *payload, uint16_t len)" in header


def test_the_bundle_ships_the_cpp_message_headers():
    files = bundle.bundle("jazzy")

    for type_name in bundle.BUNDLED_TYPES:
        path = f"src/{cpp.message_path(type_name)}"
        assert path in files
        assert "namespace r2s" in files[path]


def test_the_bundle_command_writes_the_library_files(tmp_path, capsys):
    out = tmp_path / "out"

    assert cli.main(["bundle", "--distro", "jazzy", "--out", str(out)]) == 0
    assert sorted(path.relative_to(out).as_posix() for path in out.rglob("*") if path.is_file()) == sorted(
        bundle.files_for()
    )
    assert "src/ros2serial.h" in capsys.readouterr().out


def test_the_bundle_command_defaults_to_jazzy(tmp_path):
    out = tmp_path / "out"

    assert cli.main(["bundle", "--out", str(out)]) == 0
    assert (out / "src" / "r2s_bundled.c").is_file()


def test_the_bundle_command_rejects_an_unknown_distro(tmp_path, capsys):
    assert cli.main(["bundle", "--distro", "humble", "--out", str(tmp_path / "out")]) == 1
    assert "--distro: unknown distro 'humble'" in capsys.readouterr().err


def sketch_dir(tmp_path, out_files):
    root = tmp_path / "sketch"
    for name, text in out_files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    return root


def compile_unit(root, source, extra=()):
    obj = root / (Path(source).stem + ".o")
    compiled = subprocess.run(
        [
            "cc",
            "-std=c99",
            "-pedantic",
            "-Werror",
            "-Wall",
            "-Wextra",
            "-I",
            str(root),
            "-I",
            str(root / "src"),
            "-I",
            str(CORE_INCLUDE),
            *extra,
            "-c",
            str(source),
            "-o",
            str(obj),
        ],
        capture_output=True,
        text=True,
    )
    assert compiled.returncode == 0, compiled.stderr
    return obj


def compile_cxx_unit(root, source):
    """Compile one sketch translation unit: rows name C++ types, so the proof builds as C++."""
    obj = root / (Path(source).stem + ".o")
    compiled = subprocess.run(
        [
            "g++",
            "-std=c++11",
            "-pedantic",
            "-Werror",
            "-Wall",
            "-Wextra",
            "-I",
            str(root),
            "-I",
            str(root / "src"),
            "-I",
            str(CORE_INCLUDE),
            "-c",
            str(source),
            "-o",
            str(obj),
        ],
        capture_output=True,
        text=True,
    )
    assert compiled.returncode == 0, compiled.stderr
    return obj


def compile_sketch(root, sources, core_sources, extra=()):
    """Link the sketch: its translation units read the limits from the header,
    while the core ones take them as defines, as the Arduino build does."""
    objects = [
        compile_cxx_unit(root, source) if source.endswith(".cpp") else compile_unit(root, source) for source in sources
    ]
    objects += [compile_unit(root, source, extra) for source in core_sources]
    linked = subprocess.run(
        ["g++", *[str(obj) for obj in objects], "-o", str(root / "sketch")],
        capture_output=True,
        text=True,
    )
    assert linked.returncode == 0, linked.stderr


def c_bytes(payload):
    return ", ".join(f"0x{byte:02x}" for byte in payload)


def host_sketch(frame, nest):
    repository = msgs.MessageRepository(distro="jazzy")
    battery = messages.encode(repository, "std_msgs/msg/Float32", {"data": 1.5}, narrow=False)
    greeting = messages.encode(repository, "std_msgs/msg/String", {"data": "hi"}, narrow=False)
    twist = {
        "linear": {"x": 1.0, "y": 2.0, "z": 3.0},
        "angular": {"x": 4.0, "y": 5.0, "z": 6.0},
    }
    twist_payload = messages.encode(repository, "geometry_msgs/msg/Twist", twist, narrow=False)
    joints = {
        "header": {"stamp": {"sec": 1, "nanosec": 2}, "frame_id": "base"},
        "name": ["a", "bb"],
        "position": [1.0, 2.0],
        "velocity": [3.0],
        "effort": [],
    }
    joints_payload = messages.encode(repository, "sensor_msgs/msg/JointState", joints, narrow=False)
    twist_narrow = messages.encode(repository, "geometry_msgs/msg/Twist", twist, narrow=True)
    hashes = {}
    for type_name in (
        "std_msgs/msg/Float32",
        "std_msgs/msg/String",
        "geometry_msgs/msg/Twist",
        "sensor_msgs/msg/JointState",
    ):
        digest = typehash.type_hash(repository, type_name)
        hashes[type_name] = ", ".join(f"0x{byte:02x}" for byte in bytes.fromhex(digest[len("RIHS01_") :])[:8])
    return (
        """\
/* The host proof of the X-macro expansion: the table, the wrappers and
 * dispatch against the Python reference, with stubbed session and tx. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

"""
        + SKETCH_TOPICS
        + f'#define EXPECTED_FRAME {frame}u\n#define EXPECTED_NEST {nest}u\n#include "ros2serial.h"\n'
        + "static geometry_msgs::msg::Twist got_twist;\n"
        + """\
static sensor_msgs::msg::JointState got_joints;
static uint8_t got_twist_calls;
static uint8_t got_joints_calls;

extern uint8_t stub_tx_payload[2048];
extern uint16_t stub_tx_len;
extern uint8_t stub_refused;

void on_cmd_vel(const geometry_msgs::msg::Twist &msg)
{
    got_twist = msg;
    got_twist_calls++;
}

void on_joints(const sensor_msgs::msg::JointState &msg)
{
    got_joints = msg;
    got_joints_calls++;
}

#define CHECK(cond) \\
    do { \\
        if (!(cond)) { \\
            printf("fail line %d: %s\\n", __LINE__, #cond); \\
            return 1; \\
        } \\
    } while (0)

int main(void)
{
    static const uint8_t battery_payload[] = {"""
        + c_bytes(battery)
        + """};
    static const uint8_t greeting_payload[] = {"""
        + c_bytes(greeting)
        + """};
    static const uint8_t twist_payload[] = {"""
        + c_bytes(twist_payload)
        + """};
    static const uint8_t joints_payload[] = {"""
        + c_bytes(joints_payload)
        + """};
    static const uint8_t twist_narrow[] = {"""
        + c_bytes(twist_narrow)
        + """};
    static const uint8_t float_hash[] = {"""
        + hashes["std_msgs/msg/Float32"]
        + """};
    std_msgs::msg::Float32 battery;
    std_msgs::msg::String greeting;
    sensor_msgs::msg::JointState joints;
    r2s_codec_out_t out = {NULL, 0u, 0u, 0u};
    uint8_t field = 0xFFu;
    uint8_t narrow = 0u;

    CHECK(R2S_TOPIC_COUNT == 4u);
    CHECK(R2S_MAX_FRAME == EXPECTED_FRAME);
    CHECK(R2S_MAX_NEST == EXPECTED_NEST);
    CHECK(r2s_topics[0].topic_id == 16u);
    CHECK(r2s_topics[3].topic_id == 19u);
    CHECK(r2s_topics[0].flags == (uint8_t)(0x00u | R2S_NARROW_FLAG));
    CHECK(r2s_topics[2].flags == (uint8_t)(0x01u | R2S_NARROW_FLAG));
    CHECK(strcmp(r2s_topics[1].type_name, "std_msgs/msg/String") == 0);
    CHECK(strcmp(r2s_topics[3].topic_name, "joints") == 0);
    CHECK(memcmp(r2s_topics[0].type_hash, float_hash, 8) == 0);

    memset(&battery, 0, sizeof battery);
    battery.data = 1.5f;
    CHECK(r2s_publish_battery_pub(&battery) == R2S_OK);
    CHECK(stub_tx_len == sizeof battery_payload);
    CHECK(memcmp(stub_tx_payload, battery_payload, sizeof battery_payload) == 0);

    memset(&greeting, 0, sizeof greeting);
    strcpy(greeting.data, "hi");
    CHECK(r2s_publish_greeting_pub(&greeting) == R2S_OK);
    CHECK(stub_tx_len == sizeof greeting_payload);
    CHECK(memcmp(stub_tx_payload, greeting_payload, sizeof greeting_payload) == 0);

    r2s_codec_dispatch(18u, 7u, twist_payload, (uint16_t)sizeof twist_payload);
    CHECK(got_twist_calls == 1u);
    CHECK(got_twist.linear.x == 1.0);
    CHECK(got_twist.angular.z == 6.0);
    CHECK(stub_refused == 0u);

    r2s_codec_dispatch(19u, 3u, joints_payload, (uint16_t)sizeof joints_payload);
    CHECK(got_joints_calls == 1u);
    CHECK(got_joints.name.size == 2u);
    CHECK(strcmp(got_joints.name.data[0], "a") == 0);
    CHECK(got_joints.position.data[1] == 2.0);
    CHECK(stub_refused == 0u);

    r2s_codec_dispatch(200u, 0u, twist_payload, (uint16_t)sizeof twist_payload);
    CHECK(stub_refused == 1u);
    r2s_codec_dispatch(18u, 8u, twist_payload, 1u);
    CHECK(stub_refused == 2u);

    memset(&joints, 0, sizeof joints);
    joints.header.stamp.sec = 1;
    joints.header.stamp.nanosec = 2;
    strcpy(joints.header.frame_id, "base");
    strcpy(joints.name.data[0], "a");
    strcpy(joints.name.data[1], "bb");
    joints.name.size = 2u;
    joints.position.data[0] = 1.0;
    joints.position.data[1] = 2.0;
    joints.position.size = 2u;
    joints.velocity.data[0] = 3.0;
    joints.velocity.size = 1u;
    out.len = 0u;
    field = 0xFFu;
    CHECK(
        r2s_codec_encode(
            &R2S_BUNDLED_TABLE_sensor_msgs_JointState,
            R2S_BUNDLED_INDEX_sensor_msgs_JointState,
            &joints,
            narrow,
            &out,
            &field) == R2S_OK);
    CHECK(out.len == sizeof joints_payload);
    CHECK(out.len + 4u <= R2S_MAX_FRAME);

    narrow = 1u;
    out.len = 0u;
    field = 0xFFu;
    {
        geometry_msgs::msg::Twist narrow_twist;
        uint8_t narrow_buf[64];
        out.buf = narrow_buf;
        out.cap = (uint16_t)sizeof narrow_buf;
        memset(&narrow_twist, 0, sizeof narrow_twist);
        narrow_twist.linear.x = 1.0;
        narrow_twist.linear.y = 2.0;
        narrow_twist.linear.z = 3.0;
        narrow_twist.angular.x = 4.0;
        narrow_twist.angular.y = 5.0;
        narrow_twist.angular.z = 6.0;
        CHECK(
            r2s_codec_encode(
                &R2S_BUNDLED_TABLE_geometry_msgs_Twist,
                R2S_BUNDLED_INDEX_geometry_msgs_Twist,
                &narrow_twist,
                narrow,
                &out,
                &field) == R2S_OK);
        CHECK(out.len == sizeof twist_narrow);
        CHECK(memcmp(narrow_buf, twist_narrow, sizeof twist_narrow) == 0);
    }

    printf("bundle-host-ok\\n");
    return 0;
}
"""
    )


STUBS = """\
/* Stubbed session and tx: the host proof drives the expansion, not the link. */
#include <stdint.h>

#include "r2s/r2s.h"

uint8_t stub_tx_payload[2048];
uint16_t stub_tx_len;
uint8_t stub_refused;

uint8_t r2s_connected(void)
{
    return 1u;
}

uint8_t r2s_session_find_topic(uint8_t topic_id)
{
    return (uint8_t)(topic_id - 16u);
}

uint8_t r2s_session_topic_flags(uint8_t index)
{
    (void)index;
    return 0u;
}

uint8_t r2s_session_topic_status(uint8_t index)
{
    (void)index;
    return 0u;
}

uint8_t r2s_session_tx_seq(uint8_t index)
{
    (void)index;
    return 0u;
}

void r2s_session_advance_tx_seq(uint8_t index)
{
    (void)index;
}

uint16_t r2s_session_note_rx_seq(uint8_t index, uint8_t seq)
{
    (void)index;
    (void)seq;
    return 0u;
}

r2s_status_t r2s_tx_begin(uint8_t topic_id, uint8_t seq, uint16_t payload_len)
{
    (void)topic_id;
    (void)seq;
    (void)payload_len;
    stub_tx_len = 0u;
    return R2S_OK;
}

r2s_status_t r2s_tx_byte(uint8_t byte)
{
    stub_tx_payload[stub_tx_len] = byte;
    stub_tx_len++;
    return R2S_OK;
}

r2s_status_t r2s_tx_end(void)
{
    return R2S_OK;
}

void r2s_framer_count_refused(void)
{
    stub_refused++;
}
"""


def test_the_expansion_matches_the_reference_on_the_host(tmp_path):
    out_files = files()
    root = sketch_dir(tmp_path, out_files)
    frame = frame_limit(out_files)
    nest = nest_limit(out_files)
    (root / "sketch.cpp").write_text(host_sketch(frame, nest), encoding="utf-8")
    (root / "stubs.c").write_text(STUBS, encoding="utf-8")

    compile_sketch(
        root,
        [
            str(root / "sketch.cpp"),
            str(root / "stubs.c"),
            str(root / "src" / "r2s_bundled.c"),
        ],
        [
            str(CORE_SRC / "codec.c"),
            str(CORE_SRC / "varint.c"),
        ],
        extra=(
            "-DR2S_WITH_CODEC",
            f"-DR2S_MAX_FRAME={frame}",
            f"-DR2S_MAX_NEST={nest}",
        ),
    )
    run = subprocess.run([str(root / "sketch")], capture_output=True, text=True)

    assert run.returncode == 0, run.stdout + run.stderr
    assert "bundle-host-ok" in run.stdout


def frame_limit(out_files):
    match = re.search(r"^#define R2S_MAX_FRAME ([0-9]+)u$", out_files["src/r2s_bundled.h"], re.MULTILINE)
    assert match is not None
    return match.group(1)


def nest_limit(out_files):
    match = re.search(r"^#define R2S_MAX_NEST ([0-9]+)u$", out_files["src/r2s_bundled.h"], re.MULTILINE)
    assert match is not None
    return match.group(1)


def compile_case(root, body):
    (root / "case.c").write_text(body, encoding="utf-8")

    return subprocess.run(
        [
            "cc",
            "-std=c99",
            "-fsyntax-only",
            "-I",
            str(root),
            "-I",
            str(root / "src"),
            "-I",
            str(CORE_INCLUDE),
            str(root / "case.c"),
        ],
        capture_output=True,
        text=True,
    )


def compile_case_cxx(root, body):
    (root / "case.cpp").write_text(body, encoding="utf-8")

    return subprocess.run(
        [
            "g++",
            "-std=c++11",
            "-fsyntax-only",
            "-I",
            str(root),
            "-I",
            str(root / "src"),
            "-I",
            str(CORE_INCLUDE),
            str(root / "case.cpp"),
        ],
        capture_output=True,
        text=True,
    )


def case_with_rows(rows):
    """One topic list, as a sketch writes it: the header brings the message types."""
    return "#define R2S_TOPICS(X) " + rows + '\n#include "ros2serial.h"\n'


def test_an_unknown_type_in_the_list_fails_naming_it(tmp_path):
    root = sketch_dir(tmp_path, files())
    body = case_with_rows('X(Subscription, foo_msgs::msg::Bar, x, "x")')

    compiled = compile_case_cxx(root, body)

    assert compiled.returncode != 0
    assert "foo_msgs" in compiled.stderr
    assert "Bar" in compiled.stderr


def test_a_missing_list_fails_naming_it(tmp_path):
    root = sketch_dir(tmp_path, files())

    compiled = compile_case(root, '#include "ros2serial.h"\n')

    assert compiled.returncode != 0
    assert "R2S_TOPICS" in compiled.stderr


def test_a_c_list_fails_pointing_at_a_cxx_translation_unit(tmp_path):
    root = sketch_dir(tmp_path, files())
    body = case_with_rows('X(Publisher, std_msgs::msg::Float32, battery_pub, "battery")')

    compiled = compile_case(root, body)

    assert compiled.returncode != 0
    assert "C++ translation unit" in compiled.stderr


def test_an_empty_list_fails_in_cxx(tmp_path):
    root = sketch_dir(tmp_path, files())
    (root / "case.cpp").write_text(
        '#define R2S_TOPICS(X)\n#include "ros2serial.h"\nint main()\n{\n    return 0;\n}\n',
        encoding="utf-8",
    )

    compiled = subprocess.run(
        [
            "g++",
            "-std=c++11",
            "-fsyntax-only",
            "-I",
            str(root),
            "-I",
            str(root / "src"),
            "-I",
            str(CORE_INCLUDE),
            str(root / "case.cpp"),
        ],
        capture_output=True,
        text=True,
    )

    assert compiled.returncode != 0
    assert "a node with no topics has nothing to announce" in compiled.stderr


def test_an_unknown_direction_in_the_list_fails(tmp_path):
    root = sketch_dir(tmp_path, files())
    body = case_with_rows('X(FOO, std_msgs::msg::Float32, x, "x")')

    compiled = compile_case_cxx(root, body)

    assert compiled.returncode != 0
    assert "FOO" in compiled.stderr


def test_an_old_pub_row_fails_naming_the_new_shape(tmp_path):
    root = sketch_dir(tmp_path, files())
    body = '#define R2S_TOPICS(X) X(PUB, battery, std_msgs, Float32)\n#include "ros2serial.h"\n'

    compiled = compile_case_cxx(root, body)

    assert compiled.returncode != 0
    assert "R2S_error_rows_must_read_X_Publisher_or_X_Subscription" in compiled.stderr


def test_an_old_sub_row_fails_naming_the_new_shape(tmp_path):
    root = sketch_dir(tmp_path, files())
    body = '#define R2S_TOPICS(X) X(SUB, cmd_vel, geometry_msgs, Twist)\n#include "ros2serial.h"\n'

    compiled = compile_case_cxx(root, body)

    assert compiled.returncode != 0
    assert "R2S_error_rows_must_read_X_Publisher_or_X_Subscription" in compiled.stderr


def test_a_publish_only_list_compiles_cleanly(tmp_path):
    root = sketch_dir(tmp_path, files())
    body = case_with_rows('X(Publisher, std_msgs::msg::Float32, battery_pub, "battery")')

    assert compile_strict_cxx(root, body).returncode == 0


def test_a_subscribe_only_list_compiles_cleanly(tmp_path):
    root = sketch_dir(tmp_path, files())
    body = case_with_rows('X(Subscription, geometry_msgs::msg::Twist, cmd_vel_sub, "cmd_vel", on_cmd_vel)')
    body += "void on_cmd_vel(const geometry_msgs::msg::Twist &msg)\n{\n    (void)msg;\n}\n"

    assert compile_strict_cxx(root, body).returncode == 0


def compile_strict_cxx(root, body):
    """A list that compiles warning-free: what a `-Werror` sketch build needs."""
    (root / "case.cpp").write_text(body, encoding="utf-8")

    return subprocess.run(
        [
            "g++",
            "-std=c++11",
            "-pedantic",
            "-Werror",
            "-Wall",
            "-Wextra",
            "-I",
            str(root),
            "-I",
            str(root / "src"),
            "-I",
            str(CORE_INCLUDE),
            "-c",
            str(root / "case.cpp"),
            "-o",
            str(root / "case.o"),
        ],
        capture_output=True,
        text=True,
    )


def test_a_cxx_sketch_links_against_the_c_library(tmp_path):
    """A C++ sketch resolves the tables and dispatch with C linkage, as the Arduino build does."""
    root = sketch_dir(tmp_path, files())
    (root / "case.cpp").write_text(
        '#define R2S_NODE_NAME "test_cpp"\n'
        + '#define R2S_TOPICS(X) X(Subscription, geometry_msgs::msg::Twist, cmd_vel_sub, "cmd_vel", on_cmd_vel)\n'
        + '#include "ros2serial.h"\n'
        + "void on_cmd_vel(const geometry_msgs::msg::Twist &msg)\n"
        + "{\n"
        + "    (void)msg;\n"
        + "}\n"
        + "int main()\n"
        + "{\n"
        + "    return 0;\n"
        + "}\n",
        encoding="utf-8",
    )

    compiled = subprocess.run(
        [
            "g++",
            "-std=c++11",
            "-Werror",
            "-Wall",
            "-Wextra",
            "-I",
            str(root),
            "-I",
            str(root / "src"),
            "-I",
            str(CORE_INCLUDE),
            "-c",
            str(root / "case.cpp"),
            "-o",
            str(root / "case.o"),
        ],
        capture_output=True,
        text=True,
    )

    assert compiled.returncode == 0, compiled.stderr

    (root / "stubs.c").write_text(STUBS, encoding="utf-8")
    objects = [root / "case.o"]
    objects += [
        compile_unit(root, str(root / name))
        for name in ("stubs.c", "src/r2s_bundled.c", str(CORE_SRC / "codec.c"), str(CORE_SRC / "varint.c"))
    ]
    linked = subprocess.run(
        ["g++", *[str(obj) for obj in objects], "-o", str(root / "case")],
        capture_output=True,
        text=True,
    )

    assert linked.returncode == 0, linked.stderr
