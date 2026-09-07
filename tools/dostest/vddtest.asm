; vddtest.asm -- does a THIRD-PARTY VDD answer?  (GH #11)
;
; Drives the SDK sample device (sdk/sample/portecho.c) exactly as a real DOS
; driver would: detect it by its id register, write the latch, read it back
; through the one's-complement register, and check the write counter moved.
;
;   ID=4E OK          the card is present (0xFF here means it is not on the bus)
;   ECHO A5->5A OK    the write reached the DLL and was transformed
;   WRITES=01 OK      ...and the device counted it
;   ABI=01 OK         the version the host handed the driver
;
; A one's-complement echo is the point: reading back what you wrote proves
; nothing (an open bus floats high, and a latch you never reached would read
; 0xFF), whereas ~A5 = 5A can only come from code that ran.
;
; Assemble: nasm -f bin vddtest.asm -o vddtest.com
bits 16
org 0x100

BASE equ 0x2E0

start:
        cld
; ---- identification -------------------------------------------------------
        mov     si, m_id
        call    puts
        mov     dx, BASE
        in      al, dx
        call    puthex
        cmp     al, 0x4E
        jne     .bad_id
        call    ok
        jmp     .echo
.bad_id:
        call    bad
        call    crlf
        jmp     .done                   ; no card -- the rest would be noise
.echo:
        call    crlf

; ---- write the latch, read the complement ---------------------------------
        mov     si, m_echo
        call    puts
        mov     dx, BASE
        mov     al, 0xA5
        out     dx, al
        mov     al, 0xA5
        call    puthex
        mov     si, m_arrow
        call    puts
        mov     dx, BASE + 1
        in      al, dx
        call    puthex
        cmp     al, 0x5A                ; ~0xA5
        jne     .bad_echo
        call    ok
        jmp     .cnt
.bad_echo:
        call    bad
.cnt:
        call    crlf

; ---- the device counted the write -----------------------------------------
        mov     si, m_wr
        call    puts
        mov     dx, BASE + 2
        in      al, dx
        call    puthex
        cmp     al, 1
        jne     .bad_wr
        call    ok
        jmp     .abi
.bad_wr:
        call    bad
.abi:
        call    crlf

; ---- the ABI version the host handed it ------------------------------------
        mov     si, m_abi
        call    puts
        mov     dx, BASE + 3
        in      al, dx
        call    puthex
        cmp     al, 1
        jne     .bad_abi
        call    ok
        jmp     .done
.bad_abi:
        call    bad
.done:
        call    crlf
        mov     ax, 4C00h
        int     21h

; ---------------------------------------------------------------- helpers
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
putc:
        push    ax
        push    dx
        mov     dl, al
        mov     ah, 02h
        int     21h
        pop     dx
        pop     ax
        ret
puts:
        push    ax
.l:     lodsb
        or      al, al
        jz      .d
        call    putc
        jmp     .l
.d:     pop     ax
        ret
puthex:
        push    ax
        push    cx
        mov     cl, al
        shr     al, 4
        call    .n
        mov     al, cl
        and     al, 0Fh
        call    .n
        pop     cx
        pop     ax
        ret
.n:     and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     .o
        add     al, 7
.o:     call    putc
        ret

m_id    db 'ID=', 0
m_echo  db 'ECHO ', 0
m_arrow db '->', 0
m_wr    db 'WRITES=', 0
m_abi   db 'ABI=', 0
m_ok    db ' OK', 0
m_bad   db ' FAIL', 0
