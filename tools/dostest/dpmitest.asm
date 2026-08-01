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

    ; --- 4. we are now in PROTECTED MODE (run 26) ------------------------
    ; TWO DPMI service calls to prove the INT 31h round-trip. Each INT 31h (CD 31) is
    ; reflected by the kernel (VdmStartExecution) and serviced by the host VEH, which
    ; returns values in the registers and resumes us in PM past the INT. Reaching the
    ; second call proves the first resumed cleanly.
    ; SENTINEL: prove whether ANY PM instruction executes. Write two marker words to
    ; DS:0x600 (linear 0x1600, DS base 0x1000). The host VEH dumps linear 0x1600 -- if it
    ; reads BEEF CAFE, PM code genuinely ran; if 0000, the guest never executed.
    mov word [0x600], 0xBEEF    ; sentinel: proves PM code runs (watchdog reads 0x1600)
    mov word [0x602], 0xCAFE
    mov ax, 0x0400             ; DPMI get-version
    int 0x31                   ; FAULT: does the kernel now reflect it to the host?
.pmspin:
    jmp .pmspin                ; EB FE (spin; watchdog stops it)

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
