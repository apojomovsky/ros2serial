# Copyright (c) 2026 Alexis Pojomovsky
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

"""The r2s-gen command line: the version command, and the refusal of unknown ones."""

from r2s_gen import cli
from r2s_gen.version import PROTOCOL_VERSION
from r2s_gen.version import __version__


def test_version_prints_the_generator_version(capsys):
    assert cli.main(['version']) == 0
    assert f'r2s-gen {__version__}' in capsys.readouterr().out


def test_version_prints_the_protocol_version(capsys):
    assert cli.main(['version']) == 0
    assert f'protocol version {PROTOCOL_VERSION}' in capsys.readouterr().out


def test_an_unknown_subcommand_names_itself_and_exits_with_the_usage_code(capsys):
    assert cli.main(['frobnicate']) == 2
    assert 'frobnicate' in capsys.readouterr().err


def test_no_subcommand_exits_with_the_usage_code(capsys):
    assert cli.main([]) == 2
    assert 'usage' in capsys.readouterr().err.lower()
