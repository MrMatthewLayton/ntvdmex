; bdaprobe.asm -- does the BIOS keyboard buffer in guest memory actually fill?
;
; keyprobe hooks INT 09h itself, so it tests the route a GAME takes and, by doing so, stops
; the host's BIOS handler from ever running. This probe deliberately hooks NOTHING. It is
; the Skyroads menu's shape: poll the BIOS ring at 0040:001A/001C directly, and separately
; ask INT 16h, printing whenever either changes.
;
;   H=0020 T=0022 W=5000   <- the ring advanced and holds DOWN (AH=50 AL=00)
;   B16=5000               <- INT 16h returned the same key from the same buffer
;
; If the head/tail never move, the host is still not maintaining the buffer and no
; BDA-reading DOS program can see a keystroke, however well the interrupts are delivered.
;
; Runs ~20 s. Assemble: nasm -f bin bdaprobe.asm -o bdaprobe.com
bits 16
org 0x100

RUN_TICKS equ 364               ; ~20 s at 18.2 Hz

start:
    cld
    mov dx, s_banner
    mov ah, 0x09
    int 0x21

    mov ah, 0x00                ; run deadline
    int 0x1A
    mov [t0], dx

    mov ax, 0x40                ; snapshot the ring pointers as they start
    mov es, ax
    mov ax, [es:0x1A]
    mov [lasth], ax
    mov ax, [es:0x1C]
    mov [lastt], ax

.loop:
    mov ax, 0x40
    mov es, ax
    mov ax, [es:0x1A]
    cmp ax, [lasth]
    jne .changed
    mov ax, [es:0x1C]
    cmp ax, [lastt]
    je .no_change
.changed:
    mov ax, [es:0x1A]
    mov [lasth], ax
    mov ax, [es:0x1C]
    mov [lastt], ax
    mov dx, s_h                 ; "H="
    mov ah, 0x09
    int 0x21
    mov ax, [lasth]
    call puthex16
    mov dx, s_t                 ; " T="
    mov ah, 0x09
    int 0x21
    mov ax, [lastt]
    call puthex16
    mov dx, s_w                 ; " W=" -- the keycode sitting at the head
    mov ah, 0x09
    int 0x21
    mov ax, 0x40
    mov es, ax
    mov bx, [lasth]
    cmp bx, 0x1E
    jb .badptr
    cmp bx, 0x3E
    jae .badptr
    mov ax, [es:bx]
    jmp .showw
.badptr:
    xor ax, ax
.showw:
    call puthex16
    mov dx, s_crlf
    mov ah, 0x09
    int 0x21
.no_change:

    mov ah, 0x01                ; the BIOS route, same buffer
    int 0x16
    jz .no16
    mov ah, 0x00
    int 0x16
    push ax
    mov dx, s_b16
    mov ah, 0x09
    int 0x21
    pop ax
    call puthex16
    mov dx, s_crlf
    mov ah, 0x09
    int 0x21
.no16:

    mov ah, 0x00
    int 0x1A
    sub dx, [t0]
    cmp dx, RUN_TICKS
    jb .loop

    mov dx, s_done
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21

puthex16:
    push ax
    mov al, ah
    call puthex8
    pop ax
puthex8:
    push ax
    push dx
    push ax
    shr al, 4
    call putnib
    pop ax
    and al, 0x0F
    call putnib
    pop dx
    pop ax
    ret
putnib:
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .out
    add al, 7
.out:
    mov dl, al
    mov ah, 0x02
    int 0x21
    ret

t0       dw 0
lasth    dw 0
lastt    dw 0
s_banner db 'bdaprobe: watching the BIOS key ring at 0040:001A/001C for ~20s.',13,10
         db 'H/T = head/tail, W = keycode at head, B16 = what INT 16h returns.',13,10,'$'
s_h      db 'H=$'
s_t      db ' T=$'
s_w      db ' W=$'
s_b16    db 'B16=$'
s_crlf   db 13,10,'$'
s_done   db 'bdaprobe: done.',13,10,'$'
