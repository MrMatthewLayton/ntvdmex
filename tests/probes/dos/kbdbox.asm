; kbdbox.asm -- GH #18 milestone-#6: INTERACTIVE real-CPU protected-mode graphics.
; A box you DRIVE with the arrow keys, in mode 13h, on the real CPU in PM, rendering
; through our VDD and reading input through our keyboard VDD. Proves INT 16h works in PM.
;
; Per frame: poll the keyboard (INT 16h AH=01, non-blocking); if a key is ready read it
; (AH=00) and move the box on arrows / quit on ESC; then clear+draw+yield (INT 31h 0400).
; Arrow scancodes: Left=4B Right=4D Up=48 Down=50. Assemble: nasm -f bin.
bits 16
org 0x100

BOXW  equ 40
BOXH  equ 32
STEP  equ 8

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

    ; palette: 0 = dark blue bg, 1 = cyan box
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
    mov al, 63
    out dx, al                  ; p1 B (cyan)

    mov dx, msg_run
    mov ah, 0x09
    int 0x21

    mov es, [fbsel]
    cld

.frame:
    ; --- poll keyboard (non-blocking) ---
    mov ah, 0x01
    int 0x16
    jz .draw                    ; ZF=1 -> no key waiting
    mov ah, 0x00
    int 0x16                     ; AX = AH:scancode  AL:ascii
    cmp al, 0x1B
    je .exit                     ; ESC -> quit
    cmp ah, 0x4B
    jne .nl
    sub word [posx], STEP
.nl:
    cmp ah, 0x4D
    jne .nr
    add word [posx], STEP
.nr:
    cmp ah, 0x48
    jne .nu
    sub word [posy], STEP
.nu:
    cmp ah, 0x50
    jne .nd
    add word [posy], STEP
.nd:
    ; clamp posx -> [0, 320-BOXW], posy -> [0, 200-BOXH]
    mov ax, [posx]
    cmp ax, 0
    jge .cx1
    xor ax, ax
.cx1:
    cmp ax, 320 - BOXW
    jle .cx2
    mov ax, 320 - BOXW
.cx2:
    mov [posx], ax
    mov ax, [posy]
    cmp ax, 0
    jge .cy1
    xor ax, ax
.cy1:
    cmp ax, 200 - BOXH
    jle .cy2
    mov ax, 200 - BOXH
.cy2:
    mov [posy], ax

.draw:
    xor di, di
    xor al, al
    mov cx, 64000
    rep stosb
    mov ax, [posy]
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

.exit:
    mov ax, 0x0003             ; back to text mode
    int 0x10
    mov ax, 0x4C00
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
posx:       dw 140
posy:       dw 84
msg_start:  db 'KBDBOX: real-mode start, detecting DPMI...', 13,10, '$'
msg_run:    db 'KBDBOX: PM mode 13h -- drive the box with ARROW keys, ESC to quit (real-CPU PM input).', 13,10, '$'
msg_fail:   db 'KBDBOX: descriptor alloc FAILED.', 13,10, '$'
msg_nodpmi: db 'KBDBOX: DPMI not present.', 13,10, '$'
