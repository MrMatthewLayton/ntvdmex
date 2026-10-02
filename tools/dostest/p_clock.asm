; p_clock.com -- INT 21h AH=2Bh/2Dh (set date / set time) and the clock they set.
; ORACLE-ALSO: pcem
;
; GH #250. Both calls used to answer AL=00 ("done") and change nothing, so a program
; that set the date and read it back got today. This probe sets, then READS BACK --
; the readback is the contract; AL=00 alone proves nothing (an all-agree row that
; compares only "it said yes" is how the bug survived the first parity sweep).
;
; What each case asks:
;   * the validity rule  -- which dates/times DOS refuses with AL=FFh (leap years,
;     the 1980..2099 year range, month/day/hour/min/sec/hundredths ranges), and that
;     a refused call LEAVES THE CLOCK ALONE;
;   * the readback       -- 2Ah/2Ch return what 2Bh/2Dh set, with the day of week
;     DOS derives (AL of 2Ah) for that date;
;   * the BIOS view      -- after a DOS set, what INT 1Ah AH=00h (tick count) and
;     AH=02h/04h (the RTC) say. On an AT, DOS's CLOCK$ writes both;
;   * midnight           -- the date rolls over by itself from 23:59:59.
;     ⚠ The wait polls 0040:006C DIRECTLY, never INT 1Ah AH=00h: that call returns
;       AND CLEARS the midnight flag, and a DOS that never sees the flag never turns
;       the date over. Polling it would make the probe cause the very failure it
;       reports;
;   * INT 1Ah AH=03h/05h -- the RTC set directly, and whether DOS's own clock
;     follows it (on real DOS it does not: DOS keeps its own time from the ticks).
;
; ⚠ NOTHING HERE COMPARES THE ACTUAL TIME OF DAY. Every readback is of a value this
;   probe SET a moment earlier, with seconds/hundredths left out wherever a tick
;   could have elapsed.
; ⚠ IT PUTS THE CLOCK BACK. PCem keeps its RTC in a CMOS file between runs, so the
;   probe reads the date and time first and restores them at the end (a few seconds
;   are lost; nothing compares the absolute time).
;
; nasm -f bin p_clock.asm -o p_clock.com

        org     100h
        jmp     start
%include "probe.inc"

; SETDATE y, m, d  -- AH=2Bh with AL poisoned so an untouched AL reads EE
%macro SETDATE 3
        mov     bx, 0B1B1h
        mov     cx, %1
        mov     dh, %2
        mov     dl, %3
        mov     ax, 2BEEh
        int     21h
%endmacro

; SETTIME h, m, s, cs
%macro SETTIME 4
        mov     bx, 0B1B1h
        mov     ch, %1
        mov     cl, %2
        mov     dh, %3
        mov     dl, %4
        mov     ax, 2DEEh
        int     21h
%endmacro

; GETDATE case -- 2Ah, every output poisoned; AX carries AL = day of week
%macro GETDATE 1
        POISON
        mov     ax, 2AEEh
        int     21h
        call    probe_capture
        EMIT    %1, "AX,CX,DX"
%endmacro

; GETTIME_HM case -- 2Ch, compare CH:CL only (seconds may have moved)
%macro GETTIME_HM 1
        POISON
        mov     ax, 2CEEh
        int     21h
        call    probe_capture
        EMIT    %1, "CX"
%endmacro

