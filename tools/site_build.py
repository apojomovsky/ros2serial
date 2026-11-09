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

"""Build the static docs site from the hand-written docs.

The build uses the standard library only, so no new pin enters
ci/versions.env. Every repo-relative link must name a built page;
anything else is a build problem, not a silent dead link.
"""

import argparse
import html
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


@dataclass
class Page:
    source: str
    output: str
    title: str
    blurb: str


PAGES = (
    Page('README.md', 'walkthrough.html', 'Walkthrough', 'Install, run and flash the examples.'),
    Page(
        'docs/02-architecture.md',
        'architecture.html',
        'Architecture',
        'From the sketch to the ROS 2 graph.',
    ),
    Page(
        'docs/03-port-rosserial-sketch.md',
        'port-guide.html',
        'Port your rosserial sketch',
        'Keep the sketch, swap the includes, fix the fields.',
    ),
    Page(
        'spec/protocol.md',
        'protocol.html',
        'Protocol reference',
        'The wire format both sides prove.',
    ),
    Page('docs/00-design.md', 'design.html', 'Design', 'Why the system looks this way.'),
    Page('CONTRIBUTING.md', 'contributing.html', 'Contributing', 'Tickets, branches and review.'),
    Page(
        'AGENTS.md', 'rules.html', 'Contributor rules', 'The conventions agents and humans follow.'
    ),
    Page('LICENSE', 'license.html', 'License', 'The MIT terms.'),
)

HEADING = re.compile(r'^(#{1,6})\s+(.*?)\s*#*\s*$')
FENCE = re.compile(r'^ *```([A-Za-z0-9+#-]*)\s*$')
HR = re.compile(r'^ *(?:\*{3,}|-{3,}|_{3,}) *$')
TABLE_DELIM = re.compile(r'^\|? *:?-+:? *(\| *:?-+:? *)*\|? *$')
LIST_ITEM = re.compile(r'^ {0,3}([*+-]|\d{1,9}[.)])\s+(.*)$')
HTML_COMMENT = re.compile(r'^\s*<!--')
EXTERNAL = re.compile(r'^[a-zA-Z][a-zA-Z0-9+.-]*:')
STRIP_CODE = re.compile(r'`([^`]*)`')


def slugify(heading: str) -> str:
    """A GitHub-style anchor: lowercase, punctuation dropped, spaces dashed."""
    text = re.sub(r'<[^>]*>', '', heading).lower()
    text = re.sub(r'[^a-z0-9 _-]', '', text).strip()
    return re.sub(r'\s+', '-', text)


def _split_cells(row: str) -> list[str]:
    """Split a table row; a backslash escapes the pipe it stands before."""
    row = row.strip()
    if row.startswith('|'):
        row = row[1:]
    if row.endswith('|') and not row.endswith('\\|'):
        row = row[:-1]
    cells, current = [], []
    escaped = False
    for char in row:
        if escaped:
            current.append(char)
            escaped = False
        elif char == '\\':
            escaped = True
        elif char == '|':
            cells.append(''.join(current).strip())
            current = []
        else:
            current.append(char)
    if escaped:
        current.append('\\')
    cells.append(''.join(current).strip())
    return cells


def _norm(page: str, target: str) -> str:
    """Join a link target to its source directory, without host paths."""
    parts: list[str] = []
    for part in (Path(page).parent / target).parts:
        if part in ('', '.'):
            continue
        if part == '..':
            if parts:
                parts.pop()
            continue
        parts.append(part)
    return '/'.join(parts)


def _rewrite_link(dest: str, page: str, pages: dict[str, str]) -> tuple[str, list[str]]:
    """Map a repo-relative link to its built page; anchors ride along."""
    if not dest or dest.startswith('#') or EXTERNAL.match(dest):
        return dest, []
    target, hash_, frag = dest.partition('#')
    anchor = f'#{frag}' if hash_ else ''
    key = _norm(page, target) if target else None
    if key is None or key not in pages:
        return dest, [f'{page}: link to {dest} names no site page']
    return pages[key] + anchor, []


