; p_vbepm.com -- #53 acceptance: a protected-mode client USES the 4F0Ah interface.
;
; What a client does, end to end, on the real CPU: set a banked VESA mode (0101h,
; 640x480x8), ask 4F0Ah for the block, COPY it into its own memory (so nothing may
; point back at B260:0000), enter DPMI, run the copy from a 32-bit code segment on a
; 32-bit stack with near calls -- SetWindow, SetDisplayStart, SetPalette -- and then
; check every effect through a DIFFERENT path: INT 10h 4F05h/4F07h get, the DAC read
; ports, and VRAM read back through INT 10h bank switches. If the block's bank switch
; and 4F05h's disagree about where a byte went, a row here says so.
;
; ⚠ NO ORACLE CAN RUN THIS. MS-DOS 6.22 (QEMU, PCem) has no DPMI host, DOSBox-X none
;   built in, and both real VBE BIOSes we can execute answer 4F0Ah with AX=0100h (no
;   interface at all -- p_vesapm). This is the SPEC's acceptance test (VBE 2.0 §4.13),
;   run on the subject alone; the expected values are written in the comments.
;
; nasm -f bin p_vbepm.asm -o p_vbepm.com

        bits    16
        org     100h
        jmp     start
%include "probe.inc"

entry   dd      0               ; DPMI mode-switch entry
blkseg  dw      0               ; ES:DI / CX of 4F0Ah
blkoff  dw      0
blklen  dw      0
vsel    dw      0               ; selector for A000h
fp32    dd      0               ; 16:16 far pointer into the 32-bit code
        dw      0
fp16    dd      back16          ; 16:32 far pointer back (offset, selector)
        dw      0
old_ss  dw      0
old_esp dd      0
ss32    dw      0
r_win   dw      0               ; results
r_x     dw      0
r_y     dw      0
r_r     db      0
r_g     db      0
r_b     db      0
r_bank1 db      0
r_bank0 db      0
paldata db      03h, 15h, 2Ah, 0          ; B, G, R, pad -> DAC 40h = (2A, 15, 03)

start:
        PROBE_BEGIN "vbepm"
        mov     ah, 4Ah                 ; keep 64 KB, release the rest for the DPMI host
        mov     bx, 1000h
        int     21h

        mov     ax, 4F02h               ; 640x480x8, BANKED
        mov     bx, 0101h
        int     10h
        call    probe_capture
        EMIT    "vbepm.4F02.0101", "AX"     ; 004F
        cmp     word [__ax], 004Fh
        jne     l_bail

        POISON
        mov     ax, 4F0Ah
        mov     bx, 0000h
        xor     di, di
        int     10h
        call    probe_capture
        EMIT    "vbepm.4F0A", "AX"          ; 004F (was 0100)
        cmp     word [__ax], 004Fh
        jne     l_bail
        mov     ax, [__es]
        mov     [blkseg], ax
        mov     ax, [__di]
        mov     [blkoff], ax
        mov     ax, [__cx]
        mov     [blklen], ax
        cmp     ax, 512
        ja      l_bail

        ; ---- COPY the block into our own memory -- the client's copy.
        push    ds
        push    es
        push    cs
        pop     es
        mov     di, pmcode
        mov     cx, [blklen]
        mov     si, [blkoff]
        mov     ds, [blkseg]
        cld
        rep     movsb
        pop     es
        pop     ds

        ; ---- DPMI, 16-bit client.
        mov     ax, 1687h
        int     2Fh
        test    ax, ax
        jnz     l_bail
        mov     [entry], di
        mov     [entry+2], es
        xor     ax, ax
        call    far [entry]
        jc      l_bail

        ; now in 16-bit protected mode: CS/DS/SS are selectors over this segment
        mov     bx, 0A000h              ; a selector for the window
        mov     ax, 0002h
        int     31h
        jc      l_pmfail
        mov     [vsel], ax
        mov     bx, cs                  ; 32-bit code alias of CS
        mov     ax, 000Ah
        int     31h
        jc      l_pmfail
        mov     [fp32+2], ax
        mov     bx, ax
        mov     ax, 0009h
        mov     cx, 40FAh               ; code exec/read, DPL3, present, D=1
        int     31h
        jc      l_pmfail
        mov     bx, ds                  ; 32-bit (B=1) stack alias of DS
        mov     ax, 000Ah
        int     31h
        jc      l_pmfail
        mov     [ss32], ax
        mov     bx, ax
        mov     ax, 0009h
        mov     cx, 40F2h               ; data r/w, DPL3, present, B=1
        int     31h
        jc      l_pmfail
        mov     word [fp32], do32
        mov     [fp16+4], cs
        jmp     far [fp32]

