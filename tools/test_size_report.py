"""Unit tests for tools/size_report.py: avr-size parsing and budget checks."""

import pytest
import size_report

AVR_SIZE = """\
   text\t   data\t    bss\t    dec\t    hex\tfilename
   8000\t    200\t    600\t   8800\t   2260\tfirmware.elf
"""

NM_SIZES = """\
00800100 00000020 t _etext
008001eb 00000004 b r2s_rx_buf
00800460 000000e0 b r2s_runtime_topics
"""


def test_flash_is_text_plus_data_and_ram_is_data_plus_bss():
    sizes = size_report.parse_avr_size(AVR_SIZE)
    assert sizes["flash"] == 8200
    assert sizes["ram"] == 800


def test_avr_size_rejects_garbage():
    with pytest.raises(size_report.SizeError):
        size_report.parse_avr_size("not an avr-size table\n")


def test_nm_sizes_maps_symbols_to_byte_counts():
    assert size_report.parse_nm_sizes(NM_SIZES) == {"_etext": 32, "r2s_rx_buf": 4, "r2s_runtime_topics": 224}


def test_check_passes_inside_budget():
    measurements = {"uno": {"flash": 8200, "ram": 800}}
    budgets = {"firmware": {"uno": {"flash": 9000, "ram": 1000}}}
    assert size_report.check_sizes(measurements, budgets) == []


def test_check_names_each_breach():
    measurements = {"uno": {"flash": 9500, "ram": 800}}
    budgets = {"firmware": {"uno": {"flash": 9000, "ram": 1000}}}
    problems = size_report.check_sizes(measurements, budgets)
    assert len(problems) == 1
    assert "uno" in problems[0] and "flash" in problems[0]


def test_check_ignores_firmware_without_a_budget():
    measurements = {"new_board": {"flash": 10**9, "ram": 10**9}}
    assert size_report.check_sizes(measurements, {"firmware": {}}) == []


def test_check_fails_on_a_breached_fixture():
    measurements = {"uno": {"flash": 8200, "ram": 800}}
    budgets = {"firmware": {"uno": {"flash": 1, "ram": 1}}}
    assert len(size_report.check_sizes(measurements, budgets)) == 2
