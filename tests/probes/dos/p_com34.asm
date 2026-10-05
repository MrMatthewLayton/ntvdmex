; p_com34.asm -- how many serial/parallel ports does the machine DECLARE? (#245, s90)
;
; INT 11h's equipment word (serial count in bits 9-11, parallel in 14-15) and the BDA
; port tables at 0040:0000 (COM1-4) and 0040:0008 (LPT1-3). Asked of stock NTVDM to
; decide whether this host should fit COM3/COM4: fitting them is a claim the guest can
; read, so it must match what XP's own VDM tells a DOS program, not what the device
; model happens to be able to do.

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "com34"
        POISON
        int     11h
        call    probe_capture
        EMIT    "int11.equip", "AX"
        push    ds
        mov     ax, 40h
        mov     ds, ax
        mov     ax, [0]
        mov     bx, [2]
        mov     cx, [4]
        mov     dx, [6]
        pop     ds
        call    probe_capture
        EMIT    "bda.com1234", "AX,BX,CX,DX"
        push    ds
        mov     ax, 40h
        mov     ds, ax
        mov     ax, [8]
        mov     bx, [0Ah]
        mov     cx, [0Ch]
        mov     dx, [0Eh]
        pop     ds
        call    probe_capture
        EMIT    "bda.lpt123", "AX,BX,CX"
        PROBE_END
