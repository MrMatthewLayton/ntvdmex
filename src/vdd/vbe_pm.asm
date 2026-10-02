; vbe_pm.asm -- the VBE 2.0 protected-mode interface block (INT 10h AX=4F0Ah, #53).
;
; WHAT THIS IS. VBE 2.0 §4.13: 4F0Ah hands a protected-mode client ES:DI -> this block and
; CX = its length. The client COPIES it (into a 32-bit code segment of its own, or calls
; it through a selector over this real-mode memory) and then calls the three entry points
; with a 32-bit NEAR call -- no INT 10h, no switch back to real mode, so a banked frame no
; longer costs a DPMI 0300h round trip per bank (ZAR: 4,735 of them a run).
;
; ⛔ THE CODE MUST WORK WHEREVER IT IS COPIED. So: no absolute address anywhere -- only
;   relative jumps, the stack, and port I/O. Every register is preserved except the ones
;   the spec says carry data in. It talks to the card the way a real card's block does:
;   with OUTs to the card's own registers. Ours are the index/data pair 01CEh/01CFh
;   (vdd_video.c, vbe_port_*): index 05h is the bank (the number Bochs's VBE uses for the
;   same register), 10h/11h the display start in DWORDs, written low then high (the high
;   write commits). The palette goes through the standard VGA DAC ports 3C8h/3C9h, and the
;   "wait for vertical retrace" forms poll 3DAh bit 3, as a VGA BIOS does.
;
; Calling conventions (VBE 2.0 §4.13, as clients such as Allegro's vesa.c use them):
;   SetWindow       BH = 00h (set; the PM form has no get), BL = window (0 = A),
;                   DX = position in granularity units (64 KB here)
;   SetDisplayStart BL = 00h now / 80h during vertical retrace,
;                   CX = start bits 0-15, DX = bits 16-31, in DWORDS (byte address / 4)
;   SetPalette      BL = 00h now / 80h during vertical retrace, CX = count, DX = first,
;                   ES:EDI = entries of Blue, Green, Red, alignment (the DAC's width)
;
; Regenerate src/vdd/vbe_pm.h after any change:
;     python3 tools/gen-vbepm.py
; (assembles this with nasm and writes the bytes and the entry offsets).

        bits    32
        org     0

table:
        dw      setwindow - table
        dw      setstart - table
        dw      setpal - table
        dw      ports - table

; The I/O-privilege list (§4.13): every port the code touches, FFFFh-terminated, then the
; memory-location list -- empty -- FFFFh-terminated.
ports:
        dw      01CEh, 01CFh, 03C8h, 03C9h, 03DAh, 0FFFFh
        dw      0FFFFh

setwindow:
        test    bx, bx                  ; BH=00h set, BL=00h window A -- nothing else exists
        jnz     .out
        push    eax
        push    edx
        mov     eax, edx                ; AX = position
        mov     dx, 01CEh
        push    eax
        mov     ax, 0005h               ; index 05h: bank
        out     dx, ax
        pop     eax
        inc     dx
        out     dx, ax
        pop     edx
        pop     eax
.out:   ret

setstart:
        push    eax
        push    edx
        movzx   eax, dx                 ; EAX = DX:CX, the start in dwords
        shl     eax, 16
        mov     ax, cx
        push    eax
        test    bl, 80h
        jz      .now
        mov     dx, 03DAh
.leave: in      al, dx                  ; let any retrace in progress end...
        test    al, 08h
        jnz     .leave
.wait:  in      al, dx                  ; ...then wait for the next one to begin
        test    al, 08h
        jz      .wait
.now:   mov     dx, 01CEh
        mov     ax, 0010h               ; index 10h: start, low word
        out     dx, ax
        inc     dx
        mov     eax, [esp]
        out     dx, ax
        dec     dx
        mov     ax, 0011h               ; index 11h: start, high word -- commits
        out     dx, ax
        inc     dx
        mov     eax, [esp]
        shr     eax, 16
        out     dx, ax
        pop     eax
        pop     edx
        pop     eax
        ret

setpal:
        push    eax
        push    ecx
        push    edx
        push    edi
        test    bl, 80h
        jz      .now
        push    edx
        mov     dx, 03DAh
.leave: in      al, dx
        test    al, 08h
        jnz     .leave
.wait:  in      al, dx
        test    al, 08h
        jz      .wait
        pop     edx
.now:   movzx   ecx, cx
        mov     al, dl                  ; first entry
        mov     dx, 03C8h
        out     dx, al
        inc     dx                      ; 3C9h: R, G, B per entry
        test    ecx, ecx
        jz      .done
.next:  mov     al, [es:edi+2]
        out     dx, al
        mov     al, [es:edi+1]
        out     dx, al
        mov     al, [es:edi]
        out     dx, al
        add     edi, 4
        dec     ecx
        jnz     .next
.done:  pop     edi
        pop     edx
        pop     ecx
        pop     eax
        ret
end:
