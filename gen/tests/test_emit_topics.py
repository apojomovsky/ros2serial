"""C emission: the topic table, publish wrappers, dispatch and limits.

Covers the static path past the descriptors: IDs from 16 in configuration
order, depth-first type indices, RIHS01 hashes, per-direction limits, and the
name collision checks. The golden pair pins the exact bytes; the compile test
proves they are C99.
"""

import subprocess
from pathlib import Path

import pytest
from r2s_gen import bounds, config, msgs, topics
from r2s_gen import typehash as typehash_module
from r2s_gen.config import Config, Topic
from r2s_gen.msgs import MessageError

GOLDEN = Path(__file__).parent / "golden" / "topics"
CORE_INCLUDE = Path(__file__).resolve().parents[2] / "core" / "include"

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


def load(text, tmp_path):
    path = tmp_path / "r2s.yaml"
    path.write_text(text, encoding="utf-8")
    return config.load(path)


def write_config(text, tmp_path):
    path = tmp_path / "r2s.yaml"
    path.write_text(text, encoding="utf-8")
    return path


def resolve_for(loaded):
    repository = msgs.MessageRepository()
    roots = [topic.type_name for topic in loaded.topics]
    bound_map = dict(loaded.bounds)
    resolved = bounds.resolve(repository, roots, bounds=bound_map)
    return repository, resolved


def build_loaded(loaded, source="r2s.yaml"):
    repository, resolved = resolve_for(loaded)
    return topics.build(loaded, resolved, repository, source=source)


def test_slash_names_map_to_underscore_identifiers():
    assert topics.c_identifier("sensors/battery") == "sensors_battery"
    assert topics.c_identifier("battery") == "battery"


def test_a_slash_and_an_underscore_name_collide():
    loaded = Config(
        node="base_controller",
        topics=(
            Topic(name="a/b", type_name="std_msgs/msg/Float32", direction="pub"),
            Topic(name="a_b", type_name="std_msgs/msg/Float32", direction="pub"),
        ),
        distro="jazzy",
        handshake="full",
        tier="M",
        features=("time", "log"),
        bounds={},
        default_string_bound=None,
        max_frame=None,
    )

    with pytest.raises(MessageError) as caught:
        build_loaded(loaded)

    assert str(caught.value) == (
        "r2s.yaml: topics[1].name: topic name 'a_b' collides with 'a/b' as C identifier 'a_b'; rename one of the topics"
    )


def test_a_collision_is_already_a_check_error(tmp_path):
    path = write_config(
        "node: base_controller\ntopics:\n"
        "  - name: a/b\n    type: std_msgs/msg/Float32\n    direction: pub\n"
        "  - name: a_b\n    type: std_msgs/msg/Float32\n    direction: pub\n",
        tmp_path,
    )

    with pytest.raises(MessageError) as caught:
        config.load(path)
    assert str(caught.value) == (
        f"{path}: topics[1].name: topic name 'a_b' collides with 'a/b' as C identifier 'a_b'; rename one of the topics"
    )


def test_names_differing_only_by_case_collide():
    loaded = Config(
        node="base_controller",
        topics=(
            Topic(name="battery", type_name="std_msgs/msg/Float32", direction="pub"),
            Topic(name="BATTERY", type_name="std_msgs/msg/Float32", direction="pub"),
        ),
        distro="jazzy",
        handshake="full",
        tier="M",
        features=("time", "log"),
        bounds={},
        default_string_bound=None,
        max_frame=None,
    )

    with pytest.raises(MessageError) as caught:
        build_loaded(loaded)

    assert str(caught.value) == (
        "r2s.yaml: topics[1].name: topic name 'BATTERY' collides with 'battery' as C identifier 'BATTERY'; "
        "rename one of the topics"
    )


