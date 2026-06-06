#!/usr/bin/env bash
#
# Build + run the off-VM DOS-kernel unit battery on the build host (Layer 1 of
# the M2.4 test plan). Pure C, no Windows/VM dependency -- uses the native cc.
#
#   ./tools/dostest/run.sh
#
# Exits nonzero if any case fails, so it can gate a commit or CI step.
#
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/dos" \
   -o "$DIR/mcb_test" "$DIR/mcb_test.c"

"$DIR/mcb_test"
