; p_tick2c.com -- does DOS's time of day follow the BIOS tick count?  GH #262.
; ORACLE-ALSO: pcem
;
; On MS-DOS the CLOCK$ driver answers INT 21h AH=2Ch from the BIOS tick count at
; 0040:006C, so a program that sets the count -- INT 1Ah AH=01h, or a plain store
; to 006C -- moves what 2Ch returns. Ours read the host clock plus the VDM's offset,
; so it did not. This probe measures the coupling before anything is built on it.
;
;   A  INT 1Ah AH=01h with the count for 10:00:30, then 2Ch: CX = 0A00h (10:00)
;   B  a direct store to 0040:006C of the count for 11:30:30, then 2Ch: CX = 0B1Eh
;   C  INT 1Ah AH=00h right after B: does the BIOS hand back what was stored?
;      (CX:DX = 000Bh:8277h, give or take the ticks that elapse -- emitted as
;      CX only, the high word, which a few ticks cannot move)
;
; ⚠ HOURS AND MINUTES ONLY. Every target is 30 s into its minute, so the ticks
;   that pass between the set and the read cannot carry the minute.
; ⚠ IT PUTS THE CLOCK BACK through INT 21h AH=2Dh, which reloads 006C on DOS and
;   here alike; the date is untouched (none of the targets is near midnight).
;
; nasm -f bin p_tick2c.asm -o p_tick2c.com

        org     100h
        jmp     start
%include "probe.inc"

sv_hm   dw      0
sv_sc   dw      0

start:
        PROBE_BEGIN "tick2c"

        mov     ah, 2Ch
        int     21h
        mov     [sv_hm], cx
        mov     [sv_sc], dx

; A. INT 1Ah AH=01h -- 10:00:30 = 36030 s * 1193182 / 65536 = 655980 = 000A:026Ch
        mov     cx, 000Ah
        mov     dx, 026Ch
        mov     ah, 01h
        int     1Ah
        POISON
        mov     ax, 2CEEh
        int     21h
        call    probe_capture
        EMIT    "tick2c.after.1a01", "CX"

; B. a store to 0040:006C -- 11:30:30 = 41430 s -> 754295 = 000B:8277h
        push    es
        mov     ax, 0040h
        mov     es, ax
        cli
        mov     word [es:006Ch], 8277h
        mov     word [es:006Eh], 000Bh
        sti
        pop     es
        POISON
        mov     ax, 2CEEh
        int     21h
        call    probe_capture
        EMIT    "tick2c.after.store", "CX"

; C. and the BIOS's own reading of it
        POISON
        mov     ah, 00h
        int     1Ah
        mov     dx, 0D1D1h              ; low word moves with the ticks: not compared
        call    probe_capture
        EMIT    "tick2c.1a00.high", "CX"

        ; ---- put the time back
        mov     cx, [sv_hm]
        mov     dx, [sv_sc]
        mov     ah, 2Dh
        int     21h

        PROBE_END
