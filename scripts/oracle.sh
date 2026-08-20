#!/usr/bin/env bash
# Ask the MS-DOS 6.22 oracle.  GH #25.
#
#   ./scripts/oracle.sh tools/dostest/dosver.com     run a guest binary
#   ./scripts/oracle.sh --batch "VER"                run DOS commands
#   ./scripts/oracle.sh --selftest                   prove the oracle answers
#   ./scripts/oracle.sh --interactive                a real prompt
#
# Build the image once with scripts/dosoracle/build.py.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
oracle="$here/dosoracle/dosoracle.py"

if [[ $# -gt 0 && -f "$1" ]]; then
    # A bare path is the common case: run that program on real DOS.
    exec python3 "$oracle" run "$@"
fi
exec python3 "$oracle" "$@"
