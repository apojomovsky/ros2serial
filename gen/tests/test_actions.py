"""Action loading: .action files parse to Goal, Result and Feedback messages."""

from pathlib import Path

import pytest
from r2s_gen import msgs
from support import write_message


def write_action(root: Path, type_name: str, text: str) -> Path:
    package, _, name = type_name.partition("/action/")
    path = root / package / "action" / f"{name}.action"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def test_an_action_loads_its_goal_result_and_feedback(tmp_path):
    root = tmp_path / "messages"
    write_action(
        root,
        "demo_msgs/action/Spin",
        "int32 order\n---\nint32[] sequence\n---\nint32[] partial_sequence\n",
    )
    repository = msgs.MessageRepository(msg_paths=[root])

    goal = repository.load("demo_msgs/action/Spin_Goal")
    result = repository.load("demo_msgs/action/Spin_Result")
    feedback = repository.load("demo_msgs/action/Spin_Feedback")

    assert [field.name for field in goal.fields] == ["order"]
    assert [field.name for field in result.fields] == ["sequence"]
    assert [field.name for field in feedback.fields] == ["partial_sequence"]


def test_an_empty_feedback_section_is_valid(tmp_path):
    root = tmp_path / "messages"
    write_action(root, "demo_msgs/action/Spin", "int32 order\n---\nint32[] sequence\n---\n")
    repository = msgs.MessageRepository(msg_paths=[root])

    assert repository.load("demo_msgs/action/Spin_Feedback").fields == ()


def test_a_missing_separator_is_an_error(tmp_path):
    root = tmp_path / "messages"
    path = write_action(root, "demo_msgs/action/Spin", "int32 order\n---\nint32 result\n")
    repository = msgs.MessageRepository(msg_paths=[root])

    with pytest.raises(msgs.MessageError) as caught:
        repository.load("demo_msgs/action/Spin_Goal")

    assert str(caught.value) == (
        f"{path}: expected 2 '---' separators, found 1; write goal, '---', result, '---', feedback"
    )
