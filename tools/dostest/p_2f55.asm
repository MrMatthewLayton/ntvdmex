; p_2f55.com -- differential probe: INT 2Fh AX=5501h, the COMMAND.COM interface.
;
; WHY. XP's COMMAND.COM issues it during start-up (resident guest 0x16E5) and
; branches on AX:
;
;       mov ax,5501h ; int 2Fh
;       or  ax,ax
;       jnz skip        ; AX != 0  -> [0x326] = 1 -> BANNER SUPPRESSED
;                       ; AX == 0  -> keep SI:DS  -> banner allowed
;
; [0x326] is one of three gates on `Microsoft(R) Windows DOS`. Stock PRINTS that
; banner for the same launch, so stock's DOS must answer AX=0 here; we leave AX
; alone (5501) and the banner is skipped. This measures both.
;
; ⚠ SI and DS are part of the answer, not decoration: on the AX==0 path COMMAND.COM
;   stores DS:SI away as a pointer. A probe that captured AX alone would call a
;   half-implemented handler correct.
; ⚠ Safe to run anywhere: INT 2Fh is a query multiplex, and an unhandled AX simply
;   comes back unchanged. Unlike INT 21h AH=53h, nothing is built and nothing is
;   mutated -- so this one CAN be asked of a real DOS as well.
;
; nasm -f bin -I tools/dostest/ p_2f55.asm -o p_2f55.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "cmd2f55"

        POISON
        mov     ax, 5501h
        int     2Fh
        call    probe_capture
        EMIT    "int2f.5501", "AX,SI,DS"

        ; 5500h is the documented "COMMAND.COM interface" installation check that
        ; sits beside it; swept so a handler answering only 5501 is visible.
        POISON
        mov     ax, 5500h
        int     2Fh
        call    probe_capture
        EMIT    "int2f.5500", "AX,SI,DS"

        PROBE_END
