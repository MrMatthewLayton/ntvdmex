; p_err.com -- INT 21h AH=59h, get extended error, for each error we can provoke.
;
; 59h reports a failure as four things, not one: the extended code (AX), an error
; CLASS (BH), a suggested ACTION (BL) and a LOCUS (CH).  Only the code is
; obvious; class/action/locus for a given error are exactly the sort of value
; that gets written from memory and is wrong.  So each case provokes a specific,
; harmless failure and then asks DOS what it made of it.
;
; CL is deliberately included in one case: it came back still poisoned on the
; first run, i.e. DOS does not write it, and that is worth pinning down rather
; than assuming.
;
; Feeds GH #34.
;
; nasm -f bin p_err.asm -o p_err.com

        org     100h
        jmp     start
%include "probe.inc"

; PROVOKE runs a call expected to fail, then reads 59h and reports it.
%macro ASKERR 1
        POISON
        mov     ax, 5900h
        mov     bx, 0
        int     21h
        call    probe_capture
        EMIT    %1, "AX,BX,CX"
%endmacro

start:
        PROBE_BEGIN "err"

        ; ---- find-first on a pattern that matches nothing -> 18
        mov     ax, 4E00h
        mov     cx, 0
        mov     dx, nofile
        int     21h
        ASKERR  "err.after.4E.nofile"

        ; ---- find-first into a missing directory -> 3
        mov     ax, 4E00h
        mov     cx, 0
        mov     dx, nopath
        int     21h
        ASKERR  "err.after.4E.nopath"

        ; ---- open a file that does not exist -> 2
        mov     ax, 3D00h
        mov     dx, nofile
        int     21h
        ASKERR  "err.after.3D.missing"

        ; ---- chdir to a directory that does not exist
        mov     ax, 3B00h
        mov     dx, nopath2
        int     21h
        ASKERR  "err.after.3B.missing"

        ; ---- read from a handle that was never opened -> 6
        mov     ax, 3F00h
        mov     bx, 20                          ; never opened
        mov     cx, 1
        mov     dx, scratch
        int     21h
        ASKERR  "err.after.3F.badhandle"

        ; ---- close a handle that was never opened
        mov     ax, 3E00h
        mov     bx, 20
        int     21h
        ASKERR  "err.after.3E.badhandle"

        PROBE_END

nofile   db 'ZZNOSUCH.XYZ', 0
nopath   db '\ZZNODIR\*.*', 0
nopath2  db '\ZZNODIR', 0
scratch  db 0, 0, 0, 0
