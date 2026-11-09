// Copyright (c) 2026 Alexis Pojomovsky
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { checkFile, extractBlocks, validateDiagram } from './check.mjs';

const FLOWCHART = 'flowchart LR\n    A --> B\n';
const SEQUENCE = 'sequenceDiagram\n    Alice->>John: Hello John\n';
const STATE = 'stateDiagram-v2\n    [*] --> Active\n';
const UNCLOSED = 'flowchart LR\n    A[unclosed --> B\n';
const UNKNOWN = 'notadiagram foo\n    A --> B\n';
const NO_TARGET = 'sequenceDiagram\n    Alice->\n';
const EM_DASH = 'flowchart LR\n    A -- text \u2014 more --> B\n';

describe('validateDiagram', () => {
  for (const [name, code] of Object.entries({ flowchart: FLOWCHART, sequence: SEQUENCE, state: STATE })) {
    it(`passes a valid ${name} diagram`, async () => {
      assert.deepEqual(await validateDiagram(code), []);
    });
  }

  it('fails an unclosed node bracket with a line number', async () => {
    const problems = await validateDiagram(UNCLOSED);
    assert.equal(problems.length, 1);
    assert.match(problems[0].message, /line \d+/i);
  });

  it('fails an unknown diagram type', async () => {
    const problems = await validateDiagram(UNKNOWN);
    assert.equal(problems.length, 1);
    assert.match(problems[0].message, /no diagram type/i);
  });

  it('fails a sequence message with no target', async () => {
    const problems = await validateDiagram(NO_TARGET);
    assert.equal(problems.length, 1);
  });

  it('fails an em-dash even though mermaid would render it', async () => {
    const problems = await validateDiagram(EM_DASH);
    assert.equal(problems.length, 1);
    assert.match(problems[0].message, /em-dash/);
    assert.equal(problems[0].line, 2);
  });
});

describe('extractBlocks', () => {
  it('collects mermaid blocks with their opening line numbers', () => {
    const text = ['# T', '', '```mermaid', FLOWCHART, '```', '', '```python', 'pass', '```'].join('\n');
    const { blocks, problems } = extractBlocks(text);
    assert.deepEqual(problems, []);
    assert.equal(blocks.length, 1);
    assert.equal(blocks[0].line, 3);
    assert.equal(blocks[0].code, `${FLOWCHART}\n`);
  });

  it('accepts a fence indented up to three columns', () => {
    const { blocks, problems } = extractBlocks(`   \`\`\`mermaid\n${FLOWCHART}\`\`\`\n`);
    assert.deepEqual(problems, []);
    assert.equal(blocks.length, 1);
  });

  it('ignores a fence nested deeper than three columns', () => {
    const { blocks, problems } = extractBlocks(`    \`\`\`mermaid\n${FLOWCHART}    \`\`\`\n`);
    assert.deepEqual(problems, []);
    assert.deepEqual(blocks, []);
  });

  it('reports a fence that never closes', () => {
    const { blocks, problems } = extractBlocks(`\`\`\`mermaid\n${FLOWCHART}`);
    assert.deepEqual(blocks, []);
    assert.equal(problems.length, 1);
    assert.equal(problems[0].line, 1);
  });
});

describe('checkFile', () => {
  it('prefixes problems with the file and the absolute line', async () => {
    const text = ['# T', '', '```mermaid', ...UNCLOSED.split('\n'), '```'].join('\n');
    const { found } = await checkFile('docs/x.md', text);
    assert.equal(found.length, 1);
    assert.match(found[0], /^docs\/x\.md:3: /);
  });

  it('maps an in-block problem to the absolute line', async () => {
    const text = ['# T', '', '```mermaid', ...EM_DASH.split('\n'), '```'].join('\n');
    const { found } = await checkFile('docs/x.md', text);
    assert.equal(found.length, 1);
    assert.match(found[0], /^docs\/x\.md:5: .*em-dash/);
  });
});
