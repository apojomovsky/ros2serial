"""The zone check compiles the core for AVR and inspects the objects, not the source."""

from pathlib import Path

import pytest
import zone_check

CLEAN = """
#include "r2s/ok.h"
int step(int state, int event)
{
    switch (event) {
    case 0: return state + 1;
    case 1: return state - 1;
    case 2: return state * 2;
    case 3: return 0;
    case 4: return state ^ 5;
    case 5: return state | 8;
    default: return state;
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
    (tmp_path / "cpp/ros2serial.hpp").write_text("#define R2S_CPP 1\n")
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
    assert any("does not compile" in p for p in problems), problems


def test_repository_core_passes():
    assert zone_check.check(Path(zone_check.__file__).resolve().parent.parent) == []
