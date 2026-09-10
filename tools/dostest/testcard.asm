; testcard.asm -- a TV-style test card for every video mode NTVDMEX claims.
;
; WHY THIS EXISTS. "Lemmings' screens are garbled" is a bug report you cannot act
; on: the guest draws an unknown picture through an unknown path, so a wrong
; result tells you nothing about WHICH part is wrong. A test card is the opposite
; -- a picture whose every pixel is known in advance, so the difference between
; what should be there and what IS there names the defect.
;
; ── WHAT EACH ELEMENT IS FOR. Nothing here is decoration. ──────────────────────
;   DIAGONAL from (0,0)      THE STRIDE TEST, and the sharpest thing in the card.
;                            At the correct bytes-per-row it is a straight 45
;                            degree line; one byte out and it bends or wraps into
;                            a barber's pole. This is the classic planar failure.
;   1-PIXEL BORDER           Geometry. Clipped or gapped tells you which edge and
;                            by how much; it is drawn through the Bit Mask
;                            register, so it also proves single-pixel writes work
;                            (bars only prove 8-pixel ones do).
;   COLOUR BARS              The palette and the value->colour mapping, in order,
;                            so a swapped or dropped PLANE shows as reordered or
;                            missing bars rather than as vague "wrong colours".
;   CORNER MARKERS           Origin and far corner are both addressable; catches
;                            an off-by-one row or a lost last column.
;   MODE TALLY (top-left)    Which card you are looking at, without needing a font
;                            in a graphics mode. Count the yellow blocks.
;
; ── IT DRAWS THE WAY A GAME DRAWS. ────────────────────────────────────────────
; Planar modes go through write mode 2 + Map Mask + Bit Mask straight to A000 --
; the same path Lemmings uses and the one the host traps in vga_planar_write. It
; deliberately does NOT use INT 10h AH=0Ch for the picture: the BIOS pixel call is
; a different path in the host, and a card drawn through it would prove nothing
; about the one games actually use.
;
; Auto-advances every ~2.5 s so a headless capture run sees every mode; any key
; advances immediately and ESC quits, so a person can dwell on one.
;
; Assemble: nasm -f bin testcard.asm -o testcard.com
bits 16
org 0x100

%define K_TEXT   0
%define K_PLANAR 1
%define K_LIN8   2
%define K_PL0    3            ; planar, drawn via WRITE MODE 0 per-plane
%define K_ATTR   4            ; planar + REVERSED Attribute Controller palette
%define K_SROR   5            ; planar + Set/Reset + ALU=OR over loaded latches
%define K_SRMIX  6            ; planar + PARTIAL Enable Set/Reset (0x0E): mixed sources

; table entry: mode, kind, bytes/row, pad, width_px(2), height(2)  = 8 bytes
%define ENT_SZ 8

start:
    cld
    ; ── A DIGIT ARGUMENT SELECTS ONE CARD AND HOLDS IT. ──────────────────────
    ;   testcard          cycle every mode, ~2.5 s each
    ;   testcard 3        show card 3 and stay on it
    ; The hold is what makes this usable as an ORACLE: scripts/dosoracle runs the
    ; program under genuine MS-DOS 6.22 and screendumps on timeout, so it can only
    ; capture a screen the guest is still showing. One mode per run, held.
    mov     si, 0x81                    ; PSP command tail
    mov     cl, [0x80]                  ; its length
    xor     ch, ch
    jcxz    .noarg
    ; ⚠ TWO DIGITS, NOT ONE. The first cut took a single digit, so `testcard 10`
    ;   selected card 1 -- and because the ORACLE ran the same binary with the same
    ;   argument, both sides rendered card 1 and the comparison came back 0 differ.
    ;   A test that agrees because neither side did anything is worse than a failing
    ;   one: it reports PASS for a path it never touched.
.scan:
    lodsb
    cmp     al, '0'
    jb      .nextc
    cmp     al, '9'
    ja      .nextc
    sub     al, '0'                     ; first digit
    mov     bl, al
    xor     bh, bh
    dec     cx
    jz      .setit
    lodsb                               ; a second digit?
    cmp     al, '0'
    jb      .setit
    cmp     al, '9'
    ja      .setit
    sub     al, '0'
    mov     dx, bx                      ; bx = bx*10 + al
    shl     bx, 1
    shl     bx, 1
    add     bx, dx
    shl     bx, 1
    xor     ah, ah
    add     bx, ax
