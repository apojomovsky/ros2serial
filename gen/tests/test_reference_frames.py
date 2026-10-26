"""The reference frame codec: the frame vectors, the zero residue and the rules that drop a unit."""

import random

import pytest
from r2s_gen.reference import frames, rcobs
from r2s_gen.reference.errors import Rejected
from support import SEED, h, vector_files


# covers: FRM-1, FRM-2, CHK-3
@pytest.mark.parametrize("vector", vector_files("frames"))
def test_the_reference_makes_every_frame_vector(vector):
    payload = h(vector["payload"])

    assert frames.encode(vector["topic_id"], vector["seq"], payload).hex(" ") == vector["frame"]
    assert frames.encode_wire(vector["topic_id"], vector["seq"], payload).hex(" ") == vector["wire"]


# covers: FRM-2, FRM-8
@pytest.mark.parametrize("vector", vector_files("frames"))
def test_the_reference_reads_every_frame_vector(vector):
    assert frames.decode(h(vector["wire"])) == (vector["topic_id"], vector["seq"], h(vector["payload"]))


# covers: CHK-3, CHK-4, FRM-8, GEN-1
def test_every_frame_the_reference_makes_passes_its_own_check():
    rng = random.Random(SEED)

    for _ in range(500):
        topic_id = rng.randrange(0, 256)
        seq = rng.randrange(0, 256)
        payload = rng.randbytes(rng.randrange(0, 300))
        assert frames.decode(frames.encode_wire(topic_id, seq, payload)) == (topic_id, seq, payload)


# covers: FRM-6
def test_a_unit_the_rcobs_decoder_refuses_is_dropped_by_its_rule():
    with pytest.raises(Rejected) as caught:
        frames.decode(h("ff 00"))  # the last code is 0xFF, which an encoder never makes
    assert caught.value.rule == "COB-6"

    with pytest.raises(Rejected) as caught:
        frames.decode(h("05 00"))  # a code 5 with four bytes short
    assert caught.value.rule == "COB-5"


# covers: FRM-7
@pytest.mark.parametrize("frame", [b"", b"\x10", b"\x10\x00"])
def test_a_decoded_frame_shorter_than_four_bytes_is_dropped(frame):
    with pytest.raises(Rejected) as caught:
        frames.decode(rcobs.encode(frame) + b"\x00")

    assert caught.value.rule == "FRM-7"


# covers: FRM-8
def test_a_frame_that_fails_the_check_is_dropped():
    frame = bytearray(frames.encode(16, 0, h("00 00 c0 3f")))
    frame[2] ^= 0x01
    wire = rcobs.encode(bytes(frame)) + b"\x00"

    with pytest.raises(Rejected) as caught:
        frames.decode(bytes(wire))

    assert caught.value.rule == "FRM-8"


# covers: FRM-1
def test_a_unit_without_its_delimiter_is_rejected():
    with pytest.raises(Rejected) as caught:
        frames.decode(frames.encode_wire(16, 0, h("00 00 c0 3f"))[:-1])

    assert caught.value.rule == "FRM-1"


# covers: FRM-14
def test_a_unit_that_is_only_a_delimiter_carries_no_frame():
    assert frames.decode(b"\x00") is None
