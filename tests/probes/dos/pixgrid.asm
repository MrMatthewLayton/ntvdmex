; pixgrid.asm -- a one-pixel checkerboard in each graphics mode, for checking that the
;                picture on screen is pixel-perfect (#325).
;
; Every frame pixel alternates white (15) and blue (1) with both neighbours, so in a
; screenshot each frame pixel must be one uniform N x N block and the picture exactly
; N times the mode's resolution (or, where it cannot fit, visibly scaled). A rig script
; screenshots each screen and checks that.
;
; Screens, each waiting for a key (Esc ends early):
;   1  mode 13h            320x200
;   2  Mode X              320x240  (13h, unchained, 480-line CRTC program)
;   3  mode 12h            640x480  16 colours
;   4  mode 0Dh            320x200  16 colours
;   5  VESA 101h           640x480  256 colours, banked
;   6  VESA 103h           800x600  256 colours, banked
;   7  VESA 107h           1280x1024 256 colours, banked
;
; Assemble: nasm -f bin pixgrid.asm -o pixgrid.com   (CMake: build/probes/pixgrid.com)

        org     100h

start:
        ; ── 1: mode 13h ──────────────────────────────────────────────────────────
        mov     ax, 0013h
        int     10h
        call    lin_320x200
        call    waitkey
        jc      done

        ; ── 2: Mode X 320x240 ────────────────────────────────────────────────────
        mov     ax, 0013h
        int     10h
        mov     dx, 3C4h
        mov     ax, 0604h               ; SR4: chain-4 off
        out     dx, ax
        mov     dx, 3D4h
        mov     ax, 0E11h               ; CR11: unprotect CR0-7
        out     dx, ax
        mov     si, modex_crtc
.mx:    lodsw
        or      ax, ax
        jz      .mxd
        out     dx, ax
        jmp     .mx
.mxd:   mov     ax, 0A000h
        mov     es, ax
        xor     cx, cx                  ; cl = plane
.plane: mov     dx, 3C4h
        mov     al, 02h
        mov     ah, 1
        shl     ah, cl                  ; map mask = 1 << plane
        out     dx, ax
        xor     bx, bx                  ; bx = y
.my:    mov     ax, bx
        mov     dx, 80
        mul     dx
        mov     di, ax                  ; di = y * 80
        mov     si, cx                  ; si = x, from the plane, step 4
.mxl:   mov     ax, si
        xor     ax, bx
        test    al, 1
        mov     al, 1
        jz      .mxc
        mov     al, 15
.mxc:   stosb
        add     si, 4
        cmp     si, 320
        jb      .mxl
        inc     bx
        cmp     bx, 240
        jb      .my
        inc     cl
        cmp     cl, 4
        jb      .plane
        call    waitkey
        jc      done

        ; ── 3: mode 12h, 640x480x16 ──────────────────────────────────────────────
        mov     ax, 0012h
        int     10h
        mov     bp, 480
        mov     bx, 80
        call    planar_grid
        call    waitkey
        jc      done

        ; ── 4: mode 0Dh, 320x200x16 ──────────────────────────────────────────────
        mov     ax, 000Dh
        int     10h
        mov     bp, 200
        mov     bx, 40
        call    planar_grid
        call    waitkey
        jc      done

        ; ── 5-7: VESA, banked 256-colour ─────────────────────────────────────────
        mov     cx, 0101h
        mov     word [vw], 640
        mov     word [vh], 480
        call    vesa_grid
        jc      done
        mov     cx, 0103h
        mov     word [vw], 800
        mov     word [vh], 600
        call    vesa_grid
        jc      done
        mov     cx, 0107h
        mov     word [vw], 1280
        mov     word [vh], 1024
        call    vesa_grid

done:   mov     ax, 0003h
        int     10h
        mov     ax, 4C00h
        int     21h

; Wait for a key; CF set if it was Esc.
waitkey:
        xor     ax, ax
        int     16h
        cmp     al, 1Bh
        je      .esc
        clc
        ret
.esc:   stc
        ret

; mode 13h: 64000 bytes at A000:0, colour by (x ^ y) & 1.
lin_320x200:
        mov     ax, 0A000h
        mov     es, ax
        xor     di, di
        xor     bx, bx                  ; y
.y:     xor     si, si                  ; x
.x:     mov     ax, si
        xor     ax, bx
        test    al, 1
        mov     al, 1
        jz      .c
        mov     al, 15
.c:     stosb
        inc     si
        cmp     si, 320
        jb      .x
        inc     bx
        cmp     bx, 200
        jb      .y
        ret

; Planar 16-colour: plane 0 all ones (colours 1 and 15 both have bit 0), planes 1-3
; the checker (set = white), alternating AAh / 55h per row. BP = rows, BX = bytes/row.
planar_grid:
        mov     ax, 0A000h
        mov     es, ax
        mov     ax, bp
        mul     bx                      ; (clobbers DX -- before the port is loaded)
        mov     cx, ax
        mov     dx, 3C4h
        mov     ax, 0102h               ; map mask: plane 0
        out     dx, ax
        xor     di, di
        mov     al, 0FFh
        rep     stosb
        mov     ax, 0E02h               ; map mask: planes 1-3
        out     dx, ax
        xor     di, di
        xor     dx, dx                  ; dx = y
.row:   mov     al, 0AAh
        test    dl, 1
        jz      .r0
        mov     al, 55h
.r0:    mov     cx, bx
        rep     stosb
        inc     dx
        cmp     dx, bp
        jb      .row
        mov     dx, 3C4h
        mov     ax, 0F02h
        out     dx, ax
        ret

; VESA mode CX at [vw] x [vh], 8bpp, through window A in 64 KB steps of the window's
; granularity. CF set on Esc (or if the mode was refused, which also ends the run).
vesa_grid:
        push    cx
        mov     ax, 4F01h               ; mode info -> granularity at +4 (KB)
        mov     di, modeinfo
        push    ds
        pop     es
        int     10h
        pop     bx
        cmp     ax, 004Fh
        jne     .bad
        mov     ax, 4F02h
        int     10h
        cmp     ax, 004Fh
        jne     .bad
        mov     ax, 64
        mov     cx, [modeinfo + 4]
        or      cx, cx
        jz      .g
        xor     dx, dx
        div     cx                      ; banks per 64 KB
.g:     mov     [bstep], ax
        mov     word [bank], 0
        call    setbank
        mov     ax, 0A000h
        mov     es, ax
        xor     di, di
        xor     bx, bx                  ; y
.y:     xor     si, si                  ; x
.x:     mov     ax, si
        xor     ax, bx
        test    al, 1
        mov     al, 1
        jz      .c
        mov     al, 15
.c:     stosb
        or      di, di
        jnz     .nb
        mov     ax, [bstep]             ; DI wrapped: next 64 KB
        add     [bank], ax
        call    setbank
.nb:    inc     si
        cmp     si, [vw]
        jb      .x
        inc     bx
        cmp     bx, [vh]
        jb      .y
        jmp     waitkey
.bad:   stc
        ret

setbank:
        push    bx
        mov     ax, 4F05h
        xor     bx, bx
        mov     dx, [bank]
        int     10h
        pop     bx
        ret

modex_crtc:                             ; the classic 320x240 CRTC program
        dw      0D06h, 3E07h, 4109h, 0EA10h, 0AC11h, 0DF12h, 0014h, 0E715h, 0616h, 0E317h, 0
vw      dw      0
vh      dw      0
bank    dw      0
bstep   dw      1
modeinfo times 256 db 0
