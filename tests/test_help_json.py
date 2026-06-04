#!/usr/bin/env python3
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HELP_JSON = ROOT / "src" / "help.json"
BUILD_SCRIPT = ROOT / "scripts" / "build-module.sh"


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def walk_nodes(node: dict) -> None:
    title = node.get("title")
    assert_true(isinstance(title, str) and title.strip(), "all nodes must have a non-empty title")

    has_children = "children" in node
    has_lines = "lines" in node
    assert_true(has_children ^ has_lines, "node must contain exactly one of children or lines")

    if has_children:
        children = node["children"]
        assert_true(isinstance(children, list) and children, f'branch "{title}" must have children')
        for child in children:
            assert_true(isinstance(child, dict), f'branch "{title}" children must be objects')
            walk_nodes(child)
        return

    lines = node["lines"]
    assert_true(isinstance(lines, list) and lines, f'leaf "{title}" must have lines')
    for line in lines:
        assert_true(isinstance(line, str), f'leaf "{title}" lines must be strings')
        assert_true(len(line) <= 20, f'leaf "{title}" has line longer than 20 chars: "{line}"')


def main() -> None:
    assert_true(HELP_JSON.is_file(), "src/help.json must exist")

    data = json.loads(HELP_JSON.read_text())
    assert_true(isinstance(data, dict), "help.json root must be an object")
    walk_nodes(data)

    build_src = BUILD_SCRIPT.read_text()
    assert_true(
        "src/help.json" in build_src and "dist/fork/help.json" in build_src,
        "build script must package src/help.json into dist/fork/help.json",
    )

    print("PASS: help.json structure and packaging")


if __name__ == "__main__":
    main()
