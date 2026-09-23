; p_vgamem.com -- the VGA's CPU-side memory path, measured.
;                 docs/ref/vga.md sections 6 and 8; inventory step 4.
;
; ── WHAT THIS IS FOR ─────────────────────────────────────────────────────────
; p_vgareg measures what a BIOS LEAVES IN the register file. It cannot see what
; the registers DO. This probe measures the other half, and it is the half our
; open defect lives in:
;
;   "Our mode-Y path maps the A0000 window to ONE PLANE AT A TIME and handles
;    multi-plane map masks with a scratch buffer fanned out later by diffing
;    against a seed. Doom's low-detail drawer is dominated by TWO-PLANE masks
;    (0x03 and 0x0c, ~143,000 writes each)."   -- docs/inventory/vga.md
;
; Every case here is a CPU write followed by a per-plane read-back, so the
; answer is four bytes that either match the hardware or do not. There is no
; picture to eyeball and no frame to capture: this is the deterministic test
; the mode-Y approximation has never had.
;
; ⚠ SEVERAL OF THESE ARE EXPECTED TO FAIL ON NTVDMEX TODAY. That is the point --
;   spec-first means writing the checks from the document, watching them fail on
;   the old code, and only then fixing. A probe that passes on arrival measured
;   nothing.
;
; ── HOW TO READ THE OUTPUT ───────────────────────────────────────────────────
; One 4-byte BUF line per case: the byte at the test offset in plane 0, 1, 2, 3,
; read back with read mode 0 and GR4 stepped 0..3. A diff points at a plane.
;
; ── THE TRAPS, AND WHY THE CODE LOOKS LIKE THIS ──────────────────────────────
; * READING VIDEO MEMORY LOADS THE LATCHES, always, whatever the read mode. So
;   the read-back loop itself changes state, and any case that depends on latch
;   contents must load them as its LAST action before the write under test.
; * A CASE MUST NOT INHERIT THE PREVIOUS CASE'S REGISTERS. gc_reset puts the
;   Graphics Controller and the Map Mask back to a documented baseline before
;   every case; without it this probe measures the order of its own cases.
; * CHAIN-4 CHAINS READS TOO. You cannot read a plane back through GR4 while
;   SR4.3 is set, because the address bits are selecting the plane, not GR4.
;   Case A therefore writes chained and reads unchained, which is exactly how a
;   program verifies its own mode 13h layout.
; * MODE 12h IS ALREADY PLANAR, so the planar cases use it rather than hand-
;   unchaining 13h -- fewer moving parts between the BIOS and the measurement.
;
; ORACLE-ALSO: pcem   (a real AMI 486 BIOS + IBM VGA ROM -- see scripts/pcemoracle.py)
; nasm -f bin p_vgamem.asm -o p_vgamem.com

        org     100h
        jmp     start
%include "probe.inc"

; ---------------------------------------------------------------- state
planes          times 4 db 0            ; the four read-backs, EMIT_BUF'd

; ---------------------------------------------------------------- helpers

wr_seq:                                 ; AL = index, AH = value
        push    dx
        mov     dx, 03C4h
        out     dx, al
        inc     dx
        mov     al, ah
        out     dx, al
        pop     dx
        ret

wr_gc:                                  ; AL = index, AH = value
        push    dx
        mov     dx, 03CEh
        out     dx, al
        inc     dx
        mov     al, ah
        out     dx, al
        pop     dx
        ret

; Put the Graphics Controller and the Map Mask back to the documented "plain
; write mode 0" baseline: no set/reset, no rotate, no ALU, read plane 0, read
; mode 0, write mode 0, full bit mask, all four planes writable.
gc_reset:
        mov     ax, 0000h               ; GR0 Set/Reset      = 0
        call    wr_gc
        mov     ax, 0001h               ; GR1 Enable S/R     = 0
        call    wr_gc
        mov     ax, 0002h               ; GR2 Color Compare  = 0
        call    wr_gc
        mov     ax, 0003h               ; GR3 rotate/function= 0
        call    wr_gc
        mov     ax, 0004h               ; GR4 Read Map       = 0
        call    wr_gc
        mov     ax, 0005h               ; GR5 modes          = 0
        call    wr_gc
        mov     ax, 0007h               ; GR7 Color Don't Care = 0
        call    wr_gc
        mov     ax, 0FF08h              ; GR8 Bit Mask       = FF
        call    wr_gc
        mov     ax, 00F02h              ; SR2 Map Mask       = 0F
        call    wr_seq
        ret

; Read the byte at ES:DI from each plane in turn into planes[0..3].
; ⚠ Leaves the latches holding plane 3's row and GR4 = 3; callers that care
;   re-run gc_reset afterwards.
read_planes:
        push    cx
        xor     cx, cx
.next:  mov     al, 4                   ; GR4 = Read Map Select
        mov     ah, cl
        call    wr_gc
        mov     al, [es:di]
        mov     bx, cx
        mov     [planes+bx], al
        inc     cx
        cmp     cx, 4
        jb      .next
        pop     cx
        ret