def _render_inline(text: str, page: str, pages: dict[str, str]) -> tuple[str, list[str]]:
    """Inline markup; problems when a repo-relative link names no page."""
    out: list[str] = []
    problems: list[str] = []
    i = 0
    while i < len(text):
        if text[i] == '`':
            end = text.find('`', i + 1)
            if end < 0:
                out.append(html.escape(text[i:]))
                break
            out.append(f'<code>{html.escape(text[i + 1 : end])}</code>')
            i = end + 1
        elif text[i : i + 2] in ('**', '__'):
            mark = text[i : i + 2]
            end = text.find(mark, i + 2)
            if end < 0:
                out.append(html.escape(text[i:]))
                break
            inner, more = _render_inline(text[i + 2 : end], page, pages)
            problems += more
            out.append(f'<strong>{inner}</strong>')
            i = end + 2
        elif text[i] in ('*', '_'):
            end = text.find(text[i], i + 1)
            if end < 0:
                out.append(html.escape(text[i:]))
                break
            inner, more = _render_inline(text[i + 1 : end], page, pages)
            problems += more
            out.append(f'<em>{inner}</em>')
            i = end + 1
        elif text[i] == '[' or text[i : i + 2] == '![':
            image = text[i] == '!'
            start = i + (2 if image else 1)
            depth, j = 1, start
            while j < len(text) and depth:
                depth += (text[j] == '[') - (text[j] == ']')
                j += 1
            if depth or j >= len(text) or text[j] != '(':
                out.append(html.escape(text[i]))
                i += 1
                continue
            end = text.find(')', j + 1)
            if end < 0:
                out.append(html.escape(text[i]))
                i += 1
                continue
            label, dest = text[start : j - 1], text[j + 1 : end]
            if image:
                out.append(f'<img alt="{html.escape(label)}" src="{html.escape(dest)}" />')
            else:
                inner, more = _render_inline(label, page, pages)
                problems += more
                href, bad = _rewrite_link(dest, page, pages)
                problems += bad
                out.append(f'<a href="{html.escape(href)}">{inner}</a>')
            i = end + 1
        else:
            out.append(html.escape(text[i]))
            i += 1
    return ''.join(out), problems


def _align(delim: str) -> str:
    """A GFM delimiter cell as a style attribute, empty when left aligned."""
    delim = delim.strip()
    if delim.startswith(':') and delim.endswith(':'):
        return ' style="text-align:center"'
    if delim.endswith(':'):
        return ' style="text-align:right"'
    return ''


def _render_blocks(
    lines: list[str], page: str, pages: dict[str, str]
) -> tuple[list[str], list[str], list[str]]:
    """Block markup; returns the HTML blocks, the heading ids, the problems."""
    blocks: list[str] = []
    headings: list[str] = []
    problems: list[str] = []
    paragraph: list[str] = []

    def flush_paragraph() -> None:
        if paragraph:
            inner, more = _render_inline(' '.join(paragraph), page, pages)
            problems.extend(more)
            blocks.append(f'<p>{inner}</p>')
            paragraph.clear()

    def add_heading(level: int, title: str) -> None:
        slug = slugify(STRIP_CODE.sub(r'\1', title))
        inner, more = _render_inline(title, page, pages)
        problems.extend(more)
        headings.append(slug)
        blocks.append(f'<h{level} id="{slug}">{inner}</h{level}>')

    i = 0
    while i < len(lines):
        line = lines[i]
        fence = FENCE.match(line)
        if fence:
            flush_paragraph()
            lang = fence.group(1)
            body = []
            i += 1
            while i < len(lines) and lines[i].strip() != '```':
                body.append(lines[i])
                i += 1
            if i == len(lines):
                return blocks, headings, problems + [f'{page}: fence never closes']
            i += 1
            cls = f' class="language-{lang}"' if lang else ''
            blocks.append(
                f'<pre><code{cls}>{html.escape(chr(10).join(body) + chr(10))}</code></pre>'
            )
            continue
        if HTML_COMMENT.match(line):
            flush_paragraph()
            comment = [line]
            i += 1
            while i < len(lines) and '-->' not in comment[-1]:
                comment.append(lines[i])
                i += 1
            blocks.append('\n'.join(comment))
            continue
        if not line.strip():
            flush_paragraph()
            i += 1
            continue
        stripped = line.strip()
        if stripped in ('===', '---') and len(paragraph) == 1:
            title = paragraph.pop()
            add_heading(1 if stripped == '===' else 2, title)
            i += 1
            continue
        if HR.match(line):
            flush_paragraph()
            blocks.append('<hr />')
            i += 1
            continue
        match = HEADING.match(line)
        if match:
            flush_paragraph()
            add_heading(len(match.group(1)), match.group(2))
            i += 1
            continue
        if line.startswith('>'):
            flush_paragraph()
            quote = [line[1:].lstrip()]
            i += 1
            while i < len(lines) and lines[i].startswith('>'):
                quote.append(lines[i][1:].lstrip())
                i += 1
            inner, heads, more = _render_blocks(quote, page, pages)
            problems.extend(more)
            headings.extend(heads)
            blocks.append('<blockquote>\n' + '\n'.join(inner) + '\n</blockquote>')
            continue
        if '|' in line and i + 1 < len(lines) and TABLE_DELIM.match(lines[i + 1].strip()):
            flush_paragraph()
            aligns = [_align(cell) for cell in _split_cells(lines[i + 1])]
            head_cells = []
            for k, cell in enumerate(_split_cells(line)):
                inner, more = _render_inline(cell, page, pages)
                problems.extend(more)
                head_cells.append(f'<th{aligns[k] if k < len(aligns) else ""}>{inner}</th>')
            rows = ['<tr>' + ''.join(head_cells) + '</tr>']
            i += 2
            while i < len(lines) and '|' in lines[i] and lines[i].strip():
                row_cells = []
                for cell in _split_cells(lines[i]):
                    inner, more = _render_inline(cell, page, pages)
                    problems.extend(more)
                    row_cells.append(f'<td>{inner}</td>')
                rows.append('<tr>' + ''.join(row_cells) + '</tr>')
                i += 1
            table = '<table>\n<thead>\n' + rows[0] + '\n</thead>\n<tbody>\n'
            blocks.append(table + '\n'.join(rows[1:]) + '\n</tbody>\n</table>')
            continue
        item = LIST_ITEM.match(line)
        if item:
            flush_paragraph()
            ordered = item.group(1)[0].isdigit()
            tag = 'ol' if ordered else 'ul'
            entries = []
            entry = [item.group(2)]
            i += 1
            while i < len(lines):
                more_item = LIST_ITEM.match(lines[i])
                if more_item and (more_item.group(1)[0].isdigit() == ordered):
                    entries.append(' '.join(entry))
                    entry = [more_item.group(2)]
                    i += 1
                    continue
                if (
                    more_item
                    or not lines[i].strip()
                    or FENCE.match(lines[i])
                    or lines[i].startswith(('>', '#', '|'))
                ):
                    break
                entry.append(lines[i].strip())
                i += 1
            entries.append(' '.join(entry))
            rendered = []
            for text_entry in entries:
                inner, more = _render_inline(text_entry, page, pages)
                problems.extend(more)
                rendered.append(f'<li>{inner}</li>')
            blocks.append(f'<{tag}>\n' + '\n'.join(rendered) + f'\n</{tag}>')
            continue
        paragraph.append(line.strip())
        i += 1
    flush_paragraph()
    return blocks, headings, problems


