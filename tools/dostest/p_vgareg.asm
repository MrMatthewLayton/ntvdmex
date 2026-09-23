; p_vgareg.com -- the VGA register file, read back from the hardware.
;                 docs/inventory/vga.md, step 2.
;
; ── WHAT THIS IS FOR ─────────────────────────────────────────────────────────
; The inventory measured our VGA model at 41 of 71 registers, and step 1 gave the
; host a register FILE. Two questions are now open and neither can be answered
; from our own code:
;
;   1. WHAT DOES A REAL BIOS LEAVE IN ALL ~70 REGISTERS AFTER A MODE SET?
;      vga_defaults.h is generated from a probe against genuine MS-DOS 6.22, but
;      it covers only the Attribute Controller palette and the DAC. There is no
;      measured table for the Sequencer, the CRTC, the Graphics Controller or the
;      external registers -- so step 3 ("derive the geometry from the file") has
;      nothing to derive FROM on a BIOS mode set. This probe generates it.
;
;   2. WHAT DOES THE HARDWARE RETURN FOR THE THINGS WE GUESSED?
;      Our host answers Input Status 0 (3C2 read) with 0x00 and marks it
;      UNVERIFIED in its own log, deliberately, rather than writing an
;      expectation from memory. This is what settles it.
;
; ── HOW TO READ THE OUTPUT ───────────────────────────────────────────────────
; One 64-byte BUF line per mode, laid out so a diff points at a register:
;
;   off  0      Miscellaneous Output      (read at 3CC)
;   off  1      Feature Control           (read at 3CA)
;   off  2      Input Status 0            (read at 3C2)
;   off  3      DAC Pixel Mask            (read at 3C6)
;   off  4..8   Sequencer      SR0..SR4   (index 3C4, data 3C5)
;   off  9..33  CRTC           CR00..CR18 (index 3D4/3B4, data 3D5/3B5)
;   off 34..42  Graphics Ctrl  GR0..GR8   (index 3CE, data 3CF)
;   off 43..63  Attribute Ctrl AR00..AR14 (index+data 3C0, read 3C1)
;
; ── THE TRAPS, AND WHY THE CODE LOOKS LIKE THIS ──────────────────────────────
; * THE ATTRIBUTE CONTROLLER SHARES ONE PORT between its index and its data, and
;   the flip-flop that decides which is reset by READING INPUT STATUS 1 -- 3DA on
;   a colour setup and 3BA on a mono one. Every AC access here resets it first.
; * AR INDEX BIT 5 IS "PALETTE ADDRESS SOURCE" and clearing it BLANKS THE SCREEN.
;   It must be cleared to reach the registers and set again afterwards, or the
;   display stays dark for every later test on the same machine.
; * MODE 7 IS MONOCHROME and moves the CRTC to 3B4/3B5 and Input Status 1 to 3BA.
;   Reading 3D5 in mode 7 reads a port nothing is driving. The mono pass exists
;   precisely because NTVDMEX only started claiming 3B4/3B5/3BA in step 1.
; * NOTHING IS LEFT MODIFIED. The write-protect case saves CR00 and CR11 and puts
;   both back; the probe ends by returning to mode 3.
;
;
; ── THE SIG NOTE ─────────────────────────────────────────────────────────────
; Every case below SIGs on AX and zeroes AH first. POISON stamps BX/CX/DX only,
; so AH at capture time holds whatever the last routine left there -- and the
; first run of this probe duly reported three MISMATCHes whose low bytes AGREED
; (oracle 7205 vs ours 3205: both AL=05). A SIG that names a register the probe
; does not define is a bug in the probe, and this is what it looks like.
;
; ORACLE-ALSO: pcem   (a real AMI 486 BIOS + IBM VGA ROM -- see scripts/pcemoracle.py)
; nasm -f bin p_vgareg.asm -o p_vgareg.com

        org     100h
        jmp     start
%include "probe.inc"

