"""Run the P9 framing benchmark: build, measure, print.

Builds one firmware per framer for the Uno and the Mega, runs each under
the simavr harness, and prints the cycles, flash, RAM and stack table the
write-up in docs/benchmarks cites. Measurements come from the firmware's
own Timer1 deltas, so they are deterministic in simavr; every config runs
twice and the run refuses to report when the two runs disagree.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
FRAMING = ROOT / 'benchmark' / 'framing'

sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests' / 'harness' / 'simavr'))

import size_report  # noqa: E402

CONFIGS = (
    (0, 0, 'rcobs'),
    (1, 0, 'cobs'),
    (2, 0, 'slip'),
    (3, 0, 'rs'),
    (0, 1, 'rcobs-table'),
)
MCUS = ('atmega328p', 'atmega2560')
SOURCES = ('fw_bench.c', 'bm_cobs.c', 'bm_slip.c', 'bm_rs.c', 'bm_crc_table.c')
CORE_SOURCES = ('rcobs.c', 'crc.c')
PAYLOAD_NAMES = ('P1', 'P2', 'P3', 'P4', 'P5', 'P6')


def build_firmware(build_dir, framer, table, mcu):
    """Compile one benchmark firmware, returning its ELF path."""
    elf = build_dir / f'bm-{framer}-{table}-{mcu}.elf'
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
            f'-DBM_FRAMER={framer}',
            f'-DBM_CRC_TABLE={table}',
            f'-I{FRAMING}',
            f'-I{ROOT / "core" / "include"}',
        ]
        + [str(FRAMING / s) for s in SOURCES]
        + [str(ROOT / 'core' / 'src' / s) for s in CORE_SOURCES]
        + ['-o', str(elf)]
    )
    proc = subprocess.run(command, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise SystemExit(f'avr-gcc failed for framer={framer} table={table} {mcu}:\n{proc.stderr}')
    return elf


def measure_elf(elf):
    """Flash and static RAM from avr-size, framer symbols from avr-nm."""
    size_proc = subprocess.run(['avr-size', str(elf)], capture_output=True, text=True, check=True)
    sizes = size_report.parse_avr_size(size_proc.stdout)
    nm_proc = subprocess.run(
        ['avr-nm', '--print-size', str(elf)], capture_output=True, text=True, check=True
    )
    symbols = size_report.parse_nm_sizes(nm_proc.stdout)
    framing = {name: size for name, size in symbols.items() if name.startswith('bm_')}
    return sizes, framing


def collect_once(harness_cls, image, mcu):
    """Run one firmware under simavr and parse its BM report lines."""
    with harness_cls(mcu, '--stack-paint', image=image) as harness:
        import serial

        payloads = {}
        start = None
        done_ok = 0
        with serial.Serial(harness.pty, 115200, timeout=10) as port:
            for _ in range(40):
                raw = port.readline(256)
                if not raw:
                    raise SystemExit(f'no BM DONE from {image} on {mcu}')
                line = raw.decode('ascii', 'replace').strip()
                if line.startswith('BM START'):
                    start = line
                    payloads = {}
                elif line.startswith('BM PAYLOAD'):
                    parts = line.split()
                    fields = dict(part.split('=') for part in parts[3:])
                    payloads[int(parts[2])] = {k: int(v) for k, v in fields.items()}
                elif line.startswith('BM DONE'):
                    if start is not None and len(payloads) == 6:
                        done_ok = int(line.split('=')[1])
                        break
                    start = None
                    payloads = {}
        harness.stop()
        stack_peak = harness.summary.get('stack_peak', 0)
    if start is None or len(payloads) != 6 or done_ok != 1:
        raise SystemExit(f'incomplete report from {image} on {mcu}: {start} {payloads}')
    if any(row.get('ok') != 1 for row in payloads.values()):
        raise SystemExit(f'round trip failed for {image} on {mcu}: {payloads}')
    return {'start': start, 'payloads': payloads, 'stack_peak': stack_peak}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--harness-bin', required=True)
    args = parser.parse_args()

    for tool in ('avr-gcc', 'avr-size', 'avr-nm'):
        if shutil.which(tool) is None:
            raise SystemExit(f'{tool} not on PATH: run in the dev image')

    build_dir = Path(args.build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)
    os.environ['R2S_SIMAVR_BIN'] = args.harness_bin
    os.environ['R2S_SIMAVR_FW'] = str(build_dir)

    import test_echo

    results = {}
    for framer, table, name in CONFIGS:
        for mcu in MCUS:
            elf = build_firmware(build_dir, framer, table, mcu)
            sizes, framing = measure_elf(elf)
            image = f'bm-{framer}-{table}'
            first = collect_once(test_echo.Harness, image, mcu)
            second = collect_once(test_echo.Harness, image, mcu)
            if first['payloads'] != second['payloads']:
                raise SystemExit(f'nondeterministic cycles for {name} on {mcu}')
            key = f'{name}/{mcu}'
            results[key] = {
                'framer': framer,
                'table': table,
                'flash': sizes['flash'],
                'ram': sizes['ram'],
                'stack_peak': first['stack_peak'],
                'framing_symbols': framing,
                'payloads': first['payloads'],
            }
            cells = ' '.join(
                f'{PAYLOAD_NAMES[p - 1]}:w{row["wirelen"]}/e{row["enc"]}/d{row["dec"]}'
                for p, row in sorted(first['payloads'].items())
            )
            print(
                f'{key}: flash={sizes["flash"]} ram={sizes["ram"]} '
                f'stack={first["stack_peak"]} {cells}',
                flush=True,
            )

    with open(build_dir / 'results.json', 'w') as handle:
        json.dump(results, handle, indent=2, sort_keys=True)
    print(f'wrote {build_dir / "results.json"}')


if __name__ == '__main__':
    main()
