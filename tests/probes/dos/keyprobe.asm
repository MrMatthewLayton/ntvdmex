; keyprobe.asm -- PROMPTED keystroke ground-truth capture.
;
; keylog.asm logs whatever arrives, unlabelled, which tells you what the guest saw but not
; what it was SUPPOSED to see. This asks for one named key at a time --
;
;     Press UP arrow    : RAW=E0 48 E0 C8  B16=4800  SHIFT=00  BDA=1E/1E
;
; -- so every captured byte is tied to a key a human actually pressed. That labelling is the
; whole point: an arrow is TWO scancodes (E0 prefix + code) and a game reads it by one of
; three unrelated routes, so "arrows don't work in the Skyroads menu" has several possible
; causes that look identical from the outside. Each field separates one of them:
;
;   RAW   -- bytes our own INT 09h handler read from port 60h. This is the route an action
;            game takes (its own ISR + a held-key table). If RAW is empty, IRQ 1 never
;            reached the guest. If it shows 48 without a leading E0, the prefix was dropped.
;            If the break code (E0 C8) never lands, a game tracking held keys sticks ON.
;   B16   -- what INT 16h returned (AH=scancode, AL=ascii). The BIOS route. An arrow must be
;            AH=48h AL=00h. Empty here + present in RAW = a game polling INT 16h sees nothing.
;   SHIFT -- INT 16h AH=02h shift flags.
;   BDA   -- the BIOS keyboard buffer head/tail at 0040:001A/001C, for the third route: apps
;            that read the ring in low memory directly rather than calling the BIOS at all.
;
; Everything is printed through INT 21h, so it lands in the host log under "==> DOS OUTPUT"
; as well as on screen.
;
; Each prompt waits up to ~15 s, and ends ~0.5 s after the last byte of that key arrives (so
; a make/break pair is captured as one line). Press the keys in the order asked; ESC is last
; and also ends the run.
;
; Assemble: nasm -f bin keyprobe.asm -o keyprobe.com
bits 16
org 0x100

BEGIN_TICKS  equ 3276           ; ~3 min to walk to the box before it gives up
PROMPT_TICKS equ 273            ; ~15 s per key at the BIOS 18.2 Hz tick
SETTLE_TICKS equ 27             ; ~1.5 s of quiet = that key is done (a human holds a key
                                ; longer than half a second, and cutting the capture early
                                ; strands the BREAK code in the NEXT prompt -- which is how
                                ; the first real run mislabelled every line by one)
QUIET_TICKS  equ 18             ; ~1 s of silence required before a prompt starts capturing
QD_MAX_TICKS equ 55             ; ...but never wait more than ~3 s for that silence
RBUF_SIZE    equ 64             ; raw bytes captured per prompt (make+break is 4)
RBUF_MASK    equ RBUF_SIZE-1
B16_MAX      equ 4              ; INT 16h codes recorded per prompt

start:
    cld
    mov dx, s_banner
    mov ah, 0x09
    int 0x21

    ; --- install our INT 09h handler: the raw scancode route ---
    xor ax, ax
    mov es, ax
    cli
    mov ax, [es:0x09*4]
    mov [old09], ax
    mov ax, [es:0x09*4+2]
    mov [old09+2], ax
    mov word [es:0x09*4], isr09
    mov [es:0x09*4+2], cs
    sti

    ; --- wait for the human to actually be at the keyboard ---
    ; The previous run of this rig started the instant it was triggered and expired
    ; unattended, capturing nothing. So the prompts do not begin until a key says
    ; someone is there. Up to ~3 min, then it gives up rather than wedging the watcher.
    mov dx, s_begin
    mov ah, 0x09
    int 0x21
    call gettick
    mov [t_start], dx
    mov al, [rhead]
    mov [seen], al
.wait_begin:
    mov al, [rhead]             ; any raw byte at all
    cmp al, [seen]
    jne .began
    mov ah, 0x01                ; ...or anything on the BIOS route
    int 0x16
    jnz .began
    call gettick
    mov ax, dx
    sub ax, [t_start]
    cmp ax, BEGIN_TICKS
    jb .wait_begin
.began:
    mov dx, s_crlf
    mov ah, 0x09
    int 0x21

    mov si, prompts
