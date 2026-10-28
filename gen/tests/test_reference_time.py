"""The Python reference for the MCU time arithmetic: hand vectors, boundaries, rejections."""

import pytest
from r2s_gen.reference import time
from r2s_gen.reference.errors import Rejected
from support import vector_files


def expected_of(case):
    expect = case["expect"]
    return None if expect == "unsynced" else (expect["sec"], expect["nanosec"])


# The S7 hand vectors: the reference reproduces every case.
@pytest.mark.parametrize("vector", vector_files("time"))
def test_reference_reproduces_every_time_vector(vector):
    for case in vector["cases"]:
        assert time.compute(vector["tick_hz"], case) == expected_of(case), case


def run(**fields):
    case = {"tick0": 0, "sec0": 0, "nanosec0": 0, "drift_ppm": 0, "tick": 0, **fields}
    return time.compute(1000, case)


# covers: TIM-7
def test_a_correction_older_than_the_maximum_anchor_is_unsynced():
    assert run(tick=10000) == (10, 0)
    assert run(tick=10001) is None


# covers: TIM-6
def test_a_tick_wraparound_is_an_unsigned_difference():
    assert run(tick0=0xFFFFFFFF, tick=1000) == (1, 1000000)


# covers: TIM-4
def test_drift_at_both_ends_of_the_allowed_range_applies():
    assert run(tick=1000, drift_ppm=20000) == (1, 20000000)
    assert run(sec0=1, tick=1000, drift_ppm=-20000) == (1, 980000000)


def test_a_negative_drift_that_crosses_a_second_borrows():
    assert run(sec0=0, tick=1000, drift_ppm=-20000) == (0, 980000000)


def test_the_largest_drift_normalizes_before_and_after_it_applies():
    case = {"tick0": 0, "sec0": 0, "nanosec0": 999999999, "drift_ppm": 20000, "tick": 1999000}
    assert time.compute(1000000, case) == (3, 38979999)


def test_rejections_name_the_rule_that_rejects_them():
    with pytest.raises(Rejected) as tick_hz:
        time.compute(1500, {"tick0": 0, "sec0": 0, "nanosec0": 0, "drift_ppm": 0, "tick": 0})
    assert tick_hz.value.rule == "CTL-9"
    with pytest.raises(Rejected) as drift:
        run(drift_ppm=20001)
    assert drift.value.rule == "TIM-5"
    with pytest.raises(Rejected) as nanosec:
        run(nanosec0=1000000000)
    assert nanosec.value.rule == "TIM-11"
