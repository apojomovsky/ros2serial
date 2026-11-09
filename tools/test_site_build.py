"""The static docs site builder turns the hand-written docs into HTML.

The build runs in the dev image with the standard library only, so no
new pin enters ci/versions.env. Every repo-relative link must name a
built page; anything else is a build problem, not a dead link.
"""

import site_build


def write_tree(root, files):
    for rel, text in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    return root


def test_slugify_lowercases_and_dashes():
    cases = {
        "Broadcast tf": "broadcast-tf",
        "Getting started": "getting-started",
        "Vision and non-goals": "vision-and-non-goals",
    }
    for heading, slug in cases.items():
        assert site_build.slugify(heading) == slug


def test_render_heading_carries_its_anchor():
    body, headings, problems = site_build.render_markdown("## Broadcast tf\n", "docs/a.md")
    assert problems == []
    assert '<h2 id="broadcast-tf">' in body
    assert headings == ["broadcast-tf"]


def test_render_setext_heading_becomes_h2():
    body, headings, problems = site_build.render_markdown("Not yet\n---\n", "docs/a.md")
    assert problems == []
    assert '<h2 id="not-yet">' in body
    assert headings == ["not-yet"]


def test_render_thematic_break_stays_a_rule():
    body, _, problems = site_build.render_markdown("para\n\n---\n", "docs/a.md")
    assert problems == []
    assert "<hr />" in body


def test_render_fence_escapes_and_names_language():
    body, _, problems = site_build.render_markdown("```cpp\nif (a < b) {}\n```\n", "docs/a.md")
    assert problems == []
    assert 'class="language-cpp"' in body
    assert "a &lt; b" in body


def test_render_unclosed_fence_is_a_problem():
    _, _, problems = site_build.render_markdown("```cpp\nint x = 1;\n", "docs/a.md")
    assert len(problems) == 1
    assert "fence" in problems[0]


def test_render_table_aligns_columns():
    text = "| L | R |\n| --- | ---: |\n| a | b |\n"
    body, _, problems = site_build.render_markdown(text, "docs/a.md")
    assert problems == []
    assert "<table>" in body
    assert 'style="text-align:right"' in body


def test_render_list_groups_items():
    body, _, problems = site_build.render_markdown("- one\n- two\n", "docs/a.md")
    assert problems == []
    assert body.count("<li>") == 2
    assert "<ul>" in body


def test_render_table_keeps_escaped_pipe():
    text = "| A | B |\n| --- | --- |\n| `x\\|y` | z |\n"
    body, _, problems = site_build.render_markdown(text, "docs/a.md")
    assert problems == []
    assert body.count("<td>") == 2
    assert "x|y" in body


def test_render_fence_after_list_item_stays_a_block():
    text = "- item\n\n  ```c\n  int x = 1;\n  ```\n"
    body, _, problems = site_build.render_markdown(text, "docs/a.md")
    assert problems == []
    assert "<ul>" in body
    assert 'class="language-c"' in body


def test_render_blockquote_keeps_link():
    pages = {"docs/b.md": "b.html"}
    body, _, problems = site_build.render_markdown("> See [B](b.md).\n", "docs/a.md", pages)
    assert problems == []
    assert "<blockquote>" in body
    assert 'href="b.html"' in body


def test_render_rewrites_md_link_to_page():
    pages = {"docs/a.md": "a.html", "docs/b.md": "b.html"}
    body, _, problems = site_build.render_markdown("See [B](b.md#frag).", "docs/a.md", pages)
    assert problems == []
    assert 'href="b.html#frag"' in body


def test_render_leaves_external_link_alone():
    url = "https://github.com/apojomovsky/ros2serial"
    body, _, problems = site_build.render_markdown(f"[repo]({url})", "docs/a.md", {})
    assert problems == []
    assert f'href="{url}"' in body


def test_render_unknown_relative_link_is_a_problem():
    _, _, problems = site_build.render_markdown("See [X](nope.md).", "docs/a.md", {})
    assert len(problems) == 1
    assert "nope.md" in problems[0]


def test_build_writes_pages_and_index(tmp_path):
    root = write_tree(
        tmp_path,
        {
            "README.md": "# T\n\nSee [A](docs/a.md).\n",
            "docs/a.md": "# A\n",
        },
    )
    pages = (
        site_build.Page("README.md", "walkthrough.html", "Walkthrough", "Start here."),
        site_build.Page("docs/a.md", "a.html", "A page", "The other page."),
    )
    out = tmp_path / "site"
    assert site_build.build_site(root, out, pages) == []
    assert (out / "walkthrough.html").is_file()
    assert (out / "a.html").is_file()
    index = (out / "index.html").read_text()
    assert "walkthrough.html" in index and "a.html" in index


def test_build_missing_source_is_a_problem(tmp_path):
    root = write_tree(tmp_path, {"README.md": "# T\n"})
    pages = (
        site_build.Page("README.md", "walkthrough.html", "Walkthrough", "Start here."),
        site_build.Page("docs/gone.md", "gone.html", "Gone", "Missing."),
    )
    assert site_build.build_site(root, tmp_path / "site", pages) != []


def test_build_dead_link_is_a_problem(tmp_path):
    root = write_tree(
        tmp_path,
        {
            "README.md": "# T\n",
            "docs/a.md": "# A\n\nSee [X](missing.md).\n",
        },
    )
    pages = (
        site_build.Page("README.md", "walkthrough.html", "Walkthrough", "Start here."),
        site_build.Page("docs/a.md", "a.html", "A page", "The other page."),
    )
    problems = site_build.build_site(root, tmp_path / "site", pages)
    assert len(problems) == 1
    assert "missing.md" in problems[0]


def test_real_tree_builds_without_problems():
    assert site_build.build_site(site_build.ROOT, site_build.ROOT / "build" / "site") == []
