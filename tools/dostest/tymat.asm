; tymat.asm -- MEASURE TYPEMATIC REPEAT FROM INSIDE THE GUEST.  (GH #26, keyboard)
;
; WHY THIS EXISTS. Skyroads restarts a level with the up arrow still held, and on real
; DOS the ship accelerates immediately: the game clears its key state, and the KEYBOARD
; re-asserts the key by repeating it. Under NTVDMEX it often did not, and three attempts
; to fix that were made from REMEMBERED hardware behaviour -- "about 500 ms then about
; 10.9 a second". That is an expectation written from memory, which is the one thing this
; project's cardinal rule forbids, and two of the three attempts were wrong.
;
; So measure it instead, on both sides of the comparison:
;     tymat.com under STOCK ntvdm   -> what the oracle actually does on this box
;     tymat.com under NTVDMEX       -> what we do
; The numbers are directly comparable because the probe, the keyboard and the XP
; typematic setting are all identical; only the emulator changes.
;
; DELIBERATELY VIA INT 16h, NOT PORT 0x60. A DOS program under stock ntvdm is not
; guaranteed raw 8042 access, so a port-0x60 probe could measure nothing there and we
; would learn only that the probe does not work. INT 16h is the route every DOS program
; can rely on, it is fed by the same repeats, and it is what the comparison needs.
;
; OUTPUT (ticks are BIOS 18.2 Hz ones, so ~55 ms each):
;     FIRST=xxxx   tick of the first keystroke
;     DELAY=xxxx   ticks from the first keystroke to the SECOND -- the typematic delay
;     COUNT=xxxx   keystrokes in the measuring window after the first
;     TICKS=xxxx   length of that window
; Rate is COUNT/TICKS keystrokes per tick; multiply by 18.2 for per second.
;
; Assemble: nasm -f bin tymat.asm -o tymat.com
bits 16
org 0x100

WIN_TICKS equ 91                ; ~5 s of measuring at 18.2 Hz

start:
    cld
    mov dx, s_banner
    mov ah, 0x09
    int 0x21

    ; ---- drain anything already buffered, so a stray earlier press cannot be
    ;      mistaken for the first keystroke of the test -------------------------
.drain:
    mov ah, 0x01
    int 0x16
    jz .waitfirst
    mov ah, 0x00
    int 0x16
    jmp .drain

    ; ---- block until the first keystroke -----------------------------------
.waitfirst:
    mov ah, 0x00
    int 0x16
    mov ah, 0x00                ; read the tick counter at that moment
    int 0x1A
    mov [t_first], dx
    mov [t_prev], dx

    ; ---- measure ---------------------------------------------------------
.loop:
    mov ah, 0x00                ; now
    int 0x1A
    mov ax, dx
    sub ax, [t_first]
    cmp ax, WIN_TICKS
    jae .done                   ; window elapsed

    mov ah, 0x01                ; any key waiting?
    int 0x16
    jz .loop
    mov ah, 0x00                ; consume it
    int 0x16

    inc word [count]
    cmp word [count], 1         ; the FIRST repeat gives us the delay
    jne .loop
    mov ah, 0x00
    int 0x1A
    sub dx, [t_first]
    mov [delay], dx
    jmp .loop

.done:
    mov ah, 0x00
    int 0x1A
    sub dx, [t_first]
    mov [ticks], dx

    mov dx, s_first
    call puts
    mov ax, [t_first]
    call puthex
    mov dx, s_delay
    call puts
    mov ax, [delay]
    call puthex
    mov dx, s_count
    call puts
    mov ax, [count]
    call puthex
    mov dx, s_ticks
    call puts
    mov ax, [ticks]
    call puthex
    mov dx, s_crlf
    call puts

    mov ax, 0x4C00
    int 0x21

; ---- helpers --------------------------------------------------------------
puts:
    mov ah, 0x09
    int 0x21
    ret

puthex:                         ; AX -> four hex digits on stdout
    push ax
    mov cx, 4
.dig:
    rol ax, 4
    push ax
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .emit
    add al, 7
.emit:
    mov dl, al
    mov ah, 0x02
    int 0x21
    pop ax
    loop .dig
    pop ax
    ret

; ---- data -----------------------------------------------------------------
s_banner db 'tymat: HOLD THE UP ARROW DOWN NOW and keep holding for ~6 seconds.',13,10
         db 'Measuring typematic delay and rate via INT 16h...',13,10,'$'
s_first  db 13,10,'FIRST=$'
s_delay  db '  DELAY=$'
s_count  db '  COUNT=$'
s_ticks  db '  TICKS=$'
s_crlf   db 13,10,'$'

t_first  dw 0
t_prev   dw 0
delay    dw 0
count    dw 0
ticks    dw 0
