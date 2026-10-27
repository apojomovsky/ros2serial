"""The reference message codec: the message vectors, every bundled type, and the rules it enforces."""

import random
import struct

import pytest
from r2s_gen import msgs
from r2s_gen.reference import messages
from r2s_gen.reference.errors import Rejected
from support import SEED, h, matches, random_message, vector_files, write_message

BUNDLED = (
    "builtin_interfaces/msg/Time",
    "geometry_msgs/msg/Quaternion",
    "geometry_msgs/msg/Twist",
    "geometry_msgs/msg/Vector3",
    "sensor_msgs/msg/Imu",
    "sensor_msgs/msg/JointState",
    "std_msgs/msg/Float32",
    "std_msgs/msg/Header",
)
DISTROS = ("jazzy", "lyrical", "rolling")


def repository() -> msgs.MessageRepository:
    return msgs.MessageRepository()


def demo(tmp_path, type_name: str, text: str) -> msgs.MessageRepository:
    write_message(tmp_path / "messages", type_name, text)
    return msgs.MessageRepository(msg_paths=[tmp_path / "messages"])


# covers: ENC-3, ENC-4
@pytest.mark.parametrize("vector", vector_files("messages"))
def test_the_reference_makes_every_message_vector(vector):
    narrow = vector["profile"]["narrow_float64"]
    payload = messages.encode(repository(), vector["type"], vector["message"], narrow=narrow)
    assert payload.hex(" ") == vector["payload"]


# covers: ENC-11
@pytest.mark.parametrize("vector", vector_files("messages"))
def test_the_reference_reads_every_message_vector(vector):
    narrow = vector["profile"]["narrow_float64"]
    decoded = messages.decode(repository(), vector["type"], h(vector["payload"]), narrow=narrow)
    assert matches(repository(), vector["type"], vector["message"], decoded, narrow=narrow), decoded


def bundled(root) -> set:
    return {f"{path.parent.parent.name}/msg/{path.stem}" for path in root.glob("*/msg/*.msg")}


def test_the_round_trip_covers_every_bundled_type():
    for distro in DISTROS:
        assert bundled(msgs.SNAPSHOTS / distro) == set(BUNDLED), distro


# covers: ENC-1, ENC-2, ENC-5, ENC-7, ENC-8, ENC-9, GEN-1
@pytest.mark.parametrize("narrow", [False, True])
@pytest.mark.parametrize("distro", DISTROS)
@pytest.mark.parametrize("type_name", BUNDLED)
def test_every_bundled_type_round_trips(distro, type_name, narrow):
    repo = msgs.MessageRepository(distro=distro)
    rng = random.Random(f"{SEED}-{distro}-{type_name}-{narrow}")
    for _ in range(4):
        message = random_message(repo, type_name, rng)

        payload = messages.encode(repo, type_name, message, narrow=narrow)
        decoded = messages.decode(repo, type_name, payload, narrow=narrow)

        assert matches(repo, type_name, message, decoded, narrow=narrow), (message, decoded)
        # A signalling NaN quiets on its way through binary64, so the wire
        # settles one pass later rather than on the first.
        settled = messages.encode(repo, type_name, decoded, narrow=narrow)
        resettled = messages.decode(repo, type_name, settled, narrow=narrow)
        assert messages.encode(repo, type_name, resettled, narrow=narrow) == settled, (message, decoded, settled)


# covers: GEN-1, ENC-2
def test_integers_travel_little_endian_in_twos_complement(tmp_path):
    repo = demo(tmp_path, "demo_msgs/msg/Integers", "int8 a\nint16 b\nint32 c\nint64 d\nuint64 e\nuint16 f")
    message = {"a": -1, "b": 0x1234, "c": -0x12345678, "d": -2, "e": 2**64 - 1, "f": 0xBEEF}

    payload = messages.encode(repo, "demo_msgs/msg/Integers", message)

    assert payload.hex(" ") == "ff 34 12 88 a9 cb ed fe ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ef be"
    assert messages.decode(repo, "demo_msgs/msg/Integers", payload) == message


