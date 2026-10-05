; demo12.asm -- animated mode-12h (640x480x16 PLANAR) demo for NTVDMEX.
; Sets mode 12h, VGA write-mode 2 + Map Mask 0x0F, then continuously redraws 16
; horizontal colour bands that SCROLL (colour = band + scroll) -- each band a
; `rep stosb` straight to A0000, trapped through the host's planar write engine.
; Press any key to exit. Assemble: nasm -f bin.
bits 16
org 0x100
start:
    mov ax, 0x0012
    int 0x10
    mov dx, 0x3CE                ; GC index 5 = write mode 2
    mov al, 5
    out dx, al
    mov dx, 0x3CF
    mov al, 2
    out dx, al
    mov dx, 0x3C4               ; Sequencer index 2 = Map Mask 0x0F
    mov al, 2
    out dx, al
    mov dx, 0x3C5
    mov al, 0x0F
    out dx, al
    xor bp, bp                  ; frame counter
.frame:
    mov si, bp
    shr si, 3                   ; scroll speed
    mov ax, 0xA000
    mov es, ax
    xor di, di
    xor bx, bx                  ; band = 0..15 (bl)
.band:
    mov ax, bx
    add ax, si
    and al, 0x0F               ; colour = (band + scroll) & 0x0F
    mov cx, 2400               ; bytes per band (30 rows x 80)
    rep stosb
    inc bx
    cmp bx, 16
    jb .band
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
