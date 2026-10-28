"""The versions r2s-gen reports: the repository's, and the one the wire speaks."""

import re
from pathlib import Path

from r2s_gen import bundle
from r2s_gen.version import PROTOCOL_VERSION, __version__

ROOT = Path(__file__).resolve().parents[2]


def _capture(pattern: str, text: str) -> str:
    found = re.search(pattern, text)
    assert found, f"no match for {pattern!r}"
    return found.group(1)


def test_the_generator_version_is_the_repository_version():
    assert __version__ == (ROOT / "VERSION").read_text(encoding="utf-8").strip()


def test_every_ros_manifest_carries_the_repository_version():
    manifests = sorted(ROOT.rglob("package.xml"))
    assert len(manifests) >= 3, manifests
    for manifest in manifests:
        assert _capture(r"<version>([^<]+)</version>", manifest.read_text(encoding="utf-8")) == __version__, manifest


def test_the_bundled_library_version_is_the_repository_version():
    properties = bundle.bundle()["library.properties"]
    assert _capture(r"\nversion=([^\n]+)", properties) == __version__


def test_the_example_library_version_is_the_repository_version():
    properties = (ROOT / "examples" / "arduino_minimal" / "lib" / "library.properties").read_text(encoding="utf-8")
    assert _capture(r"\nversion=([^\n]+)", properties) == __version__


def test_the_protocol_version_is_the_one_the_core_announces():
    header = (ROOT / "core" / "include" / "r2s" / "r2s.h").read_text(encoding="utf-8")
    assert str(PROTOCOL_VERSION) == _capture(r"#define R2S_PROTOCOL_VERSION (\d+)", header)


# covers: GEN-2
def test_the_protocol_version_is_the_one_the_spec_describes():
    spec = (ROOT / "spec" / "protocol.md").read_text(encoding="utf-8")
    assert str(PROTOCOL_VERSION) == _capture(r"The protocol version described here is (\d+)", spec)
