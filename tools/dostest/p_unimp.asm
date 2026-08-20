; p_unimp.com -- differential probe: how does a host behave on paths it does NOT
; implement?  Feeds GH #27 (make every unimplemented path loud).
;
; Two gaps, both guest-observable, so the differential harness can prove them:
;
;   1. An unhandled INT 21h function.  NTVDMEX sets CF but leaves AX alone.  What
;      real DOS does is NOT assumed here -- that is what the oracle is for.
;
;   2. NULL IVT VECTORS -- the landmine.  Vectors we never plant read 0000:0000,
;      so a guest that INTs one far-jumps to 0000:0000 and executes the interrupt
;      vector table itself as code.  Each case reports AX=1 if the vector is null
;      and AX=0 if something is there.
;
;      The raw seg:off is dumped in CX:BX but deliberately NOT declared
;      significant: a real BIOS, DOSBox and we will all point these somewhere
;      different and that is fine.  The only comparable fact is "is anybody home".
;
; nasm -f bin p_unimp.asm -o p_unimp.com

        org     100h
        jmp     start
%include "probe.inc"

; CHECKVEC <vector>, "<case name>" -- AX=1 if IVT[vector] is 0000:0000.
%macro CHECKVEC 2
        push    es
        xor     ax, ax
        mov     es, ax
        mov     bx, [es:%1 * 4]                 ; offset
        mov     cx, [es:%1 * 4 + 2]             ; segment
        pop     es
        mov     ax, bx
        or      ax, cx                          ; ZF set <=> vector is 0000:0000
        mov     ax, 0                           ; MOV does not disturb ZF
        jnz     %%live
        inc     ax
%%live:
        call    probe_capture
        EMIT    %2, "AX"
%endmacro

start:
        PROBE_BEGIN "unimp"

        ; ---- an INT 21h function nobody implements.
        ; AH=FFh is not a DOS service on any DOS.  We are asking what a host does
        ; when asked for something it does not have: which of AX and CF move.
        POISON
        mov     ax, 0FF00h
        int     21h
        call    probe_capture
        EMIT    "int21.FF", "AX,CF"

        ; Two more undefined functions, so the conclusion is not n=1.
        POISON
        mov     ax, 07300h
        int     21h
        call    probe_capture
        EMIT    "int21.73", "AX,CF"

        POISON
        mov     ax, 08800h
        int     21h
        call    probe_capture
        EMIT    "int21.88", "AX,CF"

        ; ---- the null-vector landmine.
        ; 21h is the CONTROL: it is planted on every host, so if this one ever
        ; reports null the probe itself is broken and the rest means nothing.
        CHECKVEC 021h, "vec.21.control"
        CHECKVEC 011h, "vec.11"                 ; equipment list
        CHECKVEC 012h, "vec.12"                 ; memory size
        CHECKVEC 013h, "vec.13"                 ; disk services
        CHECKVEC 015h, "vec.15"                 ; system services
        CHECKVEC 017h, "vec.17"                 ; printer
        CHECKVEC 01Bh, "vec.1B"                 ; ctrl-break
        CHECKVEC 025h, "vec.25"                 ; absolute disk read
        CHECKVEC 026h, "vec.26"                 ; absolute disk write

        PROBE_END
