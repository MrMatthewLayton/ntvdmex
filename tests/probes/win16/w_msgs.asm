; w_msgs.asm -- the #305 M9 messages as a Win16 procedure receives them, vs stock (s91).
;
; Two windows of our class; the procedure records, per message, how many arrived and
; the LAST wParam/lParam (through SS = DGROUP, as w_genum's callbacks do), and answers
; WM_GETMINMAXINFO by raising ptMinTrackSize to 400x300.
;   ShowWindow(A)          -> WM_SHOWWINDOW wParam=1
;   MoveWindow(A, ...)     -> WM_MOVE arrived
;   SetActiveWindow(B), (A) -> WM_ACTIVATE: wParam = state (1), lParam high = 0
;                             (not minimised), lParam low = the OTHER window
;   MoveWindow(A, 480x330) -> WM_SIZE has ARRIVED before MoveWindow returns (sent)
;   MoveWindow(A, 100x100) -> GetWindowRect: the minimum held (width >= 400)
; Coordinates are frame-dependent (classic vs Luna) and are compared as relations.

        org     0
%include "w16.inc"
W16_HEAD
        IMP     U, 41,  CREATEWINDOW
        IMP     U, 53,  DESTROYWINDOW
        IMP     U, 57,  REGISTERCLASS
        IMP     U, 107, DEFWINDOWPROC
        IMP     U, 42,  SHOWWINDOW
        IMP     U, 56,  MOVEWINDOW
        IMP     U, 59,  SETACTIVEWINDOW
        IMP     U, 32,  GETWINDOWRECT
W16_IAT

HA      equ     D_T0
HB      equ     D_T0+2
WC      equ     D_BUF                   ; WNDCLASS16
RC      equ     D_BUF+0x20              ; RECT16
; per-message records at D_BUF+0x40: count, wParam, lParam lo, lParam hi, hwnd (10 bytes)
R_SHOW  equ     D_BUF+0x40
R_MOVE  equ     D_BUF+0x50
R_ACT   equ     D_BUF+0x60
R_MMI   equ     D_BUF+0x70
R_SIZE  equ     D_BUF+0x80

        jmp     cases

%macro REC 2                            ; msg, record
        cmp     word [bp+0Ch], %1
        jne     %%n
        inc     word [%2]
        mov     ax, [bp+0Ah]
        mov     [%2+2], ax
        mov     ax, [bp+06h]
        mov     [%2+4], ax
        mov     ax, [bp+08h]
        mov     [%2+6], ax
        mov     ax, [bp+0Eh]
        mov     [%2+8], ax
%%n:
%endmacro

wndproc:
        push    bp
        mov     bp, sp
        push    ds
        push    ss
        pop     ds
        REC     0018h, R_SHOW
        REC     0003h, R_MOVE
        REC     0006h, R_ACT
        REC     0005h, R_SIZE
        cmp     word [bp+0Ch], 0024h    ; WM_GETMINMAXINFO
        jne     .def
        inc     word [R_MMI]
        push    es
        push    bx
        les     bx, [bp+06h]
        mov     word [es:bx+12], 400    ; ptMinTrackSize (4th POINT: +0Ch)
        mov     word [es:bx+14], 300
        pop     bx
        pop     es
        xor     ax, ax
        xor     dx, dx
        pop     ds
        pop     bp
        retf    10
.def:   pop     ds
        pop     bp
        jmp     far [cs:DEFWINDOWPROC]

%macro CW 1                             ; -> AX
        PUSHCS  s_cls
        PUSHCS  %1
        push    word 00CFh              ; WS_OVERLAPPEDWINDOW, hidden
        push    word 0
        push    word 40
        push    word 40
        push    word 500
        push    word 350
        push    word 0
        push    word 0
        push    word [D_HINST]
        push    word 0
        push    word 0
        API     CREATEWINDOW
%endmacro

cases:
        mov     word [WC+0], 0003h
        mov     word [WC+2], wndproc
        mov     word [WC+4], cs
        mov     word [WC+6], 0
        mov     word [WC+8], 0
        mov     ax, [D_HINST]
        mov     [WC+10], ax
        mov     word [WC+12], 0
        mov     word [WC+14], 0
        mov     word [WC+16], 6
        mov     word [WC+18], 0
        mov     word [WC+20], 0
        mov     word [WC+22], s_cls
        mov     word [WC+24], cs
        PUSHDS  WC
        API     REGISTERCLASS
        BOOLN
        OUT     "reg.class"
        CW      s_a
        mov     [HA], ax
        CW      s_b
        mov     [HB], ax
        or      ax, [HA]
        BOOLN
        OUT     "cw.both"

        ; ── WM_SHOWWINDOW
        mov     word [R_SHOW], 0
        push    word [HA]
        push    word 1                  ; SW_SHOWNORMAL
        API     SHOWWINDOW
        mov     ax, [R_SHOW]
        BOOLN
        OUT     "show.arrived"
        mov     ax, [R_SHOW+2]
        OUT     "show.wparam"

        ; ── WM_MOVE
        mov     word [R_MOVE], 0
        push    word [HA]
        push    word 60
        push    word 70
        push    word 520
        push    word 360
        push    word 1
        API     MOVEWINDOW
        mov     ax, [R_MOVE]
        BOOLN
        OUT     "move.arrived"
        mov     ax, [R_MOVE+4]          ; client x >= 60
        cmp     ax, 60
        mov     ax, 0
        jl      .m1
        inc     ax
.m1:    OUT     "move.x.ge.60"

        ; ── WM_ACTIVATE: B then A
        push    word [HB]
        push    word 5                  ; SW_SHOW
        API     SHOWWINDOW
        push    word [HB]
        API     SETACTIVEWINDOW
        mov     word [R_ACT], 0
        push    word [HA]
        API     SETACTIVEWINDOW
        mov     ax, [R_ACT]
        BOOLN
        OUT     "act.arrived"
        mov     ax, [R_ACT+2]
        OUT     "act.wparam.last"
        mov     ax, [R_ACT+6]
        OUT     "act.lparam.hi.last"
        mov     ax, [R_ACT+4]           ; the other window: B (A gaining) or A (B losing)
        cmp     ax, [HB]
        je      .a1
        cmp     ax, [HA]
        je      .a1
        xor     ax, ax
        jmp     .a2
.a1:    mov     ax, 1
.a2:    OUT     "act.lparam.lo.is.ours"

        ; ── WM_SIZE is SENT: it has arrived by the time MoveWindow returns
        mov     word [R_SIZE], 0
        push    word [HA]
        push    word 60
        push    word 70
        push    word 480
        push    word 330
        push    word 1
        API     MOVEWINDOW
        mov     ax, [R_SIZE]
        BOOLN
        OUT     "size.before.return"

        ; ── WM_GETMINMAXINFO: shrink A to 100x100; the minimum must hold
        mov     word [R_MMI], 0
        push    word [HA]
        push    word 60
        push    word 70
        push    word 100
        push    word 100
        push    word 1
        API     MOVEWINDOW
        mov     ax, [R_MMI]
        BOOLN
        OUT     "mmi.arrived"
        push    word [HA]
        PUSHDS  RC
        API     GETWINDOWRECT
        mov     ax, [RC+4]
        sub     ax, [RC+0]
        cmp     ax, 400
        mov     ax, 0
        jl      .w1
        inc     ax
.w1:    OUT     "mmi.width.ge.400"

        push    word [HB]
        API     DESTROYWINDOW
        push    word [HA]
        API     DESTROYWINDOW
        jmp     w16_fin

s_cls:  db 'W16Msgs', 0
s_a:    db 'MsgA', 0
s_b:    db 'MsgB', 0

W16_TAIL "w16msgs"
