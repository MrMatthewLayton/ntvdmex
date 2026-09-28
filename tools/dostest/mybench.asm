; mybench.com -- how fast is the mode-Y interpreter on a CPU-BOUND workload? (#183)
;
; Wolf3D's "~80% of a core" turned out to be mostly its own wait-for-retrace spin
; (every 3DAh read asks QueryPerformanceCounter, a system call on XP), so it cannot
; show an interpreter speed-up. This can: it puts the VGA in mode Y with a
; MULTI-PLANE map mask (0Fh) -- the condition that runs the guest in the host's
; interpreter -- then executes a fixed amount of Wolf3D-shaped work (load, add,
; store to A0000, pointer arithmetic, loop) and times it with the BIOS tick
; (18.2 Hz). Prints the elapsed ticks in hex; fewer = faster. ~48M instructions.
;
; nasm -f bin mybench.asm -o mybench.com
        org     100h

        mov     ax, 0013h
        int     10h
        pushf                           ; FLAGS after the mode set (s82: who clears IF?)
        pop     word [fl1]
        mov     dx, 03C4h               ; unchain: SR4 chain-4 off, odd/even off
        mov     al, 04h
        out     dx, al
        inc     dx
        in      al, dx
        and     al, 0F7h
        or      al, 04h
        out     dx, al
        dec     dx
%ifndef MASK
%define MASK 0Fh
%endif
        mov     ax, (MASK << 8) | 02h   ; SR2 map mask: 0Fh = all four planes (interpreted);
                                        ; nasm -DMASK=01h gives the NATIVE control run
        out     dx, ax
        pushf                           ; ...and after the map-mask write
        pop     word [fl2]

        xor     ax, ax
        mov     es, ax
        mov     ax, [es:046Ch]          ; start tick
        mov     [t0], ax

        mov     ax, 0A000h
        mov     es, ax
        xor     si, si
        xor     di, di
        mov     bl, 3
        mov     bp, 80                  ; outer loops
outer:  mov     cx, 60000
inner:  mov     al, [si]
        add     al, bl
        mov     [es:di], al
        inc     di
        and     di, 3FFFh
        add     si, 3
        and     si, 7FFFh
        dec     cx
        jnz     inner
        dec     bp
        jnz     outer

        xor     ax, ax
        mov     es, ax
        mov     ax, [es:046Ch]
        sub     ax, [t0]
        mov     [elapsed], ax

        mov     dx, 03C4h               ; map mask back to one plane before text mode
        mov     ax, 0102h
        out     dx, ax
        mov     ax, 0003h
        int     10h

        mov     dx, msg
        mov     ah, 09h
        int     21h
        mov     ax, [elapsed]
        call    hex4
        jmp     after
hex4:   mov     cx, 4
hex:    rol     ax, 4
        push    ax
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     .d
        add     al, 7
.d:     mov     dl, al
        mov     ah, 02h
        int     21h
        pop     ax
        loop    hex
        ret
after:  mov     dx, mfl
        mov     ah, 09h
        int     21h
        mov     ax, [fl1]
        call    hex4
        mov     dl, ' '
        mov     ah, 02h
        int     21h
        mov     ax, [fl2]
        call    hex4
        mov     dx, crlf
        mov     ah, 09h
        int     21h
        mov     ax, 4C00h
        int     21h

msg     db 'MYBENCH ticks=0x$'
mfl     db ' flags=$'
fl1     dw 0
fl2     dw 0
crlf    db 13, 10, '$'
t0      dw 0
elapsed dw 0
