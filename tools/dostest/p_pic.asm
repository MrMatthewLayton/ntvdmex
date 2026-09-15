; p_pic.com -- the 8259 as a HOOKED INTERRUPT HANDLER sees it: the mask, the
; in-service bit, and whether a handler can be re-entered before it EOIs.
;
; WHY THIS SURFACE. Two of the worst bugs this project has had lived here and both
; were invisible to every run report:
;   * IRQ0 was AUTO-EOI'd on delivery, so an IRQ0 raised during Lemmings' timer ISR
;     (which does `sti` then spins for the retrace) RE-ENTERED the handler, and every
;     tick nested one level deeper forever -- black screen, no music, stack overrun.
;   * Our INT 09h BOP arm never EOI'd IRQ1 at all, so a guest that chains to the BIOS
;     for ordinary keys typed ONE character and then went deaf.
; Neither is a wrong RETURN VALUE, so no amount of checking answers would find them.
; They are questions about WHEN, and the only instrument that asks is a handler.
;
; THE FOUR QUESTIONS, in the order they are asked below:
;   1. does writing a mask take, and read back?
;   2. is the in-service register clear when nothing is in service?
;   3. inside a hooked INT 08h: is IRQ0's ISR bit SET before the BIOS EOIs, CLEAR
;      after it, and is the handler ever RE-ENTERED?            <- the Lemmings bug
;   4. does masking IRQ0 actually stop delivery, and unmasking resume it?
;
; ⚠ EVERYTHING IS RESTORED. The vector is put back, both masks are put back, and the
;   PIC is left in read-IRR mode. A probe that leaves IRQ0 masked takes the machine
;   with it.
; ⚠ EVERY WAIT IS BOUNDED. A host that never delivers IRQ0 must make this probe
;   report "no ticks", not hang -- an absence that reads as a timeout is not data.
; ⚠ THE MASKED SPIN IS SELF-CALIBRATING: it first measures how long two ticks take on
;   THIS machine and then spins the same amount with IRQ0 masked. A fixed delay would
;   be too short on the rig or forever on the emulated 486.
;
; nasm -f bin p_pic.asm -o p_pic.com

        org     100h
        jmp     start
%include "probe.inc"

; ---- our INT 08h hook -----------------------------------------------------
; Chains to the BIOS (pushf + far call, which is what an INT does), so the BIOS
; still counts ticks and still sends the EOI. We only observe around it.
hdlr:
        push    ax
        push    ds
        push    cs
        pop     ds
        inc     word [tick_n]
        cmp     byte [in_hdlr], 0
        je      .fresh
        inc     word [nest_n]           ; ★ re-entered before we returned = the bug
.fresh:
        inc     byte [in_hdlr]
        cmp     byte [first], 0
        je      .chain
        mov     byte [first], 0
        mov     al, 0Bh                 ; OCW3: next read of 20h returns the ISR
        out     20h, al
        in      al, 20h
        mov     [isr_b], al             ; before the BIOS EOIs: bit 0 must be SET
        pushf
        call    far [old08]
        mov     al, 0Bh
        out     20h, al
        in      al, 20h
        mov     [isr_a], al             ; after it EOI'd: bit 0 must be CLEAR
        mov     al, 0Ah                 ; leave the PIC in read-IRR mode
        out     20h, al
        jmp     .out
.chain:
        pushf
        call    far [old08]
.out:
        dec     byte [in_hdlr]
        pop     ds
        pop     ax
        iret

; ---- helpers --------------------------------------------------------------

; spin1 -- one unit of delay, short enough to be fine-grained and long enough
; that the unit count stays inside a word.
spin1:
        push    cx
        mov     cx, 0FFFFh
.l:     loop    .l
        pop     cx
        ret

; bticks -> AX = the BIOS tick counter's low word (0040:006C)
bticks:
        push    es
        push    bx
        mov     ax, 40h
        mov     es, ax
        mov     ax, [es:6Ch]
        pop     bx
        pop     es
        ret

; calib -- how many spin1 units does it take for the BIOS tick to advance twice?
; Bounded, so a machine whose timer is dead comes back rather than hanging.
calib:
        call    bticks
        mov     [t0], ax
        xor     si, si
.l:     call    spin1
        inc     si
        call    bticks
        sub     ax, [t0]
        cmp     ax, 2
        jae     .done
        cmp     si, 20000               ; the bound. Generous: it only bites when the
        jb      .l                      ; timer is genuinely dead, and then it is the
                                        ; whole answer.
.done:
        mov     [iters], si
        ret

; spinN -- SI units of delay
spinN:
        or      si, si
        jz      .done
.l:     call    spin1
        dec     si
        jnz     .l
.done:  ret

; ---- cases ----------------------------------------------------------------

