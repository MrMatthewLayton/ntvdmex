; p_pm256.com -- two protected-mode gaps from GH #256, asked of a DPMI client.
;
;   1. INT 15h AH=86h from PROTECTED MODE returned at once (the V86 arm has waited
;      since #206). Asked here in real mode first (every host can answer that row)
;      and then in PM: a 200,000 us wait must let the BIOS tick (0040:006C, 18.2 Hz)
;      advance by about 3.6 -- emitted as a verdict, AX=1 when 3..6 ticks passed.
;   2. INT 21h AH=40h from PROTECTED MODE wrote handles 1/2 to the screen even when
;      handle 1 had been redirected to a file. The probe redirects handle 1 to
;      PM256.TXT itself (3Ch, 45h, 46h), writes from PM with AH=40h, AH=02h and
;      AH=09h, restores handle 1, and reports the file's size: 6 bytes when every
;      write reached the file ("PMW" + "x" + "yz").
;
; ⚠ ONE HOST: the reference DOSes run no DPMI host, so only `dpmi.present` and the
;   real-mode row compare across hosts; the PM rows are measured on the rig and
;   held to the spec (CX:DX microseconds; a bound handle is a file).
;
; nasm -f bin p_pm256.asm -o p_pm256.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "pm256"

        ; ---- real mode: the reference row
        push    es
        mov     ax, 40h
        mov     es, ax
        mov     bx, [es:6Ch]
        mov     [t0], bx
        pop     es
        mov     ax, 8600h
        mov     cx, 0003h
        mov     dx, 0D40h                       ; 200,000 us
        int     15h
        call    probe_capture
        call    tickverdict
        EMIT    "int15.86.rm.ticks", "AX"

        ; ---- shrink to 64 KB and find the DPMI host
        mov     ah, 4Ah
        mov     bx, 1000h
        push    cs
        pop     es
        int     21h
        mov     ax, 1687h
        int     2Fh
        or      ax, ax
        jz      .have
        mov     ax, 0
        call    probe_capture
        EMIT    "dpmi.present", "AX"
        jmp     .out
.have:  mov     [entry], di
        mov     [entry+2], es
        mov     ax, 1
        call    probe_capture
        EMIT    "dpmi.present", "AX"
        mov     bx, si                          ; private data paragraphs
        or      bx, bx
        jz      .nopriv
        mov     ah, 48h
        int     21h
        jc      .out
        mov     es, ax
.nopriv:
        ; ---- redirect handle 1 to PM256.TXT before entering PM (nothing is
        ;      emitted while it is redirected)
        mov     ah, 3Ch
        xor     cx, cx
        mov     dx, fname
        int     21h
        jc      .out
        mov     [hfile], ax
        mov     ah, 45h
        mov     bx, 1
        int     21h
        mov     [hsave], ax

        xor     ax, ax                          ; 16-bit client
        call    far [entry]
        jc      .nopm

        ; ======== PROTECTED MODE ========
        mov     ah, 46h                         ; handle 1 := the file
        mov     bx, [hfile]
        mov     cx, 1
        int     21h
        mov     ah, 40h
        mov     bx, 1
        mov     cx, 3
        mov     dx, txt_pmw
        int     21h
        mov     ah, 02h
        mov     dl, 'x'
        int     21h
        mov     ah, 09h
        mov     dx, txt_yz
        int     21h
        mov     ah, 46h                         ; handle 1 := the console again
        mov     bx, [hsave]
        mov     cx, 1
        int     21h
        mov     ax, 4202h                       ; file size = seek to end
        mov     bx, [hfile]
        xor     cx, cx
        xor     dx, dx
        int     21h
        call    probe_capture
        EMIT    "pm.int21.40.redirected.size", "AX,DX,CF"

        mov     ax, 0002h                       ; a selector for 0040h
        mov     bx, 40h
        int     31h
        mov     [sel40], ax
        push    es
        mov     es, ax
        mov     bx, [es:6Ch]
        mov     [t0], bx
        pop     es
        mov     ax, 8600h
        mov     cx, 0003h
        mov     dx, 0D40h
        int     15h
        call    probe_capture
        push    es
        mov     es, [sel40]
        call    tickverdict_es
        pop     es
        EMIT    "pm.int15.86.ticks", "AX,CF"

        mov     ah, 3Eh
        mov     bx, [hfile]
        int     21h
        mov     ah, 41h
        mov     dx, fname
        int     21h
        PROBE_END                               ; AH=4Ch from PM

.nopm:  mov     ax, 0
        call    probe_capture
        EMIT    "dpmi.entered", "AX"
.out:   PROBE_END

; AX := 1 if 3..6 ticks passed since [t0], else 0x8000 + ticks. CF in [__fl] kept.
tickverdict:
        push    es
        mov     ax, 40h
        mov     es, ax
        call    tickverdict_es
        pop     es
        ret
tickverdict_es:
        mov     ax, [es:6Ch]
        sub     ax, [t0]
        cmp     ax, 3
        jb      .no
        cmp     ax, 6
        ja      .no
        mov     ax, 1
        jmp     .done
.no:    or      ax, 8000h
.done:  mov     [__ax], ax
        ret

entry   dd      0
t0      dw      0
sel40   dw      0
hfile   dw      0
hsave   dw      0
fname   db      'PM256.TXT', 0
txt_pmw db      'PMW'
txt_yz  db      'yz$'
