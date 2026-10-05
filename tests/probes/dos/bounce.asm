; bounce.asm -- GH #18 milestone-#6: a LEGIBLE real-CPU protected-mode graphics demo.
; A filled box bounces around a clean background, in mode 13h, on the real CPU in PM,
; rendering through our VDD. Unlike animate.asm's raw index ramp, this sets a real palette
; via protected-mode OUT (the DAC path proven by ioverify) so colours are intentional, and
; draws a recognizable moving shape so it reads unmistakably as "a program doing graphics".
;
; Per frame: clear to the background colour, draw the box at (posx,posy), advance + bounce,
; then INT 31h 0400 (cheap yield) so the host PM loop iterates and the UI presents. Loops
; until the window closes. Assemble: nasm -f bin.
bits 16
org 0x100

BOXW    equ 48
BOXH    equ 32

start:
    mov dx, msg_start
    mov ah, 0x09
    int 0x21

    mov ah, 0x4A                 ; shrink PSP
    mov bx, 0x1000
    int 0x21

    mov ax, 0x1687             ; detect DPMI
    int 0x2F
    test ax, ax
    jnz .nodpmi
    mov [entry], di
    mov [entry+2], es
    xor ax, ax                  ; far-call the mode-switch entry (16-bit)
    call far [entry]

    ; --- PM: mode 13h + framebuffer selector at 0xA0000 ------------------
    mov ax, 0x0013
    int 0x10
    mov ax, 0x0000
    mov cx, 1
    int 0x31
    jc .fail
    mov [fbsel], ax
    mov bx, ax
    mov ax, 0x0007
    mov cx, 0x000A
    xor dx, dx
    int 0x31
    mov bx, [fbsel]
    mov ax, 0x0008
    xor cx, cx
    mov dx, 0xFFFF
    int 0x31

    ; --- set the palette via PROTECTED-MODE OUT (DAC) --------------------
    ; index 0 = dark blue background, index 1 = bright yellow box.
    mov dx, 0x3C8
    xor al, al
    out dx, al                  ; DAC write index = 0
    mov dx, 0x3C9
    ; palette[0] = (0,0,25) dark blue
    xor al, al
    out dx, al                  ; R
    xor al, al
    out dx, al                  ; G
    mov al, 25
    out dx, al                  ; B  -> index auto-advances to 1
    ; palette[1] = (63,63,0) bright yellow
    mov al, 63
    out dx, al                  ; R
    mov al, 63
    out dx, al                  ; G
    xor al, al
    out dx, al                  ; B

    mov dx, msg_run
    mov ah, 0x09
    int 0x21

    mov es, [fbsel]
    cld

.frame:
    ; clear the screen to background (colour 0)
    xor di, di
    xor al, al
    mov cx, 64000
    rep stosb

    ; draw the box: start offset = posy*320 + posx, then W bytes/row for H rows
    mov ax, [posy]
    mov bx, 320
    mul bx                       ; ax = posy*320 (dx=0, fits)
    add ax, [posx]
    mov di, ax
    mov bp, BOXH
.row:
    push di
    mov cx, BOXW
    mov al, 1                    ; box colour = yellow
    rep stosb
    pop di
    add di, 320
    dec bp
    jnz .row

    ; advance + bounce X
    mov ax, [posx]
    add ax, [velx]
    cmp ax, 0
    jge .xlo
    neg word [velx]
    xor ax, ax
.xlo:
    cmp ax, 320 - BOXW
    jle .xhi
    neg word [velx]
    mov ax, 320 - BOXW
.xhi:
    mov [posx], ax
    ; advance + bounce Y
    mov ax, [posy]
    add ax, [vely]
    cmp ax, 0
    jge .ylo
    neg word [vely]
    xor ax, ax
.ylo:
    cmp ax, 200 - BOXH
    jle .yhi
    neg word [vely]
    mov ax, 200 - BOXH
.yhi:
    mov [posy], ax

    mov es, [fbsel]             ; keep ES on the framebuffer across the yield
    mov ax, 0x0400             ; INT 31h 0400: cheap handled yield
    int 0x31
    jmp .frame

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
posx:       dw 140
posy:       dw 84
velx:       dw 3
vely:       dw 2
msg_start:  db 'BOUNCE: real-mode start, detecting DPMI...', 13,10, '$'
msg_run:    db 'BOUNCE: PM mode 13h, palette via PM OUT -- bouncing box (real-CPU PM graphics).', 13,10, '$'
msg_fail:   db 'BOUNCE: descriptor alloc FAILED.', 13,10, '$'
msg_nodpmi: db 'BOUNCE: DPMI not present.', 13,10, '$'
