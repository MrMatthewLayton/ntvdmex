; p_kbd3.com -- the keyboard BIOS remainders, headless. GH #244, #274.
;
; No key is ever pressed. Each case drives the BIOS through an interface a program
; really uses, and reads back what the BIOS did:
;
;   int15.c0.f1.int4f   INT 15h AH=C0h table, feature byte 1 bit 4: "INT 09h calls
;                       INT 15h AH=4Fh". AX = 0010h set, 0000h clear, FFFFh no table.
;   int09.4f.*          the intercept itself. The probe hooks INT 15h and INJECTS a
;                       scancode with 8042 command D2h ("write keyboard output buffer":
;                       the byte comes out at port 60h as if typed, IRQ1 and all), so the
;                       BIOS's own INT 09h runs. The hook counts its AH=4Fh calls and
;                         1Eh ('a')  -> changes AL to 30h, CF=1: the BIOS must store 'b'
;                         2Eh ('c')  -> CF=0: the BIOS must store NOTHING
;                         1Fh ('s')  -> CF=1, AL untouched: 's' as usual
;                       AX = the key INT 16h AH=10h then reads (0000 = ring empty),
;                       BX = 4Fh calls seen for that byte, CX = the AL the hook was given.
;                       ⚠ A host whose 8042 does not take D2h shows BX=0000 AX=0000 for
;                         all three -- that is "cannot inject", not "no intercept". The
;                         original AT 8042 had no D2h; PS/2-class and AMI KBCs do.
;   kbuf.small.*        0040:0080/0082 are the ring's bounds. With IF clear the probe
;                       shrinks the ring to 0040:001E..0026 (4 slots, 3 usable), pushes
;                       with INT 16h AH=05h, reads with AH=10h, and checks it is FULL at
;                       three and WRAPS at 0026h -- a BIOS with hard-coded 001E/003E takes
;                       a fourth key and wraps at 003Eh. Everything is restored before the
;                       next INT 21h.
;   int05.*             the BIOS print-screen routine behind INT 05h. The probe hooks
;                       INT 17h (counts calls, records the first four bytes, answers 90h
;                       "ready") and issues INT 05h itself:
;                         int05.normal  AX = INT 17h calls, BX = 0050:0000 after,
;                                       CX = 0050:0000 before (informational)
;                         int05.error   the 3rd INT 17h call answers 08h (I/O error):
;                                       AX = calls (the job should stop there), BX = status
;                         int05.busy    0050:0000 = 01h first ("already printing"):
;                                       AX = calls (0), BX = status. SKIPPED (AX=FFFF,
;                                       BX = the byte) unless 0050:0000 held 00/01/FFh
;                                       to begin with -- on NTVDMEX that byte is the
;                                       first byte of the host's own INT 21h stub, and
;                                       writing 01h there breaks every INT 21h.
;                         int05.first   the first four bytes INT 17h received (the IBM
;                                       listing sends LF, CR before the screen).
;                         int05.screen  AX = columns (INT 10h AH=0Fh), BX = 0040:0084,
;                                       so int05.normal's count can be read: the IBM
;                                       routine sends 2 + rows * (columns + 2).
;
; ORACLE-ALSO: pcem
; nasm -f bin p_kbd3.asm -o p_kbd3.com
        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "kbd3"
        cld

        ; ---- A: the C0h table's feature byte 1, bit 4 -------------------------------
        push    es
        mov     ax, 0C000h
        int     15h
        jc      .c0no
        mov     al, [es:bx+5]
        and     ax, 0010h
        jmp     .c0done
