# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Unit tests for the hardware check's verdict, with no ROS and no board."""

import io
from pathlib import Path

import hardware_check
import pytest

# The sketch republishes every 100 ms, so ten seconds of it is 101 stamps.
TEN_HZ = [index / 10.0 for index in range(101)]


def clean_values(**overrides):
    """A diagnostics status as a healthy link reports it, key over value."""
    values = {
        'mcu crc errors': '0',
        'mcu rx overflows': '0',
        'mcu dropped frames': '0',
        'topic 16 button seq gaps': '0',
        'topic 17 led seq gaps': '0',
        'topic 16 button dropped': '0',
        'topic 17 led dropped': '0',
        'topic 16 button publish failures': '0',
    }
    values.update(overrides)
    return values


def test_arrivals_read_the_rate_and_the_longest_gap():
    sample = hardware_check.arrivals(TEN_HZ)
    assert sample.count == 101
    assert sample.hz == pytest.approx(10.0)
    assert sample.gap_ms == pytest.approx(100.0)


def test_arrivals_name_the_longest_gap_that_missed_a_publish():
    sample = hardware_check.arrivals([0.0, 0.1, 0.2, 0.65, 0.75])
    assert sample.gap_ms == pytest.approx(450.0)


def test_arrivals_refuse_a_single_message():
    with pytest.raises(hardware_check.CheckError) as error:
        hardware_check.arrivals([1.0])
    assert '1 /button message' in str(error.value)


def test_counters_sum_the_per_topic_gaps_and_drops():
    link = hardware_check.counters(
        clean_values(**{'topic 16 button seq gaps': '3', 'topic 17 led dropped': '2'})
    )
    assert link.seq_gaps == 3
    assert link.topic_drops == 2
    assert link.crc_errors == 0


def test_counters_ignore_the_keys_that_are_not_losses():
    link = hardware_check.counters(clean_values(**{'topic 16 button publish failures': '4'}))
    assert link == hardware_check.Counters()


def test_counters_refuse_a_missing_mcu_counter():
    values = clean_values()
    del values['mcu crc errors']
    with pytest.raises(hardware_check.CheckError) as error:
        hardware_check.counters(values)
    assert 'mcu crc errors' in str(error.value)


def test_a_clean_run_has_no_failures():
    assert (
        hardware_check.failures(
            hardware_check.counters(clean_values()),
            hardware_check.arrivals(TEN_HZ),
            8.0,
            12.0,
            300.0,
            True,
        )
        == []
    )


def test_a_rate_outside_the_window_names_both_bounds():
    slow = hardware_check.arrivals([index / 5.0 for index in range(51)])
    reasons = hardware_check.failures(
        hardware_check.counters(clean_values()), slow, 8.0, 12.0, 300.0, True
    )
    assert len(reasons) == 1
    assert 'outside 8.0 to 12.0 Hz' in reasons[0]


def test_a_gap_over_the_limit_is_named():
    # 13 messages over 1.45 s stays in band at 8.3 Hz, and one gap misses
    # three publishes, so the gap alone is the reason.
    stamps = [index / 10.0 for index in range(10)] + [1.25, 1.35, 1.45]
    sample = hardware_check.arrivals(stamps)
    assert sample.hz == pytest.approx(8.28, abs=0.01)
    assert sample.gap_ms == pytest.approx(350.0)
    reasons = hardware_check.failures(
        hardware_check.counters(clean_values()), sample, 8.0, 12.0, 300.0, True
    )
    assert len(reasons) == 1
    assert '350 ms with no /button message' in reasons[0]
    assert 'over 300 ms' in reasons[0]


def test_every_nonzero_counter_is_named():
    link = hardware_check.counters(
        clean_values(
            **{
                'mcu crc errors': '2',
                'mcu rx overflows': '1',
                'mcu dropped frames': '5',
                'topic 16 button seq gaps': '1',
                'topic 16 button dropped': '4',
            }
        )
    )
    reasons = hardware_check.failures(
        link, hardware_check.arrivals(TEN_HZ), 8.0, 12.0, 300.0, True
    )
    assert len(reasons) == 5
    for word in (
        'crc errors',
        'rx overflows',
        'dropped frames',
        'sequence gaps',
        'dropped topic frames',
    ):
        assert any(word in reason for reason in reasons), reasons


def test_an_unlit_led_fails_the_run():
    assert hardware_check.failures(
        hardware_check.counters(clean_values()),
        hardware_check.arrivals(TEN_HZ),
        8.0,
        12.0,
        300.0,
        False,
    ) == ['the LED did not light when /led went true']


