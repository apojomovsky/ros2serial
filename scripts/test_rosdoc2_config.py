import xml.etree.ElementTree as ET
from pathlib import Path

import doxygen_check
import yaml

ROOT = Path(__file__).resolve().parent.parent
ROS_PACKAGES = [
    'bridge/r2s_bridge',
    'bridge/r2s_bridge_tests',
    'gen',
    'gen/r2s_rosidl',
]


def _doxyfile_settings(path: Path) -> dict:
    settings = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line.startswith('#') and '=' in line and not line.startswith('@'):
            key, _, value = line.partition('=')
            settings[key.strip()] = value.strip().strip('"')
    return settings


def test_core_doxyfile_warns_on_undocumented_public_symbols():
    settings = _doxyfile_settings(ROOT / 'core/Doxyfile')
    assert settings['PROJECT_NAME'] == 'r2s_core'
    assert settings['INPUT'] == './include'
    assert settings['WARN_IF_UNDOCUMENTED'] == 'YES'


def test_bridge_doxyfile_warns_on_undocumented_public_symbols():
    settings = _doxyfile_settings(ROOT / 'bridge/r2s_bridge/Doxyfile')
    assert settings['PROJECT_NAME'] == 'r2s_bridge'
    assert settings['INPUT'] == './include'
    assert settings['WARN_IF_UNDOCUMENTED'] == 'YES'


def test_each_package_exports_its_rosdoc2_config():
    for package in ROS_PACKAGES:
        root = ROOT / package
        assert (root / 'rosdoc2.yaml').is_file(), package
        assert (root / 'doc/conf.py').is_file(), package
        assert (root / 'doc/index.rst').is_file(), package
        docs = list(yaml.safe_load_all((root / 'rosdoc2.yaml').read_text()))
        assert docs[0]['type'] == 'rosdoc2 config', package
        assert 'builders' in docs[1], package
        export = ET.parse(root / 'package.xml').getroot().find('./export/rosdoc2')
        assert export is not None and export.text == 'rosdoc2.yaml', package


def test_quality_declaration_names_level_four():
    text = (ROOT / 'QUALITY_DECLARATION.md').read_text()
    assert 'Quality Level 4' in text
    assert 'REP-2004' in text


def test_core_doc_comments_pass_the_doxygen_gate():
    assert doxygen_check.check_headers([ROOT / 'core/include']) == []


def test_make_target_and_advisory_job_run_the_doc_build():
    assert 'scripts/rosdoc2.sh' in (ROOT / 'Makefile').read_text()
    assert 'rosdoc2 build --package-path' in (ROOT / 'scripts/rosdoc2.sh').read_text()
    workflow = (ROOT / '.github/workflows/rosdoc2.yml').read_text()
    assert 'scripts/rosdoc2.sh' in workflow
    assert 'continue-on-error: true' in workflow
