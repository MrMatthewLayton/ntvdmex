; p_video2.com -- #188: the video BIOS rows p_video left unverified.
;   * the graphics page size at 0040:004C (and rows/height) for 04h/05h/0Dh/0Eh/10h/11h,
;     which are the standard table's values and were never asked of a genuine ROM;
;   * 0040:0065/0066 (CGA mode-select and palette registers) after each of those;
;   * AH=05h page flipping: after selecting page N, 0040:0062 (active page), 0040:004E
;     (the page's start offset) and AH=0Fh's BH -- in 80x25 text and in mode 0Dh.
; Screen output is redirected on the oracles, so the mode sets are harmless; it ends in
; mode 3 on page 0.
;
; nasm -f bin -I ./ p_video2.asm -o p_video2.com

        org     100h
        jmp     start
%include "probe.inc"

; bdaread -- the video block, packed into the four comparable registers.
;   AX = 0040:0049 current mode          BX = 0040:004A columns (word)
;   CX = 0084 rows-1 | 0085 char height  DX = 0040:004C page size (word)
bdaread:
        push    ds
        mov     ax, 40h
        mov     ds, ax
        mov     al, [49h]
        xor     ah, ah
        mov     si, ax                  ; mode
        mov     bx, [4Ah]               ; columns
        mov     al, [84h]
        xor     ah, ah
        mov     di, ax                  ; rows-1
        mov     al, [85h]               ; char height (low byte of the word at 0085)
        xor     ah, ah
        mov     cx, ax
        mov     dx, [4Ch]               ; page size in bytes
        pop     ds
        mov     [__bx], bx
        mov     [__dx], dx
        mov     ax, si
        mov     [__ax], ax
        mov     ax, di                  ; CX = rows-1 | charheight<<8
        and     ax, 0FFh
        mov     bx, cx
        and     bx, 0FFh
        mov     bh, bl
        mov     bl, al
        mov     [__cx], bx
        ret


; cgaread -- AX = 0040:0065 mode select | 0040:0066 palette << 8
cgaread:
        push    ds
        mov     ax, 40h
        mov     ds, ax
        mov     al, [65h]
        mov     ah, [66h]
        pop     ds
        mov     [__ax], ax
        ret

; pageread -- AX = 0040:0062 active page, BX = 0040:004E page offset, CX = AH=0Fh BH
pageread:
        mov     ah, 0Fh
        int     10h
        mov     cl, bh
        xor     ch, ch
        push    ds
        mov     ax, 40h
        mov     ds, ax
        mov     al, [62h]
        xor     ah, ah
        mov     bx, [4Eh]
        pop     ds
        mov     [__ax], ax
        mov     [__bx], bx
        mov     [__cx], cx
        ret

; GMODE <int10 AX>, <bda case>, <cga case>
%macro GMODE 3
        mov     ax, %1
        int     10h
        call    bdaread
        EMIT    %2, "AX,BX,CX,DX"
        call    cgaread
        EMIT    %3, "AX"
%endmacro

; PAGE <page>, <case>
%macro PAGE 2
        mov     ax, 0500h | %1
        int     10h
        call    pageread
        EMIT    %2, "AX,BX,CX"
%endmacro

start:
        PROBE_BEGIN "video2"

        GMODE   0000h, "bda.00", "cga.00"
        GMODE   0001h, "bda.01", "cga.01"
        GMODE   0002h, "bda.02", "cga.02"
        GMODE   0004h, "bda.04", "cga.04"
        GMODE   0005h, "bda.05", "cga.05"
        GMODE   000Dh, "bda.0D", "cga.0D"
        PAGE    1, "page.0D.1"
        PAGE    7, "page.0D.7"
        PAGE    0, "page.0D.0"
        GMODE   000Eh, "bda.0E", "cga.0E"
        GMODE   0010h, "bda.10", "cga.10"
        GMODE   0011h, "bda.11", "cga.11"
        GMODE   0006h, "bda.06", "cga.06"
        GMODE   0012h, "bda.12", "cga.12"
        GMODE   0013h, "bda.13", "cga.13"
        GMODE   0003h, "bda.03", "cga.03"
        PAGE    1, "page.03.1"
        PAGE    3, "page.03.3"
        PAGE    7, "page.03.7"
        PAGE    0, "page.03.0"

        PROBE_END
