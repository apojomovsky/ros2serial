"""The generate layouts: the exact file set each layout writes, and the generate command.

Covers the G10 output contract: cmake writes the generated pair plus a
CMakeLists static library, arduino writes library.properties plus the pair
under src/, every file names the generator version and the config hash, and
unsupported layouts, C++ output and compact handshakes fail precisely.
"""

import subprocess
from pathlib import Path

import pytest
from r2s_gen import bounds, cli, config, layouts, msgs
from r2s_gen.config import Config, Topic
from r2s_gen.msgs import MessageError
from r2s_gen.version import __version__

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / "core"
EXAMPLES = ROOT / "examples"
GOLDEN = Path(__file__).parent / "golden" / "layouts"

POSIX_TEXT = """\
node: r2s_posix
topics:
  - name: battery
    type: std_msgs/msg/Float32
    direction: pub
  - name: cmd_vel
    type: geometry_msgs/msg/Twist
    direction: sub
  - name: log_text
    type: std_msgs/msg/String
    direction: pub
bounds:
  std_msgs/msg/String.data: 8
"""


def write_config(text, tmp_path):
    path = tmp_path / "r2s.yaml"
    path.write_text(text, encoding="utf-8")
    return path


def load(text, tmp_path):
    return config.load(write_config(text, tmp_path))


def resolve_for(loaded):
    repository = msgs.MessageRepository()
    roots = [topic.type_name for topic in loaded.topics]
    resolved = bounds.resolve(repository, roots, bounds=dict(loaded.bounds))
    return repository, resolved


def generated(layout, tmp_path, text=POSIX_TEXT):
    loaded = load(text, tmp_path)
    repository, resolved = resolve_for(loaded)
    return loaded, layouts.generate(loaded, resolved, repository, layout)


def test_cmake_layout_names_its_exact_file_set():
    assert layouts.files_for("cmake") == ("r2s_generated.h", "r2s_generated.c", "CMakeLists.txt")


def test_arduino_layout_names_its_exact_file_set():
    assert layouts.files_for("arduino") == (
        "library.properties",
        "src/r2s_generated.h",
        "src/r2s_generated.c",
    )


def test_an_unknown_layout_names_the_supported_ones():
    with pytest.raises(MessageError) as caught:
        layouts.files_for("platformio")

    assert str(caught.value) == "--layout: unknown layout 'platformio'; use one of: arduino, cmake"


def test_generate_writes_exactly_the_cmake_file_set(tmp_path):
    _, files = generated("cmake", tmp_path)

    assert sorted(files) == sorted(layouts.files_for("cmake"))


def test_generate_writes_exactly_the_arduino_file_set(tmp_path):
    _, files = generated("arduino", tmp_path)

    assert sorted(files) == sorted(layouts.files_for("arduino"))


def test_both_layouts_share_the_same_generated_pair(tmp_path):
    _, cmake_files = generated("cmake", tmp_path)
    _, arduino_files = generated("arduino", tmp_path)

    assert arduino_files["src/r2s_generated.h"] == cmake_files["r2s_generated.h"]
    assert arduino_files["src/r2s_generated.c"] == cmake_files["r2s_generated.c"]


def test_generate_is_byte_identical_run_to_run(tmp_path):
    _, first = generated("cmake", tmp_path)
    _, second = generated("cmake", tmp_path)

    assert first == second


def test_every_file_names_the_version_and_wrappers_the_config_hash(tmp_path):
    loaded, files = generated("cmake", tmp_path)
    digest = layouts.config_hash(loaded)

    assert len(digest) == 16
    for name, text in files.items():
        assert f"r2s-gen {__version__}" in text, name
        if "r2s_generated" not in name:
            assert digest in text, name


def test_the_config_hash_moves_with_the_config(tmp_path):
    loaded, _ = generated("cmake", tmp_path)
    renamed, _ = generated("cmake", tmp_path, POSIX_TEXT.replace("node: r2s_posix", "node: other"))

    assert layouts.config_hash(loaded) != layouts.config_hash(renamed)


def test_cpp_output_is_rejected_precisely(tmp_path):
    loaded = load(POSIX_TEXT, tmp_path)
    repository, resolved = resolve_for(loaded)

    with pytest.raises(MessageError) as caught:
        layouts.generate(loaded, resolved, repository, "cmake", cpp=True)

    assert str(caught.value) == "--cpp: C++ output is not emitted yet; drop --cpp"


