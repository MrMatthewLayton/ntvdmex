; pmfault.asm -- GH #18 real-CPU PM-fault reflect probe (run 59).
;
; Purpose: fire a RAW (non-BOP) protected-mode #GP and prove that NTVDMEX's new
; +0x638 PM-fault trampoline reflects it to the host instead of the kernel silently
; terminating the VDM (the wall from runs 20-34). This is the keystone experiment for
; running protected mode on the REAL CPU like ntvdm (mainline over the interpreter).
;
; Flow:
;   1. real-mode marker
;   2. shrink PSP (so we behave like the other clients)
;   3. INT 2Fh AX=1687h -> detect DPMI, get the mode-switch entry in ES:DI
;   4. FAR-CALL the entry (AX=0, 16-bit client) -> host switches VDM to PM
;   5. in PM: print a marker (INT 21h AH=09 -- a patched-INT BOP, serviced by the host)
;   6. execute HLT (0xF4) -- a PRIVILEGED instruction: at CPL3 it raises #GP(0), which
;      is NOT a C4 C4 BOP and is NOT one of the INT sites the switch scan pre-patched.
;      So it takes the kernel's non-BOP fault-reflect path (0x4f67f8 -> +0x634) that
;      used to terminate us; with the trampoline armed it must reflect to H:0x1000.
;
; The host breaks its PM loop the moment the trampoline fires (logging the saved fault
; CS:EIP), so we never resume past the HLT. Assemble: nasm -f bin.
bits 16
org 0x100
start:
    mov dx, msg_start
    mov ah, 0x09
    int 0x21

    ; shrink our PSP block to 64 KB (matches the other DPMI clients)
    mov ah, 0x4A
    mov bx, 0x1000
    int 0x21

    ; detect DPMI (INT 2Fh AX=1687h)
    mov ax, 0x1687
    int 0x2F
    test ax, ax                 ; AX=0 => DPMI present
    jnz .nodpmi
    mov [entry], di             ; save ES:DI mode-switch entry
    mov [entry+2], es

    mov dx, msg_switch
    mov ah, 0x09
    int 0x21

    ; far-call the mode-switch entry (AX=0: 16-bit client)
    xor ax, ax
    call far [entry]

    ; --- now in PROTECTED MODE -------------------------------------------
    mov dx, msg_inpm            ; INT 21h in PM is a patched BOP -> host prints it
    mov ah, 0x09
    int 0x21

    ; --- fire a RAW protected-mode #GP -----------------------------------
    ; HLT at CPL3 -> #GP(0). Not a BOP, not a patched INT. If the +0x638
    ; trampoline works, the host logs "PM-FAULT REFLECTED" with this CS:EIP.
    hlt

    ; we should never get here (the host stops the PM loop on the reflect)
    mov dx, msg_past
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
msg_start:  db 'PMFAULT: real-mode start, detecting DPMI (2Fh/1687)...', 13,10, '$'
msg_switch: db 'PMFAULT: DPMI present -> far-calling mode-switch entry...', 13,10, '$'
msg_inpm:   db 'PMFAULT: in PROTECTED MODE -- about to HLT (raw #GP)...', 13,10, '$'
msg_past:   db 'PMFAULT: resumed PAST the HLT (unexpected!).', 13,10, '$'
msg_nodpmi: db 'PMFAULT: DPMI not present.', 13,10, '$'
