; vesacube.com -- a VESA exerciser: mode selector + a tumbling, bouncing 3-D cube.
;
; What it drives, deliberately, so a VESA implementation is judged by what a guest
; DOES with it rather than by what it says:
;   4F00 (VBE2 block, mode list)   4F01 (every mode: geometry, depth, model, window)
;   4F02 (banked set)              4F03 (read-back after set)
;   4F05 (bank switch on EVERY crossing -- the cube walks the whole framebuffer)
;   pixel formats 8 (palette), 15 (5:5:5), 16 (5:6:5), 24 (B,G,R)
;   4F06 get (the pitch the BIOS reports is the pitch we draw with)
;
; Real mode, 386 instructions, banked windows only (the LFB needs a protected-mode
; or unreal-mode client; that is heaven7's job).  Draws with a bank-aware byte
; writer, erases the previous frame's edges rather than clearing the screen, and
; paces frames on the BIOS tick (see vsync for why not 3DAh).
;
; Usage:  VESACUBE            menu: pick a mode by letter, ESC leaves the cube,
;                             ESC again quits
;         VESACUBE 101        run mode 101h straight away for ~12 s, then exit
;                             (headless: the rig harness has no keyboard)
;
; nasm -f bin vesacube.asm -o vesacube.com

        cpu     386
        org     100h

MAXMODES equ    24
CUBE     equ    1               ; half-edge, in "units" scaled below
NEDGES   equ    12
RUNTICKS equ    220             ; ~12 s at 18.2 Hz for the headless form

start:
        cld
        call    parse_arg               ; [argmode] = 0 or a mode number
        call    enum_modes
        cmp     word [nmodes], 0
        jne     .have
        mov     si, msg_novesa
        call    puts
        jmp     exit
.have:
        cmp     word [argmode], 0
        je      .menu
        ; headless: find the argument in the table, run it for RUNTICKS, exit
        mov     ax, [argmode]
        call    find_mode
        jc      .menu
        mov     word [runlimit], RUNTICKS
        call    run_cube
        jmp     exit
.menu:
        mov     word [runlimit], 0
        call    show_menu
        call    getkey
        cmp     al, 27
        je      exit
        or      al, 20h                 ; letter -> lower case
        sub     al, 'a'
        jb      .menu
        movzx   bx, al
        cmp     bx, [nmodes]
        jae     .menu
        mov     [cur], bx
        call    run_cube
        jmp     .menu

exit:
        mov     ax, 0003h
        int     10h
        mov     ax, 4C00h
        int     21h

; ───────────────────────────────────────────────────────── command line
parse_arg:
        mov     word [argmode], 0
        mov     si, 81h
        movzx   cx, byte [80h]
        jcxz    .done
.skip:  lodsb
        cmp     al, ' '
        jne     .hex
        loop    .skip
        jmp     .done
.hex:   xor     bx, bx
.h:     cmp     al, '0'
        jb      .end
        cmp     al, '9'
        jbe     .dig
        or      al, 20h
        cmp     al, 'a'
        jb      .end
        cmp     al, 'f'
        ja      .end
        sub     al, 'a'-10
        jmp     .acc
.dig:   sub     al, '0'
.acc:   shl     bx, 4
        or      bl, al
        dec     cx
        jz      .end
        lodsb
        jmp     .h
.end:   mov     [argmode], bx
.done:  ret

; ───────────────────────────────────────────────────────── VESA enumeration
; Fills modes[] with every SUPPORTED, GRAPHICS, packed/direct mode of 8/15/16/24 bpp
; that has a usable window A -- what this program can actually draw in.
enum_modes:
        mov     word [nmodes], 0
        push    ds
        pop     es
        mov     di, vbeinfo
        mov     dword [di], 'VBE2'
        mov     ax, 4F00h
        int     10h
        cmp     ax, 004Fh
        jne     .ret
        lds     si, [vbeinfo+14]                ; VideoModePtr
.next:  lodsw
        cmp     ax, 0FFFFh
        je      .fin
        push    ds
        push    si
        push    cs
        pop     ds
        call    consider_mode
        pop     si
        pop     ds
        cmp     word [cs:nmodes], MAXMODES
        jb      .next
.fin:   push    cs
        pop     ds
.ret:   ret

consider_mode:                                  ; AX = mode number (DS = CS)
        mov     [tmpmode], ax
        mov     di, modeinfo
        push    di
        mov     cx, 256
        xor     al, al
        rep     stosb
        pop     di
        mov     cx, [tmpmode]
        mov     ax, 4F01h
        int     10h
        cmp     ax, 004Fh
        jne     .no
        test    byte [modeinfo+0], 10h          ; graphics?
        jz      .no
        test    byte [modeinfo+0], 1
        jz      .no
        test    byte [modeinfo+2], 0101b        ; window A exists + writable
        jz      .no
        mov     al, [modeinfo+27]               ; MemoryModel: 4 packed, 6 direct
        cmp     al, 4
        je      .model_ok
        cmp     al, 6
        jne     .no
.model_ok:
        mov     al, [modeinfo+25]               ; bpp
        cmp     al, 8
        je      .bpp_ok
        cmp     al, 15
        je      .bpp_ok
        cmp     al, 16
        je      .bpp_ok
        cmp     al, 24
        jne     .no
.bpp_ok:
        cmp     word [modeinfo+18], 1280        ; keep the table sane
        ja      .no
        ; record
        mov     bx, [nmodes]
        imul    bx, bx, MODESZ
        add     bx, modes
        mov     ax, [tmpmode]
        mov     [bx+M_NUM], ax
        mov     ax, [modeinfo+18]
        mov     [bx+M_W], ax
        mov     ax, [modeinfo+20]
        mov     [bx+M_H], ax
        mov     al, [modeinfo+25]
        mov     [bx+M_BPP], al
        mov     ax, [modeinfo+16]
        mov     [bx+M_PITCH], ax
        mov     ax, [modeinfo+4]                ; granularity KB
        mov     [bx+M_GRAN], ax
        mov     ax, [modeinfo+8]
        mov     [bx+M_SEG], ax
        mov     al, [modeinfo+0]
        mov     [bx+M_ATTR], al
        inc     word [nmodes]
.no:    ret

find_mode:                                      ; AX = mode -> [cur], CF=1 if absent
        xor     bx, bx
.l:     cmp     bx, [nmodes]
        jae     .nf
        imul    si, bx, MODESZ
        cmp     [modes+si+M_NUM], ax
        je      .f
        inc     bx
        jmp     .l
.f:     mov     [cur], bx
        clc
        ret
.nf:    stc
        ret

; ───────────────────────────────────────────────────────── menu
show_menu:
        mov     ax, 0003h
        int     10h
        mov     si, msg_title
        call    puts
        xor     bx, bx
.row:   cmp     bx, [nmodes]
        jae     .end
        mov     al, 'A'
        add     al, bl
        call    putc
        mov     al, ')'
        call    putc
        mov     al, ' '
        call    putc
        imul    si, bx, MODESZ
        add     si, modes
        mov     ax, [si+M_NUM]
        call    puthex16
        mov     al, ' '
        call    putc
        mov     al, ' '
        call    putc
        mov     ax, [si+M_W]
        call    putdec
        mov     al, 'x'
        call    putc
        mov     ax, [si+M_H]
        call    putdec
        mov     al, 'x'
        call    putc
        movzx   ax, byte [si+M_BPP]
        call    putdec
        push    si
        mov     si, msg_pitch
        call    puts
        pop     si
        mov     ax, [si+M_PITCH]
        call    putdec
        test    byte [si+M_ATTR], 80h
        jz      .nolfb
        push    si
        mov     si, msg_lfb
        call    puts
        pop     si
.nolfb: mov     si, msg_crlf
        call    puts
        inc     bx
        jmp     .row
.end:   mov     si, msg_prompt
        call    puts
        ret

; ───────────────────────────────────────────────────────── the cube
run_cube:
        mov     bx, [cur]
        imul    bx, bx, MODESZ
        add     bx, modes
        mov     ax, [bx+M_NUM]
        and     ax, 3FFFh                       ; banked, and clear the screen
        push    bx
        mov     bx, ax
        mov     ax, 4F02h
        int     10h
        pop     bx
        cmp     ax, 004Fh
        jne     .fail
        push    bx                              ; 4F03 answers in BX -- our record pointer
        mov     ax, 4F03h                       ; read back: the set must stick
        int     10h
        pop     bx
        ; geometry for this mode
        mov     ax, [bx+M_W]
        mov     [scrw], ax
        mov     ax, [bx+M_H]
        mov     [scrh], ax
        movzx   eax, word [bx+M_PITCH]
        mov     [pitch], eax
        mov     ax, [bx+M_SEG]
        mov     [winseg], ax
        movzx   eax, word [bx+M_GRAN]
        shl     eax, 10                         ; KB -> bytes
        mov     [granb], eax
        mov     al, [bx+M_BPP]
        mov     [bpp], al
        xor     ah, ah
        add     al, 7
        shr     al, 3
        mov     [bypp], al                      ; 1, 2, 3
        ; 4F06 BL=1: the BIOS's own pitch wins over the mode block's
        mov     ax, 4F06h
        mov     bl, 1
        int     10h
        cmp     ax, 004Fh
        jne     .nopitch
        movzx   eax, bx
        mov     [pitch], eax
.nopitch:
        mov     word [curbank], 0FFFFh
        call    build_colours
        ; cube size: 1/5 of the smaller dimension; start centred, moving
        mov     ax, [scrw]
        cmp     ax, [scrh]
        jbe     .sm
        mov     ax, [scrh]
.sm:    xor     dx, dx
        mov     cx, 5
        div     cx
        mov     [size], ax
        mov     ax, [scrw]
        shr     ax, 1
        shl     ax, 4
        mov     [posx], ax                      ; 12.4 fixed
        mov     ax, [scrh]
        shr     ax, 1
        shl     ax, 4
        mov     [posy], ax
        mov     word [velx], 5*16/2
        mov     word [vely], 3*16/2
        mov     word [ang0], 0
        mov     word [ang1], 40
        mov     word [ang2], 80
        mov     byte [haveold], 0
        ; tick base for the headless form
        mov     ah, 0
        int     1Ah
        mov     [tick0], dx
.frame:
        call    step
        call    project
        call    vsync
        call    erase_old
        call    draw_new
        call    keep_old
        ; exit conditions
        cmp     word [runlimit], 0
        je      .keys
        mov     ah, 0
        int     1Ah
        sub     dx, [tick0]
        cmp     dx, [runlimit]
        jb      .frame
        ret
.keys:  mov     ah, 01h
        int     16h
        jz      .frame
        mov     ah, 00h
        int     16h
        cmp     al, 27
        jne     .frame
        ret
.fail:  mov     ax, 0003h
        int     10h
        mov     si, msg_setfail
        call    puts
        call    getkey
        ret

; per-edge colours in this mode's pixel format: 12 dwords
build_colours:
        xor     ebx, ebx
.e:     mov     al, [rgb_r+bx]
        mov     ah, [rgb_g+bx]
        mov     dl, [rgb_b+bx]
        cmp     byte [bpp], 8
        jne     .n8
        movzx   eax, byte [pal8+bx]
        jmp     .st
.n8:    cmp     byte [bpp], 15
        jne     .n15
        ; 5:5:5  r<<10 | g<<5 | b
        movzx   ecx, al
        shr     cx, 3
        shl     cx, 10
        movzx   esi, ah
        shr     si, 3
        shl     si, 5
        or      cx, si
        movzx   esi, dl
        shr     si, 3
        or      cx, si
        mov     eax, ecx
        jmp     .st
.n15:   cmp     byte [bpp], 16
        jne     .n16
        movzx   ecx, al
        shr     cx, 3
        shl     cx, 11
        movzx   esi, ah
        shr     si, 2
        shl     si, 5
        or      cx, si
        movzx   esi, dl
        shr     si, 3
        or      cx, si
        mov     eax, ecx
        jmp     .st
.n16:   ; 24: byte order B,G,R in memory -> value R<<16 | G<<8 | B
        movzx   ecx, al
        shl     ecx, 16
        movzx   esi, ah
        shl     esi, 8
        or      ecx, esi
        movzx   esi, dl
        or      ecx, esi
        mov     eax, ecx
.st:    mov     [colours+ebx*4], eax
        inc     ebx
        cmp     ebx, NEDGES
        jb      .e
        ret

; ── motion
step:
        add     word [ang0], 3
        add     word [ang1], 5
        add     word [ang2], 2
        mov     ax, [posx]
        add     ax, [velx]
        mov     [posx], ax
        mov     ax, [posy]
        add     ax, [vely]
        mov     [posy], ax
        ; bounce: keep centre within [size*1.8, dim - size*1.8]
        mov     cx, [size]
        mov     ax, cx
        shl     ax, 1                           ; size*2 (a touch generous)
        shl     ax, 4                           ; 12.4
        mov     bx, [posx]
        cmp     bx, ax
        jge     .xl
        neg     word [velx]
        mov     [posx], ax
.xl:    mov     dx, [scrw]
        shl     dx, 4
        sub     dx, ax
        cmp     bx, dx
        jle     .xr
        neg     word [velx]
        mov     [posx], dx
.xr:    mov     bx, [posy]
        cmp     bx, ax
        jge     .yt
        neg     word [vely]
        mov     [posy], ax
.yt:    mov     dx, [scrh]
        shl     dx, 4
        sub     dx, ax
        cmp     bx, dx
        jle     .yb
        neg     word [vely]
        mov     [posy], dx
.yb:    ret

; ── rotate + project the 8 corners into newpts (x,y words)
project:
        ; sin/cos of the three angles, 2.14 fixed
        mov     bx, [ang0]
        call    sincos
        mov     [s0], ax
        mov     [c0], dx
        mov     bx, [ang1]
        call    sincos
        mov     [s1], ax
        mov     [c1], dx
        mov     bx, [ang2]
        call    sincos
        mov     [s2], ax
        mov     [c2], dx
        xor     edi, edi                        ; corner index (scaled-index form below: full EDI)
.c:     ; unit corner (+-1) scaled to size, as 16-bit ints
        mov     si, di
        shl     si, 1
        add     si, di                          ; si = di*3
        movsx   ax, byte [corners+si]
        imul    ax, [size]
        mov     [vx], ax
        movsx   ax, byte [corners+si+1]
        imul    ax, [size]
        mov     [vy], ax
        movsx   ax, byte [corners+si+2]
        imul    ax, [size]
        mov     [vz], ax
        ; rotate about X: y' = y*c0 - z*s0 ; z' = y*s0 + z*c0
        movsx   eax, word [vy]
        movsx   ecx, word [c0]
        imul    eax, ecx
        movsx   ebx, word [vz]
        movsx   ecx, word [s0]
        imul    ebx, ecx
        sub     eax, ebx
        sar     eax, 14
        mov     [ty], ax
        movsx   eax, word [vy]
        movsx   ecx, word [s0]
        imul    eax, ecx
        movsx   ebx, word [vz]
        movsx   ecx, word [c0]
        imul    ebx, ecx
        add     eax, ebx
        sar     eax, 14
        mov     [vz], ax
        mov     ax, [ty]
        mov     [vy], ax
        ; rotate about Y: x' = x*c1 + z*s1 ; z' = -x*s1 + z*c1
        movsx   eax, word [vx]
        movsx   ecx, word [c1]
        imul    eax, ecx
        movsx   ebx, word [vz]
        movsx   ecx, word [s1]
        imul    ebx, ecx
        add     eax, ebx
        sar     eax, 14
        mov     [tx], ax
        movsx   eax, word [vx]
        movsx   ecx, word [s1]
        imul    eax, ecx
        neg     eax
        movsx   ebx, word [vz]
        movsx   ecx, word [c1]
        imul    ebx, ecx
        add     eax, ebx
        sar     eax, 14
        mov     [vz], ax
        mov     ax, [tx]
        mov     [vx], ax
        ; rotate about Z: x' = x*c2 - y*s2 ; y' = x*s2 + y*c2
        movsx   eax, word [vx]
        movsx   ecx, word [c2]
        imul    eax, ecx
        movsx   ebx, word [vy]
        movsx   ecx, word [s2]
        imul    ebx, ecx
        sub     eax, ebx
        sar     eax, 14
        mov     [tx], ax
        movsx   eax, word [vx]
        movsx   ecx, word [s2]
        imul    eax, ecx
        movsx   ebx, word [vy]
        movsx   ecx, word [c2]
        imul    ebx, ecx
        add     eax, ebx
        sar     eax, 14
        mov     [vy], ax
        mov     ax, [tx]
        mov     [vx], ax
        ; perspective: screen = centre + v * F / (z + D), D = 4*size, F = 3*size
        mov     cx, [size]
        mov     ax, cx
        shl     ax, 2
        add     ax, [vz]                        ; z + D  (always > 0)
        mov     bx, ax
        mov     ax, cx
        imul    dx, ax, 3
        mov     ax, dx                          ; F
        movsx   eax, ax
        movsx   ecx, word [vx]
        imul    eax, ecx
        movsx   ebx, bx
        cdq
        idiv    ebx
        mov     cx, [posx]
        sar     cx, 4
        add     ax, cx
        mov     [newpts+edi*4], ax
        mov     ax, [size]
        imul    dx, ax, 3
        movsx   eax, dx
        movsx   ecx, word [vy]
        imul    eax, ecx
        cdq
        idiv    ebx
        mov     cx, [posy]
        sar     cx, 4
        add     ax, cx
        mov     [newpts+edi*4+2], ax
        inc     edi
        cmp     edi, 8
        jb      .c
        ret

sincos:                                         ; BX = angle -> AX = sin, DX = cos (2.14)
        and     bx, 0FFh
        shl     bx, 1
        mov     ax, [sintab+bx]
        add     bx, 128                         ; +90 degrees
        and     bx, 1FFh
        mov     dx, [sintab+bx]
        ret

; ── drawing
erase_old:
        cmp     byte [haveold], 0
        je      .r
        mov     dword [colour], 0
        mov     si, oldpts
        call    draw_edges_flat
.r:     ret

draw_new:
        mov     esi, newpts
        xor     ebx, ebx
.e:     push    ebx
        mov     eax, [colours+ebx*4]
        mov     [colour], eax
        mov     al, [edges+ebx*2]
        movzx   edi, al
        mov     ax, [esi+edi*4]
        mov     [x0], ax
        mov     ax, [esi+edi*4+2]
        mov     [y0], ax
        mov     al, [edges+ebx*2+1]
        movzx   edi, al
        mov     ax, [esi+edi*4]
        mov     [x1], ax
        mov     ax, [esi+edi*4+2]
        mov     [y1], ax
        call    line
        pop     ebx
        inc     ebx
        cmp     ebx, NEDGES
        jb      .e
        ret

draw_edges_flat:                                ; SI = point table, [colour] set
        movzx   esi, si
        xor     ebx, ebx
.e:     push    ebx
        mov     al, [edges+ebx*2]
        movzx   edi, al
        mov     ax, [esi+edi*4]
        mov     [x0], ax
        mov     ax, [esi+edi*4+2]
        mov     [y0], ax
        mov     al, [edges+ebx*2+1]
        movzx   edi, al
        mov     ax, [esi+edi*4]
        mov     [x1], ax
        mov     ax, [esi+edi*4+2]
        mov     [y1], ax
        call    line
        pop     ebx
        inc     ebx
        cmp     ebx, NEDGES
        jb      .e
        ret

keep_old:
        mov     si, newpts
        mov     di, oldpts
        mov     cx, 16
        push    ds
        pop     es
        rep     movsw
        mov     byte [haveold], 1
        ret

; Bresenham from (x0,y0) to (x1,y1) in [colour]
line:
        mov     ax, [x1]
        sub     ax, [x0]
        mov     word [sx], 1
        jge     .dxok
        neg     ax
        mov     word [sx], -1
.dxok:  mov     [ldx], ax
        mov     ax, [y1]
        sub     ax, [y0]
        mov     word [sy], 1
        jge     .dyok
        neg     ax
        mov     word [sy], -1
.dyok:  mov     [ldy], ax
        mov     ax, [ldx]
        sub     ax, [ldy]
        mov     [err], ax                       ; err = dx - dy
        mov     cx, [x0]
        mov     dx, [y0]
.p:     call    plot                            ; CX,DX
        cmp     cx, [x1]
        jne     .go
        cmp     dx, [y1]
        je      .done
.go:    mov     ax, [err]
        shl     ax, 1                           ; e2 = 2*err
        mov     bx, [ldy]
        neg     bx
        cmp     ax, bx                          ; e2 > -dy ?
        jle     .noy
        mov     bx, [ldy]
        sub     [err], bx
        add     cx, [sx]
.noy:   mov     bx, [ldx]
        cmp     ax, bx                          ; e2 < dx ?
        jge     .p
        add     [err], bx
        add     dx, [sy]
        jmp     .p
.done:  ret

; plot pixel CX,DX with [colour]; clips; bank-aware byte writes
plot:
        cmp     cx, 0
        jl      .out
        cmp     dx, 0
        jl      .out
        cmp     cx, [scrw]
        jae     .out
        cmp     dx, [scrh]
        jae     .out
        pushad
        movzx   eax, dx
        imul    eax, [pitch]
        movzx   ebx, cx
        movzx   ecx, byte [bypp]
        imul    ebx, ecx
        add     eax, ebx                        ; byte offset
        mov     edx, [colour]
.b:     call    putbyte                         ; writes DL at EAX, then EAX++
        shr     edx, 8
        loop    .b
        popad
.out:   ret

; write DL at framebuffer byte offset EAX (bank-switching as needed); EAX += 1
putbyte:
        push    eax
        push    edx
        push    ebx
        push    ecx
        mov     bl, dl
        xor     edx, edx
        div     dword [granb]                   ; EAX = bank, EDX = offset in window
        cmp     ax, [curbank]
        je      .same
        mov     [curbank], ax
        push    edx
        mov     dx, ax
        mov     ax, 4F05h
        xor     bh, bh                          ; BH=0 set, BL=0 window A
        push    bx
        xor     bl, bl
        int     10h
        pop     bx
        pop     edx
.same:  push    es
        mov     ax, [winseg]
        mov     es, ax
        mov     [es:edx], bl
        pop     es
        pop     ecx
        pop     ebx
        pop     edx
        pop     eax
        inc     eax
        ret

; Frame pacing: wait for the BIOS tick (18.2 Hz) to change. ⚠ Polling 3DAh for the
; vertical retrace was tried first and spins for ~half a second per edge under the
; NTVDMEX headless harness (every IN is reflected; ~2 edges/s measured), so the cube
; would crawl. The tick is what DOS games that do not race the beam use anyway.
vsync:
        push    es
        push    ax
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:046Ch]
.w:     cmp     ax, [es:046Ch]
        je      .w
        pop     ax
        pop     es
        ret

