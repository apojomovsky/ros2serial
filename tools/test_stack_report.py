"""Unit tests for tools/stack_report.py: .su parsing, call graphs, worst paths."""

import pytest
import stack_report

SU = """\
main:48:static
setup:32:static
r2s_spin:64:static
grow:16:dynamic
"""

OBJDUMP = """\
00000100 <main>:
 100:\t1c d1 \trcall\t.+56\t; 0x13a <setup>
0000013a <setup>:
 13a:\t2a d1 \trcall\t.+84\t; 0x190 <r2s_spin>
00000190 <r2s_spin>:
 190:\t08 95 \tret
"""


def test_su_maps_functions_to_frame_sizes():
    assert stack_report.parse_su(SU) == {
        "main": (48, "static"),
        "setup": (32, "static"),
        "r2s_spin": (64, "static"),
        "grow": (16, "dynamic"),
    }


def test_su_rejects_garbage():
    with pytest.raises(stack_report.StackError):
        stack_report.parse_su("main:lots:static\n")


def test_su_reads_location_lines():
    text = "codec.c:244:15:emit_byte\t4\tstatic\ncodec.c:1:1:grow\t16\tdynamic\n"
    assert stack_report.parse_su(text) == {"emit_byte": (4, "static"), "grow": (16, "dynamic")}


def test_su_reduces_cpp_signatures_to_names():
    text = "sketch.ino:32:6:void setup()\t2\tstatic\n"
    assert stack_report.parse_su(text) == {"setup": (2, "static")}


def test_su_keeps_cpp_scope_operators():
    text = "HardwareSerial.cpp:117:6:void HardwareSerial::begin(long unsigned int, byte)\t9\tstatic\n"
    assert stack_report.parse_su(text) == {"HardwareSerial::begin": (9, "static")}


def test_call_graph_strips_target_offsets():
    text = "00000100 <loop>:\n 100:\t1c d1 \trcall\t.+56\t; 0x13a <r2s_framer_poll+0x16>\n"
    assert stack_report.parse_calls(text) == {"loop": ["r2s_framer_poll"]}


def test_call_graph_demangles_callee_names(monkeypatch):
    monkeypatch.setattr(stack_report, "_demangle", lambda names: {"_Z3foov": "foo()"})
    text = "00000100 <_Z3foov>:\n 100:\t08 95 \tret\n00000200 <main>:\n 200:\t1c d1 \trcall\t.+0\t; 0x100\n"
    assert stack_report.parse_calls(text) == {"main": ["foo"]}


def test_call_graph_skips_a_folded_self_call():
    text = (
        "00000100 <encode>:\n"
        " 100:\t1c d1 \trcall\t.+0\t; 0x108 <encode+0x8>\n"
        " 108:\t08 95 \tret\n"
        "00000200 <loop>:\n"
        " 200:\t2a d1 \trcall\t.+0\t; 0x100 <encode>\n"
    )
    assert stack_report.parse_calls(text) == {"loop": ["encode"]}


def test_call_graph_skips_a_relaxed_dead_call():
    text = "00000400 <emit_varint>:\n 40c:\t00 d0 \trcall\t.+0\t; 0x40e <__LOCK_REGION_LENGTH__+0xe>\n"
    assert stack_report.parse_calls(text) == {}


def test_call_graph_follows_rcall_targets():
    assert stack_report.parse_calls(OBJDUMP) == {"main": ["setup"], "setup": ["r2s_spin"]}


def test_worst_path_sums_the_deepest_chain():
    sizes = {"main": (48, "static"), "setup": (32, "static"), "r2s_spin": (64, "static")}
    calls = {"main": ["setup"], "setup": ["r2s_spin"]}
    worst = stack_report.worst_stack(sizes, calls, ["main"])
    assert worst["total"] == 144
    assert worst["path"] == ["main", "setup", "r2s_spin"]


def test_worst_path_counts_unknown_frames_as_zero_and_lists_them():
    worst = stack_report.worst_stack({"setup": (32, "static")}, {"main": ["setup"]}, ["main"])
    assert worst["total"] == 32
    assert worst["unknown"] == ["main"]


def test_check_fails_on_a_breached_fixture():
    data = {"uno": {"total": 144, "path": ["main"], "unknown": [], "dynamic": [], "cycles": []}}
    budgets = {"firmware": {"uno": {"stack": 1}}}
    problems = stack_report.check_stacks(data, budgets)
    assert len(problems) == 1
    assert "uno" in problems[0] and "stack" in problems[0]


def test_check_fails_on_a_call_cycle():
    data = {"uno": {"total": 0, "path": [], "unknown": [], "dynamic": [], "cycles": [["a", "a"]]}}
    budgets = {"firmware": {"uno": {"stack": 10**9}}}
    assert stack_report.check_stacks(data, budgets) != []
