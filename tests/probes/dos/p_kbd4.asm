; p_kbd4.com -- the KEYBOARD's own commands, sent through port 60h.   #180
;              docs/ref/kbc.md; docs/inventory/kbc.md section 5.
;
; ── WHAT THIS IS FOR ─────────────────────────────────────────────────────────
; p_kbc.asm asked the 8042 CONTROLLER (port 64h commands). A byte written to 60h
; with no controller command pending goes to the KEYBOARD, and the keyboard
; answers every command it accepts with FAh -- or does it, on each of the hosts?
; inventory/kbc.md section 5 marks "any FAh ACK, ever" MISS for us, and records
; that the last two attempts to add one went wrong. This asks the oracles first:
;
;   * EEh, echo:      the keyboard sends EEh back, not FAh.
;   * F2h, identify:  FAh, then the ID bytes (an MF2 keyboard sends ABh 83h).
;   * EDh, set LEDs:  FAh, then the LED byte, then FAh again.
;   * F4h, enable:    FAh.
;   * F3h, typematic: FAh, the rate byte, FAh.
;   * DOES THE REPLY RAISE IRQ1? The 8042 raises IRQ1 for every byte it puts in
;     the output buffer while the command byte enables it -- which is how a BIOS
;     INT 09h handler sees the FAh and sets BDA 0040:0097 bit 4.
;
; ── THE TRAPS ────────────────────────────────────────────────────────────────
; * IRQ1 IS MASKED AT THE PIC while the commands run, so the BIOS's INT 09h
;   cannot eat the reply before we read it. The mask is restored on every path,
;   and the case that asks about IRQ1 unmasks it on purpose with its own handler.
; * EVERY WAIT IS BOUNDED (kbc_wait_out), as in p_kbc: no reply is an answer.
; * NO RESET. FFh resets the keyboard and on real hardware it then runs its self
;   test; nothing here sends it, and nothing here writes the 8042 output port.
; * THE LEDs ARE RESTORED: EDh is sent with 00h (all off) and then with the
;   value BDA 0040:0097 bits 0-2 say they were.
;
; Each case emits AH = how many bytes arrived (0-3), AL = the first, and BL/BH the
; second and third, so "no reply" and "the wrong reply" are different answers.
;
; ORACLE-ALSO: pcem   (a real AMI 486 BIOS -- see scripts/pcemoracle.py)
; nasm -f bin p_kbd4.asm -o p_kbd4.com

        org     100h
        jmp     start
%include "probe.inc"

; ---------------------------------------------------------------- state
pic_was db      0                       ; the PIC1 mask as we found it
got     db      0                       ; bytes received by the last exchange
b0      db      0
b1      db      0
b2      db      0
irq1n   db      0                       ; INT 09h hits counted by our handler
old9    dd      0

; ---------------------------------------------------------------- helpers

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

; Drain the output buffer (bounded).
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

; Send AL to the KEYBOARD (port 60h), then collect up to CL reply bytes into
; b0..b2, the count into [got]. Every wait bounded.
kbd_send:
        mov     byte [got], 0
        mov     byte [b0], 0
        mov     byte [b1], 0
        mov     byte [b2], 0
        call    kbc_wait_in
        jc      .out
        out     060h, al
        mov     di, b0
.next:  or      cl, cl
        jz      .out
        call    kbc_wait_out
        jc      .out
        in      al, 060h
        mov     [di], al
        inc     di
        inc     byte [got]
        dec     cl
        jmp     .next
.out:   ret

; AH = count, AL = first, BL = second, BH = third.
result:
        mov     ah, [got]
        mov     al, [b0]
        mov     bl, [b1]
        mov     bh, [b2]
        ret

int9:   push    ax
        inc     byte [cs:irq1n]
        in      al, 060h                ; take the byte, as a handler would
        mov     al, 020h
        out     020h, al                ; EOI
        pop     ax
        iret

start:
        PROBE_BEGIN "kbd4"
        cli
        in      al, 021h
        mov     [pic_was], al
        or      al, 002h                ; mask IRQ1
        out     021h, al
        sti
        call    drain

; A. EEh, echo.
        mov     al, 0EEh
        mov     cl, 1
        call    kbd_send
        call    result
        call    probe_capture
        EMIT    "kbd.cmd.ee.echo", "AX"
        call    drain

