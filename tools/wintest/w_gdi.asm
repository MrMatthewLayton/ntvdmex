; w_gdi.asm -- GDI on a MEMORY DC: no window, nothing on screen, every answer readable
;              back. (GH #163)
;
; A memory DC starts with a 1x1 MONOCHROME bitmap selected, and a bitmap made
; "compatible" with it is monochrome too (SDK, CreateCompatibleBitmap) -- so every
; pixel is black or white and GetPixel's answer is exact on any machine. The default
; DC attributes (white background, black text, OPAQUE, MM_TEXT) are the SDK's.
; Device capabilities of the DISPLAY are machine facts: `*.raw`, stock value OWED.
;
;   case                    expect  contract
;   gdi.memdc.ok            0001    CreateCompatibleDC(NULL) nonzero
;   gdi.bitspixel.raw       owed    GetDeviceCaps(memDC, BITSPIXEL) (display depth)
;   gdi.planes.raw          owed    GetDeviceCaps(memDC, PLANES)
;   gdi.bitmap.ok           0001    CreateCompatibleBitmap(memDC, 8, 8) nonzero
;   gdi.select.prev         0001    SelectObject returns the previous (1x1) bitmap
;   gdi.getobject.ret       000E    GetObject(hbm, 14, &BITMAP) copies 14 bytes
;   gdi.getobject.wh        0808    ...bmWidth 8 (AH), bmHeight 8 (AL)
;   gdi.getobject.fmt       0101    ...bmPlanes 1 (AH), bmBitsPixel 1 (AL): monochrome
;   gdi.patblt              0001    PatBlt(0,0,8,8,BLACKNESS) nonzero
;   gdi.getpixel.black      0000    GetPixel(2,2) low word after BLACKNESS
;   gdi.setpixel.ret        FFFF    SetPixel(1,1,white) returns the colour set (low word)
;   gdi.getpixel.white      FFFF    GetPixel(1,1) low word
;   gdi.getpixel.white.hi   00FF    ...high word (00FFFFFFh)
;   gdi.setbkcolor.prev     FFFF    SetBkColor returns the previous: white by default
;   gdi.settextcolor.prev   0000    SetTextColor returns the previous: black by default
;   gdi.setmapmode.prev     0001    SetMapMode returns the previous: MM_TEXT
;   gdi.bkmode              0002    GetBkMode default: OPAQUE
;   gdi.stockobj            0001    GetStockObject(BLACK_BRUSH) nonzero
;   gdi.deleteobject        0001    DeleteObject(hbm) after it is deselected: nonzero
;   gdi.deletedc            0001    DeleteDC nonzero
;
; build: tools/wintest/build.sh w_gdi     run: tools/wintest/run.sh w_gdi
        org     0
%include "w16.inc"
W16_HEAD
        IMP     G, 52, CREATECOMPATIBLEDC
        IMP     G, 80, GETDEVICECAPS
        IMP     G, 51, CREATECOMPATIBLEBITMAP
        IMP     G, 45, SELECTOBJECT
        IMP     G, 82, GETOBJECT
        IMP     G, 29, PATBLT
        IMP     G, 31, SETPIXEL
        IMP     G, 83, GETPIXEL
        IMP     G, 1, SETBKCOLOR
        IMP     G, 9, SETTEXTCOLOR
        IMP     G, 3, SETMAPMODE
        IMP     G, 76, GETBKMODE
        IMP     G, 87, GETSTOCKOBJECT
        IMP     G, 69, DELETEOBJECT
        IMP     G, 68, DELETEDC
W16_IAT

HDC     equ     D_T0
HBM     equ     D_T0+2
HOLD    equ     D_T0+4
BM      equ     D_BUF                   ; BITMAP: type, w, h, widthbytes, planes(b), bpp(b), bits(dd)

cases:
        push    word 0
        API     CREATECOMPATIBLEDC
        mov     [HDC], ax
        BOOLN
        OUT     "gdi.memdc.ok"
        cmp     word [HDC], 0
        jne     .go
        jmp     w16_fin
.go:
        push    word [HDC]
        push    word 12                 ; BITSPIXEL
        API     GETDEVICECAPS
        OUT     "gdi.bitspixel.raw"
        push    word [HDC]
        push    word 14                 ; PLANES
        API     GETDEVICECAPS
        OUT     "gdi.planes.raw"

        push    word [HDC]
        push    word 8
        push    word 8
        API     CREATECOMPATIBLEBITMAP
        mov     [HBM], ax
        BOOLN
        OUT     "gdi.bitmap.ok"
        push    word [HDC]
        push    word [HBM]
        API     SELECTOBJECT
        mov     [HOLD], ax
        BOOLN
        OUT     "gdi.select.prev"

        push    word [HBM]
        push    word 14
        PUSHDS  BM
        API     GETOBJECT
        OUT     "gdi.getobject.ret"
        mov     ah, [BM+2]
        mov     al, [BM+4]
        OUT     "gdi.getobject.wh"
        mov     ah, [BM+8]
        mov     al, [BM+9]
        OUT     "gdi.getobject.fmt"

        push    word [HDC]
        push    word 0
        push    word 0
        push    word 8
        push    word 8
        push    word 0                  ; BLACKNESS = 00000042h, high word first
        push    word 0042h
        API     PATBLT
        BOOLN
        OUT     "gdi.patblt"
        push    word [HDC]
        push    word 2
        push    word 2
        API     GETPIXEL
        OUT     "gdi.getpixel.black"
        push    word [HDC]
        push    word 1
        push    word 1
        push    word 00FFh              ; RGB(255,255,255) = 00FFFFFFh
        push    word 0FFFFh
        API     SETPIXEL
        OUT     "gdi.setpixel.ret"
        push    word [HDC]
        push    word 1
        push    word 1
        API     GETPIXEL
        OUT     "gdi.getpixel.white"
        mov     ax, dx
        OUT     "gdi.getpixel.white.hi"

        push    word [HDC]
        push    word 0
        push    word 0                  ; black
        API     SETBKCOLOR
        OUT     "gdi.setbkcolor.prev"
        push    word [HDC]
        push    word 00FFh
        push    word 0FFFFh             ; white
        API     SETTEXTCOLOR
        OUT     "gdi.settextcolor.prev"
        push    word [HDC]
        push    word 1                  ; MM_TEXT
        API     SETMAPMODE
        OUT     "gdi.setmapmode.prev"
        push    word [HDC]
        API     GETBKMODE
        OUT     "gdi.bkmode"
        push    word 4                  ; BLACK_BRUSH
        API     GETSTOCKOBJECT
        BOOLN
        OUT     "gdi.stockobj"

        push    word [HDC]
        push    word [HOLD]
        API     SELECTOBJECT            ; deselect ours before deleting it
        push    word [HBM]
        API     DELETEOBJECT
        BOOLN
        OUT     "gdi.deleteobject"
        push    word [HDC]
        API     DELETEDC
        BOOLN
        OUT     "gdi.deletedc"
        jmp     w16_fin

W16_TAIL "w16gdi"
