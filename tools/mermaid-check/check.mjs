// check.mjs: parse every fenced mermaid block in the tracked markdown with
// mermaid's own parser, so a block that fails here fails on GitHub too.
// Runs under the pinned node image (see scripts/lint-docs.sh), never in
// the dev image. Prints file and line for each failure; exit 1 when any
// block fails, 0 after listing what passed.

import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { JSDOM } from 'jsdom';

// mermaid reads the DOM at import time, so the jsdom globals go first and
// mermaid itself loads on a dynamic import below.
const dom = new JSDOM('<!DOCTYPE html><html><body></body></html>');
globalThis.window = dom.window;
globalThis.document = dom.window.document;
Object.defineProperty(globalThis, 'navigator', { value: dom.window.navigator, configurable: true });
globalThis.Element = dom.window.Element;
globalThis.HTMLElement = dom.window.HTMLElement;
globalThis.SVGElement = dom.window.SVGElement;
globalThis.Node = dom.window.Node;
globalThis.getComputedStyle = dom.window.getComputedStyle;

const mermaid = (await import('mermaid')).default;
mermaid.initialize({ startOnLoad: false });

const EM_DASH = '\u2014';
const OPEN = /^ {0,3}```mermaid[ \t]*$/;
const ANY_FENCE = /^ {0,3}```/;
const CLOSE = /^ {0,3}```[ \t]*$/;

export function extractBlocks(text) {
  const lines = text.split('\n');
  const blocks = [];
  const problems = [];
  let i = 0;
  while (i < lines.length) {
    if (!OPEN.test(lines[i])) {
      i = ANY_FENCE.test(lines[i]) ? skipFence(lines, i) : i + 1;
      continue;
    }
    const start = i + 1;
    const body = [];
    i += 1;
    while (i < lines.length && !CLOSE.test(lines[i])) {
      body.push(lines[i]);
      i += 1;
    }
    if (i >= lines.length) {
      problems.push({ line: start, message: 'fence never closes' });
      break;
    }
    i += 1;
    blocks.push({ line: start, code: `${body.join('\n')}\n` });
  }
  return { blocks, problems };
}

function skipFence(lines, i) {
  i += 1;
  while (i < lines.length && !CLOSE.test(lines[i])) {
    i += 1;
  }
  return Math.min(i + 1, lines.length);
}

export async function validateDiagram(code) {
  const problems = [];
  code.split('\n').forEach((text, index) => {
    if (text.includes(EM_DASH)) {
      problems.push({ line: index + 1, message: 'em-dash; use a comma, a colon or a period' });
    }
  });
  try {
    await mermaid.parse(code);
  } catch (err) {
    const first = String(err?.message ?? err).split('\n')[0];
    problems.push({ line: null, message: `mermaid parse error: ${first}` });
  }
  return problems;
}

export async function checkFile(rel, text) {
  const found = [];
  const { blocks, problems } = extractBlocks(text);
  for (const problem of problems) {
    found.push(`${rel}:${problem.line}: ${problem.message}`);
  }
  for (const block of blocks) {
    for (const problem of await validateDiagram(block.code)) {
      const line = problem.line === null ? block.line : block.line + problem.line - 1;
      found.push(`${rel}:${line}: ${problem.message}`);
    }
  }
  return { count: blocks.length, found };
}

function trackedMarkdown(root) {
  try {
    const out = execFileSync('git', ['ls-files', '-z', '*.md'], { cwd: root, encoding: 'utf8' });
    return out.split('\0').filter((path) => path.length > 0);
  } catch (err) {
    console.error(`mermaid check: cannot list tracked markdown: ${String(err?.message ?? err).split('\n')[0]}`);
    process.exit(1);
  }
  return [];
}

// Paths come from the caller (scripts/lint-docs.sh lists the tracked files
// with the host git, which the node image does not carry). With no paths,
// fall back to git ls-files, so the check also runs where git exists.
async function main() {
  const root = fileURLToPath(new URL('../..', import.meta.url));
  const given = process.argv.slice(2);
  const paths = given.length > 0 ? given : trackedMarkdown(root);
  let blocks = 0;
  const found = [];
  for (const rel of paths) {
    const text = readFileSync(`${root}/${rel}`, 'utf8');
    const result = await checkFile(rel, text);
    blocks += result.count;
    found.push(...result.found);
  }
  for (const line of found) {
    console.log(line);
  }
  if (found.length > 0) {
    console.log(`mermaid: ${found.length} problems in ${blocks} blocks across ${paths.length} files`);
    process.exit(1);
  }
  console.log(`mermaid: ${blocks} blocks in ${paths.length} files parsed`);
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  await main();
}