.next:
    mov dx, [si]
    or dx, dx
    jz .finish
    add si, 2
    push si
    mov ah, 0x09                ; "Press UP arrow    : "
    int 0x21
    call capture
    call report
    pop si
    jmp .next

.finish:
    ; --- put the BIOS handler back before leaving ---
    xor ax, ax
    mov es, ax
    cli
    mov ax, [old09]
    mov [es:0x09*4], ax
    mov ax, [old09+2]
    mov [es:0x09*4+2], ax
    sti
    mov dx, s_done
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21

; ---------------------------------------------------------------------------
; capture -- collect one key press: raw bytes (via the ISR) + INT 16h codes.
; Ends SETTLE_TICKS after the last thing arrived, or PROMPT_TICKS with nothing.
; ---------------------------------------------------------------------------
capture:
    call quiet_drain            ; the previous key must be fully over before we arm
    mov byte [b16n], 0
    mov byte [got], 0
    call gettick
    mov [t_start], dx
    mov [t_last], dx
.wait:
    mov al, [rhead]             ; new raw byte?
    cmp al, [seen]
    je .no_raw
    mov [seen], al
    mov byte [got], 1
    call gettick
    mov [t_last], dx
.no_raw:
    mov ah, 0x01                ; new BIOS key?
    int 0x16
    jz .no_bios
    mov ah, 0x00
    int 0x16
    call push_b16
    mov byte [got], 1
    call gettick
    mov [t_last], dx
.no_bios:
    call gettick                ; dx = tick now
    cmp byte [got], 0
    je .timeout
    mov ax, dx                  ; quiet long enough -> this key is complete
    sub ax, [t_last]
    cmp ax, SETTLE_TICKS
    jae .ret
.timeout:
    mov ax, dx
    sub ax, [t_start]
    cmp ax, PROMPT_TICKS
    jae .ret
    jmp .wait
.ret:
    ret

; ---------------------------------------------------------------------------
; quiet_drain -- throw away raw bytes and BIOS keys until the keyboard has been
; silent for QUIET_TICKS. Everything a prompt captures then belongs to the key
; that prompt asked for, and nothing else.
; ---------------------------------------------------------------------------
quiet_drain:
    mov al, [rhead]
    mov [seen], al
    call gettick
    mov [t_last], dx
    mov [t_qd], dx              ; hard bound: a keyboard that never goes quiet (a stuck key,
                                ; or a host-side key injector running) would otherwise spin
                                ; here forever and the program would look wedged
.qd:
    call gettick
    mov ax, dx
    sub ax, [t_qd]
    cmp ax, QD_MAX_TICKS
    jae .qd_done
    mov al, [rhead]
    cmp al, [seen]
    je .qd_noraw
    mov [seen], al              ; still arriving: restart the silence timer
    call gettick
    mov [t_last], dx
.qd_noraw:
    mov ah, 0x01
    int 0x16
    jz .qd_no16
    mov ah, 0x00
    int 0x16
    call gettick
    mov [t_last], dx
.qd_no16:
    call gettick
    mov ax, dx
    sub ax, [t_last]
    cmp ax, QUIET_TICKS
    jb .qd
.qd_done:
    mov al, [rhead]             ; discard everything seen so far
    mov [rtail], al
    mov [seen], al
    ret

; record AX (INT 16h result) unless the list is full
push_b16:
    push bx
    mov bl, [b16n]
    cmp bl, B16_MAX
    jae .full
    xor bh, bh
    shl bx, 1
    mov [b16buf + bx], ax
    inc byte [b16n]
.full:
    pop bx
    ret

; ---------------------------------------------------------------------------
; report -- print one result line for the key just captured.
; ---------------------------------------------------------------------------
report:
    cmp byte [got], 0
    jne .have
    mov dx, s_none              ; nothing at all arrived within the timeout
    mov ah, 0x09
    int 0x21
    jmp .bda
.have:
    mov dx, s_raw
    mov ah, 0x09
    int 0x21
    mov bl, [rtail]
.rawloop:
    cmp bl, [rhead]
    je .rawend
    xor bh, bh
    mov al, [rbuf + bx]
    push bx
    call puthex8
    mov dl, ' '
    mov ah, 0x02
    int 0x21
    pop bx
    inc bl
    and bl, RBUF_MASK
    jmp .rawloop