def test_a_link_that_never_connects_is_a_fail_verdict_not_a_stop():
    observed = hardware_check.Observation([], 30.0, None, None, False)
    reasons = hardware_check.link_failures(observed)
    assert len(reasons) == 1
    assert 'no connected link within 30 s' in reasons[0]


def test_a_connected_link_gives_the_counters_no_reason_of_its_own():
    observed = hardware_check.Observation([], 30.0, clean_values(), 4.2, True)
    assert hardware_check.link_failures(observed) == []


def test_the_window_copies_its_arrivals_before_the_led_phase():
    live = [index / 10.0 for index in range(21)]
    # The window closes half a second after the last stamp, as a stalled
    # publisher leaves it; the LED phase then keeps pumping the node.
    observed = hardware_check.window_observation(live, 2.5, clean_values(), 4.2, False)
    live += [2.6, 2.7, 2.8]
    sample = hardware_check.arrivals(observed.arrivals, observed.window_end)
    assert sample.count == 21
    assert sample.hz == pytest.approx(10.0)
    assert sample.gap_ms == pytest.approx(500.0)


def test_the_rate_takes_the_silence_at_the_end_of_the_window():
    sample = hardware_check.arrivals([index / 10.0 for index in range(21)], 2.5)
    assert sample.gap_ms == pytest.approx(500.0)


def test_the_snapshot_helper_is_what_keeps_the_led_phase_out():
    """A live list would grow; the helper's copy cannot."""
    live = [0.0, 0.1]
    observed = hardware_check.window_observation(live, 0.2, None, 1.0, True)
    live.append(0.3)
    assert observed.arrivals == (0.0, 0.1)
    assert isinstance(observed.arrivals, tuple)


def test_the_report_lists_every_field_once_and_ends_with_the_verdict():
    lines = hardware_check.format_report({'board': 'uno', 'port': '/dev/ttyACM0'}, []).splitlines()
    assert lines[0] == '== ros2serial hardware check'
    assert lines[-1] == 'verdict: PASS'
    assert [line.split(':', 1)[0] for line in lines[1:-1]] == list(hardware_check.FIELD_ORDER)
    assert 'board: uno' in lines
    assert 'button rate: n/a' in lines


def test_a_failed_run_prints_each_reason_before_the_fail_verdict():
    lines = hardware_check.format_report(
        {}, ['the LED did not light', 'a 400 ms gap']
    ).splitlines()
    assert lines[-3:] == ['failed: the LED did not light', 'failed: a 400 ms gap', 'verdict: FAIL']


def test_the_report_carries_what_the_ticket_names():
    for name in ('board', 'port', 'flash', 'ram', 'handshake', 'button rate', 'crc errors'):
        assert name in hardware_check.FIELD_ORDER


def test_the_board_names_the_arduino_fqbn():
    assert hardware_check.BOARDS == {
        'uno': 'arduino:avr:uno',
        'mega': 'arduino:avr:mega',
        'esp32': 'esp32:esp32:esp32',
        'esp32s3': 'esp32:esp32:esp32s3',
        'esp32s3-usb': 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc',
        'esp32c3': 'esp32:esp32:esp32c3',
        'esp32c3-usb': 'esp32:esp32:esp32c3:CDCOnBoot=cdc',
    }


def test_the_port_is_required(capsys):
    with pytest.raises(SystemExit) as exit_info:
        hardware_check.main(['--board', 'uno'])
    assert exit_info.value.code == 2
    assert '--port' in capsys.readouterr().err


def test_an_unknown_board_is_refused_before_any_hardware(capsys):
    with pytest.raises(SystemExit) as exit_info:
        hardware_check.main(['--board', 'nano', '--port', '/dev/null'])
    assert exit_info.value.code == 2
    assert 'nano' in capsys.readouterr().err


def test_a_missing_device_names_the_command_that_passes_it(capsys):
    assert hardware_check.main(['--board', 'uno', '--port', '/nonexistent/ttyACM0']) == 2
    reported = capsys.readouterr().err
    assert '/nonexistent/ttyACM0' in reported
    assert 'make hardware-check PORT=/nonexistent/ttyACM0 BOARD=uno' in reported


def test_the_board_list_parses_every_board_with_its_fqbn_and_prefix(tmp_path):
    boards = tmp_path / 'boards.list'
    boards.write_text(
        '# comment\n\nuno arduino:avr:uno avr\n'
        'esp32s3 esp32:esp32:esp32s3 xtensa-esp32s3-elf\n'
        'esp32s3-usb esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc'
        ' xtensa-esp32s3-elf\n'
    )
    fqbns, prefixes = hardware_check.load_boards(boards)
    assert fqbns == {
        'uno': 'arduino:avr:uno',
        'esp32s3': 'esp32:esp32:esp32s3',
        'esp32s3-usb': 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc',
    }
    assert prefixes == {
        'uno': 'avr',
        'esp32s3': 'xtensa-esp32s3-elf',
        'esp32s3-usb': 'xtensa-esp32s3-elf',
    }


