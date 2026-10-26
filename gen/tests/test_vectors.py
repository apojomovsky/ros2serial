"""The `vectors` subcommand: payload hex from the reference codec, reproducing the hand vectors."""

import re

import pytest
import yaml
from r2s_gen import cli
from support import vector_files, write_message

HEX = re.compile(r"^$|^[0-9a-f]{2}( [0-9a-f]{2})*$")


def run(capsys, *argv):
    code = cli.main(["vectors", *argv])
    out, err = capsys.readouterr()
    return code, out, err


def message_text(vector) -> str:
    return yaml.safe_dump(vector["message"])


# The S4a hand vectors: the subcommand reproduces the payload of each one.
@pytest.mark.parametrize("vector", vector_files("messages"))
def test_vectors_reproduces_every_hand_message_vector(capsys, vector):
    extra = ["--narrow"] if vector["profile"]["narrow_float64"] else []
    code, out, err = run(capsys, "--type", vector["type"], "--message", message_text(vector), *extra)
    assert code == 0, err
    assert out == vector["payload"] + "\n"
    assert HEX.fullmatch(out.strip())


def test_vectors_reads_the_message_from_a_file(capsys, tmp_path):
    path = tmp_path / "message.yaml"
    path.write_text("{data: 1.5}\n")
    code, out, err = run(capsys, "--type", "std_msgs/msg/Float32", "--message", f"@{path}")
    assert code == 0, err
    assert out == "00 00 c0 3f\n"


def test_vectors_resolves_a_custom_type_through_msg_path(capsys, tmp_path):
    write_message(tmp_path / "messages", "demo_msgs/msg/Small", "uint8 value")
    code, out, err = run(
        capsys,
        "--type",
        "demo_msgs/msg/Small",
        "--message",
        "{value: 7}",
        "--msg-path",
        str(tmp_path / "messages"),
    )
    assert code == 0, err
    assert out == "07\n"


def test_narrow_encodes_float64_as_binary32(capsys, tmp_path):
    write_message(tmp_path / "messages", "demo_msgs/msg/Wide", "float64 wide")
    args = ["--type", "demo_msgs/msg/Wide", "--message", "{wide: 0.1}", "--msg-path", str(tmp_path / "messages")]
    code, out, err = run(capsys, *args)
    assert code == 0, err
    assert out == "9a 99 99 99 99 99 b9 3f\n"
    code, out, err = run(capsys, *args, "--narrow")
    assert code == 0, err
    assert out == "cd cc cc 3d\n"


def test_an_unknown_type_names_itself_and_suggests_msg_path(capsys):
    code, out, err = run(capsys, "--type", "demo_msgs/msg/Foo", "--message", "{value: 1}")
    assert code == 1
    assert out == ""
    assert err == (
        "r2s-gen: error: demo_msgs/msg/Foo: no message definition found; "
        "add its package to --msg-path or check the type name\n"
    )


def test_a_type_without_a_package_names_the_expected_shapes(capsys):
    code, out, err = run(capsys, "--type", "Float32", "--message", "{data: 1.5}")
    assert code == 1
    assert out == ""
    assert err == (
        "r2s-gen: error: Float32: expected a type as 'package/Type' or 'package/msg/Type'; check the type name\n"
    )


def test_an_invalid_yaml_message_says_so_and_how_to_fix_it(capsys):
    code, out, err = run(capsys, "--type", "std_msgs/msg/Float32", "--message", "{data: [unclosed")
    assert code == 1
    assert out == ""
    assert err == (
        "r2s-gen: error: --message: not valid YAML; "
        "write the message as a YAML mapping, or @path to read it from a file\n"
    )


def test_a_non_mapping_message_names_the_expected_shape(capsys):
    code, out, err = run(capsys, "--type", "std_msgs/msg/Float32", "--message", "1.5")
    assert code == 1
    assert out == ""
    assert err == (
        "r2s-gen: error: --message: expected a mapping of field names to values; "
        "write the message as a YAML mapping, or @path to read it from a file\n"
    )


def test_a_missing_field_names_the_field(capsys):
    code, out, err = run(capsys, "--type", "std_msgs/msg/Float32", "--message", "{}")
    assert code == 1
    assert out == ""
    assert err == "r2s-gen: error: std_msgs/msg/Float32.data: no value\n"


def test_a_value_the_field_cannot_hold_names_the_field(capsys, tmp_path):
    write_message(tmp_path / "messages", "demo_msgs/msg/Small", "uint8 value")
    code, out, err = run(
        capsys,
        "--type",
        "demo_msgs/msg/Small",
        "--message",
        "{value: 256}",
        "--msg-path",
        str(tmp_path / "messages"),
    )
    assert code == 1
    assert out == ""
    assert err == "r2s-gen: error: demo_msgs/msg/Small.value: 256 does not fit 8 bits\n"


def test_a_missing_message_file_names_the_path(capsys, tmp_path):
    missing = tmp_path / "missing.yaml"
    code, out, err = run(capsys, "--type", "std_msgs/msg/Float32", "--message", f"@{missing}")
    assert code == 1
    assert out == ""
    assert err == (
        f"r2s-gen: error: {missing}: cannot read the message file "
        f"([Errno 2] No such file or directory: '{missing}'); check the path\n"
    )
