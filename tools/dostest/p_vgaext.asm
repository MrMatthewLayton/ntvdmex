; p_vgaext.com -- the VGA's two external read-only registers, asked BEHAVIOURALLY.
;                 docs/ref/vga.md 3; docs/inventory/vga.md.
;
; ── WHAT THIS IS FOR ─────────────────────────────────────────────────────────
; p_vgareg dumps the whole 64-byte register file and two of its bytes have never
; settled.  Four hosts, four answers:
;
;                          ours   6.22(QEMU)  dosbox-x   PCem (real AMI+IBM VGA)
;   Input Status 0  (3C2)  0x00     0x00      0x60/0x70       0x10
;   Feature Control (3CA)  0x00     0x00      0x00            0xFF
;
; ⚠ THAT TABLE WAS ITSELF WRONG WHERE IT WAS RECORDED, and this probe is what
;   caught it: docs/research/oracle-disagreements.md had dosbox-x's Feature
;   Control as 0x70, which is its INPUT STATUS 0 value copied into the row below.
;   The two bytes are adjacent in p_vgareg's buffer (off 1 and off 2) and were
;   read off it by eye. A hand-transcribed table is a claim.
;
; A row like that cannot be settled by taking a majority, and it was twice
; recorded as "confirmed 0x00" on the strength of ONE oracle.  The reason it
; keeps failing is that p_vgareg asks the wrong SHAPE of question: it reads each
; port once and prints the byte.  A byte is a value; what is actually in dispute
; is a MECHANISM.  So this probe asks about the mechanism instead:
;
;   * WHICH BITS OF 3C2 ARE LIVE?  Sample it across several frames and emit the
;     AND (bits always 1) and the OR (bits ever 1).  A host that returns a
;     constant says AND == OR; a host that drives a sense line or a retrace
;     interrupt says so, and names the bit, without anyone having to know how
;     the sense line is wired.
;   * IS BIT 7 THE CRT INTERRUPT?  Enable the vertical-retrace interrupt in CR11,
;     wait for a retrace, look; then clear it through CR11 bit 4 and look again.
;   * WHICH BITS OF FEATURE CONTROL ARE STORAGE?  Write 00, 0F and 08 to 3DA and
;     read 3CA back after each.  ref/vga.md calls it "two feature-connector
;     control bits ... read/write storage from software's point of view" and
;     flags the read-back as [VERIFY]; this is that verification.
;
; ── THE EXPECTATIONS, WRITTEN BEFORE THE FIRST RUN ───────────────────────────
; From ref/vga.md 3 and the four-host table above.  Recorded here so the run can
; falsify them rather than be read in their light:
;
;   vgaext.is0.live   AH=AND AL=OR.  We predict a real card gives AH=0x10 AL=0x10
;                     -- bit 4 Switch Sense high, bit 7 quiet because the BIOS
;                     leaves the vertical interrupt DISABLED (CR11 bit 5 = 1).
;                     Ours must give 0000: the port is a hard-coded 0x00.
;   vgaext.is0.vsync  AL bit 0 = bit 7 seen set after a retrace with the
;                     interrupt enabled, AL bit 1 = bit 7 still set after
;                     clearing it through CR11 bit 4.  We predict AL=0x01.
;                     Ours must give 0x00.
;   vgaext.fc.store   AH = 3CA after writing 00, AL = after 0F, BL = after 08.
;                     If only the two feature bits exist: 00, 03, 00.  If the
;                     register is byte-wide storage: 00, 0F, 08 -- which is what
;                     OURS does, so this case tells us whether we are right by
;                     accident.  PCem's 0xFF for the power-on read suggests it
;                     does not decode 3CA at all, in which case it will answer
;                     FF/FF/FF and is an ABSENCE, not evidence -- the same trap
;                     as pit.bcd.valid.
;
; ── THE TRAPS, AND WHY THE CODE LOOKS LIKE THIS ──────────────────────────────
; * 3DA IS TWO REGISTERS.  Reading it is Input Status 1; WRITING it is Feature
;   Control.  Both appear here and confusing them reads as a dead probe.
; * ⚠⚠ ENABLING THE VERTICAL-RETRACE INTERRUPT ARMS AN IRQ ON REAL HARDWARE, and
;   this probe runs unattended on a bare-metal rig.  Case B masks it at the PIC
;   for its whole duration and restores the masks afterwards, so the enable can
;   never deliver an interrupt to a machine with no handler for it.  Do not
;   remove that: a probe that wedges the rig costs a physical reboot.
;   ⚠ AND IT MASKS BOTH PICs.  The VGA's vertical interrupt is slot IRQ2, but on
;     an AT master IRQ2 is the CASCADE and the slot line is rerouted to IRQ9 --
;     so masking only 21h bit 2 would leave the interrupt free to arrive as
;     IRQ9.  Masking 21h bit 2 also silences the whole slave for those few
;     frames, which costs an RTC tick and nothing else.
; * CR11 CARRIES THE CRTC WRITE PROTECT IN BIT 7.  It is read, modified and put
;   back bit-for-bit; nothing else in the CRTC is touched.
; * EVERY LOOP IS BOUNDED.  Waiting for a retrace that never comes is a hang on
;   a host that does not model one, and a hang reads as a harness timeout rather
;   than as data.
; * NOTHING IS LEFT MODIFIED: Feature Control, CR11 and the PIC mask are all
;   saved and restored, and the video mode is never changed at all.
;
; ORACLE-ALSO: pcem   (a real AMI 486 BIOS + IBM VGA ROM -- see scripts/pcemoracle.py)
; nasm -f bin p_vgaext.asm -o p_vgaext.com

        org     100h
        jmp     start
