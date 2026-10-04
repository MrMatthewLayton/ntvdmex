; w_font.asm -- WM_SETFONT / WM_GETFONT to a system control, against stock (s91, #305 M8).
;
; A fresh EDIT's WM_GETFONT (the system font: 0 on both?), then a stock font and a
; CreateFont font set with WM_SETFONT and read back -- the handle that comes back
; must be the one the program set (compared, never printed). WM_SETFONT(0) resets.

        org     0
%include "w16.inc"
W16_HEAD
        IMP     U, 41,  CREATEWINDOW
        IMP     U, 53,  DESTROYWINDOW
        IMP     U, 111, SENDMESSAGE
        IMP     G, 87,  GETSTOCKOBJECT
        IMP     G, 56,  CREATEFONT
        IMP     G, 69,  DELETEOBJECT
W16_IAT

HEDIT   equ     D_T0
HF1     equ     D_T0+2
HF2     equ     D_T0+4

WM_SETFONT      equ 0030h
WM_GETFONT      equ 0031h

%macro SETF 1
        push    word [HEDIT]
        push    word WM_SETFONT
        push    word %1
        push    word 0
        push    word 0
        API     SENDMESSAGE
%endmacro
%macro GETF 0
        push    word [HEDIT]
        push    word WM_GETFONT
        push    word 0
        push    word 0
        push    word 0
        API     SENDMESSAGE
%endmacro
%macro SAME 1                           ; AX := (AX == [%1])
        cmp     ax, [%1]
        mov     ax, 0
        jne     %%n
        inc     ax
%%n:
%endmacro

        jmp     cases
cases:
        PUSHCS  s_edit
        PUSHCS  s_empty
        push    word 0080h              ; WS_POPUP|WS_BORDER, hidden
        push    word 0080h
        push    word 10
        push    word 10
        push    word 200
        push    word 30
        push    word 0
        push    word 0
        push    word [D_HINST]
        push    word 0
        push    word 0
        API     CREATEWINDOW
        mov     [HEDIT], ax
        BOOLN
        OUT     "cw.edit"

        GETF
        BOOLN
        OUT     "getfont.fresh.nonzero"

        push    word 10                 ; ANSI_FIXED_FONT
        API     GETSTOCKOBJECT
        mov     [HF1], ax
        SETF    [HF1]
        GETF
        SAME    HF1
        OUT     "getfont.stock.same"

        push    word -16                ; height
        push    word 0
        push    word 0
        push    word 0
        push    word 700                ; bold
        push    word 0
        push    word 0
        push    word 0
        push    word 0
        push    word 0
        push    word 0
        push    word 0
        push    word 0
        PUSHCS  s_face
        API     CREATEFONT
        mov     [HF2], ax
        BOOLN
        OUT     "createfont"
        SETF    [HF2]
        GETF
        SAME    HF2
        OUT     "getfont.created.same"

        SETF    0
        GETF
        BOOLN
        OUT     "getfont.reset.nonzero"

        push    word [HEDIT]
        API     DESTROYWINDOW
        push    word [HF2]
        API     DELETEOBJECT
        BOOLN
        OUT     "deletefont"
        jmp     w16_fin

s_empty: db 0
s_edit:  db 'EDIT', 0
s_face:  db 'Arial', 0

W16_TAIL "w16font"
