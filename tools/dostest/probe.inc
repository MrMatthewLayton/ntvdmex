; probe.inc -- shared scaffolding for differential probes.  GH #26.
;
; A probe is one .COM that runs UNCHANGED on every host (NTVDMEX, MS-DOS 6.22
; under QEMU, stock ntvdm, DOSBox) and emits a canonical, machine-parseable dump
; that scripts/dosdiff.py can line up side by side:
;
;   #PROBE dosver
;   CASE=int21.30 SIG=AX,BX AX=1606 BX=FF00 CX=0000 DX=0000 SI=0000 DI=0000 DS=01A3 ES=01A3 FL=F246 CF=0
;   BUF=cds 0000000000000000
;   #END
;
; WHY `SIG` EXISTS.  The issue asks for AX/BX/CX/DX/SI/DI/DS/ES/flags, and all of
; them are dumped -- but DS and ES follow the PSP, which sits at a different
; paragraph on every host, and most FLAGS bits are undefined after a DOS call.
; Comparing all of them blindly manufactures disagreements that mean nothing and
; buries the real ones.  So each case DECLARES which registers actually carry its
; answer; the harness compares those and reports the rest as informational.
; Getting SIG wrong is a bug in the probe, and a visible one -- it is printed.
;
; Output goes through INT 21h AH=02, so a plain `> FILE` redirect captures it on
; every host.  Nothing here uses a BIOS call, on purpose: this scaffolding must
; not itself depend on the layer under test.
;
; Usage:
;       org 100h
;       jmp start
;       %include "probe.inc"
;   start:
;       PROBE_BEGIN "dosver"
;       mov ax, 3000h
;       int 21h
;       call probe_capture          ; MUST be the next thing after the INT
;       EMIT "int21.30", "AX,BX"
;       PROBE_END

; ---------------------------------------------------------------- macros

%macro PROBE_BEGIN 1
        jmp     %%over
%%name: db      %1, 0
%%over:
        mov     word [__namep], %%name
        call    probe_begin
%endmacro

; POISON -- stamp the output registers with recognisable junk before the call.
;
; Without this, "the host left BX alone" and "the host deliberately returned 0"
; look identical, and worse, BX may hold leftovers from our own print routine --
; which is exactly what happened on the first rig run: NTVDMEX returned BX=3246
; for INT 21h AX=3306h and 3246 turned out to be a value probe_emit itself had
; left there.  With the poison in place an untouched register reads back as
; B1B1/C1C1/D1D1 and says so plainly.
;
; Only safe on calls that take no input in BX/CX/DX -- check RBIL before using it.
%macro POISON 0
        mov     bx, 0B1B1h
        mov     cx, 0C1C1h
        mov     dx, 0D1D1h
%endmacro

%macro EMIT 2
        jmp     %%over
%%case: db      %1, 0
%%sig:  db      %2, 0
%%over:
        mov     word [__casep], %%case
        mov     word [__sigp], %%sig
        call    probe_emit
%endmacro

; EMIT_BUF <name>, <address>, <length> -- dump a buffer the call filled in.
%macro EMIT_BUF 3
        jmp     %%over
%%name: db      %1, 0
%%over:
        mov     word [__casep], %%name
        mov     si, %2
        mov     cx, %3
        call    probe_emit_buf
%endmacro

%macro PROBE_END 0
        call    probe_end
%endmacro

; ---------------------------------------------------------------- routines

; probe_capture -- snapshot every register.  MUST be executed as the very next
; instruction after the call under test: anything in between (even a MOV that
; sets no flags) risks moving what we are trying to measure.  CALL itself does
; not disturb flags, and the MOVs below do not either, so the PUSHF at the end
; still sees the flags the call left.
probe_capture:
        mov     [__ax], ax
        mov     [__bx], bx
        mov     [__cx], cx
        mov     [__dx], dx
        mov     [__si], si
        mov     [__di], di
        mov     [__ds], ds
        mov     [__es], es
        pushf
        pop     ax
        mov     [__fl], ax
        mov     ax, [__ax]              ; leave the caller's AX as we found it
        ret

