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


def test_valid_yaml_snippet_is_skipped_by_tag(tmp_path, capsys):
    text = "# T\n\n```yaml\nnode: base\ntopics:\n  - name: battery\n```\n"
    root = write_tree(tmp_path, {"README.md": text})
    assert snippets_check.check_tree(root) == []
    assert "(yaml)" in capsys.readouterr().out


@pytest.mark.parametrize("tag", ["sh", "console", "text", "json", "yaml"])
def test_tolerated_non_code_tag_is_skipped_by_tag(tmp_path, capsys, tag):
    text = f"# T\n\n```{tag}\n$ ros2 topic list\nnot: [valid yaml\n```\n"
    root = write_tree(tmp_path, {"README.md": text})
    assert snippets_check.check_tree(root) == []
    out = capsys.readouterr().out
    assert f"({tag})" in out and "skipped by tag" in out


def test_html_tag_stays_a_hard_error(tmp_path):
    text = "# T\n\n```html\n<b>hi</b>\n```\n"
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "html" in problems[0]


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


@needs_gxx
def test_snippet_sees_the_cpp_layer_headers(tmp_path):
    root = write_tree(
        tmp_path,
        {
            "cpp/include/r2s/mini.hpp": "#ifndef MINI_HPP\n#define MINI_HPP\n#define MINI_OK 0\n#endif\n",
            "README.md": "# T\n\n```cpp\n#include <r2s/mini.hpp>\nint v = MINI_OK;\n```\n",
        },
    )
    assert snippets_check.check_tree(root) == []


