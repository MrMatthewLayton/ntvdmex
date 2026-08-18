; pm32flat.asm -- GH #18 run 84: the DOS/4GW base-0 flat-selector model on the real CPU.
;
; Kernel RE session 7 found XP's LDT validator caps base+limit <= MmHighestUserAddress (~2GB),
; the SAME cap stock ntvdm runs under, so a base-0 ~2GB G=1 flat selector is REACHABLE (a true
; 4GB one is rejected). This client proves it: far-call the switch with AX=1 (32-bit CS), then
; -- entirely in 32-bit PM -- allocate ONE descriptor and configure it as a base-0, ~2GB,
; page-granular (G=1), 32-bit (D/B=1) writable-data selector (INT 31h 0000/0007/0008/0009).
; It then renders mode 13h by writing the VGA aperture through that flat selector at its LINEAR
; address (ES:[0x000A0000]) -- offset == linear, the DOS/4GW idiom -- via `rep stosd`.
;
; PROOF: if the Luna window shows the vertical gradient, a base-0 flat selector addressed linear
; 0xA0000 on the real CPU. The host serial/log line "DPMI-LDT: install REJECTED ..." (run 84
; observability) would appear if XP's validator refused the ~2GB descriptor -- its ABSENCE plus a
; visible gradient is the confirmation. (A follow-on can request a true 4GB limit to see the reject.)
;
; Assemble: nasm -f bin pm32flat.asm -o pm32flat.com
bits 16
org 0x100

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
    mov dx, msg_switch
    mov ah, 0x09
    int 0x21
    mov ax, 1                  ; 32-bit client
    call far [entry]

; ---------------------------- 32-bit PROTECTED MODE ----------------------------
bits 32
    mov ax, 0x0013             ; INT 10h mode 13h (routed to the video VDD in PM)
    int 0x10

    ; --- build a base-0, ~2GB, G=1, 32-bit writable-data FLAT selector ---
    mov ax, 0x0000             ; alloc 1 LDT descriptor -> AX
    mov cx, 1
    int 0x31
    jc .fail
    mov [flatsel], ax
    mov bx, ax
    mov ax, 0x0007             ; set base = 0x00000000
    xor cx, cx
    xor dx, dx
    int 0x31
    mov bx, [flatsel]
    mov ax, 0x0008             ; set limit = 0x7FEFFFFF (~2GB, just under the XP cap)
    mov cx, 0x7FEF
    mov dx, 0xFFFF
    int 0x31
    mov bx, [flatsel]
    mov ax, 0x0009             ; set access: CL=0xF2 (present, DPL3, data R/W),
    mov cx, 0xC0F2             ;             CH=0xC0 (G=1 | D/B=1) -> 32-bit page-granular
    int 0x31

    ; grayscale DAC palette: entry i -> (i>>2, i>>2, i>>2)
    mov dx, 0x3C8
    xor al, al
    out dx, al
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

    ; render through the FLAT selector at the aperture's LINEAR address: ES:[0x000A0000].
    ; row r (0..199) filled with pixel value r via rep stosd; EDI advances contiguously.
    mov ax, [flatsel]
    mov es, ax
    mov edi, 0x000A0000        ; base=0 -> offset == linear address of the VGA aperture
    xor ebx, ebx               ; ebx = row
.row:
    movzx eax, bl
    mov edx, eax
    shl eax, 8
    or  eax, edx
    mov edx, eax
    shl eax, 16
    or  eax, edx               ; eax = r replicated in all 4 bytes
    mov ecx, 80                ; 80 dwords = 320 pixels per row
    rep stosd
    inc ebx
    cmp ebx, 200
    jb .row

    mov eax, 0x4C00            ; done -- host keeps the window up showing the gradient
    int 0x21
.fail:
    mov dx, msg_fail
    mov ah, 0x09
    int 0x21
    mov eax, 0x4C00
    int 0x21

; ------------------------------- real-mode tail --------------------------------
bits 16
no_dpmi:
    mov dx, msg_nodpmi
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21

; --------------------------------- data ----------------------------------
entry:        dd 0
flatsel:      dw 0
msg_start:    db 'PM32FLAT: real-mode start, detecting DPMI (2Fh/1687)...', 13,10, '$'
msg_switch:   db 'PM32FLAT: switch AX=1, building a base-0 ~2GB flat selector, rendering via ES:[0A0000h]...', 13,10, '$'
msg_fail:     db 'PM32FLAT: flat descriptor alloc FAILED.', 13,10, '$'
msg_nodpmi:   db 'PM32FLAT: DPMI not present.', 13,10, '$'
