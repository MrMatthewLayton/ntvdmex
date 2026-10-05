; p_pit0.com -- how many IRQ0s does counter 0 raise in each mode?   #175
;               docs/ref/pit.md; docs/inventory/pit.md.
;
; ── WHAT THIS IS FOR ─────────────────────────────────────────────────────────
; IRQ0 is counter 0's OUT pin, and the PIC is edge-triggered: one interrupt per
; RISING EDGE of OUT. Per the Intel 8254 datasheet (231164-005):
;   mode 0  OUT goes low at the Control Word, high at terminal count, and stays
;           high: ONE edge per count.
;   mode 4  OUT is high, strobes low for one clock at terminal count: ONE edge.
;   mode 1/5  count only after a GATE rising edge. Counter 0's GATE is tied high
;           on a PC, so it never rises: NO edge, no IRQ0 at all.
;   mode 2  a pulse every period: MANY edges (the control case).
; Our model raised IRQ0 every period in every mode. This asks real machines.
;
; ⚠ THE ONE QUESTION THE DATASHEET DOES NOT SETTLE (case pit0.m0.bare): after
;   mode 0 has reached terminal count, does writing a new count WITHOUT a Control
;   Word drive OUT low again, so the next terminal count is a second edge?
;
; ── EVERY CASE IS A SMALL COUNT, NOT A TIME ──────────────────────────────────
; Each case programs counter 0 with 0x1000 (~3.4 ms), waits ~220 ms and counts
; the INT 08h entries. 0, 1 and 2 are exact; anything above 3 emits 0x10
; ("many"), so the periodic control is stable across hosts and speeds.
;
; ⚠ THIS PROBE REPROGRAMS COUNTER 0 -- the system tick, which p_pit.com never
;   touches. It is put back as a period-correct BIOS leaves it (lo/hi, MODE 3,
;   count 0 = 65536: a real AMI 486 BIOS under PCem, pit.rdback.st0 = 0x36;
;   SeaBIOS leaves mode 2, same rate) and the INT 08h vector is restored. The BIOS tick loses ~1.5 s over the run.
;
; ── THE WAIT IS COUNTER 2, NOT A LOOP ────────────────────────────────────────
; A CPU loop is a different length on every host. Counter 2 in mode 0 with count
; 0xFFFF reaches terminal count after 55 ms on any of them, and 61h bit 5 shows
; its OUT. Four of those = 220 ms. The polls are spaced by a CPU burn (see
; wait55 for why). Each poll is BOUNDED; pit0.wait.ok says
; whether every wait really saw OUT rise (0 = the times below mean nothing).
;
; ORACLE-ALSO: pcem
; nasm -f bin p_pit0.asm -o p_pit0.com

        org     100h
        jmp     start
%include "probe.inc"

; ---------------------------------------------------------------- state
ticks   dw      0                       ; INT 08h entries since the case began
old8    dd      0                       ; the BIOS's INT 08h
waitok  db      1                       ; cleared by any wait that timed out

; ---------------------------------------------------------------- helpers

; Our INT 08h: count, EOI, return. Does NOT chain -- the BIOS would EOI too and
; advance its tick at our rate.
isr8:
        inc     word [cs:ticks]
        push    ax
        mov     al, 020h
        out     020h, al
        pop     ax
        iret

; ~55 ms: counter 2, mode 0, count 0xFFFF, gate on; poll 61h bit 5 until OUT
; rises. Bounded at 16 x 64K polls.
wait55:
        push    cx
        push    dx
        in      al, 061h
        and     al, 0FCh                ; gate low, speaker data off
        out     061h, al
        mov     al, 0B0h                ; 10 11 000 0: ch2, lo/hi, mode 0, binary
        out     043h, al
        mov     al, 0FFh
        out     042h, al
        out     042h, al                ; count 0xFFFF; OUT low
        in      al, 061h
        or      al, 1                   ; gate high: count
        out     061h, al
        mov     dx, 4
.outer: xor     cx, cx
.poll:  in      al, 061h
        test    al, 020h
        jnz     .done
        push    cx                      ; ⚠ SPACE THE PORT READS. A flat-out IN loop is
        mov     cx, 200                 ;   #172 on NTVDMEX (every IN is a trap and IRQ0
