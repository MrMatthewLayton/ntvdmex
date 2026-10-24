; gcursor.com -- INT 33h 09h, BY EYE: is the guest's graphics cursor drawn, with its
; masks and its hot spot? (GH #264)
;
; Lemmings, Doom, ZAR, Duke3D and Heretic never call 09h (gc=0 in every log on file):
; they draw their own pointers. So the overlay that draws a guest bitmap has never
; been seen. This program defines one and puts it on a marked pixel, in three modes.
;
; THE BITMAP, four bands of four rows, 16 pixels wide:
;     rows  0-3   screen 0000 cursor 0000   BLACK
;     rows  4-7   screen 0000 cursor FFFF   WHITE (XOR with the driver's "ones")
;     rows  8-11  screen FFFF cursor FFFF   the background INVERTED
;     rows 12-15  screen FFFF cursor 0000   TRANSPARENT (nothing drawn)
; Hot spot (11,9): off centre on purpose, inside the inverted band.
;
; THE TARGET. The pointer is moved (04h) to the middle of the screen and a yellow '+'
; is drawn with its centre on that pixel. Correct: the '+' centre sits 11 pixels from
; the box's left edge and 9 rows below its top, so more of the box is LEFT of and
; ABOVE the centre. The background is two colours, split at the centre column, so the
; box straddles the split and its inverted band shows both colours inverted. Moving
; the mouse moves the box; the '+' stays where it was drawn.
; CGA mode 04h: one bit PAIR a pixel, so the same words make an 8-pixel-wide box.
;
; KEYS: 1 = mode 13h, 2 = mode 12h, 3 = mode 04h, Esc = quit.
;
; Assemble: nasm -f bin gcursor.asm -o gcursor.com
bits 16
org 0x100

HOT_X       equ 11
HOT_Y       equ 9

start:
    mov byte [mode], 0x13

.redraw:
    mov al, [mode]
    xor ah, ah
    int 0x10                    ; mode set clears the screen
    call fill_background
    xor ax, ax                  ; 00h: reset the driver (its default cursor)
    int 0x33
    call set_pointer            ; 04h: the pointer to the middle
    call draw_target            ; the '+' on that pixel
    call print_legend
    mov ax, 0x0009              ; 09h: define the graphics cursor
    mov bx, HOT_X
    mov cx, HOT_Y
    push ds
    pop es
    mov dx, cursor_masks
    int 0x33
    mov ax, 0x0001              ; 01h: show it
    int 0x33

.key:
    xor ah, ah                  ; INT 16h 00h: wait for a key
    int 0x16
    cmp al, 0x1B
    je .quit
    mov bl, 0x13
    cmp al, '1'
    je .switch
    mov bl, 0x12
    cmp al, '2'
    je .switch
    mov bl, 0x04
    cmp al, '3'
    je .switch
    jmp .key
.switch:
    mov [mode], bl
    mov ax, 0x0002              ; 02h: hide before the mode changes
    int 0x33
    jmp .redraw

.quit:
    mov ax, 0x0002
    int 0x33
    mov ax, 0x0003
    int 0x10
    mov ax, 0x4C00
    int 0x21

; The pointer to the middle of the driver's virtual screen: 640 across in every mode
; here; 200 rows in 13h and 04h, 480 in 12h.
set_pointer:
    mov ax, 0x0004
    mov cx, 320
    mov dx, 100
    cmp byte [mode], 0x12
    jne .set
    mov dx, 240
.set:
    int 0x33
    ret

; Left half one colour, right half another, written straight to video memory.
fill_background:
    push es
    cmp byte [mode], 0x13
    je .m13
    cmp byte [mode], 0x12
    je .m12
    ; CGA 04h: B800, even rows at 0000, odd rows at 2000, 80 bytes a row; four 2-bit
    ; pixels a byte. Left = colour 1 (55h), right = colour 2 (AAh).
    mov ax, 0xB800
    mov es, ax
    xor di, di
    mov bx, 200
.cga_row:
    mov cx, 40
    mov al, 0x55
    rep stosb
    mov cx, 40
    mov al, 0xAA
    rep stosb
    dec bx
    cmp bx, 100                 ; after 100 rows the even bank is full: odd bank
    jne .cga_next
    mov di, 0x2000
.cga_next:
    test bx, bx
    jnz .cga_row
    jmp .done
.m13:
    ; 13h: A000, 320 bytes a row. Left = colour 1 (blue), right = colour 4 (red).
    mov ax, 0xA000
    mov es, ax
    xor di, di
    mov bx, 200
.r13:
    mov cx, 160
    mov al, 1
    rep stosb
    mov cx, 160
    mov al, 4
    rep stosb
    dec bx
    jnz .r13
    jmp .done
.m12:
    ; 12h: planar, 80 bytes a row. The mode set left every plane 0; the Map Mask picks
    ; the planes a write of FFh sets, so it IS the colour. Left = 1, right = 4.
    mov ax, 0xA000
    mov es, ax
    xor di, di
    mov bx, 480
.r12:
    mov dx, 0x3C4
    mov ax, 0x0102              ; Map Mask = 1
    out dx, ax
    mov cx, 40
    mov al, 0xFF
    rep stosb
    mov ax, 0x0402              ; Map Mask = 4
    out dx, ax
    mov cx, 40
    mov al, 0xFF
    rep stosb
    dec bx
    jnz .r12
    mov ax, 0x0F02              ; Map Mask back to all planes
    out dx, ax
.done:
    pop es
    ret

; A '+' of arms 20 pixels long through the pointer's pixel, in yellow (14; colour 3
; in CGA palette 1, white). INT 10h 0Ch writes each pixel.
draw_target:
    mov ax, 0x0003              ; 03h: where the driver says the pointer is (CX,DX)
    int 0x33
    cmp byte [mode], 0x12
    je .pixel_y_ok
    ; 13h and 04h: 320 pixels across a 640-wide virtual screen
    shr cx, 1
.pixel_y_ok:
    mov [centre_x], cx
    mov [centre_y], dx
    mov al, 14
    cmp byte [mode], 0x04
    jne .colour_ok
    mov al, 3
.colour_ok:
    mov [target_colour], al
    ; horizontal arm
    mov cx, [centre_x]
    sub cx, 20
    mov si, 41
.h:
    mov dx, [centre_y]
    call plot
    inc cx
    dec si
    jnz .h
    ; vertical arm
    mov cx, [centre_x]
    mov dx, [centre_y]
    sub dx, 20
    mov si, 41
.v:
    call plot
    inc dx
    dec si
    jnz .v
    ret

plot:
    push cx
    push dx
    mov ah, 0x0C
    mov al, [target_colour]
    xor bh, bh
    int 0x10
    pop dx
    pop cx
    ret

; The legend at the top left, in white (colour 15; 3 in CGA).
print_legend:
    mov ah, 0x02                ; cursor to row 0, column 0
    xor bh, bh
    xor dx, dx
    int 0x10
    mov si, legend
    mov bl, 15
    cmp byte [mode], 0x04
    jne .next
    mov bl, 3
.next:
    lodsb
    test al, al
    jz .out
    mov ah, 0x0E
    xor bh, bh
    int 0x10
    jmp .next
.out:
    ret

legend:
    db "INT 33h 09h cursor  #264", 13, 10
    db "black/white/invert/clear", 13, 10
    db "hot spot 11,9 on + centre", 13, 10
    db "1=13h 2=12h 3=04h Esc=quit", 0

; 09h's 32 words: 16 screen (AND) masks, then 16 cursor (XOR) masks. Bit 15 = leftmost.
cursor_masks:
    dw 0x0000, 0x0000, 0x0000, 0x0000    ; screen: rows 0-3   black
    dw 0x0000, 0x0000, 0x0000, 0x0000    ;         rows 4-7   white
    dw 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF    ;         rows 8-11  inverted
    dw 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF    ;         rows 12-15 transparent
    dw 0x0000, 0x0000, 0x0000, 0x0000    ; cursor: rows 0-3
    dw 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF    ;         rows 4-7
    dw 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF    ;         rows 8-11
    dw 0x0000, 0x0000, 0x0000, 0x0000    ;         rows 12-15

mode:          db 0
target_colour: db 0
centre_x:      dw 0
centre_y:      dw 0