start:
        PROBE_BEGIN "pic"

        ; ---- 1. THE MASK. Write it, read it back, put it back. 0FCh leaves IRQ0 and
        ; IRQ1 enabled, so the machine keeps its timer and its keyboard while we look.
        cli
        in      al, 21h
        mov     [m_save], al
        mov     al, 0FCh
        out     21h, al
        in      al, 21h
        mov     [m_read], al
        mov     al, [m_save]
        out     21h, al                 ; restored before anything else can care
        sti
        xor     ah, ah
        mov     al, [m_read]
        mov     [__ax], ax
        mov     al, [m_save]
        xor     ah, ah
        mov     [__bx], ax              ; informational: what the machine had
        EMIT    "pic.mask.master", "AX"

        ; ---- the slave, the same way. 0FFh masks IRQ8-15 for a few microseconds;
        ; the RTC survives that.
        cli
        in      al, 0A1h
        mov     [s_save], al
        mov     al, 0FFh
        out     0A1h, al
        in      al, 0A1h
        mov     [m_read], al
        mov     al, [s_save]
        out     0A1h, al
        sti
        xor     ah, ah
        mov     al, [m_read]
        mov     [__ax], ax
        EMIT    "pic.mask.slave", "AX"

        ; ---- 2. NOTHING IS IN SERVICE out here, so the ISR must read zero. A host
        ; that leaves a bit stuck here is a host whose next interrupt of that
        ; priority never arrives.
        cli
        mov     al, 0Bh
        out     20h, al
        in      al, 20h
        mov     [isr_i], al
        mov     al, 0Ah                 ; back to read-IRR
        out     20h, al
        sti
        xor     ah, ah
        mov     al, [isr_i]
        mov     [__ax], ax
        EMIT    "pic.isr.idle", "AX"

        ; ---- 3. ★ THE HOOKED HANDLER. Install, let it take a few ticks, remove.
        mov     ax, 3508h
        int     21h
        mov     [old08], bx
        mov     [old08 + 2], es
        push    ds
        push    cs
        pop     ds
        mov     dx, hdlr
        mov     ax, 2508h
        int     21h
        pop     ds

        xor     si, si                  ; bounded wait for five ticks
.wait:
        cmp     word [tick_n], 5
        jae     .got
        call    spin1
        inc     si
        cmp     si, 20000
        jb      .wait
.got:
        push    ds                      ; ...and ALWAYS put the vector back
        lds     dx, [old08]
        mov     ax, 2508h
        int     21h
        pop     ds

        ; did it tick at all? (the exact count is a race; "five or more" is not)
        mov     ax, 1
        cmp     word [tick_n], 5
        jae     .ticked
        xor     ax, ax
.ticked:
        mov     [__ax], ax
        mov     ax, [tick_n]
        mov     [__bx], ax              ; informational
        EMIT    "pic.hook.ticked", "AX"

        ; ★ IRQ0's in-service bit while OUR handler is running, before the BIOS EOIs.
        xor     ah, ah
        mov     al, [isr_b]
        and     ax, 1
        mov     [__ax], ax
        mov     al, [isr_b]
        xor     ah, ah
        mov     [__bx], ax              ; informational: the whole byte
        EMIT    "pic.hook.isr.before", "AX"

        ; ...and after it. The BIOS's EOI is what clears it.
        xor     ah, ah
        mov     al, [isr_a]
        and     ax, 1
        mov     [__ax], ax
        mov     al, [isr_a]
        xor     ah, ah
        mov     [__bx], ax
        EMIT    "pic.hook.isr.after", "AX"

        ; ★★ WAS THE HANDLER EVER RE-ENTERED? This is the Lemmings bug in one number:
        ; with IRQ0 held in service until the EOI, it cannot be. With an auto-EOI on
        ; delivery, it is -- once per tick, for ever.
        mov     ax, [nest_n]
        mov     [__ax], ax
        EMIT    "pic.hook.nested", "AX"

        ; ---- 4. ★ DOES THE MASK ACTUALLY STOP DELIVERY? Calibrate first, so the
        ; masked spin is long enough to have seen ticks if any were coming.
        call    calib
        mov     ax, [iters]
        mov     [__ax], ax
        mov     ax, 1                   ; did the calibration see its two ticks?
        cmp     word [iters], 20000
        jb      .calok
        xor     ax, ax
.calok:
        mov     [__bx], ax
        EMIT    "pic.calib", "BX"       ; only "the timer runs at all" is comparable

        cli
        in      al, 21h
        mov     [m_save], al
        or      al, 1                   ; mask IRQ0
        out     21h, al
        sti
        call    bticks
        mov     [t0], ax
        mov     si, [iters]
        call    spinN
        call    bticks
        sub     ax, [t0]
        mov     [d_mask], ax
        cli                             ; ...and unmask, whatever happened
        mov     al, [m_save]
        out     21h, al
        sti
        mov     ax, [d_mask]
        mov     [__ax], ax
        EMIT    "pic.mask.blocks", "AX"

        ; ---- and delivery resumes once the mask is lifted.
        call    bticks
        mov     [t0], ax
        mov     si, [iters]
        call    spinN
        call    bticks
        sub     ax, [t0]
        mov     [d_free], ax
        mov     ax, 1
        cmp     word [d_free], 0
        ja      .resumed
        xor     ax, ax
.resumed:
        mov     [__ax], ax
        mov     ax, [d_free]
        mov     [__bx], ax              ; informational
        EMIT    "pic.mask.resumes", "AX"

        PROBE_END

; ---- data -----------------------------------------------------------------
old08    dd 0
tick_n   dw 0
nest_n   dw 0
iters    dw 0
t0       dw 0
d_mask   dw 0
d_free   dw 0
in_hdlr  db 0
first    db 1
isr_b    db 0FFh
isr_a    db 0FFh
isr_i    db 0FFh
m_save   db 0
m_read   db 0
s_save   db 0
