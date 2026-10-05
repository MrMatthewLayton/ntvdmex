; blitfast.asm -- mode-12h random filled rectangles, the EFFICIENT planar way.
;
; Same visual idea as the QuickBasic BLIT.EXE demo (endless random solid
; rectangles) but drawn the way real 12h software draws: VGA write-mode 2 +
; Map Mask 0x0F (set ONCE), then each rectangle scanline is a single `REP STOSB`
; with AL = colour. In write-mode 2 the CPU data byte's low nibble IS the colour
; (one bit per plane), so there are NO per-pixel OUTs at all, and each scanline
; run faults exactly ONCE -- the host batches the whole REP through the planar
; engine in one trap.
;
; Contrast with QB's BLIT: QuickBasic's runtime plots one pixel at a time with
; XCHG ES:[DI],AL + a per-pixel Graphics-Controller OUT, i.e. ~50 instructions
; and 2 traps PER PIXEL. This program is the head-to-head control: if it's fast
; and BLIT.EXE is slow, the "12h is slow" wall is purely QB's drawing method.
;
; Press any key to exit. Assemble: nasm -f bin blitfast.asm -o blitfast.com
bits 16
org 0x100

start:
    mov ah, 0
    int 0x1A                    ; CX:DX = BIOS timer ticks -> PRNG seed
    mov [seed], dx

    mov ax, 0x0012              ; set mode 12h (640x480x16 planar)
    int 0x10

    ; --- one-time VGA setup: write mode 2, bit mask 0xFF, map mask 0x0F -------
    mov dx, 0x3CE               ; GC index 5 (mode) = 2
    mov al, 5
    out dx, al
    inc dx                      ; 0x3CF
    mov al, 2
    out dx, al

    mov dx, 0x3CE               ; GC index 8 (bit mask) = 0xFF (write all 8 bits)
    mov al, 8
    out dx, al
    inc dx
    mov al, 0xFF
    out dx, al

    mov dx, 0x3C4               ; Seq index 2 (map mask) = 0x0F (all 4 planes)
    mov al, 2
    out dx, al
    inc dx                      ; 0x3C5
    mov al, 0x0F
    out dx, al

.loop:
    mov ah, 0x01                ; INT 16h AH=01 -> exit on any key
    int 0x16
    jnz .done

    ; --- random rectangle parameters -----------------------------------------
    call rand
    and al, 0x0F
    mov [color], al             ; colour 0..15

    call rand
    and al, 0x3F
    mov [xc], al                ; byte column 0..63 (x = xc*8)

    call rand
    and al, 0x0F
    inc al
    mov [wb], al                ; width 1..16 bytes (8..128 px)

    call rand
    and ax, 0x1FF               ; 0..511
    cmp ax, 440
    jb .yok
    mov ax, 439
.yok:
    mov [yt], ax                ; top row 0..439

    call rand
    and al, 0x1F
    inc al
    mov ah, 0
    mov bp, ax                  ; height 1..32 rows (row counter)

    ; --- fill the rectangle: one REP STOSB per scanline ----------------------
    mov ax, 0xA000
    mov es, ax                  ; ES = video segment
    mov bx, [yt]                ; bx = current row
.rowloop:
    mov ax, bx                  ; di = row*80 + xc
    mov dx, 80
    mul dx                      ; ax = row*80 (row<480 -> fits 16-bit)
    xor dh, dh
    mov dl, [xc]
    add ax, dx
    mov di, ax
    mov cl, [wb]
    xor ch, ch                  ; cx = width in bytes
    mov al, [color]             ; write-mode 2: AL low nibble = colour
    rep stosb                   ; <-- one fault, host batches the whole run
    inc bx
    cmp bx, 480
    jae .rowdone                ; clip at the bottom edge
    dec bp
    jnz .rowloop
.rowdone:
    jmp .loop

.done:
    mov ax, 0x0003              ; back to text mode
    int 0x10
    mov ax, 0x4C00              ; exit
    int 0x21

; --- 16-bit linear-congruential PRNG -> AX -----------------------------------
rand:
    mov ax, [seed]
    mov dx, 25173
    mul dx
    add ax, 13849
    mov [seed], ax
    ret

seed:  dw 0
color: db 0
xc:    db 0
wb:    db 0
yt:    dw 0
