#!/usr/bin/env bash
#
# make-keytest.sh -- emit keytest.com, an interactive key-echo DOS .COM. It reads
# keys with INT 21h AH=08 (no echo) and echoes them itself via AH=02 so it can do
# proper line editing: Enter -> CR+LF (new line), Backspace -> BS,space,BS (erase),
# ESC -> exit. The M3 keyboard gate: keys travel UI WM_CHAR -> input VDD ring ->
# INT 21h -> the video VDD -> the Luna window.
#
# Hand-assembled (org 0x100); see byte map below.
#   loop: AH=08 int21; cmp ESC->done; cmp CR-> emit CR,LF; cmp BS-> emit BS,' ',BS;
#         else echo char; done: AX=4C00 int21.
#
set -euo pipefail
out="$(dirname "$0")/keytest.com"
{
  # 100: read (no echo) + ESC/CR/BS dispatch
  printf '\xB4\x08\xCD\x21\x3C\x1B\x74\x32\x3C\x0D\x75\x0E'   # 100..10B
  # 10C: Enter -> print CR then LF, jmp loop
  printf '\xB2\x0D\xB4\x02\xCD\x21\xB2\x0A\xB4\x02\xCD\x21\xEB\xE6'  # 10C..119
  # 11A: Backspace? -> print BS, space, BS, jmp loop
  printf '\x3C\x08\x75\x14'                                   # 11A..11D
  printf '\xB2\x08\xB4\x02\xCD\x21'                           # 11E..123  BS
  printf '\xB2\x20\xB4\x02\xCD\x21'                           # 124..129  space
  printf '\xB2\x08\xB4\x02\xCD\x21'                           # 12A..12F  BS
  printf '\xEB\xCE'                                           # 130..131  jmp loop
  # 132: normal char -> echo it, jmp loop
  printf '\x88\xC2\xB4\x02\xCD\x21\xEB\xC6'                   # 132..139
  # 13A: exit 0
  printf '\xB8\x00\x4C\xCD\x21'                               # 13A..13E
} > "$out"
sz=$(wc -c < "$out")
echo "wrote $out ($sz bytes)"
[ "$sz" -eq 63 ] || { echo "ERROR: expected 63 bytes, got $sz" >&2; exit 1; }
