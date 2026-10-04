; p_netb.com -- NetBIOS through INT 5Ch, the network interface a DOS box on NT has.  GH #8.
;
; Stock NTVDM answers INT 5Ch (its NetBIOS VDD hands NCBs to the NT NetBIOS driver),
; and that -- not a packet driver needing raw Ethernet -- is the period network API a
; DOS program under XP gets. This asks what the reference answers:
;   * the presence test every NetBIOS program makes: an INVALID command (7Fh) must
;     come back 03h ("invalid command") -- an absent NetBIOS leaves AL untouched;
;   * RESET (32h), ADAPTER STATUS (33h) on "*" -- retcode, how many bytes came back,
;     whether the adapter address is non-zero (its value is the machine's own);
;   * ADD NAME (30h) / DELETE NAME (31h) of a unique name -- retcodes, and whether a
;     name number was handed out;
;   * INT 2Ah AH=00h, the network installation check (AH<>0 = installed).
; The NCB is 64 bytes: +0 command +1 retcode +2 lsn +3 num +4 buffer(far) +8 length
; +0Ah callname[16] +1Ah name[16] +2Ah rto +2Bh sto +2Ch post(far) +30h lana
; +31h cmd_cplt.
; nasm -f bin p_netb.asm -o p_netb.com

        org     100h
        jmp     start
%include "probe.inc"

NCB_CMD  equ 0
NCB_RET  equ 1
NCB_NUM  equ 3
NCB_BUF  equ 4
NCB_LEN  equ 8
NCB_CALL equ 0Ah
NCB_NAME equ 1Ah
NCB_LANA equ 30h
NCB_CPLT equ 31h

; clear the NCB, set command %1
%macro NCBINIT 1
        push    ds
        pop     es
        mov     di, ncb
        mov     cx, 32
        xor     ax, ax
        cld
        rep     stosw
        mov     byte [ncb + NCB_CMD], %1
        mov     byte [ncb + NCB_LANA], 0
%endmacro

%macro CALL5C 0
        push    ds
        pop     es
        mov     bx, ncb
        mov     ax, 0BEEFh              ; poison AL: untouched means "nobody answered"
        int     5Ch
%endmacro

start:
        PROBE_BEGIN "netb"

        ; ---- presence: an invalid command
        NCBINIT 7Fh
        CALL5C
        xor     ah, ah
        call    probe_capture
        EMIT    "int5c.7f.al", "AX"
        mov     al, [ncb + NCB_RET]
        xor     ah, ah
        call    probe_capture
        EMIT    "int5c.7f.ncb_ret", "AX"

        ; ---- RESET
        NCBINIT 32h
        CALL5C
        xor     ah, ah
        call    probe_capture
        EMIT    "int5c.reset.al", "AX"

        ; ---- ADAPTER STATUS of "*" into a 64-byte buffer
        NCBINIT 33h
        mov     byte [ncb + NCB_CALL], '*'
        mov     word [ncb + NCB_BUF], abuf
        mov     [ncb + NCB_BUF + 2], ds
        mov     word [ncb + NCB_LEN], 64
        CALL5C
        xor     ah, ah
        call    probe_capture
        EMIT    "int5c.astat.al", "AX"
        mov     ax, [ncb + NCB_LEN]
        call    probe_capture
        EMIT    "int5c.astat.len", "AX"
        mov     ax, [abuf]
        or      ax, [abuf + 2]
        or      ax, [abuf + 4]
        neg     ax
        sbb     ax, ax
        neg     ax
        call    probe_capture
        EMIT    "int5c.astat.mac.nonzero", "AX"

        ; ---- ADD NAME / DELETE NAME
        NCBINIT 30h
        mov     si, uname
        mov     di, ncb + NCB_NAME
        mov     cx, 16
        rep     movsb
        CALL5C
        xor     ah, ah
        call    probe_capture
        EMIT    "int5c.addname.al", "AX"
        mov     al, [ncb + NCB_NUM]
        or      al, al
        jz      .z
        mov     al, 1
.z:     xor     ah, ah
        call    probe_capture
        EMIT    "int5c.addname.num.nonzero", "AX"

        mov     byte [ncb + NCB_CMD], 31h
        CALL5C
        xor     ah, ah
        call    probe_capture
        EMIT    "int5c.delname.al", "AX"

        ; ---- NO-WAIT ADD NAME with a POST routine. Real NetBIOS calls POST when the
        ; command completes (ES:BX = the NCB); wait up to ~8 s of BIOS ticks for it --
        ; a name registration on NetBT broadcasts for a few seconds (stock was still
        ; pending, cmd_cplt FFh, after 2 s).
        NCBINIT 0B0h                    ; 30h | 80h
        mov     si, uname2
        mov     di, ncb + NCB_NAME
        mov     cx, 16
        rep     movsb
        mov     word [ncb + 2Ch], postr
        mov     [ncb + 2Eh], cs
        mov     word [posted], 0
        CALL5C
        xor     ah, ah
        call    probe_capture
        EMIT    "int5c.nowait.al", "AX"
        push    ds
        mov     ax, 40h
        mov     es, ax
        mov     bx, [es:6Ch]
        pop     ds
.wt:    cmp     word [posted], 0
        jne     .wd
        mov     ax, [es:6Ch]
        sub     ax, bx
        cmp     ax, 146
        jb      .wt
.wd:    mov     ax, [posted]
        call    probe_capture
        EMIT    "int5c.nowait.post.called", "AX"
        mov     ax, [postbx]
        cmp     ax, ncb
        mov     ax, 0
        jne     .pb
        inc     ax
.pb:    call    probe_capture
        EMIT    "int5c.nowait.post.esbx.is.ncb", "AX"
        mov     al, [ncb + NCB_CPLT]
        xor     ah, ah
        call    probe_capture
        EMIT    "int5c.nowait.cmd_cplt", "AX"
        mov     byte [ncb + NCB_CMD], 31h          ; and delete it again (wait form)
        mov     word [ncb + 2Ch], 0
        mov     word [ncb + 2Eh], 0
        CALL5C

        ; ---- INT 2Ah AH=00h
        mov     ax, 0000h
        int     2Ah
        mov     al, ah
        xor     ah, ah
        call    probe_capture
        EMIT    "int2a.00.ah", "AX"

        PROBE_END

uname   db      'NTVDMEXPROBE    '      ; 16 bytes, space padded
uname2  db      'NTVDMEXPOST     '
posted  dw      0
postbx  dw      0

; the POST routine: entered like an interrupt handler, ES:BX = the NCB; IRET
postr:  push    ds
        push    cs
        pop     ds
        inc     word [posted]
        mov     [postbx], bx
        pop     ds
        iret
ncb     times 64 db 0
abuf    times 64 db 0
