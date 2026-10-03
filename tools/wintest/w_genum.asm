; w_genum.asm -- GDI enumerations and metafile playback (#295, #296, s90).
;
; EnumFonts (GDI.70) and EnumObjects (GDI.71) call a 16-bit callback once per item
; with a pointer to a WIN16 structure (LOGFONT/TEXTMETRIC, LOGPEN/LOGBRUSH), and
; PlayMetaFile (GDI.123) replays a recording into a DC. The cases settle, against
; stock: how many items each enumeration hands over, what the FIRST item's leading
; fields read (which pins the 16-bit layout -- a Win32 layout reads differently),
; that a callback returning 0 stops the walk and the call returns 0, and that a
; played metafile really drew (a pixel inside the played rectangle changes colour).
; ⚠ COUNTS can legitimately differ between machines (fonts installed). Both hosts here
;   run on the same box against the same GDI, so they must agree exactly.
; Ordinals: gdi.exe's entry table; GetDC/ReleaseDC are USER.66/68.

        org     0
%include "w16.inc"
W16_HEAD
        IMP     U, 66,  GETDC
        IMP     U, 68,  RELEASEDC
        IMP     G, 70,  ENUMFONTS
        IMP     G, 71,  ENUMOBJECTS
        IMP     G, 52,  CREATECOMPATIBLEDC
        IMP     G, 51,  CREATECOMPATIBLEBITMAP
        IMP     G, 45,  SELECTOBJECT
        IMP     G, 29,  PATBLT
        IMP     G, 83,  GETPIXEL
        IMP     G, 27,  RECTANGLE
        IMP     G, 123, PLAYMETAFILE
        IMP     G, 125, CREATEMETAFILE
        IMP     G, 126, CLOSEMETAFILE
        IMP     G, 127, DELETEMETAFILE
        IMP     G, 68,  DELETEDC
        IMP     G, 69,  DELETEOBJECT
W16_IAT

SDC     equ     D_T0                    ; screen DC
CNT     equ     D_T0+2                  ; callback count
FIRST   equ     D_T0+4                  ; first item's word 0
FIRST2  equ     D_T0+6                  ; first item's word at +2 (pen width / brush colour lo)
STOPAT  equ     D_T0+8                  ; return 0 once CNT reaches this (0 = never)
MDC     equ     D_T0+10
HBM     equ     D_T0+12
MF      equ     D_T0+14
RDC     equ     D_T0+16

        jmp     cases

; int FAR PASCAL cb(LPVOID lpItem [, ...], LPARAM) -- args: [bp+6]=lParam, [bp+10]=lpItem
; for EnumObjects (8 bytes); EnumFonts' callback has 14 bytes (lplf, lptm, type, lParam)
; so it has its own entry below. Both count into DGROUP through SS (the task stack).
cb_obj:
        push    bp
        mov     bp, sp
        push    ds
        push    ss
        pop     ds
        push    es
        push    bx
        les     bx, [bp+10]
        call    cb_common
        pop     bx
        pop     es
        pop     ds
        pop     bp
        retf    8

cb_font:
        push    bp
        mov     bp, sp
        push    ds
        push    ss
        pop     ds
        push    es
        push    bx
        les     bx, [bp+16]             ; lplf: lParam(4) + type(2) + lptm(4) above it
        call    cb_common
        pop     bx
        pop     es
        pop     ds
        pop     bp
        retf    14

; ES:BX = the item. Records word 0 and word 2 of the first one; AX = continue?
cb_common:
        cmp     word [CNT], 0
        jne     .n
        mov     ax, [es:bx]
        mov     [FIRST], ax
        mov     ax, [es:bx+2]
        mov     [FIRST2], ax
.n:     inc     word [CNT]
        mov     ax, 1
        mov     cx, [STOPAT]
        jcxz    .r
        cmp     [CNT], cx
        jb      .r
        xor     ax, ax
.r:     ret

%macro RESET 1
        mov     word [CNT], 0
        mov     word [FIRST], 0BEEFh
        mov     word [FIRST2], 0BEEFh
        mov     word [STOPAT], %1
%endmacro

cases:
        push    word 0
        API     GETDC
        mov     [SDC], ax

        ; ── EnumObjects: pens, brushes, a bad type, and a stop after one ──
        RESET   0
        push    word [SDC]
        push    word 1                  ; OBJ_PEN
        push    cs
        push    word cb_obj
        push    word 0
        push    word 0
        API     ENUMOBJECTS
        OUT     "eobj.pen.ret"
        mov     ax, [CNT]
        OUT     "eobj.pen.count"
        mov     ax, [FIRST]
        OUT     "eobj.pen.first.style"
        mov     ax, [FIRST2]
        OUT     "eobj.pen.first.width"

        RESET   0
        push    word [SDC]
        push    word 2                  ; OBJ_BRUSH
        push    cs
        push    word cb_obj
        push    word 0
        push    word 0
        API     ENUMOBJECTS
        OUT     "eobj.brush.ret"
        mov     ax, [CNT]
        OUT     "eobj.brush.count"
        mov     ax, [FIRST]
        OUT     "eobj.brush.first.style"
        mov     ax, [FIRST2]
        OUT     "eobj.brush.first.clr"

        RESET   0
        push    word [SDC]
        push    word 3                  ; not a pen or a brush
        push    cs
        push    word cb_obj
        push    word 0
        push    word 0
        mov     ax, 0BEEFh
        API     ENUMOBJECTS
        OUT     "eobj.badtype.ret"
        mov     ax, [CNT]
        OUT     "eobj.badtype.count"

        RESET   1
        push    word [SDC]
        push    word 1
        push    cs
        push    word cb_obj
        push    word 0
        push    word 0
        API     ENUMOBJECTS
        OUT     "eobj.stop.ret"
        mov     ax, [CNT]
        OUT     "eobj.stop.count"

        ; ── EnumFonts: all faces; one named face; a stop after one ──
        RESET   0
        push    word [SDC]
        push    word 0                  ; lpFaceName = NULL
        push    word 0
        push    cs
        push    word cb_font
        push    word 0
        push    word 0
        API     ENUMFONTS
        OUT     "efont.all.ret"
        mov     ax, [CNT]
        BOOLN
        OUT     "efont.all.some"
        mov     ax, [CNT]
        OUT     "efont.all.count"

        RESET   0
        push    word [SDC]
        PUSHCS  s_arial
        push    cs
        push    word cb_font
        push    word 0
        push    word 0
        API     ENUMFONTS
        OUT     "efont.arial.ret"
        mov     ax, [CNT]
        OUT     "efont.arial.count"
        mov     ax, [FIRST]
        OUT     "efont.arial.first.height"

        RESET   1
        push    word [SDC]
        push    word 0
        push    word 0
        push    cs
        push    word cb_font
        push    word 0
        push    word 0
        API     ENUMFONTS
        OUT     "efont.stop.ret"
        mov     ax, [CNT]
        OUT     "efont.stop.count"

        ; ── PlayMetaFile: record a filled rectangle, play it into a black bitmap ──
        push    word 0
        push    word 0
        API     CREATEMETAFILE          ; NULL = memory metafile
        mov     [RDC], ax
        BOOLN
        OUT     "pmf.create"
        push    word [RDC]
        push    word 0
        push    word 0
        push    word 8
        push    word 8
        API     RECTANGLE               ; default pen black, brush white
        push    word [RDC]
        API     CLOSEMETAFILE
        mov     [MF], ax
        BOOLN
        OUT     "pmf.close"

        push    word 0
        API     CREATECOMPATIBLEDC
        mov     [MDC], ax
        push    word [MDC]
        push    word 8
        push    word 8
        API     CREATECOMPATIBLEBITMAP
        mov     [HBM], ax
        push    word [MDC]
        push    word [HBM]
        API     SELECTOBJECT
        push    word [MDC]
        push    word 0
        push    word 0
        push    word 8
        push    word 8
        push    word 0                  ; BLACKNESS
        push    word 0042h
        API     PATBLT
        push    word [MDC]
        push    word 4
        push    word 4
        API     GETPIXEL
        OUT     "pmf.before"            ; 0000: black
        push    word [MDC]
        push    word [MF]
        mov     ax, 0BEEFh
        API     PLAYMETAFILE
        OUT     "pmf.play.ret"
        push    word [MDC]
        push    word 4
        push    word 4
        API     GETPIXEL
        OUT     "pmf.after.inside"      ; FFFF: the white brush
        push    word [MDC]
        push    word 0
        push    word 0
        API     GETPIXEL
        OUT     "pmf.after.edge"        ; 0000: the black pen
        push    word [MDC]
        push    word 0                  ; not a metafile: the bitmap's handle
        mov     ax, 0BEEFh
        API     PLAYMETAFILE
        OUT     "pmf.play.null"

        push    word [MF]
        API     DELETEMETAFILE
        BOOLN
        OUT     "pmf.delete"
        push    word [MDC]
        API     DELETEDC
        push    word [HBM]
        API     DELETEOBJECT
        push    word 0
        push    word [SDC]
        API     RELEASEDC
        jmp     w16_fin

s_arial: db 'Arial', 0

W16_TAIL "w16genum"
