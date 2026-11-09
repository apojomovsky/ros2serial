# Copyright (c) 2026 Alexis Pojomovsky
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

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
    site = write_site(tmp_path, {'a.html': GOOD_A, 'b.html': GOOD_B})
    assert site_links.check_site(site) == []


def test_check_dead_page_link_fails(tmp_path):
    site = write_site(tmp_path, {'a.html': '<html><body><a href="gone.html">G</a></body></html>'})
    problems = site_links.check_site(site)
    assert len(problems) == 1
    assert 'gone.html' in problems[0]


def test_check_dead_fragment_fails(tmp_path):
    site = write_site(
        tmp_path, {'a.html': '<html><body><h1 id="top">A</h1><a href="#nope">X</a></body></html>'}
    )
    problems = site_links.check_site(site)
    assert len(problems) == 1
    assert '#nope' in problems[0]


def test_check_fragment_on_other_page_resolves(tmp_path):
    site = write_site(tmp_path, {'a.html': GOOD_A, 'b.html': GOOD_B})
    assert site_links.check_site(site) == []


def test_check_external_link_is_skipped(tmp_path, capsys):
    url = 'https://github.com/apojomovsky/ros2serial'
    site = write_site(tmp_path, {'a.html': f'<html><body><a href="{url}">R</a></body></html>'})
    assert site_links.check_site(site) == []
    assert 'external' in capsys.readouterr().out
