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
;   be too short on the test machine or forever on the emulated 486.
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
        jne     .do_first
        cmp     byte [second], 0
        jne     .do_second
        jmp     .chain
.do_first:
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

; ── THE OCW3 QUESTIONS, ASKED WITH IRQ0 GENUINELY IN SERVICE. ────────────────
;    Both need a non-zero ISR to be discriminating, and the only place one exists
;    is inside a handler before the EOI. They run on a LATER tick than the
;    isr_b/isr_a pair above so they cannot disturb it.
.do_second:
        mov     byte [second], 0
        ; ── POLL. A POLL READ AND A STATUS READ ARE THE SAME `IN` ON THE SAME
        ;    PORT -- only the last OCW3 tells them apart, which is exactly why a
        ;    host that drops the P bit fails SILENTLY. Select the ISR first, so a
        ;    host that ignores P answers 01 (IRQ0 in service) while one that
        ;    honours it answers 00 (bit 7 clear: nothing PENDING -- in-service is
        ;    not pending). 0Ch = OCW3 with P=1 and RR=0, so the select is
        ;    deliberately left alone.
        mov     al, 0Bh
        out     20h, al
        mov     al, 0Ch
        out     20h, al
        in      al, 20h
        mov     [poll_b], al
        ; ── ROTATE ON SPECIFIC EOI (E0h + level 0). Whatever it does to the
        ;    priorities, it is an EOI: the ISR bit must go. A host that files it
        ;    under "other rotate forms: nop" leaves IRQ0 in service for ever and
        ;    that priority level dies silently.
        ;    ⚠ SAFE EITHER WAY: we chain to the BIOS below, whose own non-specific
        ;      EOI clears the bit if this did not, and is a no-op if it did.
        mov     al, 0E0h
        out     20h, al
        mov     al, 0Bh
        out     20h, al
        in      al, 20h
        mov     [rot_b], al
        mov     al, 0Ah                 ; leave the PIC in read-IRR mode
        out     20h, al
        jmp     .chain
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

        ; ════════════════════════════════════════════════════════════════════
        ; ★ OCW3's POLL COMMAND.   ref/pic.md 5
        ;   Measured inside the handler, IRQ0 in service, ISR pre-selected.
        ;   A chip that implements P answers 00: bit 7 is "an interrupt is
        ;   PENDING", and one already in service is not pending. A chip that
        ;   drops the P bit hands back the register the last OCW3 selected --
        ;   01, the ISR -- which a polling guest reads as
        ;   "bit 7 clear, no interrupt", i.e. the right shape and the wrong fact.
        ; ════════════════════════════════════════════════════════════════════
        xor     ah, ah
        mov     al, [poll_b]
        POISON
        call    probe_capture
        EMIT    "pic.ocw3.poll", "AX"

        ; ════════════════════════════════════════════════════════════════════
        ; ★ ROTATE ON SPECIFIC EOI IS STILL AN EOI.   ref/pic.md 4
        ;   E0h+level ends the interrupt AND rotates priority. The rotation is
        ;   rarely used; the EOI half is not optional. Emits the ISR read back
        ;   immediately after, so 00 = the bit went, 01 = IRQ0 is still in
        ;   service and its priority level has just died.
        ; ════════════════════════════════════════════════════════════════════
        xor     ah, ah
        mov     al, [rot_b]
        POISON
        call    probe_capture
        EMIT    "pic.ocw2.rot.speoi", "AX"

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

