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
