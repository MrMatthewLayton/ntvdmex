; p_fdcreg.com -- ALL TEN DUMPREG BYTES, to settle a byte order I wrote from
;                 memory.  docs/ref/fdc.md 5; docs/inventory/fdc.md.
;
; ── WHY THIS EXISTS ──────────────────────────────────────────────────────────
; p_fdc.asm asked DUMPREG only "how many bytes, and what is the first" -- and got
; 0A01 from both oracles, which I read as "ten bytes, and PCN of drive 0 is 1".
; That reading, and the byte ORDER underneath it, came out of my memory of the
; datasheet and then went straight into vdd_fdc.c's DUMPREG arm and into
; ref/fdc.md. Nothing has checked it.
;
; ⛔ THAT IS THE EXACT SHAPE THIS PROJECT KEEPS GETTING WRONG: a register that is
;   implemented, plausible, and never compared. A wrong order does not fail -- it
;   hands a driver ten believable numbers in the wrong slots.
;
; Two machines both implement DUMPREG and both have a BIOS that has already
; programmed SPECIFY, so the ten bytes they return ARE the answer.
;
; ── ⛔ READ-ONLY. NOTHING HERE WRITES TO THE CHIP EXCEPT ONE COMMAND BYTE. ────
; DUMPREG (0Eh) has no parameters, no execution phase, no interrupt, no head
; movement and no side effects whatever -- it is the safest command on the part.
; ⛔ In particular this does NOT issue SPECIFY to pin the values, tempting as that
;   is: SPECIFY sets the step rate, the head load/unload times AND the non-DMA
;   bit, and a wrong non-DMA bit stops the drive the probe's own OUT.TXT goes to.
;   The BIOS's own values are enough to read the order off.
; Same idle-and-ready guard and same length-free drain as p_fdc.asm; a refusal
; emits a buffer of FFh rather than a plausible one.
;
; ⚠ THE NEXT LINE IS A BARE HOSTNAME ON PURPOSE. paritysweep.sh reads it, and
;   until 2026-09-23 it took THE REST OF THE LINE -- so a trailing comment here
;   word-split into `--host (a --host real --host AMI ...`, dosdiff rejected the
;   command line, and the probe silently LEFT THE SWEEP. Ten probes were out that
;   way. The parser now takes the first word, but the habit is cheap: put the
;   explanation on its own line, above.
; ORACLE-ALSO: pcem
; nasm -f bin p_fdcreg.asm -o p_fdcreg.com

        org     100h
        jmp     start
%include "probe.inc"

FDC     equ     03F0h

dump    times 10 db 0FFh                ; the ten bytes, FFh if never filled
dcount  db      0                       ; how many actually arrived
msr_rb  db      0

frd:
        push    dx
        mov     dl, al
        xor     dh, dh
        add     dx, FDC
        in      al, dx
        pop     dx
        ret

start:
        PROBE_BEGIN "fdcreg"

        ; ---- the chip must be idle and ready, or we ask nothing at all.
        mov     al, 4
        call    frd
        mov     [msr_rb], al
        and     al, 0D0h                ; RQM | DIO | CMD BSY
        cmp     al, 080h
        jne     .done

        mov     dx, FDC + 5
        mov     al, 00Eh                ; DUMPREG
        out     dx, al

        ; ---- wait, bounded, for CMD BSY to acknowledge it.
        mov     cx, 1000h
.wbsy:  mov     al, 4
        call    frd
        test    al, 010h
        jnz     .drain
        loop    .wbsy
        jmp     .done

        ; ---- drain by the rule that needs no length table.
.drain:
        mov     di, dump
        xor     bh, bh
        mov     cx, 0
.dr:    mov     al, 4
        call    frd
        test    al, 010h                ; CMD BSY still set?
        jz      .done
        and     al, 0C0h
        cmp     al, 0C0h                ; RQM=1, DIO=1 -> a byte is waiting
        jne     .next
        mov     al, 5
        call    frd
        cmp     bh, 10
        jae     .next                   ; never write past the buffer
        mov     [di], al
        inc     di
        inc     bh
.next:  loop    .dr
.done:
        mov     [dcount], bh

; ── THE TEN BYTES. Read them side by side across the hosts: the PCN block, the
;    two SPECIFY bytes, EOT, the LOCK/PERPENDICULAR byte, and the two CONFIGURE
;    bytes should be identifiable by shape alone.
        EMIT_BUF "fdc.dumpreg.bytes", dump, 10

        xor     ah, ah
        mov     al, [dcount]
        POISON
        call    probe_capture
        EMIT    "fdc.dumpreg.count", "AX"

        xor     ah, ah
        mov     al, [msr_rb]
        POISON
        call    probe_capture
        EMIT    "fdc.dumpreg.msr", "AX"

        PROBE_END
