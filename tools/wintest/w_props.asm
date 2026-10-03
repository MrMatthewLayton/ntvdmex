; w_props.asm -- EnumProps (USER.27) against stock (#296, s90).
;
; Three properties on one window -- two by string, one by atom -- then EnumProps:
; how many, in what ORDER (the data words in callback order), whether the atom
; one arrives as a null selector with the atom in the offset, the stop contract
; (callback returns 0 -> EnumProps returns 0), and the empty case (-1).

        org     0
%include "w16.inc"
W16_HEAD
        IMP     U, 41,  CREATEWINDOW
        IMP     U, 53,  DESTROYWINDOW
        IMP     U, 26,  SETPROP
        IMP     U, 24,  REMOVEPROP
        IMP     U, 27,  ENUMPROPS
        IMP     U, 430, LSTRCMP
        IMP     U, 268, GLOBALADDATOM
        IMP     U, 269, GLOBALDELETEATOM
W16_IAT

HW      equ     D_T0
HW2     equ     D_T0+2
CNT     equ     D_T0+4
STOPAT  equ     D_T0+6
ATOM    equ     D_T0+8
SEEN    equ     D_BUF                   ; data words, in callback order (8 max)
SEGS    equ     D_BUF+0x10              ; the name's selector, per call
NAME0   equ     D_BUF+0x20              ; first string name, copied

        jmp     cases

; BOOL FAR PASCAL cb(HWND, LPCSTR, HANDLE): [bp+6]=hData [bp+8]=off [bp+10]=seg [bp+12]=hwnd
cb:
        push    bp
        mov     bp, sp
        push    ds
        push    ss
        pop     ds
        push    bx
        push    si
        mov     bx, [CNT]
        cmp     bx, 8
        jae     .n
        shl     bx, 1
        mov     ax, [bp+6]
        mov     [SEEN+bx], ax
        mov     ax, [bp+10]
        mov     [SEGS+bx], ax
        or      ax, ax
        jz      .n
        push    es
        les     si, [bp+8]
        mov     ax, [es:si]
        mov     [NAME0+bx], ax          ; first two characters of each name
        pop     es
.n:     inc     word [CNT]
        mov     ax, 1
        mov     cx, [STOPAT]
        jcxz    .r
        cmp     [CNT], cx
        jb      .r
        xor     ax, ax
.r:     pop     si
        pop     bx
        pop     ds
        pop     bp
        retf    8

%macro CWS 0
        PUSHCS  s_static
        PUSHCS  s_static
        push    word 8000h
        push    word 0
        push    word 0
        push    word 0
        push    word 10
        push    word 10
        push    word 0
        push    word 0
        push    word [D_HINST]
        push    word 0
        push    word 0
        API     CREATEWINDOW
%endmacro

%macro ENUM 2                           ; hwnd, stopat
        mov     word [CNT], 0
        mov     word [NAME0], 0
        mov     word [NAME0+2], 0
        mov     word [NAME0+4], 0
        mov     word [STOPAT], %2
        push    word %1
        push    cs
        push    word cb
        mov     ax, 0BEEFh
        API     ENUMPROPS
%endmacro

cases:
        CWS
        mov     [HW], ax
        CWS
        mov     [HW2], ax
        PUSHCS  s_gamma
        API     GLOBALADDATOM
        mov     [ATOM], ax
        BOOLN
        OUT     "atom"

        push    word [HW]
        PUSHCS  s_alpha
        push    word 11h
        API     SETPROP
        push    word [HW]
        PUSHCS  s_beta
        push    word 22h
        API     SETPROP
        push    word [HW]
        push    word 0                  ; MAKEINTATOM: null selector
        push    word [ATOM]
        push    word 33h
        API     SETPROP
        BOOLN
        OUT     "setprop.atom"

        ENUM    [HW], 0
        OUT     "enum.ret"
        mov     ax, [CNT]
        OUT     "enum.count"
        mov     ax, [SEEN]
        OUT     "enum.data0"
        mov     ax, [SEEN+2]
        OUT     "enum.data1"
        mov     ax, [SEEN+4]
        OUT     "enum.data2"
        mov     ax, [SEGS]              ; 0 if the first one came as an atom
        BOOLN
        OUT     "enum.seg0.nonnull"
        mov     ax, [SEGS+2]
        BOOLN
        OUT     "enum.seg1.nonnull"
        mov     ax, [SEGS+4]
        BOOLN
        OUT     "enum.seg2.nonnull"
        mov     ax, [NAME0]
        OUT     "enum.name0.01"
        mov     ax, [NAME0+2]
        OUT     "enum.name1.01"
        mov     ax, [NAME0+4]
        OUT     "enum.name2.01"

        ENUM    [HW], 1
        OUT     "enum.stop.ret"
        mov     ax, [CNT]
        OUT     "enum.stop.count"

        ENUM    [HW2], 0
        OUT     "enum.empty.ret"
        mov     ax, [CNT]
        OUT     "enum.empty.count"

        push    word [HW]
        PUSHCS  s_beta
        API     REMOVEPROP
        OUT     "remove.beta"
        ENUM    [HW], 0
        mov     ax, [CNT]
        OUT     "enum.after.count"

        push    word [HW]
        PUSHCS  s_alpha
        API     REMOVEPROP
        push    word [HW]
        push    word 0
        push    word [ATOM]
        API     REMOVEPROP
        push    word [ATOM]
        API     GLOBALDELETEATOM
        push    word [HW2]
        API     DESTROYWINDOW
        push    word [HW]
        API     DESTROYWINDOW
        jmp     w16_fin

s_static: db 'STATIC', 0
s_alpha:  db 'Alpha', 0
s_beta:   db 'Beta', 0
s_gamma:  db 'NtvdmexGamma', 0

W16_TAIL "w16props"
