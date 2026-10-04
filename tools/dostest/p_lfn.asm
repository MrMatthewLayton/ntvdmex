; p_lfn.com -- INT 21h AH=71h, the Windows 95 long-filename API (#210).
;
; STOCK NTVDM IS THE ORACLE HERE, NOT 6.22. MS-DOS 6.22 has no LFN API (it answers
; AX=7100h with CF CLEAR -- p_subfn int21.716C), so this probe is only meaningful as
; a pair: stock ntvdm and ours on the rig, same redirect --
;
;     scripts/dospair.sh tools/dostest/p_lfn.com
;
; Every UNMEASURED choice src/dos/dos_lfn.h and the AH=71h arm in dos_int21.c make is
; a row here: which registers a successful call writes (SIG includes AX wherever stock
; might or might not touch it), whether 71A7h converts to local time, whether 71A8h
; adds a "~1" tail, the find record's attribute DWORD and the high dword of a DOS-
; format time, the codes the short-name twins answer on failure.
;
; Paths that depend on WHERE the probe runs (7147h, 7160h) are printed as their LAST
; COMPONENT only, so the two runs compare even if their directories differ.
;
; SELF-CLEANING: it creates "Long Directory Name" and "A long file name.txt" (renamed
; to "Another long name.txt") in the current directory and removes both, plus the
; scratch ZZ6CNEW.TMP for the 6Ch row.
;
; nasm -f bin p_lfn.asm -o p_lfn.com

        org     100h
        jmp     start
%include "probe.inc"

; clear `buf` (300 bytes) so a BUF= dump never shows the previous call's answer
%macro CLRBUF 0
        push    di
        push    cx
        mov     di, buf
        mov     cx, 300
        xor     al, al
        cld
        rep     stosb
        pop     cx
        pop     di
%endmacro

; SI = ASCIIZ path -> [tailp] = the character after its last backslash
lasttail:
        mov     bx, si
.l:     lodsb
        or      al, al
        jz      .d
        cmp     al, '\'
        jne     .l
        mov     bx, si
        jmp     .l
.d:     mov     [tailp], bx
        ret

start:
        PROBE_BEGIN "lfn"

        ; ---- 71A0h get volume information: the call LFN clients DETECT the API with.
        ; BX flags (4000h = LFN functions supported), CX longest name, DX longest path,
        ; ES:DI the file-system name. AX in the SIG: does stock leave it 71A0h?
        CLRBUF
        mov     ax, 71A0h
        mov     bx, 0B1B1h
        mov     cx, 32
        mov     dx, root
        mov     di, buf
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.71A0.C", "AX,BX,CX,DX,CF"
        EMIT_BUF "lfn.71A0.fsname", buf, 8

        ; ---- 7139h mkdir, a long name; then again (it now exists)
        POISON
        mov     ax, 7139h
        mov     dx, ldir
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.7139.mkdir", "AX,CF"

        POISON
        mov     ax, 7139h
        mov     dx, ldir
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.7139.again", "AX,CF"

        ; ---- 713Bh chdir into it, 7147h reads it back LONG
        POISON
        mov     ax, 713Bh
        mov     dx, ldir
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.713B.into", "AX,CF"

        CLRBUF
        mov     ax, 7147h
        mov     bx, 0B1B1h
        mov     cx, 0C1C1h
        mov     dx, 0                           ; DL=0: the default drive
        mov     si, buf
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.7147.cwd", "AX,CF"
        mov     si, buf
        call    lasttail
        EMIT_BUF "lfn.7147.tail", [tailp], 20
        ; the short-name 47h, beside it: 8.3 upper case is what DOS keeps
        CLRBUF
        mov     ax, 4700h
        mov     dx, 0
        mov     si, buf
        int     21h
        call    probe_capture
        mov     si, buf
        call    lasttail
        EMIT_BUF "lfn.47.tail", [tailp], 9

        POISON
        mov     ax, 713Bh
        mov     dx, dotdot
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.713B.back", "AX,CF"

        POISON
        mov     ax, 713Bh
        mov     dx, nodir
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.713B.missing", "AX,CF"

        ; ---- 716Ch create a long name: BX=2 read/write, CX=0, DX=12h (truncate if it
        ; exists, create if not), DS:SI name, DI=0 alias hint. CX = action taken (2).
        mov     ax, 716Ch
        mov     bx, 0002h
        xor     cx, cx
        mov     dx, 0012h
        mov     si, lfile
        xor     di, di
        stc
        int     21h
        call    probe_capture
        mov     [fh], ax
        EMIT    "lfn.716C.create", "CX,CF"

        mov     bx, [fh]                        ; write 5 bytes through 40h
        mov     ax, 4000h
        mov     cx, 5
        mov     dx, hello
        int     21h
        call    probe_capture
        EMIT    "lfn.40.write", "AX,CF"

        ; ---- 71A6h file info by handle: the 52-byte BY_HANDLE_FILE_INFORMATION
        CLRBUF
        mov     ax, 71A6h
        mov     bx, [fh]
        mov     dx, buf
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.71A6", "AX,CF"
        EMIT_BUF "lfn.71A6.attr", buf, 4
        EMIT_BUF "lfn.71A6.sizehi.lo.links", buf+20h, 12

        mov     bx, [fh]
        mov     ax, 3E00h
        int     21h

        ; ---- 716Ch open it again, DX=01h (open; fail if missing): CX = 1 opened
        mov     ax, 716Ch
        xor     bx, bx
        xor     cx, cx
        mov     dx, 0001h
        mov     si, lfile
        xor     di, di
        stc
        int     21h
        call    probe_capture
        mov     [fh], ax
        EMIT    "lfn.716C.open", "CX,CF"
        mov     bx, [fh]
        mov     ax, 3E00h
        int     21h

        ; ---- 714Eh find first, SI=0 (FILETIME format). AX = a search handle (host-
        ; specific, not compared); CX = Unicode-conversion flags.
        CLRBUF
        mov     ax, 714Eh
        xor     cx, cx                          ; CL allowed 0, CH required 0
        mov     dx, lpat
        xor     si, si
        mov     di, buf
        stc
        int     21h
        call    probe_capture
        mov     [sh], ax
        EMIT    "lfn.714E.first", "CX,CF"
        EMIT_BUF "lfn.714E.attr", buf, 4
        EMIT_BUF "lfn.714E.size.hi.lo", buf+1Ch, 8
        EMIT_BUF "lfn.714E.long", buf+2Ch, 22
        EMIT_BUF "lfn.714E.short", buf+130h, 14

        ; ---- 714Fh find next: nothing else matches -> 12h, the handle stays open
        CLRBUF
        mov     ax, 714Fh
        mov     bx, [sh]
        mov     cx, 0C1C1h
        xor     si, si
        mov     di, buf
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.714F.nomore", "AX,CF"

        POISON
        mov     ax, 71A1h
        mov     bx, [sh]
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.71A1.close", "AX,CF"

        POISON
        mov     ax, 71A1h
        mov     bx, [sh]                        ; closed already
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.71A1.again", "AX,CF"

        ; ---- 714Eh again, SI=1 (DOS date/time format): the HIGH dword of the write time
        CLRBUF
        mov     ax, 714Eh
        xor     cx, cx
        mov     dx, lpat
        mov     si, 1
        mov     di, buf
        stc
        int     21h
        call    probe_capture
        mov     [sh], ax
        EMIT    "lfn.714E.dosfmt", "CX,CF"
        EMIT_BUF "lfn.714E.dosfmt.wt.hi", buf+18h, 4
        mov     ax, 71A1h
        mov     bx, [sh]
        int     21h

        ; ---- 714Eh on nothing: which code (2? 12h?)
        CLRBUF
        mov     ax, 714Eh
        xor     cx, cx
        mov     dx, nopat
        xor     si, si
        mov     di, buf
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.714E.none", "AX,CF"

        ; ---- 7160h truename: CL=0 full, 1 short, 2 long -- last component only
        CLRBUF
        mov     ax, 7160h
        mov     cx, 0000h
        mov     si, lfile
        mov     di, buf
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.7160.full", "AX,CF"
        mov     si, buf
        call    lasttail
        EMIT_BUF "lfn.7160.full.tail", [tailp], 22

        CLRBUF
        mov     ax, 7160h
        mov     cx, 0001h
        mov     si, lfile
        mov     di, buf
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.7160.short", "AX,CF"
        mov     si, buf
        call    lasttail
        EMIT_BUF "lfn.7160.short.tail", [tailp], 14

        CLRBUF
        mov     ax, 7160h
        mov     cx, 0002h
        mov     si, sfile                       ; the SHORT alias, back to long
        mov     di, buf
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.7160.long", "AX,CF"
        mov     si, buf
        call    lasttail
        EMIT_BUF "lfn.7160.long.tail", [tailp], 22

        ; ---- 7143h BL=0 attributes (does AX get CX's copy, as 4300h's does?)
        mov     ax, 7143h
        mov     bx, 0000h
        mov     cx, 0C1C1h
        mov     dx, lfile
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.7143.attr", "AX,CX,CF"

        ; ---- 7143h BL=4 last write: CX time, DI date (values are "now"; CF compared)
        mov     ax, 7143h
        mov     bx, 0004h
        mov     cx, 0C1C1h
        mov     di, 0E1E1h
        mov     dx, lfile
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.7143.wtime", "AX,CF"

        ; ---- 71A7h BL=0: FILETIME 2026-10-04 12:00:00 UTC (01DD53F7E1B8E000h) -> DOS.
        ; No zone: DX=5D44 CX=6000. Converted to local, CX moves by the zone offset.
        mov     ax, 71A7h
        mov     bx, 0B100h                      ; BL=0, BH poisoned
        mov     cx, 0C1C1h
        mov     dx, 0D1D1h
        mov     si, ftval
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.71A7.ft2dos", "AX,BX,CX,DX,CF"

        ; ---- 71A7h BL=1: 2026-10-04 12:00:00 DOS -> FILETIME (the inverse)
        CLRBUF
        mov     ax, 71A7h
        mov     bx, 0001h                       ; BL=1, BH=0
        mov     cx, 6000h
        mov     dx, 5D44h
        mov     di, buf
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.71A7.dos2ft", "AX,CF"
        EMIT_BUF "lfn.71A7.dos2ft.qword", buf, 8

        ; ---- 71A8h generate a short name: DH=1 "NAME.EXT", DH=0 11-byte FCB form
        CLRBUF
        mov     ax, 71A8h
        mov     dx, 0111h                       ; DH=1, DL=11h (OEM -> OEM)
        mov     si, lfile
        mov     di, buf
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.71A8.83", "AX,CF"
        EMIT_BUF "lfn.71A8.83.name", buf, 13

        CLRBUF
        mov     ax, 71A8h
        mov     dx, 0011h                       ; DH=0
        mov     si, lfile
        mov     di, buf
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.71A8.fcb", "AX,CF"
        EMIT_BUF "lfn.71A8.fcb.name", buf, 12

        ; ---- 7156h rename long -> long
        POISON
        mov     ax, 7156h
        mov     dx, lfile
        mov     di, lfile2
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.7156.rename", "AX,CF"

        ; ---- 7141h delete it (SI=0, no wildcards); then again (gone)
        POISON
        mov     ax, 7141h
        mov     dx, lfile2
        xor     si, si
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.7141.delete", "AX,CF"

        POISON
        mov     ax, 7141h
        mov     dx, lfile2
        xor     si, si
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.7141.again", "AX,CF"

        ; ---- 713Ah rmdir; then again (gone)
        POISON
        mov     ax, 713Ah
        mov     dx, ldir
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.713A.rmdir", "AX,CF"

        POISON
        mov     ax, 713Ah
        mov     dx, ldir
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.713A.again", "AX,CF"

        ; ---- 710Dh reset drive
        mov     ax, 710Dh
        mov     cx, 0001h
        mov     dx, 0003h
        mov     bx, 0B1B1h
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.710D", "AX,CF"

        ; ---- 71AAh BH=2 query SUBST on Z: (not one, on both machines, we hope)
        CLRBUF
        mov     ax, 71AAh
        mov     bx, 021Ah                       ; BH=2 query, BL=26 = Z:
        mov     dx, buf
        stc
        int     21h
        call    probe_capture
        EMIT    "lfn.71AA.query.Z", "AX,CF"

        ; ---- unknown sub-functions: AX=7100h CF=1, the "no such LFN call" answer?
        POISON
        mov     ax, 71FFh
        clc
        int     21h
        call    probe_capture
        EMIT    "lfn.71FF", "AX,CF"

        POISON
        mov     ax, 71A2h
        clc
        int     21h
        call    probe_capture
        EMIT    "lfn.71A2", "AX,CF"

        ; ---- and the 6Ch fix that rode in with 716Ch: action 12h on a NEW file -> CX=2
        mov     ax, 6C00h
        mov     bx, 0002h
        xor     cx, cx
        mov     dx, 0012h
        mov     si, f6c
        int     21h
        call    probe_capture
        mov     [fh], ax
        EMIT    "lfn.6C.12.new", "CX,CF"
        mov     bx, [fh]
        mov     ax, 3E00h
        int     21h
        mov     ax, 6C00h                       ; and again: it now exists -> CX=3
        mov     bx, 0002h
        xor     cx, cx
        mov     dx, 0012h
        mov     si, f6c
        int     21h
        call    probe_capture
        mov     [fh], ax
        EMIT    "lfn.6C.12.exists", "CX,CF"
        mov     bx, [fh]
        mov     ax, 3E00h
        int     21h
        mov     ax, 4100h
        mov     dx, f6c
        int     21h

        PROBE_END

root    db      'C:\', 0
ldir    db      'Long Directory Name', 0
dotdot  db      '..', 0
nodir   db      'No Such Long Directory', 0
lfile   db      'A long file name.txt', 0
sfile   db      'ALONGF~1.TXT', 0
lfile2  db      'Another long name.txt', 0
lpat    db      'A long*', 0
nopat   db      'ZZ no such long file*.qqq', 0
f6c     db      'ZZ6CNEW.TMP', 0
hello   db      'hello'
ftval   dd      0E1B8E000h, 01DD53F7h   ; low dword, high dword
fh      dw      0
sh      dw      0
tailp   dw      0
buf     times 320 db 0