@pytest.mark.parametrize("name", ["one", "rx_buf", "int", "switch"])
def test_taken_c_names_are_an_error(name):
    loaded = Config(
        node="base_controller",
        topics=(Topic(name=name, type_name="std_msgs/msg/Float32", direction="pub"),),
        distro="jazzy",
        handshake="full",
        tier="M",
        features=("time", "log"),
        bounds={},
        default_string_bound=None,
        max_frame=None,
    )

    with pytest.raises(MessageError) as caught:
        build_loaded(loaded)

    assert str(caught.value) == (
        f"r2s.yaml: topics[0].name: topic name '{name}' has no C identifier the generated file can use "
        f"('{name}' is taken); rename the topic"
    )


def test_same_name_twice_stays_a_duplicate_error(tmp_path):
    path = write_config(
        "node: base_controller\ntopics:\n"
        "  - name: battery\n    type: std_msgs/msg/Float32\n    direction: pub\n"
        "  - name: battery\n    type: std_msgs/msg/Float32\n    direction: sub\n",
        tmp_path,
    )

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert "duplicate topic name 'battery'" in str(caught.value)


def test_topic_ids_start_at_16_in_configuration_order(tmp_path):
    table = build_loaded(load(POSIX_TEXT, tmp_path))

    assert [(entry.name, entry.topic_id) for entry in table.entries] == [
        ("battery", 16),
        ("cmd_vel", 17),
        ("log_text", 18),
    ]


def test_type_indices_follow_the_depth_first_order(tmp_path):
    table = build_loaded(load(POSIX_TEXT, tmp_path))

    assert [(entry.name, entry.type_index) for entry in table.entries] == [
        ("battery", 0),
        ("cmd_vel", 2),
        ("log_text", 3),
    ]


def test_hashes_are_the_first_8_bytes_of_the_rihs01_hash(tmp_path):
    table = build_loaded(load(POSIX_TEXT, tmp_path))
    repository = msgs.MessageRepository()

    for entry in table.entries:
        digest = typehash_module.type_hash(repository, entry.type_name)
        assert entry.type_hash == bytes.fromhex(digest[len("RIHS01_") :])[:8]


def test_direction_and_qos_set_the_flags():
    loaded = Config(
        node="base_controller",
        topics=(
            Topic(name="plain", type_name="std_msgs/msg/Float32", direction="pub"),
            Topic(name="sub", type_name="std_msgs/msg/Float32", direction="sub"),
            Topic(name="kept", type_name="std_msgs/msg/Float32", direction="sub", qos="keep_latest"),
            Topic(name="rel", type_name="std_msgs/msg/Float32", direction="pub", qos="reliable"),
            Topic(name="narrow", type_name="geometry_msgs/msg/Twist", direction="sub", narrow_float64=True),
        ),
        distro="jazzy",
        handshake="full",
        tier="M",
        features=("time", "log"),
        bounds={},
        default_string_bound=None,
        max_frame=None,
    )
    repository = msgs.MessageRepository()
    resolved = bounds.resolve(repository, ["std_msgs/msg/Float32", "geometry_msgs/msg/Twist"])
    table = topics.build(loaded, resolved, repository)

    assert [(entry.name, entry.flags) for entry in table.entries] == [
        ("plain", 0x00),
        ("sub", 0x01),
        ("kept", 0x05),
        ("rel", 0x08),
        ("narrow", 0x03),
    ]


def test_max_frame_covers_the_largest_payload_and_the_control_frames(tmp_path):
    table = build_loaded(load(POSIX_TEXT, tmp_path))

    # covers: FRM-9
    assert table.max_frame == 52


def test_a_max_frame_override_is_used(tmp_path):
    loaded = load(POSIX_TEXT + "limits:\n  max_frame: 128\n", tmp_path)

    assert build_loaded(loaded).max_frame == 128


def test_a_max_frame_override_below_the_need_is_an_error(tmp_path):
    loaded = load(POSIX_TEXT + "limits:\n  max_frame: 40\n", tmp_path)

    with pytest.raises(MessageError) as caught:
        build_loaded(loaded)

    assert str(caught.value) == (
        "r2s.yaml: limits.max_frame: max_frame 40 is below the 52 the configuration needs; raise it"
    )


