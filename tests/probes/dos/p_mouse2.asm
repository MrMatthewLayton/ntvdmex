; p_mouse2.com -- the INT 33h functions a version-8 driver PROMISES (GH #249).
;
; WHY A SECOND PROBE. p_mouse.asm measures the functions every driver since 6.0
; has; this one measures what `24h` = 8.00 entitles a guest to call: 25h-34h,
; plus the small get/set pairs (1Dh/1Eh, 22h/23h) and the 0Fh-vs-1Ah question.
; Kept apart so p_mouse's oracle cache (keyed by the binary) stays valid.
;
; WHO CAN ANSWER. The 6.22 oracle runs MOUSE.COM 6.24 (p_mouse2.pre), which
; predates 25h-34h -- for those rows it hands the POISON back, and that is its
; answer to "is this implemented": no, and it says 6.24 so it never promised to.
; DOSBox-X's built-in driver claims 8.05 and implements several of them; it is
; the only executed voice for most rows, so its answers are an EMULATOR'S
; OPINION and the Microsoft Mouse 8.x reference (RBIL INT 33h) outranks it.
; PCem has no mouse driver loaded and leaves at the vector check.
;
; Every output register is POISONED first: a driver that ignores a call returns
; the caller's registers, and "untouched" must not read as "answered".
;
; nasm -f bin p_mouse2.asm -o p_mouse2.com

        org     100h
        jmp     start
%include "probe.inc"

m33:
        int     33h
        call    probe_capture
        ret

; 09h's two 16-word masks: screen (AND) then cursor (XOR). A plain block -- only
; the HOT SPOT is read back (2Ah); the bitmap is a drawing question.
cmask:  times 16 dw 0FFFFh
        times 16 dw 0000h

start:
        PROBE_BEGIN "mouse2"

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
        ; A vector is not a driver: PCem's DOS has a BIOS dummy at INT 33h that
        ; answers the reset with AX=0. Every row after this would be noise.
        cmp     word [__ax], 0FFFFh
        je      .driver
        PROBE_END
.driver:

        ; ---- 1Bh straight after a reset: the driver's DEFAULT speed/threshold.
        POISON
        mov     ax, 001Bh
        call    m33
        EMIT    "i33.1B.reset", "BX,CX,DX"

        ; ---- ★ 32h FIRST: the call a v7.05+ guest makes to learn which of 25h-34h
        ; exist. Bit 15 = 25h ... bit 0 = 34h. BX/CX/DX are reserved (0).
        POISON
        mov     ax, 0032h
        call    m33
        EMIT    "i33.32.active", "AX,BX,CX,DX"

        ; ---- 25h general driver information. AX bits 13-12 = cursor type (00 =
        ; software text cursor, the reset default in mode 3), 11-8 = interrupt rate,
        ; 7-0 = active display drivers. BX/CX/DX: lock / in-driver / busy flags.
        POISON
        mov     ax, 0025h
        call    m33
        EMIT    "i33.25.info", "AX,BX,CX,DX"

        ; ---- 27h screen/cursor masks + mickeys. Straight after a reset: the
        ; driver's default text masks, and no motion.
        POISON
        mov     ax, 0027h
        call    m33
        EMIT    "i33.27.masks.reset", "AX,BX,CX,DX"

        ; ---- ...and after 0Ah BX=0 installs new ones, 27h must hand THEM back.
        mov     ax, 000Ah
        xor     bx, bx
        mov     cx, 1234h
        mov     dx, 5678h
        call    m33
        POISON
        mov     ax, 0027h
        call    m33
        EMIT    "i33.27.masks.set", "AX,BX"

        ; ---- 2Ah hot spot + visibility counter + mouse type. Hidden after reset,
        ; so the counter is negative. AL is the counter's low byte on every driver
        ; that answers; AH is compared separately below as informational.
        POISON
        mov     ax, 002Ah
        call    m33
        EMIT    "i33.2A.reset", "AL,BX,CX,DX"

        ; show once: the counter reaches 0
        mov     ax, 0001h
        call    m33
        POISON
        mov     ax, 002Ah
        call    m33
        EMIT    "i33.2A.shown", "AL"
        mov     ax, 0002h
        call    m33

        ; 09h with hot spot (3,5): 2Ah must report it back.
        push    cs
        pop     es
        mov     ax, 0009h
        mov     bx, 3
        mov     cx, 5
        mov     dx, cmask
        call    m33
        POISON
        mov     ax, 002Ah
        call    m33
        EMIT    "i33.2A.hotspot", "BX,CX"

        ; ---- ★ 31h vs 26h with a fence up. 31h is the CURRENT range (07h/08h);
        ; 26h's maximum is what this measures the drivers disagree about, if at all.
        mov     ax, 0007h
        mov     cx, 16
        mov     dx, 240
        call    m33
        mov     ax, 0008h
        mov     cx, 8
        mov     dx, 120
        call    m33
        POISON
        mov     ax, 0031h
        call    m33
        EMIT    "i33.31.range", "AX,BX,CX,DX"
        POISON
        mov     ax, 0026h
        call    m33
        EMIT    "i33.26.fenced", "BX,CX,DX"
        mov     ax, 0007h
        xor     cx, cx
        mov     dx, 639
        call    m33
        mov     ax, 0008h
        xor     cx, cx
        mov     dx, 199
        call    m33

        ; ---- 1Dh/1Eh display page: set 0, read back. (0, because a driver that
        ; validates the page against the mode must accept it.)
        mov     ax, 001Dh
        xor     bx, bx
        call    m33
        POISON
        mov     ax, 001Eh
        call    m33
        EMIT    "i33.1E.page", "BX"

        ; ---- 23h language: the US driver says 0 (English) whatever it was told.
        POISON
        mov     ax, 0023h
        call    m33
        EMIT    "i33.23.language", "BX"

        ; ---- ★ 0Fh IS NOT 1Ah. 0Fh sets the mickey/pixel RATIO, 1Ah the SPEED
        ; (0-100). Set a speed, then a ratio, and the speed must survive.
        mov     ax, 001Ah
        mov     bx, 40
        mov     cx, 60
        mov     dx, 32
        call    m33
        mov     ax, 000Fh
        mov     cx, 8
        mov     dx, 16
        call    m33
        POISON
        mov     ax, 001Bh
        call    m33
        EMIT    "i33.0F.notsens", "BX,CX,DX"

        ; ---- is 13h's threshold (mickeys/s) the variable 1Ah/1Bh's DX names?
        mov     ax, 0013h
        mov     dx, 100
        call    m33
        POISON
        mov     ax, 001Bh
        call    m33
        EMIT    "i33.13.vs.1B", "DX"

        ; ---- ★ 20h ENABLE MUST NOT RESET. Fence the pointer, enable, read the
        ; fence back through a position clamp (03h after an out-of-range 04h).
        mov     ax, 0007h
        mov     cx, 16
        mov     dx, 240
        call    m33
        POISON
        mov     ax, 0020h
        call    m33
        EMIT    "i33.20.enable", "AX"
        mov     ax, 0004h
        mov     cx, 600
        mov     dx, 100
        call    m33
        mov     ax, 0003h
        call    m33
        EMIT    "i33.20.keeps.range", "CX"
        mov     ax, 0007h
        xor     cx, cx
        mov     dx, 639
        call    m33

        ; ---- 2Fh mouse hardware reset: FFFFh = done.
        POISON
        mov     ax, 002Fh
        call    m33
        EMIT    "i33.2F.hwreset", "AX"

        ; ---- 30h BallPoint: FFFFh = no BallPoint here.
        POISON
        mov     ax, 0030h
        mov     cx, 0
        call    m33
        EMIT    "i33.30.ballpoint", "AX"

        ; ---- 29h enumerate video modes, first entry: CX = 0 is "no (more) modes".
        POISON
        mov     ax, 0029h
        xor     cx, cx
        call    m33
        EMIT    "i33.29.enum", "CX"

        PROBE_END
