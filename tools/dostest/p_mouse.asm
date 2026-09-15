; p_mouse.com -- the INT 33h mouse driver contract, measured against the real
; Microsoft driver on MS-DOS 6.22.
;
; WHY THIS IS A FAIR COMPARISON. INT 33h is not a BIOS service: it belongs to a
; DRIVER a program loads. On the oracle that driver is MOUSE.COM (staged by
; p_mouse.deps, loaded by p_mouse.pre); under NTVDMEX the driver IS the host. So
; this measures our INT 33h against the one every DOS program was written for.
; ⚠ The .pre is ORACLE-ONLY on purpose -- loading MOUSE.COM on our side would
;   install its INT 33h over ours and measure the wrong thing entirely.
;
; WHY IT IS HEADLESS. Nothing here needs the mouse to MOVE. The driver's whole
; coordinate model is readable through set-position/read-back (AX=0004 then 0003),
; its clamping through the range calls (0007/0008), and its configuration through
; the get/set pairs (001A/001B) -- all deterministic, all comparable.
;
; ⚠ TEXT MODE IS 640x200 TO THIS DRIVER whatever the font, which is why a text UI
;   computes its row as DX/8. Session 71 returned the 400-line frame row here and
;   every QBasic menu click landed two rows low. Mode 3 is set first so both hosts
;   answer for the same screen, and 26h/03h/04h pin the virtual size directly.
;
; nasm -f bin p_mouse.asm -o p_mouse.com

        org     100h
        jmp     start
%include "probe.inc"

m33:                                    ; INT 33h, captured
        int     33h
        call    probe_capture
        ret

start:
        PROBE_BEGIN "mouse"

        ; ---- IS THERE A DRIVER AT ALL? An INT 33h with no handler is a jump into
        ; whatever happens to be at 0000:00CC, so this is asked BEFORE anything
        ; else and the probe leaves quietly if the answer is no. An absent driver
        ; must read as absent, not as a crashed run.
        mov     ax, 3533h
        int     21h                     ; ES:BX = the INT 33h handler
        mov     ax, es
        mov     [__ax], ax
        mov     [__bx], bx
        EMIT    "i33.vector", "AX"      ; informational: 0 = nobody home
        mov     ax, es
        or      ax, ax
        jnz     .have
        PROBE_END
