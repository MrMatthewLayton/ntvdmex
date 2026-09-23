; p_pit.com -- the 8254 as software can actually observe it.
;              docs/ref/pit.md; docs/inventory/pit.md.
;
; ── WHAT THIS IS FOR ─────────────────────────────────────────────────────────
; The inventory marked counter 0 as well modelled and the rest of the chip as
; barely present: counters 1 and 2 read back 0xFF, the Read-Back Command is
; swallowed whole, BCD is dropped, modes 6/7 are not aliased to 2/3, and port
; 61h bit 5 echoes a written bit instead of reporting counter 2's OUT pin.
;
; Those are claims about OUR code. This asks the same questions of a real
; machine, so that each one becomes a number two hosts either agree on or do not.
;
; ── EVERY CASE IS DETERMINISTIC *ON A HOST THAT IMPLEMENTS THE SURFACE* ──────
; A counter's VALUE depends on when you look, so no case emits one. They emit
; STRUCTURE instead: did two reads differ (is it counting at all), are the digits
; BCD-valid, what do the read-back status bits say. Those are the same on any
; machine and on any day, which is what makes a mismatch mean something.
;
; ⚠ THE GUARANTEE IS CONDITIONAL, AND THIS COMMENT FIRST CLAIMED IT WAS ABSOLUTE.
;   The three read-back cases are stable only because a STATUS BYTE is stable. A
;   host that ignores the Read-Back Command leaves 43h undecoded, so the following
;   IN hands back a LIVE COUNT and the case varies run to run. The VERDICT stays
;   MISMATCH either way; the mismatching VALUE does not, so do not record one
;   run's figure as though it were fixed.
;
; ⚠ NOTHING HERE REPROGRAMS COUNTER 0. It is the system tick: leave it in a
;   different mode and DOS loses time for the rest of the session, and on the rig
;   that outlives the probe. Counter 0 is only ever READ -- how the BIOS left it
;   is itself a known expectation worth checking (MEASURED: mode 2, not the mode 3
;   this probe was first written to expect). Mode and
;   BCD experiments use counter 2, which drives nothing but the speaker, and it is
;   put back at the end with the speaker gated off.
;
; ── THE TRAPS ────────────────────────────────────────────────────────────────
; * READ-BACK'S TWO LATCH BITS ARE ACTIVE LOW. Bit 5 = 0 latches the count, bit
;   4 = 0 latches the status. Writing 1s there is the natural-looking mistake and
;   it asks for nothing.
; * IF BOTH ARE LATCHED THE STATUS BYTE COMES OUT FIRST, then the count. This
;   probe latches only ONE at a time so the order cannot confuse the result.
; * STATUS BIT 7 IS THE OUT PIN AND BIT 6 IS NULL COUNT -- both time-varying, so
;   every status case masks them off and emits only the programmed fields (5:0).
; * A BOUNDED POLL, ALWAYS. Waiting for an OUT pin that never moves is a hang on
;   a host that does not model it, and a hang reads as a harness timeout rather
;   than as data.
;
; ORACLE-ALSO: pcem
; nasm -f bin p_pit.asm -o p_pit.com

        org     100h
        jmp     start
%include "probe.inc"

; ---------------------------------------------------------------- state
bcdok   db      1                       ; cleared by any non-BCD nibble, any sample

; ---------------------------------------------------------------- helpers

out43:                                  ; AL -> control port
        out     043h, al
        ret

; Latch counter N's count and read both bytes -> AX (lo, hi).
; CL = counter number (0,1,2); assumes lo/hi access.
latch_read:
        mov     al, cl
        shl     al, 6                   ; counter select in bits 7:6
        call    out43                   ; access bits 00 = Counter Latch Command
        mov     dx, 040h
        xor     dh, dh
        mov     dl, cl
        add     dl, 040h                ; 40h + counter
        in      al, dx
        mov     ah, al                  ; save lo
        in      al, dx
        xchg    al, ah                  ; AL=lo, AH=hi
        ret

; Read-Back: latch STATUS only for the counter whose select-bit is in AL.
; (bit 1 = counter 0, bit 2 = counter 1, bit 3 = counter 2)
rdback_status:                          ; AL = counter select bit -> AL = status
        push    dx
        or      al, 0E0h                ; 11 100 000: read-back, latch-count DISABLED
        and     al, 0EFh                ; ...and latch-status ENABLED (bit 4 = 0)
        call    out43
        pop     dx
        ret

