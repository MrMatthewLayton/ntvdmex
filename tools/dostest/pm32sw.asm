; pm32sw.asm -- GH #18 #3 step 1: does the DPMI mode SWITCH itself produce a working 32-bit CS?
;
; pm32io (run 80) entered 16-bit PM and MANUALLY built a D/B=1 code selector. This client is
; the real-extender shape: it far-calls the mode-switch entry with AX=1 (32-bit client), and if
; the run-81 host change is correct the switch hands back a D/B=1 CS/DS/SS so the code AFTER the
; far-call executes 32-bit natively -- no manual selector juggling. It then fires a 32-bit OUT
; (VGA DAC) and prints via INT 21h, all in 32-bit PM.
;
; Read the ntvdm console (and vm/serial.log for the host's "switching to PM (32-bit client)"):
;   stops after "far-calling 32-bit switch"  => the switch's 32-bit CS did NOT resume (VDM died).
;   "32-bit SWITCH ok" + "done"              => the mode switch produced a live 32-bit CS, a
;                                               32-bit OUT reflected as event 0 and was serviced,
;                                               and INT 21h works from 32-bit PM. #3 step 1 proven.
;
; Assemble: nasm -f bin pm32sw.asm -o pm32sw.com
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
    jnz nodpmi
    mov [entry], di
    mov [entry+2], es

    mov dx, msg_switch
    mov ah, 0x09
    int 0x21

    mov ax, 1                   ; AX bit0=1: 32-BIT client
    call far [entry]            ; 16-bit far-call; host returns us into a 32-bit CS

; --------------------- 32-bit PROTECTED MODE (from the switch) ---------------------
bits 32
    mov dx, 0x3C8              ; genuine 32-bit-segment port I/O
    xor al, al
    out dx, al
    mov dx, 0x3C9
    mov al, 0x2A
    out dx, al

    mov edx, msg_survived      ; INT 21h 09h from 32-bit PM (DS based, offset < 64 KB)
    mov ah, 0x09
    int 0x21
    mov edx, msg_done
    mov ah, 0x09
    int 0x21
    mov eax, 0x4C00
    int 0x21

; --------------------------------- 16-bit tail ----------------------------------
bits 16
nodpmi:
    mov dx, msg_nodpmi
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21

entry:        dd 0
msg_start:    db 'PM32SW: real-mode start, detecting DPMI (2Fh/1687)...', 13,10, '$'
msg_switch:   db 'PM32SW: far-calling the mode-switch entry with AX=1 (32-bit client)...', 13,10, '$'
msg_survived: db 'PM32SW: 32-bit SWITCH ok -- running 32-bit CS from the switch, OUT serviced!', 13,10, '$'
msg_done:     db 'PM32SW: done, exiting cleanly.', 13,10, '$'
msg_nodpmi:   db 'PM32SW: DPMI not present.', 13,10, '$'
