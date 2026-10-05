; outprobe.asm -- GH #18 real-CPU PM I/O-virtualization probe (Kernel RE session 8).
;
; Purpose: RE session 8 showed 0x4f67f8 sits inside KiTrap0D's in-kernel VDM instruction
; EMULATOR (the bytes before it are `out dx, eax` -- the kernel emulating OUT for a VDM),
; and that pmfault's HLT was a MISLEADING probe (the kernel has no HLT case, so HLT always
; terminates). The instructions games actually use in PM are port I/O (VGA/sound) and
; CLI/STI -- which the kernel is meant to VIRTUALIZE, not terminate.
;
; This probe fires a real protected-mode `OUT DX, AL` to a VGA port (0x3C8, DAC write
; index -- a port our video VDD claims) and then keeps going. Three outcomes, all readable
; from vm/serial.log WITHOUT the KD observer (no breakpoint => no arm-then-idle reboot):
;   (A) serial stops at "about to OUT"      => the OUT #GP'd and the kernel TERMINATED the
;                                              VDM (I/O NOT virtualized for our PM VDM) --
;                                              same failure class as HLT.
;   (B) serial shows "OUT survived" + exit  => the kernel EMULATED/allowed the OUT and the
;                                              guest resumed. Then check the host log for a
;                                              port-0x3C8 access to see if it reached the VDD
;                                              => real-CPU PM I/O virtualization WORKS.
;
; Same real->PM path as pmfault.asm. Assemble: nasm -f bin.
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

    ; --- fire a RAW protected-mode OUT (VGA DAC write index, port 0x3C8) --
    ; At CPL3 with IOPL<3 this #GP's; the question is whether KiTrap0D EMULATES
    ; it (routing to our VDD) and resumes the guest, or terminates the VDM.
    mov dx, 0x3C8
    mov al, 0x00
    out dx, al                  ; <-- the instruction under test

    ; a second write (DAC data, port 0x3C9) to make a VDD hit unmistakable
    mov dx, 0x3C9
    mov al, 0x2A
    out dx, al

    ; --- if we get here, the OUTs did NOT terminate the VDM ----------------
    mov dx, msg_survived
    mov ah, 0x09
    int 0x21

    mov dx, msg_done
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

entry:        dd 0
msg_start:    db 'OUTPROBE: real-mode start, detecting DPMI (2Fh/1687)...', 13,10, '$'
msg_switch:   db 'OUTPROBE: DPMI present -> far-calling mode-switch entry...', 13,10, '$'
msg_inpm:     db 'OUTPROBE: in PROTECTED MODE -- about to OUT 0x3C8/0x3C9 (VGA DAC)...', 13,10, '$'
msg_survived: db 'OUTPROBE: OUT survived -- guest RESUMED (kernel emulated the I/O!).', 13,10, '$'
msg_done:     db 'OUTPROBE: done, exiting cleanly.', 13,10, '$'
msg_nodpmi:   db 'OUTPROBE: DPMI not present.', 13,10, '$'
