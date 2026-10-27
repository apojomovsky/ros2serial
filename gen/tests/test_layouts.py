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
from r2s_gen.msgs import MessageError
from r2s_gen.version import __version__

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / "core"
EXAMPLES = ROOT / "examples"

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


def test_every_file_names_the_version_and_config_hash(tmp_path):
    loaded, files = generated("cmake", tmp_path)
    digest = layouts.config_hash(loaded)

    assert len(digest) == 16
    for name, text in files.items():
        assert f"r2s-gen {__version__}" in text, name
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
    loaded = load(POSIX_TEXT.replace("node: r2s_posix", "node: r2s_posix\nhandshake: compact"), tmp_path)
    repository, resolved = resolve_for(loaded)

    with pytest.raises(MessageError) as caught:
        layouts.generate(loaded, resolved, repository, "cmake")

    assert str(caught.value) == (
        f"{tmp_path / 'r2s.yaml'}: compact handshakes need the node map, which generate does not emit yet; "
        "use handshake: full"
    )


def test_the_generate_command_writes_the_cmake_layout(tmp_path, capsys):
    out = tmp_path / "out"

    assert cli.main(["generate", "--config", str(write_config(POSIX_TEXT, tmp_path)), "--out", str(out)]) == 0
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
    assert "--layout: unknown layout 'platformio'" in capsys.readerr().err


def test_the_cmake_layout_builds_as_a_static_library(tmp_path):
    _, files = generated("cmake", tmp_path)
    source = tmp_path / "lib"
    for name, text in files.items():
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    build = tmp_path / "build"

    configured = subprocess.run(
        ["cmake", "-S", str(source), "-B", str(build), f"-DR2S_CORE_DIR={CORE}"],
        capture_output=True,
        text=True,
    )
    assert configured.returncode == 0, configured.stderr
    built = subprocess.run(["cmake", "--build", str(build)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    assert list(build.glob("libr2s_generated.a"))


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
        assert text == (EXAMPLES / "cmake_minimal" / name).read_text(encoding="utf-8"), name


def test_the_arduino_example_matches_the_generator(tmp_path):
    config_path = EXAMPLES / "arduino_minimal" / "r2s.yaml"
    loaded = config.load(config_path)
    repository = msgs.MessageRepository()
    roots = [topic.type_name for topic in loaded.topics]
    resolved = bounds.resolve(repository, roots, bounds=dict(loaded.bounds))
    files = layouts.generate(loaded, resolved, repository, "arduino", source=config_path.name)

    for name, text in files.items():
        assert text == (EXAMPLES / "arduino_minimal" / "lib" / name).read_text(encoding="utf-8"), name
