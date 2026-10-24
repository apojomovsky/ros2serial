"""Message loading: the .msg grammar, the search order and the bundled snapshots."""

from pathlib import Path

import pytest
from r2s_gen import msgs
from support import write_message

DISTROS = ["jazzy", "lyrical", "rolling"]


def source_tree(tmp_path: Path) -> Path:
    """A directory of packages in a source tree layout: <root>/<package>/msg/<Type>.msg."""
    return tmp_path / "messages"


def installed_prefix(tmp_path: Path, type_name: str, text: str) -> Path:
    """A ROS install prefix: <prefix>/share/<package>/msg/<Type>.msg."""
    package, _, name = type_name.partition("/msg/")
    path = tmp_path / "prefix" / "share" / package / "msg" / f"{name}.msg"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return tmp_path / "prefix"


def test_a_message_path_wins_over_the_snapshot(tmp_path):
    root = source_tree(tmp_path)
    write_message(root, "std_msgs/msg/Float32", "int16 data\n")
    repository = msgs.MessageRepository(msg_paths=[root])

    message = repository.load("std_msgs/msg/Float32")

    assert message.field("data").type_name == "int16"


def test_a_sourced_prefix_is_searched_after_the_message_paths(tmp_path):
    root = source_tree(tmp_path)
    write_message(root, "std_msgs/msg/Float32", "int16 data\n")
    prefix = installed_prefix(tmp_path, "std_msgs/msg/Float32", "int32 data\n")
    repository = msgs.MessageRepository(msg_paths=[root], ament_prefix_path=[prefix])

    message = repository.load("std_msgs/msg/Float32")

    assert message.field("data").type_name == "int16"


def test_a_sourced_prefix_answers_when_no_message_path_does(tmp_path):
    prefix = installed_prefix(tmp_path, "std_msgs/msg/Float32", "int32 data\n")
    repository = msgs.MessageRepository(ament_prefix_path=[prefix])

    message = repository.load("std_msgs/msg/Float32")

    assert message.field("data").type_name == "int32"


def test_the_snapshot_answers_when_no_path_matches():
    repository = msgs.MessageRepository()

    message = repository.load("std_msgs/msg/Float32")

    assert message.field("data").type_name == "float32"
    assert "snapshots" in message.path.parts


@pytest.mark.parametrize("distro", DISTROS)
def test_every_bundled_distro_resolves_joint_state(distro):
    repository = msgs.MessageRepository(distro=distro)

    message = repository.load("sensor_msgs/msg/JointState")

    assert [field.name for field in message.fields] == ["header", "name", "position", "velocity", "effort"]
    assert message.path.parts[-4] == distro


def test_the_message_records_the_file_it_came_from(tmp_path):
    root = source_tree(tmp_path)
    path = write_message(root, "demo_msgs/msg/Sample", "uint8 value\n")
    repository = msgs.MessageRepository(msg_paths=[root])

    assert repository.load("demo_msgs/msg/Sample").path == path


def test_an_unknown_type_is_an_error_naming_the_type():
    repository = msgs.MessageRepository()

    with pytest.raises(msgs.MessageError) as caught:
        repository.load("test_msgs/msg/Missing")

    assert str(caught.value) == (
        "test_msgs/msg/Missing: no message definition found; add its package to --msg-path or check the type name"
    )


def test_a_type_name_is_canonicalised_from_the_package_form(tmp_path):
    root = source_tree(tmp_path)
    write_message(root, "demo_msgs/msg/Sample", "uint8 value\n")
    repository = msgs.MessageRepository(msg_paths=[root])

    assert repository.load("demo_msgs/Sample").type_name == "demo_msgs/msg/Sample"


def test_a_sourced_distro_that_differs_warns():
    repository = msgs.MessageRepository(distro="jazzy", sourced_distro="humble")

    assert repository.warnings == (
        "the sourced ROS distro is 'humble' but 'jazzy' is configured; search paths may resolve different definitions",
    )


def test_a_matching_sourced_distro_does_not_warn():
    assert msgs.MessageRepository(distro="jazzy", sourced_distro="jazzy").warnings == ()


def test_comments_and_blank_lines_are_not_fields(tmp_path):
    root = source_tree(tmp_path)
    write_message(root, "demo_msgs/msg/Sample", "# a comment\n\nuint8 value  # trailing\n")
    repository = msgs.MessageRepository(msg_paths=[root])

    message = repository.load("demo_msgs/msg/Sample")

    assert [field.name for field in message.fields] == ["value"]


def test_a_constant_is_not_a_field(tmp_path):
    root = source_tree(tmp_path)
    write_message(root, "demo_msgs/msg/Sample", "int32 LIMIT=7\nuint8 value\n")
    repository = msgs.MessageRepository(msg_paths=[root])

    message = repository.load("demo_msgs/msg/Sample")

    assert [field.name for field in message.fields] == ["value"]
    assert [(constant.name, constant.value) for constant in message.constants] == [("LIMIT", "7")]


