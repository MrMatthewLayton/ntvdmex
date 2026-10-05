; p_fdc.com -- the 82077AA at 3F0h, asked the way a DETECTION ROUTINE asks.
;              docs/ref/fdc.md; docs/inventory/fdc.md.
;
; ── WHAT THIS IS FOR ─────────────────────────────────────────────────────────
; Nothing in this project claims ports 3F0h-3F7h. INT 13h is serviced entirely in
; the host out of an image file, so the floppy CONTROLLER has never existed, and
; every port in the block falls through to the unclaimed default of FFh.
;
; For this one chip that default is the worst possible answer. 3F4h is the Main
; Status Register and FFh there reads as RQM=1, DIO=1 -- "ready, and I am the one
; talking". The command-write loop out of the datasheet is
;
;       wait:   in al,3F4h / and al,0C0h / cmp al,80h / jne wait
;
; and C0h never equals 80h, so it spins for ever with no fault and no timeout.
; That is the MC146818's UIP bit one surface later. Case B measures exactly that
; loop, bounded, and reports whether it would ever have exited.
;
; ── ⛔⛔⛔ THE SAFETY NOTE. THIS PROBE CAN DESTROY ITS OWN EVIDENCE. ───────────
; The run writes OUT.TXT to the scratch floppy on two of the four hosts, and the
; floppy IS this chip. Worse than the 8237 case: the FDC is not a register file,
; it is a CONVERSATION with no framing and no timeout, so a half-issued command
; eats the BIOS's next command byte as one of its own parameters. There is no
; resynchronisation short of a reset, and a reset is itself one of the things
; that can kill the drive. So:
;
; * NOTHING HERE WRITES 3F2h (DOR). Bit 2 is an active-low RESET, bit 3 gates
;   IRQ6 and the DMA request, bits 7:4 are the motors. A plausible-looking "turn
;   it all off" of 00h does all four at once and the machine never writes a file
;   again.
; * NOTHING HERE WRITES 3F4h (DSR) OR 3F7h (CCR). Both carry the data rate, and
;   the wrong data rate makes every subsequent access fail. DSR bit 7 is also a
;   software reset.
; * NO COMMAND THAT MOVES A HEAD, TRANSFERS DATA, OR CHANGES CONFIGURATION.
;   No SEEK, RECALIBRATE, READ ID, READ/WRITE/FORMAT; and no SPECIFY, CONFIGURE,
;   PERPENDICULAR or LOCK, which would overwrite what the BIOS programmed.
; * ONLY TWO COMMANDS ARE ISSUED AT ALL -- VERSION (10h) and DUMPREG (0Eh) --
;   and both are stateless: no parameters, no execution phase, no interrupt, no
;   head movement, and nothing left behind. VERSION is what every driver asks to
;   decide whether the enhanced command set exists.
; * ⛔ SENSE INTERRUPT STATUS (08h) IS DELIBERATELY NOT ASKED, although it is the
;   command that would say most about reset behaviour. It CONSUMES a pending
;   interrupt, and if the BIOS has one outstanding from the read that loaded this
;   program, taking it hands the BIOS a machine whose disk operation never
;   finishes. The one command worth most is the one that can eat state somebody
;   else is about to read.
;
; ── THE DESYNC GUARD ─────────────────────────────────────────────────────────
; Every command is refused unless MSR says RQM=1, DIO=0 and CMD BSY=0 -- idle and
; ready for a command byte. Every result phase is drained by the rule that works
; without a length table: read 3F5h while RQM=1 && DIO=1, stop when CMD BSY
; clears. If a bounded drain expires the chip is mid-conversation, `desync` is
; set, and EVERY LATER COMMAND IS REFUSED rather than made worse. A refusal emits
; FFFF, which says "we did not ask" and is not confusable with an answer.
;
; ⚠ On NTVDMEX today the guard fires on its own: MSR reads FFh, FFh & D0h is D0h,
;   which is not 80h, so the command cases refuse and NOTHING is written to 3F5h
;   on the host with no chip. That is the correct behaviour and it is measured,
;   not assumed -- the case emits the refusal.
;
; ⚠ THE MOTOR BITS ARE MASKED OUT of the DOR read-back. Whether a motor is still
;   spinning depends on how long ago DOS last touched the disk and on the BIOS's
;   two-second countdown, so it measures the moment rather than the machine.
;
; ORACLE-ALSO: pcem   (a real AMI 486 BIOS -- see scripts/pcemoracle.py)
; nasm -f bin p_fdc.asm -o p_fdc.com

        org     100h
        jmp     start