.setit:
    mov     [midx], bx
    mov     [tally], bl
    mov     byte [hold], 1
    jmp     .noarg
.nextc:
    loop    .scan
.noarg:
.next_mode:
    mov     si, [midx]
    mov     cl, 3
    shl     si, cl                      ; * ENT_SZ
    add     si, modes
    mov     al, [si]
    cmp     al, 0xFF
    je      .wrap

    ; latch this mode's geometry where the drawing routines can see it
    mov     bl, [si+1]                  ; kind
    mov     cl, [si+2]
    xor     ch, ch
    mov     [wbytes], cx
    mov     cx, [si+4]
    mov     [wpx], cx
    mov     cx, [si+6]
    mov     [hrows], cx

    xor     ah, ah                      ; INT 10h AH=00 -- set video mode
    int     0x10

    cmp     bl, K_TEXT
    je      .do_text
    cmp     bl, K_LIN8
    je      .do_lin8
    cmp     bl, K_PL0
    je      .do_pl0
    cmp     bl, K_ATTR
    je      .do_attr
    cmp     bl, K_SROR
    je      .do_sror
    cmp     bl, K_SRMIX
    je      .do_srmix
    call    draw_planar
    jmp     .wait
.do_pl0:
    call    draw_planar0
    jmp     .wait
.do_attr:
    call    draw_planar
    call    attr_reverse
    jmp     .wait
.do_sror:
    call    draw_planar
    call    sr_or_band
    jmp     .wait
.do_srmix:
    call    draw_planar
    call    sr_mix_band
    jmp     .wait
.do_text:
    call    draw_text
    jmp     .wait
.do_lin8:
    call    draw_lin8
.wait:
    call    wait_or_key
    jc      .quit
    inc     word [midx]
    inc     byte [tally]
    jmp     .next_mode
.wrap:
    mov     word [midx], 0
    mov     byte [tally], 0
    jmp     .next_mode
.quit:
    mov     ax, 0x0003
    int     0x10
    mov     ax, 0x4C00
    int     0x21

; ── wait ~2.5 s or a key. CF set on ESC. ──────────────────────────────────────
; Timed off the BIOS tick at 0040:006C (18.2 Hz), not a delay loop: a spin count
; would make every card flash past at "Unlimited" and crawl at 4.77 MHz.
wait_or_key:
    push    es
    push    bx
    xor     ax, ax
    mov     es, ax
    mov     bx, [es:0x046C]
.spin:
    mov     ah, 0x01
    int     0x16
    jz      .notick
    mov     ah, 0x00
    int     0x16
    cmp     al, 27
    je      .esc
    clc
    jmp     .out
.notick:
    mov     ax, [es:0x046C]
    sub     ax, bx
    cmp     byte [hold], 0
    jne     .spin                       ; held: only a key ever leaves this card
    cmp     ax, 45                      ; ~2.5 s
    jb      .spin
    clc
    jmp     .out
.esc:
    stc
.out:
    pop     bx
    pop     es
    ret

; ── helpers ───────────────────────────────────────────────────────────────────
; DI = row BX * bytes-per-row. 16-bit multiply on purpose: mode 12h is 480 rows,
; and `mul byte` would have wrapped AL at 255 -- silently, and only on the tallest
; mode, which is exactly the sort of bug this program exists to catch.
row_off:
    push    dx
    push    cx
    mov     ax, bx
    mul     word [wbytes]               ; DX:AX = row * wbytes (max 38400)
    mov     di, ax
    pop     cx
    pop     dx
    ret

