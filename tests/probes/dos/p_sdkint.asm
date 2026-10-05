; p_sdkint.com -- the SDK's claim_int sample (sdk/sample/intecho.c) from DOS.  #11/#315 (s91)
;
; Our ABI, so no other host can answer: the rows are graded against what the sample
; promises (below). Without intecho.dll loaded (cfg\vdd.txt) INT 61h is an empty
; vector and AX/BX come back as they went in -- which the first row distinguishes.
;   AH=00h  AX = 4E58h ('NX'), BX = the ABI version (1), CF clear
;   AH=01h  CX = sum of CX bytes at DS:SI ('A'+'B'+'C' = C6h), CF clear
;   AH=7Fh  CF set
; nasm -f bin p_sdkint.asm -o p_sdkint.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "sdkint"

        mov     ax, 0000h
        mov     bx, 0B1B1h
        clc
        int     61h
        sbb     cx, cx
        call    probe_capture
        EMIT    "int61.00.presence", "AX,BX,CX"

        mov     ah, 01h
        mov     si, abc
        mov     cx, 3
        clc
        int     61h
        sbb     ax, ax
        call    probe_capture
        EMIT    "int61.01.sum", "AX,CX"

        mov     ah, 7Fh
        clc
        int     61h
        sbb     ax, ax
        call    probe_capture
        EMIT    "int61.7f.cf", "AX"

        PROBE_END

abc     db      'ABC'
