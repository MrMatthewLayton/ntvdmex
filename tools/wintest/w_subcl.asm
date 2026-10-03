; w_subcl.asm -- SUBCLASSING: SetWindowLong(GWL_WNDPROC), against stock (s91, #308).
;
; 1. An EDIT control (a SYSTEM control): GetWindowLong(GWL_WNDPROC) must hand back
;    something callable; SetWindowLong installs our procedure and returns the same
;    value; our procedure then RECEIVES the control's WM_CHAR, swallows 'x', counts
;    every one, and chains the rest with CallWindowProc(old) -- so the text that
;    lands in the control is the proof. A direct far call to `old` must chain too.
;    Putting `old` back removes the subclass: 'x' then reaches the control.
; 2. WM_GETDLGCODE answered by the subclass (0099h), not the control.
; 3. A window of OUR class: SetWindowLong re-points its procedure; a private
;    message then gets the new procedure's answer (55AAh); restoring works.
; Values that are addresses are reported as comparisons (BOOLs), never raw.
; The counters live in DGROUP and the procedures reach it through SS (the task
; stack is in DGROUP), as w_genum's callbacks do.

        org     0
%include "w16.inc"
W16_HEAD
        IMP     U, 41,  CREATEWINDOW
        IMP     U, 53,  DESTROYWINDOW
        IMP     U, 111, SENDMESSAGE
        IMP     U, 57,  REGISTERCLASS
        IMP     U, 107, DEFWINDOWPROC
        IMP     U, 135, GETWINDOWLONG
        IMP     U, 136, SETWINDOWLONG
        IMP     U, 122, CALLWINDOWPROC
        IMP     U, 38,  GETWINDOWTEXTLENGTH
W16_IAT

HPAR    equ     D_T0
HEDIT   equ     D_T0+2
OLDLO   equ     D_T0+4                  ; the EDIT's procedure as GetWindowLong gave it
OLDHI   equ     D_T0+6
NCHAR   equ     D_T0+8                  ; WM_CHARs our subclass saw
PARLO   equ     D_T0+10                 ; our class's procedure, as GetWindowLong gave it
PARHI   equ     D_T0+12
WC      equ     D_BUF                   ; WNDCLASS16, 26 bytes

WM_CHAR         equ 0102h
WM_GETDLGCODE   equ 0087h
GWL_WNDPROC     equ -4

        jmp     cases

wndproc:                                ; our class: DefWindowProc
        jmp     far [cs:DEFWINDOWPROC]

; the replacement for our own class: a private message gets 55AAh, the rest default
wndproc2:
        push    bp
        mov     bp, sp
        cmp     word [bp+0Ch], 0405h    ; msg
        jne     .def
        mov     ax, 55AAh
        xor     dx, dx
        pop     bp
        retf    10
.def:   pop     bp
        jmp     far [cs:DEFWINDOWPROC]

; the EDIT subclass: LONG FAR PASCAL (hwnd, msg, wParam, lParam)
;   [bp+0Eh] hwnd  [bp+0Ch] msg  [bp+0Ah] wParam  [bp+08h]/[bp+06h] lParam
subproc:
        push    bp
        mov     bp, sp
        push    ds
        push    ss
        pop     ds
        mov     ax, [bp+0Ch]
        cmp     ax, WM_GETDLGCODE
        jne     .notdlg
        mov     ax, 0099h
        xor     dx, dx
        jmp     .ret
.notdlg:
        cmp     ax, WM_CHAR
        jne     .chain
        inc     word [NCHAR]
        cmp     word [bp+0Ah], 'x'
        jne     .chain
        xor     ax, ax                  ; swallowed
        xor     dx, dx
        jmp     .ret
.chain: push    word [OLDHI]
        push    word [OLDLO]
        push    word [bp+0Eh]
        push    word [bp+0Ch]
        push    word [bp+0Ah]
        push    word [bp+08h]
        push    word [bp+06h]
        API     CALLWINDOWPROC
.ret:   pop     ds
        pop     bp
        retf    10

; SendMessage(HEDIT, WM_CHAR, %1, 1)
%macro SENDCHAR 1
        push    word [HEDIT]
        push    word WM_CHAR
        push    word %1
        push    word 0
        push    word 1
        API     SENDMESSAGE
%endmacro

%macro TEXTLEN 0
        push    word [HEDIT]
        API     GETWINDOWTEXTLENGTH
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

        ; parent: WS_OVERLAPPEDWINDOW, hidden
        PUSHCS  s_cls
        PUSHCS  s_empty
        push    word 00CFh
        push    word 0
        push    word 10
        push    word 10
        push    word 300
        push    word 200
        push    word 0
        push    word 0
        push    word [D_HINST]
        push    word 0
        push    word 0
        API     CREATEWINDOW
        mov     [HPAR], ax
        BOOLN
        OUT     "cw.parent"

        ; EDIT child: WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL
        PUSHCS  s_edit
        PUSHCS  s_empty
        push    word 5080h
        push    word 0080h
        push    word 5
        push    word 5
        push    word 200
        push    word 24
        push    word [HPAR]
        push    word 101
        push    word [D_HINST]
        push    word 0
        push    word 0
        API     CREATEWINDOW
        mov     [HEDIT], ax
        BOOLN
        OUT     "cw.edit"

        ; ── 1. the EDIT's procedure ──
        push    word [HEDIT]
        push    word GWL_WNDPROC
        API     GETWINDOWLONG
        mov     [OLDLO], ax
        mov     [OLDHI], dx
        or      ax, dx
        BOOLN
        OUT     "edit.gwl.nonzero"

        push    word [HEDIT]
        push    word GWL_WNDPROC
        push    cs
        push    word subproc
        API     SETWINDOWLONG
        cmp     ax, [OLDLO]
        jne     .ne1
        cmp     dx, [OLDHI]
        jne     .ne1
        mov     ax, 1
        jmp     .o1
.ne1:   xor     ax, ax
.o1:    OUT     "edit.swl.returns.old"

        push    word [HEDIT]
        push    word GWL_WNDPROC
        API     GETWINDOWLONG
        cmp     ax, subproc
        jne     .ne2
        mov     cx, cs
        cmp     dx, cx
        jne     .ne2
        mov     ax, 1
        jmp     .o2
.ne2:   xor     ax, ax
.o2:    OUT     "edit.gwl.is.ours"

        mov     word [NCHAR], 0
        SENDCHAR 'a'
        SENDCHAR 'x'
        SENDCHAR 'b'
        mov     ax, [NCHAR]
        OUT     "edit.sub.nchar"
        TEXTLEN
        OUT     "edit.sub.textlen"

        push    word [HEDIT]
        push    word WM_GETDLGCODE
        push    word 0
        push    word 0
        push    word 0
        API     SENDMESSAGE
        OUT     "edit.sub.getdlgcode"

        ; a direct far call to the old procedure chains to the control
        push    word [HEDIT]
        push    word WM_CHAR
        push    word 'c'
        push    word 0
        push    word 1
        call    far [OLDLO]
        TEXTLEN
        OUT     "edit.direct.textlen"

        ; ── put the old one back: 'x' reaches the control now ──
        push    word [HEDIT]
        push    word GWL_WNDPROC
        push    word [OLDHI]
        push    word [OLDLO]
        API     SETWINDOWLONG
        cmp     ax, subproc
        jne     .ne3
        mov     ax, 1
        jmp     .o3
.ne3:   xor     ax, ax
.o3:    OUT     "edit.restore.returns.ours"
        mov     word [NCHAR], 0
        SENDCHAR 'x'
        mov     ax, [NCHAR]
        OUT     "edit.restored.nchar"
        TEXTLEN
        OUT     "edit.restored.textlen"

        ; ── 3. our own class ──
        push    word [HPAR]
        push    word GWL_WNDPROC
        API     GETWINDOWLONG
        mov     [PARLO], ax
        mov     [PARHI], dx
        cmp     ax, wndproc
        jne     .ne4
        mov     ax, 1
        jmp     .o4
.ne4:   xor     ax, ax
.o4:    OUT     "own.gwl.is.class.proc"

        push    word [HPAR]
        push    word GWL_WNDPROC
        push    cs
        push    word wndproc2
        API     SETWINDOWLONG
        cmp     ax, [PARLO]
        jne     .ne5
        mov     ax, 1
        jmp     .o5
.ne5:   xor     ax, ax
.o5:    OUT     "own.swl.returns.old"

        push    word [HPAR]
        push    word 0405h
        push    word 0
        push    word 0
        push    word 0
        API     SENDMESSAGE
        OUT     "own.sub.private"

        push    word [HPAR]
        push    word GWL_WNDPROC
        push    word [PARHI]
        push    word [PARLO]
        API     SETWINDOWLONG
        push    word [HPAR]
        push    word 0405h
        push    word 0
        push    word 0
        push    word 0
        API     SENDMESSAGE
        OUT     "own.restored.private"

        push    word [HPAR]
        API     DESTROYWINDOW
        jmp     w16_fin

s_empty:   db 0
s_cls:     db 'W16Subcl', 0
s_edit:    db 'EDIT', 0

W16_TAIL "w16subcl"
