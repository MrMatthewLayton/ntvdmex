#!/usr/bin/env bash
#
# Emit dosstub.com -- a 4-byte 16-bit DOS .COM that just exits cleanly:
#   B4 4C        mov ah, 4Ch     ; DOS "terminate with return code"
#   CD 21        int 21h
#
# Its only job is to be a *16-bit* image so that launching it makes XP route to
# the VDM support process (which Spike-002 has repointed at wowprobe.exe). The
# stub's own code never actually runs in that test -- wowprobe replaces ntvdm.
#
set -euo pipefail
out="$(dirname "$0")/dosstub.com"
printf '\xB4\x4C\xCD\x21' > "$out"
echo "wrote $out ($(wc -c < "$out") bytes)"
