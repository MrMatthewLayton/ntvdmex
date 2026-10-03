; p_rtcw.com -- WRITING the MC146818's clock through ports 70h/71h.  GH #261.
; ORACLE-ALSO: pcem
;
; p_rtc only READS the chip. Setting the time and date through the registers is the
; other half, and the half we refused: a write to 00h-09h was dropped "because we
; cannot move the host's clock". Since #250 the VDM has its own RTC (an offset from
; the host's), so the write can be honoured -- this probe says what honouring means.
;
; THE PROTOCOL A PROGRAM USES, and what each case asks of it (MC146818 datasheet,
; register B; docs/ref/rtc.md):
;   SET  Status B bit 7. While it is 1 the chip does not update, so a program can
;        write hours, minutes and seconds one at a time without the clock carrying
;        between the writes. Clearing it starts the clock FROM the written values.
;   A  write 12:34:56 with SET held, read the three back while SET is still held
;      -- the registers must say what was written, and must not have moved.
;   B  Status B while SET is held: bit 7 reads back set. UIE (bit 4) is masked off
;      the comparison -- the datasheet says SET going high clears it, which is
;      checked separately in C only on a machine where it started set.
;   C  clear SET, read hours and minutes from the CHIP: 12:34 (seconds may have
;      moved on). Then INT 1Ah AH=02h: the BIOS reads the same chip, so 12:34 too.
;   D  INT 21h AH=2Ch afterwards: DOS keeps its own clock from the BIOS tick count,
;      so on 6.22 it does NOT follow a chip write (p_clock measured the same for
;      INT 1Ah AH=03h). AX = 1 if DOS's hours equal 12.
;   E  the DATE, same protocol: 1999-06-15 with century 19, read back through the
;      chip and through INT 1Ah AH=04h (CX = 1999 BCD, DX = 0615 BCD).
;   F  BINARY MODE (Status B DM, bit 2 set): write 23:45 as plain binary, read it
;      back in binary, then return to BCD and read it as BCD (0x23:0x45).
;
; ⚠ IT PUTS THE CLOCK BACK. The time and date are read first (BCD, chip) and written
;   back at the end with the same protocol; a few seconds are lost. PCem keeps its
;   CMOS in a file between runs.
; ⛔ ONLY 00h-09h AND 0Bh ARE WRITTEN, and 32h (century). Nothing in 10h-2Dh, the range
;   the checksum at 2Eh/2Fh covers: a bad checksum makes a real BIOS declare the CMOS
;   invalid at the next boot. Status B is restored to what it held.
; ⛔ BIT 7 OF PORT 70h IS THE NMI MASK and every index written here keeps it clear.
;
; nasm -f bin p_rtcw.asm -o p_rtcw.com

        org     100h
        jmp     start
%include "probe.inc"

; ---------------------------------------------------------------- state
sv_sec  db      0
sv_min  db      0
sv_hr   db      0
sv_day  db      0
sv_mon  db      0
sv_yr   db      0
sv_cen  db      0
sv_b    db      0
rd_a    db      0
rd_b    db      0
rd_c    db      0

; ---------------------------------------------------------------- helpers
; Read CMOS register AL -> AL. Index bit 7 (NMI mask) kept clear.
cmos_rd:
        and     al, 07Fh
        out     070h, al
        jmp     short $+2
        in      al, 071h
        ret

; Write AH to CMOS register AL.
cmos_wr:
        and     al, 07Fh
        out     070h, al
        jmp     short $+2
        mov     al, ah
        out     071h, al
        ret

; Wait, BOUNDED, for Status A's UIP (bit 7) to clear.
uip_wait:
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

; SET on / off, keeping the rest of Status B as it is.
set_on:
        mov     al, 00Bh
        call    cmos_rd
        or      al, 080h
        mov     ah, al
        mov     al, 00Bh
        call    cmos_wr
        ret
set_off:
        mov     al, 00Bh
        call    cmos_rd
        and     al, 07Fh
        mov     ah, al
        mov     al, 00Bh
        call    cmos_wr
        ret

start:
        PROBE_BEGIN "rtcw"

        ; ---- save the clock, and Status B, so both can be put back
        call    uip_wait
        mov     al, 00h
        call    cmos_rd
        mov     [sv_sec], al
        mov     al, 02h
        call    cmos_rd
        mov     [sv_min], al
        mov     al, 04h
        call    cmos_rd
        mov     [sv_hr], al
        mov     al, 07h
        call    cmos_rd
        mov     [sv_day], al
        mov     al, 08h
        call    cmos_rd
        mov     [sv_mon], al
        mov     al, 09h
        call    cmos_rd
        mov     [sv_yr], al
        mov     al, 32h
        call    cmos_rd
        mov     [sv_cen], al
        mov     al, 0Bh
        call    cmos_rd
        mov     [sv_b], al

; ════════════════════════════════════════════════════════════════════════════
; A. WRITE THE TIME WITH SET HELD, READ IT BACK WHILE SET IS STILL HELD.
;    AX = hours:minutes, DX = seconds. 1234 / 0056 on a chip that took the writes.
; ════════════════════════════════════════════════════════════════════════════
        call    uip_wait
        call    set_on
        mov     ax, 1204h
        call    cmos_wr
        mov     ax, 3402h
        call    cmos_wr
        mov     ax, 5600h
        call    cmos_wr
        mov     al, 04h
        call    cmos_rd
        mov     [rd_a], al
        mov     al, 02h
        call    cmos_rd
        mov     [rd_b], al
        mov     al, 00h
        call    cmos_rd
        mov     [rd_c], al
        POISON
        mov     ah, [rd_a]
        mov     al, [rd_b]
        xor     dh, dh
        mov     dl, [rd_c]
        call    probe_capture
        EMIT    "rtcw.set.held", "AX,DX"

