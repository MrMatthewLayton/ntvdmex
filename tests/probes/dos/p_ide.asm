; p_ide.com -- the IDE/ATA adapter at 1F0h/3F6h and 170h/376h, asked the way a
;              DETECTION ROUTINE asks.  GH #179; docs/inventory/ide.md; vdd_ide.h.
;
; ── WHAT THIS IS FOR ─────────────────────────────────────────────────────────
; Until #179 nothing claimed the ATA blocks, every register read FFh, and FFh in
; the status register is BSY=1. The ATA's own wait -- `test al,80h / jnz` --
; then never exits. Case A measures exactly that loop, bounded, on both channels.
;
; ⚠ THE ORACLES HAVE DRIVES AND WE DELIBERATELY DO NOT (ide.md decision (b): an
;   adapter with both channels empty; INT 13h exposes no fixed disk by design).
;   So the raw register values below are MACHINE FACTS, not contracts, and
;   oracle-rules.json abstains them with that rationale. The CONTRACT rows are
;   the ones that hold whether or not a drive is attached:
;     ide.*.bsywait   the canonical BSY wait terminates
;     ide.*.alt.bsy   alternate status bit 7 is clear on an idle channel
;
; ── ⛔ THE SAFETY NOTE. THE ORACLES BOOT AND WRITE THROUGH THE BIOS. ─────────
; * NOTHING HERE WRITES 3F6h/376h (device control). SRST would reset the
;   oracle's drive under its BIOS; nIEN would stop its interrupts.
; * NO COMMAND IS WRITTEN to 1F7h/177h. A command is a conversation the BIOS
;   did not start and would have to finish.
; * The only writes are Sector Count / Sector Number (the presence test) and
;   Device/Head (to select device 1 and back). The BIOS reprograms all three
;   before every command it issues, so nothing it relies on is left behind --
;   and Device/Head is restored to A0h (device 0, CHS) before the next case.
; * STATUS (1F7h) IS READ ONCE, LAST, per channel: reading it acknowledges a
;   pending INTRQ. Everything else reads the ALTERNATE status, which has no
;   side effects.
;
; ORACLE-ALSO: pcem   (a real AMI 486 BIOS -- see scripts/pcemoracle.py)
; nasm -f bin p_ide.asm -o p_ide.com

        org     100h
        jmp     start
%include "probe.inc"

; ---------------------------------------------------------------- state
base    dw      0                       ; command block of the channel under test
ctl     dw      0                       ; its alternate status / device control
res     db      0                       ; scratch result byte
res2    db      0

; ---- ain: AL = alternate status (no side effects)
ain:
        push    dx
        mov     dx, [ctl]
        in      al, dx
        pop     dx
        ret

; ---- 400 ns settle after a Device/Head write: four alternate-status reads,
;      which is the documented way to wait without a timer.
settle:
        call    ain
        call    ain
        call    ain
        call    ain
        ret

; ---- bsywait: the datasheet's loop, bounded to 65536 turns.
;      AX = 0000 it exited, 0001 it would never have.
bsywait:
        push    cx
        push    dx
        mov     dx, [ctl]
        xor     cx, cx                  ; 65536
.l:     in      al, dx
        test    al, 80h
        jz      .out
        loop    .l
        mov     ax, 1
        jmp     .r
.out:   xor     ax, ax
.r:     pop     dx
        pop     cx
        ret

; ---- EMITC prefix, suffix: EMIT "prefix.suffix" with SIG=AX.
%macro EMITC 2
%strcat __emitc_name %1, %2
        EMIT    __emitc_name, "AX"
%endmacro

; ---- one channel, [base]/[ctl] set. NAME is the case prefix.
%macro CHANNEL 1
        ; A. does the BSY wait exit?  CONTRACT.
        call    bsywait
        POISON
        call    probe_capture
        EMITC   %1, ".bsywait"

        ; B. alternate status bit 7.  CONTRACT.
        call    ain
        and     al, 80h
        xor     ah, ah
        POISON
        call    probe_capture
        EMITC   %1, ".alt.bsy"

        ; C. alternate status, whole byte.  MACHINE FACT (drive or not).
        call    ain
        xor     ah, ah
        POISON
        call    probe_capture
        EMITC   %1, ".alt.raw"

        ; D. the presence test: write 55h/AAh to Sector Count/Number, read back.
        ;    AH = sector count read, AL = sector number read. A drive echoes
        ;    5555..AA; an empty channel does not. MACHINE FACT.
        mov     dx, [base]
        add     dx, 2
        mov     al, 55h
        out     dx, al
        inc     dx
        mov     al, 0AAh
        out     dx, al
        dec     dx
        in      al, dx
        mov     [res], al
        inc     dx
        in      al, dx
        mov     [res2], al
        mov     ah, [res]
        mov     al, [res2]
        POISON
        call    probe_capture
        EMITC   %1, ".echo"

        ; E. device 1 selected: its alternate status. ATA-3 8.7.1(h): with
        ;    device 1 absent, device 0 answers 00h after reset. MACHINE FACT
        ;    (whether device 1 exists), informational.
        mov     dx, [base]
        add     dx, 6
        mov     al, 0B0h
        out     dx, al
        call    settle
        call    ain
        mov     [res], al
        mov     al, 0A0h                ; back to device 0 before anything else
        out     dx, al
        call    settle
        xor     ah, ah
        mov     al, [res]
        POISON
        call    probe_capture
        EMITC   %1, ".dev1.alt"

        ; F. status (1F7h) against alternate status: the same register through
        ;    two doors. AH = status, AL = alternate. MACHINE FACT for the value;
        ;    the two halves agree on every host.
        mov     dx, [base]
        add     dx, 7
        in      al, dx
        mov     [res], al
        call    ain
        mov     ah, [res]
        POISON
        call    probe_capture
        EMITC   %1, ".status.alt"
%endmacro

start:
        PROBE_BEGIN "ide"

        mov     word [base], 1F0h
        mov     word [ctl], 3F6h
        CHANNEL "ide.pri"

        mov     word [base], 170h
        mov     word [ctl], 376h
        CHANNEL "ide.sec"

        PROBE_END
