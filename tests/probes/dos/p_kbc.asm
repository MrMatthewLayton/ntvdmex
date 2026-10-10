; p_kbc.com -- the 8042 as a CONTROLLER, not as a scancode FIFO.
;              docs/ref/kbc.md; docs/inventory/kbc.md.
;
; ── WHAT THIS IS FOR ─────────────────────────────────────────────────────────
; p_kbd.asm asks about INT 16h and the BDA -- the firmware. Nothing has ever
; asked about the chip underneath it, and docs/inventory/keyboard.md has said so
; for two sessions ("port 60h/64h re-read semantics, 8042 status bits: not yet
; inventoried"). Marking the code against the datasheet first predicted four
; gaps, and this asks three real machines about them:
;
;   * WHAT IS IN THE STATUS REGISTER WHEN NOTHING IS HAPPENING? We answer 1 bit
;     of 8. Bit 2 (SYS) is set by POST on any machine DOS is running on; we say 0.
;   * DOES THE CONTROLLER ANSWER A COMMAND AT ALL? `AAh` self test must put 55h
;     in the output buffer. We discard the command, so a poll for OBF never ends.
;   * WHERE IS A20? It is bit 1 of the 8042's output port, readable with `D0h`.
;     We do not implement the output port, so that read returns the last
;     SCANCODE -- and a driver reads bit 1 of it as the state of the A20 gate.
;   * AND DO THE THREE A20 DOORS AGREE? Enable it through the 8042, then ask the
;     XMS driver. They are one wire. We keep one flag per door and only XMS has one.
;
; ── ⛔⛔⛔ THE SAFETY NOTE. TWO OF THESE PORTS REBOOT THE MACHINE. ─────────────
; * 8042 output port bit 0 is **CPU RESET, ACTIVE LOW**. `D1h` followed by a byte
;   with bit 0 CLEAR resets the machine immediately. Every write here is `DFh`
;   (A20 on, reset line high) or `DDh` (A20 off, reset line high) -- bit 0 is set
;   in both, and there is no path through this file that writes an even byte to
;   60h after a D1h.
; * Port 92h bit 0 is the PS/2 FAST RESET, active HIGH. This probe therefore
;   READS 92h and never writes it. A read cannot reset anything.
; ⚠ On the test machine NTVDMEX traps these ports in user mode, so a mistake cannot reach
;   the physical box -- but under QEMU, dosbox-x and PCem the guest IS the
;   machine, and a reset there loses the run, not just the case.
;
; ── THE OTHER TRAPS ──────────────────────────────────────────────────────────
; * EVERY WAIT IS BOUNDED. Polling OBF for a reply that a host never produces is
;   the expected failure on three of the four hosts here, and an unbounded poll
;   turns that into a harness timeout instead of a data point. Each wait returns
;   a flag saying whether it succeeded, and the flag is what gets emitted.
; * A20 IS ONLY EVER ENABLED, NEVER DISABLED. Enabling a gate that is already
;   open costs nothing and cannot strand anything; disabling it can strand DOS
;   itself, which lives in the HMA when DOS=HIGH. There is therefore no "restore"
;   to get wrong -- the safe direction is the only direction taken. See case E.
; * THE KEYBOARD IS LEFT ENABLED. Nothing here sends ADh (disable keyboard)
;   without an AEh, because a host that DOES implement it and no subsequent
;   enable leaves the machine unable to type.
;
; ORACLE-ALSO: pcem   (a real AMI 486 BIOS -- see scripts/pcemoracle.py)
; nasm -f bin p_kbc.asm -o p_kbc.com

        org     100h
        jmp     start
%include "probe.inc"

; ---------------------------------------------------------------- state
a20_was db      0                       ; the gate as we found it (XMS's view)
outp_d0 db      0FFh                    ; the output port, read back with D0h
st_idle db      0FFh                    ; 64h with nothing in the buffer
selftst db      0                       ; 1 = a byte came back from AAh
reply   db      0                       ; ...and what it was
gotop   db      0                       ; 1 = a byte came back from D0h
p92_val db      0FFh                    ; System Control Port A, as read
a20_rb  db      0                       ; A20 as the output port reads it BACK

; ---------------------------------------------------------------- helpers

; Wait, BOUNDED, for status bit 1 (IBF) to clear -- "the controller has taken the
; last byte". CF=1 on timeout.
kbc_wait_in:
        push    ax
        push    cx
        mov     cx, 2000h
.l:     in      al, 064h
        test    al, 002h
        jz      .ok
        loop    .l
        pop     cx
        pop     ax
        stc
        ret
.ok:    pop     cx
        pop     ax
        clc
        ret

; Wait, BOUNDED, for status bit 0 (OBF) to set -- "there is a byte to read".
; CF=1 on timeout, which is the EXPECTED answer on a host that drops commands.
kbc_wait_out:
        push    ax
        push    cx
        mov     cx, 2000h
.l:     in      al, 064h
        test    al, 001h
        jnz     .ok
        loop    .l
        pop     cx
        pop     ax
        stc
        ret
.ok:    pop     cx
        pop     ax
        clc
        ret

; Send the 8042 command in AL. CF=1 if the controller never became ready.
kbc_cmd:
        push    ax
        call    kbc_wait_in
        jc      .fail
        pop     ax
        out     064h, al
        clc
        ret
.fail:  pop     ax
        stc
        ret

start:
        PROBE_BEGIN "kbc"

; ════════════════════════════════════════════════════════════════════════════
; A. THE STATUS REGISTER WITH NOTHING HAPPENING.   ref/kbc.md 2
;
;    Drain anything the buffer is holding first -- a scancode left over from the
;    keystroke that launched us would set OBF and make the case depend on how the
;    probe was started. Then read the idle status.
;    ⚠ Bit 0 (OBF) and bit 5 (AUXB) are masked out of the emitted value: both are
;      "there is a byte waiting", which is a function of WHEN you look. What is
;      left is the part that is a property of the machine -- above all bit 2,
;      SYS, which POST sets and which we answer 0.
; ════════════════════════════════════════════════════════════════════════════
        mov     cx, 32
.drain: in      al, 064h
        test    al, 001h
        jz      .drained
        in      al, 060h
        loop    .drain
.drained:
        in      al, 064h
        and     al, 0DEh                ; drop OBF (bit 0) and AUXB (bit 5)
        mov     [st_idle], al
        xor     ah, ah
        mov     al, [st_idle]
        POISON
        call    probe_capture
        EMIT    "kbc.status.idle", "AX"

; ════════════════════════════════════════════════════════════════════════════
; B. DOES THE CONTROLLER ANSWER A COMMAND?   ref/kbc.md 3
;
;    AAh is the self test and its reply is a fixed byte: 55h. This is the
;    cheapest possible "is there a controller here", and it is the question every
;    later case depends on -- a host that cannot answer AAh cannot answer D0h
;    either, and this says which of the two a mismatch is about.
;    ⛔ AH = DID A BYTE ARRIVE AT ALL, AL = what it was. The first cut emitted
;      only a 0/1 verdict, and 0 then meant BOTH "the controller never replied"
;      and "it replied with the wrong byte" -- two completely different defects
;      collapsed into one number. A verdict that cannot say which of two things
;      happened is worth exactly as much as the cheaper of them.
; ════════════════════════════════════════════════════════════════════════════
        mov     byte [selftst], 0
        mov     byte [reply], 0
        mov     al, 0AAh
        call    kbc_cmd
        jc      .st_done
        call    kbc_wait_out
        jc      .st_done
        mov     byte [selftst], 1       ; something arrived
        in      al, 060h
        mov     [reply], al
.st_done:
        mov     ah, [selftst]
        mov     al, [reply]
        POISON
        call    probe_capture
        EMIT    "kbc.selftest.55", "AX"

; ════════════════════════════════════════════════════════════════════════════
; C. THE OUTPUT PORT, AND A20 INSIDE IT.   ref/kbc.md 3 and 4
;
;    D0h puts the output port in the output buffer. Bit 1 is the A20 gate.
;    ⛔ THE FAILURE SHAPE IS THE POINT: a host that discards D0h leaves whatever
;      was already in the buffer, so the read returns a SCANCODE and the driver
;      reads bit 1 of it as A20. Plausible, wrong, silent. So the case emits the
;      whole byte, not just bit 1 -- the byte is what says which happened.
;    ⚠ A real output port always has bit 0 SET (the reset line is high, or the
;      machine would be resetting), so an answer with bit 0 clear is not an
;      output port at all.
; ════════════════════════════════════════════════════════════════════════════
        mov     byte [gotop], 0
        mov     al, 0D0h
        call    kbc_cmd
        jc      .op_done
        call    kbc_wait_out
        jc      .op_done
        mov     byte [gotop], 1
        in      al, 060h
        mov     [outp_d0], al
.op_done:
        mov     ah, [gotop]             ; AH = a byte arrived, AL = the byte
        mov     al, [outp_d0]
        POISON
        call    probe_capture
        EMIT    "kbc.outport.d0", "AX"

; ════════════════════════════════════════════════════════════════════════════
; D. SYSTEM CONTROL PORT A -- READ ONLY.   ref/kbc.md 4
;
;    92h bit 1 is the fast A20 gate. ⛔ BIT 0 IS THE FAST RESET AND THIS PROBE
;    NEVER WRITES THIS PORT -- a read cannot reset anything, and there is no
;    version of this case worth a reboot.
;    A host that does not decode 92h answers FFh (the ISA bus floating high),
;    which is exactly what NTVDMEX does today because nothing claims the port.
; ════════════════════════════════════════════════════════════════════════════
        in      al, 092h
        mov     [p92_val], al
        xor     ah, ah
        mov     al, [p92_val]
        POISON
        call    probe_capture
        EMIT    "kbc.port92.read", "AX"

; ════════════════════════════════════════════════════════════════════════════
; E. ★★ A20: DOES WHAT YOU WRITE COME BACK?   ref/kbc.md 4
;
;    Open the gate through the 8042 (D1h then DFh), then read the output port
;    back (D0h) and look at bit 1. On any machine that implements the output
;    port this is 1, because that is what was just written to it. On one that
;    discards both commands the read returns whatever was in the output buffer --
;    a SCANCODE -- and bit 1 of it is a coin toss the driver takes for the state
;    of the A20 gate. Plausible, wrong, silent.
;
;    AH = bit 1 as read back from the output port, AL = the XMS driver's answer
;    to "is A20 on" (AH=07h), or FFh with no XMS driver present.
;
; ⛔⛔ THE FIRST CUT OF THIS CASE WAS BOTH UNSAFE AND UNINFORMATIVE, and both
;    faults are worth keeping written down.
;    * UNINFORMATIVE: it enabled A20 and then asked XMS whether A20 was on. With
;      HIMEM loaded the answer is "yes" BEFORE the write too, so agreement and
;      "it was already on" produced the same number. dosbox-x duly answered
;      0101h, which proved nothing.
;    * UNSAFE: the obvious repair -- toggle it to the OPPOSITE state and see if
;      XMS follows -- would call the XMS entry point with A20 DISABLED. With
;      DOS=HIGH, HIMEM and much of DOS live in the HMA, which is only reachable
;      *because* A20 is on. That call is a jump into wrapped memory: a hang, on a
;      XP test machine, for one bit of data.
;    ⇒ So the write stays in the SAFE DIRECTION ONLY (enable), and the read-back
;      comes from the controller itself rather than from a driver living in the
;      HMA. The XMS answer is reported alongside as context, not asserted.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 0D1h                ; write output port
        call    kbc_cmd
        jc      .a20_rd
        call    kbc_wait_in
        jc      .a20_rd
        mov     al, 0DFh                ; A20 ON, reset line HIGH (bit 0 set)
        out     060h, al

.a20_rd:
        mov     al, 0D0h                ; ...and read it straight back
        call    kbc_cmd
        jc      .a20_xms
        call    kbc_wait_out
        jc      .a20_xms
        in      al, 060h
        shr     al, 1
        and     al, 1
        mov     [a20_rb], al

.a20_xms:
        mov     byte [a20_was], 0FFh    ; "no XMS driver" is a distinct answer
        mov     ax, 04300h              ; XMS installation check
        int     02Fh
        cmp     al, 080h
        jne     .a20_emit
        push    es
        mov     ax, 04310h              ; entry point -> ES:BX
        int     02Fh
        mov     [xmsent], bx
        mov     [xmsent + 2], es
        pop     es
        mov     ah, 007h                ; query A20
        call    far [xmsent]
        mov     [a20_was], al

.a20_emit:
        mov     ah, [a20_rb]
        mov     al, [a20_was]
        POISON
        call    probe_capture
        EMIT    "kbc.a20.readback", "AX"

        PROBE_END

; ---------------------------------------------------------------- data
xmsent   dd 0
