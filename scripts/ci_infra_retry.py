# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Decide whether a failed master run died of infrastructure trouble.

The retry workflow downloads each failed job's log and asks this script
whether the whole set reads as infrastructure: an HTTP 5xx with no sign
that the tests ran. A test failure, a mixed log or an unreadable log
refuses the retry, so real failures always keep their conclusion.
"""

import re
import sys
from pathlib import Path

# A proxy or registry answering 5xx: the upload, image pull or API call
# failed before any test could report.
INFRA_PATTERNS = (
    r'HTTP[/ ]5\d\d',
    r'(status|error)[^\n]{0,30}\b5\d\d\b',
    r'Service Unavailable',
    r'Bad Gateway',
    r'Gateway Timeout',
    r'Internal Server Error',
)

# Markers only a running suite prints: seeing one means tests reported,
# so the failure is real even beside a 5xx.
TEST_PATTERNS = (
    r'\bFAILED\b',
    r'^FAIL:',
    r'not ok ',
    r'AssertionError',
    r'Traceback \(most recent call last\)',
    r'test result: FAILED',
    r'^Failures:',
    r'^Errors:',
    r'Failed tests:',
)

INFRA = [re.compile(pattern, re.IGNORECASE) for pattern in INFRA_PATTERNS]
TESTS = [re.compile(pattern, re.MULTILINE) for pattern in TEST_PATTERNS]


def classify_log(text):
    """Read one failed job log as infra, test or unknown."""
    if any(pattern.search(text) for pattern in TESTS):
        return 'test'
    if any(pattern.search(text) for pattern in INFRA):
        return 'infra'
    return 'unknown'


def should_retry(texts):
    """Retry only a non-empty set where every log reads as infrastructure."""
    return bool(texts) and all(classify_log(text) == 'infra' for text in texts)


def decide(paths):
    """Exit 0 to rerun, 1 to keep the conclusion, 2 on a usage error."""
    if not paths:
        print('ci_infra_retry: no logs to read', file=sys.stderr)
        return 2
    texts = []
    for path in paths:
        try:
            texts.append(Path(path).read_text(encoding='utf-8', errors='replace'))
        except OSError as exc:
            print(f'ci_infra_retry: cannot read {path}: {exc}', file=sys.stderr)
            return 1
    for path, text in zip(paths, texts, strict=True):
        print(f'ci_infra_retry: {path} reads as {classify_log(text)}')
    return 0 if should_retry(texts) else 1


def main(argv=None):
    return decide((argv or sys.argv)[1:])


if __name__ == '__main__':
    sys.exit(main())
