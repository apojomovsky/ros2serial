# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Prepare Arduino libraries and pass their required limits to Arduino CLI."""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from collections.abc import Mapping
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


def _runtime_sources() -> tuple[Path, Path]:
    """Return the asset roots, refusing an installation that lacks them."""
    assets, cpp = _assets()
    for source in (
        assets / 'core/include/r2s',
        assets / 'core/src',
        cpp / 'src',
        cpp / 'include/r2s',
        assets / 'ports/arduino',
    ):
        if not source.is_dir():
            raise ValueError(f'missing firmware assets at {source}; rebuild r2s_gen')
    return assets, cpp


def stage_runtime(destination: Path) -> None:
    """
    Copy the runtime a sketch compiles against into the library at `destination`.

    The core sources and headers, the C++ layer and the Arduino port glue: what
    `r2s_bundled.c` and the sketch's `#include <ros2serial.h>` reach but the
    emitted files do not carry. The test core source stays behind, as in every
    library the repository ships.

    :param destination: the library root, whose `src/` receives the files.
    :raises ValueError: if the installation lacks the assets.
    """
    assets, cpp = _runtime_sources()
    shutil.copytree(assets / 'core/include/r2s', destination / 'src/r2s', dirs_exist_ok=True)
    shutil.copytree(cpp / 'include/r2s', destination / 'src/r2s', dirs_exist_ok=True)
    for source in (assets / 'core/src').glob('*.c'):
        if source.name != 'test_reset.c':
            shutil.copy2(source, destination / 'src' / source.name)
    for source in (cpp / 'src').glob('*.cpp'):
        shutil.copy2(source, destination / 'src' / source.name)
    for name in ('r2s_arduino_port.h', 'r2s_arduino_port.cpp'):
        shutil.copy2(assets / 'ports/arduino' / name, destination / 'src' / name)


def write_archive(path: str | Path, library: bundle.Library, files: Mapping[str, str]) -> None:
    """
    Write a library as the ZIP the Arduino IDE installs.

    Add .ZIP Library takes the archive's single top folder as the library, so
    every entry sits under one folder named for the library, and the entries are
    the bytes the out directory holds. The runtime a sketch compiles against
    joins them there: the IDE passes no build property, so the archive is the
    only place the core, the C++ layer and the port can come from.

    :param path: the archive to write.
    :param library: the library the files describe, whose name the folder takes.
    :param files: relative path to file text, as the out directory holds them.
    :raises MessageError: if the path names a directory.
    :raises ValueError: if the installation lacks the runtime assets.
    :raises OSError: if the archive cannot be written.
    """
    destination = Path(path)
    if destination.is_dir():
        raise MessageError('--zip', f'{destination} is a directory', 'name the archive file')
    with tempfile.TemporaryDirectory() as staging:
        root = Path(staging) / library.name
        layouts._write(str(root), dict(files))
        stage_runtime(root)
        entries = sorted(
            item.relative_to(staging).as_posix() for item in root.rglob('*') if item.is_file()
        )
        try:
            destination.parent.mkdir(parents=True, exist_ok=True)
            # Fixed timestamps and modes keep a rebuilt archive byte-identical,
            # as the release zip is: a diff between two builds means a change.
            # A ZipInfo starts at ZIP_STORED, so each entry names its method
            # itself; the archive's default never reaches it.
            with zipfile.ZipFile(destination, 'w', zipfile.ZIP_DEFLATED) as archive:
                for name in entries:
                    info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
                    info.external_attr = 0o644 << 16
                    info.compress_type = zipfile.ZIP_DEFLATED
                    archive.writestr(info, (Path(staging) / name).read_bytes())
        except OSError as error:
            raise OSError(f'cannot write {destination} ({error}); check the path') from error


def _library(args: argparse.Namespace) -> int:
    destination = Path(args.out)
    if destination.exists():
        raise ValueError(f'{destination} already exists; choose a new library directory')
    files = bundle.bundle(args.distro)
    # Resolve inputs before creating the destination: an incomplete installation
    # must not leave a library that appears ready to compile.
    assets, _ = _runtime_sources()
    layouts._write(str(destination), files)
    stage_runtime(destination)
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
