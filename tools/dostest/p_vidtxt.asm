; p_vidtxt.com -- #252: the BIOS character services OUTSIDE the text modes, pages
; that page, and AH=12h's answers.
;
; WHAT IT ASKS, and why each is a RASTER question. AH=09h/0Ah/0Eh in a graphics mode
; do not write (char, attr) pairs -- they DRAW the glyph into the frame buffer, in that
; mode's own layout: 8x8 at 320 bytes a line in 13h, 2 bits a pixel across the two
; interleaved CGA banks in 04h, 1 bit in 06h, one bit a pixel per PLANE in the EGA
; modes at the mode's own cell height and stride. So every row below reads the frame
; buffer back and dumps it -- the bytes, not a screenshot.
; AH=05h is asked through the CRTC start address (3D4h index 0Ch/0Dh): which page the
; card SHOWS, not which page the BDA says. And AH=12h is asked for the BL values a VGA
; BIOS answers (AL=12h) and one it does not.
;
; WHO IS TRUTH. PCem's genuine IBM VGA ROM (`pcem`). 6.22 under QEMU is SeaVGABIOS, a
; rewrite -- provisional. DOSBox-X is an emulator's opinion. Output is redirected on
; every oracle, so the mode sets draw nothing anyone sees; it ends in mode 3, page 0.
;
; nasm -f bin p_vidtxt.asm -o p_vidtxt.com

        org     100h
        jmp     start
; The dump is held until the end (PROBE_DEFER, see probe.inc): on our host DOS output
; is DRAWN by the teletype, so printing between cases would paint the very frame
; buffer the next case reads back.
%define PROBE_DEFER
%define PROBE_FCAP 8192
%include "probe.inc"

buf:    times 64 db 0

; v10 -- INT 10h, then capture
v10:
        int     10h
        call    probe_capture
        ret

; setmode AL
%macro SETMODE 1
        mov     ax, %1
        int     10h
%endmacro

; curpos page, row, col
%macro CURPOS 3
        mov     ah, 02h
        mov     bh, %1
        mov     dh, %2
        mov     dl, %3
        int     10h
%endmacro

; vcopy -- CX bytes from AX:SI to buf+DI (DI = offset into buf)
vcopy:
        push    ds
        push    es
        push    cs
        pop     es
        add     di, buf
        mov     ds, ax
        cld
        rep     movsb
        pop     es
        pop     ds
        ret

; rows8 -- 8 rows of BX bytes each from AX:SI, stride DX, into buf from 0
rows8:
        xor     di, di
        mov     bp, 8
.r:     push    si
        push    ax
        push    dx
        mov     cx, bx
        call    vcopy
        sub     di, buf
        pop     dx
        pop     ax
        pop     si
        add     si, dx
        dec     bp
        jnz     .r
        ret

; crtc -- __ax = CRTC start address (0Ch high, 0Dh low), colour port
crtc:
        mov     dx, 3D4h
        mov     al, 0Ch
        out     dx, al
        inc     dx
        in      al, dx
        mov     ah, al
        dec     dx
        mov     al, 0Dh
        out     dx, al
        inc     dx
        in      al, dx
        mov     [__ax], ax
        ret

; plane -- select read map AL (GC index 4)
plane:
        push    dx
        push    ax
        mov     dx, 3CEh
        mov     al, 4
        out     dx, al
        inc     dx
        pop     ax
        out     dx, al
        pop     dx
        ret

; prow -- N rows (CX) of plane AL at A000:SI, stride DX, one byte a row, to buf+DI
prow:
        call    plane
        push    ds
        push    ax
        mov     ax, 0A000h
        mov     ds, ax
.l:     mov     al, [si]
        mov     [cs:buf+di], al
        inc     di
        add     si, dx
        loop    .l
        pop     ax
        pop     ds
        ret

