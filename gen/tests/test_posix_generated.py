"""The posix firmware builds from generated code: regenerating its sources
from its configuration reproduces the checked-in pair byte for byte."""

import os
from pathlib import Path

from r2s_gen import bounds
from r2s_gen import config
from r2s_gen import msgs
from r2s_gen import services
from r2s_gen import topics

POSIX = Path(__file__).resolve().parents[2] / 'ports' / 'posix'
SRC = POSIX / 'src'


def test_the_checked_in_sources_match_the_emitter():
    loaded = config.load(POSIX / 'r2s.yaml')
    prefixes = [
        Path(entry) for entry in os.environ.get('AMENT_PREFIX_PATH', '').split(os.pathsep) if entry
    ]
    repository = msgs.MessageRepository(ament_prefix_path=prefixes, distro=loaded.distro)
    roots = [topic.type_name for topic in loaded.topics] + services.roots(loaded)
    resolved = bounds.resolve(repository, roots, bounds=dict(loaded.bounds))
    header, source = topics.generate(loaded, resolved, repository)

    assert header == (SRC / 'r2s_generated.h').read_text(encoding='utf-8')
    assert source == (SRC / 'r2s_generated.c').read_text(encoding='utf-8')
