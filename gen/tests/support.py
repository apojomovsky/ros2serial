"""Helpers the generator tests share: throwaway message packages on disk."""

from pathlib import Path


def write_message(root: Path, type_name: str, text: str) -> Path:
    """Write `text` as the definition of `type_name` under a message root, and return its path."""
    package, _, name = type_name.partition("/msg/")
    path = root / package / "msg" / f"{name}.msg"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path