; ── ⚠⚠ A MASKED WRITE NEEDS THE LATCHES LOADED FIRST, AND A READ IS WHAT LOADS
;      THEM. In write mode 2 the pixels NOT selected by the Bit Mask come from the
;      VGA's four latch registers, and those only take a value when the CPU READS
;      video memory. Write without reading and the other seven pixels in the byte
;      get whatever was latched last -- zeros, i.e. black.
;    This card was written without it and the reference render caught it exactly:
;    the top row went black at x=312..318 (the byte the right border is written
;    into), the bottom row at x=192..198 (where the diagonal lands on row 199),
;    and the left border at y=1..7 (the rows where the diagonal is still inside
;    byte 0). Three unrelated-looking symptoms, one cause.
;    ES:DI must already point at the byte. AL is destroyed.
pl_latch:
    mov     al, [es:di]
    ret

; AL = bit mask -> GC index 8.
pl_bitmask:
    push    dx
    push    ax
    mov     dx, 0x3CE
    mov     al, 8
    out     dx, al
    inc     dx
    pop     ax
    out     dx, al
    pop     dx
    ret
pl_mask_ff:
    mov     al, 0xFF
    jmp     pl_bitmask

; ── PLANAR (0Dh / 0Eh / 10h / 12h) ────────────────────────────────────────────
draw_planar:
    mov     ax, 0xA000
    mov     es, ax
    ; write mode 2: the CPU byte's low nibble IS the colour; the Bit Mask decides
    ; which of the 8 pixels in that byte receive it.
    mov     dx, 0x3CE
    mov     al, 5
    out     dx, al
    inc     dx
    mov     al, 2
    out     dx, al
    mov     dx, 0x3C4                   ; Sequencer 2 = Map Mask, all four planes
    mov     al, 2
    out     dx, al
    inc     dx
    mov     al, 0x0F
    out     dx, al

    ; --- colour bars: build ONE row, then blast it to every line ------------
    ; Done this way because the per-pixel version needed CX for both the column
    ; counter and the divisor, which is how the first draft got tangled. One row
    ; built once is also what a real program would do.
    call    pl_mask_ff
    xor     cx, cx
.build:
    mov     ax, cx
    push    cx
    mov     cl, 4
    shl     ax, cl                      ; col * 16
    pop     cx
    xor     dx, dx
    div     word [wbytes]               ; AX = (col*16) / bytes_per_row = 0..15
    mov     si, cx
    mov     [barbuf + si], al
    inc     cx
    cmp     cx, [wbytes]
    jb      .build

    xor     bx, bx
.row:
    call    row_off
    mov     si, barbuf
    mov     cx, [wbytes]
    rep     movsb                       ; DS:SI -> ES:DI
    inc     bx
    cmp     bx, [hrows]
    jb      .row

    call    pl_edges
    call    pl_diag
    call    pl_mask_ff
    call    pl_corners
    call    pl_tally
    ret

; top and bottom rows solid, then 1-pixel left/right columns via the Bit Mask.
pl_edges:
    call    pl_mask_ff
    xor     bx, bx
    call    row_off
    mov     cx, [wbytes]
    mov     al, 0x0F
    rep     stosb                       ; top row
    mov     bx, [hrows]
    dec     bx
    call    row_off
    mov     cx, [wbytes]
    mov     al, 0x0F
    rep     stosb                       ; bottom row
    ; 1-pixel left and right columns, through the Bit Mask register -- the bars
    ; above only prove 8-pixel writes work, these prove single-pixel ones do.
    xor     bx, bx
.v:
    mov     al, 0x80
    call    pl_bitmask
    call    row_off
    call    pl_latch                    ; load the latches -- see pl_latch
    mov     byte [es:di], 0x0F
    mov     al, 0x01
    call    pl_bitmask
    call    row_off
    add     di, [wbytes]
    dec     di
    call    pl_latch
    mov     byte [es:di], 0x0F
    inc     bx
    cmp     bx, [hrows]
    jb      .v
    ret

; the diagonal, drawn one pixel at a time -- stops at min(width, height).
pl_diag:
    mov     cx, [hrows]
    cmp     cx, [wpx]
    jbe     .have
    mov     cx, [wpx]
.have:
    dec     cx                          ; stop one short: a diagonal that TOUCHES the
    mov     [.lim], cx                  ; border reads as a border defect, and the
    mov     bx, 1                       ; whole point of the card is unambiguity
