; animate.asm -- GH #18 milestone-#6: ANIMATED real protected-mode VGA on the real CPU.
; mode13 proved a static PM render; this proves a *moving* one -- the game shape. After the
; real->PM switch it sets mode 13h, bases a selector at 0xA0000, then loops forever:
;   each frame -> fill the framebuffer with a colour ramp whose start index = a phase counter
;   that increments per frame (so the gradient SCROLLS), then INT 31h 0400 (a cheap, handled
;   DPMI call) to yield -- which lets the host PM loop iterate (advancing the watchdog heartbeat
;   so it does NOT kill us) and the UI thread present the new frame.
; Two QMP screendumps a few seconds apart show DIFFERENT frames => real-CPU PM animation through
; the VDD. The loop runs until the window is closed (host raises the PM step cap + the watchdog
; now only kills a *sustained* freeze, not a progressing loop). Assemble: nasm -f bin.
bits 16
org 0x100
start:
    mov dx, msg_start
    mov ah, 0x09
    int 0x21

    mov ah, 0x4A                 ; shrink PSP to 64 KB
    mov bx, 0x1000
    int 0x21

    mov ax, 0x1687             ; detect DPMI
    int 0x2F
    test ax, ax
    jnz .nodpmi
    mov [entry], di
    mov [entry+2], es

    xor ax, ax                  ; far-call mode-switch entry (16-bit client)
    call far [entry]

    ; --- PM: mode 13h + framebuffer selector -----------------------------
    mov ax, 0x0013
    int 0x10

    mov ax, 0x0000             ; allocate 1 descriptor
    mov cx, 1
    int 0x31
    jc .fail
    mov [fbsel], ax
    mov bx, ax                  ; base = 0x000A0000
    mov ax, 0x0007
    mov cx, 0x000A
    xor dx, dx
    int 0x31
    mov bx, [fbsel]            ; limit = 0x0000FFFF
    mov ax, 0x0008
    xor cx, cx
    mov dx, 0xFFFF
    int 0x31

    mov dx, msg_anim
    mov ah, 0x09
    int 0x21

    mov es, [fbsel]            ; ES -> 0xA0000
    mov byte [phase], 0

.frame:
    xor di, di
    mov cx, 64000
    mov al, [phase]           ; start colour = phase -> the ramp SCROLLS each frame
.fill:
    stosb
    inc al
    loop .fill
    inc byte [phase]          ; advance the scroll

    mov ax, 0x0400            ; INT 31h 0400 = get DPMI version: a cheap, handled yield
    int 0x31                  ; -> host loop iterates (watchdog sees progress) + UI presents

    jmp .frame               ; loop forever (until the window is closed)

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
phase:      db 0
msg_start:  db 'ANIMATE: real-mode start, detecting DPMI...', 13,10, '$'
msg_anim:   db 'ANIMATE: PM mode 13h -- scrolling the gradient forever (real-CPU PM animation).', 13,10, '$'
msg_fail:   db 'ANIMATE: descriptor alloc FAILED.', 13,10, '$'
msg_nodpmi: db 'ANIMATE: DPMI not present.', 13,10, '$'