def render_markdown(
    text: str, page: str, pages: dict[str, str] | None = None
) -> tuple[str, list[str], list[str]]:
    """Render one page; headings carry GitHub-style anchors.

    Returns the body HTML, the heading ids in order, and the problems:
    repo-relative links that name no built page.
    """
    blocks, headings, problems = _render_blocks(text.splitlines(), page, dict(pages or {}))
    return '\n'.join(blocks), headings, problems


STYLE = (
    'body{font-family:sans-serif;max-width:60em;margin:0 auto;padding:0 1em;line-height:1.5}'
    'nav ul{list-style:none;padding:0;display:flex;flex-wrap:wrap;gap:0 1em}'
    'table{border-collapse:collapse}th,td{border:1px solid #999;padding:.2em .6em}'
    'pre{background:#f4f4f4;padding:.8em;overflow-x:auto}code{font-size:.95em}'
)


def wrap(title: str, nav: str, body: str) -> str:
    """One full page: nav to every page, then the rendered body."""
    return (
        '<!DOCTYPE html>\n'
        '<html lang="en">\n<head>\n<meta charset="utf-8" />\n'
        '<meta name="viewport" content="width=device-width,initial-scale=1" />\n'
        f'<title>{html.escape(title)} - ros2serial</title>\n'
        f'<style>{STYLE}</style>\n</head>\n<body>\n{nav}\n{body}\n</body>\n</html>\n'
    )


def build_site(root: Path, outdir: Path, pages: tuple[Page, ...] = PAGES) -> list[str]:
    """Render every page plus the landing index; problems, empty when green."""
    problems: list[str] = []
    mapping = {page.source: page.output for page in pages}
    nav = (
        '<nav><ul>'
        + ''.join(
            f'<li><a href="{page.output}">{html.escape(page.title)}</a></li>' for page in pages
        )
        + '</ul></nav>'
    )
    outdir.mkdir(parents=True, exist_ok=True)
    for page in pages:
        source = root / page.source
        if not source.is_file():
            problems.append(f'{page.source}: page source is missing')
            continue
        if source.suffix == '.md':
            body, _, bad = render_markdown(
                source.read_text(encoding='utf-8'), page.source, mapping
            )
            problems.extend(bad)
            (outdir / page.output).write_text(wrap(page.title, nav, body), encoding='utf-8')
        else:
            text = html.escape(source.read_text(encoding='utf-8'))
            (outdir / page.output).write_text(
                wrap(page.title, nav, f'<h1>{html.escape(page.title)}</h1>\n<pre>{text}</pre>'),
                encoding='utf-8',
            )
    index_body = (
        '<h1>ros2serial documentation</h1>\n<ul>'
        + ''.join(
            f'<li><a href="{page.output}">{html.escape(page.title)}</a> - {html.escape(page.blurb)}</li>'
            for page in pages
        )
        + '</ul>'
    )
    (outdir / 'index.html').write_text(wrap('Documentation', nav, index_body), encoding='utf-8')
    return problems


def main(argv: list[str] | None = None) -> int:
    """Build the site; --out selects the directory, default build/site."""
    parser = argparse.ArgumentParser(description='Build the static docs site.')
    parser.add_argument('--out', default=str(ROOT / 'build' / 'site'))
    args = parser.parse_args(argv)
    problems = build_site(ROOT, Path(args.out))
    for problem in problems:
        print(problem)
    if problems:
        return 1
    print(f'site: {len(PAGES)} pages and the index in {args.out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