# covers: ENC-5
def test_a_bool_travels_as_one_byte(tmp_path):
    repo = demo(tmp_path, "demo_msgs/msg/Flags", "bool set\nbool clear")

    payload = messages.encode(repo, "demo_msgs/msg/Flags", {"set": True, "clear": False})

    assert payload.hex(" ") == "01 00"
    assert messages.decode(repo, "demo_msgs/msg/Flags", payload) == {"set": True, "clear": False}


# covers: ENC-6
def test_a_decoder_drops_a_bool_byte_that_is_neither_zero_nor_one(tmp_path):
    repo = demo(tmp_path, "demo_msgs/msg/Flags", "bool set")

    with pytest.raises(Rejected) as caught:
        messages.decode(repo, "demo_msgs/msg/Flags", b"\x02")

    assert caught.value.rule == "ENC-6"


# covers: ENC-7
def test_a_string_is_a_varint_length_and_its_bytes_with_no_terminator(tmp_path):
    repo = demo(tmp_path, "demo_msgs/msg/Text", "string text")

    assert messages.encode(repo, "demo_msgs/msg/Text", {"text": "hello"}).hex(" ") == "05 68 65 6c 6c 6f"
    assert messages.encode(repo, "demo_msgs/msg/Text", {"text": ""}) == b"\x00"


# covers: ENC-7
def test_a_string_passes_its_bytes_through_whatever_their_encoding(tmp_path):
    repo = demo(tmp_path, "demo_msgs/msg/Text", "string text")

    payload = messages.encode(repo, "demo_msgs/msg/Text", {"text": b"\xff\xfe"})

    assert payload.hex(" ") == "02 ff fe"
    assert messages.decode(repo, "demo_msgs/msg/Text", payload)["text"] == b"\xff\xfe"


# covers: ENC-14
def test_a_string_holds_no_zero_byte(tmp_path):
    repo = demo(tmp_path, "demo_msgs/msg/Text", "string text")

    with pytest.raises(Rejected) as caught:
        messages.encode(repo, "demo_msgs/msg/Text", {"text": "a\x00b"})
    assert caught.value.rule == "ENC-14"

    with pytest.raises(Rejected) as caught:
        messages.decode(repo, "demo_msgs/msg/Text", h("03 61 00 62"))
    assert caught.value.rule == "ENC-14"


# covers: ENC-8
def test_a_fixed_array_is_its_elements_and_no_length(tmp_path):
    repo = demo(tmp_path, "demo_msgs/msg/Triple", "int8[3] three")

    payload = messages.encode(repo, "demo_msgs/msg/Triple", {"three": [1, 2, 3]})

    assert payload.hex(" ") == "01 02 03"
    assert messages.decode(repo, "demo_msgs/msg/Triple", payload) == {"three": [1, 2, 3]}

    with pytest.raises(ValueError, match="3 elements"):
        messages.encode(repo, "demo_msgs/msg/Triple", {"three": [1, 2]})


# covers: ENC-9
def test_a_sequence_is_a_varint_count_and_its_elements(tmp_path):
    repo = demo(tmp_path, "demo_msgs/msg/Items", "uint8[] items")

    assert messages.encode(repo, "demo_msgs/msg/Items", {"items": []}) == b"\x00"
    counted = messages.encode(repo, "demo_msgs/msg/Items", {"items": [7] * 130})
    assert counted[:2] == h("82 01")
    assert messages.decode(repo, "demo_msgs/msg/Items", counted) == {"items": [7] * 130}


# covers: ENC-13
def test_the_same_message_encodes_identically_under_different_bounds(tmp_path):
    tight = demo(tmp_path / "tight", "demo_msgs/msg/Entry", "string<=4 name\nint16[<=4] values")
    loose = demo(tmp_path / "loose", "demo_msgs/msg/Entry", "string<=16 name\nint16[<=16] values")
    message = {"name": "ab", "values": [1, 2, 3]}

    assert messages.encode(tight, "demo_msgs/msg/Entry", message) == h("02 61 62 03 01 00 02 00 03 00")
    assert messages.encode(loose, "demo_msgs/msg/Entry", message) == messages.encode(
        tight, "demo_msgs/msg/Entry", message
    )


