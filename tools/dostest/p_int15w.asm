; p_int15w.com -- INT 15h's timed waits and the keyboard intercept (GH #206).
;
; What is a contract here (AT BIOS):
;   - AH=86h waits CX:DX microseconds, and the machine keeps taking its timer
;     interrupts meanwhile (the BIOS tick at 0040:006C advances across the wait).
;   - AH=83h AL=0 returns at once and later sets bit 7 of the byte at ES:BX;
;     AL=1 cancels it, and a cancelled event never posts.
;   - AH=4Fh, unhooked, returns CF=1 with AL unchanged ("process the key").
; Times are measured in BIOS ticks (18.2/s) and reported as BUCKETS, not raw counts,
; so a correct answer agrees on every host: 1 s is 18 ticks, +/- scheduling.
;
; nasm -f bin p_int15w.asm -o p_int15w.com

        org     100h
        jmp     start
%include "probe.inc"

; ticks -> bucket: 0 = under 4 ticks (no wait), 1 = 4..12, 2 = 13..24 (about 1 s),
; 3 = over 24. tick0 stores the tick count in [t0] (memory: EMIT may clobber
; registers); bucket returns the bucket of the ticks since then in AX.
tick0:  push    es
        push    ax
        xor     ax, ax
        mov     es, ax
        sti
        mov     ax, [es:046Ch]
        mov     [t0], ax
        pop     ax
        pop     es
        ret
since:  push    es                              ; AX = ticks since [t0]
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:046Ch]
        pop     es
        sub     ax, [t0]
        ret
bucket: call    since
        cmp     ax, 4
        jb      .b0
        cmp     ax, 13
        jb      .b1
        cmp     ax, 25
        jb      .b2
        mov     ax, 3
        ret
.b0:    xor     ax, ax
        ret
.b1:    mov     ax, 1
        ret
.b2:    mov     ax, 2
        ret

start:
        PROBE_BEGIN "int15w"

        ; ---- AH=86h, 1,000,000 us (CX:DX = 000F:4240). CF=0, and ~18 ticks pass.
        call    tick0
        mov     ax, 8600h
        mov     cx, 000Fh
        mov     dx, 4240h
        int     15h
        call    probe_capture
        EMIT    "int15w.86.status", "CF"
        call    bucket
        mov     [__ax], ax
        mov     word [__fl], 0
        EMIT    "int15w.86.1s.ticks_bucket", "AX"

        ; ---- AH=83h AL=0, 500,000 us (0007:A120): returns at once, then posts bit 7.
        mov     byte [flag], 0
        call    tick0
        mov     ax, 8300h
        mov     cx, 0007h
        mov     dx, 0A120h
        push    cs
        pop     es
        mov     bx, flag
        int     15h
        call    probe_capture
        EMIT    "int15w.83.start", "CF"
        call    bucket                          ; must still be "no wait"
        mov     [__ax], ax
        mov     word [__fl], 0
        EMIT    "int15w.83.returns_at_once", "AX"
        call    tick0                           ; poll the flag, at most ~3 s
.poll:  test    byte [flag], 80h
        jnz     .posted
        call    since
        cmp     ax, 55
        jb      .poll
.posted:
        xor     ax, ax
        mov     al, [flag]
        and     al, 80h
        mov     [__ax], ax
        mov     word [__fl], 0
        EMIT    "int15w.83.flag_bit7", "AX"
        call    bucket                          ; 0.5 s = 9 ticks -> bucket 1
        mov     [__ax], ax
        mov     word [__fl], 0
        EMIT    "int15w.83.half_s.ticks_bucket", "AX"

        ; ---- AH=83h AL=1 cancels: start 500 ms, cancel, wait 1 s, flag stays clear.
        mov     byte [flag], 0
        mov     ax, 8300h
        mov     cx, 0007h
        mov     dx, 0A120h
        push    cs
        pop     es
        mov     bx, flag
        int     15h
        mov     ax, 8301h
        int     15h
        call    probe_capture
        EMIT    "int15w.83.cancel", "CF"
        call    tick0
.wait1: call    since
        cmp     ax, 18
        jb      .wait1
        xor     ax, ax
        mov     al, [flag]
        mov     [__ax], ax
        mov     word [__fl], 0
        EMIT    "int15w.83.cancelled_flag", "AX"

        ; ---- AH=4Fh unhooked: CF=1, AL unchanged.
        POISON
        mov     ax, 4F1Eh
        stc
        int     15h
        call    probe_capture
        EMIT    "int15w.4f.default", "AX,CF"

        PROBE_END

flag    db      0
t0      dw      0