def test_a_compact_handshake_is_rejected_precisely(tmp_path):
    path = write_config(POSIX_TEXT.replace("node: r2s_posix", "node: r2s_posix\nhandshake: compact"), tmp_path)
    loaded = config.load(path)
    repository, resolved = resolve_for(loaded)

    with pytest.raises(MessageError) as caught:
        layouts.generate(loaded, resolved, repository, "cmake", source=str(path))

    assert str(caught.value) == (
        f"{path}: compact handshakes need the node map, which generate does not emit yet; use handshake: full"
    )


def test_the_generate_command_writes_the_cmake_layout(tmp_path, capsys):
    out = tmp_path / "out"

    assert (
        cli.main(
            ["generate", "--config", str(write_config(POSIX_TEXT, tmp_path)), "--out", str(out), "--layout", "cmake"]
        )
        == 0
    )
    assert sorted(path.name for path in out.iterdir()) == ["CMakeLists.txt", "r2s_generated.c", "r2s_generated.h"]


def test_the_generate_command_rejects_an_unknown_layout(tmp_path, capsys):
    out = tmp_path / "out"

    assert (
        cli.main(
            [
                "generate",
                "--config",
                str(write_config(POSIX_TEXT, tmp_path)),
                "--out",
                str(out),
                "--layout",
                "platformio",
            ]
        )
        == 1
    )
    assert "--layout: unknown layout 'platformio'" in capsys.readouterr().err


def configure_lib(tmp_path, files):
    """Configure the cmake layout against the in-tree core, returning the result."""
    source = tmp_path / "lib"
    for name, text in files.items():
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    return subprocess.run(
        ["cmake", "-S", str(source), "-B", str(tmp_path / "build"), f"-DR2S_CORE_DIR={CORE}"],
        capture_output=True,
        text=True,
    )


