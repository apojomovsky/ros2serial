"""The docs snippet check extracts tagged fences and compiles each one."""

import shutil

import pytest
import snippets_check

needs_gcc = pytest.mark.skipif(shutil.which("gcc") is None, reason="needs gcc")
needs_gxx = pytest.mark.skipif(shutil.which("g++") is None, reason="needs g++")

C_OK = "```c\nint snippet_main(void) { return 0; }\n```\n"
C_BROKEN = "```c\nint snippet_main(void) { return NO_SUCH_SYMBOL; }\n```\n"
CPP_OK = "```cpp\nint snippet_main() { return 0; }\n```\n"


def write_tree(root, files):
    for rel, text in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    return root


@needs_gcc
def test_valid_c_snippet_passes(tmp_path):
    root = write_tree(tmp_path, {"README.md": "# T\n\n" + C_OK})
    assert snippets_check.check_tree(root) == []


@needs_gcc
def test_broken_readme_snippet_fails(tmp_path):
    root = write_tree(tmp_path, {"README.md": "# T\n\n" + C_BROKEN})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "README.md:3" in problems[0]


@needs_gcc
def test_broken_snippet_in_docs_fails(tmp_path):
    root = write_tree(tmp_path, {"README.md": "# T\n", "docs/a.md": "# A\n\n" + C_BROKEN})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "docs/a.md:3" in problems[0]


def test_skip_marker_skips_until_ticket(tmp_path, capsys):
    text = "# T\n\n<!-- snippet: skip until #94 -->\n" + C_BROKEN
    root = write_tree(tmp_path, {"README.md": text})
    assert snippets_check.check_tree(root) == []
    out = capsys.readouterr().out
    assert "skip" in out and "README.md:4" in out and "#94" in out


def test_skip_marker_without_ticket_is_an_error(tmp_path):
    text = "# T\n\n<!-- snippet: skip -->\n" + C_OK
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "ticket" in problems[0]


def test_marker_above_nothing_is_an_error(tmp_path):
    text = "# T\n\n<!-- snippet: skip until #94 -->\n\nSome prose.\n"
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "README.md:3" in problems[0]


def test_unknown_language_fails(tmp_path):
    root = write_tree(tmp_path, {"README.md": "# T\n\n```ruby\nputs 1\n```\n"})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "ruby" in problems[0]


def test_plain_fences_are_ignored(tmp_path):
    text = "# T\n\n```\nnot code: <oops &>\n```\n"
    root = write_tree(tmp_path, {"README.md": text})
    assert snippets_check.check_tree(root) == []


def test_unclosed_fence_fails(tmp_path):
    root = write_tree(tmp_path, {"README.md": "# T\n\n```c\nint x = 1;\n"})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "README.md:3" in problems[0]


def test_yaml_snippet_parses(tmp_path):
    text = "# T\n\n```yaml\nnode: base\ntopics:\n  - name: battery\n```\n"
    root = write_tree(tmp_path, {"README.md": text})
    assert snippets_check.check_tree(root) == []


def test_broken_yaml_snippet_fails(tmp_path):
    text = "# T\n\n```yaml\nnode: [unclosed\n```\n"
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "README.md:3" in problems[0]


def test_python_snippet_compiles(tmp_path):
    text = "# T\n\n```python\ndef f():\n    return 1\n```\n"
    root = write_tree(tmp_path, {"README.md": text})
    assert snippets_check.check_tree(root) == []


def test_broken_python_snippet_fails(tmp_path):
    text = "# T\n\n```python\ndef f(:\n```\n"
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "README.md:3" in problems[0]


@needs_gcc
def test_snippet_sees_core_headers(tmp_path):
    root = write_tree(
        tmp_path,
        {
            "core/include/r2s/mini.h": "#ifndef MINI_H\n#define MINI_H\n#define MINI_OK 0\n#endif\n",
            "README.md": '# T\n\n```c\n#include "r2s/mini.h"\nint v = MINI_OK;\n```\n',
        },
    )
    assert snippets_check.check_tree(root) == []


@needs_gcc
def test_c_implicit_declaration_fails(tmp_path):
    text = "# T\n\n```c\nint f(void) { return undeclared(); }\n```\n"
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1


@needs_gxx
def test_valid_cpp_snippet_passes(tmp_path):
    root = write_tree(tmp_path, {"README.md": "# T\n\n" + CPP_OK})
    assert snippets_check.check_tree(root) == []


@needs_gcc
def test_indented_fence_inside_list_is_checked(tmp_path):
    text = "# T\n\n1. Step:\n\n   ```c\n   int broken = ;\n   ```\n"
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "README.md:5" in problems[0]


@needs_gcc
def test_marker_mentioned_in_prose_skips_nothing(tmp_path):
    text = "# T\n\nCarries <!-- snippet: skip until #94 --> in passing.\n\n" + C_BROKEN
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "README.md:5" in problems[0]


@needs_gcc
def test_nested_docs_dir_is_checked(tmp_path):
    root = write_tree(tmp_path, {"README.md": "# T\n", "docs/sub/a.md": "# A\n\n" + C_BROKEN})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "docs/sub/a.md:3" in problems[0]


def test_indented_python_body_is_dedented(tmp_path):
    text = "# T\n\n1. Step:\n\n   ```python\n   x = 1\n   ```\n"
    root = write_tree(tmp_path, {"README.md": text})
    assert snippets_check.check_tree(root) == []


@needs_gcc
def test_deeply_nested_broken_snippet_fails(tmp_path):
    text = "# T\n\n1. Outer:\n   1. Inner:\n\n      ```c\n      int broken = ;\n      ```\n"
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "README.md:6" in problems[0]


@needs_gcc
def test_marker_in_indented_code_block_is_inert(tmp_path):
    text = "# T\n\n    <!-- snippet: skip until #94 -->\n\n" + C_BROKEN
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "README.md:5" in problems[0]
