; p_int53f.com -- p_int53 asked with NOTHING REDIRECTED.
;
; ⛔⛔⛔ WHY THIS EXISTS, AND IT IS NOT A DUPLICATE. (s79)
;   `p_int53.com` reports through INT 21h AH=02, so the harness captures it with
;   `> FILE`. Every stock-ntvdm measurement this project holds was taken that way.
;   For INT 21h AH=53h that turns out to matter, because its private AL=5 answer is
;   what XP's COMMAND.COM stores in [0x327] and uses to decide whether it reads the
;   keyboard at all -- and COMMAND.COM's own image proves the point:
;
;       transient 0x0A0D is the read-a-line routine; AL=0 reads the keyboard.
;       Its only two AL=0 callers (0x0924, 0x0C33) both sit behind
;           cmp byte [0x327],1 / jz away
;       and [0x327]'s only writer is
;           resident 0x169B  mov al,5 / mov ah,53h / int 21h / mov [0x327],al
;
;   Stock ntvdm's COMMAND.COM IS interactive, so in the shell's context stock must
;   answer AL=0. Our probe measured AL=1. One of the two contexts is lying about the
;   other, and the most obvious difference between them is that the probe was asked
;   with its stdout pointed at a file.
;
;   ⇒ This probe writes its dump with AH=3Ch/40h/3Eh into INT53F.TXT, so it can be
;     run with NO redirection of any kind. Same eight questions, same order, same
;     SI=BP=0 as COMMAND.COM issues them. The ONLY difference is how the answer
;     gets out -- which is exactly the variable under test.
;
;   ⚠ RUN IT BOTH WAYS against stock and diff the two files. A single run cannot
;     distinguish "the value is X" from "the value is X when redirected".
;   ⚠ `tools/ntvdm/cmdcom.py` re-derives every address quoted above from the binary.
;
; ⛔⛔ DO NOT RUN THIS AGAINST A REAL-DOS ORACLE. Documented AH=53h BUILDS a DPB from
;   a caller-supplied BPB, and a fabricated pointer HANGS MS-DOS 6.22 -- measured
;   twice. `--host ntvdmex` or stock ntvdm only, exactly as p_int53.asm says.
;
; ⚠ Exit code is the verdict, because a probe that could not write its answer must
;   not look like one that had nothing to say:
;     0 = written   1 = create failed   2 = short write   3 = buffer overflowed
;
; nasm -f bin -I tests/probes/dos/ p_int53f.asm -o p_int53f.com

        org     100h
        jmp     start
%define PROBE_FILE "INT53F.TXT"
%include "probe.inc"

start:
        PROBE_BEGIN "int53f"

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
