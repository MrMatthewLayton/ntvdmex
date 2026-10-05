; w_wcb.asm -- a 32-bit thunk DLL calling BACK into the VDM (s91, #309).
;
; W16THK.DLL (tests/probes/win16/thunk32/) is loaded through the generic thunks and
; asked to use WOW32.DLL's other direction:
;   * WOWCallback16(our cb1, 12345678h): cb1 answers DX:AX = p + 1 -> 1234:5679
;   * WOWCallback16Ex(our cb3) with three PASCAL WORDs 1,2,3: cb3 answers
;     a*100 + b*10 + c -> 007B only if the argument order is right
;   * WOWGlobal*16: a bit per documented answer, plus what Free16/UnlockFree16
;     said. STOCK: wglobal.bits 0E5F, wglobal.hi 0101 -- both FREE calls answer 1
;     (TRUE), not Win16 GlobalFree's 0 (see thunk32/w16thk.c for the encoding)
; ⚠ STAGE W16THK.DLL BESIDE THE EXE (demo\win16\w16wcb\) before running; the
;   path below is absolute, so stock and ours load the same file.

        org     0
%include "w16.inc"
W16_HEAD
        IMP     K, 513, LOADLIBRARYEX32W
        IMP     K, 515, GETPROCADDRESS32W
        IMP     K, 517, CALLPROC32W
W16_IAT

HLO     equ     D_T0
HHI     equ     D_T0+2
CBLO    equ     D_T0+4                  ; T_Cb
CBHI    equ     D_T0+6
CXLO    equ     D_T0+8                  ; T_CbEx
CXHI    equ     D_T0+10
GLLO    equ     D_T0+12                 ; T_Glob
GLHI    equ     D_T0+14

%macro PD 1
        push    word ((%1) >> 16)
        push    word ((%1) & 0FFFFh)
%endmacro
%macro PDV 2
        push    word [%1]
        push    word [%2]
%endmacro

        jmp     cases

; DWORD FAR PASCAL cb1(DWORD p) -> p + 1
cb1:    push    bp
        mov     bp, sp
        mov     ax, [bp+6]
        mov     dx, [bp+8]
        add     ax, 1
        adc     dx, 0
        pop     bp
        retf    4

; WORD FAR PASCAL cb3(WORD a, WORD b, WORD c) -> a*100 + b*10 + c
cb3:    push    bp
        mov     bp, sp
        mov     ax, [bp+10]             ; a
        mov     cx, 100
        mul     cx
        mov     bx, ax
        mov     ax, [bp+8]              ; b
        mov     cx, 10
        mul     cx
        add     ax, bx
        add     ax, [bp+6]              ; c
        xor     dx, dx
        pop     bp
        retf    6

cases:
        PUSHCS  s_dll
        PD      0
        PD      0
        API     LOADLIBRARYEX32W
        mov     [HLO], ax
        mov     [HHI], dx
        or      ax, dx
        BOOLN
        OUT     "llex.w16thk"
        mov     ax, [HLO]
        or      ax, [HHI]
        jnz     .have
        jmp     w16_fin
.have:
        PDV     HHI, HLO
        PUSHCS  s_cb
        API     GETPROCADDRESS32W
        mov     [CBLO], ax
        mov     [CBHI], dx
        PDV     HHI, HLO
        PUSHCS  s_cbex
        API     GETPROCADDRESS32W
        mov     [CXLO], ax
        mov     [CXHI], dx
        PDV     HHI, HLO
        PUSHCS  s_glob
        API     GETPROCADDRESS32W
        mov     [GLLO], ax
        mov     [GLHI], dx
        or      ax, dx
        or      ax, [CBLO]
        or      ax, [CXLO]
        BOOLN
        OUT     "gpa.all"

        ; T_Cb(vpfn = cb1, p = 12345678h) -- neither converted (mask 0)
        push    cs
        push    word cb1
        PD      12345678h
        PDV     CBHI, CBLO
        PD      0
        PD      2
        API     CALLPROC32W
        push    dx
        OUT     "wcb16.lo"
        pop     ax
        OUT     "wcb16.hi"

        ; T_CbEx(vpfn = cb3)
        push    cs
        push    word cb3
        PDV     CXHI, CXLO
        PD      0
        PD      1
        API     CALLPROC32W
        push    dx
        OUT     "wcb16ex.lo"
        pop     ax
        OUT     "wcb16ex.hi"

        ; T_Glob()
        PDV     GLHI, GLLO
        PD      0
        PD      0
        API     CALLPROC32W
        push    dx
        OUT     "wglobal.bits"
        pop     ax
        OUT     "wglobal.hi"
        jmp     w16_fin

s_dll:  db 'C:\Documents and Settings\All Users\Documents\ntvdmex\demo\win16\w16wcb\W16THK.DLL', 0
s_cb:   db 'T_Cb', 0
s_cbex: db 'T_CbEx', 0
s_glob: db 'T_Glob', 0

W16_TAIL "w16wcb"