; ---------------------------------------------------------------- state
crtc_idx        dw      03D4h           ; 3D4 colour / 3B4 mono -- set per mode
stat1           dw      03DAh           ; 3DA colour / 3BA mono
regbuf          times 64 db 0

; ---------------------------------------------------------------- helpers
;
; Each reader takes the index in AL and returns the value in AL. DX is the port.

rd_seq:                                 ; AL = index -> AL = value
        mov     dx, 03C4h
        out     dx, al
        inc     dx
        in      al, dx
        ret

rd_gc:                                  ; AL = index -> AL = value
        mov     dx, 03CEh
        out     dx, al
        inc     dx
        in      al, dx
        ret

rd_crtc:                                ; AL = index -> AL = value
        mov     dx, [crtc_idx]
        out     dx, al
        inc     dx
        in      al, dx
        ret

wr_seq_ax:                              ; AL = index, AH = value
        push    dx
        mov     dx, 03C4h
        out     dx, al
        inc     dx
        mov     al, ah
        out     dx, al
        pop     dx
        ret

wr_crtc:                                ; AL = index, AH = value
        mov     dx, [crtc_idx]
        out     dx, al
        inc     dx
        mov     al, ah
        out     dx, al
        ret

; The Attribute Controller. Reset the flip-flop, select the index with bit 5
; CLEAR (so the register file is reachable), read the data port.
rd_ac:                                  ; AL = index -> AL = value
        push    dx
        mov     ah, al
        mov     dx, [stat1]
        in      al, dx                  ; reset index/data flip-flop
        mov     dx, 03C0h
        mov     al, ah
        out     dx, al                  ; index, PAS clear
        inc     dx                      ; 3C1 = read port
        in      al, dx
        pop     dx
        ret

; Put the Attribute Controller back into "video on" (index bit 5 set). Every
; path that touches the AC ends here, or the screen stays blank.
ac_enable:
        push    ax
        push    dx
        mov     dx, [stat1]
        in      al, dx
        mov     dx, 03C0h
        mov     al, 20h                 ; PAS = 1: palette drives the display
        out     dx, al
        pop     dx
        pop     ax
        ret

; ---------------------------------------------------------------- the dump
;
; Fill regbuf with the whole file, in the documented order.
dump_all:
        push    si
        push    cx
        mov     di, regbuf

        mov     dx, 03CCh               ; Miscellaneous Output (read alias)
        in      al, dx
        mov     [di], al
        mov     dx, 03CAh               ; Feature Control (read alias)
        in      al, dx
        mov     [di+1], al
        mov     dx, 03C2h               ; Input Status 0
        in      al, dx
        mov     [di+2], al
        mov     dx, 03C6h               ; DAC Pixel Mask
        in      al, dx
        mov     [di+3], al

        xor     cx, cx                  ; SR0..SR4
.seq:   mov     al, cl
        call    rd_seq
        mov     bx, cx
        mov     [regbuf+4+bx], al
        inc     cx
        cmp     cx, 5
        jb      .seq

        xor     cx, cx                  ; CR00..CR18
.crtc:  mov     al, cl
        call    rd_crtc
        mov     bx, cx
        mov     [regbuf+9+bx], al
        inc     cx
        cmp     cx, 25
        jb      .crtc

        xor     cx, cx                  ; GR0..GR8
.gc:    mov     al, cl
        call    rd_gc
        mov     bx, cx
        mov     [regbuf+34+bx], al
        inc     cx
        cmp     cx, 9
        jb      .gc

        xor     cx, cx                  ; AR00..AR14
.ac:    mov     al, cl
        call    rd_ac
        mov     bx, cx
        mov     [regbuf+43+bx], al
        inc     cx
        cmp     cx, 21
        jb      .ac
        call    ac_enable

        pop     cx
        pop     si
        ret

; Set a colour mode, dump it. AL = mode number on entry.
do_mode:
        mov     ah, 0
        int     10h                     ; BIOS mode set
        mov     word [crtc_idx], 03D4h
        mov     word [stat1], 03DAh
        call    dump_all
        ret

