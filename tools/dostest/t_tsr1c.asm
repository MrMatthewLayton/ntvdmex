; t_tsr1c.com -- a real-mode TSR that hooks the timer tick, for #143's "a real-mode TSR
; loaded before a DPMI client". Most real TSRs hook INT 1Ch/08h, so a DPMI client that
; starts on top of one must keep reflecting timer ticks down into real-mode code that is
; not ours. The handler counts ticks and chains; the count sits at a fixed place in the
; resident block (offset 0x103) so a memory dump can read it.
;
; nasm -f bin t_tsr1c.asm -o t_tsr1c.com
        org     100h
        jmp     short start
        db      0
count   dw      0
old1c   dd      0

handler:
        inc     word [cs:count]
        jmp     far [cs:old1c]

start:
        mov     ax, 351Ch               ; save the old vector
        int     21h
        mov     [old1c], bx
        mov     [old1c+2], es
        mov     dx, handler
        mov     ax, 251Ch
        int     21h
        mov     dx, msg
        mov     ah, 9
        int     21h
        mov     dx, (start - $$ + 100h + 15) / 16
        mov     ax, 3100h               ; keep: everything below start
        int     21h

msg     db      "t_tsr1c: INT 1Ch hooked, resident", 13, 10, "$"
