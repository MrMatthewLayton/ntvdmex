#!/usr/bin/env bash
#
# make-hello.sh -- emit hello.com, a DOS .COM that prints a multi-line message via
# INT 21h AH=09 ($-terminated string) and exits. Used to eyeball the M3 merge:
# its output must paint in the Luna window via the INT 21h -> video VDD path.
#
# Hand-assembled (org 0x100):
#   100: B4 09         mov ah, 09h
#   102: BA 0C 01      mov dx, 010Ch        ; -> msg (just past this 12-byte stub)
#   105: CD 21         int 21h              ; print $-string
#   107: B8 00 4C      mov ax, 4C00h
#   10A: CD 21         int 21h              ; exit 0
#   10C: msg db "...$"
#
set -euo pipefail
out="$(dirname "$0")/hello.com"
printf '\xB4\x09\xBA\x0C\x01\xCD\x21\xB8\x00\x4C\xCD\x21' > "$out"
printf 'NTVDMEX text mode 3 -- DOS in a Luna window!\r\n'      >> "$out"
printf '0123456789  ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n'           >> "$out"
printf 'abcdefghijklmnopqrstuvwxyz  {}[]()<>#@!*=+\r\n'       >> "$out"
printf '\r\nC:\\>$'                                            >> "$out"
sz=$(wc -c < "$out")
echo "wrote $out ($sz bytes)"
