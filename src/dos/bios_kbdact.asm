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
