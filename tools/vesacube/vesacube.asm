; vesacube.com -- a VESA exerciser: mode selector + a tumbling, bouncing solid 3-D cube.
;
; What it drives, deliberately, so a VESA implementation is judged by what a guest
; DOES with it rather than by what it says:
;   4F00 (VBE2 block, mode list)   4F01 (every mode: geometry, depth, model, window)
;   4F02 (banked set)              4F03 (read-back after set)
;   4F05 (bank switch on EVERY crossing -- the cube walks the whole framebuffer)
;   4F06 get (pitch AND how many rows VRAM holds -> one page or two)
;   4F07 BL=80h (page flip on the retrace when two pages fit -- double buffering)
;   4F09 (the 8 bpp shade palette, 24 entries from index 16)
;   pixel formats 8 (palette), 15 (5:5:5), 16 (5:6:5), 24 (B,G,R)
; The cube is SOLID: six faces with outward winding, back-face culled on the sign of
; the rotated normal's z, flat-shaded in four levels from that normal, filled by a
; convex scan-line filler with horizontal runs -- a filled face crosses bank
; boundaries mid-run, which a wire frame rarely does.
;
; Real mode, 386 instructions, banked windows only (the LFB needs a protected-mode
; or unreal-mode client; that is heaven7's job).  Draws with a bank-aware byte
; writer; with two pages it flips, with one it erases the last frame's bounding box.
; Paces frames on the BIOS tick (see vsync for why not 3DAh).
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
        mov     byte [pages], 1
        ; 4F06 BL=1: the BIOS's own pitch wins over the mode block's, and DX says
        ; how many rows VRAM holds -- whether a second page exists
        mov     ax, 4F06h
        mov     bl, 1
        int     10h
        cmp     ax, 004Fh
        jne     .nopitch
        movzx   eax, bx
        mov     [pitch], eax
        ; DX = rows VRAM holds at this pitch: two pages if the whole picture fits twice
        mov     ax, [scrh]
        shl     ax, 1
        cmp     dx, ax
        jb      .nopitch
        mov     byte [pages], 2
.nopitch:
        mov     word [curbank], 0FFFFh
        call    build_colours
        call    load_palette                    ; 4F09, 8 bpp only
        mov     byte [page], 0
        mov     dword [pagebase], 0
        mov     word [bb0+4], -1                ; old bbox invalid (ymax < 0)
        mov     word [bb1+4], -1
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
        ; tick base for the headless form
        mov     ah, 0
        int     1Ah
        mov     [tick0], dx
.frame:
        call    step
        call    project
        call    vsync
        ; pick the page to draw on (the one NOT being shown), and its byte base
        cmp     byte [pages], 2
        jne     .onepage
        xor     byte [page], 1
        movzx   eax, byte [page]
        imul    eax, [pitch]
        movzx   ecx, word [scrh]
        imul    eax, ecx
        mov     [pagebase], eax
.onepage:
        call    erase_bbox                      ; what this page showed last time
        call    draw_solid                      ; culled, shaded faces; records the bbox
        cmp     byte [pages], 2
        jne     .shown
        mov     ax, 4F07h                       ; show this page on the retrace
        mov     bx, 0080h
        xor     cx, cx
        movzx   dx, byte [page]
        imul    dx, [scrh]
        int     10h
.shown:
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

; colours[face*4+level] in this mode's pixel format. Face base colour is a 0/1 RGB
; mask (facecol), level 0..3 scales it 25/50/75/100 %. 8 bpp: palette index 16+.
build_colours:
        xor     ebx, ebx                        ; face*4+level
.e:     mov     eax, ebx
        shr     eax, 2                          ; face
        mov     al, [facecol+eax]               ; bit0 R, bit1 G, bit2 B
        mov     ah, bl
        and     ah, 3                           ; level
        ; channel value by level: 40/60/80/100 %
        movzx   ecx, ah
        mov     cl, [lvl8+ecx]
        xor     edx, edx                        ; edx: B<<16 | G<<8 | R (scratch)
        test    al, 1
        jz      .nr
        mov     [tr], cl
        jmp     .g
.nr:    mov     byte [tr], 0
.g:     test    al, 2
        jz      .ng
        mov     [tg], cl
        jmp     .b
.ng:    mov     byte [tg], 0
.b:     test    al, 4
        jz      .nb
        mov     [tb], cl
        jmp     .pack
.nb:    mov     byte [tb], 0
.pack:  cmp     byte [bpp], 8
        jne     .n8
        lea     eax, [ebx+16]                   ; palette index
        jmp     .st
.n8:    mov     al, [tr]
        mov     ah, [tg]
        mov     dl, [tb]
        call    pack_rgb
.st:    mov     [colours+ebx*4], eax
        inc     ebx
        cmp     ebx, 24
        jb      .e
        ret

; pack_rgb -- AL=R AH=G DL=B (8-bit) -> EAX in the mode's format (15/16/24)
pack_rgb:
        cmp     byte [bpp], 15
        jne     .n15
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
        ret
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
        ret
.n16:   movzx   ecx, al                         ; 24: B,G,R in memory = R<<16|G<<8|B
        shl     ecx, 16
        movzx   esi, ah
        shl     esi, 8
        or      ecx, esi
        movzx   esi, dl
        or      ecx, esi
        mov     eax, ecx
        ret

; load_palette -- 8 bpp: 24 shade entries at index 16 through 4F09 (6-bit DAC)
load_palette:
        cmp     byte [bpp], 8
        jne     .r
        xor     ebx, ebx
.e:     mov     eax, ebx
        shr     eax, 2
        mov     al, [facecol+eax]
        movzx   ecx, bl
        and     cl, 3
        mov     cl, [lvl6+ecx]                  ; 6-bit DAC: 40/60/80/100 %
        lea     di, [palq+ebx*4]
        mov     byte [di+3], 0
        mov     byte [di+2], 0                  ; R
        mov     byte [di+1], 0                  ; G
        mov     byte [di+0], 0                  ; B
        test    al, 1
        jz      .g
        mov     [di+2], cl
.g:     test    al, 2
        jz      .b
        mov     [di+1], cl
.b:     test    al, 4
        jz      .n
        mov     [di+0], cl
.n:     inc     ebx
        cmp     ebx, 24
        jb      .e
        push    ds
        pop     es
        mov     di, palq
        mov     ax, 4F09h
        xor     bl, bl
        mov     cx, 24
        mov     dx, 16
        int     10h
.r:     ret

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
        ; keep the rotated point for the face normals
        lea     esi, [edi+edi*2]
        shl     esi, 1                          ; di*6
        mov     ax, [vx]
        mov     [rot+esi], ax
        mov     ax, [vy]
        mov     [rot+esi+2], ax
        mov     ax, [vz]
        mov     [rot+esi+4], ax
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
; erase_bbox: black out the rectangle this page showed last time (bb0/bb1: x0,y0,x1,y1)
erase_bbox:
        mov     si, bb0
        cmp     byte [page], 0
        je      .p
        mov     si, bb1
.p:     mov     ax, [si+6]
        cmp     ax, 0
        jl      .r                              ; nothing drawn on this page yet
        mov     dword [colour], 0
        mov     dx, [si+2]                      ; y0
.row:   cmp     dx, [si+6]
        jg      .r
        mov     cx, [si]                        ; x0
        mov     bx, [si+4]                      ; x1
        call    hline
        inc     dx
        jmp     .row
.r:     ret

; draw_solid: for each face, cull on the rotated normal, shade, fill; track the bbox
draw_solid:
        mov     word [nbx0], 32767
        mov     word [nby0], 32767
        mov     word [nbx1], -32768
        mov     word [nby1], -32768
        xor     ebx, ebx                        ; face
.f:     push    ebx
        ; vertex indices a,b,c,d
        movzx   eax, byte [faces+ebx*4]
        movzx   ecx, byte [faces+ebx*4+1]
        movzx   edx, byte [faces+ebx*4+2]
        mov     [ia], ax
        mov     [ib], cx
        mov     [ic], dx
        movzx   eax, byte [faces+ebx*4+3]
        mov     [id], ax
        ; normal n = (b-a) x (c-a) from the rotated points (each point is 3 words)
        movzx   esi, word [ia]
        imul    esi, esi, 6
        movzx   edi, word [ib]
        imul    edi, edi, 6
        movsx   eax, word [rot+edi]
        movsx   ecx, word [rot+esi]
        sub     eax, ecx
        mov     [e1x], eax
        movsx   eax, word [rot+edi+2]
        movsx   ecx, word [rot+esi+2]
        sub     eax, ecx
        mov     [e1y], eax
        movsx   eax, word [rot+edi+4]
        movsx   ecx, word [rot+esi+4]
        sub     eax, ecx
        mov     [e1z], eax
        movzx   edi, word [ic]
        imul    edi, edi, 6
        movsx   eax, word [rot+edi]
        movsx   ecx, word [rot+esi]
        sub     eax, ecx
        mov     [e2x], eax
        movsx   eax, word [rot+edi+2]
        movsx   ecx, word [rot+esi+2]
        sub     eax, ecx
        mov     [e2y], eax
        movsx   eax, word [rot+edi+4]
        movsx   ecx, word [rot+esi+4]
        sub     eax, ecx
        mov     [e2z], eax
        ; nz = e1x*e2y - e1y*e2x ; visible iff nz < 0 (camera at -z)
        mov     eax, [e1x]
        imul    eax, [e2y]
        mov     ecx, [e1y]
        imul    ecx, [e2x]
        sub     eax, ecx
        mov     [nz], eax
        cmp     eax, 0
        jge     .skip
        ; nx = e1y*e2z - e1z*e2y ; ny = e1z*e2x - e1x*e2z
        mov     eax, [e1y]
        imul    eax, [e2z]
        mov     ecx, [e1z]
        imul    ecx, [e2y]
        sub     eax, ecx
        mov     [nx], eax
        mov     eax, [e1z]
        imul    eax, [e2x]
        mov     ecx, [e1x]
        imul    ecx, [e2z]
        sub     eax, ecx
        mov     [ny], eax
        ; light from the upper left front: val = -2*nz + nx - ny (y grows downward)
        ; |n| = 4*size^2, so level = val / (2*size^2), clamped to 0..3
        mov     eax, [nz]
        neg     eax
        shl     eax, 1
        add     eax, [nx]
        sub     eax, [ny]
        jns     .pos
        xor     eax, eax
.pos:   movzx   ecx, word [size]
        imul    ecx, ecx
        shl     ecx, 1
        xor     edx, edx
        div     ecx
        cmp     eax, 3
        jbe     .lv
        mov     eax, 3
.lv:    pop     ebx                             ; face (re-pushed: .skip pops it)
        push    ebx
        shl     ebx, 2
        add     ebx, eax                        ; face*4+level
        mov     eax, [colours+ebx*4]
        mov     [colour], eax
        ; the quad's four projected points -> qp[]
        mov     esi, newpts
        movzx   edi, word [ia]
        mov     eax, [esi+edi*4]
        mov     [qp], eax
        movzx   edi, word [ib]
        mov     eax, [esi+edi*4]
        mov     [qp+4], eax
        movzx   edi, word [ic]
        mov     eax, [esi+edi*4]
        mov     [qp+8], eax
        movzx   edi, word [id]
        mov     eax, [esi+edi*4]
        mov     [qp+12], eax
        call    fill_quad
.skip:  pop     ebx
        inc     ebx
        cmp     ebx, 6
        jb      .f
        ; commit the bbox for this page
        mov     si, bb0
        cmp     byte [page], 0
        je      .c
        mov     si, bb1
.c:     mov     ax, [nbx0]
        mov     [si], ax
        mov     ax, [nby0]
        mov     [si+2], ax
        mov     ax, [nbx1]
        mov     [si+4], ax
        mov     ax, [nby1]
        mov     [si+6], ax
        ret

; fill_quad: convex fill of qp[0..3] in [colour]; updates the new bbox
fill_quad:
        ; y range of the quad, clipped to the screen
        mov     ax, 32767
        mov     [qy0], ax
        mov     word [qy1], -32768
        xor     ebx, ebx
.yr:    mov     ax, [qp+ebx*4+2]
        cmp     ax, [qy0]
        jge     .n1
        mov     [qy0], ax
.n1:    cmp     ax, [qy1]
        jle     .n2
        mov     [qy1], ax
.n2:    inc     ebx
        cmp     ebx, 4
        jb      .yr
        cmp     word [qy0], 0
        jge     .c0
        mov     word [qy0], 0
.c0:    mov     ax, [scrh]
        dec     ax
        cmp     [qy1], ax
        jle     .c1
        mov     [qy1], ax
.c1:    mov     ax, [qy0]
        cmp     ax, [qy1]
        jg      .done
        ; init spans
        movzx   edi, word [qy0]
.ini:   mov     word [xl+edi*2], 32767
        mov     word [xr+edi*2], -32768
        inc     edi
        cmp     di, [qy1]
        jle     .ini
        ; walk the four edges
        xor     ebx, ebx
.ed:    mov     ax, [qp+ebx*4]
        mov     [ex0], ax
        mov     ax, [qp+ebx*4+2]
        mov     [ey0], ax
        mov     ecx, ebx
        inc     ecx
        and     ecx, 3
        mov     ax, [qp+ecx*4]
        mov     [ex1], ax
        mov     ax, [qp+ecx*4+2]
        mov     [ey1], ax
        push    ebx
        call    edge_walk
        pop     ebx
        inc     ebx
        cmp     ebx, 4
        jb      .ed
        ; fill the spans
        mov     dx, [qy0]
.sp:    movzx   edi, dx
        mov     cx, [xl+edi*2]
        mov     bx, [xr+edi*2]
        cmp     cx, bx
        jg      .nx
        call    hline
.nx:    inc     dx
        cmp     dx, [qy1]
        jle     .sp
        ; bbox
        mov     ax, [qy0]
        cmp     ax, [nby0]
        jge     .b1
        mov     [nby0], ax
.b1:    mov     ax, [qy1]
        cmp     ax, [nby1]
        jle     .done
        mov     [nby1], ax
.done:  ret

; edge_walk: (ex0,ey0)-(ex1,ey1): per row, widen xl/xr; also the x bbox
edge_walk:
        mov     ax, [ex0]
        call    bbx
        mov     ax, [ex1]
        call    bbx
        mov     ax, [ey0]
        cmp     ax, [ey1]
        jle     .ord
        xchg    ax, [ey1]
        mov     [ey0], ax
        mov     ax, [ex0]
        xchg    ax, [ex1]
        mov     [ex0], ax
.ord:   movsx   eax, word [ex0]
        shl     eax, 16                         ; x in 16.16
        mov     [efx], eax
        movsx   ecx, word [ey1]
        movsx   edx, word [ey0]
        sub     ecx, edx                        ; dy >= 0
        jnz     .slope
        ; horizontal edge: both x on one row
        mov     dx, [ey0]
        mov     ax, [ex0]
        call    span_x
        mov     ax, [ex1]
        call    span_x
        ret
.slope: movsx   eax, word [ex1]
        movsx   edx, word [ex0]
        sub     eax, edx
        shl     eax, 16
        cdq
        idiv    ecx                             ; step per row, 16.16
        mov     [estep], eax
        mov     dx, [ey0]
.w:     mov     eax, [efx]
        sar     eax, 16
        call    span_x                          ; AX=x, DX=y
        mov     eax, [efx]
        add     eax, [estep]
        mov     [efx], eax
        inc     dx
        cmp     dx, [ey1]
        jle     .w
        ret

span_x:                                         ; AX = x, DX = y: widen row y's span
        cmp     dx, 0
        jl      .r
        cmp     dx, [scrh]
        jge     .r
        push    edi
        movzx   edi, dx
        cmp     ax, [xl+edi*2]
        jge     .nl
        mov     [xl+edi*2], ax
.nl:    cmp     ax, [xr+edi*2]
        jle     .nr
        mov     [xr+edi*2], ax
.nr:    pop     edi
.r:     ret

bbx:                                            ; AX = x: widen the new x bbox
        cmp     ax, [nbx0]
        jge     .n
        mov     [nbx0], ax
.n:     cmp     ax, [nbx1]
        jle     .r
        mov     [nbx1], ax
.r:     ret

; hline: row DX from CX to BX inclusive in [colour]; clips; bank-aware
hline:
        pushad
        cmp     dx, 0
        jl      .out
        cmp     dx, [scrh]
        jge     .out
        cmp     cx, 0
        jge     .l
        xor     cx, cx
.l:     mov     ax, [scrw]
        dec     ax
        cmp     bx, ax
        jle     .r
        mov     bx, ax
.r:     cmp     cx, bx
        jg      .out
        sub     bx, cx
        inc     bx                              ; run length in pixels
        movzx   eax, dx
        imul    eax, [pitch]
        movzx   esi, cx
        movzx   ecx, byte [bypp]
        imul    esi, ecx
        add     eax, esi
        add     eax, [pagebase]                 ; EAX = first byte
        movzx   esi, bx                         ; pixels left
.px:    mov     edx, [colour]
        movzx   ecx, byte [bypp]
.b:     call    putbyte
        shr     edx, 8
        loop    .b
        dec     esi
        jnz     .px
.out:   popad
        ret

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
; faces as a,b,c,d with (b-a)x(c-a) pointing OUTWARD (checked by hand per face)
faces:      db 0,3,2,1      ; front  z=-1
            db 4,5,6,7      ; back   z=+1
            db 0,1,5,4      ; bottom y=-1
            db 3,7,6,2      ; top    y=+1
            db 0,4,7,3      ; left   x=-1
            db 1,2,6,5      ; right  x=+1
facecol:    db 1, 2, 4, 3, 5, 6     ; R, G, B, yellow, magenta, cyan (bit0 R bit1 G bit2 B)
lvl8:       db 102, 153, 204, 255   ; shade levels, 8-bit channels
lvl6:       db  25,  38,  51,  63   ; the same for a 6-bit DAC

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
tr: db 0
tg: db 0
tb: db 0
pages:    db 1
page:     db 0
pagebase: dd 0
ia: dw 0
ib: dw 0
ic: dw 0
id: dw 0
e1x: dd 0
e1y: dd 0
e1z: dd 0
e2x: dd 0
e2y: dd 0
e2z: dd 0
nx: dd 0
ny: dd 0
nz: dd 0
qp:       times 8 dw 0
qy0: dw 0
qy1: dw 0
ex0: dw 0
ey0: dw 0
ex1: dw 0
ey1: dw 0
efx: dd 0
estep: dd 0
nbx0: dw 0
nby0: dw 0
nbx1: dw 0
nby1: dw 0
bb0:      times 4 dw 0
bb1:      times 4 dw 0
colours:  times 24 dd 0
palq:     times 24 dd 0
newpts:   times 16 dw 0
rot:      times 24 dw 0
xl:       times 1024 dw 0
xr:       times 1024 dw 0
modes:    times MAXMODES*14 db 0
vbeinfo:  times 512 db 0
modeinfo: times 256 db 0
