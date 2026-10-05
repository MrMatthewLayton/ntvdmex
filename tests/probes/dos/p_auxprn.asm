; p_auxprn.com -- DOS's AUX and PRN devices, as the BIOS underneath them sees them.
; GH #251.
;
; On MS-DOS, AUX and PRN are device drivers in IO.SYS that talk to the BIOS: PRN
; prints through INT 17h, AUX sends and receives through INT 14h. That is the
; whole contract, and it is what made period printer redirectors and serial
; TSRs work: they hook INT 17h / INT 14h, and everything DOS prints passes them.
; We answered AH=04h/05h "accepted and discarded" and AH=03h with ^Z -- the byte
; went nowhere, and no hook ever saw it.
;
; ★ THE PROBE HOOKS INT 14h AND INT 17h ITSELF, so no printer, no serial line
;   and no BIOS is involved on any host: each hook RECORDS the AX/DX it was
;   called with and answers "ready" (AUX receive hands out bytes from a script).
;   What is compared is the call sequence DOS makes -- an implementation
;   contract, not a fact about the machine. The logs are dumped as BUF rows:
;   up to 8 calls, 4 bytes each (AX lo/hi, DX lo/hi), then the count byte.
;
; Cases: AH=05h, AH=04h, AH=03h, AH=40h on handles 4 and 3, AH=3Fh on handle 3,
; and IOCTL 4400h on handles 3 and 4 (the device-information word).
;
; ORACLE-ALSO: pcem
; nasm -f bin p_auxprn.asm -o p_auxprn.com

        org     100h
        jmp     start
%include "probe.inc"

%macro CLRLOG 0
        call    clrlog
%endmacro

start:
        PROBE_BEGIN "auxprn"

        ; ---- hook INT 14h and INT 17h
        mov     ax, 3514h
        int     21h
        mov     [old14], bx
        mov     [old14+2], es
        mov     ax, 3517h
        int     21h
        mov     [old17], bx
        mov     [old17+2], es
        push    ds
        pop     es
        mov     ax, 2514h
        mov     dx, hook14
        int     21h
        mov     ax, 2517h
        mov     dx, hook17
        int     21h

        ; ---- AH=05h: one byte to the printer
        CLRLOG
        mov     ax, 05A5h
        mov     bx, 0B1B1h
        mov     cx, 0C1C1h
        mov     dx, 0D150h                      ; DL = 'P'
        int     21h
        call    probe_capture
        EMIT    "int21.05.prn", "AX,BX,CX,DX"
        EMIT_BUF "int21.05.int17", log17, 33
        EMIT_BUF "int21.05.int14", log14, 33

        ; ---- AH=04h: one byte to AUX
        CLRLOG
        mov     ax, 04A5h
        mov     bx, 0B1B1h
        mov     cx, 0C1C1h
        mov     dx, 0D141h                      ; DL = 'A'
        int     21h
        call    probe_capture
        EMIT    "int21.04.aux", "AX,BX,CX,DX"
        EMIT_BUF "int21.04.int14", log14, 33
        EMIT_BUF "int21.04.int17", log17, 33

        ; ---- AH=03h: one byte from AUX (the hook hands out 'Q')
        CLRLOG
        mov     ax, 03A5h
        POISON
        int     21h
        call    probe_capture
        EMIT    "int21.03.aux", "AX,BX,CX,DX"
        EMIT_BUF "int21.03.int14", log14, 33

        ; ---- AH=40h on handle 4 (PRN), two bytes
        CLRLOG
        mov     ah, 40h
        mov     bx, 4
        mov     cx, 2
        mov     dx, txt_pq
        int     21h
        call    probe_capture
        EMIT    "int21.40.h4", "AX,CF"
        EMIT_BUF "int21.40.h4.int17", log17, 33

        ; ---- AH=40h on handle 3 (AUX), two bytes
        CLRLOG
        mov     ah, 40h
        mov     bx, 3
        mov     cx, 2
        mov     dx, txt_ab
        int     21h
        call    probe_capture
        EMIT    "int21.40.h3", "AX,CF"
        EMIT_BUF "int21.40.h3.int14", log14, 33

        ; ---- AH=3Fh on handle 3 (AUX), up to 6 bytes. The script continues
        ; 'r', CR, 'w', 'x', ... -- so whether DOS stops at the CR shows here.
        CLRLOG
        push    ds
        pop     es
        mov     di, rbuf
        mov     cx, 8
        mov     al, 0EEh
        rep     stosb
        mov     ah, 3Fh
        mov     bx, 3
        mov     cx, 6
        mov     dx, rbuf
        int     21h
        call    probe_capture
        EMIT    "int21.3F.h3", "AX,CF"
        EMIT_BUF "int21.3F.h3.data", rbuf, 8
        EMIT_BUF "int21.3F.h3.int14", log14, 33

        ; ---- IOCTL 4400h: device information word for AUX and PRN
        CLRLOG
        mov     ax, 4400h
        mov     bx, 3
        mov     cx, 0C1C1h
        mov     dx, 0D1D1h
        int     21h
        call    probe_capture
        EMIT    "int21.4400.h3", "AX,DX,CF"

        mov     ax, 4400h
        mov     bx, 4
        mov     cx, 0C1C1h
        mov     dx, 0D1D1h
        int     21h
        call    probe_capture
        EMIT    "int21.4400.h4", "AX,DX,CF"

        ; ---- unhook
        push    ds
        lds     dx, [old14]
        mov     ax, 2514h
        int     21h
        pop     ds
        push    ds
        lds     dx, [old17]
        mov     ax, 2517h
        int     21h
        pop     ds

        PROBE_END