; B. F2h, identify: up to three bytes.
        mov     al, 0F2h
        mov     cl, 3
        call    kbd_send
        call    result
        call    probe_capture
        EMIT    "kbd.cmd.f2.id", "AX,BX"
        call    drain

; C. EDh, set LEDs: the ACK to the command, then to its parameter (00h = all off).
        mov     al, 0EDh
        mov     cl, 1
        call    kbd_send
        mov     dl, [got]
        mov     dh, [b0]
        mov     al, 000h
        mov     cl, 1
        call    kbd_send
        mov     ah, dl                  ; AH = count after EDh, AL = its byte
        mov     al, dh
        mov     bh, [got]               ; BH = count after the parameter, BL = its byte
        mov     bl, [b0]
        call    probe_capture
        EMIT    "kbd.cmd.ed.leds", "AX,BX"
        call    drain
        ; put the LEDs back the way the BIOS says they were (0040:0097 bits 0-2)
        push    es
        mov     ax, 0040h
        mov     es, ax
        mov     dl, [es:0097h]
        pop     es
        and     dl, 007h
        mov     al, 0EDh
        mov     cl, 1
        call    kbd_send
        mov     al, dl
        mov     cl, 1
        call    kbd_send
        call    drain

; D. F4h, enable scanning.
        mov     al, 0F4h
        mov     cl, 1
        call    kbd_send
        call    result
        call    probe_capture
        EMIT    "kbd.cmd.f4.enable", "AX"
        call    drain

; E. F3h, typematic, with 20h (500 ms, 30 cps -- the power-on default).
        mov     al, 0F3h
        mov     cl, 1
        call    kbd_send
        mov     dl, [got]
        mov     dh, [b0]
        mov     al, 020h
        mov     cl, 1
        call    kbd_send
        mov     ah, dl
        mov     al, dh
        mov     bh, [got]
        mov     bl, [b0]
        call    probe_capture
        EMIT    "kbd.cmd.f3.typematic", "AX,BX"
        call    drain

; F. DOES THE ACK RAISE IRQ1? Our own INT 09h handler, IRQ1 unmasked, EEh sent;
;    count the interrupts that arrive within a bounded wait.
        push    es
        xor     ax, ax
        mov     es, ax
        cli
        mov     ax, [es:9*4]
        mov     [old9], ax
        mov     ax, [es:9*4+2]
        mov     [old9+2], ax
        mov     word [es:9*4], int9
        mov     [es:9*4+2], cs
        mov     byte [irq1n], 0
        in      al, 021h
        and     al, 0FDh                ; unmask IRQ1
        out     021h, al
        sti
        call    kbc_wait_in
        mov     al, 0EEh
        out     060h, al
        mov     cx, 0F000h
.w:     cmp     byte [irq1n], 0
        jne     .seen
        loop    .w
.seen:  cli
        in      al, 021h
        or      al, 002h                ; mask it again
        out     021h, al
        mov     ax, [old9]
        mov     [es:9*4], ax
        mov     ax, [old9+2]
        mov     [es:9*4+2], ax
        sti
        pop     es
        call    drain
        xor     ah, ah
        mov     al, [irq1n]
        call    probe_capture
        EMIT    "kbd.ack.irq1", "AX"

; G. ADh (keyboard interface off), THEN a keyboard command: does its reply arrive
;    while the interface is off? (Some 8042s re-enable it on a write to the
;    keyboard; if ours withheld the reply, a driver doing this would wait for
;    ever.) AH/AL = what arrived before AEh; BH/BL = what arrived after it.
;    ⚠ AEh IS SENT ON EVERY PATH, so the keyboard is never left off.
        mov     al, 0ADh
        call    kbc_wait_in
        out     064h, al
        mov     al, 0EEh
        mov     cl, 1
        call    kbd_send
        mov     dl, [got]
        mov     dh, [b0]
        mov     al, 0AEh
        call    kbc_wait_in
        out     064h, al
        mov     byte [got], 0
        mov     byte [b0], 0
        call    kbc_wait_out
        jc      .g_none
        in      al, 060h
        mov     [b0], al
        inc     byte [got]
.g_none:
        mov     ah, dl
        mov     al, dh
        mov     bh, [got]
        mov     bl, [b0]
        call    probe_capture
        EMIT    "kbd.cmd.ee.while.disabled", "AX,BX"
        call    drain

        cli
        mov     al, [pic_was]
        out     021h, al
        sti
        PROBE_END