.rawend:
    cmp byte [b16n], 0
    jne .b16
    mov dx, s_rawonly           ; raw bytes but the BIOS route stayed empty
    mov ah, 0x09
    int 0x21
    jmp .shift
.b16:
    mov dx, s_b16
    mov ah, 0x09
    int 0x21
    xor bx, bx
.b16loop:
    cmp bl, [b16n]
    jae .shift
    push bx
    shl bx, 1
    mov ax, [b16buf + bx]
    call puthex16
    mov dl, ' '
    mov ah, 0x02
    int 0x21
    pop bx
    inc bl
    jmp .b16loop
.shift:
    mov dx, s_shift
    mov ah, 0x09
    int 0x21
    mov ah, 0x02                ; INT 16h shift flags
    int 0x16
    call puthex8
.bda:
    mov dx, s_bda               ; the ring apps read directly at 0040:001A/001C
    mov ah, 0x09
    int 0x21
    push es
    mov ax, 0x40
    mov es, ax
    mov ax, [es:0x1A]
    pop es
    call puthex16
    mov dl, '/'
    mov ah, 0x02
    int 0x21
    push es
    mov ax, 0x40
    mov es, ax
    mov ax, [es:0x1C]
    pop es
    call puthex16
    mov dx, s_crlf
    mov ah, 0x09
    int 0x21
    ret

; ---------------------------------------------------------------------------
; INT 09h -- read the scancode ourselves, exactly as a game's own handler does,
; and EOI the PIC (the host only auto-EOIs while its own stub owns the vector).
; ---------------------------------------------------------------------------
isr09:
    push ax
    push bx
    push ds
    push cs
    pop ds
    in al, 0x60
    mov bl, [rhead]
    xor bh, bh
    mov [rbuf + bx], al
    inc bl
    and bl, RBUF_MASK
    mov [rhead], bl
    mov al, 0x20
    out 0x20, al
    pop ds
    pop bx
    pop ax
    iret

; --- helpers ----------------------------------------------------------------
gettick:                        ; -> DX = low word of the BIOS tick count
    push ax
    push cx
    mov ah, 0x00
    int 0x1A
    pop cx
    pop ax
    ret

puthex16:                       ; AX as 4 hex digits
    push ax
    mov al, ah
    call puthex8
    pop ax
    ; fall through
puthex8:                        ; AL as 2 hex digits
    push ax
    push dx
    push ax
    shr al, 4
    call putnib
    pop ax
    and al, 0x0F
    call putnib
    pop dx
    pop ax
    ret

putnib:
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .out
    add al, 7
.out:
    mov dl, al
    mov ah, 0x02
    int 0x21
    ret

; --- data -------------------------------------------------------------------
old09    dd 0
rhead    db 0
rtail    db 0
seen     db 0
got      db 0
b16n     db 0
t_start  dw 0
t_last   dw 0
t_qd     dw 0
b16buf   times B16_MAX dw 0
rbuf     times RBUF_SIZE db 0

prompts  dw p_up, p_down, p_left, p_right, p_enter, p_space, p_esc, 0

; The expected scancode is printed with each prompt, so a shifted or wrong capture is
; obvious in the log without cross-referencing a scancode table.
p_up     db 13,10,'Press UP arrow    (expect E0 48): $'
p_down   db 'Press DOWN arrow  (expect E0 50): $'
p_left   db 'Press LEFT arrow  (expect E0 4B): $'
p_right  db 'Press RIGHT arrow (expect E0 4D): $'
p_enter  db 'Press ENTER       (expect    1C): $'
p_space  db 'Press SPACE       (expect    39): $'
p_esc    db 'Press ESC         (expect    01): $'

s_banner db 'keyprobe: press the key each line asks for. ~15s each, then it moves on.',13,10
         db 'RAW=port 60h bytes (game route)  B16=INT 16h AX  SHIFT=AH02  BDA=head/tail',13,10,'$'
s_raw    db 'RAW=$'
s_b16    db ' B16=$'
s_rawonly db ' B16=(none)$'
s_shift  db ' SHIFT=$'
s_bda    db ' BDA=$'
s_none   db '(nothing arrived)$'
s_begin  db 13,10,'Press any key when you are at the keyboard to begin...$'
s_crlf   db 13,10,'$'
s_done   db 13,10,'keyprobe: done.',13,10,'$'