.d:
    mov     ax, bx                      ; x = y
    mov     cl, 3
    shr     ax, cl
    mov     si, ax                      ; byte index within the row
    mov     ax, bx
    and     ax, 7
    mov     cl, al
    mov     al, 0x80
    shr     al, cl
    call    pl_bitmask
    call    row_off
    add     di, si
    call    pl_latch                    ; load the latches -- see pl_latch
    mov     byte [es:di], 0x0C          ; red, reads clearly over the bars
    inc     bx
    cmp     bx, [.lim]
    jb      .d
    ret
.lim: dw 0

pl_corners:
    xor     bx, bx
    call    row_off
    mov     byte [es:di], 0x0F
    mov     byte [es:di+1], 0x0F
    mov     bx, [hrows]
    dec     bx
    call    row_off
    mov     byte [es:di], 0x0F
    mov     byte [es:di+1], 0x0F
    ret

; N yellow blocks on row 3 -- which card is this, with no font available.
pl_tally:
    mov     bx, 3
    call    row_off
    add     di, 2
    mov     cl, [tally]
    xor     ch, ch
    inc     cx
.t:
    mov     byte [es:di], 0x0E
    add     di, 2
    loop    .t
    ret

; ── PLANAR VIA WRITE MODE 0, ONE PLANE AT A TIME ──────────────────────────────
; ★ THIS IS THE PATH LEMMINGS ACTUALLY USES, and until this card existed nothing
;   tested it. Measured on the rig, one Lemmings run:
;       wmode hist: 00x12e5 01x1 02x0 03x0
;       pairs: w00/m01=2 w00/m02=4828 w00/m04=4820 w00/m08=4785 w00/m0f=4835
;   i.e. ~4800 writes in WRITE MODE 0 with the Map Mask selecting ONE PLANE at a
;   time, and not a single write in mode 2 -- which is the only mode the rest of
;   this card exercises. A card that passes while the game is wrong is a card that
;   is testing the wrong thing.
;
; In write mode 0 the CPU byte is the PLANE's bits directly (8 pixels' worth of one
; bit), and the Map Mask says which planes receive it. So the same 16 colour bars
; are built as four separate bit-planes and blasted one plane at a time.
draw_planar0:
    mov     ax, 0xA000
    mov     es, ax
    mov     dx, 0x3CE                   ; GC 5 = write mode 0
    mov     al, 5
    out     dx, al
    inc     dx
    xor     al, al
    out     dx, al
    call    pl_mask_ff                  ; GC 8 = bit mask, all bits
    mov     dx, 0x3CE                   ; GC 3 = data rotate/function: replace
    mov     al, 3
    out     dx, al
    inc     dx
    xor     al, al
    out     dx, al

    xor     bp, bp                      ; bp = plane 0..3
.plane:
    mov     ax, 2                       ; Sequencer 2 = Map Mask
    mov     dx, 0x3C4
    out     dx, al
    inc     dx
    mov     cx, bp
    mov     al, 1
    shl     al, cl                      ; this plane only
    out     dx, al

    ; build one row of THIS plane's bits: bit set where (colour >> plane) & 1
    xor     cx, cx
.build:
    mov     ax, cx
    push    cx
    mov     cl, 4
    shl     ax, cl
    pop     cx
    xor     dx, dx
    div     word [wbytes]               ; ax = colour 0..15 for this byte column
    push    cx
    mov     cx, bp
    shr     ax, cl                      ; >> plane
    pop     cx
    test    al, 1
    mov     al, 0
    jz      .zero
    mov     al, 0xFF                    ; all 8 pixels of this byte have the bit
.zero:
    mov     si, cx
    mov     [barbuf + si], al
    inc     cx
    cmp     cx, [wbytes]
    jb      .build

    xor     bx, bx
.row:
    call    row_off
    mov     si, barbuf
    mov     cx, [wbytes]
    rep     movsb
    inc     bx
    cmp     bx, [hrows]
    jb      .row

    inc     bp
    cmp     bp, 4
    jb      .plane

    ; borders/diagonal/corners still go through write mode 2 -- they are proven,
    ; and mixing the two is itself worth testing: a game does exactly that.
    mov     dx, 0x3CE
    mov     al, 5
    out     dx, al
    inc     dx
    mov     al, 2
    out     dx, al
    mov     dx, 0x3C4
    mov     al, 2
    out     dx, al
    inc     dx
    mov     al, 0x0F
    out     dx, al
    call    pl_edges
    call    pl_diag
    call    pl_mask_ff
    call    pl_corners
    call    pl_tally
    ret

