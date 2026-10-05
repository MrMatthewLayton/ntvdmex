; p_drv.com -- the current DRIVE: AH=0Eh select, AH=19h read back, and whether
; AH=3Bh to another drive's path moves the current drive.  (s72, QBasic)
;
; QB.EXE's File > Open dialog sizes its drive list by the classic probe -- for each
; letter: select it (0Eh), read the current drive back (19h), compare, restore.
; On the rig only C: survived that, because our 0Eh selected a drive only when
; Win32 could chdir to it, and an empty floppy / CD-ROM drive says NOT READY.
; DOS selects from the CDS without touching the media.  The questions:
;
;   * 0Eh on a drive that EXISTS but has no media (B: is the phantom floppy on the
;     oracle; A: and D: on the rig): does 19h read it back?
;   * 0Eh on a drive beyond LASTDRIVE / with no CDS entry: does 19h stay put?
;   * 3Bh to `X:\dir` where X is NOT the current drive: does 19h stay put, and does
;     47h for X then answer `dir`?  (DOS: per-drive current directories.)
;
; Cross-drive cases use the current drive's letter and C:, so on the oracle
; (runs from A:) they are cross-drive and on the rig (runs from C:) same-drive.
; ⚠ NOTHING here TOUCHES a file on the selected drive: on the oracle that would
; be "Insert diskette for drive B:" -- a prompt nobody can answer.
;
; nasm -f bin p_drv.asm -o p_drv.com

        org     100h
        jmp     start
%include "probe.inc"

; sel -- DL = drive (0=A). 0Eh then 19h, both captured; the 19h result is the answer.
sel:
        mov     ah, 0Eh
        int     21h
        call    probe_capture
        ret
cur:
        POISON
        mov     ah, 19h
        int     21h
        call    probe_capture
        ret
; ask47 -- DL = drive (0 = current, 3 = C:). Path -> DS:SI buffer at `path`.
ask47:
        mov     byte [path], 0
        mov     byte [path + 1], 0
        mov     si, path
        mov     ah, 47h
        int     21h
        call    probe_capture
        ret

; samedrv -- AX=1 if the current drive is still [orig], else 0. THE CROSS-HOST
; CONTRACT THIS PROBE WAS MISSING: every absolute answer here (which drive is
; current, how many letters exist, whether Z: is a drive) is a fact about the
; MACHINE -- the oracle boots from A: with LASTDRIVE=5 and no Z:, the rig runs
; from C: with a CD on D: and Z: mapped to a server -- so the raw 19h value can
; never agree and 27 rows read as defects. What IS the same on both is the
; RELATION: selecting a drive that does not exist must leave you where you were.
samedrv:
        POISON
        mov     ah, 19h
        int     21h
        mov     ah, 0
        cmp     al, [orig]
        mov     ax, 0
        jne     .no
        inc     ax
.no:
        mov     [__ax], ax
        mov     word [__fl], 0
        ret

