; p_uart.com -- the 8250/16550 at COM1, asked the way a driver asks.
;               docs/ref/uart.md; docs/inventory/uart.md.
;
; ── WHAT THIS IS FOR ─────────────────────────────────────────────────────────
; vdd_comm.c is 402 lines and looks thought-through: loopback is implemented as
; a REWIRING rather than a flag, LSR clears its error bits on read, the MSR delta
; bits are acknowledged by the read, IIR's FIFO bits are gated on FCR, and the
; DLAB bank switch is honoured. Nothing has ever asked a real machine whether any
; of that is right.
;
; That is exactly the state the 8259 was in -- "10 fields, all AGREE" and five
; gaps underneath -- so the questions here are chosen to be the ones a DRIVER
; asks, not the ones our code makes easy.
;
; ── WHY LOOPBACK IS THE BACKBONE OF THIS PROBE ───────────────────────────────
; Everything else a UART does depends on what is attached to the other end of the
; wire, which is a property of the HOST's configuration and not of the chip --
; the same trap as the CMOS equipment byte. Loopback (MCR bit 4) disconnects the
; pins and wires the part to itself, so every answer becomes a fact about the
; chip. It is also precisely what every detection routine ever written does.
;
;   transmitter    -> receiver
;   DTR (MCR bit0) -> DSR (MSR bit5)
;   RTS (MCR bit1) -> CTS (MSR bit4)
;   OUT1(MCR bit2) -> RI  (MSR bit6)
;   OUT2(MCR bit3) -> DCD (MSR bit7)
;
; Getting that pairing wrong gives a port that echoes bytes and still fails every
; detection routine -- a failure that looks like success until nothing uses it.
;
; ── THE TRAPS ────────────────────────────────────────────────────────────────
; * ⛔ MCR, LCR AND IER ARE SAVED AND RESTORED. A serial mouse driver may own
;   COM1 on the machine this runs on, and leaving the port in loopback would cut
;   it off from its mouse for the rest of the session.
; * ⛔ DLAB (LCR bit 7) SWITCHES THE REGISTER BANK. With it set, the first two
;   registers are the divisor latch and NOT RBR/IER -- so every access here
;   clears it first except the one case that is testing it, and that case puts
;   it back immediately.
; * ⚠ NO CASE READS A BYTE THAT THE OUTSIDE WORLD WOULD HAVE TO SUPPLY. A host
;   with nothing attached and a host with a modem attached must give the same
;   answers, or the case is measuring the configuration.
; * EVERY WAIT IS BOUNDED -- a host that never sets DR would otherwise spin for
;   ever on a bit that is not coming.
;
; ORACLE-ALSO: pcem   (a real AMI 486 BIOS -- see scripts/pcemoracle.py)
; nasm -f bin p_uart.asm -o p_uart.com

        org     100h
        jmp     start
%include "probe.inc"

COM1    equ     03F8h

; ---------------------------------------------------------------- state
mcr_was db      0
lcr_was db      0
ier_was db      0
echoed  db      0                       ; the byte loopback handed back
loopmsr db      0                       ; MSR while looped with DTR+RTS asserted
scr_rb  db      0                       ; scratch register read-back
dl_rb   dw      0                       ; divisor latch read-back
lsr_id  db      0                       ; LSR with nothing to send or receive
iir_id  db      0                       ; IIR with no interrupt enabled

; ---------------------------------------------------------------- helpers

; AL -> COM1 + AH.  (AH = register offset)
uwr:
        push    dx
        mov     dl, ah
        xor     dh, dh
        add     dx, COM1
        out     dx, al
        pop     dx
        ret

; COM1 + AL -> AL.
urd:
        push    dx
        mov     dl, al
        xor     dh, dh
        add     dx, COM1
        in      al, dx
        pop     dx
        ret

start:
        PROBE_BEGIN "uart"

        ; ---- save what we are about to disturb. See the traps above.
        mov     al, 3
        call    urd
        mov     [lcr_was], al
        and     al, 07Fh                ; DLAB off for everything that follows
        mov     ah, 3
        call    uwr
        mov     al, 4
        call    urd
        mov     [mcr_was], al
        mov     al, 1
        call    urd
        mov     [ier_was], al
        xor     al, al                  ; no interrupts while we poke at it
        mov     ah, 1
        call    uwr

; ════════════════════════════════════════════════════════════════════════════
; A. ★★ THE LOOPBACK SELF-TEST -- DOES A BYTE COME BACK?   ref/uart.md 4
;
;    MCR bit 4 disconnects the pins and wires the transmitter to the receiver.
;    Write a byte, wait (bounded) for LSR's Data Ready, read it out of RBR.
;    This is the whole of "is there a UART here" for every driver ever written,
;    and it is a fact about the CHIP rather than about what is plugged into it.
;    Emits the byte that came back: 5Ah on a working port, FFh where nothing
;    answered at all.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 01Fh                ; LOOP + OUT2 + OUT1 + RTS + DTR
        mov     ah, 4
        call    uwr

        mov     al, 05Ah                ; ...into the transmit holding register
        xor     ah, ah
        call    uwr

        mov     cx, 4000h               ; bounded: a dead port must not hang us
.wdr:   mov     al, 5
        call    urd
        test    al, 001h                ; LSR bit 0 = Data Ready
        jnz     .gotdr
        loop    .wdr
        mov     byte [echoed], 0FFh
        jmp     .a_emit