.c0no:  mov     ax, 0FFFFh
.c0done:
        pop     es
        call    probe_capture
        EMIT    "int15.c0.f1.int4f", "AX"

        ; ---- B: INT 09h -> INT 15h AH=4Fh, by injection --------------------------------
        mov     ax, 3515h
        int     21h
        mov     [old15], bx
        mov     [old15+2], es
        mov     dx, hook15
        mov     ax, 2515h
        int     21h
        call    flush

        mov     al, 1Eh
        call    inject_and_read
        call    probe_capture
        EMIT    "int09.4f.change", "AX,BX,CX"
        mov     al, 2Eh
        call    inject_and_read
        call    probe_capture
        EMIT    "int09.4f.swallow", "AX,BX,CX"
        mov     al, 1Fh
        call    inject_and_read
        call    probe_capture
        EMIT    "int09.4f.pass", "AX,BX,CX"

        push    ds
        lds     dx, [old15]
        mov     ax, 2515h
        int     21h
        pop     ds

        ; ---- C: the ring's bounds at 0040:0080/0082 ------------------------------------
        call    flush
        cli
        push    es
        mov     ax, 40h
        mov     es, ax
        mov     ax, [es:80h]
        mov     [s80], ax
        mov     ax, [es:82h]
        mov     [s82], ax
        mov     ax, [es:1Ah]
        mov     [s1a], ax
        mov     ax, [es:1Ch]
        mov     [s1c], ax
        mov     word [es:80h], 001Eh
        mov     word [es:82h], 0026h
        mov     word [es:1Ah], 001Eh
        mov     word [es:1Ch], 001Eh
        mov     di, kres
        mov     cx, 1E61h
        call    push16
        mov     cx, 3062h
        call    push16
        mov     cx, 2E63h
        call    push16
        mov     ax, [es:1Ch]
        mov     [ktail1], ax
        mov     cx, 2064h                       ; the 4th: full on a 4-slot ring
        call    push16
        mov     si, kk
        call    read16
        call    read16
        mov     cx, 2064h                       ; now it fits, and the tail wraps
        call    push16
        mov     ax, [es:1Ch]
        mov     [ktail2], ax
        mov     cx, 1F73h                       ; ...into 001Eh
        call    push16
        mov     ax, [es:1Eh]
        mov     [kslot], ax
        call    read16
        call    read16
        call    read16
        mov     ax, [es:1Ah]
        mov     [khead], ax
        mov     ax, [s80]
        mov     [es:80h], ax
        mov     ax, [s82]
        mov     [es:82h], ax
        mov     ax, [s1a]
        mov     [es:1Ah], ax
        mov     ax, [s1c]
        mov     [es:1Ch], ax
        pop     es
        sti
        EMIT_BUF "kbuf.small.push", kres, 6
        mov     ax, [ktail1]
        mov     bx, [ktail2]
        mov     cx, [kslot]
        mov     dx, [khead]
        call    probe_capture
        EMIT    "kbuf.small.ptrs", "AX,BX,CX,DX"
        EMIT_BUF "kbuf.small.keys", kk, 10

        ; ---- D: INT 05h, the print-screen routine --------------------------------------
        mov     ah, 0Fh
        int     10h
        mov     al, ah
        xor     ah, ah
        push    es
        mov     bx, 40h
        mov     es, bx
        mov     bl, [es:84h]
        pop     es
        xor     bh, bh
        call    probe_capture
        EMIT    "int05.screen", "AX,BX"

        mov     ax, 3517h
        int     21h
        mov     [old17], bx
        mov     [old17+2], es
        mov     dx, hook17
        mov     ax, 2517h
        int     21h

        mov     word [fail17], 0
        call    prtsc
        call    probe_capture
        EMIT    "int05.normal", "AX,BX"
        EMIT_BUF "int05.first", b17, 4

        mov     word [fail17], 3
        call    prtsc
        call    probe_capture
        EMIT    "int05.error", "AX,BX"

        mov     word [fail17], 0
        push    es
        mov     ax, 50h
        mov     es, ax
        mov     al, [es:0]
        pop     es
        cmp     al, 00h
        je      .busyok
        cmp     al, 01h
        je      .busyok
        cmp     al, 0FFh
        je      .busyok
        xor     ah, ah
        mov     bx, ax
        mov     ax, 0FFFFh
        jmp     .busydone
.busyok:
        mov     byte [busy], 1
        call    prtsc
.busydone:
        call    probe_capture
        EMIT    "int05.busy", "AX,BX"

        push    ds
        lds     dx, [old17]
        mov     ax, 2517h
        int     21h
        pop     ds

        PROBE_END