start:
        PROBE_BEGIN "clock"

        ; ---- save the clock so it can be put back at the end
        mov     ah, 2Ah
        int     21h
        mov     [sv_year], cx
        mov     [sv_md], dx
        mov     ah, 2Ch
        int     21h
        mov     [sv_hm], cx
        mov     [sv_sc], dx

        ; ---- midday first, so no date case can be disturbed by a real midnight
        SETTIME 12, 0, 0, 0
        call    probe_capture
        EMIT    "clk.2d.noon", "AX"

        ; ================================================================ 2Bh
        SETDATE 1999, 12, 31
        call    probe_capture
        EMIT    "clk.2b.19991231", "AX"
        GETDATE "clk.2a.after.19991231"          ; CX=07CF DX=0C1F AL=5 (Friday)

        SETDATE 2001, 2, 30
        call    probe_capture
        EMIT    "clk.2b.feb30", "AX"
        GETDATE "clk.2a.after.feb30"             ; refused => still 1999-12-31

        SETDATE 2000, 2, 29                      ; 2000 IS a leap year (div by 400)
        call    probe_capture
        EMIT    "clk.2b.leap2000", "AX"
        GETDATE "clk.2a.after.leap2000"

        SETDATE 2001, 2, 29
        call    probe_capture
        EMIT    "clk.2b.feb29.2001", "AX"

        SETDATE 1979, 12, 31
        call    probe_capture
        EMIT    "clk.2b.y1979", "AX"

        SETDATE 1980, 1, 1
        call    probe_capture
        EMIT    "clk.2b.y1980", "AX"
        GETDATE "clk.2a.after.y1980"             ; Tuesday

        SETDATE 2099, 12, 31
        call    probe_capture
        EMIT    "clk.2b.y2099", "AX"
        GETDATE "clk.2a.after.y2099"

        SETDATE 2100, 1, 1
        call    probe_capture
        EMIT    "clk.2b.y2100", "AX"

        SETDATE 2004, 13, 1
        call    probe_capture
        EMIT    "clk.2b.month13", "AX"

        SETDATE 2004, 0, 1
        call    probe_capture
        EMIT    "clk.2b.month0", "AX"

        SETDATE 2004, 4, 0
        call    probe_capture
        EMIT    "clk.2b.day0", "AX"

        SETDATE 2004, 4, 31                      ; April has 30
        call    probe_capture
        EMIT    "clk.2b.apr31", "AX"

        SETDATE 2004, 2, 29                      ; ordinary leap year
        call    probe_capture
        EMIT    "clk.2b.leap2004", "AX"
        GETDATE "clk.2a.after.leap2004"          ; Sunday

        ; ---- the RTC after a DOS date set (AT CLOCK$ writes it)
        POISON
        mov     ah, 04h
        int     1Ah
        call    probe_capture
        EMIT    "clk.1a04.after.2b", "CX,DX"     ; 2004 / 02 29 in BCD

        ; ================================================================ 2Dh
        SETTIME 12, 34, 10, 0
        call    probe_capture
        EMIT    "clk.2d.123410", "AX"
        GETTIME_HM "clk.2c.after.123410"         ; CX=0C22

        SETTIME 24, 0, 0, 0
        call    probe_capture
        EMIT    "clk.2d.h24", "AX"
        SETTIME 12, 60, 0, 0
        call    probe_capture
        EMIT    "clk.2d.m60", "AX"
        SETTIME 12, 0, 60, 0
        call    probe_capture
        EMIT    "clk.2d.s60", "AX"
        SETTIME 12, 0, 0, 100
        call    probe_capture
        EMIT    "clk.2d.cs100", "AX"
        GETTIME_HM "clk.2c.after.refusals"       ; refused => still 12:34

        SETTIME 23, 59, 59, 99                   ; the last valid instant
        call    probe_capture
        EMIT    "clk.2d.235959", "AX"
        ; ⚠ NO READBACK OF 23:59:59.99: it is one hundredth from midnight, and PCem
        ;   read 00:00 back while QEMU read 23:59 -- a race, not a disagreement.
        SETTIME 23, 59, 58, 0
        GETTIME_HM "clk.2c.after.235958"
        SETTIME 0, 0, 0, 0
        call    probe_capture
        EMIT    "clk.2d.000000", "AX"
        GETTIME_HM "clk.2c.after.000000"

        ; ---- the BIOS view after a DOS time set
        SETTIME 12, 34, 10, 0
        POISON
        mov     ah, 02h
        int     1Ah
        call    probe_capture
        EMIT    "clk.1a02.after.2d", "CX"        ; 1234 BCD
        POISON
        xor     ax, ax
        int     1Ah
        call    probe_capture
        mov     word [__dx], 0                   ; the low word moves; the high is 000C
        mov     ax, [__ax]
        and     ax, 0FFh                         ; AL = midnight flag; AH undefined
        mov     [__ax], ax
        EMIT    "clk.1a00.after.2d", "AX,CX"

        ; ================================================================ midnight
        SETDATE 1999, 12, 31
        SETTIME 23, 59, 59, 0
        ; wait for the date to turn over, bounded by ~10 s of BIOS ticks read straight
        ; out of the BDA (see the header: never INT 1Ah AH=00h here)
        mov     word [tchg], 0
        push    es
        mov     ax, 40h
        mov     es, ax
        mov     ax, [es:6Ch]
        mov     [tlast], ax
.mw:    mov     ah, 2Ah
        int     21h
        cmp     dx, 0C1Fh
        jne     .mdone
        mov     ax, [es:6Ch]
        cmp     ax, [tlast]
        je      .mw
        mov     [tlast], ax
        inc     word [tchg]
        cmp     word [tchg], 182
        jb      .mw
.mdone: pop     es
        GETDATE "clk.midnight.2a"                ; 2000-01-01, Saturday (AL=6)
        GETTIME_HM "clk.midnight.2c"             ; 00:00

        ; ================================================================ INT 1Ah 03h/05h
        ; the RTC set directly; does DOS's clock follow it? (real DOS: no)
        mov     bx, 0B1B1h
        mov     cx, 0815h                        ; 08:15 BCD
        mov     dx, 3000h                        ; :30, DST flag 0
        mov     ah, 03h
        int     1Ah
        call    probe_capture
        EMIT    "clk.1a03.set", "CF"
        POISON
        mov     ah, 02h
        int     1Ah
        call    probe_capture
        EMIT    "clk.1a02.after.1a03", "CX"      ; 0815
        GETTIME_HM "clk.2c.after.1a03"           ; still 00:00 on real DOS

        mov     bx, 0B1B1h
        mov     cx, 2010h
        mov     dx, 0615h
        mov     ah, 05h
        int     1Ah
        call    probe_capture
        EMIT    "clk.1a05.set", "CF"
        POISON
        mov     ah, 04h
        int     1Ah
        call    probe_capture
        EMIT    "clk.1a04.after.1a05", "CX,DX"   ; 2010 0615
        GETDATE "clk.2a.after.1a05"              ; still 2000-01-01 on real DOS

        ; ================================================================ put it back
        mov     cx, [sv_year]
        mov     dx, [sv_md]
        mov     ah, 2Bh
        int     21h
        mov     cx, [sv_hm]
        mov     dx, [sv_sc]
        mov     ah, 2Dh
        int     21h
        mov     ah, 0
        mov     [__ax], ax
        EMIT    "clk.restore", "AX"

        PROBE_END

sv_year dw 0
sv_md   dw 0
sv_hm   dw 0
sv_sc   dw 0
tlast   dw 0
tchg    dw 0