start:
        PROBE_BEGIN "pit"

; ════════════════════════════════════════════════════════════════════════════
; A. IS COUNTER 1 COUNTING AT ALL?   ref/pit.md §2
;
;    Two latched reads separated by work. On hardware they differ -- counter 1 is
;    free-running DRAM refresh. A host that returns 0xFF for port 41h gives two
;    identical reads and says "no time is passing".
;    Emits 1 if the two reads differ, 0 if not.
; ════════════════════════════════════════════════════════════════════════════
        mov     cl, 1
        call    latch_read
        mov     bx, ax                  ; first sample
        mov     cx, 400h                ; burn some clocks
.w1:    loop    .w1
        mov     cl, 1
        call    latch_read
        xor     ax, bx                  ; 0 => identical
        mov     ax, 0
        jz      .same1
        inc     ax
.same1: POISON
        call    probe_capture
        EMIT    "pit.ch1.counting", "AX"

; ════════════════════════════════════════════════════════════════════════════
; B. IS COUNTER 2 COUNTING, AND CAN ITS COUNT BE READ BACK?
;
;    Program counter 2 mode 0 (counts down once), full count, gate ON via port
;    61h bit 0 -- then sample twice. Speaker DATA (bit 1) is left OFF throughout,
;    so nothing is audible.
; ════════════════════════════════════════════════════════════════════════════
        in      al, 061h
        mov     bh, al                  ; save port 61h for the restore
        and     al, 0FCh
        or      al, 1                   ; gate ON, speaker data OFF
        out     061h, al

        mov     al, 0B0h                ; 10 11 000 0: ch2, lo/hi, mode 0, binary
        call    out43
        mov     al, 0FFh
        out     042h, al
        mov     al, 0FFh
        out     042h, al                ; count = 0xFFFF

        mov     cl, 2
        call    latch_read
        mov     si, ax
        mov     cx, 400h
.w2:    loop    .w2
        mov     cl, 2
        call    latch_read
        xor     ax, si
        mov     ax, 0
        jz      .same2
        inc     ax
.same2: POISON
        call    probe_capture
        EMIT    "pit.ch2.counting", "AX"

; ════════════════════════════════════════════════════════════════════════════
; C. THE READ-BACK COMMAND AND ITS STATUS BYTE.   ref/pit.md §4
;
;    Counter 0 as the BIOS left it. Bits 7 (OUT) and 6 (null count) are
;    time-varying and are masked off; bits 5:0 are the programmed fields.
;    ⚠ MEASURED 0x34 = lo/hi, MODE 2, binary. This probe was written expecting
;      0x36 (mode 3) FROM MEMORY and was wrong -- both modes give a periodic IRQ0
;      at the same rate, which is how that error survived being written down in
;      docs/ref/pit.md too. The mode is a BIOS choice, not a chip fact.
;
;    ⚠ A host that ignores read-back leaves 43h undecoded and the following read
;      of 40h returns a COUNT, not a status -- which is exactly the failure this
;      case is shaped to catch.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 2                   ; select counter 0
        call    rdback_status
        in      al, 040h
        and     al, 03Fh                ; drop OUT + null-count
        xor     ah, ah
        POISON
        call    probe_capture
        EMIT    "pit.rdback.st0", "AX"  ; MEASURED 0x0034 (mode 2)

; ════════════════════════════════════════════════════════════════════════════
; D. DOES READ-BACK REPORT WHAT WE PROGRAMMED INTO COUNTER 2?
;    Counter 2 was set to lo/hi, mode 0, binary in case B -> bits 5:0 = 0x30.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 8                   ; select counter 2
        call    rdback_status
        in      al, 042h
        and     al, 03Fh
        xor     ah, ah
        POISON
        call    probe_capture
        EMIT    "pit.rdback.st2", "AX"  ; expect 0x0030

