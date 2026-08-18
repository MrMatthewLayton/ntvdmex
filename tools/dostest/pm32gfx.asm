; pm32gfx.asm -- GH #18 #3: a 32-bit real-CPU PM RENDERER (the DOS/4GW shape).
;
; Builds on run 81 (the mode switch produces a working 32-bit CS): this client far-calls the
; switch with AX=1, then -- entirely in 32-bit PM -- sets mode 13h, allocates an A0000
; framebuffer selector, loads a grayscale DAC palette via PM OUTs, and fills the screen with a
; vertical gradient using **rep stosd** (a native 32-bit string store: 32-bit ES:EDI, 80 dwords
; per row). If the Luna window shows a smooth black->gray vertical ramp, a 32-bit-segment
; renderer drove our video VDD on the real CPU -- graphics parity with the 16-bit run-74 path.
;
; Assemble: nasm -f bin pm32gfx.asm -o pm32gfx.com
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
    jz .have
    mov dx, msg_nodpmi
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21
.have:
    mov [entry], di
    mov [entry+2], es
    mov dx, msg_switch
    mov ah, 0x09
    int 0x21
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

    ; grayscale DAC palette: entry i -> (i>>2, i>>2, i>>2), 6-bit DAC (0..63)
    mov dx, 0x3C8
    xor al, al
    out dx, al                 ; DAC write index = 0
    mov dx, 0x3C9
    xor ecx, ecx
.pal:
    mov al, cl
    shr al, 2
    out dx, al                 ; R
    out dx, al                 ; G
    out dx, al                 ; B
    inc ecx
    cmp ecx, 256
    jb .pal

    ; vertical gradient: row r (0..199) filled with pixel value r, via rep stosd
    mov ax, [fbsel]
    mov es, ax
    xor edi, edi
    xor ebx, ebx               ; ebx = row
.row:
    movzx eax, bl              ; eax = r
    mov edx, eax
    shl eax, 8
    or  eax, edx
    mov edx, eax
    shl eax, 16
    or  eax, edx               ; eax = r replicated in all 4 bytes
    mov ecx, 80                ; 80 dwords = 320 pixels per row
    rep stosd                  ; 32-bit string store into ES:EDI (advances EDI by 320)
    inc ebx
    cmp ebx, 200
    jb .row

    ; done -- exit cleanly; the host keeps the window up (g_dpmi_done) showing the gradient
    mov eax, 0x4C00
    int 0x21
.fail:
    mov eax, 0x4C00
    int 0x21

; --------------------------------- data ----------------------------------
bits 16
entry:        dd 0
fbsel:        dw 0
msg_start:    db 'PM32GFX: real-mode start, detecting DPMI (2Fh/1687)...', 13,10, '$'
msg_switch:   db 'PM32GFX: far-calling the switch with AX=1, rendering a 32-bit mode-13h gradient...', 13,10, '$'
msg_nodpmi:   db 'PM32GFX: DPMI not present.', 13,10, '$'
