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
    assert sizes['flash'] == 8200
    assert sizes['ram'] == 800


def test_avr_size_rejects_garbage():
    with pytest.raises(size_report.SizeError):
        size_report.parse_avr_size('not an avr-size table\n')


def test_nm_sizes_maps_symbols_to_byte_counts():
    assert size_report.parse_nm_sizes(NM_SIZES) == {
        '_etext': 32,
        'r2s_rx_buf': 4,
        'r2s_runtime_topics': 224,
    }


def test_check_passes_inside_budget():
    measurements = {'uno': {'flash': 8200, 'ram': 800}}
    budgets = {'firmware': {'uno': {'flash': 9000, 'ram': 1000}}}
    assert size_report.check_sizes(measurements, budgets) == []


def test_check_names_each_breach():
    measurements = {'uno': {'flash': 9500, 'ram': 800}}
    budgets = {'firmware': {'uno': {'flash': 9000, 'ram': 1000}}}
    problems = size_report.check_sizes(measurements, budgets)
    assert len(problems) == 1
    assert 'uno' in problems[0] and 'flash' in problems[0]


def test_check_ignores_firmware_without_a_budget():
    measurements = {'new_board': {'flash': 10**9, 'ram': 10**9}}
    assert size_report.check_sizes(measurements, {'firmware': {}}) == []


def test_check_fails_on_a_breached_fixture():
    measurements = {'uno': {'flash': 8200, 'ram': 800}}
    budgets = {'firmware': {'uno': {'flash': 1, 'ram': 1}}}
    assert len(size_report.check_sizes(measurements, budgets)) == 2


def elf_bytes(machine):
    header = bytearray(64)
    header[0:4] = b'\x7fELF'
    header[4] = 1
    header[5] = 1
    header[18:20] = machine.to_bytes(2, 'little')
    return bytes(header)


def test_elf_machine_reads_an_xtensa_header(tmp_path):
    firmware = tmp_path / 'esp32.elf'
    firmware.write_bytes(elf_bytes(0x5E))
    assert size_report.elf_machine(firmware) == 0x5E


def test_elf_machine_reads_an_avr_header(tmp_path):
    firmware = tmp_path / 'uno.elf'
    firmware.write_bytes(elf_bytes(83))
    assert size_report.elf_machine(firmware) == 83


def test_elf_machine_is_none_when_unreadable(tmp_path):
    assert size_report.elf_machine(tmp_path / 'missing.elf') is None
    empty = tmp_path / 'empty.elf'
    empty.write_bytes(b'')
    assert size_report.elf_machine(empty) is None


def fake_run(calls):
    def run(tool, *args):
        calls.append(tool)
        if '--print-size' in args:
            return NM_SIZES
        return AVR_SIZE

    return run


def empty_data_dir(tmp_path, monkeypatch):
    data = tmp_path / 'arduino-data'
    data.mkdir()
    monkeypatch.setenv('R2S_ARDUINO_DATA_DIR', str(data))
    return data


def test_measure_uses_the_xtensa_tools_for_xtensa_firmware(tmp_path, monkeypatch):
    empty_data_dir(tmp_path, monkeypatch)
    firmware = tmp_path / 'arduino_minimal' / 'esp32' / 'arduino_minimal.ino.elf'
    firmware.parent.mkdir(parents=True)
    firmware.write_bytes(elf_bytes(0x5E))
    calls = []
    monkeypatch.setattr(size_report, '_run', fake_run(calls))
    sizes = size_report.measure(tmp_path)['arduino_minimal/esp32/arduino_minimal.ino.elf']
    assert 'xtensa-esp32-elf-size' in calls
    assert 'avr-size' not in calls
    assert sizes['flash'] == 8200


def test_measure_keeps_the_avr_tools_for_avr_firmware(tmp_path, monkeypatch):
    empty_data_dir(tmp_path, monkeypatch)
    firmware = tmp_path / 'arduino_minimal' / 'uno' / 'arduino_minimal.ino.elf'
    firmware.parent.mkdir(parents=True)
    firmware.write_bytes(elf_bytes(83))
    calls = []
    monkeypatch.setattr(size_report, '_run', fake_run(calls))
    size_report.measure(tmp_path)
    assert 'avr-size' in calls
    assert 'xtensa-esp32-elf-size' not in calls


def test_measure_keeps_the_avr_tools_when_the_machine_is_unknown(tmp_path, monkeypatch):
    empty_data_dir(tmp_path, monkeypatch)
    firmware = tmp_path / 'stubbed' / 'uno' / 'stubbed.ino.elf'
    firmware.parent.mkdir(parents=True)
    firmware.write_bytes(b'')
    calls = []
    monkeypatch.setattr(size_report, '_run', fake_run(calls))
    size_report.measure(tmp_path)
    assert 'avr-size' in calls
    assert 'xtensa-esp32-elf-size' not in calls


def test_xtensa_tools_resolve_into_the_installed_core(tmp_path, monkeypatch):
    data = empty_data_dir(tmp_path, monkeypatch)
    tool = (
        data
        / 'packages'
        / 'esp32'
        / 'tools'
        / 'esp-x32'
        / '2601'
        / 'bin'
        / 'xtensa-esp32-elf-size'
    )
    tool.parent.mkdir(parents=True)
    tool.write_bytes(b'')
    assert size_report.xtensa_size() == str(tool)


def test_xtensa_tools_fall_back_to_the_bare_name(tmp_path, monkeypatch):
    empty_data_dir(tmp_path, monkeypatch)
    assert size_report.xtensa_size() == 'xtensa-esp32-elf-size'
    assert size_report.xtensa_nm() == 'xtensa-esp32-elf-nm'


def test_xtensa_tool_overrides_win(tmp_path, monkeypatch):
    empty_data_dir(tmp_path, monkeypatch)
    monkeypatch.setenv('R2S_XTENSA_SIZE', 'custom-size')
    monkeypatch.setenv('R2S_XTENSA_NM', 'custom-nm')
    assert size_report.xtensa_size() == 'custom-size'
    assert size_report.xtensa_nm() == 'custom-nm'
