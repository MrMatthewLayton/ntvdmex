#!/usr/bin/env bash
#
# make-argtest.sh -- emit argtest.com, a DOS .COM that echoes its PSP command tail
# and exits with errorlevel = tail length. Proves M2.5 plumbing end-to-end: the
# host must build the PSP command tail (DS:0x80) for the guest to read, and the
# AH=4Ch exit code must propagate. Verified against dosbox-x (verify-argtest.sh).
#
# Hand-assembled (org 0x100; PSP tail length at DS:0x80, bytes at DS:0x81):
#   100: BE 81 00      mov si, 0x81        ; -> first tail byte
#   103: 8A 0E 80 00   mov cl, [0x80]      ; tail length
#   107: 08 C9      ploop: or cl, cl
#   109: 74 0B            jz done
#   10B: 8A 14            mov dl, [si]
#   10D: B4 02            mov ah, 02h       ; print char
#   10F: CD 21            int 21h
#   111: 46               inc si
#   112: FE C9            dec cl
#   114: EB F1            jmp ploop
#   116: A0 80 00   done:  mov al, [0x80]   ; errorlevel = tail length
#   119: B4 4C            mov ah, 4Ch
#   11B: CD 21            int 21h           (29 bytes)
#
set -euo pipefail
out="$(dirname "$0")/argtest.com"
printf '\xBE\x81\x00\x8A\x0E\x80\x00\x08\xC9\x74\x0B\x8A\x14\xB4\x02\xCD\x21' >  "$out"
printf '\x46\xFE\xC9\xEB\xF1\xA0\x80\x00\xB4\x4C\xCD\x21'                     >> "$out"
sz=$(wc -c < "$out")
echo "wrote $out ($sz bytes)"
[ "$sz" -eq 29 ] || { echo "ERROR: expected 29 bytes, got $sz" >&2; exit 1; }
