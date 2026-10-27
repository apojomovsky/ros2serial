"""The r2s-gen command line: the version command, and the refusal of unknown ones."""

from r2s_gen import cli
from r2s_gen.version import PROTOCOL_VERSION, __version__


def test_version_prints_the_generator_version(capsys):
    assert cli.main(["version"]) == 0
    assert f"r2s-gen {__version__}" in capsys.readouterr().out


def test_version_prints_the_protocol_version(capsys):
    assert cli.main(["version"]) == 0
    assert f"protocol version {PROTOCOL_VERSION}" in capsys.readouterr().out


def test_an_unknown_subcommand_names_itself_and_exits_with_the_usage_code(capsys):
    assert cli.main(["frobnicate"]) == 2
    assert "frobnicate" in capsys.readouterr().err


def test_no_subcommand_exits_with_the_usage_code(capsys):
    assert cli.main([]) == 2
    assert "usage" in capsys.readouterr().err.lower()
