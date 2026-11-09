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

"""The posix firmware builds from generated code: regenerating its sources
from its configuration reproduces the checked-in pair byte for byte."""

from pathlib import Path

from r2s_gen import bounds, config, msgs, topics

POSIX = Path(__file__).resolve().parents[2] / "ports" / "posix"
SRC = POSIX / "src"


def test_the_checked_in_sources_match_the_emitter():
    loaded = config.load(POSIX / "r2s.yaml")
    repository = msgs.MessageRepository()
    roots = [topic.type_name for topic in loaded.topics]
    resolved = bounds.resolve(repository, roots, bounds=dict(loaded.bounds))
    header, source = topics.generate(loaded, resolved, repository)

    assert header == (SRC / "r2s_generated.h").read_text(encoding="utf-8")
    assert source == (SRC / "r2s_generated.c").read_text(encoding="utf-8")
