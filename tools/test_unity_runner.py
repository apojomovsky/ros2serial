import re
import subprocess
import sys
from pathlib import Path

import pytest
import unity_runner

TOOL = Path(__file__).resolve().parent / "unity_runner.py"


def write(tmp_path: Path, name: str, text: str) -> Path:
    path = tmp_path / name
    path.write_text(text)
    return path


def test_test_functions_are_found_in_source_order():
    src = """
void test_b_runs_first(void) {}
static void helper(void) {}
/* void test_commented_out(void) {} */
void test_a_runs_second(void)
{
}
"""
    assert unity_runner.test_names(src) == ["test_b_runs_first", "test_a_runs_second"]


def test_the_tests_runner_runs_every_test_once():
    out = unity_runner.tests_runner("test_x.c", ["test_one", "test_two"])
    assert out.count("r2s_run_test(test_one, ") == 1 and out.count("r2s_run_test(test_two, ") == 1
    assert "void test_one(void);" in out and "return UNITY_END();" in out


def test_failures_are_reported_against_the_test_source_not_the_runner(tmp_path):
    assert 'UnityBegin("/src/test_x.c");' in unity_runner.tests_runner("/src/test_x.c", ["test_one"])
    f = write(tmp_path, "basic.yaml", 'covers: [VAR-1]\ncases:\n  - {value: 1, encoded: "01"}\n')
    assert 'UnityBegin("/src/check.c");' in unity_runner.vector_runner("varint", [f], "/src/check.c")


def test_a_source_without_tests_is_an_error(tmp_path, capsys):
    src = write(tmp_path, "test_empty.c", "int x;\n")
    assert unity_runner.main(["tests", str(src), str(tmp_path / "out.c")]) == 1
    assert "no test functions" in capsys.readouterr().out


def test_rcobs_cases_become_arrays_with_their_lengths(tmp_path):
    f = write(
        tmp_path, "basic.yaml", 'covers: [COB-1, COB-4]\ncases:\n  - {decoded: "11 00", encoded: "11 01 02 00"}\n'
    )
    out = unity_runner.vector_runner("rcobs", [f])
    assert re.search(r"\{0x11, 0x00\}", out) and re.search(r"\{0x11, 0x01, 0x02, 0x00\}", out)
    assert f'"{f} case 0"' in out and '"COB-1 COB-4"' in out
    assert '"11 00", ' in out and "r2s_run_test(test_rcobs_basic_0, " in out
    assert re.search(r"r2s_vec_check_rcobs\(&\w+\)", out)


def test_crc_text_cases_carry_the_ascii_bytes(tmp_path):
    f = write(tmp_path, "check.yaml", 'covers: [CHK-2]\ncases:\n  - {text: "12", crc: 28561}\n')
    out = unity_runner.vector_runner("crc", [f])
    assert "{0x31, 0x32}" in out and 'NULL, "12", 28561u' in out


def test_empty_byte_strings_have_length_zero(tmp_path):
    f = write(tmp_path, "empty.yaml", 'covers: [COB-1]\ncases:\n  - {decoded: "", encoded: "01 00"}\n')
    out = unity_runner.vector_runner("rcobs", [f])
    assert re.search(r'NULL, 0u, ""', out), out


def test_varint_values_are_unsigned_32_bit(tmp_path):
    f = write(tmp_path, "big.yaml", 'covers: [VAR-1]\ncases:\n  - {value: 65535, encoded: "ff ff 03"}\n')
    assert "65535ul" in unity_runner.vector_runner("varint", [f])
    f = write(tmp_path, "huge.yaml", 'covers: [VAR-2]\ncases:\n  - {value: 4294967296, encoded: "80"}\n')
    with pytest.raises(unity_runner.VectorError, match="huge.yaml case 0: value"):
        unity_runner.vector_runner("varint", [f])


def test_file_names_become_valid_unique_identifiers(tmp_path):
    a = write(tmp_path, "edge-cases.yaml", 'covers: [COB-3]\ncases:\n  - {decoded: "", encoded: "01 00"}\n')
    out = unity_runner.vector_runner("rcobs", [a])
    assert "test_rcobs_edge_cases_0" in out


def test_malformed_vectors_fail_with_the_file_and_case(tmp_path):
    f = write(tmp_path, "bad.yaml", 'covers: [COB-1]\ncases:\n  - {decoded: "1", encoded: "01 00"}\n')
    with pytest.raises(unity_runner.VectorError, match="bad.yaml case 0: decoded"):
        unity_runner.vector_runner("rcobs", [f])


def test_unknown_kinds_are_rejected(tmp_path, capsys):
    f = write(tmp_path, "x.yaml", "covers: [FRM-1]\ncases: []\n")
    assert unity_runner.main(["vectors", "bogus", "check.c", str(tmp_path / "out.c"), str(f)]) == 2
    assert "kind" in capsys.readouterr().out