; ════════════════════════════════════════════════════════════════════════════
; B. STATUS B WHILE SET IS HELD -- bit 7 set; UIE (bit 4) masked off.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 0Bh
        call    cmos_rd
        and     al, 0EFh
        xor     ah, ah
        POISON
        call    probe_capture
        EMIT    "rtcw.statusb.held", "AX"

; ════════════════════════════════════════════════════════════════════════════
; C. RELEASE SET; THE CLOCK RUNS FROM 12:34:56.
;    AX = chip hours:minutes; CX = INT 1Ah AH=02h's CH:CL (hours:minutes, BCD).
; ════════════════════════════════════════════════════════════════════════════
        call    set_off
        call    uip_wait
        mov     al, 04h
        call    cmos_rd
        mov     [rd_a], al
        mov     al, 02h
        call    cmos_rd
        mov     [rd_b], al
        mov     ah, 02h
        int     1Ah
        mov     bx, 0B1B1h              ; CX carries the answer: poison the rest
        mov     dx, 0D1D1h
        mov     ah, [rd_a]
        mov     al, [rd_b]
        call    probe_capture
        EMIT    "rtcw.run.chip+bios", "AX,CX"

; ════════════════════════════════════════════════════════════════════════════
; D. DOES DOS'S OWN CLOCK FOLLOW A CHIP WRITE?  AX = 1 if 2Ch's hours are 12.
; ════════════════════════════════════════════════════════════════════════════
        mov     ah, 2Ch
        int     21h
        xor     ax, ax
        cmp     ch, 12
        jne     .d_emit
        inc     ax
.d_emit:
        POISON
        call    probe_capture
        EMIT    "rtcw.dos.follows", "AX"

; ════════════════════════════════════════════════════════════════════════════
; E. THE DATE: 1999-06-15, century 19, with SET held.
;    AX = month:day from the chip, BX = century:year from the chip,
;    CX:DX = INT 1Ah AH=04h (CX = 1999h, DX = 0615h).
; ════════════════════════════════════════════════════════════════════════════
        call    uip_wait
        call    set_on
        mov     ax, 1507h
        call    cmos_wr
        mov     ax, 0608h
        call    cmos_wr
        mov     ax, 9909h
        call    cmos_wr
        mov     ax, 1932h
        call    cmos_wr
        call    set_off
        call    uip_wait
        mov     al, 08h
        call    cmos_rd
        mov     [rd_a], al
        mov     al, 07h
        call    cmos_rd
        mov     [rd_b], al
        mov     al, 32h
        call    cmos_rd
        mov     [rd_c], al
        mov     al, 09h
        call    cmos_rd
        mov     bl, al
        mov     bh, [rd_c]
        push    bx
        mov     ah, 04h
        int     1Ah
        pop     bx
        mov     ah, [rd_a]
        mov     al, [rd_b]
        call    probe_capture
        EMIT    "rtcw.date", "AX,BX,CX,DX"

; ════════════════════════════════════════════════════════════════════════════
; F. BINARY MODE. DM on, write 23:45 as 0x17:0x2D, read back binary; DM off, read
;    back BCD. AX = binary hours:minutes, CX = BCD hours:minutes.
; ════════════════════════════════════════════════════════════════════════════
        call    uip_wait
        mov     al, 0Bh
        call    cmos_rd
        or      al, 084h                ; SET + DM (binary)
        mov     ah, al
        mov     al, 0Bh
        call    cmos_wr
        mov     ax, 1704h
        call    cmos_wr
        mov     ax, 2D02h
        call    cmos_wr
        mov     al, 0Bh
        call    cmos_rd
        and     al, 07Fh                ; release SET, still binary
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
        mov     al, 0Bh
        call    cmos_rd
        and     al, 0FBh                ; back to BCD
        mov     ah, al
        mov     al, 0Bh
        call    cmos_wr
        call    uip_wait
        mov     al, 04h
        call    cmos_rd
        mov     ch, al
        mov     al, 02h
        call    cmos_rd
        mov     cl, al
        mov     bx, 0B1B1h
        mov     dx, 0D1D1h
        mov     ah, [rd_a]
        mov     al, [rd_b]
        call    probe_capture
        EMIT    "rtcw.binary", "AX,CX"

        ; ---- put the clock and Status B back (BCD, as saved)
        call    uip_wait
        call    set_on
        mov     ah, [sv_sec]
        mov     al, 00h
        call    cmos_wr
        mov     ah, [sv_min]
        mov     al, 02h
        call    cmos_wr
        mov     ah, [sv_hr]
        mov     al, 04h
        call    cmos_wr
        mov     ah, [sv_day]
        mov     al, 07h
        call    cmos_wr
        mov     ah, [sv_mon]
        mov     al, 08h
        call    cmos_wr
        mov     ah, [sv_yr]
        mov     al, 09h
        call    cmos_wr
        mov     ah, [sv_cen]
        mov     al, 32h
        call    cmos_wr
        mov     ah, [sv_b]
        and     ah, 07Fh                ; restore with SET released
        mov     al, 0Bh
        call    cmos_wr

        PROBE_END
