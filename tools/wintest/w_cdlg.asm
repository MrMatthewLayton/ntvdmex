; w_cdlg.asm -- COMMDLG PrintDlg (ordinal 20), against stock (s91, #294).
;
; Only the forms that show NO dialog, so the run needs nobody at the keyboard:
;   * a wrong lStructSize -- refused, and CommDlgExtendedError says why;
;   * PD_RETURNDEFAULT (0400h) -- "give me the default printer's DEVMODE/DEVNAMES
;     without asking", with and without PD_RETURNIC (0800h).
; On a machine with no printer both hosts must say 0 and PDERR_NODEFAULTPRN (1008h);
; with one, 1 and real handles. Handles and the IC are reported as BOOLs: their
; values are each host's own.
;
; Win16 PRINTDLG, 0x34 bytes: +00 lStructSize(D) +04 hwndOwner +06 hDevMode
; +08 hDevNames +0A hDC +0C Flags(D) +10 nFromPage +12 nToPage +14 nMinPage
; +16 nMaxPage +18 nCopies +1A hInstance +1C lCustData(D) +20 lpfnPrintHook(D)
; +24 lpfnSetupHook(D) +28 lpPrintTemplateName(D) +2C lpSetupTemplateName(D)
; +30 hPrintTemplate +32 hSetupTemplate

        org     0
%include "w16.inc"
W16_HEAD 'COMMDLG'
        IMP     X, 20,  PRINTDLG
        IMP     X, 26,  COMMDLGEXTENDEDERROR
W16_IAT

PD      equ     D_BUF                   ; the PRINTDLG, 0x34 bytes

%macro POISON 0
        mov     ax, 0BEEFh
%endmacro

; fill the PRINTDLG: lStructSize = %1, Flags low word = %2, everything else 0,
; nCopies 1, the page range 1..1
%macro PDINIT 2
        push    ds
        pop     es
        mov     di, PD
        mov     cx, 1Ah
        xor     ax, ax
        cld
        rep     stosw
        mov     word [PD+00h], %1
        mov     word [PD+0Ch], %2
        mov     word [PD+10h], 1
        mov     word [PD+12h], 1
        mov     word [PD+14h], 1
        mov     word [PD+16h], 1
        mov     word [PD+18h], 1
%endmacro

        jmp     cases

cases:
        ; ── a wrong size: refused, CDERR_STRUCTSIZE ──
        PDINIT  0010h, 0400h
        PUSHDS  PD
        POISON
        API     PRINTDLG
        OUT     "pd.badsize.ret"
        POISON
        API     COMMDLGEXTENDEDERROR
        OUT     "pd.badsize.err"

        ; ── PD_RETURNDEFAULT: no dialog ──
        PDINIT  0034h, 0400h
        PUSHDS  PD
        POISON
        API     PRINTDLG
        OUT     "pd.default.ret"
        POISON
        API     COMMDLGEXTENDEDERROR
        OUT     "pd.default.err"
        mov     ax, [PD+06h]
        BOOLN
        OUT     "pd.default.hdevmode"
        mov     ax, [PD+08h]
        BOOLN
        OUT     "pd.default.hdevnames"
        mov     ax, [PD+0Ah]
        BOOLN
        OUT     "pd.default.hdc"

        ; ── PD_RETURNDEFAULT | PD_RETURNIC ──
        PDINIT  0034h, 0C00h
        PUSHDS  PD
        POISON
        API     PRINTDLG
        OUT     "pd.defic.ret"
        POISON
        API     COMMDLGEXTENDEDERROR
        OUT     "pd.defic.err"
        mov     ax, [PD+0Ah]
        BOOLN
        OUT     "pd.defic.hdc"
        mov     ax, [PD+18h]
        OUT     "pd.defic.ncopies"
        jmp     w16_fin

W16_TAIL "w16cdlg"