@needs_gxx
def test_snippet_sees_the_library_umbrella_header(tmp_path):
    root = write_tree(
        tmp_path,
        {
            "cpp/include/ros2serial.h": "#ifndef ROS2SERIAL_H\n#define ROS2SERIAL_H\n#endif\n",
            "README.md": "# T\n\n```cpp\n#include <ros2serial.h>\nint v = 1;\n```\n",
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
    # A fence in a nested list item opens relative to the item, so a broken
    # snippet nested two levels deep is still checked.
    text = "# T\n\n1. Outer:\n   1. Inner:\n\n      ```c\n      int broken = ;\n      ```\n"
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "README.md:6" in problems[0]


@needs_gcc
def test_indented_marker_is_diagnosed(tmp_path):
    text = "# T\n\n    <!-- snippet: skip until #94 -->\n\n" + C_BROKEN
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert any("starts past column 3" in p for p in problems)


@needs_gcc
def test_literal_fence_in_indented_block_hides_nothing(tmp_path):
    text = "# T\n\n    ```\n    example\n\n```c\nint broken = ;\n```\n\n    ```\n"
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert any("does not compile" in p for p in problems)


def test_deeper_closing_fence_is_content(tmp_path):
    text = "# T\n\n```python\nx = 1\n    ```\n```\n"
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "README.md:3" in problems[0]


def test_closer_less_indented_than_opener_closes(tmp_path):
    text = "# T\n\n1. Step:\n\n   ```python\n   x = 1\n```\n"
    root = write_tree(tmp_path, {"README.md": text})
    assert snippets_check.check_tree(root) == []


@needs_gcc
def test_nested_marker_skips_nested_snippet(tmp_path, capsys):
    text = "# T\n\n1. Step:\n\n   <!-- snippet: skip until #94 -->\n\n   " + C_BROKEN
    root = write_tree(tmp_path, {"README.md": text})
    assert snippets_check.check_tree(root) == []
    assert "#94" in capsys.readouterr().out


@needs_gcc
def test_fence_after_two_blanks_is_still_checked(tmp_path):
    text = "# T\n\n1. Step:\n\n\n    ```c\n    int broken = ;\n    ```\n"
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "README.md:6" in problems[0]


def test_closer_up_to_three_past_opener_closes(tmp_path):
    text = "# T\n\n```c\nint v = 1;\n  ```\n"
    root = write_tree(tmp_path, {"README.md": text})
    assert snippets_check.check_tree(root) == []


@needs_gcc
def test_inner_fence_past_container_is_content(tmp_path):
    # Opener at column 2, inner fence at column 4: the inner fence is body
    # text, so the broken line after it still reaches the compiler.
    text = "# T\n\n  ```c\n  int v = 1;\n    ```\n    int broken = ;\n  ```\n"
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "README.md:3" in problems[0] and "does not compile" in problems[0]


@needs_gcc
def test_swallowed_tagged_fence_is_reported(tmp_path):
    # The ```c line closes the outer plain fence instead of opening a
    # snippet; the sweep reports it instead of silently passing the file.
    text = "# T\n\n  ```\n  x = 1\n    ```\n```c\nint broken = ;\n```\n```\n"
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert any("never opened a snippet" in p for p in problems)


def test_quoted_tagged_fence_is_reported(tmp_path):
    text = "# T\n\n> ```c\n> int broken = ;\n> ```\n"
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "never opened a snippet" in problems[0]


def test_quoted_tolerated_fence_is_reported(tmp_path):
    text = "# T\n\n> ```yaml\n> node: [unclosed\n> ```\n"
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "never opened a snippet" in problems[0]


def test_marker_above_tolerated_fence_skips_by_tag(tmp_path, capsys):
    text = "# T\n\n<!-- snippet: skip until #94 -->\n```text\nhello\n```\n"
    root = write_tree(tmp_path, {"README.md": text})
    assert snippets_check.check_tree(root) == []
    out = capsys.readouterr().out
    assert "skip README.md:4 (text): skipped by tag" in out
    assert "(until #94)" in out
    assert "0 skipped, 1 skipped by tag" in out


@needs_gcc
def test_snippet_sees_generated_header_fixture(tmp_path):
    root = write_tree(
        tmp_path,
        {
            "gen/tests/golden/topics/posix.h": (
                "#ifndef R2S_GENERATED_H\n"
                "#define R2S_GENERATED_H\n"
                "typedef struct std_msgs__msg__Float32 { float data; } std_msgs__msg__Float32;\n"
                "int r2s_generated_magic(void);\n"
                "#endif\n"
            ),
            "README.md": (
                '# T\n\n```c\n#include "r2s_generated.h"\n'
                "static std_msgs__msg__Float32 battery;\n"
                "int snippet_main(void) { battery.data = 1.0f; return r2s_generated_magic(); }\n```\n"
            ),
        },
    )
    assert snippets_check.check_tree(root) == []


@needs_gcc
def test_snippet_sees_bundle_fixture(tmp_path):
    root = write_tree(
        tmp_path,
        {
            "gen/tests/golden/bundle/ros2serial.h": (
                "#ifndef ROS2SERIAL_H\n"
                "#define ROS2SERIAL_H\n"
                "#ifndef R2S_TOPICS\n"
                '#error "Define R2S_TOPICS(X) rows before including <ros2serial.h>."\n'
                "#endif\n"
                "#define R2S_X_PUB_COUNT(DIR, NAME, PKG, TYPE) + 1\n"
                "#define R2S_PUB_COUNT (0 R2S_TOPICS(R2S_X_PUB_COUNT))\n"
                "#endif\n"
            ),
            "README.md": (
                "# T\n\n```c\n#define R2S_TOPICS(X) X(PUB, battery, std_msgs, Float32)\n"
                "#include <ros2serial.h>\n"
                "int snippet_main(void) { return R2S_PUB_COUNT - 1; }\n```\n"
            ),
        },
    )
    assert snippets_check.check_tree(root) == []


@needs_gcc
def test_generated_header_missing_fails(tmp_path):
    text = '# T\n\n```c\n#include "r2s_generated.h"\nint snippet_main(void) { return 0; }\n```\n'
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "r2s_generated.h" in problems[0]


@needs_gcc
def test_snippet_including_both_staged_headers_fails(tmp_path):
    # The two staged headers define the same message structs, so a snippet
    # uses at most one; the checker names both headers and the rule.
    root = write_tree(
        tmp_path,
        {
            "gen/tests/golden/topics/posix.h": (
                "#ifndef R2S_GENERATED_H\n"
                "#define R2S_GENERATED_H\n"
                "typedef struct widget_a { int x; } widget_a;\n"
                "#endif\n"
            ),
            "gen/tests/golden/bundle/r2s_bundled.h": (
                "#ifndef R2S_BUNDLED_H\n#define R2S_BUNDLED_H\ntypedef struct widget_b { int x; } widget_b;\n#endif\n"
            ),
            "README.md": (
                '# T\n\n```c\n#include "r2s_generated.h"\n'
                '#include "r2s_bundled.h"\n'
                "int snippet_main(void) { return 0; }\n```\n"
            ),
        },
    )
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "README.md:3" in problems[0]
    assert "r2s_generated.h" in problems[0] and "r2s_bundled.h" in problems[0]
    assert "at most one" in problems[0]


@needs_gcc
def test_snippet_including_both_staged_headers_angle_form_fails(tmp_path):
    root = write_tree(
        tmp_path,
        {
            "gen/tests/golden/topics/posix.h": "#ifndef R2S_GENERATED_H\n#define R2S_GENERATED_H\n#endif\n",
            "gen/tests/golden/bundle/r2s_bundled.h": "#ifndef R2S_BUNDLED_H\n#define R2S_BUNDLED_H\n#endif\n",
            "README.md": (
                "# T\n\n```c\n#include <r2s_generated.h>\n"
                "#include <r2s_bundled.h>\n"
                "int snippet_main(void) { return 0; }\n```\n"
            ),
        },
    )
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "at most one" in problems[0]


@needs_gcc
def test_snippet_sees_bundled_header_fixture(tmp_path):
    root = write_tree(
        tmp_path,
        {
            "gen/tests/golden/bundle/r2s_bundled.h": (
                "#ifndef R2S_BUNDLED_H\n"
                "#define R2S_BUNDLED_H\n"
                "typedef struct widget_b { int x; } widget_b;\n"
                "int r2s_bundled_magic(void);\n"
                "#endif\n"
            ),
            "README.md": (
                '# T\n\n```c\n#include "r2s_bundled.h"\n'
                "static widget_b widget;\n"
                "int snippet_main(void) { widget.x = 1; return r2s_bundled_magic(); }\n```\n"
            ),
        },
    )
    assert snippets_check.check_tree(root) == []


@needs_gcc
def test_commented_out_second_header_passes(tmp_path):
    # An include inside a C comment is not a use of the staged header.
    root = write_tree(
        tmp_path,
        {
            "gen/tests/golden/topics/posix.h": "#ifndef R2S_GENERATED_H\n#define R2S_GENERATED_H\n#endif\n",
            "gen/tests/golden/bundle/r2s_bundled.h": "#ifndef R2S_BUNDLED_H\n#define R2S_BUNDLED_H\n#endif\n",
            "README.md": (
                '# T\n\n```c\n#include "r2s_generated.h"\n'
                '/* #include "r2s_bundled.h" */\n'
                "// #include <r2s_bundled.h>\n"
                "int snippet_main(void) { return 0; }\n```\n"
            ),
        },
    )
    assert snippets_check.check_tree(root) == []


def test_python_comment_naming_both_headers_passes(tmp_path):
    text = "# T\n\n```python\n# uses r2s_generated.h and r2s_bundled.h\nx = 1\n```\n"
    root = write_tree(tmp_path, {"README.md": text})
    assert snippets_check.check_tree(root) == []


def test_skip_marker_with_reason_skips_and_prints_reason(tmp_path, capsys):
    text = "# T\n\n<!-- snippet: skip until #75: template placeholders, never literal C -->\n" + C_BROKEN
    root = write_tree(tmp_path, {"README.md": text})
    assert snippets_check.check_tree(root) == []
    out = capsys.readouterr().out
    assert "#75" in out and "template placeholders" in out


def test_skip_marker_reason_printed_for_tag_skipped_fence(tmp_path, capsys):
    text = "# T\n\n<!-- snippet: skip until #75: template placeholders, never literal C -->\n```text\nhello\n```\n"
    root = write_tree(tmp_path, {"README.md": text})
    assert snippets_check.check_tree(root) == []
    out = capsys.readouterr().out
    assert "skipped by tag" in out and "#75" in out and "template placeholders" in out


def test_skip_marker_with_empty_reason_is_an_error(tmp_path):
    text = "# T\n\n<!-- snippet: skip until #75: -->\n" + C_OK
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "reason is empty" in problems[0]


def test_quoted_list_item_fence_is_reported(tmp_path):
    text = "# T\n\n> - ```c\n>   int broken = ;\n>   ```\n"
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "never opened a snippet" in problems[0]


def test_quoted_ordered_item_fence_is_reported(tmp_path):
    text = "# T\n\n> 1. ```c\n>    int broken = ;\n>    ```\n"
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert len(problems) == 1
    assert "never opened a snippet" in problems[0]


def test_literal_fence_in_indented_block_with_marker_passes(tmp_path, capsys):
    text = "# T\n\n<!-- snippet: skip until #193: literal fence syntax example -->\n    ```c\n    int x = 1;\n    ```\n"
    root = write_tree(tmp_path, {"README.md": text})
    assert snippets_check.check_tree(root) == []
    assert "until #193" in capsys.readouterr().out


def test_literal_fence_in_outer_fence_with_marker_passes(tmp_path, capsys):
    text = "# T\n\n```\n<!-- snippet: skip until #193: shows fence syntax literally -->\n```c\nint x = 1;\n```\n```\n"
    root = write_tree(tmp_path, {"README.md": text})
    assert snippets_check.check_tree(root) == []
    assert "until #193" in capsys.readouterr().out


def test_literal_fence_in_yaml_scalar_with_marker_passes(tmp_path, capsys):
    text = "# T\n\n```yaml\nexample: |\n  <!-- snippet: skip until #193: literal -->\n    ```c\n    ...\n```\n"
    root = write_tree(tmp_path, {"README.md": text})
    assert snippets_check.check_tree(root) == []
    assert "until #193" in capsys.readouterr().out


def test_marker_above_outer_fence_does_not_silence_inner_fence(tmp_path):
    text = "# T\n\n<!-- snippet: skip until #94 -->\n```\nx = 1\n```c\nint broken = ;\n```\n```\n"
    root = write_tree(tmp_path, {"README.md": text})
    problems = snippets_check.check_tree(root)
    assert any("never opened a snippet" in p for p in problems)
