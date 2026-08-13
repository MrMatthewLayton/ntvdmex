; timerbox.asm -- GH #18 milestone-#6: TIMING in PM. A box that marches across the screen
; driven purely by the BIOS timer (INT 1Ah), with NO input, in mode 13h on the real CPU in PM.
; If the box moves between two screendumps with no keys pressed, the BIOS tick (0040:006C) is
; advancing in PM => polled timing works. Assemble: nasm -f bin.
bits 16
org 0x100

BOXW equ 24
BOXH equ 24

start:
    mov dx, msg_start
    mov ah, 0x09
    int 0x21
    mov ah, 0x4A
    mov bx, 0x1000
    int 0x21
    mov ax, 0x1687
    int 0x2F
    test ax, ax
    jnz .nodpmi
    mov [entry], di
    mov [entry+2], es
    xor ax, ax
    call far [entry]

    ; PM: mode 13h + framebuffer selector
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

    ; palette: 0 = dark blue bg, 1 = bright green box
    mov dx, 0x3C8
    xor al, al
    out dx, al
    mov dx, 0x3C9
    xor al, al
    out dx, al                  ; p0 R
    xor al, al
    out dx, al                  ; p0 G
    mov al, 25
    out dx, al                  ; p0 B
    xor al, al
    out dx, al                  ; p1 R
    mov al, 63
    out dx, al                  ; p1 G
    xor al, al
    out dx, al                  ; p1 B (green)

    mov dx, msg_run
    mov ah, 0x09
    int 0x21

    mov es, [fbsel]
    cld

.frame:
    ; read the BIOS tick (INT 1Ah AH=00 -> CX:DX = tick); use DX (low word)
    mov ah, 0x00
    int 0x1A
    ; posx = 20 + (tick_low mod 240)
    mov ax, dx
    xor dx, dx
    mov bx, 240
    div bx                      ; dx = ax mod 240
    add dx, 20
    mov [posx], dx

    ; clear + draw the box at (posx, 90)
    xor di, di
    xor al, al
    mov cx, 64000
    rep stosb
    mov ax, 90
    mov bx, 320
    mul bx
    add ax, [posx]
    mov di, ax
    mov bp, BOXH
.row:
    push di
    mov cx, BOXW
    mov al, 1
    rep stosb
    pop di
    add di, 320
    dec bp
    jnz .row

    mov es, [fbsel]
    mov ax, 0x0400
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
posx:       dw 20
msg_start:  db 'TIMERBOX: real-mode start, detecting DPMI...', 13,10, '$'
msg_run:    db 'TIMERBOX: PM mode 13h -- box marches via INT 1Ah tick, NO input (PM timing).', 13,10, '$'
msg_fail:   db 'TIMERBOX: descriptor alloc FAILED.', 13,10, '$'
msg_nodpmi: db 'TIMERBOX: DPMI not present.', 13,10, '$'