; Write AL to ES:DI through the current Graphics Controller state.
poke:
        mov     [es:di], al
        ret

; Fill all four planes at ES:DI with AL, using the baseline write path.
fill_all:
        push    ax
        mov     ax, 00F02h              ; SR2 = all planes
        call    wr_seq
        pop     ax
        mov     [es:di], al
        ret

; Set a BIOS mode and point ES at the graphics aperture.
set_mode:                               ; AL = mode
        mov     ah, 0
        int     10h
        mov     ax, 0A000h
        mov     es, ax
        ret

; ---------------------------------------------------------------- the probe
start:
        PROBE_BEGIN "vgamem"

; ════════════════════════════════════════════════════════════════════════════
; A. CHAIN-4 MAPS ADDRESS BITS 1:0 TO THE PLANE.   docs/ref/vga.md §8
;
;    Mode 13h. Four consecutive CPU bytes must land in four DIFFERENT planes at
;    the SAME offset. Written chained, read back unchained -- see the trap note.
;    Expect 11 22 33 44.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 13h
        call    set_mode
        call    gc_reset

        xor     di, di
        mov     byte [es:di], 011h
        mov     byte [es:di+1], 022h
        mov     byte [es:di+2], 033h
        mov     byte [es:di+3], 044h

        mov     ax, 00604h              ; SR4 = 06: chain-4 OFF, odd/even off
        call    wr_seq
        xor     di, di
        call    read_planes
        EMIT_BUF "vgamem.chain4.abcd", planes, 4

; ════════════════════════════════════════════════════════════════════════════
; B. THE MAP MASK WRITES EVERY SELECTED PLANE, IN ONE STORE.   §8, §6
;
;    This is the defect. Mode 12h is planar, so one CPU byte with SR2 = 0x03
;    must appear in planes 0 AND 1 and nowhere else. 0x03 and 0x0c are the two
;    masks Doom's low-detail drawer uses ~143,000 times each.
; ════════════════════════════════════════════════════════════════════════════
        mov     al, 12h
        call    set_mode
        call    gc_reset

        xor     di, di
        mov     al, 0
        call    fill_all                ; every plane at offset 0 = 00

        mov     ax, 00302h              ; SR2 = 0x03 -> planes 0 and 1
        call    wr_seq
        mov     al, 0AAh
        call    poke
        call    read_planes
        EMIT_BUF "vgamem.mask03.aa", planes, 4      ; expect AA AA 00 00

        call    gc_reset
        mov     al, 0
        call    fill_all
        mov     ax, 00C02h              ; SR2 = 0x0c -> planes 2 and 3
        call    wr_seq
        mov     al, 055h
        call    poke
        call    read_planes
        EMIT_BUF "vgamem.mask0c.55", planes, 4      ; expect 00 00 55 55

        call    gc_reset
        mov     al, 0
        call    fill_all
        mov     ax, 00F02h              ; SR2 = 0x0f -> all four
        call    wr_seq
        mov     al, 05Ah
        call    poke
        call    read_planes
        EMIT_BUF "vgamem.mask0f.5a", planes, 4      ; expect 5A 5A 5A 5A

        call    gc_reset
        mov     al, 0
        call    fill_all
        mov     ax, 00202h              ; SR2 = 0x02 -> plane 1 only
        call    wr_seq
        mov     al, 0C3h
        call    poke
        call    read_planes
        EMIT_BUF "vgamem.mask02.c3", planes, 4      ; expect 00 C3 00 00

; ════════════════════════════════════════════════════════════════════════════
; C. THE BIT MASK SELECTS PER BIT BETWEEN NEW DATA AND THE LATCH.   §6
;
;    Latches loaded with FF, bit mask 0x0F, write 0x00:
;    low nibble takes the new data (0), high nibble takes the latch (F) -> F0.
; ════════════════════════════════════════════════════════════════════════════
        call    gc_reset
        mov     di, 8
        mov     al, 0FFh
        call    fill_all                ; all planes = FF at offset 8

        mov     al, [es:di]             ; ⚠ LAST action before the write: latches = FF
        mov     ax, 00F08h              ; GR8 = 0x0F
        call    wr_gc
        mov     al, 000h
        call    poke
        call    read_planes
        EMIT_BUF "vgamem.bitmask0f", planes, 4      ; expect F0 F0 F0 F0

; ════════════════════════════════════════════════════════════════════════════
; D. WRITE MODE 1 COPIES THE LATCHES AND IGNORES THE CPU BYTE.   §6
;
;    Four distinct plane values are latched from one offset and stored to
;    another. This is the second half of every planar blit.
; ════════════════════════════════════════════════════════════════════════════
        call    gc_reset
        mov     di, 16
        mov     ax, 00102h
        call    wr_seq
        mov     byte [es:di], 011h      ; plane 0 = 11
        mov     ax, 00202h
        call    wr_seq
        mov     byte [es:di], 022h      ; plane 1 = 22
        mov     ax, 00402h
        call    wr_seq
        mov     byte [es:di], 044h      ; plane 2 = 44
        mov     ax, 00802h
        call    wr_seq
        mov     byte [es:di], 088h      ; plane 3 = 88

        mov     ax, 00F02h              ; all planes writable again
        call    wr_seq
        mov     al, [es:di]             ; latches = 11 22 44 88
        mov     ax, 00105h              ; GR5 write mode 1
        call    wr_gc
        mov     di, 20
        mov     byte [es:di], 0         ; CPU byte is ignored
        call    read_planes
        EMIT_BUF "vgamem.wmode1.copy", planes, 4    ; expect 11 22 44 88

; ════════════════════════════════════════════════════════════════════════════
; E. WRITE MODE 2 TREATS THE CPU BYTE AS A COLOUR.   §6
;
;    Colour 5 = planes 0 and 2 set. With a full bit mask every one of the eight
;    pixels takes it, so those planes go FF and the others 00.
; ════════════════════════════════════════════════════════════════════════════
        call    gc_reset
        mov     di, 24
        mov     al, 0
        call    fill_all
        mov     al, [es:di]             ; latches = 00
        mov     ax, 00205h              ; GR5 write mode 2
        call    wr_gc
        mov     al, 005h
        call    poke
        call    read_planes
        EMIT_BUF "vgamem.wmode2.col5", planes, 4    ; expect FF 00 FF 00

; ════════════════════════════════════════════════════════════════════════════
; F. WRITE MODE 3 ANDs THE CPU BYTE WITH THE BIT MASK; THE COLOUR IS SET/RESET.
; ════════════════════════════════════════════════════════════════════════════
        call    gc_reset
        mov     di, 28
        mov     al, 0
        call    fill_all
        mov     al, [es:di]             ; latches = 00
        mov     ax, 00F00h              ; GR0 Set/Reset = 0x0F (all planes)
        call    wr_gc
        mov     ax, 00305h              ; GR5 write mode 3
        call    wr_gc
        mov     al, 0F0h                ; effective mask = F0 & FF
        call    poke
        call    read_planes
        EMIT_BUF "vgamem.wmode3.f0", planes, 4      ; expect F0 F0 F0 F0

; ════════════════════════════════════════════════════════════════════════════
; G. THE ALU COMBINES CPU DATA WITH THE LATCHES.   §6 (GR3 bits 4:3)
;
;    Latches FF, function XOR, write 0x0F -> 0x0F XOR 0xFF = 0xF0.
; ════════════════════════════════════════════════════════════════════════════
        call    gc_reset
        mov     di, 32
        mov     al, 0FFh
        call    fill_all
        mov     al, [es:di]             ; latches = FF
        mov     ax, 01803h              ; GR3 = 0x18: function 11b = XOR
        call    wr_gc
        mov     al, 00Fh
        call    poke
        call    read_planes
        EMIT_BUF "vgamem.alu.xor", planes, 4        ; expect F0 F0 F0 F0

        call    gc_reset
        mov     di, 36
        mov     al, 0FFh
        call    fill_all
        mov     al, [es:di]             ; latches = FF
        mov     ax, 00803h              ; GR3 = 0x08: function 01b = AND
        call    wr_gc
        mov     al, 00Fh
        call    poke
        call    read_planes
        EMIT_BUF "vgamem.alu.and", planes, 4        ; expect 0F 0F 0F 0F

; ════════════════════════════════════════════════════════════════════════════
; H. READ MODE 1 IS A COLOUR COMPARE, NOT A PLANE READ.   §6
;
;    Planes 0 and 2 high nibble set -> the top four pixels are colour 5 and the
;    bottom four colour 0. Comparing against 5 must return F0, against 0 -> 0F.
;    ⚠ This is the class Doom's I_ReadScreen is in.
; ════════════════════════════════════════════════════════════════════════════
        call    gc_reset
        mov     di, 40
        mov     al, 0
        call    fill_all
        mov     ax, 00502h              ; SR2 = planes 0 and 2
        call    wr_seq
        mov     byte [es:di], 0F0h

        call    gc_reset
        mov     ax, 00F07h              ; GR7 Color Don't Care = 0x0F: compare all
        call    wr_gc
        mov     ax, 00805h              ; GR5 bit 3 = read mode 1
        call    wr_gc
        mov     ax, 00502h              ; GR2 Color Compare = 5
        call    wr_gc
        mov     al, [es:di]             ; read mode 1: AL is a MATCH BITMASK
        xor     ah, ah
        POISON
        call    probe_capture
        EMIT    "vgamem.rmode1.cmp5", "AX"          ; expect AX = 00F0

        mov     ax, 00002h              ; GR2 Color Compare = 0
        call    wr_gc
        mov     al, [es:di]
        xor     ah, ah
        POISON
        call    probe_capture
        EMIT    "vgamem.rmode1.cmp0", "AX"          ; expect AX = 000F

; ---- leave the machine as we found it.
        call    gc_reset
        mov     ax, 00005h              ; GR5 = 0: read mode 0, write mode 0
        call    wr_gc
        mov     al, 03h
        mov     ah, 0
        int     10h

        PROBE_END
