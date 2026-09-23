; p_dma.com -- the 8237A's REGISTER FILE, as software can read it back.
;              docs/ref/dma.md; docs/inventory/dma.md.
;
; ── WHAT THIS IS FOR ─────────────────────────────────────────────────────────
; The transfer ENGINE is exercised continuously by Sound Blaster playback and is
; why Doom has audio. Nothing has ever asked about the REGISTER FILE, and
; tools/dostest/dma_test.c is an off-VM battery written against our own model --
; so it encodes our behaviour, not the datasheet's. Marking the code against the
; datasheet first predicted four gaps; this asks three real machines about the
; two that a DOS program can see without a device attached:
;
;   * THE FLIP-FLOP. The 8237 is a 16-bit device behind an 8-bit port, and ONE
;     flip-flop -- per CONTROLLER, shared between the address and count registers
;     -- decides which half you get. Write a known address and count, read them
;     back, and the pair says whether the model has the flip-flop or has two
;     independent ones.
;   * THE SPARE PAGE LATCHES. Seven of the sixteen ports at 80h-8Fh map to DMA
;     channels; the other nine are still REAL READ/WRITE LATCHES on a PC, because
;     the address decoder does not bother to leave them out. We answer FFh there,
;     which describes an empty bus rather than a machine.
;
; ── ⛔⛔⛔ THE SAFETY NOTE. THIS PROBE CAN DESTROY ITS OWN EVIDENCE. ───────────
; The run writes OUT.TXT to the floppy, and **THE FLOPPY IS DMA CHANNEL 2**. So:
; * NOTHING HERE TOUCHES CHANNEL 2, its page register (81h), or the mask.
; * NO MASTER CLEAR (port 0Dh). Master clear SETS EVERY MASK BIT -- it disables
;   the floppy, and the machine then cannot write the file that would have told
;   us what happened. A probe whose failure mode is "no output at all" is
;   indistinguishable from a harness fault, which is the one shape to avoid.
; * CHANNEL 1 IS THE VICTIM. It is the 8-bit channel a sound card would take, no
;   DOS component uses it, and no oracle here has a driver loaded that would.
;   Its page register is saved and restored anyway.
; * The clear-byte-pointer write (0Ch) IS used -- it is the documented way to put
;   the flip-flop in a known state and affects nothing else.
;
; ⚠ EVERY CASE IS DETERMINISTIC. No case reads a count while a transfer is in
;   flight: with channel 1 idle and masked by the BIOS, the current registers
;   hold exactly what was programmed, on any machine and on any day.
;
; ORACLE-ALSO: pcem   (a real AMI 486 BIOS -- see scripts/pcemoracle.py)
; nasm -f bin p_dma.asm -o p_dma.com

        org     100h
        jmp     start
%include "probe.inc"

; ---------------------------------------------------------------- state
pg1_was db      0                       ; channel 1's page, as we found it
spare   db      0                       ; port 80h's read-back

start:
        PROBE_BEGIN "dma"

; ════════════════════════════════════════════════════════════════════════════
; A. THE ADDRESS REGISTER THROUGH THE FLIP-FLOP.   ref/dma.md 1 and 2
;
;    Clear the byte pointer, write lo then hi, clear it again, read lo then hi.
;    A model with a working flip-flop returns what was written. One that ignores
;    the flip-flop returns the same half twice -- 3434h for a written 1234h.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 0                   ; 0Ch: clear byte pointer
        out     00Ch, al
        mov     al, 034h                ; channel 1 address, low half
        out     002h, al
        mov     al, 012h                ; ...and high
        out     002h, al

        mov     al, 0
        out     00Ch, al
        in      al, 002h
        mov     bl, al                  ; low
        in      al, 002h
        mov     bh, al                  ; high
        mov     ax, bx
        POISON
        call    probe_capture
        EMIT    "dma.ch1.addr", "AX"

; ════════════════════════════════════════════════════════════════════════════
; B. THE COUNT REGISTER SHARES THAT SAME FLIP-FLOP.   ref/dma.md 2
;
;    ★ THIS IS THE CASE WITH TEETH. The flip-flop is per CONTROLLER, not per
;      register: reading one half of the ADDRESS leaves it pointing at the high
;      half of the COUNT. So this case deliberately does NOT clear the pointer
;      between A and B -- case A left it back at "low" after two reads, and B
;      writes two bytes and reads two bytes, which only lines up if the model
;      shares one flip-flop the way the part does.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 078h                ; channel 1 count, low half
        out     003h, al
        mov     al, 056h                ; ...and high
        out     003h, al
        in      al, 003h
        mov     bl, al
        in      al, 003h
        mov     bh, al
        mov     ax, bx
        POISON
        call    probe_capture
        EMIT    "dma.ch1.count", "AX"

; ════════════════════════════════════════════════════════════════════════════
; C. A MAPPED PAGE REGISTER.   ref/dma.md 3
;
;    83h is channel 1's page. Plain storage, and the control for case D: if this
;    reads back and the spare does not, the difference is the decode and not the
;    port. Saved and restored.
; ════════════════════════════════════════════════════════════════════════════
        in      al, 083h
        mov     [pg1_was], al
        mov     al, 05Ah
        out     083h, al
        in      al, 083h
        mov     bl, al
        mov     al, [pg1_was]           ; put it back before anything else runs
        out     083h, al
        xor     ah, ah
        mov     al, bl
        POISON
        call    probe_capture
        EMIT    "dma.page.ch1", "AX"

; ════════════════════════════════════════════════════════════════════════════
; D. ★ A SPARE PAGE LATCH.   ref/dma.md 3
;
;    80h maps to no DMA channel, and a model that builds its port list from the
;    CHANNEL TABLE therefore has nothing there. On a PC it is a real latch -- the
;    decoder does not bother to leave it out -- and it doubles as the POST
;    diagnostic port, which is why writing it is harmless: POST codes go here all
;    through boot.
;    Expect 5Ah on a machine. FFh is an empty bus, which is what we answer.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 05Ah
        out     080h, al
        in      al, 080h
        mov     [spare], al
        xor     ah, ah
        mov     al, [spare]
        POISON
        call    probe_capture
        EMIT    "dma.page.spare80", "AX"

; ════════════════════════════════════════════════════════════════════════════
; E. THE STATUS REGISTER WITH NOTHING RUNNING.   ref/dma.md 4
;
;    Bits 3:0 are terminal count (latched, and CLEARED BY THIS READ); bits 7:4
;    are "a request is pending". With every channel idle both nibbles should be
;    quiet -- so the interesting part is the SECOND read: whatever the first one
;    latched, the second must be 0 unless something is genuinely requesting.
;    AH = the first read, AL = the second.
;    ⚠ READING STATUS IS DESTRUCTIVE -- it clears the TC bits, so this case takes
;      them away from anything else that was waiting on them. Nothing on these
;      machines is: no DMA device is active, which is the same reason the case is
;      deterministic.
; ════════════════════════════════════════════════════════════════════════════
        in      al, 008h
        mov     bh, al
        in      al, 008h
        mov     bl, al
        mov     ax, bx
        POISON
        call    probe_capture
        EMIT    "dma.status.idle", "AX"

        PROBE_END
