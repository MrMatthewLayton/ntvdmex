; p_fdate.com -- the date a file is stamped with after a program sets the date.  GH #263.
; ORACLE-ALSO: pcem
;
; INT 21h AH=2Bh moves the VDM's own clock (#250), never the host's. File I/O is
; Win32's, and Win32 stamps a write with the HOST's time -- so a program that sets
; 1999-06-15 and writes a file got today's date on it, where DOS would give the
; date it was told. This probe sets the date, writes a file, and reads the stamp
; back three ways.
;
;   A  AH=57h AL=00h on the still-open handle, after the write: DX = the date
;   B  the same after close and reopen
;   C  FindFirst (AH=4Eh) on the name: the DTA's date word
;   Expected on DOS: 1999-06-15 = ((1999-1980) << 9) | (6 << 5) | 15 = 26CFh.
;
; ⚠ DATE ONLY. The time word moves with the clock and is not compared.
; ⚠ IT PUTS THE DATE BACK and DELETES THE FILE (FDATE.TMP, in the current
;   directory). If a run dies in between, the file is all that is left.
;
; nasm -f bin p_fdate.asm -o p_fdate.com

        org     100h
        jmp     start
%include "probe.inc"

fname   db      "FDATE.TMP", 0
sv_y    dw      0
sv_md   dw      0
hnd     dw      0
dta     times 43 db 0

start:
        PROBE_BEGIN "fdate"

        mov     ah, 2Ah
        int     21h
        mov     [sv_y], cx
        mov     [sv_md], dx

        mov     ah, 1Ah                 ; our own DTA, so 4Eh's answer is ours
        mov     dx, dta
        int     21h

        mov     cx, 1999
        mov     dh, 6
        mov     dl, 15
        mov     ah, 2Bh
        int     21h

        mov     ah, 3Ch                 ; create
        xor     cx, cx
        mov     dx, fname
        int     21h
        jc      .done
        mov     [hnd], ax
        mov     bx, ax                  ; write a few bytes
        mov     ah, 40h
        mov     cx, 9
        mov     dx, fname
        int     21h

; A. on the open handle
        mov     bx, [hnd]
        mov     cx, 0C1C1h
        mov     dx, 0D1D1h
        mov     ax, 5700h
        int     21h
        mov     cx, 0C1C1h              ; the time is not compared
        call    probe_capture
        EMIT    "fdate.57.open", "DX"

        mov     bx, [hnd]
        mov     ah, 3Eh
        int     21h

; B. after close and reopen
        mov     ax, 3D00h
        mov     dx, fname
        int     21h
        jc      .c
        mov     [hnd], ax
        mov     bx, ax
        mov     cx, 0C1C1h
        mov     dx, 0D1D1h
        mov     ax, 5700h
        int     21h
        mov     cx, 0C1C1h
        call    probe_capture
        EMIT    "fdate.57.reopened", "DX"
        mov     bx, [hnd]
        mov     ah, 3Eh
        int     21h

; C. FindFirst: the date word is at DTA+18h
.c:
        mov     ah, 4Eh
        xor     cx, cx
        mov     dx, fname
        int     21h
        mov     dx, [dta + 18h]
        mov     ax, 0
        adc     ax, 0                   ; CF of the find, as AX
        call    probe_capture
        EMIT    "fdate.4e", "AX,DX"

        mov     ah, 41h
        mov     dx, fname
        int     21h
.done:
        mov     cx, [sv_y]
        mov     dx, [sv_md]
        mov     ah, 2Bh
        int     21h

        PROBE_END
