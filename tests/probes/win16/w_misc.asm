; w_misc.asm -- the #297 singles and the #295 extent getters, against stock (s90).
;
; USER: GetClipboardFormatName, DlgDirSelect, SetParent, GetClassInfo,
;       ChildWindowFromPoint, CallMsgFilter, GetInternalIconHeader
; SHELL: FindEnvironmentString, InternalExtractIcon  (⚠ XP's shell.dll thunks for
;       these two carry NO argument bytes, so the stub pops nothing: SP is saved and
;       restored around each, as a real caller's BP frame does for it)
; KEYBOARD: GetKBCodePage    GDI: SetPaletteEntries, GetViewportExt, GetWindowExt,
;       GetBitmapDimension
; ⚠ AX POISONED (BEEF) before the calls whose "no answer" must be visible.

        org     0
%include "w16.inc"
W16_HEAD 'SHELL', 'KEYBOARD'
        IMP     U, 41,  CREATEWINDOW
        IMP     U, 53,  DESTROYWINDOW
        IMP     U, 46,  GETPARENT
        IMP     U, 111, SENDMESSAGE
        IMP     U, 57,  REGISTERCLASS
        IMP     U, 107, DEFWINDOWPROC
        IMP     U, 145, REGISTERCLIPBOARDFORMAT
        IMP     U, 146, GETCLIPBOARDFORMATNAME
        IMP     U, 100, DLGDIRLIST
        IMP     U, 99,  DLGDIRSELECT
        IMP     U, 233, SETPARENT
        IMP     U, 404, GETCLASSINFO
        IMP     U, 191, CHILDWINDOWFROMPOINT
        IMP     U, 123, CALLMSGFILTER
        IMP     U, 372, GETINTERNALICONHEADER
        IMP     U, 30,  WINDOWFROMPOINT
        IMP     U, 66,  GETDC
        IMP     U, 68,  RELEASEDC
        IMP     U, 430, LSTRCMP
        IMP     G, 360, CREATEPALETTE
        IMP     G, 364, SETPALETTEENTRIES
        IMP     G, 363, GETPALETTEENTRIES
        IMP     G, 94,  GETVIEWPORTEXT
        IMP     G, 96,  GETWINDOWEXT
        IMP     G, 48,  CREATEBITMAP
        IMP     G, 162, GETBITMAPDIMENSION
        IMP     G, 163, SETBITMAPDIMENSION
        IMP     G, 69,  DELETEOBJECT
        IMP     X, 38,  FINDENVIRONMENTSTRING
        IMP     X, 39,  INTERNALEXTRACTICON
        IMP     Y, 132, GETKBCODEPAGE
W16_IAT

HPAR    equ     D_T0
HSTAT   equ     D_T0+2
HLIST   equ     D_T0+4
HPAR2   equ     D_T0+6
HVIS    equ     D_T0+18
HPAL    equ     D_T0+8
HBMP    equ     D_T0+10
SDC     equ     D_T0+12
FMT     equ     D_T0+14
SAVSP   equ     D_T0+16
BUF     equ     D_BUF                   ; 0x60..0xDF
WC      equ     D_BUF+0x80              ; WNDCLASS16, 26 bytes
SPEC    equ     D_BUF+0xA0
PAL     equ     D_BUF+0xB0              ; LOGPALETTE: ver, n, 2 entries

%macro POISON 0
        mov     ax, 0BEEFh
%endmacro

        jmp     cases

wndproc:                                ; our class's procedure: DefWindowProc
        jmp     far [cs:DEFWINDOWPROC]

; CreateWindow(class, title, style(DWORD), x, y, w, h, parent, menu, hinst, lpParam)
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

cases:
        ; ── register a class of our own, then GetClassInfo it ──
        mov     word [WC+0], 0003h      ; CS_VREDRAW|CS_HREDRAW
        mov     word [WC+2], wndproc
        mov     word [WC+4], cs
        mov     word [WC+6], 0
        mov     word [WC+8], 6          ; cbWndExtra
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
        OUT     "reg.class"
        mov     cx, 13                  ; wipe the WNDCLASS: GetClassInfo must refill it
        mov     di, WC
        push    ds
        pop     es
        mov     ax, 0EEEEh
        rep     stosw
        push    word [D_HINST]
        PUSHCS  s_cls
        PUSHDS  WC
        POISON
        API     GETCLASSINFO
        BOOLN
        OUT     "gci.own"
        mov     ax, [WC+0]
        OUT     "gci.own.style"
        mov     ax, [WC+8]
        OUT     "gci.own.wndextra"
        mov     ax, [WC+16]
        OUT     "gci.own.hbr"
        mov     ax, [WC+2]
        cmp     ax, wndproc
        mov     ax, 0
        jne     .p1
        inc     ax
.p1:    OUT     "gci.own.proc.off.eq"
        push    word [D_HINST]
        PUSHCS  s_nocls
        PUSHDS  WC
        POISON
        API     GETCLASSINFO
        OUT     "gci.none"

        ; ── windows: a hidden popup parent, a static child, a list box ──
        CW      s_cls, 8000h, 0, 0, 0, 200, 200, 0, 0      ; WS_POPUP, hidden
        mov     [HPAR], ax
        BOOLN
        OUT     "cw.parent"
        CW      s_static, 5000h, 0, 10, 10, 20, 20, [HPAR], 5
        mov     [HSTAT], ax
        CW      s_listbox, 4000h, 0, 100, 10, 80, 100, [HPAR], 7
        mov     [HLIST], ax
        CW      s_cls, 8000h, 0, 0, 0, 50, 50, 0, 0
        mov     [HPAR2], ax

        ; ChildWindowFromPoint: on the static; on bare parent; outside
        push    word [HPAR]
        push    word 15                 ; POINT pushed y first? the PASCAL rule:
        push    word 12                 ;   field order in memory -> y, then x
        API     CHILDWINDOWFROMPOINT
        cmp     ax, [HSTAT]
        mov     ax, 0
        jne     .c1
        inc     ax
.c1:    OUT     "cwfp.static"
        push    word [HPAR]
        push    word 150                ; y
        push    word 50                 ; x: bare client area
        API     CHILDWINDOWFROMPOINT
        cmp     ax, [HPAR]
        mov     ax, 0
        jne     .c2
        inc     ax
.c2:    OUT     "cwfp.parent"
        push    word [HPAR]
        push    word 15                 ; y = 15 (inside the static's rows)
        push    word 150                ; x = 150 -> the list box, not the static
        API     CHILDWINDOWFROMPOINT
        cmp     ax, [HLIST]
        mov     ax, 0
        jne     .c3
        inc     ax
.c3:    OUT     "cwfp.xy.order"
        push    word [HPAR]
        push    word 900
        push    word 900
        POISON
        API     CHILDWINDOWFROMPOINT
        OUT     "cwfp.outside"

        ; WindowFromPoint on a VISIBLE popup at screen (300,500) 100x50: a point
        ; that is inside only if x and y are read the right way round
        CW      s_cls, 9000h, 0, 300, 500, 100, 50, 0, 0   ; WS_POPUP|WS_VISIBLE
        mov     [HVIS], ax
        push    word 520                ; y
        push    word 350                ; x
        API     WINDOWFROMPOINT
        cmp     ax, [HVIS]
        mov     ax, 0
        jne     .w1
        inc     ax
.w1:    OUT     "wfp.visible"
        push    word [HVIS]
        API     DESTROYWINDOW

        ; SetParent(static, parent2) -> old parent; GetParent follows
        push    word [HSTAT]
        push    word [HPAR2]
        API     SETPARENT
        cmp     ax, [HPAR]
        mov     ax, 0
        jne     .s1
        inc     ax
.s1:    OUT     "setparent.prev"
        push    word [HSTAT]
        API     GETPARENT
        cmp     ax, [HPAR2]
        mov     ax, 0
        jne     .s2
        inc     ax
.s2:    OUT     "setparent.now"

        ; DlgDirList(drives only) then select item 1 and DlgDirSelect it
        mov     word [SPEC], '*.'
        mov     word [SPEC+2], '*'
        push    word [HPAR]
        PUSHDS  SPEC
        push    word 7
        push    word 0
        push    word 0C000h             ; DDL_EXCLUSIVE|DDL_DRIVES
        API     DLGDIRLIST
        BOOLN
        OUT     "ddl.drives"
        push    word [HLIST]
        push    word 0407h              ; LB_SETCURSEL (WM_USER+7)
        push    word 1
        push    word 0
        push    word 0
        API     SENDMESSAGE
        OUT     "ddl.setcursel"
        mov     word [BUF], 0EEEEh
        mov     word [BUF+2], 0EEEEh
        push    word [HPAR]
        PUSHDS  BUF
        push    word 7
        POISON
        API     DLGDIRSELECT
        OUT     "dds.ret"
        mov     ax, [BUF]
        OUT     "dds.buf01"
        mov     ax, [BUF+2]
        OUT     "dds.buf23"

        ; ── clipboard format names ──
        PUSHCS  s_fmt
        API     REGISTERCLIPBOARDFORMAT
        mov     [FMT], ax
        BOOLN
        OUT     "rcf"
        push    word [FMT]
        PUSHDS  BUF
        push    word 64
        POISON
        API     GETCLIPBOARDFORMATNAME
        OUT     "gcfn.len"
        PUSHDS  BUF
        PUSHCS  s_fmt
        API     LSTRCMP
        SIGN
        OUT     "gcfn.eq"
        push    word 1                  ; CF_TEXT
        PUSHDS  BUF
        push    word 64
        POISON
        API     GETCLIPBOARDFORMATNAME
        OUT     "gcfn.cftext"
        push    word [FMT]
        PUSHDS  BUF
        push    word 4                  ; truncated
        POISON
        API     GETCLIPBOARDFORMATNAME
        OUT     "gcfn.trunc.len"

        ; ── CallMsgFilter with no hook ──
        PUSHDS  BUF
        push    word 0
        POISON
        API     CALLMSGFILTER
        OUT     "callmsgfilter"

        ; ── GetInternalIconHeader(lp, lp) -- undocumented ──
        mov     [SAVSP], sp
        PUSHDS  BUF
        PUSHDS  BUF+0x20
        POISON
        API     GETINTERNALICONHEADER
        mov     sp, [SAVSP]
        OUT     "giih"

        ; ── SHELL's two argument-less thunks ──
        mov     [SAVSP], sp
        PUSHCS  s_path
        POISON
        mov     dx, 0BEEFh
        API     FINDENVIRONMENTSTRING
        mov     sp, [SAVSP]
        OUT     "fes.ax"
        mov     ax, dx
        OUT     "fes.dx"
        mov     [SAVSP], sp
        push    word [D_HINST]
        PUSHCS  s_progman
        push    word 0
        push    word 1
        POISON
        API     INTERNALEXTRACTICON
        mov     sp, [SAVSP]
        OUT     "iei"

        ; ── KEYBOARD ──
        POISON
        API     GETKBCODEPAGE
        OUT     "kbcp"

        ; ── palettes ──
        mov     word [PAL], 0300h
        mov     word [PAL+2], 2
        mov     word [PAL+4], 0011h     ; R=11 G=00
        mov     word [PAL+6], 0022h     ; B=22 flags=00
        mov     word [PAL+8], 0033h
        mov     word [PAL+10], 0044h
        PUSHDS  PAL
        API     CREATEPALETTE
        mov     [HPAL], ax
        BOOLN
        OUT     "pal.create"
        mov     word [BUF], 5566h
        mov     word [BUF+2], 0077h
        push    word [HPAL]
        push    word 1
        push    word 1
        PUSHDS  BUF
        POISON
        API     SETPALETTEENTRIES
        OUT     "spe.ret"
        mov     word [BUF], 0
        mov     word [BUF+2], 0
        push    word [HPAL]
        push    word 1
        push    word 1
        PUSHDS  BUF
        API     GETPALETTEENTRIES
        OUT     "gpe.ret"
        mov     ax, [BUF]
        OUT     "gpe.rg"
        mov     ax, [BUF+2]
        OUT     "gpe.bf"
        push    word [HPAL]
        push    word 5                  ; past the end
        push    word 1
        PUSHDS  BUF
        POISON
        API     SETPALETTEENTRIES
        OUT     "spe.past"

        ; ── extents ──
        push    word 0
        API     GETDC
        mov     [SDC], ax
        push    word [SDC]
        POISON
        API     GETVIEWPORTEXT
        OUT     "gve.cx"
        mov     ax, dx
        OUT     "gve.cy"
        push    word [SDC]
        POISON
        API     GETWINDOWEXT
        OUT     "gwe.cx"
        mov     ax, dx
        OUT     "gwe.cy"
        push    word 8
        push    word 8
        push    word 1
        push    word 1
        push    word 0
        push    word 0
        API     CREATEBITMAP
        mov     [HBMP], ax
        push    word [HBMP]
        POISON
        API     GETBITMAPDIMENSION
        OUT     "gbd.zero"
        push    word [HBMP]
        push    word 123
        push    word 45
        API     SETBITMAPDIMENSION
        push    word [HBMP]
        API     GETBITMAPDIMENSION
        OUT     "gbd.cx"
        mov     ax, dx
        OUT     "gbd.cy"

        ; tidy
        push    word [HBMP]
        API     DELETEOBJECT
        push    word [HPAL]
        API     DELETEOBJECT
        push    word 0
        push    word [SDC]
        API     RELEASEDC
        push    word [HPAR2]
        API     DESTROYWINDOW
        push    word [HPAR]
        API     DESTROYWINDOW
        jmp     w16_fin

s_empty:   db 0
s_cls:     db 'W16Misc', 0
s_nocls:   db 'NoSuchClassHere', 0
s_static:  db 'STATIC', 0
s_listbox: db 'LISTBOX', 0
s_fmt:     db 'NtvdmexFmt', 0
s_path:    db 'PATH', 0
s_progman: db 'PROGMAN.EXE', 0

W16_TAIL "w16misc"
