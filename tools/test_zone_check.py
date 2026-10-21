"""The zone check compiles the core for AVR and inspects the objects, not the source."""

from pathlib import Path

import pytest
import zone_check

CLEAN = """
#include "r2s/ok.h"
void a0(void); void a1(void); void a2(void); void a3(void); void a4(void);
void a5(void); void a6(void); void a7(void); void a8(void); void a9(void);
void dispatch(unsigned char id)
{
    switch (id) {
    case 0: a0(); break;
    case 1: a1(); break;
    case 2: a2(); break;
    case 3: a3(); break;
    case 4: a4(); break;
    case 5: a5(); break;
    case 6: a6(); break;
    case 7: a7(); break;
    case 8: a8(); break;
    case 9: a9(); break;
    default: break;
    }
}
void copy_float_bytes(unsigned char *dst, const float *src)
{
    const unsigned char *s = (const unsigned char *)src;
    for (int i = 0; i < 4; i++) dst[i] = s[i];
}
"""


def tree(tmp_path: Path, sources: dict[str, str]) -> Path:
    (tmp_path / "core/include/r2s").mkdir(parents=True)
    (tmp_path / "core/include/r2s/ok.h").write_text("#define R2S_OK 0\n")
    (tmp_path / "cpp").mkdir()
    (tmp_path / "cpp/ros2serial.hpp").write_text(
        "namespace r2s {\ntemplate <class T> struct A {};\n}\n#define R2S_CPP 1\n"
    )
    (tmp_path / "core/src").mkdir()
    for name, text in sources.items():
        (tmp_path / "core/src" / name).write_text(text)
    return tmp_path


def test_a_clean_core_passes(tmp_path):
    assert zone_check.check(tree(tmp_path, {"clean.c": CLEAN})) == []


def test_no_sources_passes(tmp_path):
    (tmp_path / "core/src").mkdir(parents=True)
    assert zone_check.check(tmp_path) == []


@pytest.mark.parametrize(
    ("source", "expected"),
    [
        (
            "static int inc(int x) { return x + 1; }\n"
            "int (*volatile fp)(int) = inc;\n"
            "int call(int x) { return fp(x); }\n",
            "function pointer",
        ),
        (
            "extern void reg(void (*h)(void));\nstatic void handler(void) {}\nvoid setup(void) { reg(handler); }\n",
            "function pointer",
        ),
        ("float add(float a, float b) { return a + b; }\n", "float"),
        ("#include <math.h>\nfloat root(float x) { return sqrtf(x); }\n", "sqrtf"),
        ("#include <math.h>\ndouble down(double x) { return floor(x); }\n", "floor"),
        ("float p(float a, int n) { return __builtin_powif(a, n); }\n", "float"),
        ("long to_int(float f) { return (long)f; }\n", "float"),
        ('#include "../../cpp/ros2serial.hpp"\nint x = R2S_CPP;\n', "cpp/"),
    ],
)
def test_violations_are_reported_with_the_source_file(tmp_path, source, expected):
    problems = zone_check.check(tree(tmp_path, {"clean.c": CLEAN, "bad.c": source}))
    assert problems, "expected a violation"
    assert all(p.startswith("core/src/bad.c") for p in problems), problems
    assert any(expected in p for p in problems), problems


def test_a_compile_error_is_reported(tmp_path):
    problems = zone_check.check(tree(tmp_path, {"broken.c": "int f( {\n"}))
    assert any("does not compile" in p and "error:" in p for p in problems), problems


def test_repository_core_passes():
    assert zone_check.check(Path(zone_check.__file__).resolve().parent.parent) == []


def test_the_address_taken_message_names_the_function(tmp_path):
    source = "extern void reg(void (*h)(void));\nstatic void handler(void) {}\nvoid setup(void) { reg(handler); }\n"
    problems = zone_check.check(tree(tmp_path, {"bad.c": source}))
    assert any("address of handler" in p for p in problems), problems


def test_a_cpp_include_is_named_even_when_the_c_compile_fails(tmp_path):
    problems = zone_check.check(tree(tmp_path, {"bad.c": '#include "../../cpp/ros2serial.hpp"\n'}))
    assert any("cpp/ros2serial.hpp" in p for p in problems), problems


def test_a_cpp_include_is_found_in_a_path_with_spaces(tmp_path):
    root = tmp_path / "with space"
    root.mkdir()
    problems = zone_check.check(tree(root, {"bad.c": '#include "../../cpp/ros2serial.hpp"\n'}))
    assert any("cpp/ros2serial.hpp" in p for p in problems), problems


def test_sources_in_subdirectories_are_checked(tmp_path):
    root = tree(tmp_path, {})
    (root / "core/src/port").mkdir()
    (root / "core/src/port/t.c").write_text("float twice(float a) { return a * 2; }\n")
    problems = zone_check.check(root)
    assert any(p.startswith("core/src/port/t.c") and "float" in p for p in problems), problems


def test_float_in_an_unused_static_inline_header_function_is_caught(tmp_path):
    root = tree(tmp_path, {"user.c": '#include "r2s/x.h"\nint f(void) { return 1; }\n'})
    (root / "core/include/r2s/x.h").write_text("static inline float h(float a) { return a * 2; }\n")
    problems = zone_check.check(root)
    assert any(p.startswith("core/include/r2s/x.h") and "float" in p for p in problems), problems


def test_a_source_named_after_an_instruction_is_not_flagged(tmp_path):
    assert zone_check.check(tree(tmp_path, {"icall.c": "int f(int x) { return x + 1; }\n"})) == []


def test_a_failing_inspection_tool_fails_the_check(tmp_path, monkeypatch):
    monkeypatch.setattr(zone_check, "OBJDUMP", "false")
    problems = zone_check.check(tree(tmp_path, {"clean.c": CLEAN}))
    assert any("failed" in p for p in problems), problems


def test_a_missing_tool_is_reported_not_raised(tmp_path, monkeypatch):
    monkeypatch.setattr(zone_check, "CC", "avr-gcc-not-installed")
    problems = zone_check.check(tree(tmp_path, {"clean.c": CLEAN}))
    assert problems and "not found" in problems[0], problems