; ───────────────────────────────────────────────────────── text helpers
puts:   lodsb
        or      al, al
        jz      .r
        call    putc
        jmp     puts
.r:     ret
putc:   push    bx
        mov     ah, 0Eh
        mov     bx, 0007h
        int     10h
        pop     bx
        ret
puthex16:
        push    cx
        mov     cx, 4
.d:     rol     ax, 4
        push    ax
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     .o
        add     al, 7
.o:     call    putc
        pop     ax
        loop    .d
        pop     cx
        ret
putdec:                                         ; AX unsigned
        push    bx
        push    cx
        push    dx
        xor     cx, cx
        mov     bx, 10
.s:     xor     dx, dx
        div     bx
        push    dx
        inc     cx
        test    ax, ax
        jnz     .s
.p:     pop     ax
        add     al, '0'
        call    putc
        loop    .p
        pop     dx
        pop     cx
        pop     bx
        ret
getkey: mov     ah, 00h
        int     16h
        ret

; ───────────────────────────────────────────────────────── data
msg_title:  db 'VESACUBE -- NTVDMEX VESA exerciser', 13, 10
            db 'banked graphics modes this BIOS offers (4F00/4F01):', 13, 10, 13, 10, 0
msg_pitch:  db '  pitch ', 0
msg_lfb:    db '  [LFB]', 0
msg_crlf:   db 13, 10, 0
msg_prompt: db 13, 10, 'letter = run that mode (ESC returns here), ESC = quit', 13, 10, 0
msg_novesa: db 'no VESA BIOS answered 4F00', 13, 10, 0
msg_setfail: db 'mode set (4F02) refused -- any key', 13, 10, 0

