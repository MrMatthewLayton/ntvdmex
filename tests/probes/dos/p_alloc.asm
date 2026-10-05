; p_alloc.com -- INT 21h AH=58h (allocation strategy) and AH=52h (list of lists).
;
; These are the two services real MS-DOS 6.22 MEM.EXE asks for and we do not
; provide -- found by running MEM.EXE under NTVDMEX and reading the STAGE2
; to-do list that GH #27 added.  Feeds GH #35.
;
; 52h returns ES:BX pointing at DOS's internal SysVars ("list of lists").  The
; POINTER is host-specific and meaningless to compare, so what is dumped is the
; CONTENT around it -- including the word at ES:BX-2, which is the first MCB
; segment and the field a memory walker actually wants.
;
; nasm -f bin p_alloc.asm -o p_alloc.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "alloc"

        ; ---- 5800h: get allocation strategy -> AX
        POISON
        mov     ax, 5800h
        int     21h
        call    probe_capture
        EMIT    "int21.5800.get", "AX,CF"

        ; ---- 5802h: get UMB link state -> AL
        POISON
        mov     ax, 5802h
        int     21h
        call    probe_capture
        EMIT    "int21.5802.umb", "AX,CF"

        ; ---- 5801h: set strategy to BL=0 (first fit).  Harmless: 0 is the
        ; default, so this restores rather than changes.
        POISON
        mov     ax, 5801h
        mov     bx, 0
        int     21h
        call    probe_capture
        EMIT    "int21.5801.set", "AX,CF"

        ; ---- 52h: list of lists -> ES:BX
        POISON
        mov     ax, 5200h
        int     21h
        call    probe_capture
        ; Save ES:BX NOW. probe_emit uses BX as scratch, so by the time EMIT has
        ; run the pointer is gone -- the same leftover-register trap that POISON
        ; exists to expose, wearing a different hat. The first attempt at this
        ; probe dumped 48 bytes of x86 code because of it.
        mov     [sv_es], es
        mov     [sv_bx], bx
        EMIT    "int21.52", "CF"

        ; Keep ES:BX from the call above and dump SysVars from BX-2 forward.
        ; BX-2 is the first MCB segment; the rest is the structure we have to
        ; model.  Copy into our own segment first so EMIT_BUF can read it.
        push    ds
        mov     ax, [sv_es]
        mov     ds, ax
        mov     si, [sv_bx]
        sub     si, 2
        mov     ax, cs
        mov     es, ax
        mov     di, sysvars
        mov     cx, 48
        cld
        rep     movsb
        pop     ds

        EMIT_BUF "sysvars.at.bx-2", sysvars, 48

        PROBE_END

sv_es   dw 0
sv_bx   dw 0
sysvars:
        times 48 db 0
