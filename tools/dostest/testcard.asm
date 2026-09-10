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
.scan:
    lodsb
    cmp     al, '0'
    jb      .nextc
    cmp     al, '9'
    ja      .nextc
    sub     al, '0'
    xor     ah, ah
    mov     [midx], ax
    mov     [tally], al
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
    call    draw_planar
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
    db 0xFF, 0,        0,  0
    dw 0, 0

midx:    dw 0
tally:   db 0
hold:    db 0
wbytes:  dw 40
barbuf:  times 80 db 0
wpx:     dw 320
hrows:   dw 200