FRAME = """name: float32_topic16
covers: [FRM-1]
topic_id: 16
seq: 0
payload: "00 00 c0 3f"
frame: "10 00 00 00 c0 3f 1e 49"
wire: "10 02 01 01 c0 3f 1e 49 05 00"
"""


def test_a_frame_file_is_one_case_named_for_its_file(tmp_path):
    f = write(tmp_path, "float32_topic16.yaml", FRAME)
    out = unity_runner.vector_runner("frames", [f])
    assert "r2s_run_test(test_frames_float32_topic16, " in out
    assert re.search(r"r2s_vec_check_frames\(&v_float32_topic16\)", out)
    assert "16u, 0u," in out
    assert '"' + str(f) + '"' in out and "case 0" not in out


def test_a_frame_field_out_of_range_is_rejected(tmp_path):
    f = write(tmp_path, "bad.yaml", FRAME.replace("topic_id: 16", "topic_id: 256"))
    with pytest.raises(unity_runner.VectorError, match="bad.yaml: topic_id"):
        unity_runner.vector_runner("frames", [f])


def test_the_cli_writes_the_runner(tmp_path):
    f = write(tmp_path, "basic.yaml", 'covers: [VAR-1]\ncases:\n  - {value: 1, encoded: "01"}\n')
    out = tmp_path / "runner.c"
    result = subprocess.run(
        [sys.executable, str(TOOL), "vectors", "varint", "check.c", str(out), str(f)], capture_output=True
    )
    assert result.returncode == 0, result.stdout
    assert "r2s_run_test(test_varint_basic_0, " in out.read_text()


def test_rejected_cases_carry_the_rule_and_no_decoded_value(tmp_path):
    f = write(tmp_path, "invalid.yaml", 'covers: [COB-5]\ncases:\n  - {encoded: "05 00", reject: COB-5}\n')
    out = unity_runner.vector_runner("rcobs", [f])
    assert re.search(r'NULL, 0u, "", v_invalid_0_encoded, 2u, "05 00", "COB-5"', out), out
    f = write(tmp_path, "bad.yaml", 'covers: [VAR-3]\ncases:\n  - {encoded: "80", reject: VAR-3}\n')
    assert re.search(r'0ul, v_bad_0_encoded, 1u, "80", "VAR-3"', unity_runner.vector_runner("varint", [f]))


def test_valid_cases_have_a_null_rule(tmp_path):
    f = write(tmp_path, "basic.yaml", 'covers: [VAR-1]\ncases:\n  - {value: 1, encoded: "01"}\n')
    assert re.search(r'"01", NULL\}', unity_runner.vector_runner("varint", [f]))


def test_a_prototype_does_not_run_a_test_twice():
    assert unity_runner.test_names("void test_a(void);\nvoid test_a(void)\n{\n}\n") == ["test_a"]


def test_a_test_defined_twice_is_an_error(tmp_path, capsys):
    src = write(tmp_path, "test_twice.c", "void test_a(void)\n{\n}\nvoid test_a(void) {\n}\n")
    assert unity_runner.main(["tests", str(src), str(tmp_path / "out.c")]) == 1
    assert "test_a is defined twice" in capsys.readouterr().out


def test_hex_with_a_trailing_newline_is_malformed(tmp_path):
    f = write(tmp_path, "nl.yaml", 'covers: [COB-1]\ncases:\n  - {decoded: "01\\n", encoded: "01 00"}\n')
    with pytest.raises(unity_runner.VectorError, match="nl.yaml case 0: decoded"):
        unity_runner.vector_runner("rcobs", [f])


def test_records_name_the_file_as_given_and_escape_it(tmp_path):
    d = tmp_path / 'we"ird'
    d.mkdir()
    f = write(d, "basic.yaml", 'covers: [VAR-1]\ncases:\n  - {value: 1, encoded: "01"}\n')
    out = unity_runner.vector_runner("varint", [f])
    assert '"' + str(f).replace('"', '\\"') + ' case 0"' in out


def test_crc_text_cases_keep_the_text_for_the_self_test(tmp_path):
    f = write(tmp_path, "t.yaml", 'covers: [CHK-2]\ncases:\n  - {text: "12", crc: 1}\n  - {data: "31", crc: 2}\n')
    out = unity_runner.vector_runner("crc", [f])
    assert '2u, NULL, "12", 1u' in out and '1u, "31", NULL, 2u' in out


def test_a_runner_can_run_one_named_test_and_fails_on_an_unknown_name():
    out = unity_runner.tests_runner("test_x.c", ["test_one", "test_two"])
    assert "int main(int argc, char **argv)" in out
    assert 'r2s_run_test(test_one, "test_one", argc, argv)' in out
    assert "no test named" in out


def test_the_names_command_lists_the_tests_for_cmake(tmp_path, capsys):
    src = write(tmp_path, "test_n.c", "void test_a(void)\n{\n}\nvoid test_b(void)\n{\n}\n")
    assert unity_runner.main(["names", str(src)]) == 0
    assert capsys.readouterr().out.split() == ["test_a", "test_b"]
