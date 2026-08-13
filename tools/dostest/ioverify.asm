; ioverify.asm -- GH #18 run 73 follow-up: prove real-CPU PM port I/O reaches our VDD
; END-TO-END (not just "OUT survived"). Writes a known DAC palette entry via protected-mode
; OUT, then reads it back via protected-mode IN, and prints the read-back bytes. If they match
; what was written, our host_try_io_pm dispatched both directions through the video VDD and the
; VDD's palette state actually changed -- confirming the fix is real, and exercising the IN path.
;
; DAC round-trip math (vdd_video.c): dac_pack stores comp<<2, dac_in returns >>2, so a written
; 6-bit component (0..63) reads back unchanged. Write idx5 = R:0x0A G:0x14 B:0x1E, expect the
; same on read-back. Same real->PM path as outprobe/pmfault. Assemble: nasm -f bin.
bits 16
org 0x100
start:
    mov dx, msg_start
    mov ah, 0x09
    int 0x21

    mov ah, 0x4A                 ; shrink PSP to 64 KB
    mov bx, 0x1000
    int 0x21

    mov ax, 0x1687              ; detect DPMI
    int 0x2F
    test ax, ax
    jnz .nodpmi
    mov [entry], di
    mov [entry+2], es

    xor ax, ax                  ; far-call mode-switch entry (16-bit client)
    call far [entry]

    ; --- in PROTECTED MODE: write DAC palette entry index 5 = 0x0A/0x14/0x1E ---
    mov dx, 0x3C8
    mov al, 5
    out dx, al                  ; DAC write index = 5
    mov dx, 0x3C9
    mov al, 0x0A
    out dx, al                  ; R = 0x0A
    mov al, 0x14
    out dx, al                  ; G = 0x14
    mov al, 0x1E
    out dx, al                  ; B = 0x1E

    ; --- read it back via protected-mode IN ---
    mov dx, 0x3C7
    mov al, 5
    out dx, al                  ; DAC read index = 5
    mov dx, 0x3C9
    in  al, dx                  ; R'
    mov di, rbuf
    call hexbyte
    in  al, dx                  ; G'
    mov di, rbuf + 3
    call hexbyte
    in  al, dx                  ; B'
    mov di, rbuf + 6
    call hexbyte

    mov dx, msg_read
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

; AL -> two ASCII hex chars at DS:DI (DI, DI+1)
hexbyte:
    push ax
    mov ah, al
    shr al, 4
    call nyb
    mov [di], al
    mov al, ah
    call nyb
    mov [di + 1], al
    pop ax
    ret
nyb:
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .d
    add al, 7                   ; 'A'..'F'
.d: ret

entry:      dd 0
msg_start:  db 'IOVERIFY: PM DAC round-trip -- write idx5 = 0A 14 1E, reading back...', 13,10, '$'
msg_read:   db 'IOVERIFY: read back = '
rbuf:       db 'xx xx xx'
            db ' (PASS iff = 0A 14 1E)', 13,10, '$'
msg_nodpmi: db 'IOVERIFY: DPMI not present.', 13,10, '$'
