"""The site link check proves every built page only links at the site.

Internal links must name a built file, and every #fragment must match
a heading id on its target page. External links are skipped without
network access, so the check stays hermetic.
"""

import site_links

GOOD_A = '<html><body><h1 id="top">A</h1><a href="b.html#frag">B</a></body></html>'
GOOD_B = '<html><body><h2 id="frag">B</h2><a href="a.html#top">A</a></body></html>'


def write_site(root, files):
    for name, text in files.items():
        (root / name).write_text(text)
    return root


def test_check_clean_site_passes(tmp_path):
    site = write_site(tmp_path, {"a.html": GOOD_A, "b.html": GOOD_B})
    assert site_links.check_site(site) == []


def test_check_dead_page_link_fails(tmp_path):
    site = write_site(tmp_path, {"a.html": '<html><body><a href="gone.html">G</a></body></html>'})
    problems = site_links.check_site(site)
    assert len(problems) == 1
    assert "gone.html" in problems[0]


def test_check_dead_fragment_fails(tmp_path):
    site = write_site(tmp_path, {"a.html": '<html><body><h1 id="top">A</h1><a href="#nope">X</a></body></html>'})
    problems = site_links.check_site(site)
    assert len(problems) == 1
    assert "#nope" in problems[0]


def test_check_fragment_on_other_page_resolves(tmp_path):
    site = write_site(tmp_path, {"a.html": GOOD_A, "b.html": GOOD_B})
    assert site_links.check_site(site) == []


def test_check_external_link_is_skipped(tmp_path, capsys):
    url = "https://github.com/apojomovsky/ros2serial"
    site = write_site(tmp_path, {"a.html": f'<html><body><a href="{url}">R</a></body></html>'})
    assert site_links.check_site(site) == []
    assert "external" in capsys.readouterr().out