.gotdr:
        xor     al, al
        call    urd
        mov     [echoed], al
.a_emit:
        xor     ah, ah
        mov     al, [echoed]
        POISON
        call    probe_capture
        EMIT    "uart.loop.echo", "AX"

; ════════════════════════════════════════════════════════════════════════════
; B. ★★ THE FOUR MODEM LINES, LOOPED BACK.   ref/uart.md 4
;
;    Still in loopback with DTR, RTS, OUT1 and OUT2 all asserted, the modem
;    status register must show DSR, CTS, RI and DCD -- bits 5, 4, 6 and 7, i.e.
;    0xF0. The DELTA bits (3:0) are masked off: they say "this changed since you
;    last read", which depends on how many times the probe has looked.
;    ⛔ THIS IS THE CASE THAT CATCHES A TRANSPOSED PAIRING. A port that echoes
;      bytes but maps DTR to CTS instead of DSR passes case A and fails every
;      real detection routine -- success that looks like success.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 6
        call    urd                     ; a first read clears the deltas
        mov     al, 6
        call    urd
        and     al, 0F0h
        mov     [loopmsr], al
        xor     ah, ah
        mov     al, [loopmsr]
        POISON
        call    probe_capture
        EMIT    "uart.loop.msr", "AX"

        ; ---- out of loopback before anything else.
        mov     al, [mcr_was]
        mov     ah, 4
        call    uwr

; ════════════════════════════════════════════════════════════════════════════
; C. THE SCRATCH REGISTER.   ref/uart.md 3
;
;    SCR has no function at all -- and that is why drivers use it: an 8250 does
;    not have one and reads FFh, a 16450 and later store a byte. It is how the
;    part is IDENTIFIED. Write 5Ah, read it back.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 05Ah
        mov     ah, 7
        call    uwr
        mov     al, 7
        call    urd
        mov     [scr_rb], al
        xor     ah, ah
        mov     al, [scr_rb]
        POISON
        call    probe_capture
        EMIT    "uart.scratch", "AX"

; ════════════════════════════════════════════════════════════════════════════
; D. ★ DLAB SWITCHES THE REGISTER BANK.   ref/uart.md 2
;
;    With LCR bit 7 set, the first two registers stop being RBR/THR and IER and
;    become the low and high halves of the baud divisor. A model that stores the
;    bit without switching the bank writes the divisor into the receive buffer
;    and the interrupt enables -- which is silent until the first interrupt.
;    Writes 0x0060 (1200 baud) and reads it back through the same door.
;    AH = high half, AL = low half.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 080h                ; DLAB on
        mov     ah, 3
        call    uwr
        mov     al, 060h
        xor     ah, ah
        call    uwr                     ; divisor low
        mov     al, 000h
        mov     ah, 1
        call    uwr                     ; divisor high
        xor     al, al
        call    urd
        mov     bl, al
        mov     al, 1
        call    urd
        mov     bh, al
        mov     al, 003h                ; DLAB off, 8N1 -- and put the bank back
        mov     ah, 3
        call    uwr
        mov     ax, bx
        POISON
        call    probe_capture
        EMIT    "uart.dlab.divisor", "AX"

; ════════════════════════════════════════════════════════════════════════════
; E. THE TWO IDLE STATUS REGISTERS.   ref/uart.md 3
;
;    AH = LSR with nothing sent and nothing received: the transmitter is empty,
;    so THRE (bit 5) and TEMT (bit 6) are both set -- 60h -- and nothing else is.
;    A driver's "wait until I may transmit" loop is built on THRE, and a driver
;    that drops RTS after the last byte waits on TEMT; a model with only one of
;    them serves one of those two and hangs the other.
;    AL = IIR with every interrupt disabled: 01h, "nothing owed".
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 5
        call    urd
        mov     [lsr_id], al
        mov     al, 2
        call    urd
        mov     [iir_id], al
        mov     ah, [lsr_id]
        mov     al, [iir_id]
        POISON
        call    probe_capture
        EMIT    "uart.idle.lsr.iir", "AX"

; ════════════════════════════════════════════════════════════════════════════
; F. ★ IF SOMETHING IS PENDING AT IDLE, WHAT IS IT?   ref/uart.md 3
;
;    Case E measured LSR and PCem alone reported bit 0 (Data Ready) set: 61h
;    against 60h everywhere else. A VALUE says the hosts differ; it does not say
;    WHY, and the difference between those two is the whole lesson of p_vgaext.
;    So: if DR is set, take the byte out and report it.
;
;    AH = 1 if a byte was pending, AL = the byte (or 00 if not).
;    ⚠ Sequence-dependent by construction -- it can only mean anything read
;      alongside case E, which is why it is a separate case rather than a
;      redefinition of that one: E's numbers stay comparable with the run that
;      first found this.
; ════════════════════════════════════════════════════════════════════════════
        xor     bx, bx
        mov     al, 5
        call    urd
        test    al, 001h
        jz      .f_emit
        mov     bh, 1
        xor     al, al
        call    urd
        mov     bl, al
.f_emit:
        mov     ax, bx
        POISON
        call    probe_capture
        EMIT    "uart.idle.pending", "AX"

        ; ---- put the port back exactly as it was found.
        mov     al, [ier_was]
        mov     ah, 1
        call    uwr
        mov     al, [mcr_was]
        mov     ah, 4
        call    uwr
        mov     al, [lcr_was]
        mov     ah, 3
        call    uwr

        PROBE_END
