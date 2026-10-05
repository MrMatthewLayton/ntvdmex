; w_mdi.asm -- the MDI client's messages, against stock (s91, #305 M13).
;
; A frame of our class, an MDICLIENT, two children made with WM_MDICREATE; then
; WM_MDIGETACTIVE (which child, and the MAXIMIZED flag Win16 returns in DX) after
; WM_MDIACTIVATE / WM_MDINEXT / WM_MDIMAXIMIZE / WM_MDIRESTORE, the arrangement
; messages' answers, and WM_MDIDESTROY. Handles compared, never printed.

        org     0
%include "w16.inc"
W16_HEAD
        IMP     U, 41,  CREATEWINDOW
        IMP     U, 53,  DESTROYWINDOW
        IMP     U, 111, SENDMESSAGE
        IMP     U, 57,  REGISTERCLASS
        IMP     U, 107, DEFWINDOWPROC
        IMP     U, 447, DEFMDICHILDPROC
        IMP     U, 47,  ISWINDOW
W16_IAT

HFR     equ     D_T0
HCL     equ     D_T0+2
HC1     equ     D_T0+4
HC2     equ     D_T0+6
WC      equ     D_BUF                   ; WNDCLASS16
CCS     equ     D_BUF+0x20              ; CLIENTCREATESTRUCT: hWindowMenu, idFirstChild
MCS     equ     D_BUF+0x30              ; MDICREATESTRUCT16, 26 bytes

WM_MDICREATE    equ 0220h
WM_MDIDESTROY   equ 0221h
WM_MDIACTIVATE  equ 0222h
WM_MDIRESTORE   equ 0223h
WM_MDINEXT      equ 0224h
WM_MDIMAXIMIZE  equ 0225h
WM_MDITILE      equ 0226h
WM_MDICASCADE   equ 0227h
WM_MDIICONARRANGE equ 0228h
WM_MDIGETACTIVE equ 0229h

        jmp     cases

frameproc:
        jmp     far [cs:DEFWINDOWPROC]
childproc:
        jmp     far [cs:DEFMDICHILDPROC]

; SendMessage(HCL, %1, %2, 0)
%macro MDI 2
        push    word [HCL]
        push    word %1
        push    word %2
        push    word 0
        push    word 0
        API     SENDMESSAGE
%endmacro
; AX := (AX == [%1])
%macro SAME 1
        cmp     ax, [%1]
        mov     ax, 0
        jne     %%n
        inc     ax
%%n:
%endmacro

%macro REG 2                            ; proc, name
        mov     word [WC+0], 0003h
        mov     word [WC+2], %1
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
        mov     word [WC+22], %2
        mov     word [WC+24], cs
        PUSHDS  WC
        API     REGISTERCLASS
%endmacro

%macro MDICREATE 1                      ; title -> AX = child
        mov     word [MCS+0], s_child
        mov     [MCS+2], cs
        mov     word [MCS+4], %1
        mov     [MCS+6], cs
        mov     ax, [D_HINST]
        mov     [MCS+8], ax
        mov     word [MCS+10], 8000h    ; CW_USEDEFAULT x4
        mov     word [MCS+12], 8000h
        mov     word [MCS+14], 8000h
        mov     word [MCS+16], 8000h
        mov     word [MCS+18], 0
        mov     word [MCS+20], 0
        mov     word [MCS+22], 0
        mov     word [MCS+24], 0
        push    word [HCL]
        push    word WM_MDICREATE
        push    word 0
        push    ds
        push    word MCS
        API     SENDMESSAGE
%endmacro

cases:
        REG     frameproc, s_frame
        REG     childproc, s_child
        BOOLN
        OUT     "reg.classes"

        PUSHCS  s_frame
        PUSHCS  s_frame
        push    word 02CFh              ; WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN, hidden
        push    word 0
        push    word 20
        push    word 20
        push    word 500
        push    word 400
        push    word 0
        push    word 0
        push    word [D_HINST]
        push    word 0
        push    word 0
        API     CREATEWINDOW
        mov     [HFR], ax

        mov     word [CCS+0], 0
        mov     word [CCS+2], 100
        PUSHCS  s_mdiclient
        PUSHCS  s_empty
        push    word 5600h              ; WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN
        push    word 0
        push    word 0
        push    word 0
        push    word 480
        push    word 360
        push    word [HFR]
        push    word 1
        push    word [D_HINST]
        push    ds
        push    word CCS
        API     CREATEWINDOW
        mov     [HCL], ax
        BOOLN
        OUT     "cw.mdiclient"

        MDICREATE s_one
        mov     [HC1], ax
        BOOLN
        OUT     "mdicreate.1"
        MDICREATE s_two
        mov     [HC2], ax
        BOOLN
        OUT     "mdicreate.2"

        MDI     WM_MDIGETACTIVE, 0
        SAME    HC2
        OUT     "getactive.is.2"

        MDI     WM_MDIACTIVATE, [HC1]
        MDI     WM_MDIGETACTIVE, 0
        SAME    HC1
        OUT     "activate1.getactive.is.1"

        MDI     WM_MDINEXT, [HC1]
        MDI     WM_MDIGETACTIVE, 0
        SAME    HC2
        OUT     "next.getactive.is.2"

        MDI     WM_MDIMAXIMIZE, [HC2]
        MDI     WM_MDIGETACTIVE, 0
        mov     ax, dx
        OUT     "maximize.getactive.dx"
        MDI     WM_MDIRESTORE, [HC2]
        MDI     WM_MDIGETACTIVE, 0
        mov     ax, dx
        OUT     "restore.getactive.dx"

        MDI     WM_MDICASCADE, 0
        BOOLN
        OUT     "cascade"
        MDI     WM_MDITILE, 0
        BOOLN
        OUT     "tile"

        MDI     WM_MDIDESTROY, [HC1]
        push    word [HC1]
        API     ISWINDOW
        BOOLN
        OUT     "destroy1.iswindow"
        MDI     WM_MDIGETACTIVE, 0
        SAME    HC2
        OUT     "destroy1.getactive.is.2"

        push    word [HFR]
        API     DESTROYWINDOW
        jmp     w16_fin

s_empty:     db 0
s_frame:     db 'W16MdiFrame', 0
s_child:     db 'W16MdiChild', 0
s_mdiclient: db 'MDICLIENT', 0
s_one:       db 'One', 0
s_two:       db 'Two', 0

W16_TAIL "w16mdi"
