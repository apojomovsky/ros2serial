from pathlib import Path

import pytest
import vectors_check

VALID = {
    "crc/check.yaml": """
covers: [FRM-5]
cases:
  - {text: "123456789", crc: 0x6F91}
  - {data: "00 01", crc: 0x1234}
""",
    "rcobs/basic.yaml": """
covers: [FRM-2]
cases:
  - {decoded: "", encoded: "01 00"}
  - {decoded: "11 22 00 33", encoded: "11 22 03 33 02 00"}
""",
    "varint/basic.yaml": """
covers: [ENC-6]
cases:
  - {value: 300, encoded: "ac 02"}
""",
    "frames/float32_topic16.yaml": """
name: float32_topic16
covers: [FRM-1]
topic_id: 16
seq: 0
payload: "00 00 c0 3f"
frame: "10 00 00 00 c0 3f 1e 49"
wire: "10 02 01 01 c0 3f 1e 49 05 00"
""",
    "messages/float32_1_5.yaml": """
name: float32_1_5
covers: [ENC-3]
type: std_msgs/msg/Float32
profile: {narrow_float64: false}
message: {data: 1.5}
payload: "00 00 c0 3f"
""",
    "session/handshake.yaml": """
name: handshake
covers: [SES-1]
steps:
  - {from: host, frame: "00 00 01 01"}
  - {from: mcu, frame: "00 00 02 01", at_ms: 3}
""",
    "time/wraparound.yaml": """
name: wraparound
covers: [TIM-2]
tick_hz: 1000000
cases:
  - {tick0: 4294967000, sec0: 10, nanosec0: 0, drift_ppm: 0, tick: 704, expect: {sec: 10, nanosec: 1000000}}
  - {tick0: 0, sec0: 0, nanosec0: 0, drift_ppm: 0, tick: 4000000000, expect: unsynced}
""",
}


def make_tree(root: Path, files: dict[str, str]) -> Path:
    for name, text in files.items():
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).write_text(text.lstrip())
    return root


def test_valid_files_of_every_kind_pass(tmp_path):
    assert vectors_check.check_tree(make_tree(tmp_path, VALID)) == []


def test_gitkeep_and_empty_kind_directories_are_ignored(tmp_path):
    make_tree(tmp_path, VALID)
    (tmp_path / "rcobs" / ".gitkeep").write_text("")
    (tmp_path / "session" / "extra").mkdir()
    assert vectors_check.check_tree(tmp_path) == []


@pytest.mark.parametrize(
    ("name", "text", "expected"),
    [
        ("rcobs/bad_hex.yaml", "covers: [FRM-2]\ncases:\n  - {decoded: '1g', encoded: '01 00'}\n", "cases/0/decoded"),
        ("rcobs/odd_hex.yaml", "covers: [FRM-2]\ncases:\n  - {decoded: '1 22', encoded: '01 00'}\n", "cases/0/decoded"),
        ("varint/missing.yaml", "covers: [ENC-6]\ncases:\n  - {value: 1}\n", "encoded"),
        ("varint/wrong_type.yaml", "covers: [ENC-6]\ncases:\n  - {value: '1', encoded: '01'}\n", "cases/0/value"),
        (
            "frames/big_id.yaml",
            VALID["frames/float32_topic16.yaml"].replace("topic_id: 16", "topic_id: 300"),
            "topic_id",
        ),
        ("messages/bad_type.yaml", VALID["messages/float32_1_5.yaml"].replace("std_msgs/msg/", "std_msgs/"), "type"),
        (
            "session/bad_from.yaml",
            "name: x\ncovers: [SES-1]\nsteps:\n  - {from: bridge, frame: '00'}\n",
            "steps/0/from",
        ),
        ("crc/both.yaml", "covers: [FRM-5]\ncases:\n  - {text: 'a', data: '61', crc: 1}\n", "cases/0"),
        ("crc/no_covers.yaml", "cases:\n  - {text: 'a', crc: 1}\n", "covers"),
        ("crc/bad_id.yaml", "covers: [frm-5]\ncases:\n  - {text: 'a', crc: 1}\n", "covers/0"),
        ("crc/extra_key.yaml", "covers: [FRM-5]\nnote: x\ncases:\n  - {text: 'a', crc: 1}\n", "note"),
        ("unknown/thing.yaml", "covers: [X-1]\n", "unknown vector kind"),
        ("rcobs/basic.yml", "covers: [FRM-2]\ncases: []\n", "must end in .yaml"),
        ("rcobs/broken.yaml", "covers: [FRM-2\n", "not valid YAML"),
    ],
)
def test_invalid_files_name_the_file_and_the_key(tmp_path, name, text, expected):
    make_tree(tmp_path, VALID)
    make_tree(tmp_path, {name: text})
    problems = vectors_check.check_tree(tmp_path)
    assert len(problems) == 1, problems
    assert problems[0].startswith(name), problems
    assert expected in problems[0], problems


def test_repository_vectors_pass():
    root = Path(vectors_check.__file__).resolve().parent.parent / "spec" / "vectors"
    assert vectors_check.check_tree(root) == []


def test_rejected_encodings_name_the_rule_that_rejects_them(tmp_path):
    files = {
        "rcobs/invalid.yaml": "covers: [COB-5]\ncases:\n  - {encoded: '05 00', reject: COB-5}\n",
        "varint/invalid.yaml": "covers: [VAR-3]\ncases:\n  - {encoded: '80', reject: VAR-3}\n",
    }
    assert vectors_check.check_tree(make_tree(tmp_path, files)) == []


@pytest.mark.parametrize(
    ("name", "text"),
    [
        ("rcobs/both.yaml", "covers: [COB-5]\ncases:\n  - {decoded: '', encoded: '05 00', reject: COB-5}\n"),
        ("rcobs/bad_rule.yaml", "covers: [COB-5]\ncases:\n  - {encoded: '05 00', reject: cob5}\n"),
        ("varint/both.yaml", "covers: [VAR-3]\ncases:\n  - {value: 1, encoded: '80', reject: VAR-3}\n"),
    ],
)
def test_a_rejected_case_carries_no_decoded_value_and_a_real_rule(tmp_path, name, text):
    assert vectors_check.check_tree(make_tree(tmp_path, {name: text})) != []


def test_a_bad_field_in_a_reject_case_is_reported_once(tmp_path):
    files = {"rcobs/bad.yaml": "covers: [COB-5]\ncases:\n  - {encoded: 'zz 00', reject: COB-5}\n"}
    problems = vectors_check.check_tree(make_tree(tmp_path, files))
    assert len(problems) == 1 and "cases/0/encoded" in problems[0], problems


def test_a_reject_rule_must_be_listed_in_covers(tmp_path):
    files = {"rcobs/invalid.yaml": "covers: [COB-5]\ncases:\n  - {encoded: '05 00', reject: COB-6}\n"}
    assert vectors_check.check_tree(make_tree(tmp_path, files)) == [
        "rcobs/invalid.yaml: cases/0/reject: COB-6 is not in the file's covers"
    ]
