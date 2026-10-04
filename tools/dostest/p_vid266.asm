; p_vid266.com -- #266: INT 10h after #252's remainders.
;
; WHAT IT ASKS -- each one a question of STATE the call leaves, read back the way a
; guest reads it, never the screen:
;   0Bh  set background / border / CGA palette. The attribute controller's AR00-AR03
;        and AR11, read at 3C1h, and the CGA colour-select byte 0040:0066, after BH=1
;        BL=0/1 and BH=0 BL=04h/1Ch in modes 04h/05h/06h/03h/0Dh/13h. Rows `tMM.0B.*`
;        are 6 bytes: AR00 AR01 AR02 AR03 AR11 0066.
;   04h  read light pen: AX..DX after the call (a VGA has none; AH=00h expected).
;   11h  AL=03h -> SR3 read back; AL=20h-24h -> INT 1Fh / INT 43h and 0040:0084/0085.
;        A vector's SEGMENT differs on every host (C000 on a real ROM, B000-B180 on
;        ours), so a font row does not print the pointer: it prints whether INT 43h
;        EQUALS the pointer AH=11h AL=30h hands out for BH=02h (8x14), 03h (8x8), 06h
;        (8x16) -- a fact about the BIOS that is the same on every honest host.
;        Rows `i43.*` / `f2x.*` are 7 bytes: [=8x14] [=8x8] [=8x16] 0084 0085
;        [=1130h BH=01h's pointer] [1130h BH=01h's CX].
;   00A8 the Video Save Pointer table: which of its 7 far pointers are non-null, the
;        secondary table's length + the DCC table, and the 64-byte parameter-table
;        entries 03h (mode 3 at 200 lines -- what a naive `mode*64` lookup finds), 18h
;        (mode 3+, 400 lines), 1Ch (13h), 04h, 0Dh, 12h (10h), 1Bh (12h).
;   SR1  12h BL=36h AL=1/0 -> Clocking Mode read back (bit 5 = screen off).
;
; WHO IS TRUTH. PCem's genuine IBM VGA ROM (`pcem`). 6.22 under QEMU is SeaVGABIOS, a
; rewrite -- provisional. DOSBox-X is an emulator's opinion. Every INT 1Fh/43h the probe
; moves is put back before it exits; it ends in mode 3.
;
; nasm -f bin p_vid266.asm -o p_vid266.com

        org     100h
        jmp     start
; Deferred dump (probe.inc): mode sets and AH=09h-free, but the teletype would still
; paint mode 13h's frame buffer if the rows were printed as they were made.
%define PROBE_DEFER
%define PROBE_FCAP 8192
%include "probe.inc"

buf:    times 64 db 0
ufont:  times 32 db 0                   ; a "caller's font" for 21h/20h (never drawn)
o1f:    dw 0
s1f:    dw 0
o43:    dw 0
s43:    dw 0
fseg:   dw 0
foff:   dw 0
fcx:    dw 0
sp_o:   dw 0
sec_o:  dw 0
sec_s:  dw 0
seclen: dw 0
bhv:    db 0

; v10 -- INT 10h, then capture
v10:
        int     10h
        call    probe_capture
        ret

%macro SETMODE 1
        mov     ax, %1
        int     10h
%endmacro

; ac_read -- AL = AC index -> AL = value. 3DAh first (flip-flop to index), index with
; bit 5 set (palette address source = the display stays on), read 3C1h; then 3DAh again
; and 20h, so the card is left displaying and the flip-flop in a known state.
ac_read:
        push    dx
        push    bx
        mov     bl, al
        mov     dx, 3DAh
        in      al, dx
        mov     dx, 3C0h
        mov     al, bl
        or      al, 20h
        out     dx, al
        inc     dx
        in      al, dx
        mov     bl, al
        mov     dx, 3DAh
        in      al, dx
        mov     dx, 3C0h
        mov     al, 20h
        out     dx, al
        mov     dx, 3DAh
        in      al, dx
        mov     al, bl
        pop     bx
        pop     dx
        ret

; bda -- BX = offset in 0040: -> AL
bda:
        push    es
        mov     ax, 40h
        mov     es, ax
        mov     al, [es:bx]
        pop     es
        ret

; acdump -- buf = AR00 AR01 AR02 AR03 AR11 [0040:0066]
acdump:
        mov     al, 00h
        call    ac_read
        mov     [buf+0], al
        mov     al, 01h
        call    ac_read
        mov     [buf+1], al
        mov     al, 02h
        call    ac_read
        mov     [buf+2], al
        mov     al, 03h
        call    ac_read
        mov     [buf+3], al
        mov     al, 11h
        call    ac_read
        mov     [buf+4], al
        mov     bx, 66h
        call    bda
        mov     [buf+5], al
        ret

; set0b BH, BL -- INT 10h AH=0Bh
%macro SET0B 2
        mov     ah, 0Bh
        mov     bh, %1
        mov     bl, %2
        int     10h
%endmacro

; f30 -- AX=1130h, BH=[bhv] -> [fseg]:[foff], CX -> [fcx]
f30:
        push    es
        push    bp
        mov     ax, 1130h
        mov     bh, [bhv]
        int     10h
        mov     [fseg], es
        mov     [foff], bp
        mov     [fcx], cx
        pop     bp
        pop     es
        ret
%macro F30 1
        mov     byte [bhv], %1
        call    f30
%endmacro

; eqvec -- BL = vector: AL = 1 if IVT[BL] == [fseg]:[foff], else 0
eqvec:
        push    es
        push    si
        push    bx
        xor     ax, ax
        mov     es, ax
        xor     bh, bh
        shl     bx, 1
        shl     bx, 1
        mov     si, [es:bx]
        mov     ax, [es:bx+2]
        cmp     si, [foff]
        jne     .no
        cmp     ax, [fseg]
        jne     .no
        mov     al, 1
        jmp     .done
.no:    xor     al, al
.done:  pop     bx
        pop     si
        pop     es
        ret

; fontrow -- buf = [43h=8x14] [43h=8x8] [43h=8x16] 0084 0085 [43h=1130/01] [1130/01 CX]
fontrow:
        F30     02h
        mov     bl, 43h
        call    eqvec
        mov     [buf+0], al
        F30     03h
        mov     bl, 43h
        call    eqvec
        mov     [buf+1], al
        F30     06h
        mov     bl, 43h
        call    eqvec
        mov     [buf+2], al
        mov     bx, 84h
        call    bda
        mov     [buf+3], al
        mov     bx, 85h
        call    bda
        mov     [buf+4], al
        F30     01h
        mov     bl, 43h
        call    eqvec
        mov     [buf+5], al
        mov     al, [fcx]
        mov     [buf+6], al
        ret

; vcopy -- CX bytes from AX:SI to buf (from 0)
vcopy:
        push    ds
        push    es
        push    di
        push    cs
        pop     es
        mov     di, buf
        mov     ds, ax
        cld
        rep     movsb
        pop     di
        pop     es
        pop     ds
        ret

; sr -- AL = sequencer index -> AL = value
sr:
        push    dx
        mov     dx, 3C4h
        out     dx, al
        inc     dx
        in      al, dx
        pop     dx
        ret

