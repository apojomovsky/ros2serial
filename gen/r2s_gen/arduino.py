# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Prepare Arduino libraries and pass their required limits to Arduino CLI."""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

from r2s_gen import bundle
from r2s_gen import layouts
from r2s_gen.msgs import MessageError


def _assets() -> tuple[Path, Path]:
    installed = Path(__file__).parent
    if (installed / 'firmware').is_dir():
        return installed / 'firmware', installed / 'cpp'
    checkout = installed.parents[1]
    return checkout, checkout / 'cpp'


def _library(args: argparse.Namespace) -> int:
    destination = Path(args.out)
    if destination.exists():
        raise ValueError(f'{destination} already exists; choose a new library directory')
    files = bundle.bundle(args.distro)
    assets, cpp = _assets()
    # Resolve inputs before creating the destination: an incomplete installation
    # must not leave a library that appears ready to compile.
    sources = {
        'src/r2s': assets / 'core/include/r2s',
        'src/ros2serial': cpp / 'include/ros2serial',
    }
    for source in (*sources.values(), assets / 'core/src', cpp / 'src'):
        if not source.is_dir():
            raise ValueError(f'missing firmware assets at {source}; rebuild r2s_gen')
    layouts._write(str(destination), files)
    for target, source in sources.items():
        shutil.copytree(source, destination / target, dirs_exist_ok=True)
    shutil.copytree(cpp / 'include/r2s', destination / 'src/r2s', dirs_exist_ok=True)
    for source in (assets / 'core/src').glob('*.c'):
        if source.name != 'test_reset.c':
            shutil.copy2(source, destination / 'src' / source.name)
    for source in (cpp / 'src').glob('*.cpp'):
        shutil.copy2(source, destination / 'src' / source.name)
    for name in ('r2s_arduino_port.h', 'r2s_arduino_port.cpp'):
        shutil.copy2(assets / 'ports/arduino' / name, destination / 'src' / name)
    for sketch in (assets / 'examples').glob('*/*.ino'):
        name = sketch.parent.name.removeprefix('arduino_')
        target = destination / 'examples' / name
        target.mkdir(parents=True, exist_ok=True)
        for source in sketch.parent.iterdir():
            if source.suffix in ('.ino', '.c', '.h'):
                shutil.copy2(
                    source, target / (name + '.ino' if source.suffix == '.ino' else source.name)
                )
    print(f'Arduino library: {destination.resolve()}')
    return 0


def _compile(args: argparse.Namespace) -> int:
    library = Path(args.library).resolve()
    header = library / 'src/r2s_bundled.h'
    definitions = layouts.build_defines(header.read_text(), str(header))
    if not Path(args.sketch).is_dir():
        raise ValueError(f'no sketch directory at {args.sketch}')
    flags = ' '.join('-D' + definition for definition in definitions)
    command = [
        'arduino-cli',
        'compile',
        '--fqbn',
        args.fqbn,
        '--library',
        str(library),
        '--output-dir',
        args.out,
        '--build-property',
        f'compiler.c.extra_flags={flags}',
        '--build-property',
        f'compiler.cpp.extra_flags={flags}',
        args.sketch,
    ]
    return subprocess.run(command, check=False).returncode


def main(argv: list[str] | None = None) -> int:
    """Run library preparation or compilation, preserving Arduino CLI failures."""
    parser = argparse.ArgumentParser(description=__doc__, prog='r2s-arduino')
    commands = parser.add_subparsers(dest='command', required=True)
    library = commands.add_parser('library', help='create a complete library in a new directory')
    library.add_argument('--out', required=True)
    library.add_argument('--distro', default=os.environ.get('ROS_DISTRO', 'jazzy'))
    library.set_defaults(run=_library)
    compile_ = commands.add_parser('compile', help='compile a bundled-type sketch with its limits')
    compile_.add_argument('--library', required=True)
    compile_.add_argument('--fqbn', required=True)
    compile_.add_argument('--out', required=True)
    compile_.add_argument('sketch')
    compile_.set_defaults(run=_compile)
    try:
        args = parser.parse_args(argv)
        return int(args.run(args))
    except SystemExit as error:
        return int(error.code or 0)
    except (OSError, ValueError, MessageError) as error:
        print(f'r2s-arduino: error: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
