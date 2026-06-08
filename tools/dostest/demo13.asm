; demo13.asm -- animated mode-13h (320x200x256) demo for NTVDMEX.
; Sets mode 13h, loads a rainbow palette in ONE INT 10h call (AH=10/AL=12, so no
; slow 768-OUT loop), then continuously redraws a diagonal rainbow that SCROLLS
; (colour = x + y + scroll). Press any key to exit. Assemble: nasm -f bin.
bits 16
org 0x100
start:
    mov ax, 0x0013
    int 0x10
    call set_palette
    xor bp, bp                  ; frame counter
.frame:
    mov si, bp
    shr si, 5                   ; gentle scroll speed
    mov ax, 0xA000
    mov es, ax
    xor di, di
    xor dx, dx                  ; y = 0..199
.yl:
    xor cx, cx                  ; x = 0..319
.xl:
    mov ax, cx
    add ax, dx
    add ax, si                  ; colour = x + y + scroll
    mov [es:di], al
    inc di
    inc cx
    cmp cx, 320
    jb .xl
    inc dx
    cmp dx, 200
    jb .yl
    inc bp
    mov ah, 0x01                ; check for a keypress (non-blocking)
    int 0x16
    jnz .done                   ; ZF=0 -> a key is waiting -> exit
    jmp .frame
.done:
    mov ax, 0x0003
    int 0x10
    mov ax, 0x4C00
    int 0x21

; build a 256-entry rainbow DAC table and set it in one call
set_palette:
    mov di, palbuf
    xor cx, cx
.pl:
    mov ax, cx
    and al, 0x3F
    mov [di], al                ; R = i & 3F
    mov ax, cx
    shr al, 1
    and al, 0x3F
    mov [di+1], al              ; G = (i>>1) & 3F
    mov bl, 0x3F
    mov ax, cx
    and al, 0x3F
    sub bl, al
    mov [di+2], bl              ; B = 3F - (i & 3F)
    add di, 3
    inc cx
    cmp cx, 256
    jb .pl
    push ds
    pop es                      ; ES = DS (table segment)
    mov dx, palbuf
    xor bx, bx                  ; first DAC reg = 0
    mov cx, 256                 ; count
    mov ax, 0x1012              ; set block of DAC registers
    int 0x10
    ret
palbuf: times 768 db 0
