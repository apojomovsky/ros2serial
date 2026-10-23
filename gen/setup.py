"""The r2s-gen distribution: pip installs it, and colcon builds the same sources as an ament_python package.

The version comes from the package itself, where the release cut writes it, so
a wheel and a colcon build of one commit always agree.
"""

import sys
from pathlib import Path

from setuptools import find_packages, setup

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from r2s_gen.version import __version__  # noqa: E402

setup(
    name="r2s_gen",
    version=__version__,
    description="The ros2serial code generator",
    license="MIT",
    packages=find_packages(include=["r2s_gen", "r2s_gen.*"]),
    python_requires=">=3.10",
    data_files=[
        ("share/ament_index/resource_index/packages", ["resource/r2s_gen"]),
        ("share/r2s_gen", ["package.xml"]),
    ],
    zip_safe=False,
    entry_points={"console_scripts": ["r2s-gen = r2s_gen.cli:main"]},
)