%include "probe.inc"

FDC     equ     03F0h

; ---------------------------------------------------------------- state
msr_rb  db      0                       ; MSR, untouched, as the BIOS left it
msr_pr  db      0                       ; MSR & C0h after the canonical loop
cmdwait db      0                       ; 1 = that loop would never have exited
dor_rb  db      0                       ; DOR bits 3:0 (motors masked off)
dir_rb  db      0                       ; DIR, whole byte
sra_rb  db      0                       ; 3F0h -- driven only in PS/2 mode
srb_rb  db      0                       ; 3F1h -- ditto
alt_rb  db      0                       ; 3F6h -- NOT the FDC's; the ATA side
ver_ax  dw      0                       ; VERSION:  AH = result count, AL = ST/ID
dmp_ax  dw      0                       ; DUMPREG:  ditto
desync  db      0                       ; the chip was left mid-command
got1    db      0                       ; first result byte captured

; ---------------------------------------------------------------- helpers

; FDC + AL -> AL.
frd:
        push    dx
        mov     dl, al
        xor     dh, dh
        add     dx, FDC
        in      al, dx
        pop     dx
        ret

; AL -> FDC + AH.
fwr:
        push    dx
        mov     dl, ah
        xor     dh, dh
        add     dx, FDC
        out     dx, al
        pop     dx
        ret

; ── fdc_cmd: issue the stateless command in BL and drain its result phase.
;    in : BL = command byte
;    out: AH = number of result bytes read, AL = the FIRST of them (FFh if none)
;         AX = FFFFh if the command was REFUSED -- either the chip was not idle
;         and ready, or an earlier drain left it desynced.
;    ⛔ This is the only routine in the file that writes to the chip.
fdc_cmd:
        cmp     byte [desync], 0
        jne     .refuse
        mov     al, 4
        call    frd
        and     al, 0D0h                ; RQM | DIO | CMD BSY
        cmp     al, 080h                ; ready for a command byte, none running
        jne     .refuse

        mov     al, bl                  ; the command byte
        mov     ah, 5
        call    fwr

        ; ---- wait, bounded, for CMD BSY to acknowledge the byte. If it never
        ;      sets, nothing was consumed and "0 result bytes" is the truth.
        mov     cx, 1000h
.wbsy:  mov     al, 4
        call    frd
        test    al, 010h
        jnz     .drain
        loop    .wbsy
        xor     ax, ax                  ; AH=0 bytes, AL=00: the chip ignored it
        ret

        ; ---- drain by the rule that needs no length table: read while
        ;      RQM=1 && DIO=1, stop when CMD BSY clears.
.drain:
        xor     bh, bh                  ; count
        mov     bl, 0FFh                ; first byte, if one ever arrives
        mov     byte [got1], 0
        mov     cx, 0                   ; 65536 polls -- far more than 10 bytes
.dr:    mov     al, 4
        call    frd
        test    al, 010h                ; CMD BSY still set?
        jz      .done                   ; no -> the result phase is over
        and     al, 0C0h
        cmp     al, 0C0h                ; RQM=1, DIO=1 -> a byte is waiting
        jne     .next
        mov     al, 5
        call    frd
        inc     bh
        cmp     byte [got1], 0
        jne     .next
        mov     byte [got1], 1
        mov     bl, al
.next:  loop    .dr
        ; ---- it never finished. The chip is mid-conversation and the only cure
        ;      is a reset, which is exactly what we must not do. Stop asking.
        mov     byte [desync], 1
.done:
        mov     al, bl
        mov     ah, bh
        ret
.refuse:
        mov     ax, 0FFFFh
        ret

start:
        PROBE_BEGIN "fdc"

