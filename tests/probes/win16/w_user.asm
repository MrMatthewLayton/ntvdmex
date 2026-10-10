; w_user.asm -- USER: the rectangle arithmetic, the desktop window, and the system
;               metrics a program reads before it draws anything. (GH #163)
;
; The RECT functions are pure: their whole contract is in the SDK and the answer
; cannot depend on the machine, so they are exact-value rows. ⚠ The SDK's rule that a
; RECT's right and bottom edges are EXCLUSIVE is what kuser.ptinrect.edge checks.
; System metrics and colours DESCRIBE THE MACHINE (resolution, theme) and are `*.raw`
; rows whose stock value is OWED; only their documented shape is asserted.
;
;   case                    expect  contract
;   user.setrect            0A28    SetRect(10,20,30,40): AH = left, AL = bottom
;   user.inflaterect        050F    InflateRect(+5,+5): left 5, top 15 (AH, AL)
;   user.inflaterect.rb     232D    ...right 35, bottom 45
;   user.offsetrect         0610    OffsetRect(+1,+1): left 6, top 16
;   user.ptinrect.in        0001    PtInRect((6,16)) -- the top-left corner is inside
;   user.ptinrect.edge      0000    PtInRect((36,20)) -- right edge is exclusive
;   user.intersect.ret      0001    IntersectRect((0,0,10,10),(5,5,20,20)) nonzero
;   user.intersect          0505    ...= (5,5,10,10): AH left, AL top
;   user.intersect.rb       0A0A    ...right 10, bottom 10
;   user.intersect.none     0000    disjoint rectangles -> zero
;   user.intersect.empty    0001    ...and the result is empty (IsRectEmpty nonzero)
;   user.unionrect          0014    UnionRect -> (0,0,20,20): AH left, AL right
;   user.equalrect          0001    EqualRect(r, copy of r) nonzero
;   user.setrectempty       0001    SetRectEmpty then IsRectEmpty nonzero
;   user.desktop.ok         0001    GetDesktopWindow nonzero
;   user.desktop.iswindow   0001    IsWindow(GetDesktopWindow()) nonzero
;   user.iswindow.bogus     0000    IsWindow(0) is zero
;   user.cxscreen.pos       0001    GetSystemMetrics(SM_CXSCREEN) > 0
;   user.cxscreen.raw       owed    machine fact
;   user.cyscreen.raw       owed    machine fact
;   user.mousepresent       0001    GetSystemMetrics(SM_MOUSEPRESENT) nonzero (the test machine has one)
;   user.dblclick.raw       owed    GetDoubleClickTime (XP default 500 = 01F4)
;   user.syscolor.window    owed    GetSysColor(COLOR_WINDOW) low word (theme)
;
; build: tests/probes/win16/build.sh w_user     run: tests/probes/win16/run.sh w_user
        org     0
%include "w16.inc"
W16_HEAD
        IMP     U, 72, SETRECT
        IMP     U, 78, INFLATERECT
        IMP     U, 77, OFFSETRECT
        IMP     U, 76, PTINRECT
        IMP     U, 79, INTERSECTRECT
        IMP     U, 80, UNIONRECT
        IMP     U, 244, EQUALRECT
        IMP     U, 73, SETRECTEMPTY
        IMP     U, 75, ISRECTEMPTY
        IMP     U, 286, GETDESKTOPWINDOW
        IMP     U, 47, ISWINDOW
        IMP     U, 179, GETSYSTEMMETRICS
        IMP     U, 21, GETDOUBLECLICKTIME
        IMP     U, 180, GETSYSCOLOR
W16_IAT

R1      equ     D_BUF                   ; RECT = left, top, right, bottom (words)
R2      equ     D_BUF + 8
R3      equ     D_BUF + 16

; AH := low byte of [%1], AL := low byte of [%2]
%macro PAIR 2
        mov     ah, [%1]
        mov     al, [%2]
%endmacro

; SetRect(%1, l, t, r, b)
%macro SETR 5
        PUSHDS  %1
        push    word %2
        push    word %3
        push    word %4
        push    word %5
        API     SETRECT
%endmacro

cases:
        SETR    R1, 10, 20, 30, 40
        PAIR    R1, R1+6
        OUT     "user.setrect"
        PUSHDS  R1
        push    word 5
        push    word 5
        API     INFLATERECT
        PAIR    R1, R1+2
        OUT     "user.inflaterect"
        PAIR    R1+4, R1+6
        OUT     "user.inflaterect.rb"
        PUSHDS  R1
        push    word 1
        push    word 1
        API     OFFSETRECT
        PAIR    R1, R1+2
        OUT     "user.offsetrect"       ; R1 = (6,16,36,46)

        PUSHDS  R1                      ; PtInRect(LPRECT, POINT): POINT by value,
        push    word 16                 ;   y pushed first so x lies lower in memory
        push    word 6
        API     PTINRECT
        BOOLN
        OUT     "user.ptinrect.in"
        PUSHDS  R1
        push    word 20
        push    word 36                 ; x == right: outside
        API     PTINRECT
        BOOLN
        OUT     "user.ptinrect.edge"

        SETR    R1, 0, 0, 10, 10
        SETR    R2, 5, 5, 20, 20
        PUSHDS  R3
        PUSHDS  R1
        PUSHDS  R2
        API     INTERSECTRECT
        BOOLN
        OUT     "user.intersect.ret"
        PAIR    R3, R3+2
        OUT     "user.intersect"
        PAIR    R3+4, R3+6
        OUT     "user.intersect.rb"
        SETR    R2, 50, 50, 60, 60
        PUSHDS  R3
        PUSHDS  R1
        PUSHDS  R2
        API     INTERSECTRECT
        BOOLN
        OUT     "user.intersect.none"
        PUSHDS  R3
        API     ISRECTEMPTY
        BOOLN
        OUT     "user.intersect.empty"
        SETR    R2, 5, 5, 20, 20
        PUSHDS  R3
        PUSHDS  R1
        PUSHDS  R2
        API     UNIONRECT
        PAIR    R3, R3+4
        OUT     "user.unionrect"
        SETR    R2, 0, 0, 10, 10
        PUSHDS  R1
        PUSHDS  R2
        API     EQUALRECT
        BOOLN
        OUT     "user.equalrect"
        PUSHDS  R2
        API     SETRECTEMPTY
        PUSHDS  R2
        API     ISRECTEMPTY
        BOOLN
        OUT     "user.setrectempty"

        API     GETDESKTOPWINDOW
        mov     bx, ax
        BOOLN
        OUT     "user.desktop.ok"
        push    bx
        API     ISWINDOW
        BOOLN
        OUT     "user.desktop.iswindow"
        push    word 0
        API     ISWINDOW
        BOOLN
        OUT     "user.iswindow.bogus"

        push    word 0                  ; SM_CXSCREEN
        API     GETSYSTEMMETRICS
        mov     bx, ax
        xor     ax, ax
        cmp     bx, 0
        jle     .cx
        inc     ax
.cx:    OUT     "user.cxscreen.pos"
        mov     ax, bx
        OUT     "user.cxscreen.raw"
        push    word 1                  ; SM_CYSCREEN
        API     GETSYSTEMMETRICS
        OUT     "user.cyscreen.raw"
        push    word 19                 ; SM_MOUSEPRESENT
        API     GETSYSTEMMETRICS
        BOOLN
        OUT     "user.mousepresent"
        API     GETDOUBLECLICKTIME
        OUT     "user.dblclick.raw"
        push    word 5                  ; COLOR_WINDOW
        API     GETSYSCOLOR
        OUT     "user.syscolor.window"
        jmp     w16_fin

W16_TAIL "w16user"
