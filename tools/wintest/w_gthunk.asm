; w_gthunk.asm -- generic thunks: 16-bit code calling 32-bit KERNEL32 directly (#5, s90).
;
; LoadLibraryEx32W / GetProcAddress32W / CallProc32W / _CallProcEx32W /
; GetVDMPointer32W / FreeLibrary32W, against stock. The cases pin what the
; documentation leaves ambiguous:
;   * PARAMETER ORDER: MulDiv(10, 6, 3) = 20; read backwards it is 2 (or 5).
;   * THE MASK's BIT ORDER: lstrcpynA(dst, src, 4) with mask 6 and with mask 3 --
;     only the right one copies "hel" (the wrong one converts the count as a
;     pointer, which lstrcpynA survives and returns NULL for).
;   * a converted pointer really reaches 32-bit code: lstrlenA("hello") = 5.
; Handles and addresses are compared as BOOLs, never as values: they are
; per-process. Ordinals: krnl386 513-518.

        org     0
%include "w16.inc"
W16_HEAD
        IMP     K, 513, LOADLIBRARYEX32W
        IMP     K, 514, FREELIBRARY32W
        IMP     K, 515, GETPROCADDRESS32W
        IMP     K, 516, GETVDMPOINTER32W
        IMP     K, 517, CALLPROC32W
        IMP     K, 518, CALLPROCEX32W
W16_IAT

HLO     equ     D_T0                    ; kernel32's hInst32
HHI     equ     D_T0+2
MDLO    equ     D_T0+4                  ; MulDiv
MDHI    equ     D_T0+6
LLLO    equ     D_T0+8                  ; lstrlenA
LLHI    equ     D_T0+10
LCLO    equ     D_T0+12                 ; lstrcpynA
LCHI    equ     D_T0+14
FLLO    equ     D_T0+16                 ; a flat pointer
FLHI    equ     D_T0+18
DST1    equ     D_BUF                   ; 16 bytes each
DST2    equ     D_BUF+0x10
DST3    equ     D_BUF+0x20
DST4    equ     D_BUF+0x30
SBUF     equ     D_BUF+0x40

%macro PD 1                             ; push a DWORD immediate, high word first
        push    word ((%1) >> 16)
        push    word ((%1) & 0FFFFh)
%endmacro
%macro PDV 2                            ; push a DWORD held in [hi], [lo]
        push    word [%1]
        push    word [%2]
%endmacro
%macro EXPOP 1                          ; cdecl: the caller pops 12 + 4n
        add     sp, 12 + 4 * (%1)
%endmacro
%macro ZERO 1
        mov     word [%1], 0
        mov     word [%1+2], 0
        mov     word [%1+4], 0
%endmacro

cases:
        mov     word [SBUF], 'he'
        mov     word [SBUF+2], 'll'
        mov     word [SBUF+4], 'o'

        ; LoadLibraryEx32W
        PUSHCS  s_k32
        PD      0
        PD      0
        API     LOADLIBRARYEX32W
        mov     [HLO], ax
        mov     [HHI], dx
        or      ax, dx
        BOOLN
        OUT     "llex.k32"
        PUSHCS  s_nosuch
        PD      0
        PD      0
        API     LOADLIBRARYEX32W
        or      ax, dx
        BOOLN
        OUT     "llex.nosuch"

        ; GetProcAddress32W x3 + a missing name
        PDV     HHI, HLO
        PUSHCS  s_muldiv
        API     GETPROCADDRESS32W
        mov     [MDLO], ax
        mov     [MDHI], dx
        or      ax, dx
        BOOLN
        OUT     "gpa.muldiv"
        PDV     HHI, HLO
        PUSHCS  s_lstrlen
        API     GETPROCADDRESS32W
        mov     [LLLO], ax
        mov     [LLHI], dx
        PDV     HHI, HLO
        PUSHCS  s_lstrcpyn
        API     GETPROCADDRESS32W
        mov     [LCLO], ax
        mov     [LCHI], dx
        PDV     HHI, HLO
        PUSHCS  s_nofn
        API     GETPROCADDRESS32W
        or      ax, dx
        BOOLN
        OUT     "gpa.missing"

        ; CallProc32W (PASCAL): params, lpProc, mask, count
        PD      10
        PD      6
        PD      3
        PDV     MDHI, MDLO
        PD      0
        PD      3
        API     CALLPROC32W
        OUT     "cp.muldiv"                     ; 0014
        ; _CallProcEx32W (CDECL): count, mask, lpProc, params -- pushed right to left
        PD      3
        PD      6
        PD      10
        PDV     MDHI, MDLO
        PD      0
        PD      3
        API     CALLPROCEX32W
        EXPOP   3
        OUT     "cpex.muldiv"                   ; 0014

        ; one pointer, converted
        PUSHDS  SBUF
        PDV     LLHI, LLLO
        PD      1
        PD      1
        API     CALLPROC32W
        OUT     "cp.lstrlen"                    ; 0005
        PUSHDS  SBUF
        PDV     LLHI, LLLO
        PD      1
        PD      1
        API     CALLPROCEX32W
        EXPOP   1
        OUT     "cpex.lstrlen"                  ; 0005

        ; the mask's bit order: lstrcpynA(dst, src, 4)
        ZERO    DST1
        PUSHDS  DST1
        PUSHDS  SBUF
        PD      4
        PDV     LCHI, LCLO
        PD      6                               ; bit0 = LAST param
        PD      3
        API     CALLPROC32W
        mov     ax, [DST1]
        OUT     "cp.mask6.dst01"
        ZERO    DST2
        PUSHDS  DST2
        PUSHDS  SBUF
        PD      4
        PDV     LCHI, LCLO
        PD      3                               ; bit0 = FIRST param
        PD      3
        API     CALLPROC32W
        mov     ax, [DST2]
        OUT     "cp.mask3.dst01"
        ZERO    DST3
        PD      4
        PUSHDS  SBUF
        PUSHDS  DST3
        PDV     LCHI, LCLO
        PD      6
        PD      3
        API     CALLPROCEX32W
        EXPOP   3
        mov     ax, [DST3]
        OUT     "cpex.mask6.dst01"
        ZERO    DST4
        PD      4
        PUSHDS  SBUF
        PUSHDS  DST4
        PDV     LCHI, LCLO
        PD      3
        PD      3
        API     CALLPROCEX32W
        EXPOP   3
        mov     ax, [DST4]
        OUT     "cpex.mask3.dst01"

        ; GetVDMPointer32W, then lstrlenA on the flat pointer, mask 0
        PUSHDS  SBUF
        push    word 1                          ; protected mode
        API     GETVDMPOINTER32W
        mov     [FLLO], ax
        mov     [FLHI], dx
        or      ax, dx
        BOOLN
        OUT     "gvp.nonnull"
        PDV     FLHI, FLLO
        PDV     LLHI, LLLO
        PD      0
        PD      1
        API     CALLPROCEX32W
        EXPOP   1
        OUT     "gvp.lstrlen"                   ; 0005
        push    word 0
        push    word 0
        push    word 1
        API     GETVDMPOINTER32W
        or      ax, dx
        OUT     "gvp.null"

        ; FreeLibrary32W
        PDV     HHI, HLO
        API     FREELIBRARY32W
        BOOLN
        OUT     "free.k32"
        jmp     w16_fin

s_k32:      db 'KERNEL32.DLL', 0
s_nosuch:   db 'NOSUCH99.DLL', 0
s_muldiv:   db 'MulDiv', 0
s_lstrlen:  db 'lstrlenA', 0
s_lstrcpyn: db 'lstrcpynA', 0
s_nofn:     db 'NoSuchFunction99', 0

W16_TAIL "w16gthunk"
