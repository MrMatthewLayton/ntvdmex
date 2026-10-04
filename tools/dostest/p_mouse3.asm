; p_mouse3.com -- INT 33h: the graphics cursor in VIDEO MEMORY (09h, GH #264), the
; acceleration profiles / settings / .INI name (2Bh-2Eh, 33h, 34h) and the alternate
; shift-qualified handlers (18h/19h) (GH #265).
;
; WHY A THIRD PROBE. p_mouse.asm and p_mouse2.asm have oracle caches keyed by their
; binaries; adding rows there would throw both away. This one asks only the #264/#265
; questions.
;
; ── PART A, mode 3: the register contracts. ──────────────────────────────────────────
;   32h's bitmap; 2Ch/2Dh/2Bh/2Eh round trips with the blocks dumped (lengths, the
;   first thresholds and factors, the four names); 33h with a big and a 5-byte buffer
;   (the block, and whether CX comes back as the count); 34h and the string at ES:DX;
;   18h install/refuse/replace and 19h lookup, then whether a 00h reset forgets them.
;   ⚠ DELIVERY of an 18h handler needs a click with Shift held, which a probe cannot
;     make; only the bookkeeping is measured here.
;
; ── PART B, mode 13h: DOES THE DRIVER WRITE THE CURSOR INTO VRAM? ────────────────────
;   VRAM is filled with A5h, the pointer put at virtual (320,100) = pixel (160,100), a
;   bitmap defined with one diagnostic row each:
;       row 0  screen FFFF cursor 0000   transparent          -> A5
;       row 1  screen 0000 cursor 0000   cleared              -> 00
;       row 2  screen 0000 cursor FFFF   0 XOR "ones"         -> 0F or FF: how many bits
;       row 3  screen FFFF cursor FFFF   inverted             -> A5^ones
;       row 4  screen 00FF cursor 0000   left half cleared    -> bit order AND width
;       row 5  screen FFFF cursor 8001   edge pixels flipped  -> 16 or 8 pixels wide
;   then shown, and the bytes under it READ BACK (32 bytes from x=152, rows 99-106).
;   A driver that draws into video memory (MS MOUSE.COM, DOSBox's) shows the rows; a
;   host OVERLAY (ours since #264, by decision -- main.c, the present path) leaves
;   A5 everywhere. ⇒ THE SUBJECT IS EXPECTED TO DISAGREE ON THE vram ROWS. What the
;   oracle rows decide is how far that matters, and they pin the arithmetic (width,
;   bit order, the XOR value, the hot-spot unit) our overlay copies.
;   Then hidden (02h) and read back again: a driver that writes VRAM must RESTORE it.
;   Then the hot spot: a bitmap whose only visible row is row 2, hot spot (4,2) -- if
;   the hot spot counts screen pixels the row lands at y=100 from x=156; if it counts
;   the doubled virtual X, from x=158.
;   MOUSE.COM 6.24 may not support mode 13h at all (RBIL credits Logitech/Genius with
;   it); then its rows stay A5 too, and that is ITS answer.
;
; Every output register is POISONED first (probe.inc); a driver that ignores a call
; hands the caller's registers back, and "untouched" must not read as "answered".
; The dump is DEFERRED (PROBE_DEFER): on our host DOS output is drawn by the teletype,
; so printing between cases would paint the very VRAM the next case reads.
;
; nasm -f bin p_mouse3.asm -o p_mouse3.com

        org     100h
        jmp     start
%define PROBE_DEFER
%define PROBE_FCAP 16384
%include "probe.inc"

m33:
        int     33h
        call    probe_capture
        ret

; An 18h handler. Never called by this probe (no Shift-click can be made); only its
; ADDRESS is round-tripped through 19h, and its offset is the same on every host.
alth:   retf

; ---- copy CX bytes from [__es]:[__si] (or [__es]:[__dx] via copy_esdx) to CS:DI
copy_essi:
        mov     bx, [__si]
        jmp     copy_far
copy_esdx:
        mov     bx, [__dx]
copy_far:
        push    ds
        push    es
        push    cs
        pop     es
        mov     si, bx
        mov     ax, [__es]
        mov     ds, ax
        cld
        rep     movsb
        pop     es
        pop     ds
        ret

; ---- zero CX bytes at CS:DI
zero_buf:
        push    es
        push    cs
        pop     es
        xor     al, al
        cld
        rep     stosb
        pop     es
        ret

; ---- fill CX bytes at CS:DI with AL
fill_buf:
        push    es
        push    cs
        pop     es
        cld
        rep     stosb
        pop     es
        ret

; ---- wait for three BIOS ticks (a driver that draws from its IRQ has had the chance)
wait2:
        push    es
        push    ax
        push    cx
        mov     ax, 40h
        mov     es, ax
        mov     cx, 3
.t:     mov     ax, [es:6Ch]
.s:     cmp     ax, [es:6Ch]
        je      .s
        loop    .t
        pop     cx
        pop     ax
        pop     es
        ret

; ---- 32 bytes of row AX at x=BX, mode 13h, into rowbuf
grab_row:
        push    ds
        push    es
        push    dx
        mov     cx, 320
        mul     cx
        add     ax, bx
        mov     si, ax
        push    cs
        pop     es
        mov     di, rowbuf
        mov     ax, 0A000h
        mov     ds, ax
        mov     cx, 32
        cld
        rep     movsb
        pop     dx
        pop     es
        pop     ds
        ret

%macro VROW 3                           ; name, y, x
        mov     ax, %2
        mov     bx, %3
        call    grab_row
        EMIT_BUF %1, rowbuf, 32
%endmacro

; ---- data ----------------------------------------------------------------------
; 09h bitmap 1: the diagnostic rows (see the header). 16 screen words, 16 cursor words.
gc1:    dw      0FFFFh, 0000h, 0000h, 0FFFFh, 00FFh, 0FFFFh
        times 10 dw 0FFFFh
        dw      0000h, 0000h, 0FFFFh, 0FFFFh, 0000h, 8001h
        times 10 dw 0000h
; 09h bitmap 2: only row 2 visible (screen 0000, cursor FFFF), for the hot spot.
gc2:    dw      0FFFFh, 0FFFFh, 0000h
        times 13 dw 0FFFFh
        dw      0000h, 0000h, 0FFFFh
        times 13 dw 0000h

; a profile block to load with 2Bh: lengths 2/0/0/0, profile 1 thresholds 10h,20h,
; factors 18h,20h, everything else as RBIL's "unused" fill, recognisable names.
myacc:  db      2, 0, 0, 0
        db      10h, 20h
        times 126 db 7Fh
        db      18h, 20h
        times 126 db 10h
        db      'PROBE-ONE       ', 'PROBE-TWO       ', 'PROBE-THREE     ', 'PROBE-FOUR      '
mynames:
        db      'AAAAAAAAAAAAAAAA', 'BBBBBBBBBBBBBBBB', 'CCCCCCCCCCCCCCCC', 'DDDDDDDDDDDDDDDD'

accbuf: times 144h db 0
setbuf: times 200h db 0
rowbuf: times 32 db 0
strbuf: times 32 db 0

start:
        PROBE_BEGIN "mouse3"

        mov     ax, 3533h
        int     21h
        mov     ax, es
        mov     [__ax], ax
        mov     [__bx], bx
        EMIT    "i33.vector", "AX"
        mov     ax, es
        or      ax, ax
        jnz     .have
        PROBE_END
.have:
        push    cs
        pop     es
        mov     ax, 0003h
        int     10h

        POISON
        xor     ax, ax
        call    m33
        EMIT    "i33.00.reset", "AX,BX"
        cmp     word [__ax], 0FFFFh
        je      .driver
        PROBE_END
.driver:

        ; ================= PART A: the register contracts ===========================
        ; ---- 32h: which of 25h-34h the driver says it services. Bit 15 = 25h ... bit 0
        ; = 34h; 2Bh-2Eh are bits 9-6, 33h bit 1, 34h bit 0.
        POISON
        mov     ax, 0032h
        call    m33
        EMIT    "i33.32.active", "AX"

        ; ---- 2Ch get profiles: AX=0, BX = active, ES:SI -> 144h-byte block.
        push    cs
        pop     es
        POISON
        mov     si, 5151h
        mov     ax, 002Ch
        call    m33
        EMIT    "i33.2C.reset", "AX,BX"
        mov     di, accbuf
        mov     cx, 144h
        call    zero_buf
        cmp     word [__ax], 0
        jne     .no2c
        mov     di, accbuf
        mov     cx, 144h
        call    copy_essi
.no2c:
        EMIT_BUF "i33.2C.reset.lens", accbuf, 4
        EMIT_BUF "i33.2C.reset.thresh1", accbuf+4, 16
        EMIT_BUF "i33.2C.reset.factor1", accbuf+84h, 16
        EMIT_BUF "i33.2C.reset.names", accbuf+104h, 64

        ; ---- 2Dh BX=FFFFh: only ask. AX=0, BX = active, ES:SI -> its name.
        push    cs
        pop     es
        POISON
        mov     si, 5151h
        mov     ax, 002Dh
        mov     bx, 0FFFFh
        call    m33
        EMIT    "i33.2D.query", "AX,BX"
        mov     di, strbuf
        mov     cx, 16
        call    zero_buf
        cmp     word [__ax], 0
        jne     .no2d
        mov     di, strbuf
        mov     cx, 16
        call    copy_essi
.no2d:
        EMIT_BUF "i33.2D.query.name", strbuf, 16

        ; ---- 2Dh BX=2 selects; read back with FFFFh.
        POISON
        mov     ax, 002Dh
        mov     bx, 2
        call    m33
        EMIT    "i33.2D.select2", "AX,BX"
        POISON
        mov     ax, 002Dh
        mov     bx, 0FFFFh
        call    m33
        EMIT    "i33.2D.after2", "AX,BX"

        ; ---- 2Dh BX=7: invalid. RBIL: AX=FFFEh.
        POISON
        mov     ax, 002Dh
        mov     bx, 7
        call    m33
        EMIT    "i33.2D.invalid", "AX,BX"

        ; ---- 2Bh BX=3 loads OUR block and makes profile 3 active; 2Ch reads it back.
        push    cs
        pop     es
        POISON
        mov     si, myacc
        mov     ax, 002Bh
        mov     bx, 3
        call    m33
        EMIT    "i33.2B.load3", "AX"
        push    cs
        pop     es
        POISON
        mov     si, 5151h
        mov     ax, 002Ch
        call    m33
        EMIT    "i33.2C.afterload", "AX,BX"
        mov     di, accbuf
        mov     cx, 144h
        call    zero_buf
        cmp     word [__ax], 0
        jne     .no2c2
        mov     di, accbuf
        mov     cx, 144h
        call    copy_essi
.no2c2:
        EMIT_BUF "i33.2C.afterload.lens", accbuf, 4
        EMIT_BUF "i33.2C.afterload.thresh1", accbuf+4, 4
        EMIT_BUF "i33.2C.afterload.factor1", accbuf+84h, 4
        EMIT_BUF "i33.2C.afterload.name3", accbuf+124h, 16

        ; ---- 2Bh BX=FFFFh restores the default curves.
        POISON
        mov     ax, 002Bh
        mov     bx, 0FFFFh
        call    m33
        EMIT    "i33.2B.default", "AX"
        push    cs
        pop     es
        POISON
        mov     si, 5151h
        mov     ax, 002Ch
        call    m33
        EMIT    "i33.2C.afterdefault", "AX,BX"
        mov     di, accbuf
        mov     cx, 144h
        call    zero_buf
        cmp     word [__ax], 0
        jne     .no2c3
        mov     di, accbuf
        mov     cx, 144h
        call    copy_essi
.no2c3:
        EMIT_BUF "i33.2C.afterdefault.lens", accbuf, 4
        EMIT_BUF "i33.2C.afterdefault.name1", accbuf+104h, 16

        ; ---- 2Eh BL=0: set the names from our buffer; 2Ch reads them back.
        push    cs
        pop     es
        POISON
        mov     si, mynames
        mov     ax, 002Eh
        xor     bx, bx
        call    m33
        EMIT    "i33.2E.set", "AX"
        push    cs
        pop     es
        POISON
        mov     si, 5151h
        mov     ax, 002Ch
        call    m33
        mov     di, accbuf
        mov     cx, 144h
        call    zero_buf
        cmp     word [__ax], 0
        jne     .no2c4
        mov     di, accbuf
        mov     cx, 144h
        call    copy_essi
.no2c4:
        EMIT_BUF "i33.2E.set.names", accbuf+104h, 64

        ; ---- 2Eh BL=1: "fill ES:SI buffer with default names on return". The buffer
        ; is poisoned EEh first, so what the call wrote is visible byte by byte.
        mov     di, setbuf
        mov     cx, 64
        mov     al, 0EEh
        call    fill_buf
        push    cs
        pop     es
        POISON
        mov     si, setbuf
        mov     ax, 002Eh
        mov     bx, 1
        call    m33
        EMIT    "i33.2E.defaults", "AX"
        EMIT_BUF "i33.2E.defaults.buf", setbuf, 64

        ; ---- 33h, a big buffer: AX=0, CX = bytes returned; the 16-byte settings header
        ; and the start of the profile block. Poisoned EEh: the count is visible too.
        mov     di, setbuf
        mov     cx, 200h
        mov     al, 0EEh
        call    fill_buf
        push    cs
        pop     es
        POISON
        mov     ax, 0033h
        mov     cx, 200h
        mov     dx, setbuf
        call    m33
        EMIT    "i33.33.big", "AX,CX"
        EMIT_BUF "i33.33.big.header", setbuf, 16
        EMIT_BUF "i33.33.big.acc", setbuf+10h, 8
        EMIT_BUF "i33.33.big.tail", setbuf+150h, 8

        ; ---- 33h, CX=5: truncated, or refused?
        mov     di, setbuf
        mov     cx, 16
        mov     al, 0EEh
        call    fill_buf
        push    cs
        pop     es
        POISON
        mov     ax, 0033h
        mov     cx, 5
        mov     dx, setbuf
        call    m33
        EMIT    "i33.33.short", "AX,CX"
        EMIT_BUF "i33.33.short.buf", setbuf, 8

        ; ---- 34h: AX=0, ES:DX -> the .INI file name (ASCIIZ). Copied 32 bytes.
        push    cs
        pop     es
        POISON
        mov     ax, 0034h
        call    m33
        EMIT    "i33.34.ini", "AX"
        mov     di, strbuf
        mov     cx, 32
        call    zero_buf
        cmp     word [__ax], 0
        jne     .no34
        mov     di, strbuf
        mov     cx, 32
        call    copy_esdx
.no34:
        EMIT_BUF "i33.34.ini.name", strbuf, 32

        ; ---- 18h: AX=0018h installed, FFFFh refused.
        push    cs
        pop     es
        POISON
        mov     ax, 0018h
        mov     cx, 001Fh                       ; no Shift/Ctrl/Alt bit
        mov     dx, alth
        call    m33
        EMIT    "i33.18.noshift", "AX"
        push    cs
        pop     es
        POISON
        mov     ax, 0018h
        mov     cx, 0022h                       ; Shift + left press
        mov     dx, alth
        call    m33
        EMIT    "i33.18.shift", "AX"
        push    cs
        pop     es
        POISON
        mov     ax, 0018h
        mov     cx, 0044h                       ; Ctrl + left release
        mov     dx, alth
        call    m33
        EMIT    "i33.18.ctrl", "AX"
        push    cs
        pop     es
        POISON
        mov     ax, 0018h
        mov     cx, 0062h                       ; Shift+Ctrl
        mov     dx, alth
        call    m33
        EMIT    "i33.18.shiftctrl", "AX"
        push    cs
        pop     es
        POISON
        mov     ax, 0018h
        mov     cx, 0082h                       ; Alt: a FOURTH combination
        mov     dx, alth
        call    m33
        EMIT    "i33.18.fourth", "AX"
        push    cs
        pop     es
        POISON
        mov     ax, 0018h
        mov     cx, 0026h                       ; Shift again, more events: replace?
        mov     dx, alth
        call    m33
        EMIT    "i33.18.again", "AX"

        ; ---- 19h: BX:DX = the handler, CX = its mask (0 = none). BX is our segment,
        ; which differs per host -- informational; DX (our offset) is the same everywhere.
        POISON
        mov     ax, 0019h
        mov     cx, 0020h
        call    m33
        EMIT    "i33.19.shift", "CX,DX"
        POISON
        mov     ax, 0019h
        mov     cx, 0022h
        call    m33
        EMIT    "i33.19.shift.exact", "CX,DX"
        POISON
        mov     ax, 0019h
        mov     cx, 0080h
        call    m33
        EMIT    "i33.19.alt", "CX"
        POISON
        mov     ax, 0019h
        mov     cx, 0003h
        call    m33
        EMIT    "i33.19.noshift", "CX"
        ; does a reset forget them?
        xor     ax, ax
        call    m33
        POISON
        mov     ax, 0019h
        mov     cx, 0020h
        call    m33
        EMIT    "i33.19.afterreset", "CX"

        ; ================= PART B: mode 13h, the cursor in VRAM =====================
        mov     ax, 0013h
        int     10h
        POISON
        xor     ax, ax
        call    m33
        EMIT    "i33.13h.reset", "AX,BX"
        push    es
        mov     ax, 0A000h
        mov     es, ax
        xor     di, di
        mov     cx, 64000
        mov     al, 0A5h
        cld
        rep     stosb
        pop     es
        mov     ax, 0004h
        mov     cx, 320
        mov     dx, 100
        call    m33
        POISON
        mov     ax, 0003h
        call    m33
        EMIT    "i33.13h.pos", "CX,DX"

        push    cs
        pop     es
        mov     ax, 0009h
        xor     bx, bx
        xor     cx, cx
        mov     dx, gc1
        call    m33
        POISON
        mov     ax, 0025h
        call    m33
        EMIT    "i33.25.13h", "AX"
        mov     ax, 0001h
        call    m33
        call    wait2
        VROW    "i33.09.13h.vram.y099", 99, 152
        VROW    "i33.09.13h.vram.y100", 100, 152
        VROW    "i33.09.13h.vram.y101", 101, 152
        VROW    "i33.09.13h.vram.y102", 102, 152
        VROW    "i33.09.13h.vram.y103", 103, 152
        VROW    "i33.09.13h.vram.y104", 104, 152
        VROW    "i33.09.13h.vram.y105", 105, 152
        VROW    "i33.09.13h.vram.y106", 106, 152
        mov     ax, 0002h
        call    m33
        call    wait2
        VROW    "i33.09.13h.hidden.y101", 101, 152
        VROW    "i33.09.13h.hidden.y102", 102, 152
        VROW    "i33.09.13h.hidden.y103", 103, 152

        ; ---- the hot spot's unit: bitmap 2 (row 2 only), hot spot (4,2).
        push    cs
        pop     es
        mov     ax, 0009h
        mov     bx, 4
        mov     cx, 2
        mov     dx, gc2
        call    m33
        POISON
        mov     ax, 002Ah
        call    m33
        EMIT    "i33.2A.13h.hotspot", "BX,CX"
        mov     ax, 0001h
        call    m33
        call    wait2
        VROW    "i33.09.13h.hot.y099", 99, 150
        VROW    "i33.09.13h.hot.y100", 100, 150
        VROW    "i33.09.13h.hot.y101", 101, 150
        VROW    "i33.09.13h.hot.y102", 102, 150
        mov     ax, 0002h
        call    m33

        mov     ax, 0003h
        int     10h
        PROBE_END
