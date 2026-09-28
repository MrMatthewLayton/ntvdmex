; p_ifst.com -- differential probe: are interrupts enabled when a program starts? (s82)
;
; Found by mybench.com: a program that never executes STI ran with the guest-visible
; IF clear from its first instruction, so the host's gate -- reading the guest's own
; flag -- refused every timer tick and 0040:006C never moved. On MS-DOS a program is
; entered with interrupts enabled. This asks, before the program does ANYTHING:
;   start.if   FLAGS & 0200h at the first instruction
;   int10.if   ...after INT 10h AH=0Fh (a BIOS call that returns through IRET)
;   int21.if   ...after INT 21h AH=30h
;
; nasm -f bin p_ifst.asm -o p_ifst.com
        org     100h
        pushf                           ; FIRST instruction: nothing may precede it
        pop     word [fstart]
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "ifstart"

        mov     ax, [fstart]
        and     ax, 0200h
        call    probe_capture
        EMIT    "start.if", "AX"

        mov     ah, 0Fh
        int     10h
        pushf
        pop     ax
        and     ax, 0200h
        call    probe_capture
        EMIT    "int10.if", "AX"

        mov     ax, 3000h
        int     21h
        pushf
        pop     ax
        and     ax, 0200h
        call    probe_capture
        EMIT    "int21.if", "AX"

        PROBE_END

fstart  dw 0
