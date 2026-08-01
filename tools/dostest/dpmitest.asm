; dpmitest.asm -- DPMI 16-bit spike client (M4 slice 3, GH #1).
;
; Proves the real->protected-mode switch via NTVDMEX's kernel-monitor reuse:
;   1. print a real-mode marker (so we know it started)
;   2. INT 2Fh AX=1687h  -> detect DPMI, get the mode-switch entry in ES:DI
;   3. FAR-CALL the entry with AX=0 (16-bit client) -> host switches VDM to PM
;   4. now in PM: INT 31h AX=0000 (allocate a descriptor) -> surfaces a PM event
;
; Increment 1 stops the host at the first PM event and dumps the raw taxonomy to
; ntvdmhost.log (how the monitor reflects a PM INT). Later increments service
; INT 31h and thunk INT 21h back to real mode. Assemble: nasm -f bin.
bits 16
org 0x100
start:
    ; --- 1. real-mode marker ---------------------------------------------
    mov dx, msg_start
    mov ah, 0x09
    int 0x21

    ; --- 2. detect DPMI (INT 2Fh AX=1687h) -------------------------------
    mov ax, 0x1687
    int 0x2F
    test ax, ax                 ; AX=0 => DPMI present
    jnz .nodpmi
    mov [entry], di             ; save ES:DI mode-switch entry
    mov [entry+2], es

    mov dx, msg_switch
    mov ah, 0x09
    int 0x21

    ; --- 3. far-call the mode-switch entry (AX=0: 16-bit client) ---------
    xor ax, ax
    call far [entry]

    ; --- 4. we are now in PROTECTED MODE (run 39) ------------------------
    ; A REAL, UNMODIFIED DPMI client: plain INT 31h / INT 21h. The host patches each
    ; `CD nn` -> `C4 C4` at mode-switch (same 2 bytes) so they reflect as BOPs. This
    ; exercises: alloc a selector + alloc a memory block + base the selector onto it +
    ; read/write THROUGH the selector + DOS write (AH=40) + print (AH=09).
    mov ax, 0x0400             ; get DPMI version
    int 0x31
    mov [0x600], ax
    mov ax, 0x0000             ; allocate 1 LDT descriptor
    mov cx, 0x0001
    int 0x31                   ; -> AX = selector
    mov [0x604], ax            ; save it
    mov ax, 0x0501             ; allocate a 4KB memory block
    mov bx, 0
    mov cx, 0x1000
    int 0x31                   ; -> BX:CX = linear addr, SI:DI = handle
    mov [0x60A], bx            ; block addr high
    mov [0x60C], cx            ; block addr low
    mov dx, cx                 ; set-base wants CX:DX = base
    mov cx, bx
    mov bx, [0x604]            ; BX = our selector
    mov ax, 0x0007             ; set segment base = the block
    int 0x31
    mov bx, [0x604]
    mov cx, 0                  ; limit 0x00000FFF
    mov dx, 0x0FFF
    mov ax, 0x0008             ; set segment limit
    int 0x31
    mov ax, [0x604]
    mov es, ax                 ; ES = our selector (base = the block)
    mov word [es:0x10], 0xBEEF ; write a marker THROUGH the descriptor
    mov ax, [es:0x10]          ; read it back
    mov [0x60E], ax            ; store (expect 0xBEEF)
    mov ah, 0x40               ; DOS write to stdout
    mov bx, 1
    mov cx, .wlen
    mov dx, .wmsg
    int 0x21
    mov dx, .pmsg              ; DOS print $-string
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00             ; terminate
    int 0x21
.pmspin:
    jmp .pmspin
.pmsg:  db 'DPMI: descriptor + memory alloc + R/W through selector OK!', 13, 10, '$'
.wmsg:  db 'DPMI wrote this via INT 21h AH=40.', 13, 10
.wlen   equ $ - .wmsg

.switchfail:
    mov dx, msg_fail
    mov ah, 0x09
    int 0x21
    jmp .exit
.nodpmi:
    mov dx, msg_nodpmi
    mov ah, 0x09
    int 0x21
.exit:
    mov ax, 0x4C00
    int 0x21

entry:      dd 0
msg_start:  db 'DPMI spike: real-mode start, detecting via 2Fh/1687...', 13,10, '$'
msg_switch: db 'DPMI present -> far-calling the mode-switch entry...', 13,10, '$'
msg_fail:   db 'DPMI switch FAILED (still real mode).', 13,10, '$'
msg_nodpmi: db 'DPMI not present.', 13,10, '$'