; ---- inject AL through 8042 D2h, wait for the BIOS to take it, read the ring --------
; -> AX = key (0 if none), BX = AH=4Fh calls it caused, CX = the AL the hook saw
inject_and_read:
        push    ax
        mov     bx, [n4f]
        mov     [n4f0], bx
        mov     byte [al4f], 0
        cli
        call    kbc_wait
        mov     al, 0D2h
        out     64h, al
        call    kbc_wait
        pop     ax
        out     60h, al
        sti
        push    es                      ; wait up to ~1 s (18 ticks) for the hook
        mov     ax, 40h
        mov     es, ax
        mov     dx, [es:6Ch]
.wait:  mov     bx, [n4f]
        cmp     bx, [n4f0]
        jne     .seen
        mov     ax, [es:6Ch]
        sub     ax, dx
        cmp     ax, 18
        jb      .wait
.seen:  pop     es
        mov     cx, 2000h               ; let the BIOS finish its store
.settle:
        loop    .settle
        mov     ah, 11h
        int     16h
        jz      .none
        mov     ah, 10h
        int     16h
        jmp     .got
.none:  xor     ax, ax
.got:   mov     bx, [n4f]
        sub     bx, [n4f0]
        xor     ch, ch
        mov     cl, [al4f]
        ret

kbc_wait:                               ; wait for IBF (64h bit 1) to clear
        push    cx
        mov     cx, 0FFFFh
.w:     in      al, 64h
        test    al, 2
        loopnz  .w
        pop     cx
        ret

flush:  mov     ah, 11h                 ; empty the BIOS ring
        int     16h
        jz      .done
        mov     ah, 10h
        int     16h
        jmp     flush
.done:  ret

push16: mov     ah, 05h                 ; CX -> ring; AL result -> [di++]
        int     16h
        mov     [di], al
        inc     di
        ret

read16: mov     ah, 11h                 ; ring -> [si] (0000 if empty), si += 2
        int     16h
        jz      .e
        mov     ah, 10h
        int     16h
        jmp     .s
.e:     xor     ax, ax
.s:     mov     [si], ax
        add     si, 2
        ret

; ---- one INT 05h; [busy]=1 sets 0050:0000 = 01h first. -> AX = INT 17h calls,
;      BX = 0050:0000 after. 0050:0000 is put back as it was found.
prtsc:  push    es
        mov     ax, 50h
        mov     es, ax
        mov     al, [es:0]
        mov     [sv500], al
        mov     word [n17], 0
        mov     dword [b17], 0
        cmp     byte [busy], 0
        je      .go
        mov     byte [es:0], 01h
.go:    int     05h
        mov     bl, [es:0]
        xor     bh, bh
        mov     al, [sv500]
        mov     [es:0], al
        mov     byte [busy], 0
        pop     es
        mov     ax, [n17]
        ret

; ---- the hooks -----------------------------------------------------------------------
hook15: cmp     ah, 4Fh
        jne     .chain
        push    bp
        mov     bp, sp
        inc     word [cs:n4f]
        mov     [cs:al4f], al
        cmp     al, 1Eh
        jne     .n1
        mov     al, 30h                 ; 'a' -> 'b'
        jmp     .proc
.n1:    cmp     al, 2Eh
        jne     .proc
        and     word [bp+6], 0FFFEh     ; CF=0: swallow
        pop     bp
        iret
.proc:  or      word [bp+6], 0001h      ; CF=1: process AL
        pop     bp
        iret
.chain: jmp     far [cs:old15]

hook17: push    bx
        inc     word [cs:n17]
        mov     bx, [cs:n17]
        cmp     bx, 4
        ja      .nr
        mov     [cs:b17+bx-1], al
.nr:    cmp     bx, [cs:fail17]
        pop     bx
        jne     .ok
        mov     ah, 08h                 ; I/O error
        iret
.ok:    mov     ah, 90h                 ; selected, not busy
        iret

old15   dd      0
old17   dd      0
n4f     dw      0
n4f0    dw      0
al4f    db      0
n17     dw      0
fail17  dw      0
b17     db      0, 0, 0, 0
busy    db      0
sv500     db      0
s80     dw      0
s82     dw      0
s1a     dw      0
s1c     dw      0
ktail1  dw      0
ktail2  dw      0
kslot   dw      0
khead   dw      0
kres    times 6 db 0
kk      times 10 db 0