back16:
        ; ---- every effect, through another path ----------------------------------
        mov     ax, 4F05h               ; get window A -> DX = bank (expect 0000)
        mov     bx, 0100h
        mov     dx, 0D1D1h
        int     10h
        mov     [r_win], dx
        mov     ax, 4F07h               ; get display start -> CX = x, DX = y (expect 0, 10)
        mov     bx, 0001h
        mov     cx, 0C1C1h
        mov     dx, 0D1D1h
        int     10h
        mov     [r_x], cx
        mov     [r_y], dx
        mov     dx, 3C7h                ; DAC 40h back through the read port
        mov     al, 40h
        out     dx, al
        mov     dx, 3C9h
        in      al, dx
        mov     [r_r], al
        in      al, dx
        mov     [r_g], al
        in      al, dx
        mov     [r_b], al
        push    es
        mov     es, [vsel]
        mov     ax, 4F05h               ; INT 10h bank 1 -> the byte SetWindow(1) put there
        xor     bx, bx
        mov     dx, 1
        int     10h
        mov     al, [es:0]
        mov     [r_bank1], al
        mov     ax, 4F05h               ; INT 10h bank 0 -> the byte SetWindow(0) put there
        xor     bx, bx
        xor     dx, dx
        int     10h
        mov     al, [es:0]
        mov     [r_bank0], al
        pop     es
        mov     ax, 0003h
        int     10h

        mov     ax, [r_win]
        mov     [__dx], ax
        EMIT    "vbepm.win.via.4F05", "DX"          ; 0000
        mov     ax, [r_x]
        mov     [__cx], ax
        mov     ax, [r_y]
        mov     [__dx], ax
        EMIT    "vbepm.start.via.4F07", "CX,DX"     ; 0000, 000A
        mov     al, [r_r]
        mov     ah, 0
        mov     [__ax], ax
        mov     al, [r_g]
        mov     [__bx], ax
        mov     al, [r_b]
        mov     [__cx], ax
        EMIT    "vbepm.pal.via.3C9", "AX,BX,CX"     ; 002A, 0015, 0003
        mov     al, [r_bank1]
        mov     ah, [r_bank0]
        mov     [__ax], ax
        EMIT    "vbepm.vram.via.4F05", "AX"         ; 335A: bank 1 = 5A, bank 0 = 33
        PROBE_END
l_pmfail:
        EMIT    "vbepm.dpmi.setup.failed", "AX"
        mov     ax, 0003h
        int     10h
        PROBE_END
l_bail:
        mov     ax, 0003h
        int     10h
        PROBE_END

; ---- the 32-bit client -------------------------------------------------------------
        bits    32
do32:
        mov     [old_ss], ss
        mov     [old_esp], esp
        mov     ss, [ss32]
        mov     esp, stack32_top
        ; SetWindow bank 1, write 5Ah through the window
        xor     ebx, ebx
        mov     edx, 1
        movzx   esi, word [pmcode + 0]
        add     esi, pmcode
        call    esi
        push    es
        mov     es, [vsel]
        mov     byte [es:0], 5Ah
        pop     es
        ; SetWindow bank 0, write 33h
        xor     ebx, ebx
        xor     edx, edx
        movzx   esi, word [pmcode + 0]
        add     esi, pmcode
        call    esi
        push    es
        mov     es, [vsel]
        mov     byte [es:0], 33h
        pop     es
        ; SetDisplayStart to line 10: (640 * 10) / 4 dwords, BL=80h (wait for retrace)
        mov     ebx, 80h
        mov     ecx, (640 * 10) / 4
        xor     edx, edx
        movzx   esi, word [pmcode + 2]
        add     esi, pmcode
        call    esi
        ; SetPalette: one entry, 40h, from ES:EDI = our paldata
        push    es
        push    ds
        pop     es
        xor     ebx, ebx
        mov     ecx, 1
        mov     edx, 40h
        mov     edi, paldata
        movzx   esi, word [pmcode + 4]
        add     esi, pmcode
        call    esi
        pop     es
        mov     ss, [old_ss]
        mov     esp, [old_esp]
        jmp     far [fp16]
        bits    16

        align   4
pmcode:  times 512 db 0
         times 256 db 0
stack32_top:
