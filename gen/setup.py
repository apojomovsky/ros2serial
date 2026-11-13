# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""
The r2s-gen distribution.

Pip installs it, and colcon builds the same sources as an ament_python package.

The version comes from the package itself, where the release cut writes it, so
a wheel and a colcon build of one commit always agree.

The C++ layer's tree ships with the wheel, so an installed generator can hand
the runtime layer and the per-message headers to a build without a checkout.
"""

import shutil
import sys
from pathlib import Path

from setuptools import find_packages
from setuptools import setup

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from r2s_gen.version import __version__  # noqa: E402

# The C++ layer lives beside the generator, not inside it. setuptools cannot
# stage a package from outside the project directory: a ../cpp mapping
# escapes the sdist root and lands in the working tree instead of the
# archive. The tree is copied inside gen/ first, so the sdist carries it.
CPP_TREE = HERE.parent / 'cpp'
CPP_STAGE = HERE / 'cpp_stage'
if CPP_TREE.is_dir():
    shutil.rmtree(CPP_STAGE, ignore_errors=True)
    shutil.copytree(CPP_TREE, CPP_STAGE)

FIRMWARE_STAGE = HERE / 'firmware_stage'
if (HERE.parent / 'core').is_dir():
    shutil.rmtree(FIRMWARE_STAGE, ignore_errors=True)
    for directory in ('core/include', 'core/src', 'ports/arduino'):
        shutil.copytree(
            HERE.parent / directory,
            FIRMWARE_STAGE / directory,
            ignore=shutil.ignore_patterns('tests', '__pycache__'),
        )
    for sketch in (HERE.parent / 'examples').glob('arduino_*/*.ino'):
        name = sketch.parent.name.removeprefix('arduino_')
        target = FIRMWARE_STAGE / 'examples' / name
        target.mkdir(parents=True, exist_ok=True)
        for source in sketch.parent.iterdir():
            if source.suffix in ('.ino', '.c', '.h'):
                shutil.copy2(
                    source, target / (name + '.ino' if source.suffix == '.ino' else source.name)
                )

packages = find_packages(include=['r2s_gen', 'r2s_gen.*'])
package_dir = {}
package_data = {
    'r2s_gen': [
        'snapshots/README.md',
        'snapshots/*/*/msg/*.msg',
        'snapshots/*/*/srv/*.srv',
        'snapshots/*/*/action/*.action',
        'schema/*.json',
    ]
}
if CPP_STAGE.is_dir():
    packages.append('r2s_gen.cpp')
    package_dir['r2s_gen.cpp'] = 'cpp_stage'
    package_data['r2s_gen.cpp'] = ['include/**/*.h', 'include/**/*.hpp', 'src/*.cpp']
if FIRMWARE_STAGE.is_dir():
    packages.append('r2s_gen.firmware')
    package_dir['r2s_gen.firmware'] = 'firmware_stage'
    package_data['r2s_gen.firmware'] = [
        'core/include/r2s/*.h',
        'core/src/*.c',
        'ports/arduino/*.h',
        'ports/arduino/*.cpp',
        'examples/*/*.ino',
        'examples/*/*.h',
        'examples/*/*.c',
    ]

setup(
    name='r2s_gen',
    version=__version__,
    description='The ros2serial code generator',
    license='MIT',
    packages=packages,
    package_dir=package_dir,
    package_data=package_data,
    python_requires='>=3.10',
    data_files=[
        ('share/ament_index/resource_index/packages', ['resource/r2s_gen']),
        ('share/r2s_gen', ['package.xml']),
    ],
    zip_safe=False,
    entry_points={
        'console_scripts': ['r2s-gen = r2s_gen.cli:main', 'r2s-arduino = r2s_gen.arduino:main']
    },
)
