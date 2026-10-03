; vbe_rm.asm -- the VBE real-mode window function (ModeInfoBlock +0Ch WinFuncPtr, #273).
;
; WHAT THIS IS. VBE 1.2 onward lets a real-mode program bank-switch with a FAR CALL to
; WinFuncPtr instead of INT 10h AX=4F05h -- the same registers as 4F05h, no status
; returned. We left the pointer NULL (legal: "use 4F05h"), but a VBE 1.x program that
; far-calls it without checking jumps to 0000:0000. This stub drives the same card
; register as the protected-mode block's SetWindow (vbe_pm.asm): index 05h at 01CEh, data
; at 01CFh. It lives at VDD_VBEPM_SEG:VDD_VBERM_OFF, after that block.
;
; Registers (VBE 2.0 §4.7, 4F05h):
;   BH = 00h set window position, 01h get it
;   BL = window (00h = A; there is no window B)
;   DX = position in granularity units (64 KB) -- in for set, out for get
; Every other register is preserved (the spec allows AX/DX to be destroyed; we do not
; destroy AX, and DX only on a get).
;
; Regenerate src/vdd/vbe_pm.h after any change:
;     python3 tools/gen-vbepm.py

        bits    16
        org     0

winfunc:
        test    bl, bl                  ; window A only
        jnz     .ret
        cmp     bh, 01h
        ja      .ret                    ; 00h set / 01h get -- nothing else
        push    ax
        push    dx                      ; the position (set)
        mov     dx, 01CEh
        mov     ax, 0005h               ; index 05h: bank
        out     dx, ax
        inc     dx
        pop     ax                      ; AX = position
        test    bh, bh
        jnz     .get
        out     dx, ax
        mov     dx, ax                  ; DX as the caller left it
        pop     ax
        retf
.get:   in      ax, dx
        mov     dx, ax                  ; DX = the window's position
        pop     ax
.ret:   retf