; ── PARTIAL ENABLE SET/RESET: TWO DATA SOURCES IN ONE WRITE ───────────────────
; ★ ensr=0x0E IS 435,852 OF LEMMINGS' WRITES and nothing tests it. Card 9 uses
;   0x0F, where ALL FOUR planes take the Set/Reset colour and the CPU byte is
;   irrelevant. 0x0E is structurally different: planes 1,2,3 take Set/Reset while
;   PLANE 0 TAKES THE CPU BYTE. One write, two sources, and the split between them
;   is a per-plane decision -- exactly the kind of thing an implementation gets
;   subtly wrong while passing every all-or-nothing test.
;
; Set/Reset = 0x0A (planes 1 and 3 on, plane 2 off) and a CPU byte of 0xAA, with the
; replace function so the latches cannot mask a mistake. Per pixel that gives
; bit1=1, bit2=0, bit3=1 and bit0 alternating from 0xAA -- so the band must come out
; as colours 10 and 11 alternating. Anything else names which plane took the wrong
; source.
sr_mix_band:
    push    dx
    mov     ax, 0xA000
    mov     es, ax
    mov     dx, 0x3CE                   ; GR5 = write mode 0
    mov     al, 5
    out     dx, al
    inc     dx
    xor     al, al
    out     dx, al
    mov     dx, 0x3CE                   ; GR0 = Set/Reset = 0x0A
    mov     al, 0
    out     dx, al
    inc     dx
    mov     al, 0x0A
    out     dx, al
    mov     dx, 0x3CE                   ; GR1 = Enable Set/Reset = 0x0E
    mov     al, 1
    out     dx, al
    inc     dx
    mov     al, 0x0E
    out     dx, al
    mov     dx, 0x3CE                   ; GR3 = replace, rotate 0
    mov     al, 3
    out     dx, al
    inc     dx
    xor     al, al
    out     dx, al
    call    pl_mask_ff
    mov     dx, 0x3C4                   ; Map Mask: all planes
    mov     al, 2
    out     dx, al
    inc     dx
    mov     al, 0x0F
    out     dx, al

    mov     bx, [hrows]
    shr     bx, 1
    mov     ax, [hrows]
    shr     ax, 2
    add     ax, bx
    mov     [.endrow], ax
.band:
    call    row_off
    mov     cx, [wbytes]
.b1:
    mov     byte [es:di], 0xAA          ; plane 0 takes THIS; planes 1-3 take set/reset
    inc     di
    dec     cx
    jnz     .b1
    inc     bx
    cmp     bx, [.endrow]
    jb      .band

    mov     dx, 0x3CE                   ; leave the GC as we found it
    mov     al, 1
    out     dx, al
    inc     dx
    xor     al, al
    out     dx, al
    pop     dx
    ret
.endrow: dw 0