; ════════════════════════════════════════════════════════════════════════════
; E. MODES 6 AND 7 ARE ALIASES FOR 2 AND 3.   ref/pit.md §3
;
;    Program counter 2 with mode bits 110 and ask read-back what mode it is in.
;    ⚠ MEASURED: hardware answers 110 -- the status byte reports the bits AS
;      PROGRAMMED and does NOT normalise 6 to 2. So this case measures READ-BACK
;      FIDELITY, not the aliasing BEHAVIOUR; it cannot tell whether the counter
;      actually runs as mode 2. Testing that needs the OUT pin over time, which
;      case G's idiom could be extended to do. Recorded rather than glossed,
;      because a case that looks like it proves something it does not is worse
;      than no case.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 0BCh                ; 10 11 110 0: ch2, lo/hi, mode 6, binary
        call    out43
        mov     al, 010h
        out     042h, al
        mov     al, 000h
        out     042h, al
        mov     al, 8
        call    rdback_status
        in      al, 042h
        and     al, 00Eh                ; the MODE field only
        xor     ah, ah
        POISON
        call    probe_capture
        EMIT    "pit.mode6.readback", "AX"

; ════════════════════════════════════════════════════════════════════════════
; F. BCD COUNTING.   ref/pit.md §3
;
;    Program counter 2 BCD (control bit 0 = 1) with count 0x9999 and latch it.
;    In BCD every nibble is a decimal digit, so all four must be <= 9. A binary
;    counter walks straight through 0xA.. and fails this.
;    Emits 1 if all four nibbles are BCD-valid.
; ════════════════════════════════════════════════════════════════════════════
;    ⚠ SIXTEEN SAMPLES, NOT ONE, AND THE REASON IS A FALSE PASS THIS CASE
;      ACTUALLY PRODUCED. With a single sample a BINARY counter passes whenever
;      its four nibbles happen to be <= 9 -- about one value in six -- and on the
;      first run after counters 1 and 2 became readable it did exactly that:
;      NTVDMEX reported "BCD valid" while not implementing BCD at all. Sixteen
;      spread samples make that essentially impossible (~0.15^16).
;      ⛔ An all-AGREE probe is not a verified surface.
        mov     al, 0B1h                ; 10 11 000 1: ch2, lo/hi, mode 0, BCD
        call    out43
        mov     al, 099h
        out     042h, al
        mov     al, 099h
        out     042h, al
        mov     byte [bcdok], 1
        mov     bp, 16
.bcdsamp:
        mov     cx, 40h
.w3:    loop    .w3
        mov     cl, 2
        call    latch_read              ; AX = the count as read back
        mov     bx, ax
        mov     cx, 4
.digit: mov     ax, bx
        and     al, 00Fh
        cmp     al, 9
        jbe     .ok
        mov     byte [bcdok], 0         ; a nibble > 9 in ANY sample: not BCD
.ok:    shr     bx, 1
        shr     bx, 1
        shr     bx, 1
        shr     bx, 1
        loop    .digit
        dec     bp
        jnz     .bcdsamp
        xor     ax, ax
        mov     al, [bcdok]
        POISON
        call    probe_capture
        EMIT    "pit.bcd.valid", "AX"   ; expect 1

; ════════════════════════════════════════════════════════════════════════════
; G. PORT 61h BIT 5 IS COUNTER 2'S OUT PIN.   ref/pit.md §2
;
;    The classic "measure time without interrupts" idiom: program a short count,
;    poll bit 5, count the loops. Program mode 3 (square wave) with a small
;    divisor so OUT toggles fast, then poll a BOUNDED number of times and report
;    whether the bit was ever seen to CHANGE.
;    Emits 1 if it changed, 0 if it is stuck.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 0B6h                ; 10 11 011 0: ch2, lo/hi, mode 3, binary
        call    out43
        mov     al, 040h
        out     042h, al
        mov     al, 000h                ; count = 0x0040, a fast square wave
        out     042h, al

        in      al, 061h
        and     al, 020h
        mov     bh, al                  ; the first sample of bit 5
        xor     si, si                  ; si = 1 once it changes
        mov     cx, 8000h               ; ⚠ BOUNDED: a stuck bit must not hang
.poll:  in      al, 061h
        and     al, 020h
        cmp     al, bh
        je      .next
        mov     si, 1
        jmp     .polled
.next:  loop    .poll
.polled:
        mov     ax, si
        POISON
        call    probe_capture
        EMIT    "pit.61h.bit5.toggles", "AX"    ; expect 1

; ---- put counter 2 and the speaker back.
        mov     al, 0B6h                ; ch2, lo/hi, mode 3, binary -- the usual state
        call    out43
        xor     al, al
        out     042h, al
        out     042h, al
        in      al, 061h
        and     al, 0FCh                ; gate off, speaker data off
        out     061h, al

        PROBE_END
