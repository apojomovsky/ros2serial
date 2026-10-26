"""RIHS01 type hashes: the generator's hash equals the installed type support's.

The expected values are the hashes REP-2011 defines, read from the type
description JSON files the ROS build installs for these types.
"""

import pytest
from r2s_gen import msgs, typehash
from support import write_message

DISTROS = ["jazzy", "lyrical", "rolling"]

CASES = [
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
