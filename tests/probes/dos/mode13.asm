; mode13.asm -- GH #18 milestone-#6 slice: a REAL protected-mode VGA client rendering
; through our VDD on the real CPU. Flow (all after the real->PM switch):
;   1. INT 10h AX=0013  -> set mode 13h (320x200x256). In PM this reflects as a patched
;      BOP the host now routes to the video VDD (run 73 follow-on).
;   2. INT 31h 0000/0007/0008 -> allocate an LDT descriptor, base it at linear 0xA0000
;      (the mode-13h framebuffer), limit 0xFFFF (writable-data, access 0xF2 by default).
;   3. ES=that selector; fill 64000 bytes with a colour gradient via `stosb` -- direct
;      real-CPU protected-mode writes to 0xA0000 (no trap needed in linear mode 13h).
;   4. INT 21h AH=09 marker, then INT 21h 4Ch. The host keeps the Luna window open on the
;      final frame, so a QMP screendump shows the rendered gradient.
; If the window shows a gradient, a real PM graphics client rendered through the VDD on the
; real CPU -- the milestone payoff. Assemble: nasm -f bin.
bits 16
org 0x100
start:
    mov dx, msg_start
    mov ah, 0x09
    int 0x21

    mov ah, 0x4A                 ; shrink PSP to 64 KB
    mov bx, 0x1000
    int 0x21

    mov ax, 0x1687             ; detect DPMI
    int 0x2F
    test ax, ax
    jnz .nodpmi
    mov [entry], di
    mov [entry+2], es

    xor ax, ax                  ; far-call mode-switch entry (16-bit client)
    call far [entry]

    ; --- PROTECTED MODE: set video mode 13h via INT 10h -------------------
    mov ax, 0x0013
    int 0x10
    mov dx, msg_mode
    mov ah, 0x09
    int 0x21

    ; --- allocate a framebuffer descriptor -> ES = linear 0xA0000 ---------
    mov ax, 0x0000              ; DPMI allocate 1 descriptor
    mov cx, 1
    int 0x31
    jc .fail
    mov [fbsel], ax

    mov bx, ax                  ; set base = 0x000A0000 (CX:DX = hi:lo)
    mov ax, 0x0007
    mov cx, 0x000A
    xor dx, dx
    int 0x31

    mov bx, [fbsel]             ; set limit = 0x0000FFFF (CX:DX = hi:lo)
    mov ax, 0x0008
    xor cx, cx
    mov dx, 0xFFFF
    int 0x31

    ; --- fill 320x200 = 64000 bytes with a colour gradient ---------------
    mov es, [fbsel]             ; ES -> 0xA0000
    xor di, di
    mov cx, 64000
    xor al, al
.fill:
    stosb                       ; real-CPU PM write to 0xA0000+DI
    inc al                      ; ramp the colour index
    loop .fill

    mov dx, msg_drawn
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00             ; exit; window keeps the final frame
    int 0x21

.fail:
    mov dx, msg_fail
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21
.nodpmi:
    mov dx, msg_nodpmi
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21

entry:      dd 0
fbsel:      dw 0
msg_start:  db 'MODE13: real-mode start, detecting DPMI...', 13,10, '$'
msg_mode:   db 'MODE13: in PM, mode 13h set via INT 10h.', 13,10, '$'
msg_drawn:  db 'MODE13: framebuffer filled via PM selector -> A0000. RENDERED.', 13,10, '$'
msg_fail:   db 'MODE13: descriptor alloc FAILED (CF set).', 13,10, '$'
msg_nodpmi: db 'MODE13: DPMI not present.', 13,10, '$'
