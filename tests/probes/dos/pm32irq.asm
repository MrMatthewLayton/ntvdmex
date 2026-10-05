; pm32irq.asm -- GH #18 run 83: async IRQ0 injection into a 32-bit-CS PM timer hook.
;
; The 32-bit sibling of tmrhook (run 78, 16-bit). It far-calls the mode switch with AX=1 so
; the whole client runs in a 32-bit CS (run 81), then HOOKS INT 08h in protected mode via
; INT 31h 0205 with a 32-bit handler offset. When the host injects the latched IRQ0 into that
; hook (dpmi_inject_pm_irq), it must build a **32-bit** IRET frame (dword EFLAGS/CS/EIP) because
; the handler runs in a 32-bit CS and IRETs with `iretd` -- exactly the path run 83 widened.
;
; PROOF: the red box marches horizontally driven ONLY by [hits], which is bumped ONLY by the
; hooked INT 08h ISR. No INT 1Ah polling, no keyboard. If the box moves, the host injected IRQ0
; into a 32-bit PM hook and the widened dword frame let the ISR run and cleanly `iretd` back --
; i.e. run 83's catcher-frame widening works on the real CPU.
;
; Assemble: nasm -f bin pm32irq.asm -o pm32irq.com
bits 16
org 0x100

BOXW    equ 24
BOXH    equ 24
DATASEL equ 0x17            ; the DPMI switch's flat DATA selector (aliases CS base)

start:
    mov dx, msg_start
    mov ah, 0x09
    int 0x21
    mov ah, 0x4A               ; shrink PSP to 64 KB
    mov bx, 0x1000
    int 0x21
    mov ax, 0x1687             ; detect DPMI
    int 0x2F
    test ax, ax
    jnz no_dpmi
    mov [entry], di
    mov [entry+2], es
    mov ax, 1                  ; 32-bit client
    call far [entry]

; ---------------------------- 32-bit PROTECTED MODE ----------------------------
bits 32
    mov ax, 0x0013             ; INT 10h mode 13h (routed to the video VDD in PM)
    int 0x10

    mov ax, 0x0000             ; alloc 1 LDT descriptor -> AX
    mov cx, 1
    int 0x31
    jc .fail
    mov [fbsel], ax
    mov bx, ax
    mov ax, 0x0007             ; set base = 0x000A0000
    mov cx, 0x000A
    xor dx, dx
    int 0x31
    mov bx, [fbsel]
    mov ax, 0x0008             ; set limit = 0xFFFF (64 KB covers 320x200)
    xor cx, cx
    mov dx, 0xFFFF
    int 0x31

    ; palette: 0 = dark blue background, 1 = bright red box
    mov dx, 0x3C8
    xor al, al
    out dx, al
    mov dx, 0x3C9
    xor al, al
    out dx, al                 ; p0 R
    xor al, al
    out dx, al                 ; p0 G
    mov al, 25
    out dx, al                 ; p0 B
    mov al, 63
    out dx, al                 ; p1 R (red)
    xor al, al
    out dx, al                 ; p1 G
    xor al, al
    out dx, al                 ; p1 B

    ; HOOK the PM INT 08h vector with a 32-bit handler: CX = our 32-bit code sel,
    ; EDX = 32-bit handler offset (INT 31h 0205). The host stores a full 32-bit offset
    ; for a 32-bit selector (run 83) and, on injection, builds a dword IRET frame.
    mov ax, 0x0205
    mov bl, 0x08
    mov cx, cs
    mov edx, timer_isr
    int 0x31

    mov dx, msg_run
    mov ah, 0x09
    int 0x21

    cld

.frame:
    ; posx = 20 + (hits mod 240) -- hits is bumped ONLY by the injected INT 08h ISR
    movzx eax, word [hits]
    xor edx, edx
    mov ebx, 240
    div ebx                    ; edx = hits mod 240 (32-bit div)
    add edx, 20
    mov [posx], dx

    ; clear the screen (color 0), then draw the box at (posx, 90) in color 1
    mov es, [fbsel]
    xor edi, edi
    xor al, al
    mov ecx, 64000
    rep stosb
    movzx edi, word [posx]
    add edi, 90*320            ; row 90
    mov ebp, BOXH
.row:
    push edi
    mov ecx, BOXW
    mov al, 1
    rep stosb
    pop edi
    add edi, 320
    dec ebp
    jnz .row

    mov ax, 0x0400             ; yield (get DPMI version) -> host loop injects the pending IRQ0
    int 0x31
    jmp .frame

.fail:
    mov dx, msg_fail
    mov ah, 0x09
    int 0x21
    mov eax, 0x4C00
    int 0x21

; --- 32-bit PM INT 08h handler: runs when the host injects IRQ0 into our hooked vector ---
; Entered with the interrupted client's DS (a valid near-data selector), so [hits] resolves
; directly. `iret` in a 32-bit segment assembles as `iretd` -> pops the dword frame run 83 built.
timer_isr:
    inc word [hits]
    iret

; ------------------------------- real-mode tail --------------------------------
bits 16
no_dpmi:
    mov dx, msg_nodpmi
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21

; --------------------------------- data ----------------------------------
entry:      dd 0
fbsel:      dw 0
posx:       dw 20
hits:       dw 0
msg_start:  db 'PM32IRQ: real-mode start, detecting DPMI (2Fh/1687)...', 13,10, '$'
msg_run:    db 'PM32IRQ: 32-bit PM mode 13h -- box marches via HOOKED INT 08h (async IRQ0, 32-bit frame).', 13,10, '$'
msg_fail:   db 'PM32IRQ: descriptor alloc FAILED.', 13,10, '$'
msg_nodpmi: db 'PM32IRQ: DPMI not present.', 13,10, '$'
