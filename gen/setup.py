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

packages = find_packages(include=['r2s_gen', 'r2s_gen.*'])
package_dir = {}
package_data = {'r2s_gen': ['snapshots/README.md', 'snapshots/*/*/msg/*.msg', 'schema/*.json']}
if CPP_STAGE.is_dir():
    packages.append('r2s_gen.cpp')
    package_dir['r2s_gen.cpp'] = 'cpp_stage'
    package_data['r2s_gen.cpp'] = ['include/**/*.h', 'include/**/*.hpp', 'src/*.cpp']

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
    entry_points={'console_scripts': ['r2s-gen = r2s_gen.cli:main']},
)
