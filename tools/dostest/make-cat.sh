#!/usr/bin/env bash
#
# make-cat.sh -- emit cat.com, a DOS .COM that copies STDIN to STDOUT one byte at
# a time through INT 21h AH=08 (read, no echo) and AH=02 (write). GH #131.
#
# It exists to test the INPUT half of shell redirection the way hello.com tests
# the output half: `cat.com < in.txt > out.txt` must reproduce in.txt exactly,
# under NTVDMEX and under stock ntvdm alike.
#
# ⚠ AH=08 AND AH=02, NOT the handle calls (3Fh/40h), ON PURPOSE. Those go
#   straight to the DOS file layer; 08/02 go through the CONSOLE, which is the
#   path this issue is about and the one that had no redirected handle behind it.
#
# Hand-assembled (org 0x100):
#   100: B4 08       mov ah, 08h      ; read stdin, no echo
#   102: CD 21       int 21h
#   104: 3C 1A       cmp al, 1Ah      ; Ctrl-Z = end of a redirected input
#   106: 74 09       je  0x111
#   108: 8A D0       mov dl, al
#   10A: B4 02       mov ah, 02h      ; write to stdout
#   10C: CD 21       int 21h
#   10E: EB F0       jmp 0x100
#   110: 90          nop
#   111: B8 00 4C    mov ax, 4C00h
#   114: CD 21       int 21h
set -euo pipefail
out="$(dirname "$0")/cat.com"
printf '\xB4\x08\xCD\x21\x3C\x1A\x74\x09\x8A\xD0\xB4\x02\xCD\x21\xEB\xF0\x90\xB8\x00\x4C\xCD\x21' > "$out"
echo "wrote $out ($(wc -c < "$out") bytes)"
