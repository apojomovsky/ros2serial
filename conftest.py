"""Every pytest run starts without the caller's repository-local git variables.

Git exports GIT_DIR, GIT_INDEX_FILE and the like to hooks; a test that builds a
throwaway repo would otherwise run its git commands on the real repository.
"""

import os
import subprocess

try:
    _vars = subprocess.run(["git", "rev-parse", "--local-env-vars"], capture_output=True, text=True).stdout.split()
except FileNotFoundError:
    _vars = []  # no git, so nothing could have been exported for it
for _var in _vars:
    os.environ.pop(_var, None)
