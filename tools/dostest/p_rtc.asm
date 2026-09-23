; p_rtc.com -- the MC146818 clock and CMOS, as software reaches them.
;              docs/ref/rtc.md; docs/inventory/rtc.md.
;
; ── WHAT THIS IS FOR ─────────────────────────────────────────────────────────
; NTVDMEX claims no ports at 70h/71h at all -- the chip does not exist. The BIOS
; SERVICE on top of it does: INT 1Ah AH=02h/04h is answered out of the host's
; clock, in the PIT VDD of all places. So the firmware works and the hardware
; underneath it is absent, which is the same split the 8042 turned out to have.
;
; An unclaimed port reads 0x00 on our bus (iio_in leaves v=0), so the failure is
; not a hang -- it is SILENT AND PLAUSIBLE:
;   * Status A reads 0x00, so UIP is clear and the canonical "wait for UIP, then
;     read the time" loop sails straight through...
;   * ...and then reads 00:00:00 on the 1st of month 0, year 0.
;   * Status D reads 0x00, so VRT is CLEAR: "this machine's CMOS battery is dead
;     and its contents are garbage."
;   * The equipment byte reads 0: no floppies, no video, no coprocessor.
; A guest that asks the BIOS gets the right time and a guest that asks the CHIP
; gets midnight. Those are the same clock.
;
; ── WHAT THE CASES ASK ───────────────────────────────────────────────────────
;   A  Does the time agree with the BIOS? Read INT 1Ah AH=02h and the chip, and
;      emit whether the HOURS match. Not the value -- that changes -- but whether
;      the two doors onto one clock say the same thing.
;   B  Status B's shape: BCD, 24-hour. The bits a PC BIOS leaves.
;   C  Status D's VRT bit: is the CMOS claimed to be valid?
;   D  The equipment byte (14h): non-zero on any real machine.
;   E  Is Status C cleared by reading it? Read twice; the second must be 0.
;
; ── THE TRAPS ────────────────────────────────────────────────────────────────
; * ⛔⛔ BIT 7 OF PORT 70h IS THE NMI MASK. Every index write here keeps bit 7
;   CLEAR, which is "NMI enabled" -- the state a machine runs in. Writing an
;   index with bit 7 set and never clearing it would leave NMI masked for
;   everything that runs afterwards on that machine.
; * ⛔ THIS PROBE ONLY READS. It never writes 71h. A write to the wrong CMOS
;   register can invalidate the checksum at 2Eh/2Fh and make the BIOS declare the
;   configuration bad at the next boot -- on the rig that is a real machine with
;   a real battery, and the damage outlives the run.
; * ⚠ THE SECONDS FIELD IS NOT COMPARABLE and no case emits one. Case A compares
;   HOURS between two doors onto the same clock, which is stable unless a run
;   straddles the hour; minutes would straddle 60x more often for no more signal.
; * EVERY UIP WAIT IS BOUNDED -- a host that answers 0xFF would otherwise spin
;   for ever on a bit that is never going to clear.
;
; ORACLE-ALSO: pcem   (a real AMI 486 BIOS with a real CMOS -- see pcemoracle.py)
; nasm -f bin p_rtc.asm -o p_rtc.com

        org     100h
        jmp     start
%include "probe.inc"

; ---------------------------------------------------------------- state
bios_hr db      0FFh                    ; hours per INT 1Ah AH=02h (BCD)
chip_hr db      0FFh                    ; hours per the chip       (BCD)
stat_b  db      0
stat_d  db      0
equip   db      0
stat_c1 db      0
stat_c2 db      0

; ---------------------------------------------------------------- helpers

; Read CMOS register AL -> AL.  ⛔ Bit 7 of the index is the NMI MASK and is kept
; CLEAR: this probe must not leave NMI disabled behind it.
cmos_rd:
        and     al, 07Fh                ; NMI stays ENABLED
        out     070h, al
        jmp     short $+2               ; the classic I/O settling delay
        in      al, 071h
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

start:
        PROBE_BEGIN "rtc"

