; p_misc.com -- the tail of the evidence list: 65h, 60h, 59h, 69h, 5Dh.
;
; These are what remained on the STAGE2 to-do lists after 4Eh/4Fh landed:
;   65h  extended country info   ATTRIB, COMMAND.COM     GH #38
;   60h  truename (canonicalize) CHKDSK                  GH #31
;   5Dh  internal/server         COMMAND.COM             GH #31
;   59h  get extended error      TREE                    GH #34
;   69h  disk serial number      TREE                    GH #35
;
; READ-ONLY SUBFUNCTIONS ONLY.  69h AL=01 SETS the serial number and 5Dh AL=0Ah
; SETS the error info; neither is probed, because this binary also runs on the
; rig and under DOSBox where nothing rolls back.
;
; nasm -f bin p_misc.asm -o p_misc.com

        org     100h
        jmp     start
%include "probe.inc"

; Fill a buffer with 0xEE so "DOS wrote a zero" and "DOS wrote nothing" differ.
%macro POISONBUF 2
        push    es
        mov     ax, cs
        mov     es, ax
        mov     di, %1
        mov     cx, %2
        mov     al, 0EEh
        cld
        rep     stosb
        pop     es
%endmacro

start:
        PROBE_BEGIN "misc"

        ; ---- 6501h: get extended country info into ES:DI, CX = buffer size
        POISONBUF ctry65, 64
        POISON
        mov     ax, 6501h
        mov     bx, 0FFFFh                      ; BX=FFFF: current code page
        mov     dx, 0FFFFh                      ; DX=FFFF: current country
        mov     cx, 41
        push    ds
        pop     es
        mov     di, ctry65
        int     21h
        call    probe_capture
        EMIT    "int21.6501", "AX,CF"
        EMIT_BUF "ctry65", ctry65, 44

        ; ---- 60h: canonicalize a relative name.  DS:SI = in, ES:DI = out (128).
        POISONBUF tname, 128
        POISON
        push    ds
        pop     es
        mov     si, relname
        mov     di, tname
        mov     ax, 6000h
        int     21h
        call    probe_capture
        ; AX is destroyed by 60h (the oracle returns 0x5C, others leave it); only CF.
        EMIT    "int21.60", "CF"
        EMIT_BUF "truename", tname, 24

        ; ---- 59h: get extended error for the LAST error.  The 4Eh below is
        ; made to fail first so there is a defined error to report.
        mov     ax, 4E00h
        mov     cx, 0
        mov     dx, nofile
        int     21h                             ; expected to fail

        POISON
        mov     ax, 5900h
        mov     bx, 0
        int     21h
        call    probe_capture
        EMIT    "int21.5900", "AX,BX,CX,CF"

        ; ---- 6900h: get the volume serial number, DL=0 (default drive).
        ; The serial ITSELF differs per disk, so only AX and CF are compared;
        ; the block is dumped for its layout.
        POISONBUF serial, 32
        POISON
        mov     ax, 6900h
        mov     bl, 0
        mov     dx, serial
        int     21h
        call    probe_capture
        ; AX is destroyed by 69h -- all three hosts differ. CF is the fact.
        EMIT    "int21.6900", "CF"
        EMIT_BUF "serial", serial, 24

        ; ---- 5D06h: get the DOS swappable data area -> DS:SI, CX, DX.
        ; A read; 5D0Ah (set error info) is deliberately not probed.
        POISON
        mov     ax, 5D06h
        int     21h
        call    probe_capture
        EMIT    "int21.5D06", "CF"

        PROBE_END

relname db 'SUB\FILE.TXT', 0
nofile  db 'ZZNOSUCH.XYZ', 0
ctry65:
        times 64 db 0
tname:
        times 128 db 0
serial:
        times 32 db 0
