; keylog.asm -- what does a keypress ACTUALLY deliver?
;
; Arrow keys work inside a Skyroads level but do nothing in its menus, and from the host
; side I cannot tell why: a level and a menu read the keyboard by different routes, and my
; probes cannot drive a menu. So this asks a human to press keys and records, for each one,
; what arrived on BOTH routes at once:
;
;   S:xx    a raw scancode, taken from port 0x60 by our own INT 09h handler -- the route a
;           game uses in-game. An extended key (every arrow) shows up as TWO of these: the
;           E0 prefix, then the code.
;   B:xxxx  a BIOS keycode from INT 16h (AH=01 peek, then AH=00 read) -- the route menus and
;           ordinary DOS programs use. Arrows come back as AH=scancode with AL=00.
;
; Read the two together and the answer falls out: if a key produces S: but no B:, the raw
; path works and the BIOS ring is not being fed; if it produces B: but no S:, the reverse;
; if neither, the key never reached the guest at all.
;
; Everything is printed through INT 21h, so it lands in the host log under "==> DOS OUTPUT"
; and can be read off the share without anyone transcribing anything.
;
; Runs for ~110 s (bounded so the harness always collects the log) or until ESC. The host's
; headless cap must be raised to match -- see headless_ms.txt on the share.
;
; Assemble: nasm -f bin keylog.asm -o keylog.com
bits 16
org 0x100

TIMEOUT_TICKS equ 18*110                ; ~110 s: long enough for a human to be told
                                        ; it is running and then try every key

start:
    cld
    mov dx, s_banner
    mov ah, 0x09
    int 0x21

    ; --- install our INT 09h handler (raw scancode route) ---
    xor ax, ax
    mov es, ax
    cli
    mov ax, [es:0x09*4]
    mov [old09], ax
    mov ax, [es:0x09*4+2]
    mov [old09+2], ax
    mov word [es:0x09*4], isr09
    mov [es:0x09*4+2], cs
    sti

    ; --- note the starting tick so we can bound the run ---
    mov ah, 0x00
    int 0x1A
    mov [t0], dx

.loop:
    ; ---- raw scancodes our ISR captured ----
.drain:
    mov bl, [rtail]
    cmp bl, [rhead]
    je .nokeyraw
    xor bh, bh
    mov al, [rbuf + bx]
    inc byte [rtail]
    push ax
    mov dx, s_sc
    mov ah, 0x09
    int 0x21
    pop ax
    call puthex8
    mov dx, s_spc
    mov ah, 0x09
    int 0x21
    jmp .drain
.nokeyraw:

    ; ---- BIOS keycodes (INT 16h) ----
    mov ah, 0x01                        ; peek: ZF=1 means nothing waiting
    int 0x16
    jz .nokeybios
    mov ah, 0x00                        ; consume it
    int 0x16
    push ax
    mov dx, s_bios
    mov ah, 0x09
    int 0x21
    pop ax
    push ax
    mov al, ah                          ; print AH (scancode) first
    call puthex8
    pop ax
    call puthex8                        ; then AL (ascii)
    mov dx, s_spc
    mov ah, 0x09
    int 0x21
    ; ESC (AL=1Bh) ends the test early
    cmp al, 0x1B
    je .done
.nokeybios:

    ; ---- bounded run: stop after TIMEOUT_TICKS BIOS ticks ----
    mov ah, 0x00
    int 0x1A                            ; a BOP, so the host gets a turn each pass
    sub dx, [t0]
    cmp dx, TIMEOUT_TICKS
    jb .loop

.done:
    ; --- restore INT 09h ---
    cli
    xor ax, ax
    mov es, ax
    mov ax, [old09]
    mov [es:0x09*4], ax
    mov ax, [old09+2]
    mov [es:0x09*4+2], ax
    sti

    mov dx, s_end
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21

; ---- INT 09h: take the scancode and stash it; no chaining, we EOI ourselves ----
isr09:
    push ax
    push bx
    in al, 0x60
    mov bl, [cs:rhead]
    xor bh, bh
    mov [cs:rbuf + bx], al
    inc byte [cs:rhead]
    mov al, 0x20                        ; non-specific EOI to the master PIC
    out 0x20, al
    pop bx
    pop ax
    iret

; ---- print AL as two hex digits ----
puthex8:
    push ax
    shr al, 4
    call .nib
    pop ax
    and al, 0x0F
    call .nib
    ret
.nib:
    add al, '0'
    cmp al, '9'
    jbe .o
    add al, 7
.o:
    mov dl, al
    mov ah, 0x02
    int 0x21
    ret

s_banner db 'keylog: press keys (arrows, Enter, space). ESC or ~110s ends it.',13,10
         db 'S:xx = raw scancode from port 60h   B:xxxx = INT 16h (AH=scan AL=ascii)',13,10,'$'
s_sc     db 'S:$'
s_bios   db 'B:$'
s_spc    db ' $'
s_end    db 13,10,'keylog: done.',13,10,'$'
t0       dw 0
old09    dd 0
rhead    db 0
rtail    db 0
rbuf     times 256 db 0
