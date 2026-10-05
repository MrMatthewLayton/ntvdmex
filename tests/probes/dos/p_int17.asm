; p_int17.com -- INT 17h's printer number (DX) and its unknown functions. GH #256.
;
; Our INT 17h never read DX: every printer number was LPT1. And an unknown AH
; answered 90h "ready", CF=0 -- success for a call that does nothing.
;
; ⚠ WHICH PRINTERS EXIST IS A FACT ABOUT THE MACHINE, so the probe dumps the BDA
;   port table (0040:0008..000F) first and asks about a printer number that has
;   NO port there on every host we run (LPT3, DX=2) -- the question is what the
;   BIOS answers for an absent printer, not how many a machine has. DX=1 (LPT2)
;   is asked too, but read it against the dump.
; ⚠ DX=3 indexes past the three LPT slots (0040:000E is the EBDA segment on an AT
;   with one). Asked, because a BIOS that does not range-check reads that word as
;   a port base; recorded as the oracles answer it.
; Nothing here prints to a port that is absent; LPT1 is only asked for status.
;
; ORACLE-ALSO: pcem
; nasm -f bin p_int17.asm -o p_int17.com

        org     100h
        jmp     start
%include "probe.inc"

%macro ASK17 3                                  ; AX, DX, case name
        mov     ax, %1
        mov     bx, 0B1B1h
        mov     cx, 0C1C1h
        mov     dx, %2
        int     17h
        call    probe_capture
        EMIT    %3, "AX,BX,CX,DX"
%endmacro

start:
        PROBE_BEGIN "int17"

        push    ds
        mov     ax, 40h
        mov     ds, ax
        mov     si, 8
        mov     di, bdalpt
        push    cs
        pop     es
        mov     cx, 8
        rep     movsb
        pop     ds
        EMIT_BUF "bda.lpt.ports", bdalpt, 8

        ASK17   02A5h, 0000h, "int17.02.lpt1"
        ASK17   02A5h, 0001h, "int17.02.lpt2"
        ASK17   02A5h, 0002h, "int17.02.lpt3"
        ASK17   01A5h, 0002h, "int17.01.lpt3"
        ASK17   0041h, 0002h, "int17.00.lpt3"
        ASK17   02A5h, 0003h, "int17.02.dx3"
        ASK17   03A5h, 0000h, "int17.03.lpt1"
        ASK17   0FFA5h, 0000h, "int17.ff.lpt1"

        PROBE_END

bdalpt  times 8 db 0