; ════════════════════════════════════════════════════════════════════════════
; A. ★★★ THE MAIN STATUS REGISTER AT REST.   ref/fdc.md 3
;
;    One read, no side effects, and the single most load-bearing byte in the
;    chip. A controller that exists and is idle answers 80h: RQM set, DIO clear
;    (it wants a command), CMD BSY clear, no drive seeking. An absent one answers
;    FFh, which is not merely wrong -- it is the value that hangs the caller.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 4
        call    frd
        mov     [msr_rb], al
        xor     ah, ah
        mov     al, [msr_rb]
        POISON
        call    probe_capture
        EMIT    "fdc.msr.idle", "AX"

; ════════════════════════════════════════════════════════════════════════════
; B. ★★★ WOULD THE DATASHEET'S OWN COMMAND-WRITE LOOP EVER EXIT?   ref/fdc.md 3
;
;    Not a register value -- a BEHAVIOUR. Runs the exact loop every BIOS and
;    every driver uses to hand the chip a command byte, bounded to 65536 turns,
;    and reports whether it terminated. AH=00 it exited, AH=01 it did not; AL is
;    the RQM/DIO pair it was looking at when we gave up.
;
;    ⛔ THIS IS THE CASE THE WHOLE SURFACE EXISTS FOR. A wrong value is a bug; a
;      value that means "keep waiting" for ever is a machine that stops, with
;      nothing in any log to say why.
; ════════════════════════════════════════════════════════════════════════════
        mov     cx, 0
.cw:    mov     al, 4
        call    frd
        and     al, 0C0h
        cmp     al, 080h                ; RQM=1, DIO=0
        je      .cwok
        loop    .cw
        mov     byte [cmdwait], 1
        jmp     .cwe
.cwok:  mov     byte [cmdwait], 0
.cwe:
        mov     al, 4
        call    frd
        and     al, 0C0h
        mov     [msr_pr], al
        mov     ah, [cmdwait]
        mov     al, [msr_pr]
        POISON
        call    probe_capture
        EMIT    "fdc.cmdwait", "AX"

; ════════════════════════════════════════════════════════════════════════════
; C. THE DIGITAL OUTPUT REGISTER, READ BACK.   ref/fdc.md 7
;
;    Write-only on a PC/AT; readable on an 82077AA.
;
;    ⛔⛔ THIS CASE USED TO MASK TO BITS 3:0 AND THAT MANUFACTURED AN AGREEMENT.
;      An absent chip reads FFh, and FFh & 0Fh is 0Fh -- which is a perfectly
;      plausible DOR (out of reset, gated, drive 3). NTVDMEX scored 000F against
;      PCem's 000F and read as a MATCH from a port that does not exist. The
;      coincidence was the mask's, not the machine's.
;      So: the RAW byte is emitted too, where FFh cannot hide, and the
;      adjudicable question is narrowed to the two bits that are a property of
;      the CHIP rather than of the driver -- /RESET (bit 2) and DMAGATE (bit 3).
;      Both oracles say 11b there. DRIVE SEL and the motors are the BIOS's
;      business and they disagree about both.
;    ⚠ The raw byte is informational: motor bits say how long ago DOS last read
;      a sector, which measures the moment rather than the machine.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 2
        call    frd
        mov     [dor_rb], al
        xor     ah, ah
        mov     al, [dor_rb]
        POISON
        call    probe_capture
        EMIT    "fdc.dor.raw", "AX"

        xor     ah, ah
        mov     al, [dor_rb]
        and     al, 00Ch                ; /RESET and DMAGATE only
        POISON
        call    probe_capture
        EMIT    "fdc.dor.gate", "AX"

; ════════════════════════════════════════════════════════════════════════════
; D. THE DISK-CHANGE LINE.   ref/fdc.md 9
;
;    DIR bit 7 is DSKCHG: the door has been opened since the last access. It is
;    cleared only by a successful seek to a new cylinder, NOT by reading it. An
;    absent chip reads FFh and therefore says "changed" on every access for ever,
;    which makes the drive look permanently unreliable rather than absent.
;    Two cases from one read: the whole byte (informational -- bits 6:0 are not
;    driven in PC/AT mode and float) and the one bit that is adjudicable.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 7
        call    frd
        mov     [dir_rb], al
        xor     ah, ah
        mov     al, [dir_rb]
        POISON
        call    probe_capture
        EMIT    "fdc.dir.raw", "AX"

        xor     ah, ah
        mov     al, [dir_rb]
        and     al, 080h
        POISON
        call    probe_capture
        EMIT    "fdc.dir.dskchg", "AX"

