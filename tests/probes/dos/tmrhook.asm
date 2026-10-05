; tmrhook.asm -- GH #18 #2b: async IRQ0 INJECTION into a client-installed PM timer hook.
; Unlike timerbox (which POLLS INT 1Ah), this client HOOKS INT 08h in protected mode via
; INT 31h 0205 and its handler bumps a counter. The box marches purely off that counter --
; so if the box moves with NO input and NO INT 1Ah polling, the host is INJECTING IRQ0 into
; the client's own PM INT 08h vector ~18.2x/s (the mechanism timer-hooking games rely on).
; Assemble: nasm -f bin.
bits 16
org 0x100

BOXW equ 24
BOXH equ 24
DATASEL equ 0x17            ; the DPMI switch's flat DATA selector (aliases CS base)

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

    ; palette: 0 = dark blue bg, 1 = bright red box
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
    mov al, 63
    out dx, al                  ; p1 R (red)
    xor al, al
    out dx, al                  ; p1 G
    xor al, al
    out dx, al                  ; p1 B

    ; HOOK the PM INT 08h vector: BL=08, CX:DX = handler CS:offset (INT 31h 0205)
    mov ax, 0x0205
    mov bl, 0x08
    mov cx, cs
    mov dx, timer_isr
    int 0x31

    mov dx, msg_run
    mov ah, 0x09
    int 0x21

    cld

.frame:
    ; posx = 20 + (hits mod 240) -- hits is bumped ONLY by the injected INT 08h ISR
    mov ax, [hits]
    xor dx, dx
    mov bx, 240
    div bx                      ; dx = ax mod 240
    add dx, 20
    mov [posx], dx

    ; clear + draw the box at (posx, 90)
    mov es, [fbsel]
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

    mov ax, 0x0400              ; yield so the host can inject the pending IRQ0
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

; --- PM INT 08h handler: runs when the host injects IRQ0 into our hooked vector ---
; Entered with CS = our code selector; set DS to the flat data selector to reach [hits].
timer_isr:
    push ds
    push ax
    mov ax, DATASEL
    mov ds, ax
    inc word [hits]
    pop ax
    pop ds
    iret

entry:      dd 0
fbsel:      dw 0
posx:       dw 20
hits:       dw 0
msg_start:  db 'TMRHOOK: real-mode start, detecting DPMI...', 13,10, '$'
msg_run:    db 'TMRHOOK: PM mode 13h -- box marches via HOOKED INT 08h (async IRQ0 injection).', 13,10, '$'
msg_fail:   db 'TMRHOOK: descriptor alloc FAILED.', 13,10, '$'
msg_nodpmi: db 'TMRHOOK: DPMI not present.', 13,10, '$'
