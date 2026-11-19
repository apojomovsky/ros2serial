# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""A walkthrough page's marked blocks are extracted, written and run as printed.

The page marks the blocks the check owns, so the check never guesses which fence
is a command and which is prose. Every case here fails while the tool would
accept a marker above no fence, which is how a page silently stops being run.
"""

from pathlib import Path

import docs_walkthrough

ROOT = Path(__file__).resolve().parent.parent
PAGE = ROOT / 'docs' / 'arduino-ide-custom-messages.md'


def page(*blocks: str) -> str:
    return '# A walkthrough\n\nProse.\n\n' + '\n'.join(blocks)


FILE_BLOCK = '<!-- docs-walkthrough: file firmware/lib.txt -->\n```text\nhello\n```\n'
RUN_BLOCK = '<!-- docs-walkthrough: run -->\n```sh\nprintf "ran\\n" > ran.txt\n```\n'
SERVE_BLOCK = (
    '<!-- docs-walkthrough: serve -->\n'
    '```sh\nros2 launch r2s_bridge r2s_bridge.launch.py port:=/dev/ttyACM0\n```\n'
)
LIVE_BLOCK = (
    '<!-- docs-walkthrough: live -->\n'
    '```sh\nros2 topic echo /reading my_robot_interfaces/msg/SensorReading --once\n```\n'
)


def test_a_file_block_is_written_under_home(tmp_path):
    blocks, problems = docs_walkthrough.parse(page(FILE_BLOCK))

    assert problems == []
    docs_walkthrough.write_files(blocks, tmp_path)

    assert (tmp_path / 'firmware' / 'lib.txt').read_text() == 'hello\n'


def test_a_run_block_runs_in_home_and_leaves_its_output(tmp_path):
    blocks, problems = docs_walkthrough.parse(page(RUN_BLOCK))

    assert problems == []
    failure = docs_walkthrough.execute(blocks[0], tmp_path, 60)

    assert failure is None
    assert (tmp_path / 'ran.txt').read_text() == 'ran\n'


def test_a_run_block_that_exits_nonzero_is_reported_with_its_line(tmp_path):
    blocks, problems = docs_walkthrough.parse(
        page('<!-- docs-walkthrough: run -->\n```sh\nexit 3\n```\n')
    )

    assert problems == []
    failure = docs_walkthrough.execute(blocks[0], tmp_path, 60)

    assert failure is not None
    assert 'exit 3' in failure


def test_a_run_block_that_overruns_its_timeout_is_reported(tmp_path):
    blocks, _ = docs_walkthrough.parse(
        page('<!-- docs-walkthrough: run -->\n```sh\nsleep 5\n```\n')
    )

    failure = docs_walkthrough.execute(blocks[0], tmp_path, 1)

    assert failure is not None
    assert 'timeout' in failure


def test_a_marker_above_no_fence_is_named():
    _, problems = docs_walkthrough.parse(
        page('<!-- docs-walkthrough: file a.txt -->\n\nProse instead.\n')
    )

    assert len(problems) == 1
    assert 'a.txt' in problems[0]


def test_a_file_marker_with_no_path_is_named():
    _, problems = docs_walkthrough.parse(
        page('<!-- docs-walkthrough: file -->\n```text\nx\n```\n')
    )

    assert len(problems) == 1 and 'file' in problems[0]


def test_a_path_climbing_out_of_home_is_refused():
    _, problems = docs_walkthrough.parse(
        page('<!-- docs-walkthrough: file ../escape.txt -->\n```text\nx\n```\n')
    )

    assert len(problems) == 1 and 'escape' in problems[0]


def test_prose_between_a_marker_and_its_fence_is_refused():
    """A marker binds the very next line, so a lost fence cannot adopt a later one."""
    _, problems = docs_walkthrough.parse(
        page('<!-- docs-walkthrough: run -->\n\nProse.\n\n```sh\nexit 0\n```\n')
    )

    assert len(problems) == 1 and 'run' in problems[0]


def test_a_marker_indented_into_a_list_item_is_refused():
    """Past column 3 a marker is literal text to a renderer, so it must not vanish."""
    _, problems = docs_walkthrough.parse(
        page('1. Step one.\n\n    <!-- docs-walkthrough: run -->\n    ```sh\nexit 0\n```\n')
    )

    assert len(problems) == 1 and 'literal' in problems[0]


def test_a_run_marker_on_a_fence_that_is_not_sh_is_refused():
    _, problems = docs_walkthrough.parse(
        page('<!-- docs-walkthrough: run -->\n```cpp\nint x;\n```\n')
    )

    assert len(problems) == 1 and 'sh' in problems[0]


def test_a_serve_block_is_one_bridge_launch_line():
    blocks, problems = docs_walkthrough.parse(page(SERVE_BLOCK))

    assert problems == []
    assert blocks[0].kind == 'serve'


def test_a_serve_block_holding_two_commands_is_refused():
    text = page(
        '<!-- docs-walkthrough: serve -->\n'
        '```sh\nros2 launch r2s_bridge r2s_bridge.launch.py port:=/dev/ttyACM0\n'
        'ros2 topic list\n```\n'
    )

    _, problems = docs_walkthrough.parse(text)

    assert len(problems) == 1 and 'serve' in problems[0]


def test_a_live_block_names_the_topic_it_reads():
    blocks, problems = docs_walkthrough.parse(page(LIVE_BLOCK))

    assert problems == []
    assert blocks[0].topic == '/reading'


def test_a_live_block_over_anything_but_an_echo_is_refused():
    text = page('<!-- docs-walkthrough: live -->\n```sh\nros2 topic list\n```\n')

    _, problems = docs_walkthrough.parse(text)

    assert len(problems) == 1 and 'live' in problems[0]


def test_a_page_with_no_marker_is_refused():
    _, problems = docs_walkthrough.parse('# Just prose\n')

    assert len(problems) == 1 and 'marks no block' in problems[0]


def test_emit_writes_the_serve_and_live_scripts_with_the_named_port(tmp_path):
    blocks, _ = docs_walkthrough.parse(page(SERVE_BLOCK, LIVE_BLOCK))

    problems = docs_walkthrough.emitted(blocks, tmp_path, '/dev/pts/7')

    assert problems == []
    assert 'port:=/dev/pts/7' in (tmp_path / 'serve.sh').read_text()
    assert 'ros2 topic echo /reading' in (tmp_path / 'live.sh').read_text()


def test_emit_refuses_a_serve_block_with_no_port(tmp_path):
    blocks, _ = docs_walkthrough.parse(page(SERVE_BLOCK))

    problems = docs_walkthrough.emitted(blocks, tmp_path, None)

    assert len(problems) == 1 and 'port' in problems[0]


def test_emit_refuses_a_second_live_block_instead_of_overwriting(tmp_path):
    """A page holds one echo, so an added one fails instead of going unexercised."""
    other = LIVE_BLOCK.replace('/reading', '/status')
    blocks, _ = docs_walkthrough.parse(page(LIVE_BLOCK, other))

    problems = docs_walkthrough.emitted(blocks, tmp_path, '/dev/pts/7')

    assert len(problems) == 1 and 'second live' in problems[0]


def test_run_executes_the_page_in_order(tmp_path):
    """A run block sees the file printed above it and not the one printed below."""
    text = page(
        '<!-- docs-walkthrough: file early.txt -->\n```text\nhere\n```\n',
        '<!-- docs-walkthrough: run -->\n```sh\ntest -e early.txt && test ! -e later.txt\n```\n',
        '<!-- docs-walkthrough: file later.txt -->\n```text\nhere\n```\n',
    )
    page_file = tmp_path / 'page.md'
    page_file.write_text(text, encoding='utf-8')

    status = docs_walkthrough.main(
        ['--page', str(page_file), '--run', '--home', str(tmp_path / 'home')]
    )

    assert status == 0
    assert (tmp_path / 'home' / 'later.txt').read_text() == 'here\n'


def test_the_tree_page_names_the_package_the_sketch_and_the_live_topic():
    blocks, problems = docs_walkthrough.parse(PAGE.read_text(encoding='utf-8'))

    assert problems == []
    written = {block.path for block in blocks if block.kind == 'file'}
    assert 'ros2_ws/src/my_robot_interfaces/msg/SensorReading.msg' in written
    assert 'firmware/sensor_sketch/sensor_sketch.ino' in written
    live = [block for block in blocks if block.kind == 'live']
    assert [block.topic for block in live] == ['/reading']


def test_the_tree_page_marks_every_owned_block_at_column_zero():
    text = PAGE.read_text(encoding='utf-8')

    for line in text.splitlines():
        if 'docs-walkthrough:' in line:
            assert line.startswith('<!-- docs-walkthrough:'), line


def test_the_tree_page_hides_no_block_from_the_snippet_check():
    text = PAGE.read_text(encoding='utf-8')

    assert 'snippet: skip' not in text
