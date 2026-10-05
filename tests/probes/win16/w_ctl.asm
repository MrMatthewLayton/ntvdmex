; w_ctl.asm -- control messages and focus, against stock (#301, #303, #304; s90).
;
; #301  BM_GETCHECK/SETCHECK/GETSTATE/SETSTATE to a real BUTTON (Win16 WM_USER+0..3)
; #303  WM_SETFOCUS / WM_KILLFOCUS carry the OTHER window's Win16 handle in wParam
; #304  M5 EM_ pointer/struct messages, M6 LB_/CB_ structure messages, M7 owner-draw
;       LB_GETTEXT without HASSTRINGS returning the item DATA into the caller's buffer
; Handles are compared to the ones the probe holds, never printed as values.

        org     0
%include "w16.inc"
W16_HEAD
        IMP     U, 41,  CREATEWINDOW
        IMP     U, 53,  DESTROYWINDOW
        IMP     U, 57,  REGISTERCLASS
        IMP     U, 107, DEFWINDOWPROC
        IMP     U, 111, SENDMESSAGE
        IMP     U, 22,  SETFOCUS
        IMP     U, 109, PEEKMESSAGE
        IMP     U, 114, DISPATCHMESSAGE
        IMP     U, 37,  SETWINDOWTEXT
        IMP     U, 36,  GETWINDOWTEXT
        IMP     U, 98,  ISDLGBUTTONCHECKED
        IMP     U, 430, LSTRCMP
W16_IAT

HPAR    equ     D_T0
HA      equ     D_T0+2
HB      equ     D_T0+4
HCHK    equ     D_T0+6
HEDIT   equ     D_T0+8
HLB     equ     D_T0+10
HCB     equ     D_T0+12
HOD     equ     D_T0+14
SFH     equ     D_T0+16                 ; WM_SETFOCUS: hwnd, wParam
SFW     equ     D_T0+18
KFH     equ     D_T0+20                 ; WM_KILLFOCUS: hwnd, wParam
KFW     equ     D_T0+22
BUF     equ     D_BUF                   ; 0x60..
WC      equ     D_BUF+0x100
MSG     equ     D_BUF+0x120             ; MSG16: 18 bytes

        jmp     cases

; LRESULT FAR PASCAL wndproc(HWND, UINT, WPARAM, LPARAM)
; [bp+6]=lParam [bp+10]=wParam [bp+12]=msg [bp+14]=hwnd
wndproc:
        push    bp
        mov     bp, sp
        push    ds
        push    ss
        pop     ds
        push    ax
        mov     ax, [bp+12]
        cmp     ax, 7                   ; WM_SETFOCUS
        jne     .k
        mov     ax, [bp+14]
        mov     [SFH], ax
        mov     ax, [bp+10]
        mov     [SFW], ax
        jmp     .d
.k:     cmp     ax, 8                   ; WM_KILLFOCUS
        jne     .d
        mov     ax, [bp+14]
        mov     [KFH], ax
        mov     ax, [bp+10]
        mov     [KFW], ax
.d:     pop     ax
        pop     ds
        pop     bp
        jmp     far [cs:DEFWINDOWPROC]

%macro CW 9                             ; class, stylehi, stylelo, x, y, w, h, parent, id
        PUSHCS  %1
        PUSHCS  s_empty
        push    word %2
        push    word %3
        push    word %4
        push    word %5
        push    word %6
        push    word %7
        push    word %8
        push    word %9
        push    word [D_HINST]
        push    word 0
        push    word 0
        API     CREATEWINDOW
%endmacro

%macro SMV 5                            ; hwnd, msg, wParam, lParam hi, lParam lo
        push    word %1
        push    word %2
        push    word %3
        push    word %4
        push    word %5
        API     SENDMESSAGE
%endmacro
%macro SMP 4                            ; hwnd, msg, wParam, DS:ptr
        push    word %1
        push    word %2
        push    word %3
        PUSHDS  %4
        API     SENDMESSAGE
%endmacro
%macro SMC 4                            ; hwnd, msg, wParam, CS:string
        push    word %1
        push    word %2
        push    word %3
        PUSHCS  %4
        API     SENDMESSAGE
%endmacro

%macro EQ 2                             ; AX := (AX == %1) ; OUT %2
        cmp     ax, %1
        mov     ax, 0
        jne     %%n
        inc     ax
%%n:    OUT     %2
%endmacro

pump:                                   ; dispatch whatever is queued, ~50 rounds
        mov     cx, 50
.l:     push    cx
        PUSHDS  MSG
        push    word 0
        push    word 0
        push    word 0
        push    word 1                  ; PM_REMOVE
        API     PEEKMESSAGE
        or      ax, ax
        jz      .n
        PUSHDS  MSG
        API     DISPATCHMESSAGE
.n:     pop     cx
        loop    .l
        ret

cases:
        mov     word [WC+0], 0
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

        ; ---- controls in a visible popup parent ----
        CW      s_cls, 9000h, 0, 0, 0, 300, 300, 0, 0
        mov     [HPAR], ax
        CW      s_button, 5001h, 0003h, 5, 5, 80, 20, [HPAR], 10       ; BS_AUTOCHECKBOX
        mov     [HCHK], ax
        CW      s_edit, 5080h, 0004h, 5, 30, 150, 60, [HPAR], 12       ; ES_MULTILINE|WS_BORDER
        mov     [HEDIT], ax
        CW      s_listbox, 5080h, 0008h, 5, 100, 150, 80, [HPAR], 13   ; LBS_MULTIPLESEL
        mov     [HLB], ax
        CW      s_combo, 5000h, 0002h, 160, 5, 100, 100, [HPAR], 14    ; CBS_DROPDOWN
        mov     [HCB], ax
        CW      s_listbox, 5080h, 0010h, 160, 120, 100, 80, [HPAR], 15 ; LBS_OWNERDRAWFIXED
        mov     [HOD], ax

        ; ---- #301 BM_ ----
        SMV      [HCHK], 0400h, 0, 0, 0                            ; BM_GETCHECK
        OUT     "bm.getcheck.0"
        SMV      [HCHK], 0401h, 1, 0, 0                            ; BM_SETCHECK
        SMV      [HCHK], 0400h, 0, 0, 0
        OUT     "bm.getcheck.1"
        push    word [HPAR]
        push    word 10
        API     ISDLGBUTTONCHECKED
        OUT     "bm.isdlgbuttonchecked"
        SMV      [HCHK], 0403h, 1, 0, 0                            ; BM_SETSTATE (highlight)
        SMV      [HCHK], 0402h, 0, 0, 0                            ; BM_GETSTATE
        and     ax, 0007h
        OUT     "bm.getstate"

        ; ---- #304 M5 EM_ ----
        push    word [HEDIT]
        PUSHCS  s_two
        API     SETWINDOWTEXT
        SMV      [HEDIT], 040Ah, 0, 0, 0                           ; EM_GETLINECOUNT
        OUT     "em.linecount"
        mov     word [BUF], 20                                  ; cap in the first WORD
        SMP      [HEDIT], 0414h, 1, BUF                 ; EM_GETLINE line 1
        OUT     "em.getline.len"
        mov     ax, [BUF]
        OUT     "em.getline.01"
        SMV      [HEDIT], 0401h, 0, 5, 0                ; EM_SETSEL 0..5
        SMC      [HEDIT], 0412h, 0, s_hello             ; EM_REPLACESEL
        push    word [HEDIT]
        PUSHDS  BUF
        push    word 64
        API     GETWINDOWTEXT
        OUT     "em.replacesel.len"
        mov     ax, [BUF]
        OUT     "em.replacesel.01"
        mov     word [BUF+8], 0
        mov     word [BUF+12], 0
        SMP      [HEDIT], 0402h, 0, BUF+8               ; EM_GETRECT
        mov     ax, [BUF+12]
        cmp     ax, 100
        mov     ax, 0
        jb      .r1
        inc     ax
.r1:    OUT     "em.getrect.right.wide"
        mov     word [BUF+16], 16
        SMP      [HEDIT], 041Bh, 1, BUF+16              ; EM_SETTABSTOPS
        OUT     "em.settabstops"
        SMV      [HEDIT], 0418h, 1, 0, 0                           ; EM_FMTLINES
        OUT     "em.fmtlines"
        SMV      [HEDIT], 0406h, 0, 0, 1                ; EM_LINESCROLL vert 1
        OUT     "em.linescroll"

        ; ---- #304 M6 LB_/CB_ structures ----
        SMC      [HLB], 0401h, 0, s_a                   ; LB_ADDSTRING
        SMC      [HLB], 0401h, 0, s_b
        SMC      [HLB], 0401h, 0, s_c
        SMV      [HLB], 0406h, 1, 0, 0                  ; LB_SETSEL TRUE, 0
        SMV      [HLB], 0406h, 1, 0, 2                  ; LB_SETSEL TRUE, 2
        mov     word [BUF], 0EEEEh
        mov     word [BUF+2], 0EEEEh
        SMP      [HLB], 0412h, 10, BUF                  ; LB_GETSELITEMS
        OUT     "lb.getselitems.n"
        mov     ax, [BUF]
        OUT     "lb.getselitems.0"
        mov     ax, [BUF+2]
        OUT     "lb.getselitems.1"
        mov     word [BUF+8], 0EEEEh
        SMP      [HLB], 0419h, 1, BUF+8                 ; LB_GETITEMRECT item 1
        OUT     "lb.getitemrect.ret"
        mov     ax, [BUF+10]                                    ; top of item 1
        OUT     "lb.getitemrect.top"
        SMV      [HLB], 0413h, 0, 0, 0                             ; LB_SETTABSTOPS (defaults)
        OUT     "lb.settabstops"
        SMV      [HLB], 0404h, 1, 0, 1                  ; LB_SELITEMRANGEEX 1..1
        OUT     "lb.selitemrangeex"
        SMV      [HLB], 0411h, 0, 0, 0                             ; LB_GETSELCOUNT
        OUT     "lb.getselcount"
        mov     word [BUF+16], 0
        mov     word [BUF+20], 0
        SMP      [HCB], 0412h, 0, BUF+16                ; CB_GETDROPPEDCONTROLRECT
        mov     ax, [BUF+20]
        sub     ax, [BUF+16]
        OUT     "cb.droppedrect.width"

        ; ---- #304 M7 owner-draw GETTEXT without HASSTRINGS ----
        SMV      [HOD], 0401h, 0, 1234h, 5678h              ; LB_ADDSTRING data
        mov     word [BUF], 0EEEEh
        mov     word [BUF+2], 0EEEEh
        mov     word [BUF+4], 0EEEEh
        SMP      [HOD], 040Ah, 0, BUF                   ; LB_GETTEXT 0
        OUT     "od.gettext.ret"
        mov     ax, [BUF]
        OUT     "od.gettext.lo"
        mov     ax, [BUF+2]
        OUT     "od.gettext.hi"
        mov     ax, [BUF+4]
        OUT     "od.gettext.past"

        ; ---- #303 focus: two of our own windows, A then B ----
        CW      s_cls, 9000h, 0, 0, 0, 20, 20, 0, 0
        mov     [HA], ax
        CW      s_cls, 9000h, 0, 30, 0, 20, 20, 0, 0
        mov     [HB], ax
        push    word [HA]
        API     SETFOCUS
        call    pump
        mov     word [SFH], 0
        mov     word [SFW], 0
        mov     word [KFH], 0
        mov     word [KFW], 0
        push    word [HB]
        API     SETFOCUS
        call    pump
        mov     ax, [KFH]
        EQ      [HA], "focus.kill.hwnd.is.A"
        mov     ax, [KFW]
        EQ      [HB], "focus.kill.wparam.is.B"
        mov     ax, [SFH]
        EQ      [HB], "focus.set.hwnd.is.B"
        mov     ax, [SFW]
        EQ      [HA], "focus.set.wparam.is.A"

        push    word [HB]
        API     DESTROYWINDOW
        push    word [HA]
        API     DESTROYWINDOW
        push    word [HPAR]
        API     DESTROYWINDOW
        jmp     w16_fin

s_empty:   db 0
s_cls:     db 'W16Ctl', 0
s_button:  db 'BUTTON', 0
s_edit:    db 'EDIT', 0
s_listbox: db 'LISTBOX', 0
s_combo:   db 'COMBOBOX', 0
s_two:     db 'line1', 13, 10, 'line2', 0
s_hello:   db 'HELLO', 0
s_a:       db 'a', 0
s_b:       db 'b', 0
s_c:       db 'c', 0

W16_TAIL "w16ctl"
