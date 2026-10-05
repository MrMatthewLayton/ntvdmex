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
; ⛔⛔⛔ AND THAT REDIRECT IS PART OF THE MEASUREMENT.  (s79)
;   Every value this project has ever measured against stock ntvdm was measured
;   with the guest's stdout pointed at a file, because that is how the harness
;   reads the answer.  For almost every call that is invisible.  For a call that
;   ASKS ABOUT THE CONSOLE it is the whole question -- and INT 21h AH=53h AL=5 is
;   exactly that: XP's COMMAND.COM stores its answer in [0x327] and uses it to
;   decide whether to read the keyboard at all.  We measured AL=1 ("do not"), and
;   stock's own shell demonstrably does.  A probe that reports through the thing
;   it is asking about cannot answer that question.
;
;   ⇒ DEFINE `PROBE_FILE` BEFORE THE INCLUDE to write the dump with AH=3Ch/40h/3Eh
;     instead, so the probe needs no redirection at all:
;
;         %define PROBE_FILE "int53f.txt"
;         %include "probe.inc"
;
;     Off by default: existing probes are unchanged, byte for byte.
;   ⚠ A run with PROBE_FILE and a run without are DIFFERENT QUESTIONS asked of any
;     host that can tell.  That is the point; label the rows accordingly.
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
; IT ALSO RESTORES DS TO CS ON THE WAY OUT, and that is not incidental.  Several
; DOS calls RETURN A SEGMENT IN DS -- 1Bh and 1Ch hand back DS:BX pointing at the
; media byte, 32h a DPB, 52h the list of lists.  Every store below, and every
; store the EMIT macros make afterwards, is DS-relative, so without this the
; probe writes its own state into DOS's segment and then prints labels read from
; there: the dump comes out as unlabelled hex, or as fragments of code.  The
; guest-visible DS is still reported faithfully -- it is captured first.
probe_capture:
        pushf                           ; flags FIRST, before anything of ours
        push    ds
        push    ax
        mov     ax, cs
        mov     ds, ax                  ; from here on our own data is reachable
        pop     ax
        mov     [__ax], ax
        mov     [__bx], bx
        mov     [__cx], cx
        mov     [__dx], dx
        mov     [__si], si
        mov     [__di], di
        mov     [__es], es
        pop     ax                      ; the DS the call left
        mov     [__ds], ax
        pop     ax                      ; the flags the call left
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
%ifdef PROBE_FILE
        call    probe_flush
%endif
%ifdef PROBE_DEFER
        call    probe_flush_con
%endif
        mov     ax, 4C00h
        int     21h

; ── PROBE_DEFER: THE DUMP WAITS UNTIL THE SCREEN IS NOT THE SUBJECT. (#252)
;    A probe that measures what INT 10h draws into a GRAPHICS frame buffer cannot
;    print as it goes: on a host whose console is the screen (ours -- DOS output goes
;    to the teletype), every EMIT line would itself be drawn into the frame the next
;    case reads back. The oracles redirect stdout to a file and never see it, so the
;    disagreement would be manufactured by the reporting. Define PROBE_DEFER and the
;    dump is buffered (as PROBE_FILE does) and written to handle 1 with AH=40h at
;    PROBE_END -- after the probe has set its last mode. Same exit codes as the file
;    sink; an overflow appends #TRUNCATED. Off by default: other probes unchanged.
%ifdef PROBE_DEFER
probe_flush_con:
        cmp     word [__fovf], 0
        je      .w
        mov     si, __s_trunc
        call    probe_f_puts
.w:     mov     ah, 40h
        mov     bx, 1
        mov     cx, [__flen]
        mov     dx, __fbuf
        int     21h
        jc      .e
        cmp     word [__fovf], 0
        jne     .eovf
        ret
.e:     mov     ax, 4C02h
        int     21h
.eovf:  mov     ax, 4C03h
        int     21h
%define __PROBE_SINK
%endif
%ifdef PROBE_FILE
%define __PROBE_SINK
%endif

%ifdef __PROBE_SINK
%ifdef PROBE_FILE
; ── THE FILE SINK. Only assembled when PROBE_FILE is defined; see the note at the
;    top for why it exists. AH=3Ch creates/truncates, AH=40h writes, AH=3Eh closes.
;    ⚠ EXIT CODE CARRIES THE VERDICT, because a probe that cannot write its answer
;      must not look like one that wrote nothing interesting:
;         0 = written   1 = create failed   2 = short write   3 = BUFFER OVERFLOWED
;      An overflow ALSO appends `#TRUNCATED` as the last line, so a truncated dump
;      can never read as a complete one. A file that stops early and says nothing
;      is the exact shape of a measurement this project has been burned by twice.
probe_flush:
        cmp     word [__fovf], 0
        je      .create
        mov     si, __s_trunc           ; loud, in the file itself
        call    probe_f_puts
.create:
        mov     ah, 3Ch
        xor     cx, cx                  ; normal attributes
        mov     dx, __fname
        int     21h
        jc      .ecreate
        mov     bx, ax                  ; handle
        mov     ah, 40h
        mov     cx, [__flen]
        mov     dx, __fbuf
        int     21h
        jc      .eshort
        cmp     ax, [__flen]
        jne     .eshort
        mov     ah, 3Eh
        int     21h
        cmp     word [__fovf], 0
        jne     .eovf
        ret
.ecreate:
        mov     ax, 4C01h
        int     21h
.eshort:
        mov     ah, 3Eh
        int     21h
        mov     ax, 4C02h
        int     21h
.eovf:
        mov     ax, 4C03h
        int     21h
%endif

; append AL to the buffer; sets __fovf and drops the byte if it would not fit.
probe_f_putc:
        push    bx
        mov     bx, [__flen]
        cmp     bx, __fcap
        jae     .full
        mov     [__fbuf + bx], al
        inc     word [__flen]
        pop     bx
        ret
.full:
        mov     word [__fovf], 1
        pop     bx
        ret

probe_f_puts:                           ; SI = asciiz, straight to the buffer
        push    ax
        push    si
.l:     lodsb
        or      al, al
        jz      .d
        call    probe_f_putc
        jmp     .l
.d:     pop     si
        pop     ax
        ret

%ifdef PROBE_FILE
__fname    db PROBE_FILE, 0
%endif
__s_trunc  db 13, 10, '#TRUNCATED -- the buffer filled; this dump is INCOMPLETE', 13, 10, 0
__flen     dw 0
__fovf     dw 0
%ifdef PROBE_FCAP
__fcap     equ PROBE_FCAP
%else
__fcap     equ 4096
%endif
__fbuf     times __fcap db 0
%endif

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
%ifdef __PROBE_SINK
        call    probe_f_putc            ; no console at all -- see the note at the top
%else
        mov     dl, al
        mov     ah, 02h
        int     21h
%endif
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