start:
        PROBE_BEGIN "vidtxt"

        ; =================================================== mode 13h
        SETMODE 0013h
        push    es
        mov     ax, 0A000h
        mov     es, ax
        xor     di, di
        mov     cx, 8*320
        mov     al, 55h
        cld
        rep     stosb
        pop     es
        CURPOS  0, 0, 0
        mov     ax, 0941h               ; 'A'
        mov     bx, 000Eh
        mov     cx, 1
        int     10h
        mov     ax, 0A000h
        xor     si, si
        mov     bx, 8
        mov     dx, 320
        call    rows8
        EMIT_BUF "t13.09.A", buf, 64

        CURPOS  0, 0, 1
        mov     ax, 0E42h               ; 'B' by teletype
        mov     bx, 000Ch
        int     10h
        mov     ax, 0A000h
        mov     si, 8
        mov     bx, 8
        mov     dx, 320
        call    rows8
        EMIT_BUF "t13.0E.B", buf, 64
        POISON
        mov     ah, 03h
        mov     bh, 0
        call    v10
        EMIT    "t13.0E.cursor", "DX"

        CURPOS  0, 0, 0
        mov     ax, 0941h               ; 'A' again, XOR bit set
        mov     bx, 008Eh
        mov     cx, 1
        int     10h
        mov     ax, 0A000h
        xor     si, si
        mov     bx, 8
        mov     dx, 320
        call    rows8
        EMIT_BUF "t13.09.xor", buf, 64

        CURPOS  0, 0, 1
        mov     ax, 0800h
        mov     bx, 0
        call    v10
        EMIT    "t13.08.read", "AL"

        ; 06h scroll in a graphics mode: row 1 moves to row 0, row 1 filled with BH
        SETMODE 0013h
        CURPOS  0, 1, 0
        mov     ax, 0958h               ; 'X'
        mov     bx, 000Fh
        mov     cx, 1
        int     10h
        mov     ax, 0601h
        mov     bh, 07h
        xor     cx, cx
        mov     dx, 0127h
        int     10h
        mov     ax, 0A000h
        xor     si, si
        mov     bx, 8
        mov     dx, 320
        call    rows8
        EMIT_BUF "t13.06.row0", buf, 64
        mov     ax, 0A000h
        mov     si, 8*320
        mov     bx, 8
        mov     dx, 320
        call    rows8
        EMIT_BUF "t13.06.row1", buf, 64

        ; =================================================== mode 04h (CGA 2bpp)
        SETMODE 0004h
        CURPOS  0, 0, 0
        mov     ax, 0941h
        mov     bx, 0003h
        mov     cx, 1
        int     10h
        call    cga8
        EMIT_BUF "t04.09.A", buf, 16
        CURPOS  0, 0, 0
        mov     ax, 0941h
        mov     bx, 0083h               ; XOR the same glyph off again
        mov     cx, 1
        int     10h
        call    cga8
        EMIT_BUF "t04.09.xor", buf, 16
        CURPOS  0, 0, 2
        mov     ax, 0E42h
        mov     bx, 0002h
        int     10h
        mov     word [cgaoff], 4
        call    cga8
        mov     word [cgaoff], 0
        EMIT_BUF "t04.0E.B", buf, 16
        mov     ax, 0C02h               ; pixel (5,5) = colour 2
        xor     bx, bx
        mov     cx, 5
        mov     dx, 5
        int     10h
        mov     ax, 0D00h
        xor     bx, bx
        mov     cx, 5
        mov     dx, 5
        call    v10
        EMIT    "t04.0D.read", "AL"
        mov     ax, 0B800h
        mov     si, 2000h + 2*80 + 1
        xor     di, di
        mov     cx, 1
        call    vcopy
        EMIT_BUF "t04.0C.byte", buf, 1

        ; =================================================== mode 06h (CGA 1bpp)
        SETMODE 0006h
        CURPOS  0, 0, 0
        mov     ax, 0941h
        mov     bx, 0001h
        mov     cx, 1
        int     10h
        mov     byte [cgaw], 1
        call    cga8
        mov     byte [cgaw], 2
        EMIT_BUF "t06.09.A", buf, 8

        ; =================================================== mode 0Dh (planar 40x25, 8x8)
        SETMODE 000Dh
        CURPOS  0, 0, 1
        mov     ax, 0941h
        mov     bx, 001Eh               ; colour E; bits 4-6 are NOT a background
        mov     cx, 1
        int     10h
        call    p0d4
        EMIT_BUF "t0D.09.A", buf, 32
        CURPOS  0, 0, 1
        mov     ax, 0941h
        mov     bx, 008Ch               ; XOR C over E -> only plane 1 keeps the glyph
        mov     cx, 1
        int     10h
        call    p0d4
        EMIT_BUF "t0D.09.xor", buf, 32
        mov     ax, 0501h
        int     10h
        call    crtc
        EMIT    "t0D.05.crtc", "AX"
        CURPOS  1, 0, 0
        mov     ax, 095Ah               ; 'Z' on page 1
        mov     bx, 010Fh
        mov     cx, 1
        int     10h
        xor     di, di
        mov     si, 2000h
        mov     dx, 40
        mov     cx, 8
        mov     al, 0
        call    prow
        EMIT_BUF "t0D.09.page1", buf, 8
        mov     ax, 0500h
        int     10h
        call    crtc
        EMIT    "t0D.05.back", "AX"

        ; =================================================== mode 10h (planar 80x25, 8x14)
        SETMODE 0010h
        CURPOS  0, 1, 1
        mov     ax, 0941h
        mov     bx, 000Eh
        mov     cx, 1
        int     10h
        xor     di, di
        mov     si, 14*80 + 1
        mov     dx, 80
        mov     cx, 14
        mov     al, 1
        call    prow
        EMIT_BUF "t10.09.A.p1", buf, 14
        xor     di, di
        mov     si, 14*80 + 1
        mov     dx, 80
        mov     cx, 14
        mov     al, 0
        call    prow
        EMIT_BUF "t10.09.A.p0", buf, 14

        ; =================================================== mode 03h pages
        SETMODE 0003h
        mov     ax, 0501h
        int     10h
        call    crtc
        EMIT    "t03.05.crtc", "AX"
        CURPOS  1, 2, 3
        POISON
        mov     ah, 03h
        mov     bh, 1
        call    v10
        EMIT    "t03.03.page1", "DX"
        POISON
        mov     ah, 03h
        mov     bh, 0
        call    v10
        EMIT    "t03.03.page0", "DX"
        mov     ax, 095Ah               ; 'Z' to page 1 at ITS cursor
        mov     bx, 011Fh
        mov     cx, 1
        int     10h
        mov     ax, 0B900h
        mov     si, (2*80+3)*2
        xor     di, di
        mov     cx, 2
        call    vcopy
        mov     ax, 0B800h
        mov     si, (2*80+3)*2
        mov     di, 2
        mov     cx, 2
        call    vcopy
        EMIT_BUF "t03.09.page1", buf, 4
        mov     ax, 0E51h               ; 'Q' by teletype, BH=0, page 1 active
        mov     bx, 0007h
        int     10h
        mov     ax, 0B800h
        xor     si, si
        xor     di, di
        mov     cx, 2
        call    vcopy
        mov     ax, 0B900h
        mov     si, (2*80+3)*2
        mov     di, 2
        mov     cx, 2
        call    vcopy
        EMIT_BUF "t03.0E.which", buf, 4
        mov     ax, 0500h
        int     10h
        call    crtc
        EMIT    "t03.05.back", "AX"

        ; =================================================== AH=12h
        mov     ax, 1201h               ; BL=30h AL=1: 350 scan lines
        mov     bx, 0030h
        call    v10
        EMIT    "t12.30", "AL"
        SETMODE 0003h
        push    ds
        mov     ax, 40h
        mov     ds, ax
        mov     al, [85h]
        xor     ah, ah
        mov     bl, [84h]
        xor     bh, bh
        mov     cl, [89h]
        xor     ch, ch
        pop     ds
        mov     [__ax], ax
        mov     [__bx], bx
        mov     [__cx], cx
        EMIT    "t12.30.mode3", "AX,BX,CX"
        mov     ax, 1202h               ; back to 400
        mov     bx, 0030h
        int     10h
        SETMODE 0003h

        mov     ax, 1200h
        mov     bx, 0032h
        call    v10
        EMIT    "t12.32", "AL"
        mov     ax, 1201h
        mov     bx, 0033h
        call    v10
        EMIT    "t12.33", "AL"
        mov     ax, 1200h
        mov     bx, 0034h
        call    v10
        EMIT    "t12.34", "AL"
        mov     ax, 1200h
        mov     bx, 0036h
        call    v10
        EMIT    "t12.36", "AL"
        mov     ax, 1277h
        mov     bx, 0055h
        call    v10
        EMIT    "t12.55.unknown", "AL"

        SETMODE 0003h
        PROBE_END

; cga8 -- the 8 glyph rows of the cell at byte [cgaoff] of row 0, [cgaw] bytes each,
; from the two interleaved banks: y even at (y/2)*80, y odd at 2000h + (y/2)*80.
cgaoff: dw 0
cgaw:   db 2
cga8:
        xor     di, di
        xor     bp, bp                  ; y
.y:     mov     ax, bp
        shr     ax, 1
        mov     bx, 80
        mul     bx
        test    bp, 1
        jz      .even
        add     ax, 2000h
.even:  add     ax, [cgaoff]
        mov     si, ax
        mov     ax, 0B800h
        xor     cx, cx
        mov     cl, [cgaw]
        call    vcopy
        sub     di, buf
        inc     bp
        cmp     bp, 8
        jb      .y
        ret

; p0d4 -- planes 0..3, rows 0..7 of mode 0Dh column 1 (stride 40), 8 bytes a plane
p0d4:
        xor     di, di
        xor     al, al
.p:     push    ax
        mov     si, 1
        mov     dx, 40
        mov     cx, 8
        call    prow
        pop     ax
        inc     al
        cmp     al, 4
        jb      .p
        ret