start:
        PROBE_BEGIN "vid266"
        ; keep INT 1Fh / INT 43h as we found them -- put back at the end
        push    es
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:1Fh*4]
        mov     [o1f], ax
        mov     ax, [es:1Fh*4+2]
        mov     [s1f], ax
        mov     ax, [es:43h*4]
        mov     [o43], ax
        mov     ax, [es:43h*4+2]
        mov     [s43], ax
        pop     es

        ; =================================================== 0Bh, mode 04h
        SETMODE 0004h
        call    acdump
        EMIT_BUF "t04.def", buf, 6
        SET0B   1, 0
        call    acdump
        EMIT_BUF "t04.0B.p0", buf, 6
        SET0B   1, 1
        call    acdump
        EMIT_BUF "t04.0B.p1", buf, 6
        SET0B   0, 04h
        call    acdump
        EMIT_BUF "t04.0B.bg04", buf, 6
        SET0B   0, 1Ch
        call    acdump
        EMIT_BUF "t04.0B.bg1C", buf, 6
        SET0B   1, 0
        call    acdump
        EMIT_BUF "t04.0B.bg1C.p0", buf, 6
        mov     ax, 0B00h               ; what the call leaves in the registers
        mov     bx, 0101h
        mov     cx, 0C1C1h
        mov     dx, 0D1D1h
        call    v10
        EMIT    "t04.0B.regs", "AX,BX,CX,DX"

        ; =================================================== 05h, 06h
        SETMODE 0005h
        call    acdump
        EMIT_BUF "t05.def", buf, 6
        SET0B   1, 0
        call    acdump
        EMIT_BUF "t05.0B.p0", buf, 6
        SETMODE 0006h
        call    acdump
        EMIT_BUF "t06.def", buf, 6
        SET0B   0, 04h
        call    acdump
        EMIT_BUF "t06.0B.bg04", buf, 6
        SET0B   1, 1
        call    acdump
        EMIT_BUF "t06.0B.p1", buf, 6

        ; =================================================== text, EGA, 13h
        SETMODE 0003h
        call    acdump
        EMIT_BUF "t03.def", buf, 6
        SET0B   0, 01h
        call    acdump
        EMIT_BUF "t03.0B.bg01", buf, 6
        SET0B   1, 1
        call    acdump
        EMIT_BUF "t03.0B.p1", buf, 6
        SETMODE 000Dh
        SET0B   0, 02h
        call    acdump
        EMIT_BUF "t0D.0B.bg02", buf, 6
        SETMODE 0013h
        SET0B   0, 02h
        call    acdump
        EMIT_BUF "t13.0B.bg02", buf, 6

        ; =================================================== 04h light pen
        SETMODE 0003h
        mov     ax, 04A5h
        mov     bx, 0B1B1h
        mov     cx, 0C1C1h
        mov     dx, 0D1D1h
        call    v10
        EMIT    "t03.04.pen", "AX,BX,CX,DX"

        ; =================================================== INT 43h after a mode set
        SETMODE 0012h
        call    fontrow
        EMIT_BUF "i43.mode12", buf, 7
        SETMODE 0010h
        call    fontrow
        EMIT_BUF "i43.mode10", buf, 7
        SETMODE 0013h
        call    fontrow
        EMIT_BUF "i43.mode13", buf, 7
        SETMODE 0004h
        call    fontrow
        EMIT_BUF "i43.mode04", buf, 7
        SETMODE 0003h
        call    fontrow
        EMIT_BUF "i43.mode03", buf, 7
        mov     ax, 1112h               ; text 8x8: does INT 43h follow? (unmeasured)
        mov     bl, 0
        int     10h
        call    fontrow
        EMIT_BUF "i43.mode03.1112", buf, 7

        ; =================================================== 11h AL=22h-24h, mode 12h
        SETMODE 0012h
        mov     ax, 1122h
        mov     bl, 1
        int     10h
        call    fontrow
        EMIT_BUF "f22.bl1", buf, 7
        mov     ax, 1123h
        mov     bx, 0002h
        mov     cx, 0C1C1h
        mov     dx, 0D1D1h
        call    v10
        EMIT    "f23.regs", "AX,BX,CX,DX"
        call    fontrow
        EMIT_BUF "f23.bl2", buf, 7
        mov     ax, 1124h
        mov     bl, 3
        int     10h
        call    fontrow
        EMIT_BUF "f24.bl3", buf, 7
        mov     ax, 1123h
        mov     bl, 0
        mov     dl, 30
        int     10h
        call    fontrow
        EMIT_BUF "f23.bl0.dl30", buf, 7
        mov     ax, 1123h
        mov     bl, 7
        mov     dl, 30
        int     10h
        call    fontrow
        EMIT_BUF "f23.bl7", buf, 7

        ; 21h: the caller's table -- INT 43h = ES:BP, rows from DL, height CX
        push    es
        push    bp
        push    cs
        pop     es
        mov     bp, ufont
        mov     ax, 1121h
        mov     bl, 0
        mov     cx, 10
        mov     dl, 48
        int     10h
        pop     bp
        pop     es
        call    fontrow                 ; the three ROM flags read 0 here
        mov     [fseg], cs
        mov     word [foff], ufont
        mov     bl, 43h
        call    eqvec
        mov     [buf+0], al             ; [0] = INT 43h == the caller's table
        F30     01h
        mov     ax, [foff]
        cmp     ax, ufont
        jne     .n21
        mov     ax, [fseg]
        mov     bx, cs
        cmp     ax, bx
        jne     .n21
        mov     byte [buf+5], 1         ; [5] = 1130h BH=01h hands out the caller's table
        jmp     .d21
.n21:   mov     byte [buf+5], 0
.d21:   EMIT_BUF "f21.user", buf, 7

        ; 20h: INT 1Fh = ES:BP
        push    es
        push    bp
        push    cs
        pop     es
        mov     bp, ufont+16
        mov     ax, 1120h
        int     10h
        pop     bp
        pop     es
        mov     [fseg], cs
        mov     word [foff], ufont+16
        mov     bl, 1Fh
        call    eqvec
        mov     [buf+0], al             ; [0] = INT 1Fh == ES:BP
        F30     00h
        mov     ax, [foff]
        mov     bx, [fseg]
        mov     cx, cs
        xor     dl, dl
        cmp     ax, ufont+16
        jne     .n20
        cmp     bx, cx
        jne     .n20
        mov     dl, 1
