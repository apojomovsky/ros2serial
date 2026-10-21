"""The Python reference for CRC, rCOBS and varint: published values, properties, rejections, vectors."""

import random
from pathlib import Path

import pytest
import yaml
from r2s_gen.reference import crc, rcobs, varint
from r2s_gen.reference.errors import Rejected

VECTORS = Path(__file__).resolve().parents[2] / "spec" / "vectors"
SEED = 20261021


def cases(kind: str):
    for path in sorted((VECTORS / kind).glob("*.yaml")):
        doc = yaml.safe_load(path.read_text())
        for i, case in enumerate(doc["cases"]):
            yield pytest.param(case, id=f"{path.stem}-{i}")


def h(text: str) -> bytes:
    return bytes.fromhex(text)


# covers: CHK-2
def test_crc_of_the_check_string_is_the_published_value():
    assert crc.crc16(b"123456789") == 0x6F91


# covers: CHK-3, CHK-4
def test_crc_appended_little_endian_leaves_a_zero_residue():
    rng = random.Random(SEED)
    for _ in range(2000):
        data = rng.randbytes(rng.randrange(0, 64))
        assert crc.crc16(data + crc.crc16(data).to_bytes(2, "little")) == 0


# covers: CHK-1
@pytest.mark.parametrize("case", cases("crc"))
def test_crc_reproduces_every_vector(case):
    data = case["text"].encode("ascii") if "text" in case else h(case["data"])
    assert crc.crc16(data) == case["crc"]


# covers: COB-1, COB-4, COB-7
def test_rcobs_round_trips_random_input():
    rng = random.Random(SEED)
    for _ in range(2000):
        size = rng.choice([0, 1, 2, 253, 254, 255, 508, 509, rng.randrange(0, 1200)])
        data = bytes(rng.choice([0, rng.randrange(1, 256)]) for _ in range(size))
        assert rcobs.decode(rcobs.encode(data)) == data


# covers: COB-2, COB-8
def test_rcobs_output_has_no_zero_and_stays_within_the_bound():
    rng = random.Random(SEED)
    for _ in range(2000):
        data = rng.randbytes(rng.randrange(0, 1200))
        out = rcobs.encode(data)
        assert 0 not in out
        assert len(out) <= len(data) + len(data) // 254 + 1


# covers: COB-8
def test_rcobs_bound_is_reached_without_zeros():
    for n in (0, 1, 253, 254, 255, 508, 1000):
        assert len(rcobs.encode(b"\x01" * n)) == n + n // 254 + 1


# covers: COB-3
def test_rcobs_254_nonzero_bytes_at_the_end_give_a_trailing_empty_group():
    assert rcobs.encode(b"\x11" * 254)[-2:] == b"\xff\x01"


@pytest.mark.parametrize("case", cases("rcobs"))
def test_rcobs_reproduces_every_vector(case):
    wire = h(case["encoded"])
    assert wire[-1:] == b"\x00", "rcobs vectors include the delimiter"
    if "reject" in case:
        with pytest.raises(Rejected) as e:
            rcobs.decode(wire[:-1])
        assert e.value.rule == case["reject"]
    else:
        assert rcobs.encode(h(case["decoded"])) + b"\x00" == wire
        assert rcobs.decode(wire[:-1]) == h(case["decoded"])


# covers: COB-5
def test_rcobs_rejects_a_zero_code():
    with pytest.raises(Rejected) as e:
        rcobs.decode(b"\x11\x00\x02")
    assert e.value.rule == "COB-5"


# covers: VAR-1, VAR-4
def test_varint_round_trips_every_length():
    for n in range(0, 65536):
        out = varint.encode(n)
        assert varint.decode(out) == (n, len(out))
        assert len(out) == (1 if n < 128 else 2 if n < 16384 else 3)


# covers: VAR-2
def test_varint_encoder_refuses_lengths_over_65535():
    with pytest.raises(ValueError):
        varint.encode(65536)


# covers: VAR-1
def test_varint_decode_reports_the_bytes_it_used():
    assert varint.decode(h("ac 02 ff")) == (300, 2)


@pytest.mark.parametrize("case", cases("varint"))
def test_varint_reproduces_every_vector(case):
    wire = h(case["encoded"])
    if "reject" in case:
        with pytest.raises(Rejected) as e:
            varint.decode(wire)
        assert e.value.rule == case["reject"]
    else:
        assert varint.encode(case["value"]) == wire
        assert varint.decode(wire) == (case["value"], len(wire))


def test_every_kind_has_vectors():
    for kind in ("crc", "rcobs", "varint"):
        assert list((VECTORS / kind).glob("*.yaml")), kind
