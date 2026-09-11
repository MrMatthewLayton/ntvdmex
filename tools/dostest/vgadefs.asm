; vgadefs.com -- WHAT DOES A MODE SET LEAVE IN THE ATTRIBUTE CONTROLLER AND THE DAC?
;
; This exists because card 11 answered that question from a PICTURE and got it
; wrong.  The card reprogrammed DAC 0..15 to a ramp, left the AC alone, and the
; bars were read off the screen; from that the AC default table was concluded to
; be identity for the low eight -- index 6 = 6 rather than the 0x14 every
; reference quotes -- and VPAL_DEFAULT was changed to match.
;
; Two things were wrong with that.  The card ran in mode 0Dh while the guest that
; prompted it (Lemmings) plays in mode 10h, and the AC defaults come from the
; per-mode video parameter table, so the two need not agree.  And a bar's colour
; is a reading of the WHOLE chain -- AC, DAC, renderer -- so it cannot say which
; link differs.  This probe reads the registers themselves.
;
; For each mode it sets the mode, reads the sixteen AC palette registers back
; through 0x3C1, reads all 256 DAC entries back through 0x3C7/0x3C9, returns to
; mode 3 and prints.  0x8D and 0x90 are the same modes with bit 7 set ("do not
; clear the buffer") -- which is how Lemmings sets them, and the probe shows
; whether that bit changes the palette load as well.
;
;   nasm -f bin vgadefs.asm -o vgadefs.com
;   scripts/dosoracle/dosoracle.py run tools/dostest/vgadefs.com

        org     100h

; ⚠ The mode list is walked through a memory pointer, not SI: puts() uses SI, so
; a lodsb cursor here is silently destroyed by the first thing printed.
start:
        mov     word [mp], modes
.next:
        mov     bx, [mp]
        mov     al, [bx]
        inc     word [mp]
        cmp     al, 0FFh        ; ⚠ not 0: mode 00h is a real mode to measure
        je      .done
        mov     [curmode], al

        xor     ah, ah                  ; ---- INT 10h AH=00: set mode
        int     10h

        ; ---- the sixteen AC palette registers, via 0x3C0 index / 0x3C1 data.
        ; Bit 5 of the index is video-enable, not part of the register number, so
        ; the index written here blanks the screen; 0x20 at the end restores it.
        ; The 0x3DA read before every index write resets the address flip-flop --
        ; assuming its state across an iteration is how this goes subtly wrong.
        xor     cx, cx
.ac:
        mov     dx, 3DAh
        in      al, dx
        mov     dx, 3C0h
        mov     al, cl
        out     dx, al
        mov     dx, 3C1h
        in      al, dx
        mov     bx, cx
        mov     [acbuf + bx], al
        inc     cx
        cmp     cx, 16
        jb      .ac
        mov     dx, 3DAh
        in      al, dx
        mov     dx, 3C0h
        mov     al, 20h                 ; video back on
        out     dx, al

        ; ---- all 256 DAC entries, three 6-bit components each. All 256 and not
        ; just the 64 an AC register can reach, because mode 13h's default
        ; palette is the interesting one up there and we seed a grey ramp.
        mov     dx, 3C7h
        xor     al, al
        out     dx, al
        mov     dx, 3C9h
        mov     cx, 256 * 3
        xor     bx, bx
.dac:
        in      al, dx
        mov     [dacbuf + bx], al
        inc     bx
        loop    .dac

        ; ---- CRTC registers 00..18. A mode set reprograms the whole CRTC, which is
        ; why these belong next to the palette: the host used to leave crtc_offset
        ; and crtc_start alone across a mode set, so a screen inherited the geometry
        ; of the one before it. Lemmings' gameplay sets offset=22 and the NEXT mode
        ; 10h screen was then drawn 44 bytes to the line instead of 80.
        ; ⚠ The index port is 3D4 on a colour adapter and 3B4 on mono; the BIOS keeps
        ; the live one at 0040:0063, so take it from there rather than assume.
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     dx, [463h]
        pop     ds
        mov     [crtcport], dx
        xor     cx, cx
.crtc:
        mov     dx, [crtcport]
        mov     al, cl
        out     dx, al
        inc     dx
        in      al, dx
        mov     bx, cx
        mov     [crtcbuf + bx], al
        inc     cx
        cmp     cx, 25
        jb      .crtc

        mov     ax, 0003h               ; text mode before printing
        int     10h

        mov     si, s_mode
        call    puts
        mov     al, [curmode]
        call    puthex
        mov     si, s_ac
        call    puts
        xor     bx, bx
.pac:
        mov     al, [acbuf + bx]
        call    puthex
        mov     al, ' '
        call    putc
        inc     bx
        cmp     bx, 16
        jb      .pac
        call    crlf

        mov     si, s_crtc
        call    puts
        xor     bx, bx
.pcrtc:
        mov     al, [crtcbuf + bx]
        call    puthex
        mov     al, ' '
        call    putc
        inc     bx
        cmp     bx, 25
        jb      .pcrtc
        call    crlf

        ; DAC, eight entries to a line, each line prefixed with its first index.
        ; DI counts ENTRIES and BX counts BYTES, so neither has to be divided back
        ; out of the other -- the first cut of this derived one from the other with
        ; a div and got the line breaks wrong.
        xor     di, di
        xor     bx, bx
.pdac:
        test    di, 7
        jnz     .nohdr
        mov     si, s_dac
        call    puts
        mov     ax, di
        call    puthex
        mov     al, '='
        call    putc
.nohdr:
        mov     al, [dacbuf + bx]
        call    puthex
        inc     bx
        mov     al, [dacbuf + bx]
        call    puthex
        inc     bx
        mov     al, [dacbuf + bx]
        call    puthex
        inc     bx
        mov     al, ' '
        call    putc
        inc     di
        cmp     di, 256
        jb      .pdac
        call    crlf
        jmp     .next
.done:
        mov     ax, 4C00h
        int     21h

; ---------------------------------------------------------------- print helpers
putc:
        push    ax
        push    dx
        mov     dl, al
        mov     ah, 02h
        int     21h
        pop     dx
        pop     ax
        ret

puthex:
        push    ax
        push    ax
        shr     al, 1
        shr     al, 1
        shr     al, 1
        shr     al, 1
        call    .nyb
        pop     ax
        and     al, 0Fh
        call    .nyb
        pop     ax
        ret
.nyb:
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     .ok
        add     al, 7
.ok:
        jmp     putc

puts:
        push    ax
.l:
        lodsb
        or      al, al
        jz      .e
        call    putc
        jmp     .l
.e:
        pop     ax
        ret

crlf:
        push    ax
        mov     al, 13
        call    putc
        mov     al, 10
        call    putc
        pop     ax
        ret

; ---------------------------------------------------------------------- data
mp      dw      0
modes   db      000h, 001h, 002h, 003h, 004h, 005h, 006h, 007h
        db      00Dh, 00Eh, 00Fh, 010h, 011h, 012h, 013h
        db      08Dh, 090h      ; the same modes with bit 7 set
        db      0FFh
curmode db      0
s_mode  db      'MODE=', 0
s_ac    db      ' AC=', 0
s_dac   db      13, 10, 'DAC', 0
s_crtc  db      'CRTC=', 0
crtcport dw     3D4h
crtcbuf times 25 db 0
acbuf   times 16 db 0
dacbuf  times 256*3 db 0
