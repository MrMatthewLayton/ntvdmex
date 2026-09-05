; p_plan12.com -- mode 12h planar stores through ARBITRARY ModRM forms.  GH #15.
;
; In mode 12h the A0000 window is PAGE_NOACCESS, so every guest pixel write
; faults to the host, which runs a bounded 8086 interpreter over the faulting
; instruction and its loop. #15 says the REP STOS idiom and the QuickBASIC
; MOV/XCHG stores work but "arbitrary MOV/ModRM stores" are a gap.
;
; That is a claim about the DECODER, so this exercises the addressing forms
; rather than the opcodes: base, base+index, base+index+disp8, a BP-relative
; form with a segment override (BP defaults to SS, so a host that ignores the
; override writes to the wrong place entirely), a bare disp16, and a WORD store.
;
; ── HOW A FAILURE SHOWS UP ───────────────────────────────────────────────────
; Each form writes 0FFh to its own byte of the first scan line. With the default
; mode-12h write state that lights eight pixels to colour 15. The probe then
; reads them back with INT 10h AH=0Dh, which goes through the planar READ path,
; so a store that never landed reads 0 and a store that landed in the WRONG
; PLACE leaves its own cell 0 as well -- both visible, and distinguishable,
; because every form gets its own cell.
;
; ⚠ AH=0Dh is used deliberately rather than reading A0000 back directly: reading
;   the aperture would exercise the same fault path as the write and could hide a
;   fault-handler bug behind itself.
;
; nasm -f bin p_plan12.asm -o p_plan12.com

        org     100h
        jmp     start
%include "probe.inc"

; rdpix -- read the pixel at (CX,DX) via INT 10h AH=0Dh, colour -> AL.
rdpix:
        mov     ah, 0Dh
        mov     bh, 0
        int     10h
        ret

start:
        PROBE_BEGIN "plan12"

        ; ---- mode 12h, 640x480 planar.
        mov     ax, 0012h
        int     10h
        call    probe_capture
        EMIT    "int10.set.mode12", "AX"

        mov     ax, 0A000h
        mov     es, ax
        mov     al, 0FFh

        ; ---- form 1: [bx]                       -> byte 0, pixels x=0..7
        xor     bx, bx
        mov     [es:bx], al

        ; ---- form 2: [bx+si]                    -> byte 1, x=8..15
        xor     bx, bx
        mov     si, 1
        mov     [es:bx+si], al

        ; ---- form 3: [bx+di+disp8]              -> byte 2, x=16..23
        xor     bx, bx
        xor     di, di
        mov     [es:bx+di+2], al

        ; ---- form 4: [bp+si+disp8] WITH AN ES OVERRIDE. BP defaults to SS, so a
        ; decoder that drops the segment override writes into the stack instead
        ; of the framebuffer and this cell stays black.  -> byte 3, x=24..31
        xor     si, si
        mov     bp, 3
        mov     [es:bp+si], al

        ; ---- form 5: bare disp16                -> byte 4, x=32..39
        mov     [es:0004h], al

        ; ---- form 6: WORD store through [di]     -> bytes 5,6, x=40..55
        mov     di, 5
        mov     ax, 0FFFFh
        mov     [es:di], ax

        ; ---- read every cell back and report the colour.
        mov     cx, 0
        mov     dx, 0
        call    rdpix
        mov     [c1], al
        mov     cx, 8
        call    rdpix
        mov     [c2], al
        mov     cx, 16
        call    rdpix
        mov     [c3], al
        mov     cx, 24
        call    rdpix
        mov     [c4], al
        mov     cx, 32
        call    rdpix
        mov     [c5], al
        mov     cx, 40
        call    rdpix
        mov     [c6], al
        mov     cx, 48
        call    rdpix
        mov     [c7], al

        ; ---- back to text so the dump is readable and the box is left usable.
        mov     ax, 0003h
        int     10h

        ; Every cell should be 15. A zero names exactly which addressing form the
        ; store decoder does not handle.
        EMIT_BUF "plan12.colours", c1, 7

        PROBE_END

c1       db 0
c2       db 0
c3       db 0
c4       db 0
c5       db 0
c6       db 0
c7       db 0