%include "probe.inc"

; ---------------------------------------------------------------- state
fc_save         db      0               ; Feature Control, as we found it
cr11_save       db      0               ; CRTC Vertical Retrace End + write protect
pic_save        db      0               ; master 8259 mask, as we found it
pic2_save       db      0               ; slave 8259 mask, as we found it
is0_and         db      0
is0_or          db      0
vsync_res       db      0

; ---------------------------------------------------------------- helpers

; Read CRTC index AL -> AL.  Colour setup only (3D4/3D5): this probe never
; changes the video mode, and DOS leaves us in mode 3.
rd_crtc:
        push    dx
        mov     dx, 03D4h
        out     dx, al
        inc     dx
        in      al, dx
        pop     dx
        ret

; Write AH to CRTC index AL.
wr_crtc:
        push    dx
        push    ax
        mov     dx, 03D4h
        out     dx, al
        inc     dx
        mov     al, ah
        out     dx, al
        pop     ax
        pop     dx
        ret

; Wait, BOUNDED, for Input Status 1 bit 3 to read as CX-selected level.
; AL = 0 wait for retrace to end, AL = 8 wait for retrace to start.
wait_vr:                                ; AL = the wanted level of bit 3
        push    bx
        push    cx
        push    dx
        mov     bl, al
        mov     dx, 03DAh
        mov     cx, 0FFFFh              ; bounded: a dead port must not hang us
.spin:  in      al, dx
        and     al, 008h
        cmp     al, bl
        je      .done
        loop    .spin
.done:  pop     dx
        pop     cx
        pop     bx
        ret

; Write AL to Feature Control (3DA on a colour setup) and read 3CA back -> AL.
; ⚠ 3DA is TWO REGISTERS: reading it is Input Status 1, writing it is Feature
;   Control. The read-back port is a different one, 3CA.
fc_write:
        push    dx
        mov     dx, 03DAh
        out     dx, al
        mov     dx, 03CAh
        in      al, dx
        pop     dx
        ret

start:
        PROBE_BEGIN "vgaext"

; ════════════════════════════════════════════════════════════════════════════
; A. INPUT STATUS 0 (3C2 read) -- WHICH BITS ARE LIVE?   ref/vga.md 3
;
;    65536 reads is roughly four frames on a period-correct machine, so any bit
;    tied to the frame is certain to be caught in BOTH states.  AND collects the
;    bits that are always 1, OR the bits that are ever 1; a host answering a
;    constant reports AND == OR, and the constant is in both.
; ════════════════════════════════════════════════════════════════════════════
        mov     byte [is0_and], 0FFh
        mov     byte [is0_or], 000h
        mov     dx, 03C2h
        xor     cx, cx                  ; 0 => 65536 iterations
.sample:
        in      al, dx
        and     [is0_and], al
        or      [is0_or], al
        loop    .sample

        mov     ah, [is0_and]
        mov     al, [is0_or]
        POISON
        call    probe_capture
        EMIT    "vgaext.is0.live", "AX"

