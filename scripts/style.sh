#!/usr/bin/env bash
#
# style.sh -- check or apply the house style (docs/STYLE.md).
#
#   scripts/style.sh --check [FILE...]   exit 1, with the diff, if a file is not in the style
#   scripts/style.sh --fix [FILE...]     rewrite the files that are not
#   scripts/style.sh --test              the style tool's own tests
#
# With no FILE, every C source and header under src/, sdk/, tests/ and tools/. Every change
# the tool makes is proven not to change the code; see tools/style/style.py.
#
# To check the files you are about to commit, on every commit:
#
#   git config core.hooksPath scripts/hooks
#
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "${1:-}" == "--test" ]]; then
    exec python3 tools/style/test_style.py
fi

exec python3 tools/style/style.py "$@"
