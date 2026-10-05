; p_ctry.com -- INT 21h AH=38h, get country information.
;
; Named by MEM.EXE's STAGE2 to-do list once 52h/58h were in.  Feeds GH #38.
;
; AL=00 fills a caller-supplied buffer at DS:DX with the country block and
; returns the country code in BX.  The block's LAYOUT is documented, but the
; VALUES 6.22 actually puts there are what we need, so they are dumped raw.
;
; Only the read side is probed.  Setting the country (AL=01-FEh) is skipped
; deliberately: it changes global DOS state, and the same binary runs on the rig
; and under DOSBox where there is no snapshot to roll back.
;
; nasm -f bin p_ctry.asm -o p_ctry.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "ctry"

        ; Poison the destination too, not just the registers: with a zeroed
        ; buffer "DOS wrote a zero here" and "DOS never wrote this far" look
        ; identical, so the block length would be a guess.
        mov     di, ctrybuf
        mov     cx, 68
        mov     al, 0EEh
        push    es
        mov     bx, cs
        mov     es, bx
        cld
        rep     stosb
        pop     es

        ; ---- 3800h: get current country info into DS:DX
        POISON
        mov     ax, 3800h
        mov     dx, ctrybuf
        int     21h
        call    probe_capture
        EMIT    "int21.3800", "AX,BX,CF"
        EMIT_BUF "ctry.block", ctrybuf, 34

        ; ---- 38FFh with BX=1 (USA): the "country code in BX" form.
        ; Still a READ -- AL=FFh selects the extended form of the query, it does
        ; not set anything.
        POISON
        mov     ax, 38FFh
        mov     bx, 1
        mov     dx, ctrybuf2
        int     21h
        call    probe_capture
        EMIT    "int21.38FF.us", "AX,CF"
        EMIT_BUF "ctry.block.us", ctrybuf2, 34

        PROBE_END

ctrybuf:
        times 34 db 0
ctrybuf2:
        times 34 db 0
