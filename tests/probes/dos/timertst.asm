; timertst.asm -- prove the PIT timer interrupt actually fires in the guest.
;
; Hooks INT 1Ch (the BIOS user-timer hook). INT 1Ch is reached ONLY via the full
; chain we just built: PIT raises IRQ0 -> host injects INT 08h -> our INT 08h stub
; runs `CD 1C` -> this handler. So if the dots stream, IRQ0 delivery + INT 08h +
; the INT 1Ch chain all work. The handler bumps a counter; the main loop prints a
; '.' each time the counter changes (~18.2 times/sec) and exits on any key.
;
; Assemble: nasm -f bin timertst.asm -o timertst.com
bits 16
org 0x100

start:
    mov ah, 0x09                ; banner
    mov dx, msg
    int 0x21

    cli                         ; hook INT 1Ch (offset in CS, segment = CS)
    xor ax, ax
    mov es, ax
    mov word [es:0x1C*4], handler
    mov [es:0x1C*4 + 2], cs
    sti

.loop:
    mov ax, [ticks]
    cmp ax, [last]
    je .key
    mov [last], ax
    mov dl, '.'                 ; one dot per timer tick
    mov ah, 0x02
    int 0x21
.key:
    mov ah, 0x01                ; INT 16h: any key -> exit
    int 0x16
    jz .loop

    mov ax, 0x4C00
    int 0x21

handler:                        ; INT 1Ch -- fires ~18.2x/sec if the timer works
    inc word [cs:ticks]
    iret

msg:   db "PIT timer test: dots = timer ticks. Press a key to quit.", 13, 10, "$"
ticks: dw 0
last:  dw 0