; ════════════════════════════════════════════════════════════════════════════
; E. AT MODE OR PS/2 MODE?   ref/fdc.md 2
;
;    Offsets 0 and 1 are Status Register A and B, and they are DRIVEN ONLY BY A
;    PART STRAPPED FOR PS/2 MODE. On a PC/AT machine they are not driven at all
;    and read as bus float. This is a genuine two-machines question rather than a
;    right-and-wrong one, and knowing which each oracle is tells us which answer
;    we would be copying. AH = 3F0h, AL = 3F1h.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 0
        call    frd
        mov     [sra_rb], al
        mov     al, 1
        call    frd
        mov     [srb_rb], al
        mov     ah, [sra_rb]
        mov     al, [srb_rb]
        POISON
        call    probe_capture
        EMIT    "fdc.sra.srb", "AX"

; ════════════════════════════════════════════════════════════════════════════
; F. 3F6h IS NOT THE FLOPPY CONTROLLER'S.   ref/fdc.md 2
;
;    The FDC does not decode offset 6. On a PC/AT it belongs to the hard-disk
;    controller as the alternate status register, which is why claiming the whole
;    eight-port block would take a register that is somebody else's. Reading ATA
;    alternate status has no side effects (unlike 1F7h, which acknowledges the
;    interrupt). Informational: it tells us whether a disk controller answers.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 6
        call    frd
        mov     [alt_rb], al
        xor     ah, ah
        mov     al, [alt_rb]
        POISON
        call    probe_capture
        EMIT    "fdc.alt.3f6", "AX"

; ════════════════════════════════════════════════════════════════════════════
; G. ★★ THE VERSION COMMAND -- THE ONE EVERY DRIVER ISSUES.   ref/fdc.md 5
;
;    10h. No parameters, no execution phase, no interrupt, no head movement, one
;    result byte, nothing left behind. An enhanced 82077AA answers 90h; an
;    original 765A/8272A does not know the opcode and gives the invalid-command
;    reply 80h. That one byte is how a driver decides whether CONFIGURE, LOCK and
;    PERPENDICULAR MODE exist.
;
;    AH = how many result bytes the drain actually took, AL = the first of them.
;    A real part gives 0190h or 0180h. FFFFh means REFUSED -- the chip was not
;    idle and ready, so nothing was written. See the desync guard in the header.
; ════════════════════════════════════════════════════════════════════════════
        mov     bl, 010h
        call    fdc_cmd
        mov     [ver_ax], ax
        mov     ax, [ver_ax]
        POISON
        call    probe_capture
        EMIT    "fdc.version", "AX"

; ════════════════════════════════════════════════════════════════════════════
; H. DUMPREG -- WHAT DID THE BIOS PROGRAM INTO IT?   ref/fdc.md 5
;
;    0Eh. Ten result bytes and no side effects whatever: it reads back the step
;    rate, head load and unload times SPECIFY set, the CONFIGURE settings, and
;    the present cylinder of all four drives. It is the safest command on the
;    chip and it is the only way to see what the firmware chose -- which is what
;    a model would have to store.
;
;    ⚠ ASKED LAST. If VERSION desynced the chip this is refused, not attempted.
;    A part without the enhanced set answers 80h with a count of 1, which
;    self-identifies without a table.
; ════════════════════════════════════════════════════════════════════════════
        mov     bl, 00Eh
        call    fdc_cmd
        mov     [dmp_ax], ax
        mov     ax, [dmp_ax]
        POISON
        call    probe_capture
        EMIT    "fdc.dumpreg", "AX"

; ── the state we leave behind, reported rather than assumed. 0000 = the chip is
;    as we found it. 0001 = a drain did not finish and the chip is mid-command,
;    which invalidates every case after the one that did it.
        xor     ah, ah
        mov     al, [desync]
        POISON
        call    probe_capture
        EMIT    "fdc.desync", "AX"

        PROBE_END
