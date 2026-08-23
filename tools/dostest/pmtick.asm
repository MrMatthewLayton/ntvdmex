; pmtick.asm -- DOES THE KERNEL DELIVER IRQ0 TO A PROTECTED-MODE CLIENT BY ITSELF?
;
; WHY THIS EXISTS. It is the question the whole `pmkernel.flag` spike is for, and it
; has never been asked directly. The ceiling in ./return-ntvdm.md says that while PM
; runs IN-PROCESS via far-jmp the kernel can NEVER hand a hardware interrupt to a PM
; client (POPFD at CPL 3 cannot set VIF, and VdmpCanDeliver reads VIF, not IF). That
; is why the async SuspendThread/SetThreadContext injector had to be invented, and
; that injector is what tears the VDM down. But stock ntvdm demonstrably DOES deliver
; a timer interrupt to a DOS/4GW PM client on this very box, and `VdmStartExecution`
; DOES run protected mode. So: under the kernel, with nothing of ours injecting, does
; IRQ0 arrive at a PM handler?
;
;   YES -> the async injector and every failure it caused stop existing rather than
;          getting fixed, and the kernel path is worth its store-fault bugs.
;   NO  -> the store faults are not worth chasing and the spike is a dead end.
;
; ► RUN IT WITH BOTH `pmkernel.flag` AND `pmnoirq.flag` ON THE SHARE. pmnoirq is not
;   optional: without it OUR injector answers the question instead of the kernel, and
;   the probe reports a confident YES that means nothing.
;
; ► THE CONTROL CASE IS THE POINT, and it is the lesson pmfault.asm's `pmfin` paid
;   for: "the counter said zero" cannot distinguish "the kernel does not deliver"
;   from "the spin was too short for a single 55 ms tick to land". So the probe ALSO
;   reads the real-mode BIOS tick either side of the spin, via INT 31h 0300 -> INT 1Ah
;   AH=00 -- a path already proven to work from PM. Read the two numbers together:
;
;     RM delta > 0, PM count > 0   -> the kernel DELIVERS. Answer is yes.
;     RM delta > 0, PM count = 0   -> time passed and nothing arrived. Answer is no.
;     RM delta = 0                 -> THE PROBE IS INVALID, whatever PM count says.
;                                     The spin was too short; raise SPINHI.
;
; Assemble: nasm -f bin -o pmtick.com pmtick.asm

bits 16
org 0x100

; The spin is bounded twice over: it stops early once TICKGOAL ticks have arrived (so
; a working kernel answers in a fraction of a second), and gives up after SPINHI outer
; passes (so a silent one still reaches the report instead of the watchdog).
; ► THE SPIN BOUNDS ITSELF BY THE CLOCK, NOT BY A CYCLE COUNT I GUESSED.
;   First cut used a fixed iteration count and got it wrong in both directions: one
;   length reported "no real-mode ticks either" and 32x that length ran past the 45 s
;   watchdog. Deducing between the two runs (if the short spin had really been short,
;   32x it could not have wedged) showed the fixed count was never the problem -- the
;   INT 31h 0300 route I used to read the clock was returning 0 every time, so the
;   CONTROL was the thing lying. Both faults go away by asking the BIOS how much time
;   has actually passed, so the probe self-calibrates on any box.
TICKGOAL    equ 20                  ; PM handler entries that settle the question early
WALLTICKS   equ 36                  ; ~2 s at the default 18.2 Hz, then give up
MIDPASSES   equ 0x40                ; inner passes between clock checks (keeps INTs rare)

start:
    mov     dx, msg_start
    mov     ah, 0x09
    int     0x21

    ; ---- shrink to 64 KB so there is conventional memory left ------------------
    mov     ah, 0x4A
    mov     bx, 0x1000
    int     0x21

    ; ---- detect DPMI and get the mode-switch entry ----------------------------
    mov     ax, 0x1687
    int     0x2F
    test    ax, ax
    jnz     .nodpmi
    mov     [entry], di
    mov     [entry+2], es

    ; ---- switch to protected mode (AX bit0 = 0: 16-bit client) ----------------
    xor     ax, ax
    call    far [entry]
    jc      .noswitch

    ; ================= WE ARE NOW IN PROTECTED MODE ============================
    mov     dx, msg_inpm
    mov     ah, 0x09
    int     0x21

    ; Remember our data selector where the ISR can reach it. The ISR is entered
    ; with an unknown DS, and a code selector is not writable, so the handler has
    ; to load DS from somewhere readable: a word in the code image, read through
    ; CS. This store is the one place DS is known to be ours.
    mov     ax, ds
    mov     [dsel], ax

    ; ---- CONTROL, HALF ONE: the real-mode BIOS tick before the spin -----------
    call    rm_tick
    mov     [tick0], ax

    ; ---- hook IRQ0 in PROTECTED MODE ------------------------------------------
    ; 0205 = set protected-mode interrupt vector. CX:DX = our PM handler. Vector
    ; 08h is IRQ0 as the PIC is programmed at boot; this is the same vector Doom
    ; ends up owning, so a yes here is a yes for Doom.
    mov     ax, 0x0205
    mov     bl, 0x08
    mov     cx, cs
    mov     dx, isr
    int     0x31
    jc      .hookfail

    ; ---- let interrupts in ----------------------------------------------------
    mov     ax, 0x0901              ; get and ENABLE the virtual interrupt state
    int     0x31
    sti                             ; measured safe in PM (pmfault: STI SURVIVED)

    ; ---- the spin -------------------------------------------------------------
    ; Burn MIDPASSES x 65536 iterations, then ask the clock. The burn is what gives
    ; the kernel a long stretch of uninterrupted PM execution to interrupt; the clock
    ; check is what stops the probe outrunning the watchdog on a slow box.