; ---------------------------------------------------------------- the probe
start:
        PROBE_BEGIN "vgareg"

        ; ---- the BIOS mode sets. One line per mode, 64 bytes each.
        mov     al, 03h
        call    do_mode
        EMIT_BUF "vga.mode03", regbuf, 64

        mov     al, 0Dh
        call    do_mode
        EMIT_BUF "vga.mode0D", regbuf, 64

        mov     al, 12h
        call    do_mode
        EMIT_BUF "vga.mode12", regbuf, 64

        mov     al, 13h
        call    do_mode
        EMIT_BUF "vga.mode13", regbuf, 64

        ; ---- THE MODES STEP 3 HAS NO ROWS FOR.
        ;      Step 3 derives the geometry from MiscOut + CRTC + SR1 instead of
        ;      from a mode number, and it can only be checked against modes whose
        ;      real register values are known. The five above cover 200- and
        ;      350-line timings and nothing else; these five add 480-line, the
        ;      CGA-compatible odd/even modes, and the 720-pixel dot clock.
        mov     al, 04h                 ; CGA 320x200 4-colour: odd/even + chain
        call    do_mode
        EMIT_BUF "vga.mode04", regbuf, 64

        mov     al, 06h                 ; CGA 640x200 2-colour
        call    do_mode
        EMIT_BUF "vga.mode06", regbuf, 64

        mov     al, 0Eh                 ; 640x200 16-colour planar
        call    do_mode
        EMIT_BUF "vga.mode0E", regbuf, 64

        mov     al, 10h                 ; 640x350 16-colour -- 350-line sync
        call    do_mode
        EMIT_BUF "vga.mode10", regbuf, 64

        mov     al, 11h                 ; 640x480 2-colour  -- 480-line sync
        call    do_mode
        EMIT_BUF "vga.mode11", regbuf, 64

        ; ---- MODE Y AND MODE X ARE NOT BIOS MODES. A program makes them, and
        ;      this is the sequence Doom and every Mode-X engine uses. Capturing
        ;      the file AFTER the unchain is the only way to know what the
        ;      hardware actually holds on the path our renderer has to reproduce
        ;      -- docs/ref/vga.md 5.2.
        ;
        ;      Y first: mode 13h, chain-4 off, dword off, byte mode on. Nothing
        ;      touches the CRTC's protected registers, so no CR11 dance.
        mov     al, 13h
        call    do_mode
        mov     ax, 00604h              ; SR4 = 06: chain-4 OFF, odd/even off
        call    wr_seq_ax
        mov     ax, 00014h              ; CR14 = 00: dword mode off
        call    wr_crtc
        mov     ax, 0E317h              ; CR17 = E3: byte mode on
        call    wr_crtc
        call    dump_all
        EMIT_BUF "vga.modeY.unchained", regbuf, 64

        ;      X adds the 480-line CRTC table and MiscOut E3, which is what makes
        ;      it 320x240. CR11 bit 7 write-protects CR00-CR07, so it is cleared
        ;      before CR06/CR07 and the table restores it at CR11 -- exactly as
        ;      the canonical listing does, which is also a live test of our
        ;      write-protect implementation.
        mov     dx, 03C2h
        mov     al, 0E3h                ; MiscOut: 25.175 MHz, both syncs -ve
        out     dx, al
        mov     ax, 00011h              ; CR11 = 00: protect OFF first
        call    wr_crtc
        mov     ax, 00D06h              ; CR06 vertical total
        call    wr_crtc
        mov     ax, 03E07h              ; CR07 overflow
        call    wr_crtc
        mov     ax, 04109h              ; CR09 max scan line = 1 -> rows are 2 lines
        call    wr_crtc
        mov     ax, 0EA10h              ; CR10 v retrace start
        call    wr_crtc
        mov     ax, 0AC11h              ; CR11 v retrace end + protect back ON
        call    wr_crtc
        mov     ax, 0DF12h              ; CR12 vertical display end
        call    wr_crtc
        mov     ax, 0E715h              ; CR15 v blank start
        call    wr_crtc
        mov     ax, 00616h              ; CR16 v blank end
        call    wr_crtc
        call    dump_all
        EMIT_BUF "vga.modeX.320x240", regbuf, 64

        ; ---- mode 7 is MONOCHROME: CRTC at 3B4, Input Status 1 at 3BA.
        ;      NTVDMEX claimed neither until step 1, so before it this line was
        ;      all-0xFF from the bus's absent-device default.
        mov     ax, 0007h
        int     10h
        mov     word [crtc_idx], 03B4h
        mov     word [stat1], 03BAh
        call    dump_all
        EMIT_BUF "vga.mode07mono", regbuf, 64

        ; ---- back to a known colour mode for the behavioural cases.
        mov     al, 03h
        call    do_mode

        ; ---- CR11 BIT 7 WRITE-PROTECTS CR00..CR07.
        ;      This is a BEHAVIOUR, not a value, so no static dump can show it:
        ;      with the bit set a write to CR00 must be REFUSED by the hardware.
        ;      NTVDMEX accepts it (the inventory says so), so this case is
        ;      expected to DISAGREE with the oracle until step 3 -- which is the
        ;      point of writing the check before the fix.
        mov     al, 00h                 ; save CR00
        call    rd_crtc
        mov     bl, al
        mov     al, 11h                 ; save CR11
        call    rd_crtc
        mov     bh, al

        or      al, 80h                 ; protect on
        mov     ah, al
        mov     al, 11h
        call    wr_crtc
        mov     ah, 55h                 ; try to write 0x55 to CR00
        mov     al, 00h
        call    wr_crtc
        mov     al, 00h
        call    rd_crtc
        xor     ah, ah                  ; see THE SIG NOTE below
        POISON
        call    probe_capture
        EMIT    "vga.cr11wp.protected.cr00", "AX"

        mov     al, bh                  ; protect off (restore CR11's own value)
        and     al, 7Fh
        mov     ah, al
        mov     al, 11h
        call    wr_crtc
        mov     ah, 55h                 ; now the write must land
        mov     al, 00h
        call    wr_crtc
        mov     al, 00h
        call    rd_crtc
        xor     ah, ah
        POISON
        call    probe_capture
        EMIT    "vga.cr11wp.open.cr00", "AX"

        mov     ah, bl                  ; put CR00 and CR11 back
        mov     al, 00h
        call    wr_crtc
        mov     ah, bh
        mov     al, 11h
        call    wr_crtc

        ; ---- DAC PIXEL MASK: default, and does it hold a written value?
        ;      3C6 was claimed by nobody before step 1, so a write vanished and a
        ;      read returned 0xFF by accident of the bus default -- which happens
        ;      to be the correct reset value, so only the WRITE separates them.
        mov     dx, 03C6h
        in      al, dx
        mov     bl, al                  ; save
        mov     al, 3Ch
        out     dx, al
        in      al, dx
        xor     ah, ah
        POISON
        call    probe_capture
        EMIT    "vga.dacmask.wr3C", "AX"
        mov     al, bl                  ; restore -- a masked DAC hides the screen
        out     dx, al

        ; ---- SEQUENCER READ-BACK. Our host answered 0 for every index but 2
        ;      until step 1, so a card-detect that writes and re-reads saw "no
        ;      card". SR2 (Map Mask) is safe to disturb and put back.
        mov     al, 02h
        call    rd_seq
        mov     bl, al
        mov     dx, 03C4h
        mov     al, 02h
        out     dx, al
        inc     dx
        mov     al, 05h
        out     dx, al
        mov     al, 02h
        call    rd_seq
        xor     ah, ah
        POISON
        call    probe_capture
        EMIT    "vga.sr2.readback05", "AX"
        mov     dx, 03C4h               ; restore
        mov     al, 02h
        out     dx, al
        inc     dx
        mov     al, bl
        out     dx, al

        ; ---- leave the machine in mode 3 with the palette driving the display.
        mov     ax, 0003h
        int     10h
        call    ac_enable

        PROBE_END
