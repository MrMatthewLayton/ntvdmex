; p_sti.com -- does STI make interrupts enabled, and when does that stop?   #212
;
; p_pit0.com caught NTVDMEX with FLAGS.IF = 0 straight after an `sti` (and so no
; IRQ0 could land). On any PC, `sti; pushf` has IF set, always. This narrows WHEN:
;   start.if        FLAGS at the first instruction (p_ifst's question, for context)
;   sti.if          `sti; pushf`, before anything else has happened
;   int21.if        after INT 21h AH=30h
;   int21.sti.if    `sti; pushf` after that INT 21h
;   spin.ticks      INT 08h hooked, `sti`, a pure CPU loop (no port I/O): did any
;                   timer tick land? (0 / 1 / 2 / 3 / 0x10 = many)
;   spin.if         `pushf` right after that loop (no cli since the sti)
;   port.ticks      the same with an `in al,61h` in the loop -- the trap path (#172)
;   port.if         ...and FLAGS after it
; Oracles: every *.if is 0200h; the tick counts depend on CPU speed, so they are
; recorded, not held (see oracle-rules.json).
;
; nasm -f bin p_sti.asm -o p_sti.com
        org     100h
        pushf                           ; FIRST instruction
        pop     word [fstart]
        jmp     start
%include "probe.inc"

fstart  dw      0
ticks   dw      0
old8    dd      0

isr8:   inc     word [cs:ticks]
        jmp     far [cs:old8]           ; chain: the BIOS EOIs and keeps its tick

bucket:                                 ; AX -> 0..3, else 0x10
        cmp     ax, 3
        jbe     .r
        mov     ax, 010h
.r:     ret

; ~a few hundred ms of pure CPU on a slow machine, less on a fast one.
spin:   push    cx
        push    dx
        mov     dx, 40h
.o:     xor     cx, cx
.i:     loop    .i
        dec     dx
        jnz     .o
        pop     dx
        pop     cx
        ret

; the same shape with a port read per 64 iterations
spinport:
        push    cx
        push    dx
        push    bx
        mov     dx, 400h
.o:     mov     cx, 64
.i:     loop    .i
        in      al, 061h
        dec     dx
        jnz     .o
        pop     bx
        pop     dx
        pop     cx
        ret

start:
        PROBE_BEGIN "sti"
        mov     ax, [fstart]
        and     ax, 0200h
        POISON
        call    probe_capture
        EMIT    "start.if", "AX"

        sti
        pushf
        pop     ax
        and     ax, 0200h
        POISON
        call    probe_capture
        EMIT    "sti.if", "AX"

        mov     ax, 3000h
        int     21h
        pushf
        pop     ax
        and     ax, 0200h
        POISON
        call    probe_capture
        EMIT    "int21.if", "AX"

        sti
        pushf
        pop     ax
        and     ax, 0200h
        POISON
        call    probe_capture
        EMIT    "int21.sti.if", "AX"

        cli
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:8*4]
        mov     [old8], ax
        mov     ax, [es:8*4+2]
        mov     [old8+2], ax
        mov     word [es:8*4], isr8
        mov     [es:8*4+2], cs
        mov     word [ticks], 0
        sti
        call    spin
        pushf
        pop     bx
        mov     ax, [ticks]
        call    bucket
        push    bx
        POISON
        call    probe_capture
        EMIT    "spin.ticks", "AX"
        pop     ax
        and     ax, 0200h
        POISON
        call    probe_capture
        EMIT    "spin.if", "AX"

        cli
        mov     word [ticks], 0
        sti
        call    spinport
        pushf
        pop     bx
        mov     ax, [ticks]
        call    bucket
        push    bx
        POISON
        call    probe_capture
        EMIT    "port.ticks", "AX"
        pop     ax
        and     ax, 0200h
        POISON
        call    probe_capture
        EMIT    "port.if", "AX"

        cli
        xor     ax, ax
        mov     es, ax
        mov     ax, [old8]
        mov     [es:8*4], ax
        mov     ax, [old8+2]
        mov     [es:8*4+2], ax
        sti
        PROBE_END
