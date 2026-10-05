; pmal.asm -- MAKE THE GUEST'S PROGRESS A NUMBER THE FAULT LOG CAN READ.
;
; WHY THIS EXISTS. The `entry+0 or entry+3` rule is solid (~25 samples), but the
; STORY built on it -- "the VEH resumes mid-instruction, so instructions re-execute"
; -- rests on inferring how far the guest had actually got from AX values I could not
; fully account for. dpmitest entry 3 is the case in point: the VEH reports AX=0x0206
; and the next INT 31h sees 0x0208, which needs `add al,2` to have run once BEFORE
; the fault and once after. That is consistent with several different mechanisms and
; I could not separate them by staring at it.
;
; So stop inferring. Fill the PM entry with a run of `mov al,imm8` -- TWO bytes each,
; which is exactly the case that misbehaves -- with ascending values. AL then IS a
; program counter, and the VEH already prints the faulting CONTEXT's AX. One run says:
;
;   ctx AL  = how many instructions the guest REALLY executed
;   cx->Eip = where the kernel CLAIMS it is
;
; and the difference between them is the bug, measured instead of argued.
;
; THE MAP (E = pm_entry; every instruction here is 2 bytes, on purpose):
;   E+0   jc noswitch     AL unchanged from entry
;   E+2   mov al,0x01     AL=01  <- so AL=n means the guest reached E+2n
;   E+4   mov al,0x02     AL=02
;   ...
;   E+22  mov al,0x0B     AL=0B
;   E+24  int 21h (BOP)   ends the entry
;
; PREDICTIONS, registered before the run:
;   * a fault reported at E+0 with ctx AL unchanged  -> the kernel reports the entry
;     before executing anything, and the resume there is CORRECT.
;   * a fault reported at E+3 with AL=0x01           -> the guest is really at E+4;
;     the reported EIP is 1 byte SHORT of the true boundary, and resuming at E+3
;     re-runs `mov al,0x01` harmlessly here but would corrupt a real client.
;   * a fault reported at E+3 with AL unchanged      -> the guest is really at E+2 and
;     the reported EIP is 1 byte PAST it.
;   * AL greater than 1 at a +3 fault                -> the reported EIP is badly
;     stale and neither of the two stories above survives.
;
; Run with `pmkernel.flag`. Assemble: nasm -f bin -o pmal.com pmal.asm

bits 16
org 0x100

start:
    mov     dx, msg_start
    mov     ah, 0x09
    int     0x21

    mov     ah, 0x4A                ; shrink to 64 KB
    mov     bx, 0x1000
    int     0x21

    mov     ax, 0x1687              ; detect DPMI
    int     0x2F
    test    ax, ax
    jnz     nodpmi
    mov     [entry], di
    mov     [entry+2], es

    xor     ax, ax                  ; ...but AL must start at 0 to read as a counter
    call    far [entry]
; ---------------------------------------------------------------------------
; DO NOT "TIDY" THIS. Every instruction is deliberately 2 bytes and the values
; are deliberately ascending -- that is the measurement.
; ---------------------------------------------------------------------------
pm_entry:
    jc      noswitch                ; E+0
    mov     al, 0x01                ; E+2
    mov     al, 0x02                ; E+4
    mov     al, 0x03                ; E+6
    mov     al, 0x04                ; E+8
    mov     al, 0x05                ; E+10
    mov     al, 0x06                ; E+12
    mov     al, 0x07                ; E+14
    mov     al, 0x08                ; E+16
    mov     al, 0x09                ; E+18
    mov     al, 0x0A                ; E+20
    mov     al, 0x0B                ; E+22
    mov     ah, 0x09                ; E+24
    mov     dx, msg_inpm
    int     0x21

    mov     ax, 0x4C00
    int     0x21

noswitch:
    mov     dx, msg_noswitch
    mov     ah, 0x09
    int     0x21
    mov     ax, 0x4C01
    int     0x21
nodpmi:
    mov     dx, msg_nodpmi
    mov     ah, 0x09
    int     0x21
    mov     ax, 0x4C02
    int     0x21

entry:          dd 0

msg_start:      db 'PMAL: start', 13, 10, '$'
msg_inpm:       db 'PMAL: reached the print', 13, 10, '$'
msg_noswitch:   db 'PMAL: mode switch FAILED (CF=1)', 13, 10, '$'
msg_nodpmi:     db 'PMAL: no DPMI host', 13, 10, '$'