def test_a_max_frame_above_the_protocol_limit_is_an_error(tmp_path):
    loaded = load(POSIX_TEXT + "limits:\n  max_frame: 65278\n", tmp_path)

    with pytest.raises(MessageError) as caught:
        build_loaded(loaded)

    assert str(caught.value) == (
        "r2s.yaml: limits.max_frame: max_frame 65278 exceeds the protocol limit of 65277; lower it"
    )


def test_more_than_240_topics_is_an_error():
    loaded = Config(
        node="base_controller",
        topics=tuple(Topic(name=f"t{i}", type_name="std_msgs/msg/Float32", direction="pub") for i in range(241)),
        distro="jazzy",
        handshake="full",
        tier="M",
        features=("time", "log"),
        bounds={},
        default_string_bound=None,
        max_frame=None,
    )

    with pytest.raises(MessageError) as caught:
        topics.build(loaded, {}, msgs.MessageRepository())

    assert str(caught.value) == (
        "r2s.yaml: topics: 241 topics exceed the 240 user topic IDs (16 to 255); remove some topics"
    )


def generated_pair(tmp_path):
    loaded = load(POSIX_TEXT, tmp_path)
    repository, resolved = resolve_for(loaded)
    return topics.generate(loaded, resolved, repository)


def test_the_golden_pair_is_emitted(tmp_path):
    header, source = generated_pair(tmp_path)

    assert header == (GOLDEN / "posix.h").read_text(encoding="utf-8")
    assert source == (GOLDEN / "posix.c").read_text(encoding="utf-8")


def compile_pair(tmp_path, header, source):
    """A generated pair that compiles as C99 with the core headers and -Werror."""
    (tmp_path / "r2s_generated.h").write_text(header, encoding="utf-8")
    (tmp_path / "r2s_generated.c").write_text(source, encoding="utf-8")

    compiled = subprocess.run(
        [
            "cc",
            "-std=c99",
            "-pedantic",
            "-Werror",
            "-Wall",
            "-Wextra",
            "-I",
            str(tmp_path),
            "-I",
            str(CORE_INCLUDE),
            "-DR2S_WITH_CODEC",
            "-c",
            str(tmp_path / "r2s_generated.c"),
            "-o",
            str(tmp_path / "r2s_generated.o"),
        ],
        capture_output=True,
        text=True,
    )

    assert compiled.returncode == 0, compiled.stderr


def test_the_golden_pair_compiles_as_c99(tmp_path):
    header, source = generated_pair(tmp_path)

    compile_pair(tmp_path, header, source)


def test_a_publish_only_config_has_no_receive_buffer(tmp_path):
    loaded = load(
        "node: base_controller\ntopics:\n  - name: battery\n    type: std_msgs/msg/Float32\n    direction: pub\n",
        tmp_path,
    )
    repository, resolved = resolve_for(loaded)
    header, source = topics.generate(loaded, resolved, repository)

    assert "r2s_publish_battery" in header
    assert "union" not in source
    assert "switch (topic_id)" not in source
    assert "r2s_framer_count_refused" in source


def test_a_subscribe_only_config_compiles_without_a_publish_path(tmp_path):
    loaded = load(
        "node: base_controller\ntopics:\n  - name: cmd_vel\n    type: geometry_msgs/msg/Twist\n    direction: sub\n",
        tmp_path,
    )
    repository, resolved = resolve_for(loaded)
    header, source = topics.generate(loaded, resolved, repository)

    assert "r2s_publish_one" not in source
    assert "r2s_on_cmd_vel" in header
    compile_pair(tmp_path, header, source)


def test_a_session_table_above_16_raises_the_session_limit():
    loaded = Config(
        node="base_controller",
        topics=tuple(Topic(name=f"t{i}", type_name="std_msgs/msg/Float32", direction="pub") for i in range(17)),
        distro="jazzy",
        handshake="full",
        tier="M",
        features=("time", "log"),
        bounds={},
        default_string_bound=None,
        max_frame=None,
    )
    repository = msgs.MessageRepository()
    resolved = bounds.resolve(repository, ["std_msgs/msg/Float32"])
    header, _source = topics.generate(loaded, resolved, repository)

    assert "#define R2S_SESSION_MAX_TOPICS 17u" in header
