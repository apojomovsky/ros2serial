"""Config validation: every schema rule has a failing config with a golden message."""

import json

import jsonschema
import pytest
from r2s_gen import cli, config
from r2s_gen.msgs import MessageError

VALID = """\
node: base_controller
topics:
  - name: battery
    type: std_msgs/msg/Float32
    direction: pub
"""


def write(path, text):
    path.write_text(text, encoding="utf-8")
    return path


def load_text(tmp_path, text):
    return config.load(write(tmp_path / "r2s.yaml", text))


def test_a_valid_config_loads_with_the_defaults_filled(tmp_path):
    loaded = load_text(tmp_path, VALID)

    assert loaded.node == "base_controller"
    assert loaded.distro == "jazzy"
    assert loaded.handshake == "full"
    assert loaded.tier == "M"
    assert loaded.features == ("time", "log")
    assert [(topic.name, topic.type_name, topic.direction) for topic in loaded.topics] == [
        ("battery", "std_msgs/msg/Float32", "pub")
    ]
    assert loaded.topics[0].qos == "best_effort"
    assert loaded.topics[0].narrow_float64 is False
    assert loaded.bounds == {}
    assert loaded.default_string_bound is None
    assert loaded.max_frame is None


def test_a_full_config_keeps_every_value(tmp_path):
    loaded = load_text(
        tmp_path,
        """\
node: base_controller
distro: lyrical
handshake: compact
tier: L
features: [time, params]
topics:
  - name: cmd_vel
    type: geometry_msgs/msg/Twist
    direction: sub
    qos: keep_latest
    narrow_float64: true
bounds:
  default_string_bound: 16
  std_msgs/msg/Header.frame_id: 24
  sensor_msgs/msg/JointState.name: [4, 12]
limits:
  max_frame: 256
""",
    )

    assert loaded.distro == "lyrical"
    assert loaded.handshake == "compact"
    assert loaded.tier == "L"
    assert loaded.features == ("time", "params")
    assert loaded.topics[0].qos == "keep_latest"
    assert loaded.topics[0].narrow_float64 is True
    assert loaded.bounds == {
        "std_msgs/msg/Header.frame_id": 24,
        "sensor_msgs/msg/JointState.name": [4, 12],
    }
    assert loaded.default_string_bound == 16
    assert loaded.max_frame == 256


def test_the_schema_file_is_a_valid_json_schema():
    schema = json.loads(config.SCHEMA_PATH.read_text(encoding="utf-8"))
    jsonschema.validators.validator_for(schema).check_schema(schema)


def test_a_missing_file_is_an_error_naming_the_path(tmp_path):
    missing = tmp_path / "r2s.yaml"

    with pytest.raises(MessageError) as caught:
        config.load(missing)

    assert (
        str(caught.value)
        == f"{missing}: cannot read the config ([Errno 2] No such file or directory: '{missing}'); check the path"
    )


def test_invalid_yaml_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "\tnode: base_controller\n")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == f"{path}: not valid YAML; fix the indentation and quoting"


def test_a_list_root_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "- node: base_controller\n")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == f"{path}: the config must be a mapping of keys; start the file with node: <name>"


def test_a_missing_node_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "topics: []\n")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == f"{path}: missing required key 'node'; add node: <name>"


def test_missing_topics_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "node: base_controller\n")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == f"{path}: missing required key 'topics'; add topics: with one entry per topic"


def test_a_numeric_node_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "node: 42\ntopics: []\n")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == f"{path}: node: expected a string; write the node name, such as node: base_controller"


def test_an_invalid_node_name_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "node: 9lives\ntopics: []\n")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == (
        f"{path}: node: '9lives' is not a valid ROS node name; "
        "use letters, digits and underscores, starting with a letter"
    )


def test_an_unknown_distro_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "node: base_controller\ndistro: lunar\ntopics: []\n")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == f"{path}: distro: 'lunar' is not a known distro; use one of: jazzy, lyrical, rolling"


def test_an_unknown_handshake_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "node: base_controller\nhandshake: shake\ntopics: []\n")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == f"{path}: handshake: 'shake' is not valid; use one of: full, compact"


def test_an_unknown_tier_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "node: base_controller\ntier: XL\ntopics: []\n")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == f"{path}: tier: 'XL' is not valid; use one of: S, M, L"


