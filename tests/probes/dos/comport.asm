; comport.asm -- does this machine actually have a serial port?  (GH #9)
;
; The question is not "does INT 14h return something" -- it always did. It is
; whether the three layers AGREE and whether the UART behaves like a UART:
;
;   EQUIP=xxxx SER=n     INT 11h's serial-port count (bits 9-11)
;   BDA COM1=03F8        the 0040:0000 port base table
;   SCRATCH 5A A5 OK     base+7 holds a byte -- the classic "is anything there"
;   LOOP TX=5A RX=5A OK  MCR bit 4, the self-test every serial driver runs
;   DTR>DSR OK  RTS>CTS OK  OUT1>RI OK  OUT2>DCD OK
;   BIOS14 TX/RX=5A OK   INT 14h and the registers are ONE part
;   SENT 12 BYTES TO COM1
;
; The loopback block is the important half: it needs no cable and no peer, and
; a port that passes it is a port a driver will believe in. The last line sends
; real bytes with loopback OFF, so the host side leaves an inspectable file.
;
; Assemble: nasm -f bin comport.asm -o comport.com
bits 16
org 0x100

COM1    equ 03F8h
RBR     equ 0
IER     equ 1
IIR     equ 2
LCR     equ 3
MCR     equ 4
LSR     equ 5
MSR     equ 6
SCR     equ 7

start:
        cld

; ---- INT 11h equipment word, and the serial count inside it ---------------
        mov     si, m_equip
        call    puts
        int     11h
        mov     [equip], ax
        call    puthex16
        mov     si, m_ser
        call    puts
        mov     ax, [equip]
        shr     ax, 9
        and     ax, 7
        add     al, '0'
        call    putc
        call    crlf

; ---- the BDA port base table ----------------------------------------------
        mov     si, m_bda
        call    puts
        push    ds
        mov     ax, 40h
        mov     ds, ax
        mov     ax, [0]                 ; 0040:0000 = COM1 base
        pop     ds
        mov     [com1], ax
        call    puthex16
        call    crlf

; ---- is there anything at 3F8 at all?  The scratch register ---------------
        mov     si, m_scr
        call    puts
        mov     dx, COM1 + SCR
        mov     al, 5Ah
        out     dx, al
        in      al, dx
        mov     bl, al
        call    puthex
        mov     al, ' '
        call    putc
        mov     al, 0A5h
        out     dx, al
        in      al, dx
        mov     bh, al
        call    puthex
        cmp     bl, 5Ah
        jne     .scrbad
        cmp     bh, 0A5h
        jne     .scrbad
        call    ok
        jmp     .scrdone
.scrbad:
        call    bad
.scrdone:
        call    crlf

; ---- LOCAL LOOPBACK: transmit must come back ------------------------------
        mov     si, m_loop
        call    puts
        mov     dx, COM1 + MCR
        mov     al, 10h                 ; LOOP
        out     dx, al
        mov     dx, COM1 + LCR
        mov     al, 03h                 ; 8N1, DLAB clear
        out     dx, al
        mov     dx, COM1 + RBR
        mov     al, 5Ah
        out     dx, al
        call    puthex                  ; TX=
        mov     si, m_rx
        call    puts
        mov     dx, COM1 + LSR
        in      al, dx
        test    al, 01h                 ; data ready?
        jz      .loopbad
        mov     dx, COM1 + RBR
        in      al, dx
        call    puthex
        cmp     al, 5Ah
        jne     .loopbad2
        call    ok
        jmp     .loopdone
.loopbad:
        mov     si, m_nodr
        call    puts
        jmp     .loopdone
.loopbad2:
        call    bad
.loopdone:
        call    crlf

; ---- the four modem-control pairings, one at a time -----------------------
        mov     si, m_dtr
        call    puts
        mov     bl, 11h                 ; LOOP|DTR
        mov     bh, 20h                 ; expect DSR
        call    looppair
        mov     si, m_rts
        call    puts
        mov     bl, 12h                 ; LOOP|RTS
        mov     bh, 10h                 ; expect CTS
        call    looppair
        mov     si, m_ou1
        call    puts
        mov     bl, 14h                 ; LOOP|OUT1
        mov     bh, 40h                 ; expect RI
        call    looppair
        mov     si, m_ou2
        call    puts
        mov     bl, 18h                 ; LOOP|OUT2
        mov     bh, 80h                 ; expect DCD
        call    looppair
        call    crlf

; ---- INT 14h and the registers must be the SAME part ----------------------
        mov     si, m_bios
        call    puts
        mov     dx, COM1 + MCR
        mov     al, 10h                 ; still in loopback
        out     dx, al
        mov     ah, 01h                 ; BIOS send
        mov     al, 5Ah
        xor     dx, dx                  ; COM1
        int     14h
        mov     al, 5Ah
        call    puthex
        mov     si, m_rx
        call    puts
        mov     dx, COM1 + LSR          ; ...read back through the REGISTERS
        in      al, dx
        test    al, 01h
        jz      .biosbad
        mov     dx, COM1 + RBR
        in      al, dx
        call    puthex
        cmp     al, 5Ah
        jne     .biosbad
        call    ok
        jmp     .biosdone
.biosbad:
        call    bad
.biosdone:
        call    crlf

; ---- and out of loopback, send something the host can show us -------------
        mov     dx, COM1 + MCR
        xor     al, al                  ; loopback OFF: the pins go outward
        out     dx, al
        mov     si, payload
.send:
        lodsb
        or      al, al
        jz      .sent
        mov     ah, 01h
        xor     dx, dx
        int     14h
        jmp     .send
.sent:
        mov     si, m_sent
        call    puts
        call    crlf

; ---- the parallel port: status, then bytes out via the STROBE edge --------
        mov     si, m_lpt
        call    puts
        mov     dx, 0379h
        in      al, dx
        call    puthex
        test    al, 80h                 ; BUSY is INVERTED: 1 = ready
        jz      .lptbad
        call    ok
        mov     si, lptpay
.lp:
        lodsb
        or      al, al
        jz      .lptend
        mov     dx, 0378h
        out     dx, al                  ; latch the byte
        mov     dx, 037Ah
        xor     al, al
        out     dx, al                  ; STROBE low
        mov     al, 01h
        out     dx, al                  ; ...and the RISING edge prints it
        jmp     .lp
.lptbad:
        call    bad
.lptend:
        call    crlf

        mov     ax, 4C00h
        int     21h

; ---------------------------------------------------------------- helpers

; BL = MCR value to write, BH = the ONE MSR bit that must come back.
; Checked one at a time so a swapped pair cannot hide behind another bit.
looppair:
        push    ax
        push    dx
        mov     dx, COM1 + MCR
        mov     al, bl
        out     dx, al
        mov     dx, COM1 + MSR
        in      al, dx
        and     al, 0F0h
        cmp     al, bh
        jne     .no
        call    ok
        jmp     .done
.no:
        call    puthex
        call    bad
.done:
        pop     dx
        pop     ax
        ret

ok:
        push    si
        mov     si, m_ok
        call    puts
        pop     si
        ret

bad:
        push    si
        mov     si, m_bad
        call    puts
        pop     si
        ret

crlf:
        push    ax
        mov     al, 13
        call    putc
        mov     al, 10
        call    putc
        pop     ax
        ret

putc:                                   ; AL = character
        push    ax
        push    dx
        mov     dl, al
        mov     ah, 02h
        int     21h
        pop     dx
        pop     ax
        ret

puts:                                   ; SI = NUL-terminated string
        push    ax
.loop:
        lodsb
        or      al, al
        jz      .done
        call    putc
        jmp     .loop
.done:
        pop     ax
        ret

puthex:                                 ; AL = byte
        push    ax
        push    cx
        mov     cl, al
        shr     al, 4
        call    .nyb
        mov     al, cl
        and     al, 0Fh
        call    .nyb
        pop     cx
        pop     ax
        ret
.nyb:
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     .out
        add     al, 7
.out:
        call    putc
        ret

puthex16:                               ; AX = word
        push    ax
        mov     al, ah
        call    puthex
        pop     ax
        call    puthex
        ret

; ---------------------------------------------------------------- data

equip   dw 0
com1    dw 0

m_equip db 'EQUIP=', 0
m_ser   db '  SER=', 0
m_bda   db 'BDA COM1=', 0
m_scr   db 'SCRATCH ', 0
m_loop  db 'LOOP TX=', 0
m_rx    db ' RX=', 0
m_nodr  db ' NO DATA-READY -- FAIL', 0
m_dtr   db 'DTR>DSR', 0
m_rts   db '  RTS>CTS', 0
m_ou1   db '  OUT1>RI', 0
m_ou2   db '  OUT2>DCD', 0
m_bios  db 'BIOS14 TX=', 0
m_ok    db ' OK', 0
m_bad   db ' FAIL', 0
m_sent  db 'SENT PAYLOAD TO COM1', 0
payload db 'NTVDMEX COM1', 13, 10, 0
m_lpt   db 'LPT1 STATUS=', 0
lptpay  db 'NTVDMEX LPT1', 13, 10, 0