def test_the_cmake_layout_builds_as_a_static_library(tmp_path):
    _, files = generated("cmake", tmp_path)

    configured = configure_lib(tmp_path, files)
    assert configured.returncode == 0, configured.stderr
    built = subprocess.run(["cmake", "--build", str(tmp_path / "build")], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    assert list((tmp_path / "build").glob("libr2s_generated.a"))


def test_the_cmake_layout_rejects_a_malformed_topic_limit(tmp_path):
    loaded = Config(
        node="wide",
        topics=tuple(Topic(name=f"t{i}", type_name="std_msgs/msg/Float32", direction="pub") for i in range(17)),
        bounds={},
    )
    repository = msgs.MessageRepository()
    resolved = bounds.resolve(repository, ["std_msgs/msg/Float32"])
    files = layouts.generate(loaded, resolved, repository, "cmake")
    broken = files["r2s_generated.h"].replace("#define R2S_TOPIC_MAX_COUNT 17u", "#define R2S_TOPIC_MAX_COUNT 17")
    assert broken != files["r2s_generated.h"]

    configured = configure_lib(tmp_path, dict(files, **{"r2s_generated.h": broken}))

    assert configured.returncode != 0
    assert "malformed topic limit" in configured.stderr


def test_the_cmake_layout_rejects_a_valueless_topic_limit(tmp_path):
    loaded = Config(
        node="wide",
        topics=tuple(Topic(name=f"t{i}", type_name="std_msgs/msg/Float32", direction="pub") for i in range(17)),
        bounds={},
    )
    repository = msgs.MessageRepository()
    resolved = bounds.resolve(repository, ["std_msgs/msg/Float32"])
    files = layouts.generate(loaded, resolved, repository, "cmake")
    broken = files["r2s_generated.h"].replace("#define R2S_TOPIC_MAX_COUNT 17u", "#define R2S_TOPIC_MAX_COUNT")
    assert broken != files["r2s_generated.h"]

    configured = configure_lib(tmp_path, dict(files, **{"r2s_generated.h": broken}))

    assert configured.returncode != 0
    assert "malformed topic limit" in configured.stderr


def test_the_arduino_properties_name_the_node_and_the_toolchain(tmp_path):
    _, files = generated("arduino", tmp_path)

    properties = files["library.properties"]
    assert "name=r2s_posix" in properties.splitlines()
    assert "architectures=avr" in properties.splitlines()


def test_the_cmake_example_matches_the_generator(tmp_path):
    config_path = EXAMPLES / "cmake_minimal" / "r2s.yaml"
    loaded = config.load(config_path)
    repository = msgs.MessageRepository()
    roots = [topic.type_name for topic in loaded.topics]
    resolved = bounds.resolve(repository, roots, bounds=dict(loaded.bounds))
    files = layouts.generate(loaded, resolved, repository, "cmake", source=config_path.name)

    for name, text in files.items():
        assert text == (EXAMPLES / "cmake_minimal" / "lib" / name).read_text(encoding="utf-8"), name


def test_the_arduino_example_matches_the_generator(tmp_path):
    # The Arduino example declares its topics at runtime, so it has no
    # r2s.yaml and no generated output to pin. The arduino layout's
    # generated pair is the cmake layout's pair, so the cmake example
    # covers both layouts' bytes.
    config_path = EXAMPLES / "cmake_minimal" / "r2s.yaml"
    loaded = config.load(config_path)
    repository = msgs.MessageRepository()
    roots = [topic.type_name for topic in loaded.topics]
    resolved = bounds.resolve(repository, roots, bounds=dict(loaded.bounds))
    files = layouts.generate(loaded, resolved, repository, "arduino", source=config_path.name)

    for name in ("src/r2s_generated.h", "src/r2s_generated.c"):
        assert files[name] == (EXAMPLES / "cmake_minimal" / "lib" / Path(name).name).read_text(encoding="utf-8"), name


def test_the_arduino_layout_names_the_node_and_the_config(tmp_path):
    config_path = EXAMPLES / "cmake_minimal" / "r2s.yaml"
    loaded = config.load(config_path)
    repository = msgs.MessageRepository()
    roots = [topic.type_name for topic in loaded.topics]
    resolved = bounds.resolve(repository, roots, bounds=dict(loaded.bounds))
    files = layouts.generate(loaded, resolved, repository, "arduino", source=config_path.name)

    assert files["library.properties"] == (GOLDEN / "library.properties").read_text(encoding="utf-8")


def test_build_defines_names_the_codec_flag_and_limits(tmp_path):
    _, files = generated("cmake", tmp_path)

    assert layouts.build_defines(files["r2s_generated.h"]) == [
        "R2S_WITH_CODEC",
        "R2S_MAX_FRAME=52",
        "R2S_MAX_NEST=2",
    ]


def test_build_defines_carries_the_raised_topic_limit(tmp_path):
    loaded = Config(
        node="wide",
        topics=tuple(Topic(name=f"t{i}", type_name="std_msgs/msg/Float32", direction="pub") for i in range(17)),
        bounds={},
    )
    repository = msgs.MessageRepository()
    resolved = bounds.resolve(repository, ["std_msgs/msg/Float32"])
    files = layouts.generate(loaded, resolved, repository, "cmake")

    assert layouts.build_defines(files["r2s_generated.h"])[-1] == "R2S_TOPIC_MAX_COUNT=17"


def test_build_defines_rejects_a_header_without_the_frame_limit(tmp_path):
    _, files = generated("cmake", tmp_path)
    broken = files["r2s_generated.h"].replace("#define R2S_MAX_FRAME 52u", "#define R2S_MAX_FRAME 52")

    with pytest.raises(MessageError) as caught:
        layouts.build_defines(broken)

    assert str(caught.value) == "r2s_generated.h: no R2S_MAX_FRAME limit; regenerate the sources"


def test_build_defines_rejects_a_header_without_the_nest_limit(tmp_path):
    _, files = generated("cmake", tmp_path)
    broken = files["r2s_generated.h"].replace("#define R2S_MAX_NEST 2u", "#define R2S_MAX_NEST 2")

    with pytest.raises(MessageError) as caught:
        layouts.build_defines(broken)

    assert str(caught.value) == "r2s_generated.h: no R2S_MAX_NEST limit; regenerate the sources"


def test_build_defines_rejects_a_malformed_topic_limit(tmp_path):
    loaded = Config(
        node="wide",
        topics=tuple(Topic(name=f"t{i}", type_name="std_msgs/msg/Float32", direction="pub") for i in range(17)),
        bounds={},
    )
    repository = msgs.MessageRepository()
    resolved = bounds.resolve(repository, ["std_msgs/msg/Float32"])
    files = layouts.generate(loaded, resolved, repository, "cmake")
    broken = files["r2s_generated.h"].replace("#define R2S_TOPIC_MAX_COUNT 17u", "#define R2S_TOPIC_MAX_COUNT 17")

    with pytest.raises(MessageError) as caught:
        layouts.build_defines(broken)

    assert str(caught.value) == "r2s_generated.h: no R2S_TOPIC_MAX_COUNT limit; regenerate the sources"


def test_build_defines_rejects_a_valueless_topic_limit(tmp_path):
    loaded = Config(
        node="wide",
        topics=tuple(Topic(name=f"t{i}", type_name="std_msgs/msg/Float32", direction="pub") for i in range(17)),
        bounds={},
    )
    repository = msgs.MessageRepository()
    resolved = bounds.resolve(repository, ["std_msgs/msg/Float32"])
    files = layouts.generate(loaded, resolved, repository, "cmake")
    broken = files["r2s_generated.h"].replace("#define R2S_TOPIC_MAX_COUNT 17u", "#define R2S_TOPIC_MAX_COUNT")

    with pytest.raises(MessageError) as caught:
        layouts.build_defines(broken)

    assert str(caught.value) == "r2s_generated.h: no R2S_TOPIC_MAX_COUNT limit; regenerate the sources"


ENCODE_MAIN = """\
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "r2s_generated.h"

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
    (void)buf;
    (void)max;
    return 0u;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
    (void)buf;
    (void)len;
}

uint16_t r2s_port_tx_free(void)
{
    return 0xFFFFu;
}

uint32_t r2s_port_tick(void)
{
    return 0u;
}

void r2s_on_cmd_vel(const geometry_msgs__msg__Twist *msg)
{
    (void)msg;
}

int main(void)
{
    geometry_msgs__msg__Twist msg;
    static uint8_t buf[128];
    r2s_codec_out_t out = {buf, sizeof buf, 0u, 0u};
    uint8_t field = 0xFFu;
    int8_t rc;

    memset(&msg, 0, sizeof msg);
    msg.linear.x = 1.0;
    rc = r2s_codec_encode(&r2s_generated_table, R2S_TOPIC_CMD_VEL_TYPE, &msg, 0u, &out, &field);
    printf("rc=%d len=%u field=%u\\n", rc, out.len, field);
    return rc == 0 ? 0 : 1;
}
"""


def run_encode(tmp_path, header, source, defines):
    """Compile the pair with the core and run the nested Twist encode."""
    (tmp_path / "r2s_generated.h").write_text(header, encoding="utf-8")
    (tmp_path / "r2s_generated.c").write_text(source, encoding="utf-8")
    (tmp_path / "encode_main.c").write_text(ENCODE_MAIN, encoding="utf-8")
    core_sources = [str(path) for path in sorted((CORE / "src").glob("*.c")) if path.name != "test_reset.c"]
    compiled = subprocess.run(
        ["cc", "-std=c99", "-pedantic", "-Werror", "-Wall", "-Wextra", "-I", str(tmp_path), "-I", str(CORE / "include")]
        + [f"-D{define}" for define in defines]
        + core_sources
        + [str(tmp_path / "r2s_generated.c"), str(tmp_path / "encode_main.c"), "-o", str(tmp_path / "encode_main")],
        capture_output=True,
        text=True,
    )
    assert compiled.returncode == 0, compiled.stderr
    return subprocess.run([str(tmp_path / "encode_main")], capture_output=True, text=True)


def test_the_defines_make_the_nested_twist_encode_succeed(tmp_path):
    _, files = generated("cmake", tmp_path)
    defines = layouts.build_defines(files["r2s_generated.h"])

    completed = run_encode(tmp_path, files["r2s_generated.h"], files["r2s_generated.c"], defines)

    assert completed.returncode == 0, completed.stdout
    assert "len=48" in completed.stdout


def test_the_encode_fails_without_the_defines(tmp_path):
    _, files = generated("cmake", tmp_path)

    completed = run_encode(tmp_path, files["r2s_generated.h"], files["r2s_generated.c"], [])

    assert completed.returncode != 0
    assert "rc=-2" in completed.stdout
