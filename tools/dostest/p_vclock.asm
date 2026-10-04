; p_vclock.com -- ONE clock, every door onto it.  GH #261, #262, #263.
; ORACLE-ALSO: pcem
;
; p_clock, p_tick2c, p_fdate and p_rtcw each ask one door. This probe walks a single
; timeline through all of them, so a model that keeps the doors in step only pairwise
; shows up: DOS's date/time (INT 21h 2Ah-2Dh), the BIOS tick count (INT 1Ah 00h/01h and
; a raw store to 0040:006C), a file's stamp (AH=57h) and the MC146818 (ports 70h/71h,
; INT 1Ah 02h/04h).
;
; WHAT AN AT WITH MS-DOS 6.22 DOES -- the expectations, and where each comes from:
;   A  2Bh 1999-12-31, 2Dh 12:00:30.00. 2Ah: CX=07CFh DX=0C1Fh AL=5 (Friday);
;      2Ch: CX=0C00h; INT 1Ah 00h: CX=000Ch (787068 ticks = 000C:027Ch); the CHIP's
;      hours register: 12h -- CLOCK$ writes the RTC on a set (p_clock clk.1a02.after.2d).
;   B  INT 1Ah 01h, the count for 10:00:30 (000A:026Ch). 2Ch: CX=0A00h (p_tick2c A);
;      2Ah: DX=0C1Fh, unchanged; the chip's hours: still 12h -- 1Ah 01h is the tick
;      count only, the RTC is a separate clock.
;   C  a STORE to 0040:006C, the count for 11:30:30 (000B:8277h). 2Ch: CX=0B1Eh
;      (p_tick2c B: 6.22, PCem and DOSBox-X all follow); 2Ah still 1999-12-31.
;   D  a STORE of 0x1800A7, nine ticks short of the BIOS's day, then wait (reading
;      006C directly -- INT 1Ah 00h would CONSUME the midnight flag DOS needs) until the
;      count wraps. 2Ah: CX=07D0h DX=0101h AL=6 -- DOS's day number moved on because
;      CLOCK$ saw the rollover; 2Ch: CX=0000h. AX of the wait case = 1 if the wrap was
;      seen at all (0 = the probe gave up: ticks did not advance on this host).
;      ⚠ D IS UNMEASURED ON EVERY HOST at the time of writing -- the case this probe
;        exists to ask. RBIL: DOS increments its date when INT 1Ah AH=00h returns
;        AL<>0; whether 6.22's CLOCK$ is reached by 2Ah as well as 2Ch is the question.
;   E  2Dh 12:34:10, create VCLOCK.TMP, write 9 bytes, AX=5700h on the open handle:
;      DX = 2000-01-01 = (20<<9)|(1<<5)|1 = 2821h; CX AND FFE0h = 12:34 = (12<<11)|(34<<5) = 6440h
;      (the 2-second field is masked: it moves with the clock). (#263, p_fdate)
;   F  the CHIP, with SET held: 08:09 on 1998-03-04, century 19. Chip hours:minutes
;      AX=0809h; INT 1Ah 02h CX=0809h; INT 1Ah 04h CX=1998h DX=0304h. Then DOS:
;      2Ah CX=07D0h DX=0101h, 2Ch CH=0Ch -- DOS does NOT follow a chip write (p_rtcw D).
;
; ⚠ HOURS AND MINUTES ONLY wherever a clock is running: every target is at least
;   10 s from the next minute, so the time the probe itself takes cannot carry one.
; ⚠ IT PUTS THE CLOCK BACK: date and time are read at the start and set again with
;   2Bh/2Dh at the end (a few seconds are lost); on an AT that also rewrites the RTC.
;   Status B is saved and restored. VCLOCK.TMP is deleted.
; ⛔ ONLY 00h-09h, 0Bh AND 32h ARE WRITTEN on the chip -- nothing in the checksummed
;   10h-2Dh range. Bit 7 of port 70h (NMI mask) is kept clear.
;
; nasm -f bin p_vclock.asm -o p_vclock.com

        org     100h
        jmp     start
%include "probe.inc"

fname   db      "VCLOCK.TMP", 0
sv_y    dw      0
sv_md   dw      0
sv_hm   dw      0
sv_sc   dw      0
sv_b    db      0
hnd     dw      0
rd_a    db      0
rd_b    db      0
wrapped dw      0

; ---------------------------------------------------------------- CMOS helpers
cmos_rd:                                ; AL = register -> AL
        and     al, 07Fh
        out     070h, al
        jmp     short $+2
        in      al, 071h
        ret

cmos_wr:                                ; AH -> register AL
        and     al, 07Fh
        out     070h, al
        jmp     short $+2
        mov     al, ah
        out     071h, al
        ret

uip_wait:                               ; bounded wait for Status A UIP to clear
        push    ax
        push    cx
        mov     cx, 8000h
.l:     mov     al, 00Ah
        call    cmos_rd
        test    al, 080h
        jz      .ok
        loop    .l
.ok:    pop     cx
        pop     ax
        ret

; DOS's date and time into the capture: CX = year, DX = month:day, AL = weekday
; (2Ah), BX = 2Ch's CX (hours:minutes). SI/DI poisoned.
dos_now:
        mov     ah, 2Ch
        int     21h
        push    cx
        mov     ah, 2Ah
        int     21h
        pop     bx
        xor     ah, ah
        mov     si, 05151h
        mov     di, 0D1D1h
        ret

; the chip's hours register (BCD) -> AL
chip_hours:
        call    uip_wait
        mov     al, 04h
        call    cmos_rd
        ret

start:
        PROBE_BEGIN "vclock"

        ; ---- save DOS's clock and Status B
        mov     ah, 2Ah
        int     21h
        mov     [sv_y], cx
        mov     [sv_md], dx
        mov     ah, 2Ch
        int     21h
        mov     [sv_hm], cx
        mov     [sv_sc], dx
        mov     al, 0Bh
        call    cmos_rd
        mov     [sv_b], al

; ════════════════════════════════════════════════════════════════════════════
; A. 2Bh 1999-12-31, 2Dh 12:00:30.00
; ════════════════════════════════════════════════════════════════════════════
        mov     cx, 1999
        mov     dx, 0C1Fh
        mov     ah, 2Bh
        int     21h
        mov     cx, 0C00h
        mov     dx, 1E00h
        mov     ah, 2Dh
        int     21h
        call    dos_now
        call    probe_capture
        EMIT    "vclock.a.dos", "AX,BX,CX,DX"
        POISON
        mov     ah, 00h
        int     1Ah
        mov     dx, 0D1D1h              ; the low word moves with the ticks
        mov     ax, 0A1A1h              ; AL = midnight flag: not this case's question
        call    probe_capture
        EMIT    "vclock.a.ticks.high", "CX"
        call    chip_hours
        xor     ah, ah
        POISON
        call    probe_capture
        EMIT    "vclock.a.chip.hours", "AX"

; ════════════════════════════════════════════════════════════════════════════
; B. INT 1Ah AH=01h -- 10:00:30 = 000A:026Ch
; ════════════════════════════════════════════════════════════════════════════
        mov     cx, 000Ah
        mov     dx, 026Ch
        mov     ah, 01h
        int     1Ah
        call    dos_now
        call    probe_capture
        EMIT    "vclock.b.dos", "AX,BX,CX,DX"
        call    chip_hours
        xor     ah, ah
        POISON
        call    probe_capture
        EMIT    "vclock.b.chip.hours", "AX"

; ════════════════════════════════════════════════════════════════════════════
; C. a store to 0040:006C -- 11:30:30 = 000B:8277h
; ════════════════════════════════════════════════════════════════════════════
        push    es
        mov     ax, 0040h
        mov     es, ax
        cli
        mov     word [es:006Ch], 8277h
        mov     word [es:006Eh], 000Bh
        sti
        pop     es
        call    dos_now
        call    probe_capture
        EMIT    "vclock.c.dos", "AX,BX,CX,DX"

; ════════════════════════════════════════════════════════════════════════════
; D. a store nine ticks short of midnight; wait for the wrap WITHOUT INT 1Ah
; ════════════════════════════════════════════════════════════════════════════
        push    es
        mov     ax, 0040h
        mov     es, ax
        cli
        mov     word [es:006Ch], 00A7h
        mov     word [es:006Eh], 0018h
        sti
        ; Bounded twice over: at most 64 tick changes (~3.5 s), and at most
        ; 0x400 x 0x10000 polls in case ticks never move at all.
        xor     bx, bx                  ; tick changes seen
        mov     si, 0400h               ; outer poll budget
        mov     dx, [es:006Ch]
.d_outer:
        xor     cx, cx
.d_poll:
        cmp     word [es:006Eh], 0018h
        jne     .d_wrapped              ; high word left 0018h: the count wrapped
        mov     ax, [es:006Ch]
        cmp     ax, dx
        je      .d_same
        mov     dx, ax
        inc     bx
        cmp     bx, 64
        jae     .d_gaveup
.d_same:
        loop    .d_poll
        dec     si
        jnz     .d_outer
        jmp     .d_gaveup
.d_wrapped:
        mov     word [wrapped], 1
.d_gaveup:
        pop     es
        mov     ax, [wrapped]
        POISON
        call    probe_capture
        EMIT    "vclock.d.wrap.seen", "AX"
        call    dos_now
        call    probe_capture
        EMIT    "vclock.d.dos", "AX,BX,CX,DX"

; ════════════════════════════════════════════════════════════════════════════
; E. 2Dh 12:34:10; a new file's stamp on the open handle
; ════════════════════════════════════════════════════════════════════════════
        mov     cx, 0C22h
        mov     dx, 0A00h
        mov     ah, 2Dh
        int     21h
        mov     ah, 3Ch
        xor     cx, cx
        mov     dx, fname
        int     21h
        jc      .f
        mov     [hnd], ax
        mov     bx, ax
        mov     ah, 40h
        mov     cx, 9
        mov     dx, fname
        int     21h
        mov     bx, [hnd]
        mov     cx, 0C1C1h
        mov     dx, 0D1D1h
        mov     ax, 5700h
        int     21h
        pushf
        and     cx, 0FFE0h              ; hours:minutes; the 2-second field moves
        popf
        call    probe_capture
        EMIT    "vclock.e.57.open", "CX,DX"
        mov     bx, [hnd]
        mov     ah, 3Eh
        int     21h
        mov     ah, 41h
        mov     dx, fname
        int     21h

; ════════════════════════════════════════════════════════════════════════════
; F. the chip, SET held: 08:09 on 1998-03-04 (century 19); then the BIOS, then DOS
; ════════════════════════════════════════════════════════════════════════════
.f:
        call    uip_wait
        mov     al, 0Bh
        call    cmos_rd
        or      al, 080h                ; SET
        mov     ah, al
        mov     al, 0Bh
        call    cmos_wr
        mov     ax, 0804h
        call    cmos_wr
        mov     ax, 0902h
        call    cmos_wr
        mov     ax, 0000h
        call    cmos_wr
        mov     ax, 0407h
        call    cmos_wr
        mov     ax, 0308h
        call    cmos_wr
        mov     ax, 9809h
        call    cmos_wr
        mov     ax, 1932h
        call    cmos_wr
        mov     al, 0Bh
        call    cmos_rd
        and     al, 07Fh                ; release SET: the chip runs from 08:09:00
        mov     ah, al
        mov     al, 0Bh
        call    cmos_wr
        call    uip_wait
        mov     al, 04h
        call    cmos_rd
        mov     [rd_a], al
        mov     al, 02h
        call    cmos_rd
        mov     [rd_b], al
        POISON
        mov     ah, 02h
        int     1Ah
        mov     bx, 0B1B1h
        mov     dx, 0D1D1h
        mov     ah, [rd_a]
        mov     al, [rd_b]
        call    probe_capture
        EMIT    "vclock.f.chip+1a02", "AX,CX"
        POISON
        mov     ah, 04h
        int     1Ah
        call    probe_capture
        EMIT    "vclock.f.1a04", "CX,DX"
        call    dos_now
        call    probe_capture
        EMIT    "vclock.f.dos", "AX,BX,CX,DX"

        ; ---- put DOS's clock back (CLOCK$ rewrites the RTC on an AT), then Status B
        mov     cx, [sv_y]
        mov     dx, [sv_md]
        mov     ah, 2Bh
        int     21h
        mov     cx, [sv_hm]
        mov     dx, [sv_sc]
        mov     ah, 2Dh
        int     21h
        mov     ah, [sv_b]
        and     ah, 07Fh
        mov     al, 0Bh
        call    cmos_wr

        PROBE_END