; ── SET/RESET + ALU=OR OVER LOADED LATCHES ────────────────────────────────────
; ★ THIS IS THE IDIOM LEMMINGS ACTUALLY DRAWS WITH, and nothing tested it. Measured
;   inside vga_planar_write on a real gameplay run -- the registers as they were AT
;   EACH WRITE, not sampled at exit:
;       ensr@write 0x00=1252120 0x0c=41792 0x0e=435852 0x0f=189508
;       alu        0=1423170    2=496102
;   So half a million writes go through Enable Set/Reset with the ALU set to OR,
;   combining the Set/Reset colour with the LATCHES. Every other card here uses the
;   replace function with set/reset off, which exercises none of that.
;
; What this draws: the colour bars, then a band redrawn as (bar OR 5). On correct
; hardware every pixel in the band becomes its bar colour with bits 0 and 2 forced
; on. Any disagreement isolates the ALU, the Enable Set/Reset gating, or the latches.
;
; ⚠ THE READ IS THE POINT. OR-with-latch is meaningless unless the latches hold the
;   destination, and only a READ loads them -- the same trap that bit the border.
sr_or_band:
    push    dx
    mov     ax, 0xA000
    mov     es, ax
    mov     dx, 0x3CE                   ; GR5 = write mode 0
    mov     al, 5
    out     dx, al
    inc     dx
    xor     al, al
    out     dx, al
    mov     dx, 0x3CE                   ; GR0 = Set/Reset value = 5
    mov     al, 0
    out     dx, al
    inc     dx
    mov     al, 5
    out     dx, al
    mov     dx, 0x3CE                   ; GR1 = Enable Set/Reset, all four planes
    mov     al, 1
    out     dx, al
    inc     dx
    mov     al, 0x0F
    out     dx, al
    mov     dx, 0x3CE                   ; GR3 = function select 2 (OR), rotate 0
    mov     al, 3
    out     dx, al
    inc     dx
    mov     al, 0x10                    ; bits 3-4 = 10b = OR
    out     dx, al
    call    pl_mask_ff
    mov     dx, 0x3C4                   ; Map Mask: all planes
    mov     al, 2
    out     dx, al
    inc     dx
    mov     al, 0x0F
    out     dx, al

    ; ⚠ COUNT AGAINST AN END ROW HELD IN MEMORY, not a register. The first cut kept
    ;   the row count in SI across `call row_off` and `call pl_latch`, and the loop ran
    ;   away -- 144000 writes where 2000 were intended. That mattered for more than
    ;   tidiness: a runaway write clips differently on real hardware than it does
    ;   against our bounds check, so the card would have "found" an emulator bug that
    ;   was entirely its own. Verify the instrument before believing its verdict.
    mov     bx, [hrows]                 ; band starts at h/2
    shr     bx, 1
    mov     ax, [hrows]
    shr     ax, 2                       ; ...and is h/4 rows tall
    add     ax, bx
    mov     [.endrow], ax
.band:
    call    row_off
    mov     cx, [wbytes]
.b1:
    call    pl_latch                    ; load the latches from the destination
    mov     byte [es:di], 0             ; CPU byte unused: all planes take set/reset
    inc     di
    dec     cx
    jnz     .b1
    inc     bx
    cmp     bx, [.endrow]
    jb      .band

    ; put the Graphics Controller back so later cards are unaffected
    mov     dx, 0x3CE
    mov     al, 1
    out     dx, al
    inc     dx
    xor     al, al                      ; Enable Set/Reset off
    out     dx, al
    mov     dx, 0x3CE
    mov     al, 3
    out     dx, al
    inc     dx
    xor     al, al                      ; function select back to replace
    out     dx, al
    pop     dx
    ret
.endrow: dw 0

; ── REVERSE THE ATTRIBUTE CONTROLLER PALETTE ──────────────────────────────────
; ★ WHY THIS CARD EXISTS. In a 16-colour planar mode the 4-bit pixel value does NOT
;   index the DAC directly -- it indexes the ATTRIBUTE CONTROLLER's 16 palette
;   registers (port 0x3C0), and THAT result reaches the DAC. A guest which
;   reprograms those registers changes every colour on screen without touching a
;   single pixel.
;   src/vdd/vdd_video.c claims 0x3C4/5 (Sequencer), 0x3C7-9 (DAC), 0x3CE/F (Graphics
;   Controller), 0x3D4/5 (CRTC) and 0x3DA -- but NOT 0x3C0. So on NTVDMEX these OUTs
;   go nowhere and the colours should not change, while on real hardware the bars
;   run white-to-black instead of black-to-white. An unmistakable difference.
;
; ⚠ THE INDEX/DATA FLIP-FLOP. 0x3C0 is index and data on the SAME port, alternating,
;   and the toggle is reset by READING 0x3DA. Miss that read and every value lands
;   in the wrong half. Bit 5 of the index must be 0 while programming the palette
;   and set again afterwards, or the display stays blanked.
attr_reverse:
    push    dx
    mov     dx, 0x3DA
    in      al, dx                      ; reset the index/data flip-flop
    xor     cx, cx