; ════════════════════════════════════════════════════════════════════════════
; ★ WHAT ICW1 RESETS BESIDES THE MASK.   ref/pic.md 3
;
;   The datasheet gives ICW1 six side effects. Two of them are guest-visible on
;   a PC and independent of each other: it CLEARS THE IMR, and it SETS THE
;   STATUS READ BACK TO IRR. A model can easily do the first and not the second,
;   and then a guest that selected the ISR, re-initialised, and read 20h gets an
;   ISR byte where the hardware gives an IRR byte.
;
;   The catch is that both registers are usually ZERO, so the case has to
;   MANUFACTURE a difference. It does that without needing a clock: mask IRQ0,
;   and the timer's requests pile up in the IRR with nothing ever delivered, so
;   IRR != 0 while ISR stays 0. The wait is self-timing -- it polls the IRR
;   itself, bounded -- so it costs exactly as long as one tick on any host and
;   cannot hang on one where nothing latches.
;
;   AH = the base-port read taken straight after ICW1..ICW4, with NO OCW3 in
;        between: the IRR on a correct host, the still-selected ISR on ours.
;   AL = the IRR, explicitly selected -- which also answers a second question
;        our code decides with no evidence: does ICW1 clear the IRR at all?
;
; ⚠⚠ THIS REPROGRAMS THE MASTER PIC ON A BARE-METAL TEST MACHINE. The sequence written
;    is exactly what a PC BIOS writes (11h 08h 04h 01h: edge-triggered,
;    cascaded, 8086 mode, no auto-EOI), so the chip ends where it started, and
;    the IMR is saved beforehand and restored immediately after -- ICW1 clears
;    it, so skipping that restore would leave the machine deaf. Interrupts stay
;    ENABLED throughout: nothing here is delivered while IRQ0 is masked, and a
;    CLI window around a four-port sequence buys nothing.
; ════════════════════════════════════════════════════════════════════════════
        in      al, 021h
        mov     [imr_sav], al
        or      al, 001h                ; mask IRQ0: requests latch, none deliver
        out     021h, al

        mov     cx, 0FFFFh              ; bounded: a host that never latches the
.icwwait:                               ; IRR must not hang the probe
        mov     al, 00Ah                ; OCW3: select IRR
        out     020h, al
        in      al, 020h
        test    al, 001h
        jnz     .icwgot
        loop    .icwwait
.icwgot:
        mov     al, 00Bh                ; OCW3: select ISR -- the thing ICW1 must undo
        out     020h, al

        mov     al, 011h                ; ICW1: ICW4 to follow, cascaded, edge
        out     020h, al
        mov     al, 008h                ; ICW2: master vector base = INT 08h
        out     021h, al
        mov     al, 004h                ; ICW3: a slave on IR2
        out     021h, al
        mov     al, 001h                ; ICW4: 8086 mode, no auto-EOI
        out     021h, al
        ; ⛔ THE MASK GOES BACK **WITH IRQ0 STILL MASKED**, and the first cut of
        ;   this case got it wrong. Restoring the real IMR here unmasks IRQ0, and
        ;   with interrupts enabled the latched request is DELIVERED before the
        ;   next instruction -- consuming the very IRR bit the case exists to
        ;   read, and making the answer depend on how fast the host vectors.
        ;   PCem duly disagreed with the other two on a row that was measuring
        ;   its interrupt latency rather than its ICW1.
        mov     al, [imr_sav]
        or      al, 001h
        out     021h, al

        in      al, 020h                ; NO OCW3 first -- that is the question
        mov     [icw_raw], al
        mov     al, 00Ah                ; now ask for the IRR explicitly
        out     020h, al
        in      al, 020h
        mov     [icw_irr], al

        mov     al, [imr_sav]           ; NOW let the timer back in
        out     021h, al

        mov     ah, [icw_raw]
        mov     al, [icw_irr]
        POISON
        call    probe_capture
        EMIT    "pic.icw1.readsel", "AX"

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
second   db 1                           ; the SECOND serviced tick does the OCW3 work
poll_b   db 0FFh                        ; base-port read after an OCW3 POLL
rot_b    db 0FFh                        ; ISR after a rotate-on-specific EOI
imr_sav  db 0
icw_raw  db 0FFh                        ; base-port read straight after ICW1..ICW4
icw_irr  db 0FFh                        ; ...and the IRR, explicitly selected
isr_i    db 0FFh
m_save   db 0
m_read   db 0
s_save   db 0
