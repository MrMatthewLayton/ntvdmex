; pm32io.asm -- GH #18 #3 FIRST PROBE: does the kernel reflect a 32-BIT PM I/O as event 0?
;
; All real-CPU PM work to date (runs 72-78) is 16-bit. DOS/4GW-class clients run a 32-bit
; flat CODE segment (descriptor D/B=1). This probe is the smallest test of the pivotal open
; question: when a 32-bit (CS.D=1) protected-mode instruction does port I/O, does XP's
; KiTrap0D reflect it to our monitor as VTIB_EVENT=0 -- the SAME I/O event our host services
; for 16-bit PM (run 72/73) -- or does it terminate the VDM / reflect differently?
;
; Path: 16-bit DPMI entry -> mode switch -> alias CS (0x0F) into a fresh selector (000A) ->
; retype it CODE + D/B=1 (0009, CL=0xFA CH=0x40) -> far-jmp into 32-bit code -> a genuine
; 32-bit-segment OUT DX,AL to the VGA DAC (0x3C8 index, 0x3C9 data) -> far-jmp back to the
; 16-bit code selector -> report + exit.
;
; Read vm/serial.log (no KD needed):
;   (A) stops at "about to run 32-bit"   => the far-jmp into a D/B=1 CS, or its first faulting
;                                           insn, was NOT handled (VDM terminated / different
;                                           reflect) -- 32-bit PM I/O does NOT match the 16-bit gate.
;   (B) "32-bit OUT survived" + exit     => the kernel reflected the 32-bit OUT as event 0 and
;                                           our host_try_io_pm (D/B-aware, run 79) serviced it.
;                                           Then confirm a port-0x3C8 VDD hit in the host log
;                                           => real-CPU 32-bit PM I/O virtualizes like 16-bit.
;
; Assemble: nasm -f bin pm32io.asm -o pm32io.com
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
    test ax, ax
    jnz l_nodpmi
    mov [entry], di
    mov [entry+2], es

    ; far-call the mode-switch entry (AX=0: 16-bit client)
    xor ax, ax
    call far [entry]

    ; --- now in 16-bit PROTECTED MODE -----------------------------------
    ; Build a 32-bit CODE selector: alias CS (copies CS's linear base), then
    ; retype the alias to code exec/read + D/B=1 via INT 31h 0009.
    mov bx, cs                  ; source selector = our 16-bit code sel (0x0F)
    mov ax, 0x000A              ; create a descriptor aliased to BX -> AX
    int 0x31
    jc l_fail
    mov [fp32+2], ax            ; selector field of the 16:16 far pointer
    mov bx, ax
    mov ax, 0x0009              ; set access rights of BX
    mov cx, 0x40FA              ; CL=0xFA code x/r DPL3 present; CH=0x40 => D/B=1, G=0
    int 0x31

    mov dx, msg_go32
    mov ah, 0x09
    int 0x21

    ; far-jump into the 32-bit selector. A 16-bit far JMP loads a 16-bit offset and
    ; CS=csel; because csel.D=1 the CPU now executes in 32-bit mode.
    jmp far [fp32]

back16:
    ; --- back in 16-bit PROTECTED MODE (reached only if the 32-bit OUTs resumed) ---
    mov dx, msg_survived
    mov ah, 0x09
    int 0x21
    mov dx, msg_done
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21

l_fail:
    mov dx, msg_fail
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21
l_nodpmi:
    mov dx, msg_nodpmi
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21

; ------------------------- 32-bit code (CS.D=1) -------------------------
bits 32
pm32:
    ; genuine 32-bit-segment port I/O -- 'bits 32' emits the correct encodings
    ; (mov dx,imm16 gets a 0x66 operand-size prefix; out dx,al is 0xEE either way,
    ; but the fault now originates from a D/B=1 code segment).
    mov dx, 0x3C8
    xor al, al
    out dx, al                  ; <-- the 32-bit instruction under test (DAC write index)
    mov dx, 0x3C9
    mov al, 0x2A
    out dx, al                  ; DAC data -- a second, unmistakable VDD hit
    ; return to 16-bit: an m16:32 far pointer selects CS=0x0F (D=0)
    jmp far [fp16]

; ----------------------------- data ------------------------------------
bits 16
entry:        dd 0
fp32:         dw pm32            ; 16:16 far ptr: offset ...
              dw 0               ;               ... selector (patched to the 32-bit alias)
fp16:         dd back16          ; m16:32 far ptr: 32-bit offset ...
              dw 0x000F          ;                ... selector = 16-bit code sel (0x0F)
msg_start:    db 'PM32IO: real-mode start, detecting DPMI (2Fh/1687)...', 13,10, '$'
msg_go32:     db 'PM32IO: in 16-bit PM -- aliased CS to a D/B=1 code sel, about to run 32-bit...', 13,10, '$'
msg_survived: db 'PM32IO: 32-bit OUT survived -- guest RESUMED (kernel reflected 32-bit PM I/O!).', 13,10, '$'
msg_done:     db 'PM32IO: done, exiting cleanly.', 13,10, '$'
msg_fail:     db 'PM32IO: descriptor alias/alloc FAILED.', 13,10, '$'
msg_nodpmi:   db 'PM32IO: DPMI not present.', 13,10, '$'
