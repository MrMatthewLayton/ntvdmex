; fontprobe.asm -- draw text with the font the BIOS hands out, exactly as Skyroads does.
;
; Skyroads asks INT 10h AX=1130h for BH=3 (ROM 8x8, chars 0-127) and BH=4 (upper 128) --
; measured -- and then renders its own text from the returned ES:BP. So the quality of that
; table IS the quality of the game's text. This reproduces that in isolation: get the
; pointer, blit a string from it into mode 13h, and hold the screen so a screenshot catches
; it. If the letters come out merged and blobby here, the game's "garbled text" is our font
; and not the game.
;
; Two rows are drawn from the SAME table:
;   row 1: "ROAD COMPLETED" -- the string the bug report is about
;   row 2: "EB80#@" -- glyphs whose internal gaps are the first thing a bad downsample eats
;
; Assemble: nasm -f bin fontprobe.asm -o fontprobe.com
bits 16
org 0x100

VRAM      equ 0xA000
SCR_W     equ 320
HOLD_TICKS equ 200              ; ~11 s on screen, long enough to be captured

start:
    cld
    mov ax, 0x0013              ; mode 13h, 320x200x256
    int 0x10

    mov ax, 0x1130              ; get font pointer: BH=3 = ROM 8x8, chars 0-127
    mov bh, 0x03
    int 0x10
    mov [fseg], es              ; ES:BP -> the table
    mov [foff], bp
    mov [fcx], cx               ; bytes per character (should be 8)

    mov si, s_line1
    mov word [px], 16
    mov word [py], 40
    call draw_str

    mov si, s_line2
    mov word [px], 16
    mov word [py], 64
    call draw_str

    ; hold the screen, then leave cleanly so the harness collects the log
    mov ah, 0x00
    int 0x1A
    mov [t0], dx
.hold:
    mov ah, 0x00
    int 0x1A
    sub dx, [t0]
    cmp dx, HOLD_TICKS
    jb .hold

    mov ax, 0x0003              ; back to text so the log is readable
    int 0x10
    mov dx, s_done
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21

; draw the $-terminated string at DS:SI at (px,py), 8x8 from the BIOS table
draw_str:
.next:
    lodsb
    cmp al, '$'
    je .done
    call draw_char
    add word [px], 8
    jmp .next
.done:
    ret

; draw AL using the font at [fseg]:[foff]. DS is pointed at the font table, so every
; access to our own variables needs a CS override while it is.
draw_char:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    xor ah, ah
    mov bx, ax                  ; BX = character
    mov ax, [fcx]
    mul bx                      ; AX = char * bytes-per-char (8 * 255 fits)
    add ax, [foff]
    mov si, ax                  ; SI = offset of this glyph
    mov ax, VRAM
    mov es, ax
    mov ax, [fseg]
    mov ds, ax                  ; DS:SI -> the glyph's rows

    xor bx, bx                  ; BX = row index
.row:
    mov ax, [cs:py]             ; DI = (py + row) * 320 + px
    add ax, bx
    mov dx, SCR_W
    push bx
    mul dx
    add ax, [cs:px]
    mov di, ax
    pop bx

    mov ah, [si + bx]           ; the row's 8 pixels, bit 7 = leftmost
    mov cx, 8
.pixel:
    mov dl, 0x00                ; background
    test ah, 0x80
    jz .put
    mov dl, 0x0F                ; white
.put:
    mov [es:di], dl
    inc di
    shl ah, 1
    loop .pixel

    inc bx
    cmp bx, [cs:fcx]
    jb .row

    push cs                     ; restore DS = our data
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

fseg  dw 0
foff  dw 0
fcx   dw 8
px    dw 0
py    dw 0
t0    dw 0
s_line1 db 'ROAD COMPLETED$'
s_line2 db 'EB80#@ABC$'
s_done  db 'fontprobe: drew both rows from the BIOS 8x8 table.',13,10,'$'