probe_begin:
        mov     si, __s_probe
        call    probe_puts
        mov     si, [__namep]
        call    probe_puts
        call    probe_crlf
        ret

probe_end:
        mov     si, __s_end
        call    probe_puts
        call    probe_crlf
        mov     ax, 4C00h
        int     21h

probe_emit:
        mov     si, __s_casee
        call    probe_puts
        mov     si, [__casep]
        call    probe_puts
        mov     si, __s_sige
        call    probe_puts
        mov     si, [__sigp]
        call    probe_puts

        mov     si, __s_ax
        mov     bx, [__ax]
        call    __field
        mov     si, __s_bx
        mov     bx, [__bx]
        call    __field
        mov     si, __s_cx
        mov     bx, [__cx]
        call    __field
        mov     si, __s_dx
        mov     bx, [__dx]
        call    __field
        mov     si, __s_si
        mov     bx, [__si]
        call    __field
        mov     si, __s_di
        mov     bx, [__di]
        call    __field
        mov     si, __s_ds
        mov     bx, [__ds]
        call    __field
        mov     si, __s_es
        mov     bx, [__es]
        call    __field
        mov     si, __s_fl
        mov     bx, [__fl]
        call    __field

        mov     si, __s_cf
        call    probe_puts
        mov     al, [__fl]
        and     al, 1
        add     al, '0'
        call    probe_putc
        call    probe_crlf
        ret

__field:                                ; SI = " NAME=", BX = value
        call    probe_puts
        mov     ax, bx
        call    probe_hex16
        ret

probe_emit_buf:                         ; [__casep] = name, SI = buf, CX = len
        push    si
        push    cx
        mov     si, __s_bufe
        call    probe_puts
        mov     si, [__casep]
        call    probe_puts
        mov     al, ' '
        call    probe_putc
        pop     cx
        pop     si
.loop:
        jcxz    .done
        lodsb
        call    probe_hex8
        dec     cx
        jmp     .loop
.done:
        call    probe_crlf
        ret

; ---- primitives

probe_putc:                             ; AL = char
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        mov     dl, al
        mov     ah, 02h
        int     21h
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

probe_puts:                             ; SI = asciiz
        push    ax
        push    si
.loop:
        lodsb
        or      al, al
        jz      .done
        call    probe_putc
        jmp     .loop
.done:
        pop     si
        pop     ax
        ret

probe_hex8:                             ; AL = byte
        push    ax
        push    cx
        mov     cl, al
        shr     al, 4
        call    .nyb
        mov     al, cl
        call    .nyb
        pop     cx
        pop     ax
        ret
.nyb:
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     .out
        add     al, 7                   ; 'A'..'F'
.out:
        call    probe_putc
        ret

probe_hex16:                            ; AX = word
        push    ax
        mov     al, ah
        call    probe_hex8
        pop     ax
        call    probe_hex8
        ret

probe_crlf:
        push    ax
        mov     al, 13
        call    probe_putc
        mov     al, 10
        call    probe_putc
        pop     ax
        ret

; ------------------------------------------------------------------- data

__s_probe  db '#PROBE ', 0
__s_end    db '#END', 0
__s_casee  db 'CASE=', 0
__s_sige   db ' SIG=', 0
__s_bufe   db 'BUF=', 0
__s_ax     db ' AX=', 0
__s_bx     db ' BX=', 0
__s_cx     db ' CX=', 0
__s_dx     db ' DX=', 0
__s_si     db ' SI=', 0
__s_di     db ' DI=', 0
__s_ds     db ' DS=', 0
__s_es     db ' ES=', 0
__s_fl     db ' FL=', 0
__s_cf     db ' CF=', 0

__namep    dw 0
__casep    dw 0
__sigp     dw 0

__ax       dw 0
__bx       dw 0
__cx       dw 0
__dx       dw 0
__si       dw 0
__di       dw 0
__ds       dw 0
__es       dw 0
__fl       dw 0
