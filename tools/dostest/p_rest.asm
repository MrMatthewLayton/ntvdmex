; p_rest.com -- the tail of the INT 21h table: 1Bh, 1Ch, 32h, 37h, 66h, 5Eh, 64h.
;
; Read-only subfunctions only.  5Eh AL=02/03 SET the printer setup string and
; 66h AL=02 SETS the code page; neither is probed, because this binary runs on
; the rig and under DOSBox too.  Feeds GH #35 and #38.
;
; nasm -f bin p_rest.asm -o p_rest.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "rest"

        ; ---- 1Bh: allocation info for the default drive.
        ; AL=sectors/cluster, CX=bytes/sector, DX=clusters, DS:BX -> media byte.
        ; All of those are disk geometry, so only CF is comparable.
        POISON
        mov     ax, 1B00h
        int     21h
        call    probe_capture
        EMIT    "int21.1B.allocinfo", "CF"

        ; ---- 1Ch: the same for a specific drive (DL=3, C:)
        POISON
        mov     ax, 1C00h
        mov     dl, 3
        int     21h
        call    probe_capture
        EMIT    "int21.1C.allocinfo", "CF"

        ; ---- 3700h: get the SWITCH character.  This one IS a DOS fact rather
        ; than a machine property, so it is compared.
        POISON
        mov     ax, 3700h
        int     21h
        call    probe_capture
        EMIT    "int21.3700.getswitch", "AX,DL,CF"

        ; ---- 3200h: get the drive parameter block for C:.  The DPB contents are
        ; disk geometry and its address is host-specific; AL says whether the
        ; drive is valid at all, which is the comparable part.
        POISON
        mov     ax, 3200h
        mov     dl, 3
        int     21h
        call    probe_capture
        EMIT    "int21.3200.dpb", "AX"

        ; ---- 3200h for a drive that does not exist
        POISON
        mov     ax, 3200h
        mov     dl, 26
        int     21h
        call    probe_capture
        EMIT    "int21.3200.baddrive", "AX"

        ; ---- 6601h: get the global code page (BX=active, DX=system)
        POISON
        mov     ax, 6601h
        int     21h
        call    probe_capture
        EMIT    "int21.6601.getcp", "BX,DX,CF"

        ; ---- 5E00h: get the network machine name into DS:DX
        POISON
        mov     ax, 5E00h
        mov     dx, mach
        int     21h
        call    probe_capture
        EMIT    "int21.5E00.machname", "CF"

        PROBE_END

mach:
        times 20 db 0