corners:    db -1,-1,-1,  1,-1,-1,  1, 1,-1, -1, 1,-1
            db -1,-1, 1,  1,-1, 1,  1, 1, 1, -1, 1, 1
edges:      db 0,1, 1,2, 2,3, 3,0,  4,5, 5,6, 6,7, 7,4,  0,4, 1,5, 2,6, 3,7
rgb_r:      db 255,255,255,255,  0,  0,  0,  0, 255,  0,255,128
rgb_g:      db   0,128,255,255,255,255,128,  0,   0,  0,255,128
rgb_b:      db   0,  0,  0,128,  0,255,255,255, 255,255,255,255
pal8:       db  12, 6, 14, 10, 10, 11, 9, 9, 13, 1, 15, 7

sintab:
%include "sintab.inc"

MODESZ   equ 14
M_NUM    equ 0
M_W      equ 2
M_H      equ 4
M_BPP    equ 6
M_ATTR   equ 7
M_PITCH  equ 8
M_GRAN   equ 10
M_SEG    equ 12

argmode:  dw 0
nmodes:   dw 0
cur:      dw 0
tmpmode:  dw 0
runlimit: dw 0
tick0:    dw 0
scrw:     dw 0
scrh:     dw 0
pitch:    dd 0
granb:    dd 0
winseg:   dw 0
bpp:      db 0
bypp:     db 0
curbank:  dw 0
size:     dw 0
posx:     dw 0
posy:     dw 0
velx:     dw 0
vely:     dw 0
ang0:     dw 0
ang1:     dw 0
ang2:     dw 0
s0: dw 0
c0: dw 0
s1: dw 0
c1: dw 0
s2: dw 0
c2: dw 0
vx: dw 0
vy: dw 0
vz: dw 0
tx: dw 0
ty: dw 0
colour:   dd 0
haveold:  db 0
x0: dw 0
y0: dw 0
x1: dw 0
y1: dw 0
ldx: dw 0
ldy: dw 0
sx: dw 0
sy: dw 0
err: dw 0
colours:  times NEDGES dd 0
newpts:   times 16 dw 0
oldpts:   times 16 dw 0
modes:    times MAXMODES*14 db 0
vbeinfo:  times 512 db 0
modeinfo: times 256 db 0
