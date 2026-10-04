; bios_kbdact.asm -- the BIOS INT 09h's side-calls, as guest code. GH #254.
;
; The source of the bytes in bios_kbdact.h. Rebuild with:
;     nasm -f bin src/dos/bios_kbdact.asm -o /tmp/k.bin -l /tmp/k.lst
; tools/dostest/kbdact_test.c runs the bytes in v86interp.
;
; Our INT 09h is a host BOP (`BOP 09h; IRET`). It translates the key, updates the
; BDA and sends the EOI; when the key also needs the BIOS to CALL something, the host
; resumes the guest at one of these entries (instead of at the stub's IRET), with the
; INT 09h frame still on the stack, and each ends in IRET back to the interrupted code.
;   brk:   INT 1Bh                         Ctrl-Break (DOS's break handler hooks it)
;   prt:   INT 05h                         Print Screen
;   sysd:  INT 15h AX=8500h                SysReq pressed
;   sysu:  INT 15h AX=8501h                SysReq released
;   pause: spin, interrupts ON, until 0040:0018 bit 3 clears -- the next keystroke's
;          INT 09h clears it (and throws that key away), as the IBM BIOS's K39 loop.
; Registers are preserved; the EOI has already been sent, as the BIOS sends it
; before INT 05h and before its pause loop.
;
; ── #244: k4f, THE INT 15h AH=4Fh KEYBOARD INTERCEPT. ─────────────────────────────────
;   An AT/PS/2 BIOS INT 09h reads the byte, then `mov ah,4Fh / stc / int 15h`: a hook
;   may change AL (the scancode the BIOS then translates) or return CF=0 to swallow the
;   byte. (IBM PS/2 TR INT 15h AH=4Fh; RBIL INT 15/AH=4Fh.) The host enters k4f ONLY
;   when IVT[15h] no longer points at our own INT 15h stub -- our default answers CF=1
;   with AL untouched, so calling it would cost two VM exits per byte to change nothing
;   (Doom/Skyroads input is latency-fragile: no new work on the default path).
;   On entry the host has already pushed the interrupted code's AX and loaded
;   AX = 4F00h | scancode; the INT 09h frame is under it. The BOP at k4f+5 is the
;   "translate AL" half: the host translates, sends the EOI, POPS the saved AX itself and
;   resumes in one of the side-calls above (or at brk's IRET). The CF=0 path is ours:
;   the BIOS's own non-specific EOI, restore AX, IRET -- nothing translated or stored.
;
; ── #274: p5, THE DEFAULT INT 05h -- PRINT SCREEN. ───────────────────────────────────
;   IVT[05h] points here. The IBM routine (PC/AT TR, PRINT_SCREEN) prints the text
;   screen through INT 17h (printer 0) and keeps its state in 0050:0000: 01h while
;   printing (a nested INT 05h returns at once), 00h when done, FFh if the printer
;   reported an error (INT 17h AH & 29h). ⛔ Ours keeps that state HOST-side: 0050:0000
;   is the first byte of our own INT 21h stub (main.c prtsc_bop). The loop runs here so every byte goes through
;   the guest's INT 17h -- hooked or not; the host's BOP supplies the next byte (from
;   our INT 10h's read of the screen) and judges INT 17h's AH. begin = p5+1, next =
;   p5+8, both BOP 09h; CF=1 from either means "done", and the host has put back every
;   register it used by then.
        bits 16
        org 0

brk:    sti
        int 1Bh
        iret

prt:    sti
        int 05h
        iret

sysd:   push ax
        mov ax, 8500h
        int 15h
        pop ax
        iret

sysu:   push ax
        mov ax, 8501h
        int 15h
        pop ax
        iret

pause:  sti
        push ds
        push ax
        xor ax, ax
        mov ds, ax
.spin:  test byte [0418h], 08h
        jnz .spin
        pop ax
        pop ds
        iret

k4f:    stc
        int 15h
        jnc .swal
        db 0C4h, 0C4h, 09h      ; BOP 09h: translate AL (the host resumes elsewhere)
.swal:  mov al, 20h             ; swallowed: the BIOS's own EOI ...
        out 20h, al
        pop ax                  ; ... the interrupted code's AX ...
        iret                    ; ... and back

p5:     sti
        db 0C4h, 0C4h, 09h      ; BOP 09h: begin (CF=1: already printing -> return)
.t:     jc .d
        int 17h                 ; AH=00h AL=byte DX=0, set by the host
        db 0C4h, 0C4h, 09h      ; BOP 09h: judge AH, next byte (CF=1: done)
        jmp short .t
.d:     iret