; ---------------------------------------------------------------- hooks
; Record AX and DX (as the caller passed them), then answer.
hook17:
        push    bx
        mov     bx, [cs:n17]
        cmp     bx, 32
        jae     .full
        mov     [cs:log17+bx], ax
        mov     [cs:log17+bx+2], dx
        add     word [cs:n17], 4
        inc     byte [cs:log17+32]
.full:  pop     bx
        mov     ah, 90h                         ; not busy, selected
        iret

hook14:
        push    bx
        mov     bx, [cs:n14]
        cmp     bx, 32
        jae     .full
        mov     [cs:log14+bx], ax
        mov     [cs:log14+bx+2], dx
        add     word [cs:n14], 4
        inc     byte [cs:log14+32]
.full:  pop     bx
        cmp     ah, 1
        je      .send
        cmp     ah, 2
        je      .recv
        cmp     ah, 3
        je      .stat
        mov     ax, 6030h                       ; init: THRE+TSRE, CTS+DSR
        iret
.send:  mov     ah, 60h                         ; sent, no error; AL kept
        iret
.recv:  push    bx
        mov     bx, [cs:rxi]
        mov     al, [cs:rxtab+bx]
        inc     word [cs:rxi]
        and     word [cs:rxi], 15
        pop     bx
        mov     ah, 00h                         ; no error
        iret
.stat:  mov     ax, 6130h                       ; data ready + THRE/TSRE; CTS+DSR
        iret

clrlog:
        push    ax
        push    cx
        push    di
        push    es
        push    ds
        pop     es
        xor     ax, ax
        mov     di, log17
        mov     cx, 33
        rep     stosb
        mov     di, log14
        mov     cx, 33
        rep     stosb
        mov     [n17], ax
        mov     [n14], ax
        pop     es
        pop     di
        pop     cx
        pop     ax
        ret

old14   dd      0
old17   dd      0
n17     dw      0
n14     dw      0
log17   times 33 db 0
log14   times 33 db 0
rxi     dw      0
rxtab   db      'Q', 'r', 0Dh, 'w', 'x', 'y', 'z', '1', '2', '3', '4', '5', '6', '7', '8', '9'
txt_pq  db      'PQ'
txt_ab  db      'AB'
rbuf    times 8 db 0
