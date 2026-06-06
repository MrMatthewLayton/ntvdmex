#!/usr/bin/env bash
#
# make-memtest.sh -- emit memtest.com, a self-checking DOS .COM that exercises
# the M2.4 memory allocator (INT 21h AH=48/49/4A) and reports a verdict the
# harness can read two ways: it prints "MEMTEST PASS"/"MEMTEST FAIL" (AH=09, which
# vdmhost captures into vdmhost.log) AND exits with errorlevel = number of failed
# checks (AH=4Ch AL=fails). Designed to PASS on any correct DOS (verified against
# dosbox-x, see verify-memtest.sh) and on vdmhost's implemented allocator subset.
# It deliberately does NOT depend on merge-on-alloc (vdmhost's known gap, pinned
# by mcb_test T9), so a PASS is meaningful on both.
#
# Hand-assembled (the repo has no DOS assembler; cf. tools/wowprobe/make-dosstub.sh).
# org 0x100; entry CS=DS=ES=SS=PSP, SP=0xFFFE. Listing (offset: bytes  asm):
#
#   ; --- T1: shrink our own program block so memory is free to allocate -------
#   100: 8C C8         mov ax, cs            ; ES = our PSP segment (.COM owns all mem)
#   102: 8E C0         mov es, ax
#   104: B4 4A         mov ah, 4Ah          ; resize ES to 0x1000 paras (keep our 64KB)
#   106: BB 00 10      mov bx, 1000h
#   109: CD 21         int 21h
#   10B: 73 03         jnc  +3              ; CF=0 expected
#   10D: E8 68 00      call markfail
#   ; --- T2: allocate 0x100 paragraphs ----------------------------------------
#   110: B4 48         mov ah, 48h
#   112: BB 00 01      mov bx, 0100h
#   115: CD 21         int 21h
#   117: 72 05         jc   t2fail          ; CF=0 expected (AX = segment)
#   119: A3 7E 01      mov  [savedseg], ax
#   11C: EB 09         jmp  t2ok
#   11E: E8 57 00  t2fail: call markfail
#   121: C7 06 7E 01 00 00  mov word [savedseg], 0
#   ; --- T3: shrink the allocated block to 0x80 paras --------------------------
#   127: A1 7E 01  t2ok: mov ax, [savedseg]
#   12A: 09 C0         or   ax, ax
#   12C: 74 0E         jz   t3skip          ; skip if alloc failed (already counted)
#   12E: 8E C0         mov  es, ax
#   130: B4 4A         mov  ah, 4Ah
#   132: BB 80 00      mov  bx, 0080h
#   135: CD 21         int  21h
#   137: 73 03         jnc  +3              ; CF=0 expected
#   139: E8 3C 00      call markfail
#   ; --- T4: free the block ----------------------------------------------------
#   13C: A1 7E 01  t3skip: mov ax, [savedseg]
#   13F: 09 C0         or   ax, ax
#   141: 74 0B         jz   t5              ; skip if no block
#   143: 8E C0         mov  es, ax
#   145: B4 49         mov  ah, 49h
#   147: CD 21         int  21h
#   149: 73 03         jnc  +3              ; CF=0 expected
#   14B: E8 2A 00      call markfail
#   ; --- T5: an absurd allocation must FAIL (CF=1) -----------------------------
#   14E: B4 48     t5: mov ah, 48h
#   150: BB FF FF      mov bx, 0FFFFh
#   153: CD 21         int 21h
#   155: 72 03         jc  verdict          ; CF=1 expected (correctly refused)
#   157: E8 1E 00      call markfail        ; CF=0 = wrongly succeeded
#   ; --- verdict: print PASS/FAIL, exit with errorlevel = fails ----------------
#   15A: A0 7D 01  verdict: mov al, [fails]
#   15D: 08 C0         or   al, al
#   15F: 75 09         jnz  sayfail
#   161: BA 80 01      mov  dx, msgpass
#   164: B4 09         mov  ah, 09h
#   166: CD 21         int  21h
#   168: EB 07         jmp  doexit
#   16A: BA 8F 01  sayfail: mov dx, msgfail
#   16D: B4 09         mov  ah, 09h
#   16F: CD 21         int  21h
#   171: A0 7D 01  doexit: mov al, [fails]
#   174: B4 4C         mov  ah, 4Ch         ; terminate, AL = errorlevel
#   176: CD 21         int  21h
#   178: FE 06 7D 01  markfail: inc byte [fails]
#   17C: C3           ret
#   17D: 00           fails    db 0
#   17E: 00 00        savedseg dw 0
#   180: "MEMTEST PASS",0D,0A,'$'   msgpass
#   18F: "MEMTEST FAIL",0D,0A,'$'   msgfail   (end 0x19E; total 158 bytes)
#
set -euo pipefail
out="$(dirname "$0")/memtest.com"

printf '\x8C\xC8\x8E\xC0\xB4\x4A\xBB\x00\x10\xCD\x21\x73\x03\xE8\x68\x00' >  "$out"
printf '\xB4\x48\xBB\x00\x01\xCD\x21\x72\x05\xA3\x7E\x01\xEB\x09\xE8\x57\x00' >> "$out"
printf '\xC7\x06\x7E\x01\x00\x00\xA1\x7E\x01\x09\xC0\x74\x0E\x8E\xC0\xB4\x4A' >> "$out"
printf '\xBB\x80\x00\xCD\x21\x73\x03\xE8\x3C\x00\xA1\x7E\x01\x09\xC0\x74\x0B' >> "$out"
printf '\x8E\xC0\xB4\x49\xCD\x21\x73\x03\xE8\x2A\x00\xB4\x48\xBB\xFF\xFF\xCD\x21' >> "$out"
printf '\x72\x03\xE8\x1E\x00\xA0\x7D\x01\x08\xC0\x75\x09\xBA\x80\x01\xB4\x09\xCD\x21' >> "$out"
printf '\xEB\x07\xBA\x8F\x01\xB4\x09\xCD\x21\xA0\x7D\x01\xB4\x4C\xCD\x21' >> "$out"
printf '\xFE\x06\x7D\x01\xC3\x00\x00\x00' >> "$out"
printf 'MEMTEST PASS\r\n$' >> "$out"
printf 'MEMTEST FAIL\r\n$' >> "$out"

sz=$(wc -c < "$out")
echo "wrote $out ($sz bytes)"
[ "$sz" -eq 158 ] || { echo "ERROR: expected 158 bytes, got $sz" >&2; exit 1; }
