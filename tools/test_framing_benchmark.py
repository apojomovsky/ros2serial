"""Cross-checks for the P9 framing benchmark.

The Python models in benchmark/framing are the readable spec of each
alternative framer; the C next to them is what the AVR firmware runs. These
tests pin both to hand-computed vectors, to each other, and to the recovery
behavior the write-up reports. The AVR firmware build test proves the
measured code compiles for both boards.
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
FRAMING = ROOT / 'benchmark' / 'framing'

sys.path.insert(0, str(FRAMING))  # noqa: E402

import models  # noqa: E402

FRAMERS = ('rcobs', 'cobs', 'slip', 'rs')
TOPIC = 0x10
C_SOURCES = ('host_driver.c', 'bm_cobs.c', 'bm_slip.c', 'bm_rs.c', 'bm_crc_table.c')


def read_payloads():
    """Parse the payload arrays out of benchmark/framing/payloads.h."""
    text = (FRAMING / 'payloads.h').read_text()
    found = re.findall(r'static const uint8_t (bm_p\d)\[\] = \{([^}]*)\}', text)
    payloads = {}
    for name, body in found:
        payloads[name] = bytes(
            int(token, 16) for token in re.findall(r'0x([0-9A-Fa-f]{2})u', body)
        )
    return payloads


def raw_frame(seq, payload):
    """Topic, seq and payload without the check bytes."""
    return bytes((TOPIC, seq)) + bytes(payload)


def model_wire(framer, seq, payload):
    """The exact wire bytes the model emits for one frame."""
    payload = bytes(payload)
    if framer == 'rs':
        return models.rs_encode(TOPIC, seq, payload)
    raw = raw_frame(seq, payload)
    crc = models.crc16_mcrf4xx_bitwise(raw)
    dec = raw + bytes((crc & 0xFF, (crc >> 8) & 0xFF))
    if framer == 'rcobs':
        enc = models.rcobs_encode(dec)
    elif framer == 'cobs':
        enc = models.cobs_encode(dec)
    else:
        # SLIP carries its own END; the others take the delimiter here.
        return models.slip_encode(dec)
    return enc + bytes((models.delimiter_of(framer),))


def model_decode_line(framer, segment):
    """The one driver output line decoding a segment would print."""
    try:
        if framer == 'rcobs':
            dec = models.rcobs_decode(segment)
        elif framer == 'cobs':
            dec = models.cobs_decode(segment)
        else:
            dec = models.slip_decode(segment)
    except ValueError:
        return 'DROP'
    if models.crc16_mcrf4xx_bitwise(dec) != 0:
        return 'CRC ' + dec.hex()
    return 'OK ' + dec.hex()


def model_scan_lines(framer, stream):
    """Every driver output line scanning a stream would print."""
    lines = []
    if framer == 'rs':
        frames, _, last_end = models.rs_scan(stream)
        for topic, seq, payload in frames:
            lines.append(f'FRAME {topic:04x} {seq:02x} ' + bytes(payload).hex())
        lines.append(f'CONSUMED {last_end if frames else 0}')
        return lines
    delimiter = models.delimiter_of(framer)
    consumed = len(stream)
    while consumed > 0 and stream[consumed - 1] != delimiter:
        consumed -= 1
    segments = []
    current = bytearray()
    for byte in stream[:consumed]:
        if byte == delimiter:
            segments.append(bytes(current))
            current = bytearray()
        else:
            current.append(byte)
    for segment in segments:
        if segment:
            lines.append(model_decode_line(framer, segment))
    lines.append(f'CONSUMED {consumed}')
    return lines


def corruption_streams(payloads):
    """One flipped, one truncated and one garbage stream per framer."""
    p5 = payloads['bm_p5']
    p1 = payloads['bm_p1']
    streams = {}
    for framer in FRAMERS:
        first = model_wire(framer, 0x07, p5)
        second = model_wire(framer, 0x08, p1)
        flipped = bytearray(first)
        flipped[3] ^= 0x40
        if framer == 'rs':
            streams[(framer, 'flip')] = bytes(flipped) + second
            streams[(framer, 'trunc')] = first[:-1] + second
            streams[(framer, 'garbage')] = first + b'\x55\xaa\x55' + second
        else:
            delimiter = bytes((models.delimiter_of(framer),))
            streams[(framer, 'flip')] = bytes(flipped) + second
            streams[(framer, 'trunc')] = first[:-1] + second
            streams[(framer, 'garbage')] = first + b'\x55\xaa\x55' + delimiter + second
    return streams


@pytest.fixture(scope='module')
def payloads():
    found = read_payloads()
    assert len(found) == 6
    assert [len(found[f'bm_p{i}']) for i in range(1, 7)] == [4, 48, 32, 122, 64, 32]
    assert found['bm_p1'] == bytes((0x00, 0x00, 0x80, 0x3F))
    return found


@pytest.fixture(scope='module')
def driver(tmp_path_factory):
    for source in C_SOURCES:
        assert (FRAMING / source).exists(), f'missing {source}'
    exe = tmp_path_factory.mktemp('bm') / 'host_driver'
    command = (
        ['gcc', '-std=c99', '-Wall', '-Werror']
        + [f'-I{d}' for d in (FRAMING, ROOT / 'core' / 'include')]
        + [str(FRAMING / s) for s in C_SOURCES]
        + [str(ROOT / 'core' / 'src' / s) for s in ('rcobs.c', 'crc.c')]
        + ['-o', str(exe)]
    )
    subprocess.run(command, capture_output=True, text=True, check=True)
    return str(exe)


def run_driver(driver, op, framer, data):
    proc = subprocess.run(
        [driver, op, framer, bytes(data).hex()], capture_output=True, text=True, check=True
    )
    return proc.stdout.splitlines()


def test_c_table_crc_matches_bitwise(driver, payloads):
    for payload in payloads.values():
        raw = raw_frame(0x01, payload)
        expected = f'{models.crc16_mcrf4xx_bitwise(raw):04x}'
        proc = subprocess.run(
            [driver, 'crc', 'table', bytes(raw).hex()], capture_output=True, text=True, check=True
        )
        assert proc.stdout.splitlines() == [expected]
    proc = subprocess.run(
        [driver, 'crc', 'table', b'123456789'.hex()], capture_output=True, text=True, check=True
    )
    assert proc.stdout.splitlines() == ['6f91']


def test_crc_check_value_matches_the_spec():
    assert models.crc16_mcrf4xx_bitwise(b'123456789') == 0x6F91
    assert models.crc16_mcrf4xx_table(b'123456789') == 0x6F91


def test_crc_table_matches_bitwise_on_every_payload(payloads):
    for payload in payloads.values():
        raw = raw_frame(0x01, payload)
        assert models.crc16_mcrf4xx_table(raw) == models.crc16_mcrf4xx_bitwise(raw)


def test_crc_table_matches_bitwise_on_every_single_byte():
    for value in range(256):
        data = bytes((value,))
        assert models.crc16_mcrf4xx_table(data) == models.crc16_mcrf4xx_bitwise(data)


def test_rcobs_hand_vectors():
    assert models.rcobs_encode(b'\x00') == bytes((0x01, 0x01))
    assert models.rcobs_encode(b'\x11\x22') == bytes((0x11, 0x22, 0x03))
    long_run = bytes(range(1, 255))
    assert models.rcobs_encode(long_run)[-2:] == bytes((0xFF, 0x01))
    assert models.rcobs_decode(bytes((0x01, 0x01))) == b'\x00'
    assert models.rcobs_decode(bytes((0x11, 0x22, 0x03))) == b'\x11\x22'
    with pytest.raises(ValueError):
        models.rcobs_decode(bytes((0x11, 0xFF)))
    with pytest.raises(ValueError):
        models.rcobs_decode(bytes((0x01, 0x00, 0x01)))


def test_cobs_hand_vectors():
    assert models.cobs_encode(b'\x00') == bytes((0x01, 0x01))
    assert models.cobs_encode(bytes((0x11, 0x22, 0x00, 0x33))) == bytes(
        (0x03, 0x11, 0x22, 0x02, 0x33)
    )
    assert models.cobs_decode(bytes((0x01, 0x01))) == b'\x00'
    assert models.cobs_decode(bytes((0x03, 0x11, 0x22, 0x02, 0x33))) == bytes(
        (0x11, 0x22, 0x00, 0x33)
    )
    with pytest.raises(ValueError):
        models.cobs_decode(bytes((0xFF,)))
    with pytest.raises(ValueError):
        models.cobs_decode(bytes((0x02, 0x00)))


def test_slip_hand_vectors():
    assert models.slip_encode(bytes((0xC0,))) == bytes((0xDB, 0xDC, 0xC0))
    assert models.slip_encode(bytes((0xDB,))) == bytes((0xDB, 0xDD, 0xC0))
    assert models.slip_encode(bytes((0xC0, 0xDB, 0x00, 0x11))) == bytes(
        (0xDB, 0xDC, 0xDB, 0xDD, 0x00, 0x11, 0xC0)
    )
    assert models.slip_decode(bytes((0xDB, 0xDC, 0xDB, 0xDD, 0x00, 0x11))) == bytes(
        (0xC0, 0xDB, 0x00, 0x11)
    )
    with pytest.raises(ValueError):
        models.slip_decode(bytes((0xDB,)))
    with pytest.raises(ValueError):
        models.slip_decode(bytes((0xDB, 0x09)))


def test_rs_hand_vector():
    assert models.rs_encode(TOPIC, 0x00, b'') == bytes.fromhex('ff010400fc100000f0')
    frames, first, last = models.rs_scan(bytes.fromhex('ff010400fc100000f0'))
    assert frames == [(TOPIC, 0x00, b'')]
    assert (first, last) == (0, 9)


def test_every_model_round_trips_every_payload(payloads):
    for framer in FRAMERS:
        for seq, payload in enumerate(payloads.values()):
            wire = model_wire(framer, seq, payload)
            if framer == 'rs':
                frames, _, _ = models.rs_scan(wire)
                assert frames == [(TOPIC, seq, bytes(payload))]
            else:
                assert wire.endswith(bytes((models.delimiter_of(framer),)))
                lines = model_scan_lines(framer, wire)
                assert lines[-1] == f'CONSUMED {len(wire)}'
                assert sum(line.startswith('OK ') for line in lines) == 1


def test_wire_has_no_stray_delimiters(payloads):
    for framer in ('rcobs', 'cobs'):
        for seq, payload in enumerate(payloads.values()):
            wire = model_wire(framer, seq, payload)
            assert wire.endswith(b'\x00')
            assert b'\x00' not in wire[:-1]
    for seq, payload in enumerate(payloads.values()):
        wire = model_wire('slip', seq, payload)
        assert wire.endswith(bytes((models.SLIP_END,)))
        assert wire.count(bytes((models.SLIP_END,))) == 1


def test_wire_overhead_stays_within_bounds(payloads):
    for seq, payload in enumerate(payloads.values()):
        raw = raw_frame(seq, payload)
        crc = models.crc16_mcrf4xx_bitwise(raw)
        dec_len = len(raw) + 2
        assert len(model_wire('rcobs', seq, payload)) - 1 <= dec_len + dec_len // 254 + 1
        assert len(model_wire('cobs', seq, payload)) - 1 <= dec_len + dec_len // 254 + 1
        assert len(model_wire('slip', seq, payload)) - 1 <= 2 * dec_len + 1
        assert len(model_wire('rs', seq, payload)) == 9 + len(bytes(payload))
        assert models.crc16_mcrf4xx_bitwise(raw + bytes((crc & 0xFF, (crc >> 8) & 0xFF))) == 0


def ok_line(seq, payload):
    """The driver line for one intact frame."""
    dec = raw_frame(seq, payload)
    crc = models.crc16_mcrf4xx_bitwise(dec)
    return 'OK ' + (dec + bytes((crc & 0xFF, (crc >> 8) & 0xFF))).hex()


def test_delimited_models_recover_around_corruption(payloads):
    streams = corruption_streams(payloads)
    first_line = ok_line(0x07, payloads['bm_p5'])
    second_line = ok_line(0x08, payloads['bm_p1'])
    for framer in ('rcobs', 'cobs', 'slip'):
        flip_lines = model_scan_lines(framer, streams[(framer, 'flip')])
        assert flip_lines[-1] == f'CONSUMED {len(streams[(framer, "flip")])}'
        assert [line for line in flip_lines if line.startswith('OK ')] == [second_line]
        trunc_lines = model_scan_lines(framer, streams[(framer, 'trunc')])
        assert not any(line.startswith('OK ') for line in trunc_lines), (framer, trunc_lines)
        assert trunc_lines[-1] == f'CONSUMED {len(streams[(framer, "trunc")])}'
        garbage_lines = model_scan_lines(framer, streams[(framer, 'garbage')])
        assert [line for line in garbage_lines if line.startswith('OK ')] == [
            first_line,
            second_line,
        ]


def test_rs_model_recover_around_corruption(payloads):
    streams = corruption_streams(payloads)
    first = (TOPIC, 0x07, bytes(payloads['bm_p5']))
    second = (TOPIC, 0x08, bytes(payloads['bm_p1']))
    for kind in ('flip', 'trunc'):
        stream = streams[('rs', kind)]
        frames, _, last_end = models.rs_scan(stream)
        assert frames == [second]
        assert last_end == len(stream)
    stream = streams[('rs', 'garbage')]
    frames, _, last_end = models.rs_scan(stream)
    assert frames == [first, second]
    assert last_end == len(stream)


def test_c_encode_matches_the_model(driver, payloads):
    for framer in FRAMERS:
        for seq, payload in enumerate(payloads.values()):
            expected = model_wire(framer, seq, payload).hex()
            assert run_driver(driver, 'encode', framer, raw_frame(seq, payload)) == [expected]


def test_c_decode_matches_the_model(driver, payloads):
    for framer in ('rcobs', 'cobs', 'slip'):
        for seq, payload in enumerate(payloads.values()):
            wire = model_wire(framer, seq, payload)
            assert run_driver(driver, 'decode', framer, wire[:-1]) == [
                model_decode_line(framer, wire[:-1])
            ]
    assert run_driver(driver, 'decode', 'rcobs', bytes((0x11, 0xFF))) == ['ERR']
    assert run_driver(driver, 'decode', 'cobs', bytes((0xFF,))) == ['ERR']
    assert run_driver(driver, 'decode', 'slip', bytes((0xDB,))) == ['ERR']
    assert run_driver(driver, 'decode', 'slip', bytes((0xDB, 0x09))) == ['ERR']


def test_c_scan_matches_the_model_on_corruption(driver, payloads):
    streams = corruption_streams(payloads)
    for key, stream in streams.items():
        framer = key[0]
        assert run_driver(driver, 'scan', framer, stream) == model_scan_lines(framer, stream)


def test_c_scan_recovers_the_second_frame(driver, payloads):
    streams = corruption_streams(payloads)
    first_ok = ok_line(0x07, payloads['bm_p5'])
    second_ok = ok_line(0x08, payloads['bm_p1'])
    for framer in ('rcobs', 'cobs', 'slip'):
        flip_lines = run_driver(driver, 'scan', framer, streams[(framer, 'flip')])
        assert [line for line in flip_lines if line.startswith('OK ')] == [second_ok]
        trunc_lines = run_driver(driver, 'scan', framer, streams[(framer, 'trunc')])
        assert not any(line.startswith('OK ') for line in trunc_lines)
        garbage_lines = run_driver(driver, 'scan', framer, streams[(framer, 'garbage')])
        assert [line for line in garbage_lines if line.startswith('OK ')] == [first_ok, second_ok]
    for kind in ('flip', 'trunc', 'garbage'):
        rs_lines = run_driver(driver, 'scan', 'rs', streams[('rs', kind)])
        assert rs_lines == model_scan_lines('rs', streams[('rs', kind)])
        assert rs_lines[-2] == f'FRAME {TOPIC:04x} 08 ' + bytes(payloads['bm_p1']).hex()


@pytest.mark.parametrize('mcu', ('atmega328p', 'atmega2560'))
def test_benchmark_firmware_builds_for_mcu(tmp_path, mcu):
    if shutil.which('avr-gcc') is None:
        pytest.skip('avr-gcc not on PATH: run in the dev image')
    firmware = FRAMING / 'fw_bench.c'
    assert firmware.exists(), 'missing fw_bench.c'
    exe = tmp_path / f'fw-{mcu}.elf'
    command = (
        [
            'avr-gcc',
            f'-mmcu={mcu}',
            '-DF_CPU=16000000UL',
            '-Os',
            '-std=c99',
            '-Wall',
            '-Wextra',
            '-Werror',
            '-ffunction-sections',
            '-fdata-sections',
            '-Wl,--gc-sections',
            '-DBM_FRAMER=0',
            '-DBM_CRC_TABLE=0',
            f'-I{FRAMING}',
            f'-I{ROOT / "core" / "include"}',
            str(firmware),
        ]
        + [str(FRAMING / s) for s in ('bm_cobs.c', 'bm_slip.c', 'bm_rs.c', 'bm_crc_table.c')]
        + [str(ROOT / 'core' / 'src' / s) for s in ('rcobs.c', 'crc.c')]
        + ['-o', str(exe)]
    )
    subprocess.run(command, capture_output=True, text=True, check=True)
