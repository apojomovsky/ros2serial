from pathlib import Path

import pytest
import vectors_check
from r2s_gen import msgs
from r2s_gen.reference import messages

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
  - {from: host, frame: "00 00 01 01 70 0b"}
  - {from: mcu, frame: "00 00 02 01 18 21", at_ms: 3}
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
            "name: x\ncovers: [SES-1]\nsteps:\n  - {from: bridge, frame: '00 00 02 01 18 21'}\n",
            "steps/0/from",
        ),
        # A step carries a frame with its check, so a frame without one that
        # leaves no zero residue is reported on its step.
        (
            "session/bad_check.yaml",
            "name: x\ncovers: [SES-1]\nsteps:\n  - {from: mcu, frame: '00 00 02 01 00 00'}\n",
            "steps/0/frame",
        ),
        (
            "session/short_frame.yaml",
            "name: x\ncovers: [SES-1]\nsteps:\n  - {from: mcu, frame: '00 00'}\n",
            "shorter than 4 bytes",
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


def test_a_frame_vector_must_carry_the_bytes_the_reference_makes(tmp_path):
    files = {
        "frames/wrong.yaml": """
name: wrong
covers: [FRM-1]
topic_id: 16
seq: 0
payload: "00 00 c0 3f"
frame: "10 00 00 00 c0 3f 1e 48"
wire: "10 02 01 01 c0 3f 1e 48 05 00"
"""
    }
    problems = vectors_check.check_tree(make_tree(tmp_path, files))
    assert any("not the frame the reference makes" in p for p in problems), problems
    assert any("not the bytes the reference makes" in p for p in problems), problems


def test_a_time_vector_must_carry_what_the_reference_makes(tmp_path):
    files = {
        "time/off_by_one.yaml": """
name: off_by_one
covers: [TIM-6]
tick_hz: 1000
cases:
  - {tick0: 0, sec0: 0, nanosec0: 0, drift_ppm: 0, tick: 1000, expect: {sec: 1, nanosec: 1}}
"""
    }
    problems = vectors_check.check_tree(make_tree(tmp_path, files))
    assert problems == ["time/off_by_one.yaml/cases/0/expect: the reference makes sec=1 nanosec=0"], problems


def test_a_time_vector_with_a_tick_hz_the_spec_refuses_is_reported(tmp_path):
    files = {
        "time/bad_hz.yaml": """
name: bad_hz
covers: [TIM-6]
tick_hz: 1500
cases:
  - {tick0: 0, sec0: 0, nanosec0: 0, drift_ppm: 0, tick: 0, expect: {sec: 0, nanosec: 0}}
"""
    }
    problems = vectors_check.check_tree(make_tree(tmp_path, files))
    assert len(problems) == 1, problems
    assert problems[0].startswith("time/bad_hz.yaml/cases/0: CTL-9"), problems


def test_a_time_vector_with_a_malformed_expect_is_reported_not_raised(tmp_path):
    files = {
        "time/bad_expect.yaml": """
name: bad_expect
covers: [TIM-6]
tick_hz: 1000
cases:
  - {tick0: 0, sec0: 0, nanosec0: 0, drift_ppm: 0, tick: 0}
  - {tick0: 0, sec0: 0, nanosec0: 0, drift_ppm: 0, tick: 0, expect: soon}
"""
    }
    problems = vectors_check.check_tree(make_tree(tmp_path, files))
    made = "the reference makes sec=0 nanosec=0"
    assert [p for p in problems if made in p] == [
        "time/bad_expect.yaml/cases/0/expect: the reference makes sec=0 nanosec=0",
        "time/bad_expect.yaml/cases/1/expect: the reference makes sec=0 nanosec=0",
    ], problems


def test_a_frame_vector_must_carry_a_payload_the_layout_fits(tmp_path):
    """A length that does not match the name it precedes is caught."""
    files = {
        "frames/bad_length.yaml": """
name: topic
covers: [FRM-1]
topic_id: 0
seq: 0
payload: "04 00 10 00 00 01 02 03 04 05 06 07 16"
frame: "00 00 04 00 10 00 00 01 02 03 04 05 06 07 16 00 00"
wire: "01 00 04 02 10 02 01 01 02 03 04 05 06 07 16 00 00 00 00 00 00"
"""
    }
    problems = vectors_check.check_tree(make_tree(tmp_path, files))
    assert any("payload" in p for p in problems), problems


def test_a_layout_is_chosen_by_the_opcode_not_by_the_vector_name(tmp_path):
    """A vector whose name says nothing still gets its layout checked."""
    files = {
        "frames/unnamed.yaml": """
name: something_else
covers: [FRM-1]
topic_id: 0
seq: 0
payload: "04 00 10 00 00 01 02 03 04 05 06 07 16"
frame: "00 00 04 00 10 00 00 01 02 03 04 05 06 07 16 00 00"
wire: "01 00 04 02 10 02 01 01 02 03 04 05 06 07 16 00 00 00 00 00 00"
"""
    }
    problems = vectors_check.check_tree(make_tree(tmp_path, files))
    assert any("payload" in p for p in problems), problems


def test_an_opcode_the_spec_does_not_give_is_reported(tmp_path):
    files = {
        "frames/bad_opcode.yaml": """
name: hello
covers: [FRM-1]
topic_id: 0
seq: 0
payload: "07 01"
frame: "00 00 07 01 00 00"
wire: "01 00 07 01 00 00 00 00"
"""
    }
    problems = vectors_check.check_tree(make_tree(tmp_path, files))
    assert any("the opcode is not one the spec gives" in p for p in problems), problems


def test_a_frame_on_the_wrong_channel_is_reported(tmp_path):
    """A PING on the control channel is not a PING."""
    files = {
        "frames/wrong_channel.yaml": """
name: ping
covers: [FRM-1]
topic_id: 0
seq: 0
payload: "01 2a"
frame: "00 00 01 2a 00 00"
wire: "01 00 01 2a 00 00 00 00"
"""
    }
    problems = vectors_check.check_tree(make_tree(tmp_path, files))
    assert any("is not the channel 1 of a ping frame" in p for p in problems), problems


def test_a_payload_that_is_not_hex_is_reported(tmp_path):
    """A payload the schema accepts but the bytes do not is not silent."""
    files = {
        "frames/bad_hex.yaml": """
name: topic
covers: [FRM-1]
topic_id: 0
seq: 0
payload: "04 00 1g"
frame: "00 00 04 00 10 00 00 01 02 03 04 05 06 07 16 00 00"
wire: "01 00 04 02 10 02 01 01 02 03 04 05 06 07 16 00 00 00 00 00 00"
"""
    }
    problems = vectors_check.check_tree(make_tree(tmp_path, files))
    assert any("not hexadecimal bytes" in p for p in problems), problems


def test_a_log_frame_with_a_reserved_value_kind_is_reported(tmp_path):
    """A frame on a channel with no vector yet is still checked."""
    files = {
        "frames/log.yaml": """
name: log
covers: [FRM-1]
topic_id: 2
seq: 0
payload: "1e 02 68 69 09"
frame: "02 00 1e 02 68 69 09 00 00"
wire: "02 00 1e 02 68 69 09 00 00 00 00 00 00"
"""
    }
    problems = vectors_check.check_tree(make_tree(tmp_path, files))
    assert any("the value kind is not one the spec gives" in p for p in problems), problems


def test_a_compact_handshake_carries_a_hash_where_the_full_one_carries_a_name(tmp_path):
    """A compact HELLO and TOPIC parse, and a full one does too."""
    files = {
        "frames/compact_hello.yaml": """
name: hello
covers: [FRM-1]
topic_id: 0
seq: 0
payload: "02 02 01 11 22 33 44 00 04 80 00 e8 03 00 00 11 01 6e 6f 64 65"
frame: "00 00 02 02 01 11 22 33 44 00 04 80 00 e8 03 00 00 11 01 6e 6f 64 65 57 86"
wire: "01 01 02 02 01 11 22 33 44 08 04 80 03 e8 03 03 01 11 01 6e 6f 64 65 57 86 09 00"
""",
        "frames/compact_topic.yaml": """
name: topic_compact
covers: [FRM-1]
topic_id: 0
seq: 1
payload: "04 00 10 03 00 01 02 03 04 05 06 07 04 61 62 63 64 12 34 56 78"
frame: "00 01 04 00 10 03 00 01 02 03 04 05 06 07 04 61 62 63 64 12 34 56 78 2a c7"
wire: "01 01 04 03 10 03 03 01 02 03 04 05 06 07 04 61 62 63 64 12 34 56 78 2a c7 13 00"
""",
    }
    problems = vectors_check.check_tree(make_tree(tmp_path, files))
    assert not any("payload" in p for p in problems), problems


def test_a_correction_whose_drift_is_outside_the_limit_is_reported(tmp_path):
    files = {
        "frames/correction.yaml": """
name: correction
covers: [FRM-1]
topic_id: 1
seq: 0
payload: "03 00 00 00 00 00 00 00 00 00 00 00 00 30 75"
frame: "01 00 03 00 00 00 00 00 00 00 00 00 00 00 30 75 00 00"
wire: "01 00 03 02 00 02 00 00 00 00 00 00 00 00 00 30 75 00 00 00 00 00 00"
"""
    }
    problems = vectors_check.check_tree(make_tree(tmp_path, files))
    assert any("the drift is outside 20000 ppm" in p for p in problems), problems


def test_a_correction_whose_nanosec_is_too_large_is_reported(tmp_path):
    files = {
        "frames/correction.yaml": """
name: correction
covers: [FRM-1]
topic_id: 1
seq: 0
payload: "03 00 00 00 00 00 00 00 00 00 94 35 77 00 00"
frame: "01 00 03 00 00 00 00 00 00 00 00 00 00 00 94 35 77 00 00 00 00"
wire: "01 00 03 02 00 02 00 00 00 00 00 00 00 00 00 94 35 77 00 00 00 00 00 00"
"""
    }
    problems = vectors_check.check_tree(make_tree(tmp_path, files))
    assert any("nanosec is not below 10^9" in p for p in problems), problems


def test_a_message_vector_must_carry_the_payload_the_reference_makes(tmp_path):
    files = {
        "messages/wrong.yaml": """
name: wrong
covers: [ENC-3]
type: std_msgs/msg/Float32
profile: {narrow_float64: false}
message: {data: 1.5}
payload: "00 00 00 00"
"""
    }
    problems = vectors_check.check_tree(make_tree(tmp_path, files))
    assert problems == ["messages/wrong.yaml/payload: not the payload the reference makes"]


def test_a_message_vector_on_an_unknown_type_is_reported(tmp_path):
    files = {
        "messages/unknown.yaml": """
name: unknown
covers: [ENC-3]
type: demo_msgs/msg/Missing
profile: {narrow_float64: false}
message: {data: 1.5}
payload: "00 00 c0 3f"
"""
    }
    problems = vectors_check.check_tree(make_tree(tmp_path, files))
    assert len(problems) == 1, problems
    assert problems[0].startswith("messages/unknown.yaml/message: "), problems
    assert "no message definition found" in problems[0], problems


@pytest.mark.parametrize(
    ("name", "text"),
    [
        ("messages/missing_message.yaml", "name: x\ncovers: [ENC-3]\ntype: std_msgs/msg/Float32\n"),
        ("messages/null_message.yaml", "name: x\ncovers: [ENC-3]\ntype: std_msgs/msg/Float32\nmessage: null\n"),
        ("messages/number_message.yaml", "name: x\ncovers: [ENC-3]\ntype: std_msgs/msg/Float32\nmessage: 5\n"),
        ("messages/number_type.yaml", "name: x\ncovers: [ENC-3]\ntype: 5\nmessage: {data: 1.5}\n"),
        (
            "messages/huge_number.yaml",
            f"name: x\ncovers: [ENC-3]\ntype: std_msgs/msg/Float32\nmessage: {{data: {'1' * 400}}}\n",
        ),
    ],
)
def test_a_malformed_message_vector_is_reported_not_a_crash(tmp_path, name, text):
    vector = text + 'profile: {narrow_float64: false}\npayload: ""\n'
    problems = vectors_check.check_tree(make_tree(tmp_path, {name: vector}))
    assert problems, (name, text)
    assert all(problem.startswith(name) for problem in problems), problems


def test_a_float32_vector_number_must_be_exactly_representable(tmp_path):
    files = {
        "messages/inexact.yaml": """
name: inexact
covers: [ENC-3]
type: std_msgs/msg/Float32
profile: {narrow_float64: false}
message: {data: 1.1}
payload: "cd cc 8c 3f"
"""
    }
    assert vectors_check.check_tree(make_tree(tmp_path, files)) == [
        "messages/inexact.yaml/message: std_msgs/msg/Float32.data must be exactly representable in binary32"
    ]


def test_exact_float32_rule_applies_inside_nested_arrays(tmp_path, monkeypatch):
    definitions = tmp_path / "definitions" / "demo_msgs" / "msg"
    definitions.mkdir(parents=True)
    (definitions / "Outer.msg").write_text("Inner inner\n", encoding="utf-8")
    (definitions / "Inner.msg").write_text("float32[2] pair\n", encoding="utf-8")
    files = {
        "messages/nested.yaml": """
name: nested
covers: [ENC-3]
type: demo_msgs/msg/Outer
profile: {narrow_float64: false}
message: {inner: {pair: [1.5, 1.1]}}
payload: "00 00 c0 3f cd cc 8c 3f"
"""
    }
    repository = msgs.MessageRepository(msg_paths=[tmp_path / "definitions"])
    monkeypatch.setattr(vectors_check, "_MESSAGES", repository)
    vectors = make_tree(tmp_path / "vectors", files)
    assert vectors_check.check_tree(vectors) == [
        "messages/nested.yaml/message: demo_msgs/msg/Outer.inner.demo_msgs/msg/Inner.pair[1] must be exactly representable in binary32"
    ]


def test_an_out_of_range_float32_does_not_hide_an_inexact_sibling(tmp_path, monkeypatch):
    definitions = tmp_path / "definitions" / "demo_msgs" / "msg"
    definitions.mkdir(parents=True)
    (definitions / "Two.msg").write_text("float32 first\nfloat32 second\n", encoding="utf-8")
    files = {
        "messages/mixed.yaml": """
name: mixed
covers: [ENC-3]
type: demo_msgs/msg/Two
profile: {narrow_float64: false}
message: {first: 1.1, second: 1.0e+300}
payload: ""
"""
    }
    repository = msgs.MessageRepository(msg_paths=[tmp_path / "definitions"])
    monkeypatch.setattr(vectors_check, "_MESSAGES", repository)
    vectors = make_tree(tmp_path / "vectors", files)
    assert vectors_check.check_tree(vectors) == [
        "messages/mixed.yaml/message: demo_msgs/msg/Two.first must be exactly representable in binary32",
        "messages/mixed.yaml/message: demo_msgs/msg/Two.second must be exactly representable in binary32",
        "messages/mixed.yaml/message: the reference does not make it: "
        "demo_msgs/msg/Two.second: 1e+300 does not fit binary32",
    ]


def test_an_aborted_exact_walk_keeps_the_findings_it_already_made(tmp_path, monkeypatch):
    definitions = tmp_path / "definitions" / "demo_msgs" / "msg"
    definitions.mkdir(parents=True)
    (definitions / "Two.msg").write_text("float32 first\nfloat32 second\nfloat32 third\n", encoding="utf-8")
    files = {
        "messages/abort.yaml": """
name: abort
covers: [ENC-3]
type: demo_msgs/msg/Two
profile: {narrow_float64: false}
message: {first: 1.1, second: 1.2}
payload: ""
"""
    }
    repository = msgs.MessageRepository(msg_paths=[tmp_path / "definitions"])
    monkeypatch.setattr(vectors_check, "_MESSAGES", repository)
    vectors = make_tree(tmp_path / "vectors", files)
    assert vectors_check.check_tree(vectors) == [
        "messages/abort.yaml/message: demo_msgs/msg/Two.first must be exactly representable in binary32",
        "messages/abort.yaml/message: demo_msgs/msg/Two.second must be exactly representable in binary32",
        "messages/abort.yaml/message: the reference does not make it: demo_msgs/msg/Two.third: no value",
    ]


def test_a_cyclic_exact_walk_reports_each_distinct_finding_once(tmp_path, monkeypatch):
    definitions = tmp_path / "definitions" / "demo_msgs" / "msg"
    definitions.mkdir(parents=True)
    (definitions / "A.msg").write_text("float32 data\nA child\n", encoding="utf-8")
    files = {
        "messages/flood.yaml": """
name: flood
covers: [ENC-3]
type: demo_msgs/msg/A
profile: {narrow_float64: false}
message: &x
  data: 1.1
  child: *x
payload: ""
"""
    }
    repository = msgs.MessageRepository(msg_paths=[tmp_path / "definitions"])
    monkeypatch.setattr(vectors_check, "_MESSAGES", repository)
    vectors = make_tree(tmp_path / "vectors", files)
    assert vectors_check.check_tree(vectors) == [
        "messages/flood.yaml: cyclic YAML alias",
        "messages/flood.yaml/message: demo_msgs/msg/A.data must be exactly representable in binary32",
        "messages/flood.yaml/message: the reference does not make it: maximum recursion depth exceeded",
    ]


def test_a_cyclic_array_walk_reports_each_distinct_finding_once(tmp_path, monkeypatch):
    definitions = tmp_path / "definitions" / "demo_msgs" / "msg"
    definitions.mkdir(parents=True)
    (definitions / "A.msg").write_text("float32 data\nA[] kids\n", encoding="utf-8")
    files = {
        "messages/flood_array.yaml": """
name: flood_array
covers: [ENC-3]
type: demo_msgs/msg/A
profile: {narrow_float64: false}
message: &x
  data: 1.1
  kids: [*x]
payload: ""
"""
    }
    repository = msgs.MessageRepository(msg_paths=[tmp_path / "definitions"])
    monkeypatch.setattr(vectors_check, "_MESSAGES", repository)
    vectors = make_tree(tmp_path / "vectors", files)
    assert vectors_check.check_tree(vectors) == [
        "messages/flood_array.yaml: cyclic YAML alias",
        "messages/flood_array.yaml/message: demo_msgs/msg/A.data must be exactly representable in binary32",
        "messages/flood_array.yaml/message: the reference does not make it: maximum recursion depth exceeded",
    ]


def test_a_self_listed_array_keeps_later_findings(tmp_path, monkeypatch):
    definitions = tmp_path / "definitions" / "demo_msgs" / "msg"
    definitions.mkdir(parents=True)
    (definitions / "A.msg").write_text("A[] kids\nfloat32 data\n", encoding="utf-8")
    files = {
        "messages/flood_list.yaml": """
name: flood_list
covers: [ENC-3]
type: demo_msgs/msg/A
profile: {narrow_float64: false}
message: {kids: &L [*L], data: 1.1}
payload: ""
"""
    }
    repository = msgs.MessageRepository(msg_paths=[tmp_path / "definitions"])
    monkeypatch.setattr(vectors_check, "_MESSAGES", repository)
    vectors = make_tree(tmp_path / "vectors", files)
    assert vectors_check.check_tree(vectors) == [
        "messages/flood_list.yaml: cyclic YAML alias",
        "messages/flood_list.yaml/message: demo_msgs/msg/A.data must be exactly representable in binary32",
        "messages/flood_list.yaml/message: the reference does not make it: "
        "demo_msgs/msg/A.kids[0]: expected a mapping of demo_msgs/msg/A's fields",
    ]


def test_twin_fields_of_one_type_keep_both_findings_on_abort(tmp_path, monkeypatch):
    definitions = tmp_path / "definitions" / "demo_msgs" / "msg"
    definitions.mkdir(parents=True)
    (definitions / "Inner.msg").write_text("float32 data\n", encoding="utf-8")
    (definitions / "Outer.msg").write_text("Inner a\nInner b\nfloat32 c\n", encoding="utf-8")
    files = {
        "messages/twins.yaml": """
name: twins
covers: [ENC-3]
type: demo_msgs/msg/Outer
profile: {narrow_float64: false}
message: {a: {data: 1.1}, b: {data: 1.2}}
payload: ""
"""
    }
    repository = msgs.MessageRepository(msg_paths=[tmp_path / "definitions"])
    monkeypatch.setattr(vectors_check, "_MESSAGES", repository)
    vectors = make_tree(tmp_path / "vectors", files)
    assert vectors_check.check_tree(vectors) == [
        "messages/twins.yaml/message: demo_msgs/msg/Outer.a.demo_msgs/msg/Inner.data must be exactly representable in binary32",
        "messages/twins.yaml/message: demo_msgs/msg/Outer.b.demo_msgs/msg/Inner.data must be exactly representable in binary32",
        "messages/twins.yaml/message: the reference does not make it: demo_msgs/msg/Outer.c: no value",
    ]


def test_nested_message_array_elements_name_their_positions(tmp_path, monkeypatch):
    definitions = tmp_path / "definitions" / "demo_msgs" / "msg"
    definitions.mkdir(parents=True)
    (definitions / "Single.msg").write_text("float32 data\n", encoding="utf-8")
    (definitions / "Arr.msg").write_text("Single[] items\n", encoding="utf-8")
    repository = msgs.MessageRepository(msg_paths=[tmp_path / "definitions"])
    monkeypatch.setattr(vectors_check, "_MESSAGES", repository)
    message = {"items": [{"data": 1.1}, {"data": 1.2}]}
    payload = messages.encode(repository, "demo_msgs/msg/Arr", message, narrow=False).hex(" ")
    files = {
        "messages/nested_array.yaml": f"""
name: nested_array
covers: [ENC-3]
type: demo_msgs/msg/Arr
profile: {{narrow_float64: false}}
message: {{items: [{{data: 1.1}}, {{data: 1.2}}]}}
payload: "{payload}"
"""
    }
    vectors = make_tree(tmp_path / "vectors", files)
    assert vectors_check.check_tree(vectors) == [
        "messages/nested_array.yaml/message: "
        "demo_msgs/msg/Arr.items[0].demo_msgs/msg/Single.data must be exactly representable in binary32",
        "messages/nested_array.yaml/message: "
        "demo_msgs/msg/Arr.items[1].demo_msgs/msg/Single.data must be exactly representable in binary32",
    ]


def test_repeated_types_with_finite_data_are_walked_in_full(tmp_path, monkeypatch):
    definitions = tmp_path / "definitions" / "demo_msgs" / "msg"
    definitions.mkdir(parents=True)
    (definitions / "A.msg").write_text("float32 d\nB b\n", encoding="utf-8")
    (definitions / "B.msg").write_text("float32 e\nA a\n", encoding="utf-8")
    files = {
        "messages/repeated.yaml": """
name: repeated
covers: [ENC-3]
type: demo_msgs/msg/A
profile: {narrow_float64: false}
message: {d: 1.1, b: {e: 1.2, a: {d: 1.3, b: {e: 1.4}}}}
payload: ""
"""
    }
    repository = msgs.MessageRepository(msg_paths=[tmp_path / "definitions"])
    monkeypatch.setattr(vectors_check, "_MESSAGES", repository)
    vectors = make_tree(tmp_path / "vectors", files)
    assert vectors_check.check_tree(vectors) == [
        "messages/repeated.yaml/message: demo_msgs/msg/A.d must be exactly representable in binary32",
        "messages/repeated.yaml/message: demo_msgs/msg/A.b.demo_msgs/msg/B.e must be exactly representable in binary32",
        "messages/repeated.yaml/message: demo_msgs/msg/A.b.demo_msgs/msg/B.a.demo_msgs/msg/A.d must be exactly representable in binary32",
        "messages/repeated.yaml/message: demo_msgs/msg/A.b.demo_msgs/msg/B.a.demo_msgs/msg/A.b.demo_msgs/msg/B.e must be exactly representable in binary32",
        "messages/repeated.yaml/message: the reference does not make it: demo_msgs/msg/B.a: no value",
    ]


def test_nested_array_positions_survive_a_plain_nested_parent(tmp_path, monkeypatch):
    definitions = tmp_path / "definitions" / "demo_msgs" / "msg"
    definitions.mkdir(parents=True)
    (definitions / "Single.msg").write_text("float32 data\n", encoding="utf-8")
    (definitions / "Inner.msg").write_text("Single[] singles\n", encoding="utf-8")
    (definitions / "Outer.msg").write_text("Inner mid\n", encoding="utf-8")
    repository = msgs.MessageRepository(msg_paths=[tmp_path / "definitions"])
    monkeypatch.setattr(vectors_check, "_MESSAGES", repository)
    message = {"mid": {"singles": [{"data": 1.1}, {"data": 1.2}]}}
    payload = messages.encode(repository, "demo_msgs/msg/Outer", message, narrow=False).hex(" ")
    files = {
        "messages/nested_plain.yaml": f"""
name: nested_plain
covers: [ENC-3]
type: demo_msgs/msg/Outer
profile: {{narrow_float64: false}}
message: {{mid: {{singles: [{{data: 1.1}}, {{data: 1.2}}]}}}}
payload: "{payload}"
"""
    }
    vectors = make_tree(tmp_path / "vectors", files)
    assert vectors_check.check_tree(vectors) == [
        "messages/nested_plain.yaml/message: "
        "demo_msgs/msg/Outer.mid.demo_msgs/msg/Inner.singles[0].demo_msgs/msg/Single.data must be exactly representable in binary32",
        "messages/nested_plain.yaml/message: "
        "demo_msgs/msg/Outer.mid.demo_msgs/msg/Inner.singles[1].demo_msgs/msg/Single.data must be exactly representable in binary32",
    ]


def test_plain_nested_twins_name_their_parents(tmp_path, monkeypatch):
    definitions = tmp_path / "definitions" / "demo_msgs" / "msg"
    definitions.mkdir(parents=True)
    (definitions / "S.msg").write_text("float32 d\n", encoding="utf-8")
    (definitions / "A.msg").write_text("S[] s\n", encoding="utf-8")
    (definitions / "O.msg").write_text("A x\nA y\n", encoding="utf-8")
    repository = msgs.MessageRepository(msg_paths=[tmp_path / "definitions"])
    monkeypatch.setattr(vectors_check, "_MESSAGES", repository)
    message = {"x": {"s": [{"d": 1.1}]}, "y": {"s": [{"d": 1.2}]}}
    payload = messages.encode(repository, "demo_msgs/msg/O", message, narrow=False).hex(" ")
    files = {
        "messages/plain_twins.yaml": f"""
name: plain_twins
covers: [ENC-3]
type: demo_msgs/msg/O
profile: {{narrow_float64: false}}
message: {{x: {{s: [{{d: 1.1}}]}}, y: {{s: [{{d: 1.2}}]}}}}
payload: "{payload}"
"""
    }
    vectors = make_tree(tmp_path / "vectors", files)
    assert vectors_check.check_tree(vectors) == [
        "messages/plain_twins.yaml/message: "
        "demo_msgs/msg/O.x.demo_msgs/msg/A.s[0].demo_msgs/msg/S.d must be exactly representable in binary32",
        "messages/plain_twins.yaml/message: "
        "demo_msgs/msg/O.y.demo_msgs/msg/A.s[0].demo_msgs/msg/S.d must be exactly representable in binary32",
    ]


def test_a_branching_alias_graph_reports_each_node_once(tmp_path, monkeypatch):
    definitions = tmp_path / "definitions" / "demo_msgs" / "msg"
    definitions.mkdir(parents=True)
    (definitions / "N.msg").write_text("float32 d\nN[] kids\n", encoding="utf-8")
    repository = msgs.MessageRepository(msg_paths=[tmp_path / "definitions"])
    monkeypatch.setattr(vectors_check, "_MESSAGES", repository)
    depth = 12
    level = {"d": 1.1, "kids": []}
    for _ in range(depth):
        level = {"d": 1.1, "kids": [level, level]}
    problems = vectors_check.check_float32_exact("messages/fanout.yaml", {"type": "demo_msgs/msg/N", "message": level})
    trail = "demo_msgs/msg/N"
    expected = [f"messages/fanout.yaml/message: {trail}.d must be exactly representable in binary32"]
    for _ in range(depth):
        trail += ".kids[0].demo_msgs/msg/N"
        expected.append(f"messages/fanout.yaml/message: {trail}.d must be exactly representable in binary32")
    assert problems == expected


def test_a_cyclic_alias_in_a_message_is_reported_not_a_crash(tmp_path, monkeypatch):
    definitions = tmp_path / "definitions" / "demo_msgs" / "msg"
    definitions.mkdir(parents=True)
    (definitions / "A.msg").write_text("B b\n", encoding="utf-8")
    (definitions / "B.msg").write_text("A a\n", encoding="utf-8")
    (definitions / "Single.msg").write_text("float32 data\n", encoding="utf-8")
    (definitions / "Arr.msg").write_text("Single[] items\n", encoding="utf-8")
    files = {
        "messages/cycle.yaml": """
name: cycle
covers: [ENC-3]
type: demo_msgs/msg/A
profile: {narrow_float64: false}
message: &x
  b: {a: *x}
payload: ""
""",
        "messages/cycle_covers.yaml": """
name: cycle_covers
covers: &c [ENC-3, *c]
type: demo_msgs/msg/Single
profile: {narrow_float64: false}
message: {data: 1.5}
payload: "00 00 c0 3f"
""",
        "messages/cycle_array.yaml": """
name: cycle_array
covers: [ENC-3]
type: demo_msgs/msg/Arr
profile: {narrow_float64: false}
message: &m
  items: [*m]
payload: ""
""",
    }
    repository = msgs.MessageRepository(msg_paths=[tmp_path / "definitions"])
    monkeypatch.setattr(vectors_check, "_MESSAGES", repository)
    vectors = make_tree(tmp_path / "vectors", files)
    problems = vectors_check.check_tree(vectors)
    assert problems == [
        "messages/cycle.yaml: cyclic YAML alias",
        "messages/cycle.yaml/message: the reference does not make it: maximum recursion depth exceeded",
        "messages/cycle_array.yaml: cyclic YAML alias",
        "messages/cycle_array.yaml/message: the reference does not make it: demo_msgs/msg/Single.items: unknown field",
        "messages/cycle_covers.yaml: cyclic YAML alias",
    ], problems
    shared = {"data": 1.5}
    assert not vectors_check._has_cycle({"first": shared, "second": [shared]})
    assert not vectors_check._has_cycle({"a": [1, None, "x"]})
    closing = {}
    closing["round"] = closing
    assert vectors_check._has_cycle({"seen": shared, "nested": {"seen": shared, "loop": closing}})


def test_a_cyclic_alias_in_a_frames_name_is_reported_not_a_crash(tmp_path):
    files = {
        "frames/cycle.yaml": """
name: &n [*n]
covers: [SYS-2, TIM-10]
topic_id: 1
seq: 0
payload: "01 2a"
frame: "01 00 01 2a 1a 88"
wire: "01 02 01 2a 1a 88 05 00"
""",
        "frames/cycle_map.yaml": """
name: &n {a: *n}
covers: [SYS-2, TIM-10]
topic_id: 1
seq: 0
payload: "01 2a"
frame: "01 00 01 2a 1a 88"
wire: "01 02 01 2a 1a 88 05 00"
""",
        "frames/bad_name.yaml": """
name: [ping]
covers: [SYS-2, TIM-10]
topic_id: 1
seq: 0
payload: "01 2a"
frame: "01 00 01 2a 1a 88"
wire: "01 02 01 2a 1a 88 05 00"
""",
    }
    vectors = make_tree(tmp_path / "vectors", files)
    assert vectors_check.check_tree(vectors) == [
        "frames/bad_name.yaml: name: ['ping'] is not of type 'string'",
        "frames/cycle.yaml: cyclic YAML alias",
        "frames/cycle_map.yaml: cyclic YAML alias",
    ]
