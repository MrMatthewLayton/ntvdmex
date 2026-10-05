; p_int53.com -- differential probe: INT 21h AH=53h, the AL sub-functions.
;
; WHY THIS EXISTS. Documented AH=53h is "translate BPB to DPB": it takes DS:SI ->
; a BPB and ES:BP -> a DPB, and has NO AL sub-function. XP's own COMMAND.COM uses
; it as a PRIVATE QUERY -- its resident part, guest 0x1692:
;
;       mov al,5 ; mov ah,53h ; int 21h ; mov [0x327],al
;       mov al,7 ; mov ah,53h ; int 21h ; mov [0x328],al
;
; -- and reads the answer out of AL. It also issues AL=02 during start-up. Our
; handler returned AX=1/CF=1 ("unimplemented"), which put a 1 in [0x327], and
; THREE separate gates in COMMAND.COM read `cmp byte [0x327],1 / jz` as "do not be
; the interactive shell". An unimplemented call that still answers is worse than
; one that does not -- see docs/inventory/bop.md.
;
; ⛔⛔ DO NOT RUN THIS AGAINST A REAL-DOS ORACLE. IT HANGS MS-DOS 6.22.
;   Measured twice, once with a broken probe and once with this one, so the hang is
;   the CALL and not the probe: `dosdiff.py --host msdos622` never reaches QUIT.COM.
;   Documented AH=53h does not report anything -- it BUILDS a DPB from a BPB the
;   caller supplies -- so handing it a fabricated pointer is not a question, it is a
;   mutation, and a real DOS does not survive being asked. `--host pcem` and
;   `--host dosbox-x` must be assumed to behave the same and have NOT been tried.
;   ⇒ Run it with `--host ntvdmex` alone, or against STOCK NTVDM, and nothing else.
;
; ⚠ WHAT THIS PROBE CAN AND CANNOT SETTLE. Against MS-DOS 6.22 it establishes the
;   DOCUMENTED behaviour, which is what our DOS should match for a real DOS guest.
;   It CANNOT settle NT's private sub-functions: only stock ntvdm can, and that
;   needs the IFEO bracket. Run it there when that is authorised -- same binary,
;   one more --host.
;
; ⚠ SI AND BP ARE ZEROED exactly as COMMAND.COM zeroes them before the call
;   (`xor si,si / xor bp,bp` at guest 0x1597). A DS:SI of anything else would be a
;   different question, and on a real DOS a wild BPB pointer is not a safe one.
;
; nasm -f bin -I tests/probes/dos/ p_int53.asm -o p_int53.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "int53"

        ; ⚠ WRITTEN OUT LONGHAND, NOT AS A MACRO. The first cut built the case
        ; names with a %%1 substitution that NASM did not expand, so all eight rows
        ; emitted under ONE name -- eight different questions collapsed into a
        ; single answer, which is the shape of a probe that looks fine and measures
        ; nothing. Eight literal names cannot do that.
        ;
        ; 00 is the documented form; 02, 05 and 07 are the ones XP's COMMAND.COM
        ; actually issues. The rest are swept so the SHAPE of the range is visible:
        ; a handler answering only the three we know about would be indistinguishable
        ; from a correct one on a three-case test.

        POISON
        xor     si, si                  ; as COMMAND.COM does (guest 0x1597)
        xor     bp, bp
        push    ds
        pop     es
        mov     ax, 5300h
        int     21h
        call    probe_capture
        EMIT    "int21.5300", "AX,CF"

        POISON
        xor     si, si                  ; as COMMAND.COM does (guest 0x1597)
        xor     bp, bp
        push    ds
        pop     es
        mov     ax, 5301h
        int     21h
        call    probe_capture
        EMIT    "int21.5301", "AX,CF"

        POISON
        xor     si, si                  ; as COMMAND.COM does (guest 0x1597)
        xor     bp, bp
        push    ds
        pop     es
        mov     ax, 5302h
        int     21h
        call    probe_capture
        EMIT    "int21.5302", "AX,CF"

        POISON
        xor     si, si                  ; as COMMAND.COM does (guest 0x1597)
        xor     bp, bp
        push    ds
        pop     es
        mov     ax, 5303h
        int     21h
        call    probe_capture
        EMIT    "int21.5303", "AX,CF"

        POISON
        xor     si, si                  ; as COMMAND.COM does (guest 0x1597)
        xor     bp, bp
        push    ds
        pop     es
        mov     ax, 5304h
        int     21h
        call    probe_capture
        EMIT    "int21.5304", "AX,CF"

        POISON
        xor     si, si                  ; as COMMAND.COM does (guest 0x1597)
        xor     bp, bp
        push    ds
        pop     es
        mov     ax, 5305h
        int     21h
        call    probe_capture
        EMIT    "int21.5305", "AX,CF"

        POISON
        xor     si, si                  ; as COMMAND.COM does (guest 0x1597)
        xor     bp, bp
        push    ds
        pop     es
        mov     ax, 5306h
        int     21h
        call    probe_capture
        EMIT    "int21.5306", "AX,CF"

        POISON
        xor     si, si                  ; as COMMAND.COM does (guest 0x1597)
        xor     bp, bp
        push    ds
        pop     es
        mov     ax, 5307h
        int     21h
        call    probe_capture
        EMIT    "int21.5307", "AX,CF"

        PROBE_END
