; p_umb.com -- INT 21h AH=58h: the allocation strategy and the UMB LINK.
;
; GH #47. MEM.EXE reports garbage upper-memory figures on NTVDMEX -- 1,664K of
; "Upper" on a machine with none, and 0K free conventional -- and its own INT 21h
; trace says why it might:
;
;     21:58/02 bx=0000        get UMB link state
;     21:58/03 bx=0000        set it OFF
;     21:58/03 bx=0001        set it ON        <- we accept this unconditionally
;     21:4a/ff bx=15b9        then resize and walk the chain
;
; If DOS lets you link a UMB chain that does not exist, a walker afterwards
; counts whatever it finds past conventional memory as upper memory. So the
; question is what 5803h does on a machine with NO UMB provider, and that is a
; measurement, not a recollection: the oracle boots with no EMM386 and no
; DOS=UMB, which is exactly the configuration NTVDMEX presents.
;
; ⚠ 5801h/5803h are SETTERS. Each case here puts the machine back the way it
;   found it, because a probe that leaves the allocation strategy changed makes
;   every later case in the run measure a different machine.
;
; nasm -f bin p_umb.asm -o p_umb.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "umb"

        ; ---- 5800h: get the current allocation strategy, so we can restore it.
        POISON
        mov     ax, 5800h
        int     21h
        call    probe_capture
        mov     [strat0], ax
        EMIT    "int21.5800.get", "AX,CF"

        ; ---- 5802h: get the UMB link state. 00 = not linked, 01 = linked.
        POISON
        mov     ax, 5802h
        int     21h
        call    probe_capture
        mov     [umb0], ax
        EMIT    "int21.5802.get", "AX,CF"

        ; ---- ★ 5803h BX=1: LINK THE UMB CHAIN. This is the call MEM makes.
        ; On a machine with no upper memory there is no chain to link, so the
        ; question is whether DOS refuses. If it does and we accept, every
        ; walker after us is told upper memory exists.
        mov     ax, 5803h
        mov     bx, 1
        int     21h
        call    probe_capture
        EMIT    "int21.5803.link.on", "AX,CF"

        ; ---- and did it actually take? A call that reports success and does
        ; nothing is a different bug from one that refuses, and they are
        ; indistinguishable without asking again.
        POISON
        mov     ax, 5802h
        int     21h
        call    probe_capture
        EMIT    "int21.5802.after.on", "AX,CF"

        ; ---- 5803h BX=0: unlink, which is the state we found it in.
        mov     ax, 5803h
        mov     bx, 0
        int     21h
        call    probe_capture
        EMIT    "int21.5803.link.off", "AX,CF"

        ; ---- restore the allocation strategy we came in with.
        mov     ax, 5801h
        mov     bx, [strat0]
        int     21h

        PROBE_END

strat0   dw 0
umb0     dw 0