.n20:   mov     [buf+1], dl             ; [1] = 1130h BH=00h hands it out
        SETMODE 0003h
        mov     [fseg], cs
        mov     word [foff], ufont+16
        mov     bl, 1Fh
        call    eqvec
        mov     [buf+2], al             ; [2] = still there after a mode set
        mov     word [foff], ufont
        mov     bl, 43h
        call    eqvec
        mov     [buf+3], al             ; [3] = INT 43h still the caller's after a mode set
        EMIT_BUF "f20.user", buf, 4

        ; =================================================== 11h AL=03h -> SR3
        mov     ax, 1103h
        mov     bl, 05h
        int     10h
        mov     al, 03h
        call    sr
        mov     [buf+0], al
        mov     ax, 1103h
        mov     bl, 00h
        int     10h
        mov     al, 03h
        call    sr
        mov     [buf+1], al
        EMIT_BUF "f03.sr3", buf, 2

        ; =================================================== SR1 via 12h BL=36h
        mov     ax, 1201h
        mov     bl, 36h
        int     10h
        mov     al, 01h
        call    sr
        mov     [buf+0], al
        mov     ax, 1200h
        mov     bl, 36h
        int     10h
        mov     al, 01h
        call    sr
        mov     [buf+1], al
        EMIT_BUF "t12.36.sr1", buf, 2

        ; =================================================== 0040:00A8
        SETMODE 0003h
        push    es
        mov     ax, 40h
        mov     es, ax
        les     si, [es:0A8h]           ; ES:SI = the save pointer table
        mov     [sp_o], si
        xor     di, di
        mov     cx, 7
.sp:    mov     ax, [es:si]
        or      ax, [es:si+2]
        jz      .z
        mov     al, 1
.z:     mov     [buf+di], al
        add     si, 4
        inc     di
        loop    .sp
        mov     si, [sp_o]
        mov     ax, [es:si]             ; +00: the parameter table
        mov     [foff], ax
        mov     ax, [es:si+2]
        mov     [fseg], ax
        mov     ax, [es:si+10h]         ; +10: the secondary save pointer table
        mov     [sec_o], ax
        mov     ax, [es:si+12h]
        mov     [sec_s], ax
        pop     es
        EMIT_BUF "saveptr.nonnull", buf, 7
        mov     ax, [sec_o]
        or      ax, [sec_s]
        jz      .nosec
        push    es
        mov     es, [sec_s]
        mov     si, [sec_o]
        mov     ax, [es:si]             ; its length
        mov     [seclen], ax
        les     si, [es:si+2]           ; +02: the display combination code table
        mov     ax, es
        pop     es
        mov     cx, 36
        call    vcopy
        EMIT_BUF "saveptr.dcc", buf, 36
        mov     ax, [seclen]
        mov     [buf], al
        mov     [buf+1], ah
        EMIT_BUF "saveptr.seclen", buf, 2
        jmp     .vp
.nosec: mov     byte [buf], 0
        EMIT_BUF "saveptr.sec.none", buf, 1
.vp:
%macro VPARAM 2
        mov     ax, [fseg]
        mov     si, [foff]
        add     si, %1 * 64
        mov     cx, 64
        call    vcopy
        EMIT_BUF %2, buf, 64
%endmacro
        VPARAM  03h, "vparam.03"
        VPARAM  18h, "vparam.18"
        VPARAM  1Ch, "vparam.1C"
        VPARAM  04h, "vparam.04"
        VPARAM  0Dh, "vparam.0D"
        VPARAM  12h, "vparam.12"
        VPARAM  1Bh, "vparam.1B"

        ; mode 3 again, then the vectors exactly as we found them (both pointed into
        ; this program's memory at some point)
        SETMODE 0003h
        push    es
        xor     ax, ax
        mov     es, ax
        cli
        mov     ax, [o1f]
        mov     [es:1Fh*4], ax
        mov     ax, [s1f]
        mov     [es:1Fh*4+2], ax
        mov     ax, [o43]
        mov     [es:43h*4], ax
        mov     ax, [s43]
        mov     [es:43h*4+2], ax
        sti
        pop     es
        PROBE_END