.p:
    mov     dx, 0x3C0
    mov     al, cl                      ; index = palette register 0..15, bit5=0
    out     dx, al
    mov     si, 15
    sub     si, cx                      ; reversed: reg i gets default colour 15-i
    mov     al, [defpal + si]
    out     dx, al                      ; same port, data half
    inc     cx
    cmp     cx, 16
    jb      .p
    mov     dx, 0x3C0
    mov     al, 0x20                    ; bit 5 back on -- re-enable video output
    out     dx, al
    pop     dx
    ret
; the standard 16-colour attribute palette, which is what the modes come up with
defpal: db 0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07
        db 0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F

; ── LINEAR 8bpp (13h) ─────────────────────────────────────────────────────────
draw_lin8:
    mov     ax, 0xA000
    mov     es, ax
    xor     bx, bx
.row:
    mov     ax, bx
    mov     cx, 320
    mul     cx
    mov     di, ax
    xor     cx, cx
.col:
    mov     ax, cx
    add     ax, bx
    mov     [es:di], al                 ; every one of the 256 indices appears
    inc     di
    inc     cx
    cmp     cx, 320
    jb      .col
    inc     bx
    cmp     bx, 200
    jb      .row
    ; border
    xor     di, di
    mov     cx, 320
    mov     al, 0x0F
    rep     stosb
    mov     di, 199*320
    mov     cx, 320
    mov     al, 0x0F
    rep     stosb
    xor     bx, bx
.v:
    mov     ax, bx
    mov     cx, 320
    mul     cx
    mov     di, ax
    mov     byte [es:di], 0x0F
    add     di, 319
    mov     byte [es:di], 0x0F
    inc     bx
    cmp     bx, 200
    jb      .v
    ; diagonal -- 1..198, clear of the border for the same reason as the planar one
    mov     bx, 1
.d:
    mov     ax, bx
    mov     cx, 320
    mul     cx
    add     ax, bx
    mov     di, ax
    mov     byte [es:di], 0x0C
    inc     bx
    cmp     bx, 199
    jb      .d
    ; tally
    mov     di, 3*320 + 2
    mov     cl, [tally]
    xor     ch, ch
    inc     cx
.t:
    mov     byte [es:di], 0x0E
    add     di, 2
    loop    .t
    ret

; ── TEXT (03h) ────────────────────────────────────────────────────────────────
; One attribute per row, solid blocks: a wrong attribute decode or a wrong row
; stride is readable at a glance.
draw_text:
    mov     ax, 0xB800
    mov     es, ax
    xor     di, di
    xor     bx, bx
.row:
    mov     cx, 80
    mov     ah, bl
    mov     al, 0xDB
.col:
    mov     [es:di], ax
    add     di, 2
    loop    .col
    inc     bx
    cmp     bx, 25
    jb      .row
    ret

; ── data ──────────────────────────────────────────────────────────────────────
;      mode  kind      bytes/row  pad  width  height
modes:
    db 0x03, K_TEXT,   0,  0
    dw 720, 400
    db 0x0D, K_PLANAR, 40, 0
    dw 320, 200
    db 0x0E, K_PLANAR, 80, 0
    dw 640, 200
    db 0x10, K_PLANAR, 80, 0
    dw 640, 350
    db 0x12, K_PLANAR, 80, 0
    dw 640, 480
    db 0x13, K_LIN8,   0,  0
    dw 320, 200
    db 0x0D, K_PL0,    40, 0            ; 6: 0Dh drawn via WRITE MODE 0 per-plane
    dw 320, 200
    db 0x10, K_PL0,    80, 0            ; 7: 10h ditto -- the mode Lemmings draws in
    dw 640, 350
    db 0x10, K_ATTR,   80, 0            ; 8: 10h + REVERSED attribute palette
    dw 640, 350
    db 0x0D, K_SROR,   40, 0            ; 9: Set/Reset + ALU=OR -- Lemmings' idiom
    dw 320, 200
    db 0x0D, K_SRMIX,  40, 0            ; 10: partial Enable Set/Reset (0x0E)
    dw 320, 200
    db 0xFF, 0,        0,  0
    dw 0, 0

midx:    dw 0
tally:   db 0
hold:    db 0
wbytes:  dw 40
barbuf:  times 80 db 0
wpx:     dw 320
hrows:   dw 200
