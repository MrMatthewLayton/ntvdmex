; p_irq8.com -- does an interrupt on the SLAVE 8259 reach a hooked handler?  (s80)
;
; WHY. Device IRQs were latched and delivered for the master's lines 2-7 only, though
; the 8259 model has the slave and its cascade. North star 2 puts a GUS on IRQ 11, so
; slave delivery has to work -- and the one slave device the machine already has is the
; MC146818 RTC, whose periodic interrupt is IRQ 8 -> INT 70h.
;
; WHAT. Hook INT 70h; set the periodic rate (register A, RS=6 -> 1024 Hz) and PIE
; (register B bit 6); unmask IRQ 8 at the slave (A1h bit 0) and the cascade at the
; master (21h bit 2); let about five BIOS ticks pass. The handler acknowledges the chip
; the way the datasheet says -- READ register C -- then EOIs the slave and the master.
; Everything is restored: B, A, both masks, the vector.
;
; ROWS.
;   irq8.fired      AX = 1 if the handler ran at least 8 times (any real machine: ~280)
;   irq8.regc.pf    AX = register C bit 6 (PF) as the handler saw it on its first entry
;   irq8.nested     AX = times the handler was entered while already running (must be 0)
;
; nasm -f bin p_irq8.asm -o p_irq8.com

        org     100h
        jmp     start
%include "probe.inc"

; ---- our INT 70h handler ----------------------------------------------------
hdlr:
        push    ax
        push    ds
        push    cs
        pop     ds
        cmp     byte [in_hdlr], 0
        je      .fresh
        inc     word [nest_n]
.fresh:
        inc     byte [in_hdlr]
        inc     word [fire_n]
        mov     al, 0Ch                 ; read register C: this IS the acknowledge
        out     70h, al
        jmp     short $+2
        in      al, 71h
        cmp     byte [first], 0
        je      .eoi
        mov     byte [first], 0
        mov     [regc1], al
.eoi:
        mov     al, 20h                 ; non-specific EOI: slave, then master
        out     0A0h, al
        out     20h, al
        dec     byte [in_hdlr]
        pop     ds
        pop     ax
        iret

old70   dd      0
fire_n  dw      0
nest_n  dw      0
in_hdlr db      0
first   db      1
regc1   db      0
sav_a   db      0
sav_b   db      0
sav_m   db      0
sav_s   db      0

cmos_rd:                                ; AL = index -> AL = value (NMI stays enabled)
        and     al, 07Fh
        out     70h, al
        jmp     short $+2
        in      al, 71h
        ret
cmos_wr:                                ; AH = index, AL = value
        push    ax
        mov     al, ah
        and     al, 07Fh
        out     70h, al
        jmp     short $+2
        pop     ax
        out     71h, al
        ret
bticks:
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     ax, [046Ch]
        pop     ds
        ret

start:
        PROBE_BEGIN "irq8"

        mov     ax, 3570h               ; save INT 70h
        int     21h
        mov     [old70], bx
        mov     [old70+2], es
        mov     dx, hdlr
        mov     ax, 2570h
        int     21h

        cli
        in      al, 21h
        mov     [sav_m], al
        in      al, 0A1h
        mov     [sav_s], al
        mov     al, 0Ah
        call    cmos_rd
        mov     [sav_a], al
        mov     al, 0Bh
        call    cmos_rd
        mov     [sav_b], al
        mov     al, [sav_a]             ; rate select 6 = 1024 Hz, keep the divider bits
        and     al, 0F0h
        or      al, 06h
        mov     ah, 0Ah
        call    cmos_wr
        mov     al, [sav_b]             ; PIE on
        or      al, 40h
        mov     ah, 0Bh
        call    cmos_wr
        mov     al, 0Ch                 ; clear anything already latched
        call    cmos_rd
        in      al, 0A1h                ; unmask IRQ 8 ...
        and     al, 0FEh
        out     0A1h, al
        in      al, 21h                 ; ... and the cascade
        and     al, 0FBh
        out     21h, al
        sti

        call    bticks                  ; about five BIOS ticks, bounded by the tick itself
        mov     cx, ax
.wait:
        call    bticks
        sub     ax, cx
        cmp     ax, 5
        jb      .wait

        cli
        mov     al, [sav_b]             ; PIE back as it was
        mov     ah, 0Bh
        call    cmos_wr
        mov     al, [sav_a]
        mov     ah, 0Ah
        call    cmos_wr
        mov     al, 0Ch
        call    cmos_rd
        mov     al, [sav_s]
        out     0A1h, al
        mov     al, [sav_m]
        out     21h, al
        sti
        push    ds
        lds     dx, [old70]
        mov     ax, 2570h
        int     21h
        pop     ds

        POISON
        xor     ax, ax
        cmp     word [fire_n], 8
        jb      .n1
        mov     ax, 1
.n1:    mov     [__ax], ax
        mov     ax, [fire_n]
        mov     [__bx], ax              ; informational: the count itself is a race
        EMIT    "irq8.fired", "AX"

        POISON
        xor     ax, ax
        test    byte [regc1], 40h
        jz      .n2
        mov     ax, 1
.n2:    mov     [__ax], ax
        EMIT    "irq8.regc.pf", "AX"

        POISON
        mov     ax, [nest_n]
        mov     [__ax], ax
        EMIT    "irq8.nested", "AX"

        PROBE_END
