#!/usr/bin/env bash
#
# mkfloppy.sh -- build the 1.44 MB FAT12 image that backs INT 13h drive 0.
#
# ── WHY THIS EXISTS ──────────────────────────────────────────────────────────
# src/dos/dos_disk.h states the design decision plainly: "a drive is a disk IMAGE
# FILE, or it is absent. Nothing is synthesised." The INT 13h layer is fully
# implemented against that model and unit-tested (tools/dostest/disk_test.c), but
# with no image on the rig it correctly answered "drive not ready" to everything
# -- and p_disk, whose oracle boots off a genuine 1.44 MB floppy, reported 13
# mismatches that read like an unimplemented BIOS.
#
# ⚠ THEY WERE NOT A GAP. Given a disk, all 18 rows AGREE -- geometry, drive type,
#   boot sector and INT 25h alike. The registers that looked "left as poison"
#   were a call that had correctly FAILED, and a failed call does not write them.
#   ▶ Before calling a surface unimplemented, check it has something to work on.
#
# The geometry is deliberately the oracle's: mformat -f 1440 gives 18 sectors x
# 2 heads x 80 cylinders, which is what 6.22 reports as CX=4F12 DX=0101 BX=0004.
#
#   ./scripts/mkfloppy.sh                      # -> build/FLOPPY.IMG
#   ./scripts/mkfloppy.sh /tmp/xpshare/cfg/FLOPPY.IMG
#
# Needs mtools (mformat/mcopy) -- the same dependency scripts/dosoracle already
# has for building the oracle's own scratch disk.
#
# ⚠ TWO RUNS ARE NOT BYTE-IDENTICAL, AND THAT IS NOT A FAULT. mformat stamps a
#   time-based VOLUME SERIAL at BPB offset 0x27, and the file's timestamp lands in
#   the root directory entry -- 8 bytes differ between builds. The GEOMETRY, which
#   is the whole contract here, is identical and is asserted below. Do not go
#   looking for a corrupted image because the md5 moved.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/build/FLOPPY.IMG}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

command -v mformat >/dev/null || { echo "mtools not found (brew install mtools)" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
# truncate(1) is not portable to macOS; python is already a dependency here.
python3 -c "open('$OUT','wb').truncate(1474560)"
mformat -i "$OUT" -f 1440 -v NTVDMEXDSK ::

# A file with known contents, so a guest that reads the data area can prove it is
# reading THIS disk rather than succeeding against something else.
printf 'NTVDMEX INT13 TEST SECTOR\r\n' > "$TMP/HELLO.TXT"
mcopy -i "$OUT" -o "$TMP/HELLO.TXT" ::/HELLO.TXT

python3 - "$OUT" <<'PY'
import sys
b = open(sys.argv[1], 'rb').read(512)
bps  = b[11] | b[12] << 8
spt  = b[24] | b[25] << 8
head = b[26] | b[27] << 8
tot  = b[19] | b[20] << 8
cyl  = tot // (spt * head) if spt and head else 0
assert bps == 512 and spt == 18 and head == 2 and tot == 2880, "unexpected BPB"
assert b[510] == 0x55 and b[511] == 0xAA, "no boot signature"
print(f"  {sys.argv[1]}")
print(f"  {cyl} cyl x {head} heads x {spt} sec, {bps} b/sec, sig 55AA"
      f"   -> INT 13h AH=08h should answer CX={cyl-1:02X}{spt:02X} DX=01{head-1:02X}")
PY
