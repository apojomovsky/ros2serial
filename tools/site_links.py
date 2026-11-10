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

"""Check the built docs site links without network access.

Every internal link must name a built file, and every #fragment must
match a heading id on its target page. External links are skipped and
counted, so the check stays hermetic on any host.
"""

import argparse
import sys
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urldefrag
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parent.parent


class _Links(HTMLParser):
    """The hrefs and ids of one page, in document order."""

    def __init__(self):
        super().__init__()
        self.hrefs: list[str] = []
        self.ids: list[str] = []

    def handle_starttag(self, tag, attrs):
        for name, value in attrs:
            if tag == 'a' and name == 'href' and value:
                self.hrefs.append(value)
            if name == 'id' and value:
                self.ids.append(value)


def _read_links(page: Path) -> _Links:
    found = _Links()
    found.feed(page.read_text(encoding='utf-8'))
    return found


def check_site(sitedir: Path) -> list[str]:
    """Check every built page; problems, empty when green."""
    problems: list[str] = []
    pages = {path.name: _read_links(path) for path in sorted(sitedir.glob('*.html'))}
    external = 0
    for name, found in pages.items():
        for href in found.hrefs:
            parsed = urlparse(href)
            if parsed.scheme or href.startswith('mailto:'):
                external += 1
                continue
            target, frag = urldefrag(href)
            if target and target not in pages:
                problems.append(f'{name}: link to {href} names no site page')
                continue
            ids = pages[target].ids if target else found.ids
            if frag and frag not in ids:
                problems.append(f'{name}: link to {href} matches no heading')
    print(f'links: {len(pages)} pages, {external} external skipped')
    return problems


def main(argv: list[str] | None = None) -> int:
    """Check the built site; --dir selects it, default build/site."""
    parser = argparse.ArgumentParser(description='Check the built docs site links.')
    parser.add_argument('--dir', default=str(ROOT / 'build' / 'site'))
    args = parser.parse_args(argv)
    sitedir = Path(args.dir)
    if not sitedir.is_dir():
        print(f'{sitedir}: no built site; run tools/site_build.py first')
        return 1
    problems = check_site(sitedir)
    for problem in problems:
        print(problem)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
