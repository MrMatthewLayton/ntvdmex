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

    ; --- 4. we are now in PROTECTED MODE ---------------------------------
    ; EXPERIMENT (run 23): the switch now installs a VALID based LDT (svc10/11=0) and
    ; we enter PM via the monitor far-jmp (dpmi_enter_pm), which saves the host CONTEXT
    ; so the KERNEL can reflect a PM event back to the host loop. Software INT 31h (CD 31)
    ; goes through the kernel's INT-nn VDM path (KiVdmOpcodeINTnn: read IVT, vector) --
    ; a DIFFERENT path than a raw #GP (HLT), so it may reflect where HLT terminated.
    ; AX=0000 = DPMI "allocate LDT descriptor". Size-independent (decodes the same 16/32).
    int 0x31                   ; CD 31 -- the DPMI service call the host must catch.
.pmspin:
    jmp .pmspin                ; EB FE

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
