; p_ps2aux.com -- the PS/2 mouse: the BIOS's INT 15h AX=C2xxh, and the 8042's aux port. #199
;              src/vdd/vdd_aux.h; docs/ref/kbc.md section 6.
;
; ── WHAT THIS IS FOR ─────────────────────────────────────────────────────────
; A program that brings its own mouse driver never calls INT 33h: it asks the BIOS
; (C204h type, C205h initialise, C201h reset) or programs the mouse itself through the
; 8042 (D4h routes the next 60h byte to the mouse; A9h tests the interface). Ours had
; none of it. This asks each host what it answers:
;
;   ps2.c204.type      AH status, BH device ID      (CF too)
;   ps2.c205.init      BH=3 -> AH status            (CF too)
;   ps2.c201.reset     AH status, BH ID, BL AAh?    (CF too)
;   ps2.c208.invalid   AH status for a function that does not exist
;   ps2.kbc.a9.test    A9h -> AL the interface test result, AH 1 if a byte came
;   ps2.kbc.d4.f2.id   D4h F2h -> AH bytes received, AL first, BL second
;
; ── THE TRAPS ────────────────────────────────────────────────────────────────
; * IRQ12 IS MASKED AT THE SLAVE PIC (A1h bit 4) around the 8042 cases, so a BIOS IRQ12
;   handler cannot take the mouse's replies first; the mask is restored on every path.
; * EVERY WAIT IS BOUNDED, as in p_kbc: no reply is an answer.
; * NOTHING HERE ENABLES REPORTING (F4h) or C200h, so no host is left with a mouse
;   streaming packets at a program that is not reading them.
;
; ⚠ UNRUN as written (s101): no oracle has answered it yet.
;
; ORACLE-ALSO: pcem   (a real AMI 486 BIOS -- see scripts/pcemoracle.py)
; nasm -f bin p_ps2aux.asm -o p_ps2aux.com

        org     100h
        jmp     start
%include "probe.inc"

pic2_was db     0
got      db     0
b0       db     0
b1       db     0

; Wait, BOUNDED, for IBF (bit 1) to clear. CF=1 on timeout.
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

; Wait, BOUNDED, for OBF (bit 0) to set. CF=1 on timeout.
kbc_wait_out:
        push    ax
        push    cx
        mov     cx, 0F000h
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

drain:
        push    ax
        push    cx
        mov     cx, 32
.l:     in      al, 064h
        test    al, 001h
        jz      .done
        in      al, 060h
        loop    .l
.done:  pop     cx
        pop     ax
        ret

start:
        PROBE_BEGIN "ps2aux"

; A. C204h: device type.
        mov     ax, 0C204h
        mov     bx, 0FFFFh
        int     15h
        call    probe_capture
        EMIT    "ps2.c204.type", "AX,BX,CF"

; B. C205h BH=3: initialise for 3-byte packets.
        mov     ax, 0C205h
        mov     bh, 3
        int     15h
        call    probe_capture
        EMIT    "ps2.c205.init", "AX,CF"

; C. C201h: reset.
        mov     ax, 0C201h
        mov     bx, 0FFFFh
        int     15h
        call    probe_capture
        EMIT    "ps2.c201.reset", "AX,BX,CF"

; D. C208h: no such function.
        mov     ax, 0C208h
        int     15h
        call    probe_capture
        EMIT    "ps2.c208.invalid", "AX,CF"

; The 8042 cases, IRQ12 masked.
        cli
        in      al, 0A1h
        mov     [pic2_was], al
        or      al, 010h
        out     0A1h, al
        sti
        call    drain

; E. A9h: test the aux interface.
        mov     byte [got], 0
        mov     byte [b0], 0FFh
        call    kbc_wait_in
        mov     al, 0A9h
        out     064h, al
        call    kbc_wait_out
        jc      .e_none
        in      al, 060h
        mov     [b0], al
        mov     byte [got], 1
.e_none:
        mov     ah, [got]
        mov     al, [b0]
        call    probe_capture
        EMIT    "ps2.kbc.a9.test", "AX"
        call    drain

; F. D4h F2h: the mouse's ID through the 8042.
        mov     byte [got], 0
        mov     byte [b0], 0
        mov     byte [b1], 0
        call    kbc_wait_in
        mov     al, 0D4h
        out     064h, al
        call    kbc_wait_in
        mov     al, 0F2h
        out     060h, al
        call    kbc_wait_out
        jc      .f_done
        in      al, 060h
        mov     [b0], al
        inc     byte [got]
        call    kbc_wait_out
        jc      .f_done
        in      al, 060h
        mov     [b1], al
        inc     byte [got]
.f_done:
        mov     ah, [got]
        mov     al, [b0]
        mov     bl, [b1]
        mov     bh, 0
        call    probe_capture
        EMIT    "ps2.kbc.d4.f2.id", "AX,BX"
        call    drain

        cli
        mov     al, [pic2_was]
        out     0A1h, al
        sti
        PROBE_END