start:
        PROBE_BEGIN "drv"

        ; ---- where are we? Every restore below goes back here.
        call    cur
        EMIT    "int21.19.start", "AX"
        mov     al, [__ax]
        mov     [orig], al
        mov     dl, 0
        call    ask47
        EMIT_BUF "curdir.start", path, 10h

        ; ---- 0Eh on each interesting letter, 19h after each, restore after each.
        ; A:
        mov     dl, 0
        call    sel
        EMIT    "int21.0E.A", "AX,CF"
        call    cur
        EMIT    "int21.19.after.A", "AX"
        mov     dl, [orig]
        call    sel
        ; B: (phantom on a one-floppy machine; absent on the rig)
        mov     dl, 1
        call    sel
        EMIT    "int21.0E.B", "AX,CF"
        call    cur
        EMIT    "int21.19.after.B", "AX"
        mov     dl, [orig]
        call    sel
        ; C:
        mov     dl, 2
        call    sel
        EMIT    "int21.0E.C", "AX,CF"
        call    cur
        EMIT    "int21.19.after.C", "AX"
        mov     dl, [orig]
        call    sel
        ; D: (CD-ROM with no disc on the rig; CDS entry with no drive on the oracle)
        mov     dl, 3
        call    sel
        EMIT    "int21.0E.D", "AX,CF"
        call    cur
        EMIT    "int21.19.after.D", "AX"
        mov     dl, [orig]
        call    sel
        ; Z: (network on the rig; beyond LASTDRIVE on the oracle)
        mov     dl, 25
        call    sel
        EMIT    "int21.0E.Z", "AX,CF"
        call    cur
        EMIT    "int21.19.after.Z", "AX"
        mov     dl, [orig]
        call    sel
        ; 26 = past the last letter altogether
        mov     dl, 26
        call    sel
        EMIT    "int21.0E.26", "AX,CF"
        call    cur
        EMIT    "int21.19.after.26", "AX"
        ; ★ THE CONTRACT: 26 is past Z: on every machine, so NOBODY has that drive
        ; and both hosts must be exactly where they started. Comparable; the raw
        ; 19h value above is not.
        call    samedrv
        EMIT    "drv.26.stayed.put", "AX"
        mov     dl, [orig]
        call    sel
        EMIT    "int21.0E.restore", "AX,CF"
        call    cur
        EMIT    "int21.19.restored", "AX"
        ; ★ AND THE ROUND TRIP: whatever the starting drive was, selecting it back
        ; must restore it. Both hosts must say 1.
        call    samedrv
        EMIT    "drv.restore.round.trip", "AX"

        ; ---- 3Bh to C:\ZZDRV (made here) while the current drive may be another:
        ; does the current drive move? does 47h(C:) say ZZDRV? does 47h(0) change?
        mov     ah, 39h
        mov     dx, cdir
        int     21h
        call    probe_capture
        EMIT    "int21.39.mkdir.C", "CF"
        mov     ah, 3Bh
        mov     dx, cdir
        int     21h
        call    probe_capture
        EMIT    "int21.3B.chdir.C", "AX,CF"
        call    cur
        EMIT    "int21.19.after.3B.C", "AX"
        mov     dl, 3
        call    ask47
        EMIT    "int21.47.C", "AX,CF"
        EMIT_BUF "curdir.C", path, 10h
        mov     dl, 0
        call    ask47
        EMIT_BUF "curdir.0.after.3B.C", path, 10h

        ; ---- 3Bh to "C:" alone (drive, no path) -- a chdir to C:'s current dir?
        mov     ah, 3Bh
        mov     dx, conly
        int     21h
        call    probe_capture
        EMIT    "int21.3B.C-only", "AX,CF"
        call    cur
        EMIT    "int21.19.after.3B.C-only", "AX"

        ; ---- 3Bh to the root of a drive beyond LASTDRIVE (oracle) / a network
        ; drive (rig). ⚠ NOT B:\ -- on the oracle that is the phantom floppy and
        ; DOS would ask for a diskette nobody can insert.
        mov     ah, 3Bh
        mov     dx, zroot
        int     21h
        call    probe_capture
        EMIT    "int21.3B.Z-root", "AX,CF"
        call    cur
        EMIT    "int21.19.after.3B.Z-root", "AX"

        ; ---- 47h on a drive with no entry. ⚠ Not B: either -- 47h on the phantom
        ; drive re-validates its directory from the media and prompts (measured).
        mov     dl, 26
        call    ask47
        EMIT    "int21.47.Z", "AX,CF"

        ; ---- clean up: C:'s directory back to the root, remove ZZDRV, the
        ; current drive back where it was and its directory too.
        mov     ah, 3Bh
        mov     dx, croot
        int     21h
        mov     ah, 3Ah
        mov     dx, cdir
        int     21h
        call    probe_capture
        EMIT    "int21.3A.rmdir.C", "CF"
        mov     dl, [orig]
        call    sel
        call    cur
        EMIT    "int21.19.end", "AX"
        mov     dl, 0
        call    ask47
        EMIT_BUF "curdir.end", path, 10h

        PROBE_END

orig     db 0
cdir     db 'C:\ZZDRV', 0
croot    db 'C:\', 0
conly    db 'C:', 0
zroot    db 'Z:\', 0
path     times 80 db 0