.have:

        ; ---- one known screen, so the coordinate rows mean the same thing on both.
        mov     ax, 0003h
        int     10h

        ; ---- ...AND PROVE BOTH HOSTS AGREE THEY ARE IN IT, before reading a single
        ; coordinate off either. The mouse driver's virtual screen is derived from
        ; the video mode, so a coordinate mismatch whose real cause is a MODE
        ; mismatch would otherwise be chased in the wrong file entirely.
        ; 0040:0049 mode, 004A columns, 0084 rows-1, 0085 character height.
        push    ds
        mov     ax, 40h
        mov     ds, ax
        mov     al, [49h]
        xor     ah, ah
        mov     bx, ax
        mov     ax, [4Ah]
        mov     cx, ax
        mov     al, [84h]
        xor     ah, ah
        mov     dx, ax
        mov     ax, [85h]
        pop     ds
        mov     [__ax], ax
        mov     [__bx], bx
        mov     [__cx], cx
        mov     [__dx], dx
        EMIT    "bda.video", "AX,BX,CX,DX"

        ; ---- AX=0000 RESET. The installation check every program makes:
        ; AX=FFFF installed, BX=button count.
        POISON
        xor     ax, ax
        call    m33
        EMIT    "i33.00.reset", "AX,BX"

        ; ---- AX=0024 driver version/type. BX=version BCD, CH=type, CL=IRQ.
        mov     ax, 0024h
        call    m33
        EMIT    "i33.24.version", "BX,CX"

        ; ---- AX=0026 the VIRTUAL SCREEN. CX/DX are the maximum x/y the driver
        ; will ever report. In text mode a real driver says 639/199 -- the 640x200
        ; virtual screen -- NOT the pixel height of the frame.
        POISON                          ; ⚠ WITHOUT THIS the row was a false answer:
        mov     ax, 0026h               ; CX still held the version call's value and
        call    m33                     ; read as a plausible 04FF. MOUSE.COM 6.24
        EMIT    "i33.26.maxvirt", "CX,DX"    ; does not implement 26h at all.

        ; ---- AX=0003 straight after a reset: where does the pointer start, and
        ; are the buttons reported up?
        mov     ax, 0003h
        call    m33
        EMIT    "i33.03.afterreset", "BX,CX,DX"

        ; ---- ★ AX=0004 SET POSITION, read back with 0003. This is the whole
        ; coordinate contract in two calls, and it needs no mouse. A driver that
        ; snaps to the 8-pixel cell grid shows it here.
        mov     ax, 0004h
        mov     cx, 100
        mov     dx, 50
        call    m33
        mov     ax, 0003h
        call    m33
        EMIT    "i33.04.setpos", "CX,DX"

        ; ---- an ODD coordinate, which is where cell snapping becomes visible.
        mov     ax, 0004h
        mov     cx, 101
        mov     dx, 51
        call    m33
        mov     ax, 0003h
        call    m33
        EMIT    "i33.04.setpos.odd", "CX,DX"

        ; ---- ★ RANGES AND CLAMPING. 0007/0008 fence the pointer; a set-position
        ; outside the fence must come back clamped, not wrapped and not accepted.
        mov     ax, 0007h
        mov     cx, 16
        mov     dx, 240
        call    m33
        mov     ax, 0008h
        mov     cx, 8
        mov     dx, 120
        call    m33
        mov     ax, 0004h               ; well past both maxima
        mov     cx, 600
        mov     dx, 190
        call    m33
        mov     ax, 0003h
        call    m33
        EMIT    "i33.07.clamp.hi", "CX,DX"
        mov     ax, 0004h               ; and below both minima
        xor     cx, cx
        xor     dx, dx
        call    m33
        mov     ax, 0003h
        call    m33
        EMIT    "i33.07.clamp.lo", "CX,DX"

        ; ---- put the fence back to the whole screen before anything else runs.
        mov     ax, 0007h
        xor     cx, cx
        mov     dx, 639
        call    m33
        mov     ax, 0008h
        xor     cx, cx
        mov     dx, 199
        call    m33

        ; ---- AX=000B relative motion. Nothing has moved, and a read is supposed
        ; to ZERO the counters, so twice in a row must both be zero.
        mov     ax, 000Bh
        call    m33
        EMIT    "i33.0B.motion", "CX,DX"
        mov     ax, 000Bh
        call    m33
        EMIT    "i33.0B.motion.again", "CX,DX"

        ; ---- AX=0006 button release info for button 0: nothing has been clicked.
        mov     ax, 0006h
        xor     bx, bx
        call    m33
        EMIT    "i33.06.release", "AX,BX"

        ; ---- AX=0005 button press info for button 0, likewise.
        mov     ax, 0005h
        xor     bx, bx
        call    m33
        EMIT    "i33.05.press", "AX,BX"

        ; ---- ★ A GET/SET PAIR: 001A sets sensitivity, 001B reads it back. If the
        ; driver stores it at all, these must agree -- and a host that accepts the
        ; set and forgets it is a host whose pointer speed setting does nothing.
        mov     ax, 001Ah
        mov     bx, 40
        mov     cx, 60
        mov     dx, 32
        call    m33
        mov     ax, 001Bh
        call    m33
        EMIT    "i33.1B.sensitivity", "BX,CX,DX"

        ; ---- AX=0015 how much memory a save-state needs. Programs that swap the
        ; driver out (every full-screen DOS app that shells out) allocate this.
        POISON
        mov     ax, 0015h
        call    m33
        EMIT    "i33.15.statesize", "BX"

        ; ---- AX=0021 software reset: like 0000 but leaves the mode alone.
        POISON
        mov     ax, 0021h
        call    m33
        EMIT    "i33.21.softreset", "AX,BX"

        ; ---- show/hide must be survivable and must not fabricate an answer.
        mov     ax, 0002h
        call    m33
        mov     ax, 0001h
        call    m33
        EMIT    "i33.01.show", "CF"

        ; ---- AX=000F mickeys per 8 pixels: set-only, but it must not fault and
        ; must not leave the position somewhere else.
        mov     ax, 000Fh
        mov     cx, 8
        mov     dx, 16
        call    m33
        mov     ax, 0004h
        mov     cx, 320
        mov     dx, 100
        call    m33
        mov     ax, 0003h
        call    m33
        EMIT    "i33.0F.after", "CX,DX"

        PROBE_END
