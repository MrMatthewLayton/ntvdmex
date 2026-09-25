; p_dpmins.com -- INT 2Fh AX=1687h, the DPMI installation check, measured rather
;                  than remembered.
;
; WHY IT EXISTS. `krnl386.exe` builds the whole WINFLAGS word from this ONE call
; (seg 1, 0xD68A -- `tools/ne/nedis.py guest/ne/krnl386.exe 1 0xd650 0x70`):
;
;       mov ax,0x1687 ; int 2Fh
;       or  ax,ax     ; jne -> no DPMI host, bail
;       xor bh,bh
;       cmp cl,3      ; jb  -> bail
;       mov bl,4      ; CL == 3
;       je  +2
;       mov bl,8      ; CL >  3
;       mov [0x464],bx        ; <- GetWinFlags (KERNEL.132) returns exactly this word
;
; ⇒ `CL` is an ORDINAL CPU class and `3` is the lowest krnl386 accepts. That reading
;   is from the binary, not from a spec sheet we do not have locally.
;
; AND IT EXPLAINS A MEASURED MISMATCH. `tools/wintest` measured `GetWinFlags` as
; **4C25 from us** and **4C29 from stock ntvdm**, reproduced twice. The single bit is
; 0x0004 vs 0x0008 -- exactly `bl=4` vs `bl=8` -- so **stock's DPMI host returns CL>3
; and ours returns CL=3** (`main.c`, two sites, both hardcoded 0x03).
;
; ⚠ WHAT THIS PROBE CAN AND CANNOT SETTLE. It reports what each host answers. It does
;   NOT establish that "4 means 80486" -- that mapping is not in any document in this
;   repo. What it establishes is the SHAPE of the answer across hosts, and whether ours
;   is the outlier. The authoritative row is stock ntvdm's, and that needs the IFEO
;   bracket.
; ⚠ A host with NO DPMI provider answers AX unchanged (0x1687) -- which is a real
;   result, not a failure, and is why AX is in every SIG.
;
; nasm -f bin -I tools/dostest/ p_dpmins.asm -o p_dpmins.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "dpmins"

        ; ── THE CALL ITSELF. Every output register is poisoned first, because
        ;    "the host left CL alone" and "the host said CL=0" must not look the
        ;    same -- and CL is the whole question here.
        POISON
        mov     si, 0DEADh
        mov     di, 0BEEFh
        ; ⚠ ES IS AN OUTPUT (the mode-switch entry's segment), so it must be POISONED
        ;   like the rest. The first cut left it as DS, i.e. the PSP -- which differs
        ;   on every host, so "no DPMI host touched ES" came out as a DISPUTED row
        ;   between two oracles that in fact agreed completely. A poison value that no
        ;   host would return says "untouched" plainly.
        mov     ax, 0E5E5h
        mov     es, ax
        mov     ax, 1687h
        int     2Fh
        call    probe_capture
        ; ES:DI is the mode-switch entry and SI the private-data paragraph count;
        ; both are part of the answer, so both are in the signature.
        EMIT    "int2f.1687", "AX,BX,CX,DX,SI,DI,ES"

        ; ── A NEGATIVE CONTROL. 1686h is "are we running under DPMI in protected
        ;    mode" -- a DIFFERENT question with a different answer, so a handler that
        ;    claimed the whole 16xx range would be visible here instead of passing.
        POISON
        mov     ax, 0E5E5h
        mov     es, ax
        mov     ax, 1686h
        int     2Fh
        call    probe_capture
        EMIT    "int2f.1686", "AX,CF"

        PROBE_END
