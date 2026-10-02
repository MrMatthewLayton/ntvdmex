; p_kbd2.com -- the keyboard BIOS beyond "store a key". GH #254.
;
; Headless, so no key is ever pressed: the probe WRITES the BIOS's own state bytes
; and asks INT 16h what it makes of them, the way p_kbd injects into the ring.
;
;   int16.12.*  AH=12h builds AH from 0040:0018 and 0040:0096 in its OWN layout
;               (0 LCtrl, 1 LAlt, 2 RCtrl, 3 RAlt, 4 Scroll, 5 Num, 6 Caps,
;               7 SysReq) -- 0018 keeps SysReq at bit 2, Pause at 3 and Insert at
;               7, and the right-hand keys live in 0096 bits 2/3. We copied 0018.
;   int15.85.*  the SysReq hook our INT 09h now calls: what the BIOS's default
;               INT 15h AH=85h answers.
; The three bytes are saved and restored around each question, with IF clear.
;
; ORACLE-ALSO: pcem
; nasm -f bin p_kbd2.asm -o p_kbd2.com
        org     100h
        jmp     start
%include "probe.inc"

%macro ASK12 4                                  ; 0017, 0018, 0096, name
        cli
        push    es
        mov     ax, 40h
        mov     es, ax
        mov     al, [es:17h]
        mov     [s17], al
        mov     al, [es:18h]
        mov     [s18], al
        mov     al, [es:96h]
        mov     [s96], al
        mov     byte [es:17h], %1
        mov     byte [es:18h], %2
        mov     al, [s96]
        and     al, 0F0h
        or      al, %3
        mov     [es:96h], al
        pop     es
        POISON
        mov     ax, 12A5h
        int     16h
        call    probe_capture
        push    es
        mov     ax, 40h
        mov     es, ax
        mov     al, [s17]
        mov     [es:17h], al
        mov     al, [s18]
        mov     [es:18h], al
        mov     al, [s96]
        mov     [es:96h], al
        pop     es
        sti
        EMIT    %4, "AX"
%endmacro

start:
        PROBE_BEGIN "kbd2"

        ASK12   00h, 00h, 00h, "int16.12.none"
        ASK12   00h, 01h, 00h, "int16.12.lctrl"
        ASK12   00h, 02h, 00h, "int16.12.lalt"
        ASK12   00h, 00h, 04h, "int16.12.rctrl"
        ASK12   00h, 00h, 08h, "int16.12.ralt"
        ASK12   00h, 04h, 00h, "int16.12.sysreq"
        ASK12   00h, 70h, 00h, "int16.12.locks.held"
        ASK12   00h, 88h, 00h, "int16.12.ins.pause"
        ASK12   0Ch, 03h, 0Ch, "int16.12.both.sides"

        POISON
        mov     ax, 8500h
        int     15h
        call    probe_capture
        EMIT    "int15.85.press", "AX,BX,CX,DX,CF"
        POISON
        mov     ax, 8501h
        int     15h
        call    probe_capture
        EMIT    "int15.85.release", "AX,BX,CX,DX,CF"

        PROBE_END

s17     db      0
s18     db      0
s96     db      0