; ════════════════════════════════════════════════════════════════════════════
; A. ★★ TWO DOORS ONTO ONE CLOCK.   ref/rtc.md 2
;
;    INT 1Ah AH=02h is the BIOS service; registers 04h/02h/00h are the chip it is
;    supposed to be reading. They must agree, and the case emits whether they do
;    rather than what they say -- a time is not comparable across hosts, an
;    AGREEMENT is.
;    AH = 1 if the hours match, AL = 1 if the chip's hours are even plausible
;    (a valid BCD value below 24h). Both 1 on a machine; 0000 where the chip does
;    not exist and answers 00 while the BIOS answers the real time.
;    ⚠ Unless the run happens at midnight, when the chip's 00 is also the right
;      answer. That is a one-hour-in-24 blind spot in the FIRST half, which is
;      why the second half asks a question midnight cannot fake: 00 is valid BCD,
;      so AL stays 1 either way and only AH can be fooled.
; ════════════════════════════════════════════════════════════════════════════
        mov     ah, 002h                ; BIOS: get RTC time -> CH = hours, BCD
        int     01Ah
        mov     [bios_hr], ch

        call    uip_wait
        mov     al, 004h                ; the chip's hours register
        call    cmos_rd
        mov     [chip_hr], al

        xor     bx, bx
        mov     al, [chip_hr]           ; plausible BCD hour? (nibbles <= 9, < 24h)
        mov     ah, al
        and     al, 00Fh
        cmp     al, 9
        ja      .a_emit
        mov     al, ah
        shr     al, 4
        cmp     al, 2
        ja      .a_emit
        mov     bl, 1                   ; AL half: the chip's hour is well-formed
.a_emit:
        mov     al, [chip_hr]
        cmp     al, [bios_hr]
        jne     .a_no
        mov     bh, 1                   ; AH half: the two doors agree
.a_no:
        mov     ax, bx
        POISON
        call    probe_capture
        EMIT    "rtc.agree.hours", "AX"

; ════════════════════════════════════════════════════════════════════════════
; B. STATUS B -- THE SHAPE A PC BIOS LEAVES.   ref/rtc.md 2
;
;    Bit 2 (DM) clear = BCD, bit 1 set = 24-hour. Emitted with the three
;    INTERRUPT ENABLES masked off: whether PIE/AIE/UIE happen to be on is a
;    property of what has run on the machine, not of the machine.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 00Bh
        call    cmos_rd
        and     al, 08Fh                ; keep SET, SQWE, DM, 24/12, DSE
        mov     [stat_b], al
        xor     ah, ah
        mov     al, [stat_b]
        POISON
        call    probe_capture
        EMIT    "rtc.statusb", "AX"

; ════════════════════════════════════════════════════════════════════════════
; C. STATUS D -- IS THE CMOS CLAIMED TO BE VALID?   ref/rtc.md 2
;
;    Bit 7 is VRT, "valid RAM and time". CLEAR means "the battery died and
;    everything in here is garbage", which firmware and setup programs act on.
;    A host with no chip answers 0x00 -- i.e. it tells every guest that asks that
;    its CMOS is invalid.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 00Dh
        call    cmos_rd
        and     al, 080h
        mov     [stat_d], al
        xor     ah, ah
        mov     al, [stat_d]
        POISON
        call    probe_capture
        EMIT    "rtc.statusd.vrt", "AX"

; ════════════════════════════════════════════════════════════════════════════
; D. THE EQUIPMENT BYTE.   ref/rtc.md 3
;
;    CMOS 14h: floppies fitted, video type, coprocessor. Non-zero on any machine
;    that has POSTed. Emitted with the floppy-count nibble masked off, because
;    how many drives a host was configured with is not a property worth diffing;
;    what is left is bits 3:0 -- video type and the coprocessor bit.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 014h
        call    cmos_rd
        and     al, 00Fh
        mov     [equip], al
        xor     ah, ah
        mov     al, [equip]
        POISON
        call    probe_capture
        EMIT    "rtc.equip.low", "AX"

; ════════════════════════════════════════════════════════════════════════════
; E. ★ IS STATUS C CLEARED BY READING IT?   ref/rtc.md 2
;
;    This is how IRQ8 is acknowledged at the chip: the flags latch, and the READ
;    clears them. A handler that does not read 0Ch gets exactly one interrupt and
;    then silence, because the chip will not re-assert while a flag is set.
;    AH = the first read, AL = the second. The second must be 0 on any machine
;    that implements the register -- and 0000h is ALSO what a host with no chip
;    answers, so this case is only meaningful alongside the ones above.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 00Ch
        call    cmos_rd
        mov     [stat_c1], al
        mov     al, 00Ch
        call    cmos_rd
        mov     [stat_c2], al
        mov     ah, [stat_c1]
        mov     al, [stat_c2]
        POISON
        call    probe_capture
        EMIT    "rtc.statusc.clear", "AX"

        PROBE_END
