; p_bios.com -- the BIOS interrupts that were never planted: 11h, 12h, 15h,
; 14h, 17h, 13h.  Feeds GH #43/#44/#45.
;
; A CAVEAT THAT MATTERS MORE HERE THAN ANYWHERE ELSE: the MS-DOS 6.22 oracle is
; NOT truth for these.  QEMU runs SeaBIOS, so its answers are another
; reimplementation's opinion regardless of which DOS sits on top (epic #24).
; Worse, several of these values are statements about the MACHINE -- how much
; memory, which ports are fitted -- so the hosts SHOULD differ.  What this probe
; is really checking is that we ANSWER AT ALL and answer sanely, because until
; now every one of these vectors was a bare IRET that returned the caller's own
; registers back to it.  Almost nothing here is declared significant.
;
; nasm -f bin p_bios.asm -o p_bios.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "bios"

        ; ---- INT 11h: equipment list.  Machine-specific, so informational.
        POISON
        int     11h
        call    probe_capture
        EMIT    "int11.equip", ""

        ; ---- INT 12h: conventional memory in KB.  Machine-specific.
        POISON
        int     12h
        call    probe_capture
        EMIT    "int12.memsize", ""

        ; ---- INT 15h AH=88h: extended memory in KB.  Machine-specific.
        POISON
        mov     ax, 8800h
        int     15h
        call    probe_capture
        EMIT    "int15.88.extmem", ""

        ; ---- INT 15h with a function nobody implements.  CF should be SET and
        ; AH=86h -- that IS a convention rather than a machine property, so it
        ; is compared.
        POISON
        mov     ax, 0B0FFh
        int     15h
        call    probe_capture
        EMIT    "int15.bad.fn", "CF"

        ; ---- INT 17h AH=02: printer status.  Compared only for CF.
        POISON
        mov     ax, 0200h
        mov     dx, 0
        int     17h
        call    probe_capture
        EMIT    "int17.status", "CF"

        ; ---- INT 14h AH=03: serial status.
        POISON
        mov     ax, 0300h
        mov     dx, 0
        int     14h
        call    probe_capture
        EMIT    "int14.status", "CF"

        ; ---- INT 13h AH=01: last disk status.  Should not fail.
        POISON
        mov     ax, 0100h
        mov     dl, 80h
        int     13h
        call    probe_capture
        EMIT    "int13.status", "CF"

        PROBE_END
