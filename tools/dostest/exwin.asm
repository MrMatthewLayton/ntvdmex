; exwin.com -- RIG-ONLY smoke test: EXEC of a Win32 console program.  GH #255.
;
; Not a p_ probe and deliberately not in the parity sweep: MS-DOS has no Win32, so
; no oracle can answer it, and a one-host run is a smoke test, not a pass. What it
; shows is the mechanism on NTVDMEX: EXEC of a PE is handed to Windows (CMD.EXE runs
; in its own console, EXEC WAITS for it), the DOS command tail reaches it, and its
; exit code comes back through AH=4Dh -- `cmd /c exit 7` must give AX=0007.
;
;   python3 scripts/dosdiff.py tools/dostest/exwin.com --host ntvdmex
;
; nasm -f bin exwin.asm -o exwin.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "exwin"
        mov     ah, 4Ah
        mov     bx, 1000h
        push    cs
        pop     es
        int     21h
        mov     ax, cs
        mov     [pb_tail + 2], ax
        mov     [pb_fcb1 + 2], ax
        mov     [pb_fcb2 + 2], ax

        POISON
        mov     bx, pblock
        mov     dx, prog
        mov     ax, 4B00h
        int     21h
        call    probe_capture
        EMIT    "exwin.exec.cmd", "AX,CF"

        POISON
        mov     ax, 4D00h
        int     21h
        call    probe_capture
        EMIT    "exwin.4d", "AX"                ; 0007

        PROBE_END

prog    db      'C:\WINDOWS\SYSTEM32\CMD.EXE', 0
tail    db      10, ' /c exit 7', 0Dh
fcb     times 16 db 0
pblock  dw      0
pb_tail dw      tail, 0
pb_fcb1 dw      fcb, 0
pb_fcb2 dw      fcb, 0
