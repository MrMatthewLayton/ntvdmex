; w_upd.asm -- USER GetUpdateRect / ValidateRect after the relay's paint, against stock (#328).
;
; Our wowwin relay answers the OS's WM_PAINT itself (BeginPaint/EndPaint, to stop a
; paint storm) and keeps the rectangle for the guest, so the REAL window reports clean.
; Write's document window asks GetUpdateRect before it paints, and was told FALSE: an
; uncovered area kept another window's pixels. Cases: a visible window validated clean;
; a rectangle invalidated, the message queue looked at (PM_NOREMOVE, so the host pumps
; the OS's WM_PAINT), then GetUpdateRect; a ValidateRect of its left part; a
; ValidateRect of the rest (NULL). Per case the BOOL and the four sides.
; Ordinals read off guest/ne/user.exe's non-resident name table.

        org     0
%include "w16.inc"
W16_HEAD
        IMP     U, 41,  CREATEWINDOW
        IMP     U, 53,  DESTROYWINDOW
        IMP     U, 57,  REGISTERCLASS
        IMP     U, 107, DEFWINDOWPROC
        IMP     U, 109, PEEKMESSAGE
        IMP     U, 125, INVALIDATERECT
        IMP     U, 127, VALIDATERECT
        IMP     U, 190, GETUPDATERECT
W16_IAT

HWIN    equ     D_T0
RC      equ     D_BUF                   ; RECT16: left, top, right, bottom
INV     equ     D_BUF+0x08              ; the rectangle invalidated
VAL     equ     D_BUF+0x10              ; the part validated first
MSG     equ     D_BUF+0x20              ; MSG16, 18 bytes
WC      equ     D_BUF+0x40              ; WNDCLASS16, 26 bytes

        jmp     cases

wndproc:                                ; our class's procedure: DefWindowProc
        jmp     far [cs:DEFWINDOWPROC]

; GetUpdateRect(hwnd, RC, FALSE) with RC poisoned; out: <name>, .l .t .r .b
%macro GUR 1
        mov     word [RC+0], 0BEEFh
        mov     word [RC+2], 0BEEFh
        mov     word [RC+4], 0BEEFh
        mov     word [RC+6], 0BEEFh
        push    word [HWIN]
        PUSHDS  RC
        push    word 0
        mov     ax, 0BEEFh
        API     GETUPDATERECT
        BOOLN
        OUT     %1
%strcat %%l %1, ".l"
%strcat %%t %1, ".t"
%strcat %%r %1, ".r"
%strcat %%b %1, ".b"
        mov     ax, [RC+0]
        OUT     %%l
        mov     ax, [RC+2]
        OUT     %%t
        mov     ax, [RC+4]
        OUT     %%r
        mov     ax, [RC+6]
        OUT     %%b
%endmacro

; PeekMessage(MSG, NULL, 0, 0, PM_NOREMOVE): lets the host pump the real window
%macro PEEK 0
        PUSHDS  MSG
        push    word 0
        push    word 0
        push    word 0
        push    word 0
        API     PEEKMESSAGE
%endmacro

cases:
        mov     word [WC+0], 0003h      ; CS_VREDRAW|CS_HREDRAW
        mov     word [WC+2], wndproc
        mov     word [WC+4], cs
        mov     word [WC+6], 0
        mov     word [WC+8], 0
        mov     ax, [D_HINST]
        mov     [WC+10], ax
        mov     word [WC+12], 0
        mov     word [WC+14], 0
        mov     word [WC+16], 6         ; COLOR_WINDOW+1
        mov     word [WC+18], 0
        mov     word [WC+20], 0
        mov     word [WC+22], s_cls
        mov     word [WC+24], cs
        PUSHDS  WC
        API     REGISTERCLASS
        BOOLN
        OUT     "upd.reg"

        ; CreateWindow: a visible popup at screen (300,500), 200x100
        PUSHCS  s_cls
        PUSHCS  s_empty
        push    word 9000h              ; WS_POPUP|WS_VISIBLE
        push    word 0
        push    word 300
        push    word 500
        push    word 200
        push    word 100
        push    word 0
        push    word 0
        push    word [D_HINST]
        push    word 0
        push    word 0
        API     CREATEWINDOW
        mov     [HWIN], ax
        BOOLN
        OUT     "upd.cw"

        ; clean: validate everything, pump, ask
        push    word [HWIN]
        push    word 0
        push    word 0
        API     VALIDATERECT
        PEEK
        GUR     "upd.clean"

        ; InvalidateRect(hwnd, {10,10,50,40}, FALSE), pump, ask
        mov     word [INV+0], 10
        mov     word [INV+2], 10
        mov     word [INV+4], 50
        mov     word [INV+6], 40
        push    word [HWIN]
        PUSHDS  INV
        push    word 0
        API     INVALIDATERECT
        PEEK
        PEEK
        GUR     "upd.inval"

        ; ValidateRect(hwnd, {10,10,30,40}): the right part {30,10,50,40} is left
        mov     word [VAL+0], 10
        mov     word [VAL+2], 10
        mov     word [VAL+4], 30
        mov     word [VAL+6], 40
        push    word [HWIN]
        PUSHDS  VAL
        API     VALIDATERECT
        GUR     "upd.part"

        ; ValidateRect(hwnd, NULL): nothing is left
        push    word [HWIN]
        push    word 0
        push    word 0
        API     VALIDATERECT
        GUR     "upd.all"

        push    word [HWIN]
        API     DESTROYWINDOW
        jmp     w16_fin

s_empty:   db 0
s_cls:     db 'W16Upd', 0

W16_TAIL "w16upd"
