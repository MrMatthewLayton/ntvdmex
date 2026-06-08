; demovesa.asm -- animated VESA VBE 2.0 demo for NTVDMEX.
; Sets mode 0x101 (640x480x256), rainbow palette in one INT 10h call, then
; continuously redraws a SMOOTH gradient that SCROLLS, filling the framebuffer
; through the 64KB banked window (4F05). Colour = (bank<<5) + (offset>>11) + scroll
; -- a function of the TRUE linear address, so it is continuous across banks.
; Press any key to exit. Assemble: nasm -f bin.
bits 16
org 0x100
start:
    mov ax, 0x4F02
    mov bx, 0x0101
    int 0x10
    call set_palette
    xor bp, bp                  ; frame counter
.frame:
    mov ax, bp
    shr ax, 4
    mov [scroll], al            ; scroll byte
    mov byte [bank], 0
.bankloop:
    mov ax, 0x4F05              ; set window A to [bank]
    xor bx, bx
    mov dl, [bank]
    xor dh, dh
    int 0x10
    mov ax, 0xA000
    mov es, ax
    xor di, di
    mov al, [bank]             ; base colour = (bank<<5) + scroll
    mov cl, 5
    shl al, cl
    add al, [scroll]
    mov bl, al                 ; bl = base
    xor cx, cx                 ; 65536 inner pixels
.fillb:
    mov ax, di
    mov al, ah
    shr al, 1
    shr al, 1
    shr al, 1                  ; al = offset>>11 (0..31)
    add al, bl                 ; colour = base + offset>>11
    mov [es:di], al
    inc di
    loop .fillb
    inc byte [bank]
    cmp byte [bank], 5
    jb .bankloop
    inc bp
    mov ah, 0x01
    int 0x16
    jnz .done
    jmp .frame
.done:
    mov ax, 0x0003
    int 0x10
    mov ax, 0x4C00
    int 0x21

set_palette:
    mov di, palbuf
    xor cx, cx
.pl:
    mov ax, cx
    and al, 0x3F
    mov [di], al
    mov ax, cx
    shr al, 1
    and al, 0x3F
    mov [di+1], al
    mov bl, 0x3F
    mov ax, cx
    and al, 0x3F
    sub bl, al
    mov [di+2], bl
    add di, 3
    inc cx
    cmp cx, 256
    jb .pl
    push ds
    pop es
    mov dx, palbuf
    xor bx, bx
    mov cx, 256
    mov ax, 0x1012
    int 0x10
    ret
bank:   db 0
scroll: db 0
palbuf: times 768 db 0