def test_a_default_value_stays_on_the_field(tmp_path):
    root = source_tree(tmp_path)
    write_message(root, "demo_msgs/msg/Sample", "float64 x 0.5\n")
    repository = msgs.MessageRepository(msg_paths=[root])

    assert repository.load("demo_msgs/msg/Sample").field("x").default == "0.5"


def test_a_same_package_reference_is_qualified(tmp_path):
    root = source_tree(tmp_path)
    write_message(root, "demo_msgs/msg/Header", "uint8 seq\n")
    write_message(root, "demo_msgs/msg/Sample", "Header header\n")
    repository = msgs.MessageRepository(msg_paths=[root])

    assert repository.load("demo_msgs/msg/Sample").field("header").type_name == "demo_msgs/msg/Header"


def test_a_package_reference_is_qualified(tmp_path):
    root = source_tree(tmp_path)
    write_message(root, "std_msgs/msg/Header", "uint8 seq\n")
    write_message(root, "demo_msgs/msg/Sample", "std_msgs/Header header\n")
    repository = msgs.MessageRepository(msg_paths=[root])

    assert repository.load("demo_msgs/msg/Sample").field("header").type_name == "std_msgs/msg/Header"


def test_an_unbounded_sequence_records_no_bound(tmp_path):
    root = source_tree(tmp_path)
    write_message(root, "demo_msgs/msg/Sample", "float64[] values\n")
    repository = msgs.MessageRepository(msg_paths=[root])

    field = repository.load("demo_msgs/msg/Sample").field("values")

    assert field.is_array
    assert field.array_length is None
    assert field.array_bound is None


def test_a_bounded_sequence_records_its_bound(tmp_path):
    root = source_tree(tmp_path)
    write_message(root, "demo_msgs/msg/Sample", "float64[<=4] values\n")
    repository = msgs.MessageRepository(msg_paths=[root])

    field = repository.load("demo_msgs/msg/Sample").field("values")

    assert field.is_array
    assert field.array_bound == 4


def test_a_fixed_array_records_its_length(tmp_path):
    root = source_tree(tmp_path)
    write_message(root, "demo_msgs/msg/Sample", "float64[9] values\n")
    repository = msgs.MessageRepository(msg_paths=[root])

    field = repository.load("demo_msgs/msg/Sample").field("values")

    assert field.is_array
    assert field.array_length == 9


def test_a_string_bound_is_recorded(tmp_path):
    root = source_tree(tmp_path)
    write_message(root, "demo_msgs/msg/Sample", "string<=12 name\n")
    repository = msgs.MessageRepository(msg_paths=[root])

    assert repository.load("demo_msgs/msg/Sample").field("name").string_bound == 12


def test_a_malformed_line_names_the_file_and_line(tmp_path):
    root = source_tree(tmp_path)
    path = write_message(root, "demo_msgs/msg/Sample", "float32\n")
    repository = msgs.MessageRepository(msg_paths=[root])

    with pytest.raises(msgs.MessageError) as caught:
        repository.load("demo_msgs/msg/Sample")

    assert str(caught.value) == f"{path}:1: expected a type and a field name; write it as 'float32 value'"


@pytest.mark.parametrize(
    ("declaration", "length"),
    [
        ("float64[0] values", "0"),
        ("float64[-1] values", "-1"),
        ("float64[<=0] values", "0"),
        ("string<=0 name", "0"),
    ],
)
def test_a_declared_length_below_one_is_an_error(tmp_path, declaration, length):
    root = source_tree(tmp_path)
    path = write_message(root, "demo_msgs/msg/Sample", declaration + "\n")
    repository = msgs.MessageRepository(msg_paths=[root])

    with pytest.raises(msgs.MessageError) as caught:
        repository.load("demo_msgs/msg/Sample")

    assert str(caught.value) == f"{path}:1: '{length}' is not a length; write a whole number of at least 1"


@pytest.mark.parametrize(("declaration", "name"), [("float32 bad-name", "bad-name"), ("float32 9bad", "9bad")])
def test_an_invalid_field_name_names_the_file_and_line(tmp_path, declaration, name):
    root = source_tree(tmp_path)
    path = write_message(root, "demo_msgs/msg/Sample", declaration + "\n")
    repository = msgs.MessageRepository(msg_paths=[root])

    with pytest.raises(msgs.MessageError) as caught:
        repository.load("demo_msgs/msg/Sample")

    assert str(caught.value) == (
        f"{path}:1: '{name}' is not a field name; "
        "use lowercase letters, digits and single underscores, starting with a letter"
    )