# covers: ENC-10
def test_the_narrow_flag_changes_float64_fields_and_no_other(tmp_path):
    repo = demo(tmp_path, "demo_msgs/msg/Mixed", "float32 single\nfloat64 wide")
    message = {"single": 0.5, "wide": 0.5}

    wide = messages.encode(repo, "demo_msgs/msg/Mixed", message)
    narrow = messages.encode(repo, "demo_msgs/msg/Mixed", message, narrow=True)

    assert wide[:4] == narrow[:4] == h("00 00 00 3f")
    assert wide[4:] == h("00 00 00 00 00 00 e0 3f")
    assert narrow[4:] == h("00 00 00 3f")


# covers: ENC-11
def test_a_narrow_float64_decodes_to_the_binary32_widened_exactly(tmp_path):
    repo = demo(tmp_path, "demo_msgs/msg/Mixed", "float64 wide")
    narrow = messages.encode(repo, "demo_msgs/msg/Mixed", {"wide": 0.1}, narrow=True)

    assert narrow.hex(" ") == "cd cc cc 3d"
    assert (
        messages.decode(repo, "demo_msgs/msg/Mixed", narrow, narrow=True)["wide"]
        == struct.unpack("<f", h("cd cc cc 3d"))[0]
    )


# covers: ENC-17
def test_a_payload_that_ends_before_its_last_field_is_dropped(tmp_path):
    repo = demo(tmp_path, "demo_msgs/msg/Pair", "bool first\nuint16 second")

    with pytest.raises(Rejected) as caught:
        messages.decode(repo, "demo_msgs/msg/Pair", b"\x01\x02")
    assert caught.value.rule == "ENC-17"


# covers: ENC-18
def test_a_payload_with_bytes_after_its_last_field_is_dropped(tmp_path):
    repo = demo(tmp_path, "demo_msgs/msg/Flags", "bool set")

    with pytest.raises(Rejected) as caught:
        messages.decode(repo, "demo_msgs/msg/Flags", b"\x01\x00")
    assert caught.value.rule == "ENC-18"


# covers: VAR-3, VAR-4
def test_a_length_that_breaks_a_varint_rule_names_it(tmp_path):
    repo = demo(tmp_path, "demo_msgs/msg/Text", "string text")

    with pytest.raises(Rejected) as caught:
        messages.decode(repo, "demo_msgs/msg/Text", b"")
    assert caught.value.rule == "VAR-3"

    with pytest.raises(Rejected) as caught:
        messages.decode(repo, "demo_msgs/msg/Text", h("80 00"))
    assert caught.value.rule == "VAR-4"


# covers: ENC-19
def test_a_wide_type_has_no_wire_encoding(tmp_path):
    repo = demo(tmp_path, "demo_msgs/msg/Wide", "wstring text")

    with pytest.raises(Rejected) as caught:
        messages.encode(repo, "demo_msgs/msg/Wide", {"text": "wide"})
    assert caught.value.rule == "ENC-19"

    with pytest.raises(Rejected) as caught:
        messages.decode(repo, "demo_msgs/msg/Wide", b"")
    assert caught.value.rule == "ENC-19"


def test_a_value_the_field_cannot_hold_is_an_error_naming_the_field(tmp_path):
    repo = demo(tmp_path, "demo_msgs/msg/Small", "uint8 value")

    with pytest.raises(ValueError, match="demo_msgs/msg/Small.value: 256"):
        messages.encode(repo, "demo_msgs/msg/Small", {"value": 256})

    with pytest.raises(ValueError, match="demo_msgs/msg/Small.value: no value"):
        messages.encode(repo, "demo_msgs/msg/Small", {})


def test_a_number_too_large_for_binary_floating_point_is_a_value_error(tmp_path):
    repo = demo(tmp_path, "demo_msgs/msg/Floats", "float32 single\nfloat64 wide")

    with pytest.raises(ValueError, match="demo_msgs/msg/Floats.single"):
        messages.encode(repo, "demo_msgs/msg/Floats", {"single": 1e300, "wide": 0.0})

    with pytest.raises(ValueError, match="demo_msgs/msg/Floats.wide"):
        messages.encode(repo, "demo_msgs/msg/Floats", {"single": 0.0, "wide": 10**400})
