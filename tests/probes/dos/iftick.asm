; iftick.com -- does INT 10h hand back IF as it found it, and does the BIOS tick run? (s82)
;
; Found by mybench.com: after INT 10h AX=0013h the guest ran with IF=0 (heartbeat EFLAGS
; 0x30002, no CLI anywhere), so the host's gate -- correctly, by its own reading --
; refused every timer interrupt, and 0040:006C never moved. A real BIOS returns through
; IRET, which restores the caller's FLAGS. This asks the question directly, natively (no
; planar mode, no interpreter): FLAGS before and after INT 10h AH=0Fh (get mode, touches
; nothing) and AX=0013h (a mode set), then whether the tick advances within ~3 s of
; spinning. Prints: F0=<flags before> F1=<after 0F> F2=<after 13> T=<ticks seen>
;
; nasm -f bin iftick.asm -o iftick.com
        org     100h
        sti
        pushf
        pop     word [f0]
        mov     ah, 0Fh
        int     10h
        pushf
        pop     word [f1]
        mov     ax, 0013h
        int     10h
        pushf
        pop     word [f2]
        mov     ax, 0003h
        int     10h

        xor     ax, ax
        mov     es, ax
        mov     bx, [es:046Ch]
        mov     si, 0
        mov     di, 600                 ; outer spins
spin:   mov     cx, 0FFFFh
.in:    mov     ax, [es:046Ch]
        cmp     ax, bx
        je      .same
        mov     bx, ax
        inc     si                      ; a tick we SAW change
.same:  loop    .in
        dec     di
        jnz     spin
        mov     [ticks], si

        mov     dx, m0
        call    puts
        mov     ax, [f0]
        call    hex4
        mov     dx, m1
        call    puts
        mov     ax, [f1]
        call    hex4
        mov     dx, m2
        call    puts
        mov     ax, [f2]
        call    hex4
        mov     dx, m3
        call    puts
        mov     ax, [ticks]
        call    hex4
        mov     dx, crlf
        call    puts
        mov     ax, 4C00h
        int     21h

puts:   mov     ah, 09h
        int     21h
        ret
hex4:   mov     cx, 4
.h:     rol     ax, 4
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
        loop    .h
        ret

m0      db 'IFTICK F0=$'
m1      db ' F1=$'
m2      db ' F2=$'
m3      db ' T=$'
crlf    db 13, 10, '$'
f0      dw 0
f1      dw 0
f2      dw 0
ticks   dw 0
