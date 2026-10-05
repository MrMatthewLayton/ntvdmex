; pmfault.asm -- the MINIMAL reproduction of the GH #18 wall.
;
; WHY THIS EXISTS. Doom is a 40-second run producing a 400 KB log, and the thing we
; actually need to iterate on is four instructions: switch to protected mode, execute
; ONE privileged instruction, and see whether the host gets told about it. Session 17
; proved (twice, with breakpoints) that a PM `STI` kills the VDM with no VEH exception
; and no fault-trampoline catch -- and that a PM `INT3` does exactly the same, so it is
; not specific to #GP. That is the whole blocker for running any real DOS extender,
; because DOS/4GW brackets every DPMI call with CLI/STI.
;
; This client reproduces it in about twenty seconds and prints a verdict either way.
;
;   AL=1 marker before the switch       -> we got to protected mode at all
;   the chosen privileged instruction   -> the experiment
;   AL=2 marker after it                -> IT WAS SURVIVED (trampoline caught + resumed)
;
; If the log shows "PM ok" and then nothing, the fault was swallowed and the VDM was
; terminated -- the wall. If it shows the "survived" line, the reflect works and the
; host emulated the instruction.
;
; ► WHICH INSTRUCTION IS SELECTED BY A BYTE IN THE PSP COMMAND TAIL, so one binary covers
;   the whole family without a rebuild-and-redeploy per idea:
;       (no argument)  STI   -- what DOS/4GW actually executes
;       "c"            CLI
;       "3"            INT3  -- a TRAP rather than a fault; distinguishes "no #GP
;                              reflect" from "no exception delivery at all"
;       "h"            HLT   -- another IOPL-sensitive op, for completeness
;       "i"            IN AL,0x21 -- port I/O, which is KNOWN to be reflected today
;                              (runs 70-79); the control case that proves the harness
;                              itself works and the guest really is in protected mode.
;   The control case matters: without it, "nothing happened" cannot distinguish a real
;   wall from a broken test.
;
; Assemble: nasm -f bin -o pmfault.com pmfault.asm

bits 16
org 0x100

start:
    mov     dx, msg_start
    mov     ah, 0x09
    int     0x21

    ; ---- pick the instruction under test from the command tail ----------------
    ; PSP:0x80 = tail length, PSP:0x81.. = the tail (leading space included).
    mov     si, 0x81
    mov     cx, [0x80]
    and     cx, 0xFF
    jcxz    .chosen
.scan:
    lodsb
    cmp     al, ' '
    je      .next
    cmp     al, 9
    jne     .got
.next:
    loop    .scan
    jmp     .chosen
.got:
    mov     [which], al
.chosen:

    ; ---- shrink to 64 KB so there is conventional memory left ------------------
    mov     ah, 0x4A
    mov     bx, 0x1000
    int     0x21

    ; ---- detect DPMI and get the mode-switch entry ----------------------------
    mov     ax, 0x1687
    int     0x2F
    test    ax, ax
    jnz     .nodpmi
    mov     [entry], di
    mov     [entry+2], es

    ; ---- switch to protected mode (AX bit0 = 0: 16-bit client) ----------------
    xor     ax, ax
    call    far [entry]
    jc      .noswitch                  ; the stub RETFs with CF=1 if the switch failed

    ; ================= WE ARE NOW IN PROTECTED MODE ============================
    ; Marker 1: prove we are here AND that ordinary DOS output still works from PM.
    mov     dx, msg_inpm
    mov     ah, 0x09
    int     0x21

    ; ---- the experiment -------------------------------------------------------
    mov     al, [which]
    cmp     al, 'c'
    je      .do_cli
    cmp     al, '3'
    je      .do_int3
    cmp     al, 'h'
    je      .do_hlt
    cmp     al, 'i'
    je      .do_in
    sti                                 ; default: the instruction DOS/4GW uses
    jmp     .survived
.do_cli:
    cli
    jmp     .survived
.do_int3:
    int3
    jmp     .survived
.do_hlt:
    hlt
    jmp     .survived
.do_in:
    in      al, 0x21                    ; CONTROL CASE: port I/O is reflected today
    jmp     .survived

.survived:
    ; Reaching here means the host was told about the instruction, emulated it, and
    ; resumed us. That is the whole acceptance test for GH #18.
    mov     dx, msg_survived
    mov     ah, 0x09
    int     0x21
    mov     ax, 0x4C00
    int     0x21

.noswitch:
    mov     dx, msg_noswitch
    mov     ah, 0x09
    int     0x21
    mov     ax, 0x4C01
    int     0x21

.nodpmi:
    mov     dx, msg_nodpmi
    mov     ah, 0x09
    int     0x21
    mov     ax, 0x4C02
    int     0x21

; The instruction is selected at ASSEMBLY time as well, because rt.bat launches a
; target with no arguments (it writes just the path into target.txt) and changing that
; needs a reboot. Five tiny binaries beat a reboot.
%ifndef SEL
  %define SEL 's'
%endif
which:          db SEL
entry:          dd 0

msg_start:      db 'PMFAULT: start', 13, 10, '$'
msg_inpm:       db 'PMFAULT: in protected mode, about to execute the test insn', 13, 10, '$'
msg_survived:   db 'PMFAULT: *** SURVIVED -- the host was told and resumed us ***', 13, 10, '$'
msg_noswitch:   db 'PMFAULT: mode switch FAILED (CF=1)', 13, 10, '$'
msg_nodpmi:     db 'PMFAULT: no DPMI host', 13, 10, '$'
