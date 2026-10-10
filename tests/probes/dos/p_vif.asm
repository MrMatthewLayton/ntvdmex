; p_vif.com -- DIAGNOSTIC, NTVDMEX only (not a dosdiff contract): #212.
;
; Hypothesis: XP keeps the VDM's virtual IF in FIXED_NTVDMSTATE (linear 0x714,
; bit 9) and reloads the guest's VIF from it when it returns to V86 after any
; ring-0 interruption (a thread switch, a hardware interrupt). If our VDM leaves
; that bit 0, an `sti` holds only until the next such return -- which is what
; p_pit0 / p_sti saw: IF=1 right after `sti`, IF=0 a moment later with no `cli`.
;
; Cases (all emitted as raw words; 0:0714 is DOS memory on a real PC, so this is
; meaningless on the oracles):
;   a.714      word at 0:0714 right after `sti`
;   a.if       FLAGS & 200h after ~0.5-1 s of pure CPU (no port I/O, no INT)
;   a.714late  0:0714 after that spin
;   b.if       the same spin after `sti` + setting bit 9 of 0:0714 ourselves
;   b.714late  0:0714 after it
; ticks are counted (INT 08h hooked, chained to the BIOS) for each spin.
;
; nasm -f bin p_vif.asm -o p_vif.com
        org     100h
        jmp     start
%include "probe.inc"

ticks   dw      0
old8    dd      0

isr8:   inc     word [cs:ticks]
        jmp     far [cs:old8]

; a long pure-CPU spin: 0x1000 x 64K `loop`s (~0.5-1 s on the test machine's CPU)
spin:   push    cx
        push    dx
        mov     dx, 1000h
.o:     xor     cx, cx
.i:     loop    .i
        dec     dx
        jnz     .o
        pop     dx
        pop     cx
        ret

rd714:  push    es
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:0714h]
        pop     es
        ret

start:
        PROBE_BEGIN "vif"
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
        call    rd714
        POISON
        call    probe_capture
        EMIT    "a.714", "AX"

        mov     word [ticks], 0
        sti
        call    spin
        pushf
        pop     ax
        and     ax, 0200h
        POISON
        call    probe_capture
        EMIT    "a.if", "AX"
        mov     ax, [ticks]
        POISON
        call    probe_capture
        EMIT    "a.ticks", "AX"
        call    rd714
        POISON
        call    probe_capture
        EMIT    "a.714late", "AX"

        mov     word [ticks], 0
        sti
        push    es
        xor     ax, ax
        mov     es, ax
        or      word [es:0714h], 0200h  ; the kernel's virtual IF, set by hand
        pop     es
        call    spin
        pushf
        pop     ax
        and     ax, 0200h
        POISON
        call    probe_capture
        EMIT    "b.if", "AX"
        mov     ax, [ticks]
        POISON
        call    probe_capture
        EMIT    "b.ticks", "AX"
        call    rd714
        POISON
        call    probe_capture
        EMIT    "b.714late", "AX"

        cli
        xor     ax, ax
        mov     es, ax
        mov     ax, [old8]
        mov     [es:8*4], ax
        mov     ax, [old8+2]
        mov     [es:8*4+2], ax
        sti
        PROBE_END