def test_nonlist_features_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "node: base_controller\nfeatures: time\ntopics: []\n")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert (
        str(caught.value)
        == f"{path}: features: expected a list; write one feature per line, such as features: [time, log]"
    )


def test_an_unknown_feature_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "node: base_controller\nfeatures: [time, nap]\ntopics: []\n")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == (
        f"{path}: features[1]: 'nap' is not a known feature; use one of: time, log, params, reliable, rx_ring"
    )


def test_nonlist_topics_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "node: base_controller\ntopics:\n  name: battery\n")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert (
        str(caught.value)
        == f"{path}: topics: expected a list; write one entry per topic, starting each with - name: <topic>"
    )


def test_a_topic_missing_direction_is_an_error(tmp_path):
    path = write(
        tmp_path / "r2s.yaml",
        "node: base_controller\ntopics:\n  - name: battery\n    type: std_msgs/msg/Float32\n",
    )

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert (
        str(caught.value)
        == f"{path}: topics[0]: missing required key 'direction'; add direction: pub or direction: sub"
    )


def test_an_absolute_topic_name_is_an_error(tmp_path):
    path = write(
        tmp_path / "r2s.yaml",
        "node: base_controller\ntopics:\n  - name: /battery\n    type: std_msgs/msg/Float32\n    direction: pub\n",
    )

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == (
        f"{path}: topics[0].name: '/battery' is not a relative topic name; drop the leading slash, such as battery"
    )


def test_a_topic_name_with_a_space_is_an_error(tmp_path):
    path = write(
        tmp_path / "r2s.yaml",
        "node: base_controller\ntopics:\n  - name: bat tery\n    type: std_msgs/msg/Float32\n    direction: pub\n",
    )

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == (
        f"{path}: topics[0].name: 'bat tery' is not a valid topic name; "
        "use letters, digits, underscores and slashes, starting with a letter"
    )


def test_a_short_type_is_an_error(tmp_path):
    path = write(
        tmp_path / "r2s.yaml",
        "node: base_controller\ntopics:\n  - name: battery\n    type: std_msgs/Float32\n    direction: pub\n",
    )

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == (
        f"{path}: topics[0].type: 'std_msgs/Float32' is not a message type; "
        "write package/msg/Type, such as std_msgs/msg/Float32"
    )


def test_an_unknown_direction_is_an_error(tmp_path):
    path = write(
        tmp_path / "r2s.yaml",
        "node: base_controller\ntopics:\n  - name: battery\n    type: std_msgs/msg/Float32\n    direction: publish\n",
    )

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == f"{path}: topics[0].direction: 'publish' is not valid; use one of: pub, sub"


def test_an_unknown_qos_is_an_error(tmp_path):
    path = write(
        tmp_path / "r2s.yaml",
        "node: base_controller\ntopics:\n  - name: battery\n    type: std_msgs/msg/Float32\n    direction: pub\n    qos: fast\n",
    )

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == (
        f"{path}: topics[0].qos: 'fast' is not valid; use one of: best_effort, keep_latest, reliable"
    )


def test_a_string_narrow_flag_is_an_error(tmp_path):
    path = write(
        tmp_path / "r2s.yaml",
        "node: base_controller\ntopics:\n  - name: battery\n    type: std_msgs/msg/Float32\n    direction: pub\n    narrow_float64: sometimes\n",
    )

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == (
        f"{path}: topics[0].narrow_float64: expected true or false; "
        "write narrow_float64: true to send float64 fields as binary32"
    )


def test_duplicate_topic_names_are_an_error(tmp_path):
    path = write(
        tmp_path / "r2s.yaml",
        "node: base_controller\ntopics:\n"
        "  - name: battery\n    type: std_msgs/msg/Float32\n    direction: pub\n"
        "  - name: battery\n    type: std_msgs/msg/Float32\n    direction: sub\n",
    )

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == f"{path}: topics[1].name: duplicate topic name 'battery'; give each topic its own name"


def test_an_unknown_root_key_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "node: base_controller\nnodes: 2\ntopics: []\n")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == (
        f"{path}: unknown key 'nodes'; "
        "remove it (expected one of: bounds, distro, features, handshake, limits, node, tier, topics)"
    )