.burn:  loop    .burn                   ;   starves: 19 delivered in 1.5 s, measured), so
        pop     cx                      ;   the probe would measure that bug, not modes.
        loop    .poll
        dec     dx
        jnz     .outer
        mov     byte [waitok], 0        ; timed out
.done:  in      al, 061h
        and     al, 0FCh
        out     061h, al
        pop     dx
        pop     cx
        ret

wait220:
        call    wait55
        call    wait55
        call    wait55
        call    wait55
        ret

; AL = counter 0 control word. Program it with count 0x1000 and zero the tally.
arm0:
        cli
        out     043h, al
        mov     word [ticks], 0
        xor     al, al
        out     040h, al
        mov     al, 010h
        out     040h, al                ; count = 0x1000
        sti
        ret

; AX = ticks, bucketed: 0..3 exact, above that 0x10.
tally:
        cli
        mov     ax, [ticks]
        sti
        cmp     ax, 3
        jbe     .r
        mov     ax, 010h
.r:     ret

start:
        PROBE_BEGIN "pit0"

        cli
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:8*4]
        mov     [old8], ax
        mov     ax, [es:8*4+2]
        mov     [old8+2], ax
        mov     word [es:8*4], isr8
        mov     [es:8*4+2], cs
        sti

; ════════════════════════════════════════════════════════════════════════════
; A. MODE 2, the control: a pulse every period.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 034h                ; 00 11 010 0: ch0, lo/hi, mode 2, binary
        call    arm0
        call    wait220
        call    tally
        POISON
        call    probe_capture
        EMIT    "pit0.m2.irqs", "AX"            ; datasheet: 0x10 (many)

; ════════════════════════════════════════════════════════════════════════════
; B. MODE 0: one terminal count, one rising edge.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 030h                ; 00 11 000 0: ch0, lo/hi, mode 0, binary
        call    arm0
        call    wait220
        call    tally
        POISON
        call    probe_capture
        EMIT    "pit0.m0.irqs", "AX"            ; datasheet: 1

; ════════════════════════════════════════════════════════════════════════════
; C. MODE 0 again, a new count WITHOUT a Control Word, after terminal count.
;    The datasheet does not say whether this drives OUT low again.
; ════════════════════════════════════════════════════════════════════════════
        cli
        mov     word [ticks], 0
        xor     al, al
        out     040h, al
        mov     al, 010h
        out     040h, al                ; bare count 0x1000
        sti
        call    wait220
        call    tally
        POISON
        call    probe_capture
        EMIT    "pit0.m0.bare.irqs", "AX"       ; measured, not predicted

; ════════════════════════════════════════════════════════════════════════════
; D. MODE 4: OUT strobes low once at terminal count.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 038h                ; 00 11 100 0: ch0, lo/hi, mode 4, binary
        call    arm0
        call    wait220
        call    tally
        POISON
        call    probe_capture
        EMIT    "pit0.m4.irqs", "AX"            ; datasheet: 1

; ════════════════════════════════════════════════════════════════════════════
; E. MODES 1 AND 5 wait for a GATE rising edge that counter 0 never sees.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 032h                ; 00 11 001 0: ch0, lo/hi, mode 1, binary
        call    arm0
        call    wait220
        call    tally
        POISON
        call    probe_capture
        EMIT    "pit0.m1.irqs", "AX"            ; datasheet: 0

        mov     al, 03Ah                ; 00 11 101 0: ch0, lo/hi, mode 5, binary
        call    arm0
        call    wait220
        call    tally
        POISON
        call    probe_capture
        EMIT    "pit0.m5.irqs", "AX"            ; datasheet: 0

; ---- put counter 0 and INT 08h back as the BIOS had them.
        cli
        mov     al, 036h                ; ch0, lo/hi, mode 3, binary
        out     043h, al
        xor     al, al
        out     040h, al
        out     040h, al                ; count 0 = 65536, 18.2 Hz
        xor     ax, ax
        mov     es, ax
        mov     ax, [old8]
        mov     [es:8*4], ax
        mov     ax, [old8+2]
        mov     [es:8*4+2], ax
        sti

        xor     ax, ax
        mov     al, [waitok]
        POISON
        call    probe_capture
        EMIT    "pit0.wait.ok", "AX"            ; 1 = every wait saw OUT rise

        PROBE_END
