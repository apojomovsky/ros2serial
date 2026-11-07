"""The versions r2s-gen reports: the repository's, and the one the wire speaks."""

import re
import sys
from pathlib import Path

import pytest
from r2s_gen import bundle
from r2s_gen.version import PROTOCOL_VERSION, __version__

ROOT = Path(__file__).resolve().parents[2]
SPEC = ROOT / "spec" / "protocol.md"


def _capture(pattern: str, text: str) -> str:
    found = re.search(pattern, text)
    assert found, f"no match for {pattern!r}"
    return found.group(1)


def test_the_generator_version_is_the_repository_version():
    assert __version__ == (ROOT / "VERSION").read_text(encoding="utf-8").strip()


def test_every_ros_manifest_carries_the_repository_version():
    # Installed copies are build outputs, not sources: the walkthrough builds
    # in-tree, so its install tree carries manifests with older versions.
    manifests = sorted(
        path for path in ROOT.rglob("package.xml") if path.relative_to(ROOT).parts[0] not in ("build", "install", "log")
    )
    assert len(manifests) >= 3, manifests
    for manifest in manifests:
        assert _capture(r"<version>([^<]+)</version>", manifest.read_text(encoding="utf-8")) == __version__, manifest


def test_the_bundled_library_version_is_the_repository_version():
    properties = bundle.bundle()["library.properties"]
    assert _capture(r"\nversion=([^\n]+)", properties) == __version__


def test_the_example_library_version_is_the_repository_version():
    properties = (ROOT / "gen" / "tests" / "golden" / "layouts" / "library.properties").read_text(encoding="utf-8")
    assert _capture(r"\nversion=([^\n]+)", properties) == __version__


def test_the_protocol_version_is_the_one_the_core_announces():
    header = (ROOT / "core" / "include" / "r2s" / "r2s.h").read_text(encoding="utf-8")
    assert str(PROTOCOL_VERSION) == _capture(r"#define R2S_PROTOCOL_VERSION (\d+)", header)


# covers: GEN-2
def _spec_text() -> str:
    try:
        return SPEC.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as e:
        pytest.fail(f"spec/protocol.md: unreadable ({str(e).splitlines()[0]})")


def test_the_protocol_version_is_the_one_the_spec_describes():
    assert str(PROTOCOL_VERSION) == _capture(r"The protocol version described here is (\d+)", _spec_text())


def test_the_readme_names_the_protocol_version_the_wire_speaks():
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    assert f"protocol v{PROTOCOL_VERSION}" in readme, (
        "README.md states the wire protocol version; it must match PROTOCOL_VERSION"
    )


def test_a_spec_that_is_not_utf8_fails(tmp_path, monkeypatch):
    bad = tmp_path / "protocol.md"
    bad.write_bytes(b"The protocol version described here is 1\n\xff")
    monkeypatch.setattr(sys.modules[__name__], "SPEC", bad)
    with pytest.raises(pytest.fail.Exception, match=r"spec/protocol\.md"):
        test_the_protocol_version_is_the_one_the_spec_describes()
