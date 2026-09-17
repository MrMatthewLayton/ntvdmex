; p_video.com -- what a mode set leaves behind: the BDA video block, INT 10h AH=0Fh
; and the cursor, for each of the modes our guests actually use.
;
; WHY. Session 71 found the BDA display fields at 0040:0049..0085 were NEVER WRITTEN
; -- rows-1 at 0040:0084 read zero, so anything that sizes itself from the BDA saw a
; ONE-ROW SCREEN, and QBasic decided it was on a CGA and sat in its snow-avoidance
; loop. Nothing in a run report catches that: the mode set "succeeded". Only asking a
; reference what the same mode set leaves behind does.
;
; ⚠ PROVISIONAL AGAINST QEMU. Its VGA BIOS is SeaVGABIOS, a reimplementation -- fine
;   for "is this field written at all, and is it the standard value", worthless as a
;   raster or palette reference. The rows here are the standardised ones every VGA
;   BIOS since 1987 agrees on; anything subtler waits for PCem. See docs/PARITY.md.
;
; ⚠ THE PROBE MUST SURVIVE ITS OWN GRAPHICS MODES. Output goes through INT 21h AH=02
;   to a REDIRECTED stdout, so nothing is drawn and mode 13h is harmless; the mode is
;   put back to 3 at the end so the shell that follows has a screen.
;
; ORACLE-ALSO: pcem   (s74b: AMI 486 + genuine IBM VGA ROM -- the bda.crtc row waited six sessions for this)
; nasm -f bin p_video.asm -o p_video.com

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

; VMODE <int10 AX> , <bda case> , <getmode case> , <cursor case>
%macro VMODE 4
        mov     ax, %1
        int     10h
        call    bdaread
        EMIT    %2, "AX,BX,CX,DX"
        POISON                          ; AH=0Fh answers in AX and BH
        mov     ah, 0Fh
        int     10h
        call    probe_capture
        EMIT    %3, "AX,BX"
        ; ⚠ SET THE CURSOR BEFORE READING IT. Asking where it happens to be compares
        ; the two HARNESSES, not the two hosts: the oracle redirects stdout to a file
        ; so nothing has moved it, while our run captures output and the screen cursor
        ; has walked down the page. Set/get is the actual contract and is
        ; harness-independent.
        mov     ah, 02h
        xor     bh, bh
        mov     dx, 0507h
        int     10h
        POISON                          ; AH=03h answers in CX (shape) and DX (pos)
        mov     ah, 03h
        xor     bh, bh
        int     10h
        call    probe_capture
        EMIT    %4, "CX,DX"
%endmacro

start:
        PROBE_BEGIN "video"

        ; ---- 03h: 80x25 colour text. The mode every DOS shell and text UI runs in,
        ; and the one whose BDA rows-1 read zero in session 71.
        VMODE   0003h, "bda.03", "int10.0F.03", "int10.03.03"

        ; ---- 01h: 40x25 text. Session 71 also drew this at stride 640 into a
        ; 320-wide frame, so the columns field is worth asking about directly.
        VMODE   0001h, "bda.01", "int10.0F.01", "int10.03.01"

        ; ---- 07h: 80x25 MONOCHROME. Its CRTC lives at 3B4h, not 3D4h, and 0040:0063
        ; is how a program finds out -- the one mode where that field earns its keep.
        VMODE   0007h, "bda.07", "int10.0F.07", "int10.03.07"

        ; ---- 12h: 640x480 planar, the mode our planar interpreter exists for.
        VMODE   0012h, "bda.12", "int10.0F.12", "int10.03.12"

        ; ---- 13h: 320x200x256, every game we run.
        VMODE   0013h, "bda.13", "int10.0F.13", "int10.03.13"

        ; ---- 06h: 640x200 CGA 2-colour, the odd one out for page size and rows.
        VMODE   0006h, "bda.06", "int10.0F.06", "int10.03.06"

        ; ---- back to text, then the fields a mode set must have refreshed. 0040:0063
        ; is the CRTC index port (3D4 colour / 3B4 mono) and 0040:0065 the mode select
        ; register -- both read by programs that drive the CRTC directly, which is
        ; every text editor that moves a cursor.
        mov     ax, 0003h
        int     10h
        push    ds
        mov     ax, 40h
        mov     ds, ax
        mov     bx, [63h]               ; CRTC index port
        mov     al, [65h]
        xor     ah, ah
        mov     cx, ax                  ; mode select register
        mov     al, [62h]
        xor     ah, ah
        mov     dx, ax                  ; active display page
        mov     ax, [4Eh]               ; page start offset
        pop     ds
        mov     [__ax], ax
        mov     [__bx], bx
        mov     [__cx], cx
        mov     [__dx], dx
        EMIT    "bda.crtc", "AX,BX,CX,DX"

        ; ---- ★ 1130h: the font/rows call every 43- and 50-line editor makes. It
        ; returns the character height in CX and rows-1 in DL, and session 71's
        ; 1112h work is only correct if these agree with the BDA above.
        POISON
        mov     ax, 1130h
        mov     bh, 0                   ; current INT 1Fh font pointer
        int     10h
        mov     ax, cx
        mov     [__ax], ax              ; CX = points (character height)
        mov     al, dl
        xor     ah, ah
        mov     [__bx], ax              ; DL = rows-1
        EMIT    "int10.1130", "AX,BX"

        ; ---- ★ BLINK LIVES IN THE ATTRIBUTE CONTROLLER, index 10h bit 3, and that is
        ; the register the hardware reads. INT 10h 1003h is only the BIOS's way of
        ; writing it, so the two must never disagree: a program that turns blink off
        ; by touching 3C0h directly (which is the usual way, and what QBasic does)
        ; gets nothing if the host keeps its own private flag instead.
        mov     ax, 0003h
        int     10h
        mov     ax, 1003h               ; BL=0 -> intensity, blink OFF
        xor     bl, bl
        int     10h
        call    rd_ar10
        and     ax, 8
        mov     [__ax], ax
        EMIT    "ar10.blink.off", "AX"
        mov     ax, 1003h               ; BL=1 -> blink ON
        mov     bl, 1
        int     10h
        call    rd_ar10
        and     ax, 8
        mov     [__ax], ax
        EMIT    "ar10.blink.on", "AX"
        mov     ax, 0003h               ; leave the screen as we found it
        int     10h

        PROBE_END

; rd_ar10 -- AL = attribute controller register 10h (mode control).
; ⚠ 3DA READ FIRST: the AC's address/data flip-flop is reset by reading the input
;   status register, and without that the index write lands in the data half.
; ⚠ INDEX | 20h keeps the palette ENABLED; leaving bit 5 clear blanks the display.
rd_ar10:
        mov     dx, 3DAh
        in      al, dx
        mov     dx, 3C0h
        mov     al, 30h
        out     dx, al
        mov     dx, 3C1h
        in      al, dx
        push    ax
        mov     dx, 3C0h                ; re-enable the palette before leaving
        mov     al, 20h
        out     dx, al
        pop     ax
        xor     ah, ah
        ret