def test_the_shipped_board_list_names_the_esp32_boards():
    root = Path(hardware_check.__file__).resolve().parent.parent
    fqbns, prefixes = hardware_check.load_boards(root / 'ports/arduino/boards.list')
    assert fqbns['uno'] == 'arduino:avr:uno'
    assert fqbns['mega'] == 'arduino:avr:mega'
    assert fqbns['esp32s3'] == 'esp32:esp32:esp32s3'
    assert fqbns['esp32s3-usb'] == 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc'
    assert prefixes['esp32s3'] == 'xtensa-esp32s3-elf'
    assert prefixes['esp32c3'] == 'riscv32-esp-elf'


def test_the_size_tool_comes_from_the_board_list_prefix():
    assert hardware_check.size_tool('uno') == 'avr-size'
    assert hardware_check.size_tool('mega') == 'avr-size'
    assert hardware_check.size_tool('esp32s3') == 'xtensa-esp32s3-elf-size'
    assert hardware_check.size_tool('esp32s3-usb') == 'xtensa-esp32s3-elf-size'
    assert hardware_check.size_tool('esp32c3') == 'riscv32-esp-elf-size'
    assert hardware_check.size_tool('esp32c3-usb') == 'riscv32-esp-elf-size'


def test_wait_for_port_returns_once_the_node_opens(monkeypatch):
    calls = {'count': 0}

    def fake_exists(path):
        calls['count'] += 1
        return calls['count'] >= 3

    opened = []

    class FakeHandle:
        def __enter__(self):
            return self

        def __exit__(self, *args):
            return False

    def fake_open(path, mode='rb'):
        opened.append(path)
        return FakeHandle()

    monkeypatch.setattr('os.path.exists', fake_exists)
    monkeypatch.setattr('builtins.open', fake_open)
    monkeypatch.setattr(hardware_check.time, 'sleep', lambda seconds: None)
    hardware_check.wait_for_port('/dev/ttyACM0', timeout_s=10.0, poll_s=0.1)
    assert opened, 'the node was never opened'


def test_wait_for_port_times_out_with_the_port_and_the_by_id_hint(monkeypatch):
    monkeypatch.setattr('os.path.exists', lambda path: False)
    monkeypatch.setattr(hardware_check.time, 'sleep', lambda seconds: None)
    with pytest.raises(hardware_check.CheckError) as error:
        hardware_check.wait_for_port('/dev/ttyACM0', timeout_s=0.3, poll_s=0.1)
    assert 'the port did not come back after the upload' in str(error.value)
    assert 'PORT=/dev/serial/by-id/' in str(error.value)


def test_the_led_question_accepts_skip_for_an_esp32_board(monkeypatch, capsys):
    monkeypatch.setattr('sys.stdin', io.StringIO('s\n'))
    assert hardware_check.ask_led('esp32s3') is None


def test_the_led_question_rejects_skip_for_an_avr_board(monkeypatch, capsys):
    monkeypatch.setattr('sys.stdin', io.StringIO('s\nn\n'))
    assert hardware_check.ask_led('uno') is False


def test_a_skipped_led_adds_no_failure():
    link = hardware_check.counters(clean_values())
    reasons = hardware_check.failures(
        link, hardware_check.arrivals(TEN_HZ), 8.0, 12.0, 300.0, None
    )
    assert reasons == []


def test_a_skipped_led_prints_skipped_and_a_qualified_pass():
    lines = hardware_check.format_report(
        {'board': 'esp32s3', 'led': 'skipped', 'core': 'esp32:esp32@3.3.12'}, []
    ).splitlines()
    assert 'led: skipped' in lines
    assert lines[-1] == 'verdict: PASS (LED not checked)'


def test_the_core_field_names_the_pinned_core(tmp_path):
    versions = tmp_path / 'versions.env'
    versions.write_text('ARDUINO_AVR_CORE_VERSION=1.8.6\nARDUINO_ESP32_CORE_VERSION=3.3.12\n')
    assert hardware_check.core_for_board('uno', versions) == 'arduino:avr@1.8.6'
    assert hardware_check.core_for_board('mega', versions) == 'arduino:avr@1.8.6'
    assert hardware_check.core_for_board('esp32s3', versions) == 'esp32:esp32@3.3.12'
    assert hardware_check.core_for_board('esp32s3-usb', versions) == 'esp32:esp32@3.3.12'


def test_the_report_field_order_carries_the_core():
    assert 'core' in hardware_check.FIELD_ORDER