.top:
    mov     bp, MIDPASSES
.outer:
    xor     cx, cx                  ; 0 -> 65536 iterations
.inner:
    loop    .inner
    dec     bp
    jnz     .outer
    cmp     word [count], TICKGOAL
    jae     .spun                   ; a working kernel answers in a fraction of a second
    call    rm_tick
    sub     ax, [tick0]
    cmp     ax, WALLTICKS
    jb      .top
.spun:
    cli

    ; ---- CONTROL, HALF TWO: the real-mode BIOS tick after the spin ------------
    call    rm_tick
    sub     ax, [tick0]
    mov     [rmdelta], ax

    ; ---- report ---------------------------------------------------------------
    mov     dx, msg_pmcount
    mov     ah, 0x09
    int     0x21
    mov     ax, [count]
    call    puthex
    mov     dx, msg_rmdelta
    mov     ah, 0x09
    int     0x21
    mov     ax, [rmdelta]
    call    puthex
    mov     dx, msg_tick0           ; the RAW clock too: a delta of 0 from a broken
    mov     ah, 0x09                ; clock and one from a fast spin look identical
    int     0x21
    mov     ax, [tick0]
    call    puthex
    mov     dx, msg_crlf
    mov     ah, 0x09
    int     0x21

    ; The verdict in words, so the log answers the question without arithmetic.
    cmp     word [rmdelta], 0
    je      .invalid
    cmp     word [count], 0
    je      .no
    mov     dx, msg_yes
    jmp     .say
.no:
    mov     dx, msg_no
    jmp     .say
.invalid:
    mov     dx, msg_invalid
.say:
    mov     ah, 0x09
    int     0x21
    mov     ax, 0x4C00
    int     0x21

.hookfail:
    mov     dx, msg_hookfail
    mov     ah, 0x09
    int     0x21
    mov     ax, 0x4C03
    int     0x21
.noswitch:
    mov     dx, msg_noswitch
    mov     ah, 0x09
    int     0x21
    mov     ax, 0x4C01
    int     0x21
.nodpmi:
    mov     dx, msg_nodpmi
    mov     ah, 0x09
    int     0x21
    mov     ax, 0x4C02
    int     0x21

; ---------------------------------------------------------------------------
; THE PROTECTED-MODE IRQ0 HANDLER. Counts, EOIs, returns.
; No chaining: this probe asks only whether the interrupt ARRIVES. The EOI is
; not optional though -- without it the PIC never unmasks IRQ0 again and a
; working kernel would still report exactly one tick, which reads like a bug.
; ---------------------------------------------------------------------------
isr:
    push    ax
    push    ds
    mov     ax, [cs:dsel]
    mov     ds, ax
    inc     word [count]
    mov     al, 0x20
    out     0x20, al                ; EOI to the master PIC (PM port I/O is reflected)
    pop     ds
    pop     ax
    iret

; ---------------------------------------------------------------------------
; rm_tick -- the BIOS tick counter, read FROM protected mode.
; Straight `INT 1Ah AH=00`: the host services vector 1Ah in PM directly
; (main.c, "BIOS time / timer tick in PM"), so this needs no 0300 round trip.
; The first cut DID go through 0300 and silently returned 0 every time, which
; made the control useless -- see the note at the top of the file.
; Returns the low word of the count in AX.
; ---------------------------------------------------------------------------
rm_tick:
    push    bx
    push    cx
    push    dx
    xor     ah, ah                      ; AH=00: read clock count -> CX:DX
    int     0x1A
    mov     ax, dx                      ; low word is all this probe needs
    pop     dx
    pop     cx
    pop     bx
    ret

; ---------------------------------------------------------------------------
; puthex -- print AX as four hex digits via INT 21h AH=02.
; ---------------------------------------------------------------------------
puthex:
    push    bx
    push    cx
    push    dx
    mov     bx, ax
    mov     cx, 4
.digit:
    rol     bx, 4
    mov     dl, bl
    and     dl, 0x0F
    add     dl, '0'
    cmp     dl, '9'
    jbe     .emit
    add     dl, 7
.emit:
    mov     ah, 0x02
    int     0x21
    loop    .digit
    pop     dx
    pop     cx
    pop     bx
    ret

; ---------------------------------------------------------------------------
dsel:           dw 0
entry:          dd 0
count:          dw 0
tick0:          dw 0
rmdelta:        dw 0


msg_start:      db 'PMTICK: start', 13, 10, '$'
msg_inpm:       db 'PMTICK: in protected mode; hooking IRQ0 and spinning', 13, 10, '$'
msg_pmcount:    db 'PMTICK: PM handler entries = 0x', '$'
msg_rmdelta:    db '   real-mode BIOS tick delta = 0x', '$'
msg_tick0:      db '   raw tick0 = 0x', '$'
msg_crlf:       db 13, 10, '$'
msg_yes:        db 'PMTICK: *** YES -- the kernel delivered IRQ0 to a PM handler ***', 13, 10, '$'
msg_no:         db 'PMTICK: *** NO -- time passed, nothing arrived in PM ***', 13, 10, '$'
msg_invalid:    db 'PMTICK: INVALID -- no real-mode ticks either; raise SPINHI', 13, 10, '$'
msg_hookfail:   db 'PMTICK: INT 31h 0205 failed (CF=1) -- could not hook IRQ0', 13, 10, '$'
msg_noswitch:   db 'PMTICK: mode switch FAILED (CF=1)', 13, 10, '$'
msg_nodpmi:     db 'PMTICK: no DPMI host', 13, 10, '$'
