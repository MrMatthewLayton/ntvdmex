; p_find.com -- INT 21h 4Eh / 4Fh, find first / find next, and the DTA block.
;
; The load-bearing pair for DIR, and the last big gap before COMMAND.COM is
; useful.  Feeds GH #29.
;
; WHAT IS AND IS NOT COMPARABLE ACROSS HOSTS: the files present differ on every
; host (the oracle's A:, the rig's C:\test, DOSBox's mount), so filenames, sizes
; and timestamps are NOT declared significant -- only the error codes, the carry
; flag, and AX on success, which are properties of DOS rather than of the disk.
; The DTA is dumped raw so the block LAYOUT can be read off the oracle.
;
; nasm -f bin p_find.asm -o p_find.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "find"

        ; ---- point the DTA at our own poisoned buffer so we can see exactly
        ; which bytes DOS writes and which it leaves alone.
        mov     di, dta
        mov     cx, 64
        mov     al, 0EEh
        push    es
        mov     bx, cs
        mov     es, bx
        cld
        rep     stosb
        pop     es

        mov     ah, 1Ah
        mov     dx, dta
        int     21h

        ; ---- 4Eh: find first "*.*", attribute mask 0 (normal files only)
        POISON
        mov     ax, 4E00h
        mov     cx, 0
        mov     dx, allfiles
        int     21h
        call    probe_capture
        EMIT    "int21.4E.first", "AX,CF"
        EMIT_BUF "dta.after.4E", dta, 44

        ; ---- 4Fh: find next
        POISON
        mov     ax, 4F00h
        int     21h
        call    probe_capture
        EMIT    "int21.4F.next", "CF"

        ; ---- 4Eh on a name that cannot exist -> "file not found"
        POISON
        mov     ax, 4E00h
        mov     cx, 0
        mov     dx, nofile
        int     21h
        call    probe_capture
        EMIT    "int21.4E.nofile", "AX,CF"

        ; ---- 4Eh into a directory that does not exist -> "path not found"
        POISON
        mov     ax, 4E00h
        mov     cx, 0
        mov     dx, nopath
        int     21h
        call    probe_capture
        EMIT    "int21.4E.nopath", "AX,CF"

        ; ---- 4Fh straight after a FAILED 4Eh.  Not "exhausted" -- the question
        ; is whether a failed find-first CLOBBERS the search state already in the
        ; DTA.  On real 6.22 it does not: this 4Fh carries on the live "*.*"
        ; search above and succeeds.  (Named wrongly at first, which made the
        ; result look like a bug in us rather than the behaviour it measures.)
        POISON
        mov     ax, 4F00h
        int     21h
        call    probe_capture
        EMIT    "int21.4F.after.failed.4E", "AX,CF"

        PROBE_END

allfiles db '*.*', 0
nofile   db 'ZZNOSUCH.XYZ', 0
nopath   db '\ZZNODIR\*.*', 0
dta:
        times 64 db 0
