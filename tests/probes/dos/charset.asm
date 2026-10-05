; charset.asm -- show the whole character set, in text mode and as the BIOS draws it
;                in a graphics mode, so a screenshot shows the font (#322).
;
; NTVDMEX builds its character tables at start-up from the system's fonts; this is
; the picture that says what came out.
;   screen 1 (mode 3, text, 8x16 cells): the 256 codes in a 16x16 grid, then a box
;            drawn from the single- and double-line box characters, whose lines must
;            join from cell to cell.
;   screen 2 (mode 12h, 640x480 graphics): the same 256 codes written by the BIOS
;            (INT 10h AH=0Eh), which draws them from the 8x16 table itself.
; Each screen waits for a key; Esc on the first skips the second.
;
; Assemble: nasm -f bin charset.asm -o charset.com   (CMake: build/probes/charset.com)

        org     100h

start:  mov     ax, 0003h               ; text mode 3
        int     10h
        mov     ax, 0B800h
        mov     es, ax

        ; -- the 16x16 grid at rows 2..17, columns 4..34 (two columns per code) ------
        xor     cx, cx                  ; cl = code
.grid:  mov     al, cl
        mov     ah, 0
        mov     bl, 16
        div     bl                      ; al = row (code/16), ah = col (code%16)
        mov     bl, ah
        mov     bh, 0
        mov     ah, 0
        add     ax, 2                   ; screen row
        mov     dx, 160
        mul     dx                      ; ax = row*160
        shl     bx, 2                   ; col*4 bytes (2 cells per code)
        add     ax, bx
        add     ax, 8                   ; left margin, 4 cells
        mov     di, ax
        mov     al, cl
        mov     ah, 07h
        stosw
        inc     cl
        jnz     .grid

        ; -- a box, columns 44..74, rows 2..10: double outside, single inside -----------
        mov     di, (2*80+44)*2
        mov     al, 0C9h                ; double top-left
        call    put
        mov     cx, 29
.top:   mov     al, 0CDh                ; double horizontal
        call    put
        loop    .top
        mov     al, 0BBh                ; double top-right
        call    put
        mov     dx, 3                   ; rows 3..9: double sides, a single-line inner box
.side:  mov     di, dx
        imul    di, di, 160
        add     di, 44*2
        mov     al, 0BAh                ; double vertical
        call    put
        mov     cx, 29
        mov     al, ' '
.fill:  call    put
        loop    .fill
        mov     al, 0BAh
        call    put
        inc     dx
        cmp     dx, 10
        jb      .side
        mov     di, (10*80+44)*2
        mov     al, 0C8h                ; double bottom-left
        call    put
        mov     cx, 29
.bot:   mov     al, 0CDh
        call    put
        loop    .bot
        mov     al, 0BCh                ; double bottom-right
        call    put
        ; inner single box rows 4..8, columns 50..68, with a cross in the middle
        mov     di, (4*80+50)*2
        mov     si, inner_t
        call    putrow
        mov     di, (5*80+50)*2
        mov     si, inner_m
        call    putrow
        mov     di, (6*80+50)*2
        mov     si, inner_x
        call    putrow
        mov     di, (7*80+50)*2
        mov     si, inner_m
        call    putrow
        mov     di, (8*80+50)*2
        mov     si, inner_b
        call    putrow
        ; shades and blocks under the box
        mov     di, (12*80+44)*2
        mov     si, shades
        call    putrow

        xor     ax, ax                  ; wait for a key
        int     16h
        cmp     al, 1Bh
        je      done

        ; -- screen 2: mode 12h, the BIOS writes all 256 codes ---------------------------
        mov     ax, 0012h
        int     10h
        xor     cx, cx
.bios:  mov     al, cl
        cmp     al, 7                   ; teletype interprets 07h/08h/0Ah/0Dh as controls:
        je      .dot                    ; show them as '.' so the grid stays square
        cmp     al, 8
        je      .dot
        cmp     al, 0Ah
        je      .dot
        cmp     al, 0Dh
        jne     .draw
.dot:   mov     al, '.'
.draw:  mov     ah, 0Eh
        mov     bx, 000Fh               ; page 0, white
        int     10h
        mov     al, ' '
        mov     ah, 0Eh
        int     10h
        inc     cl
        test    cl, 0Fh
        jnz     .nonl
        mov     ax, 0E0Dh               ; newline after every 16 codes
        int     10h
        mov     ax, 0E0Ah
        int     10h
.nonl:  or      cl, cl
        jnz     .bios
        xor     ax, ax
        int     16h

done:   mov     ax, 0003h
        int     10h
        mov     ax, 4C00h
        int     21h

put:    mov     ah, 07h                 ; AL at ES:DI, light grey; DI += 2
        stosw
        ret

putrow: lodsb                           ; zero-terminated string at DS:SI
        or      al, al
        jz      .r
        call    put
        jmp     putrow
.r:     ret

inner_t db 0DAh, 0C4h,0C4h,0C4h,0C4h,0C4h,0C4h,0C4h,0C4h, 0C2h, 0C4h,0C4h,0C4h,0C4h,0C4h,0C4h,0C4h,0C4h, 0BFh, 0
inner_m db 0B3h, '        ', 0B3h, '        ', 0B3h, 0
inner_x db 0C3h, 0C4h,0C4h,0C4h,0C4h,0C4h,0C4h,0C4h,0C4h, 0C5h, 0C4h,0C4h,0C4h,0C4h,0C4h,0C4h,0C4h,0C4h, 0B4h, 0
inner_b db 0C0h, 0C4h,0C4h,0C4h,0C4h,0C4h,0C4h,0C4h,0C4h, 0C1h, 0C4h,0C4h,0C4h,0C4h,0C4h,0C4h,0C4h,0C4h, 0D9h, 0
shades  db 0B0h,0B0h,0B0h,' ',0B1h,0B1h,0B1h,' ',0B2h,0B2h,0B2h,' ',0DBh,0DBh,0DBh,' '
        db 0DCh,0DCh,0DCh,' ',0DFh,0DFh,0DFh,' ',0DDh,0DDh,' ',0DEh,0DEh, 0