; ════════════════════════════════════════════════════════════════════════════
; B. INPUT STATUS 0 BIT 7 -- IS IT THE CRT INTERRUPT?   ref/vga.md 3
;
;    CR11 bit 5 = 0 ENABLES the vertical-retrace interrupt; bit 4 = 0 CLEARS a
;    pending one.  So: mask IRQ2, enable, let a retrace happen, read bit 7;
;    then pulse bit 4 low-then-high and read bit 7 again.  A card that
;    implements the interrupt sets the bit and then clears it.
;    ⚠ THE PIC MASK IS NOT OPTIONAL -- see the header.
; ════════════════════════════════════════════════════════════════════════════
        in      al, 021h                ; master 8259 interrupt mask
        mov     [pic_save], al
        in      al, 0A1h                ; slave 8259 interrupt mask
        mov     [pic2_save], al
        or      al, 002h                ; mask IRQ9 -- the AT's rerouted slot IRQ2
        out     0A1h, al
        mov     al, [pic_save]
        or      al, 004h                ; and the cascade, for a card wired to IRQ2
        out     021h, al

        mov     al, 011h
        call    rd_crtc
        mov     [cr11_save], al

        mov     ah, [cr11_save]
        and     ah, 0CFh                ; bit 5 = 0 enable, bit 4 = 0 clear
        or      ah, 010h                ; ...but leave bit 4 HIGH: not clearing
        mov     al, 011h
        call    wr_crtc

        mov     al, 000h                ; let any retrace in flight finish
        call    wait_vr
        mov     al, 008h                ; then wait for the next one to start
        call    wait_vr

        mov     byte [vsync_res], 0
        mov     dx, 03C2h
        in      al, dx
        test    al, 080h
        jz      .noset
        or      byte [vsync_res], 001h  ; bit 7 came up with the retrace
.noset:
        mov     ah, [cr11_save]         ; now CLEAR it: bit 4 low, then high
        and     ah, 0CFh
        mov     al, 011h
        call    wr_crtc
        mov     ah, [cr11_save]
        and     ah, 0CFh
        or      ah, 010h
        mov     al, 011h
        call    wr_crtc

        mov     dx, 03C2h
        in      al, dx
        test    al, 080h
        jz      .cleared
        or      byte [vsync_res], 002h  ; still set: the clear did nothing
.cleared:
        mov     ah, [cr11_save]         ; put CR11 back EXACTLY, write protect and all
        mov     al, 011h
        call    wr_crtc
        mov     al, [pic2_save]         ; masks back, and the CASCADE LAST: the
        out     0A1h, al                ; slave must be settled before the line
        mov     al, [pic_save]          ; that carries it is reopened
        out     021h, al

        xor     ax, ax
        mov     al, [vsync_res]
        POISON
        call    probe_capture
        EMIT    "vgaext.is0.vsync", "AX"

; ════════════════════════════════════════════════════════════════════════════
; C. FEATURE CONTROL -- WHICH BITS ARE STORAGE?   ref/vga.md 3
;
;    Write at 3DA (colour setup), read back at 3CA.  Three probes: all-zero,
;    all-ones in the low nibble, and bit 3 alone -- enough to separate "two
;    feature bits" from "a byte of storage" from "not decoded at all".
; ════════════════════════════════════════════════════════════════════════════
        mov     dx, 03CAh
        in      al, dx
        mov     [fc_save], al

        mov     al, 000h
        call    fc_write
        mov     bh, al                  ; after 00

        mov     al, 00Fh
        call    fc_write
        mov     cl, al                  ; after 0F

        mov     al, 008h
        call    fc_write
        mov     ch, al                  ; after 08

        mov     al, [fc_save]           ; put it back before anything can EMIT
        call    fc_write

        mov     ah, bh
        mov     al, cl
        xor     bx, bx
        mov     bl, ch
        ; ⚠ NO `POISON` HERE. It stamps BX, and BX is carrying the third of the
        ;   three read-backs. Poison CX/DX only -- the two registers this case
        ;   does not report.
        mov     cx, 0C1C1h
        mov     dx, 0D1D1h
        call    probe_capture
        EMIT    "vgaext.fc.store", "AX,BX"

; ⛔ THERE WAS A CASE D HERE -- "the power-on values, side by side", AH = 3C2 and
;    AL = 3CA in one field -- AND IT WAS REMOVED THE DAY IT WAS WRITTEN. It packed a
;    register the oracles have now SETTLED (3C2 = 0x10) together with one they cannot
;    adjudicate at all (3CA), so the field could never be anything but DISPUTED and the
;    dispute would have meant two different things at once. It also duplicated bytes
;    p_vgareg already carries. That is the very shape of question -- read a port, print
;    a byte -- this probe exists to replace; writing it again at the bottom of the file
;    that argues against it is worth recording rather than quietly deleting.

        PROBE_END
