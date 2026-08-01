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

    ; --- 4. we are now in PROTECTED MODE (run 41) ------------------------
    ; Unmodified DPMI client using INT 31h 0300 (simulate real-mode interrupt): the way
    ; extenders route DOS/BIOS. Build a real-mode call structure (RMCS) for INT 21h AH=09
    ; and let the host run it in V86 with the RMCS registers, then resume us in PM.
    mov ax, 0x0400             ; get DPMI version
    int 0x31
    mov [0x600], ax
    mov word [.rmcs + 0x1C], 0x0900   ; RMCS.AX = AH=09 (print string)
    mov word [.rmcs + 0x24], 0x0100   ; RMCS.DS = 0x0100 (our real-mode segment)
    mov word [.rmcs + 0x14], .rmsg    ; RMCS.DX = message offset
    mov word [.rmcs + 0x20], 0x0202   ; RMCS.Flags
    push ds
    pop es                     ; ES = DS (the RMCS lives in our data)
    mov di, .rmcs              ; ES:DI -> RMCS
    mov ax, 0x0300             ; simulate real-mode interrupt
    mov bx, 0x0021             ; BL = INT 21h
    xor cx, cx                 ; no stack words to copy
    int 0x31                   ; -> host runs real-mode INT 21h AH=09 (prints .rmsg)
    mov dx, .pmsg              ; direct PM print to confirm we resumed cleanly
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00             ; terminate
    int 0x21
.pmspin:
    jmp .pmspin
.rmsg:  db '  [printed via INT 31h 0300 -> real-mode INT 21h AH=09]', 13, 10, '$'
.pmsg:  db 'DPMI: 0300 simulate-real-mode-int OK!', 13, 10, '$'
.rmcs:  times 50 db 0

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
