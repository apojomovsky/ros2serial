"""RIHS01 type hashes: the generator's hash equals the installed type support's.

The expected values are the hashes REP-2011 defines, read from the type
description JSON files the ROS build installs for these types.
"""

import pytest
from r2s_gen import msgs, typehash
from support import write_message

DISTROS = ["jazzy", "lyrical", "rolling"]

CASES = [
    (
        "geometry_msgs/msg/Quaternion",
        "RIHS01_8a765f66778c8ff7c8ab94afcc590a2ed5325a1d9a076ffff38fbce36f458684",
    ),
    ("std_msgs/msg/Float32", "RIHS01_7170d3d8f841f7be3172ce5f4f59f3a4d7f63b0447e8b33327601ad64d83d6e2"),
    (
        "builtin_interfaces/msg/Time",
        "RIHS01_b106235e25a4c5ed35098aa0a61a3ee9c9b18d197f398b0e4206cea9acf9c197",
    ),
    (
        "std_msgs/msg/Header",
        "RIHS01_f49fb3ae2cf070f793645ff749683ac6b06203e41c891e17701b1cb597ce6a01",
    ),
    (
        "geometry_msgs/msg/Vector3",
        "RIHS01_cc12fe83e4c02719f1ce8070bfd14aecd40f75a96696a67a2a1f37f7dbb0765d",
    ),
    (
        "geometry_msgs/msg/Twist",
        "RIHS01_9c45bf16fe0983d80e3cfe750d6835843d265a9a6c46bd2e609fcddde6fb8d2a",
    ),
    (
        "sensor_msgs/msg/Imu",
        "RIHS01_7d9a00ff131080897a5ec7e26e315954b8eae3353c3f995c55faf71574000b5b",
    ),
    (
        "sensor_msgs/msg/JointState",
        "RIHS01_a13ee3a330e346c9d87b5aa18d24e11690752bd33a0350f11c5882bc9179260e",
    ),
    (
        "std_msgs/msg/String",
        "RIHS01_df668c740482bbd48fb39d76a70dfd4bd59db1288021743503259e948f6b1a18",
    ),
]


@pytest.mark.parametrize("distro", DISTROS)
@pytest.mark.parametrize("type_name,expected", CASES)
def test_type_hash_matches_the_published_check_value(distro, type_name, expected):
    repository = msgs.MessageRepository(distro=distro)

    assert typehash.type_hash(repository, type_name) == expected


def test_a_wide_string_field_is_an_error_naming_the_field(tmp_path):
    root = tmp_path / "messages"
    write_message(root, "demo_msgs/msg/Sample", "wstring name\n")
    repository = msgs.MessageRepository(msg_paths=[root])

    with pytest.raises(msgs.MessageError) as caught:
        typehash.type_hash(repository, "demo_msgs/msg/Sample")

    assert "name" in str(caught.value)


def test_an_empty_message_hashes_with_the_placeholder_member(tmp_path):
    write_message(tmp_path / "messages", "std_msgs/msg/Empty", "")
    repository = msgs.MessageRepository(msg_paths=[tmp_path / "messages"])

    assert (
        typehash.type_hash(repository, "std_msgs/msg/Empty")
        == "RIHS01_20b625256f32d5dbc0d04fee44f43c41e51c70d3502f84b4a08e7a9c26a96312"
    )


def test_bounded_fields_hash_with_bounded_type_ids(tmp_path):
    write_message(tmp_path / "messages", "demo_msgs/msg/Bounded", "string<=7 name\nint32[<=4] values\n")
    repository = msgs.MessageRepository(msg_paths=[tmp_path / "messages"])

    assert (
        typehash.type_hash(repository, "demo_msgs/msg/Bounded")
        == "RIHS01_095b6afd2bc2ff082798196072602098e624d26812b92c459ef21279db79b097"
    )


def test_a_parent_of_an_empty_message_matches_the_oracle(tmp_path):
    write_message(tmp_path / "messages", "std_msgs/msg/Empty", "")
    write_message(tmp_path / "messages", "demo_msgs/msg/HasEmpty", "std_msgs/Empty e\n")
    repository = msgs.MessageRepository(msg_paths=[tmp_path / "messages"])

    assert (
        typehash.type_hash(repository, "demo_msgs/msg/HasEmpty")
        == "RIHS01_b226f514d364b3d9c9e46d48a9a445044e450b4e87ccd5915408f9f72a174dc8"
    )