def test_an_unknown_topic_key_is_an_error(tmp_path):
    path = write(
        tmp_path / "r2s.yaml",
        "node: base_controller\ntopics:\n  - name: battery\n    type: std_msgs/msg/Float32\n    direction: pub\n    rate: 10\n",
    )

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == (
        f"{path}: topics[0]: unknown key 'rate'; "
        "remove it (expected one of: direction, name, narrow_float64, qos, type)"
    )


def test_a_short_bounds_key_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "node: base_controller\ntopics: []\nbounds:\n  Float32.data: 8\n")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == (
        f"{path}: bounds: 'Float32.data' is not a field path; "
        "write package/msg/Type.field, such as std_msgs/msg/Header.frame_id"
    )


def test_a_string_bound_value_is_an_error(tmp_path):
    path = write(
        tmp_path / "r2s.yaml", "node: base_controller\ntopics: []\nbounds:\n  std_msgs/msg/Header.frame_id: lots\n"
    )

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == (
        f"{path}: bounds['std_msgs/msg/Header.frame_id']: expected an integer bound or [count, string_bound]; "
        "write 24 or [4, 12]"
    )


def test_a_bounds_pair_needs_two_entries(tmp_path):
    path = write(
        tmp_path / "r2s.yaml", "node: base_controller\ntopics: []\nbounds:\n  sensor_msgs/msg/JointState.name: [4]\n"
    )

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == (
        f"{path}: bounds['sensor_msgs/msg/JointState.name']: a list bound is [count, string_bound]; "
        "give the count and the string bound"
    )


def test_a_zero_sequence_count_is_an_error(tmp_path):
    path = write(
        tmp_path / "r2s.yaml",
        "node: base_controller\ntopics: []\nbounds:\n  sensor_msgs/msg/JointState.name: [0, 12]\n",
    )

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == (
        f"{path}: bounds['sensor_msgs/msg/JointState.name']: the sequence bound must be at least 1; raise it"
    )


def test_a_negative_bound_is_an_error(tmp_path):
    path = write(
        tmp_path / "r2s.yaml", "node: base_controller\ntopics: []\nbounds:\n  std_msgs/msg/Header.frame_id: -1\n"
    )

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == (
        f"{path}: bounds['std_msgs/msg/Header.frame_id']: the bound must be at least 0; raise it"
    )


def test_a_string_default_string_bound_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "node: base_controller\ntopics: []\nbounds:\n  default_string_bound: many\n")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert (
        str(caught.value)
        == f"{path}: bounds.default_string_bound: expected an integer; give a whole number of characters"
    )


def test_a_negative_default_string_bound_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "node: base_controller\ntopics: []\nbounds:\n  default_string_bound: -1\n")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == f"{path}: bounds.default_string_bound: must be at least 0; raise it"


def test_a_string_max_frame_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "node: base_controller\ntopics: []\nlimits:\n  max_frame: big\n")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == f"{path}: limits.max_frame: expected an integer; give a whole number of bytes"


def test_a_zero_max_frame_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "node: base_controller\ntopics: []\nlimits:\n  max_frame: 0\n")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == f"{path}: limits.max_frame: must be at least 1; raise it"


def test_an_empty_file_is_an_error(tmp_path):
    path = write(tmp_path / "r2s.yaml", "")

    with pytest.raises(MessageError) as caught:
        config.load(path)

    assert str(caught.value) == f"{path}: the config must be a mapping of keys; start the file with node: <name>"


def test_check_accepts_a_valid_config(capsys, tmp_path):
    path = write(tmp_path / "r2s.yaml", VALID)

    assert cli.main(["check", "--config", str(path)]) == 0
    out, err = capsys.readouterr()
    assert out == ""
    assert err == ""


def test_check_rejects_an_invalid_config_with_the_golden_message(capsys, tmp_path):
    path = write(tmp_path / "r2s.yaml", "node: base_controller\ndistro: lunar\ntopics: []\n")

    assert cli.main(["check", "--config", str(path)]) == 1
    out, err = capsys.readouterr()
    assert out == ""
    assert err == (
        f"r2s-gen: error: {path}: distro: 'lunar' is not a known distro; use one of: jazzy, lyrical, rolling\n"
    )


def test_check_rejects_a_missing_file(capsys, tmp_path):
    missing = tmp_path / "r2s.yaml"

    assert cli.main(["check", "--config", str(missing)]) == 1
    out, err = capsys.readouterr()
    assert out == ""
    assert f"r2s-gen: error: {missing}: cannot read the config" in err
