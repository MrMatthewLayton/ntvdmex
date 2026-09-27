; p_subfn.com -- INT 21h sub-functions that still logged UNIMPLEMENTED (GH #165).
;
;   AH=33h  AL=00/01/02 Ctrl-Break get/set/swap, round trip; AL=07 (not 6.22's)
;   AH=38h  SET country (DX=FFFFh) to 1 and to 44; GET country 44
;   AH=43h  AL=02 (not a 6.22 sub-function)
;   AH=58h  AL=04 (not a 6.22 sub-function)
;   AH=65h  AL=20h/21h/22h capitalise char/string/ASCIIZ, AL=23h yes/no, AL=08 (none)
;
; ⚠ NOT PROBED, ON PURPOSE: AH=69h AL=01 (set disk serial) WRITES THE BOOT SECTOR
;   of the oracle's disk, and AH=4B05h changes DOS's loader state for the next EXEC.
; ⚠ Every state this changes is put back: the Ctrl-Break flag and the country.
;
; nasm -f bin p_subfn.asm -o p_subfn.com

        org     100h
        jmp     start
%include "probe.inc"

; keep DL only in __dx (DH is undefined for these)
dlonly:
        mov     ax, [__dx]
        xor     ah, ah
        mov     [__dx], ax
        ret

; keep AL only in __ax
alonly:
        mov     ax, [__ax]
        xor     ah, ah
        mov     [__ax], ax
        ret

start:
        PROBE_BEGIN "subfn"

        ; ---- Ctrl-Break: save, set on, read, swap off (returns old), read, restore
        mov     ax, 3300h
        int     21h
        mov     [brk0], dl

        mov     ax, 3301h
        mov     dl, 1
        int     21h
        mov     ax, 3300h
        mov     dl, 0EEh
        int     21h
        call    probe_capture
        call    dlonly
        EMIT    "int21.3300.after_on", "DX,CF"

        mov     ax, 3302h
        mov     dl, 0
        int     21h
        call    probe_capture
        call    dlonly
        EMIT    "int21.3302.swap_returns_old", "DX,CF"

        mov     ax, 3300h
        mov     dl, 0EEh
        int     21h
        call    probe_capture
        call    dlonly
        EMIT    "int21.3300.after_swap", "DX,CF"

        mov     ax, 3301h               ; restore
        mov     dl, [brk0]
        int     21h

        mov     ax, 3307h
        mov     dl, 0EEh
        int     21h
        call    probe_capture
        call    alonly
        EMIT    "int21.3307", "AX"

        ; ---- AH=43h AL=02, AH=58h AL=04, AH=65h AL=08: not 6.22 sub-functions
        mov     ax, 4302h
        mov     dx, fname
        int     21h
        call    probe_capture
        EMIT    "int21.4302", "AX,CF"

        mov     ax, 5804h
        int     21h
        call    probe_capture
        EMIT    "int21.5804", "AX,CF"

        push    cs
        pop     es
        mov     ax, 6508h
        mov     bx, 0FFFFh
        mov     dx, 0FFFFh
        mov     cx, 41
        mov     di, cbuf
        int     21h
        call    probe_capture
        EMIT    "int21.6508", "AX,CF"

        ; ---- AH=65h AL=20h: capitalise the character in DL
        mov     ax, 6520h
        mov     dl, 'a'
        int     21h
        call    probe_capture
        call    dlonly
        EMIT    "int21.6520.a", "DX,CF"
        mov     ax, 6520h
        mov     dl, 81h                 ; u-umlaut in CP437 -> 9Ah
        int     21h
        call    probe_capture
        call    dlonly
        EMIT    "int21.6520.81", "DX,CF"
        mov     ax, 6520h
        mov     dl, '1'
        int     21h
        call    probe_capture
        call    dlonly
        EMIT    "int21.6520.digit", "DX,CF"

        ; ---- AL=21h: capitalise CX bytes at DS:DX
        mov     ax, 6521h
        mov     dx, s21
        mov     cx, 6
        int     21h
        call    probe_capture
        EMIT    "int21.6521", "CF"
        EMIT_BUF "int21.6521.buf", s21, 8

        ; ---- AL=22h: capitalise the ASCIIZ string at DS:DX
        mov     ax, 6522h
        mov     dx, s22
        int     21h
        call    probe_capture
        EMIT    "int21.6522", "CF"
        EMIT_BUF "int21.6522.buf", s22, 8

        ; ---- AL=23h: yes/no -> AX=0 no, 1 yes, 2 neither
        mov     ax, 6523h
        mov     dl, 'y'
        mov     dh, 0
        int     21h
        call    probe_capture
        EMIT    "int21.6523.y", "AX,CF"
        mov     ax, 6523h
        mov     dl, 'N'
        mov     dh, 0
        int     21h
        call    probe_capture
        EMIT    "int21.6523.N", "AX,CF"
        mov     ax, 6523h
        mov     dl, 'q'
        mov     dh, 0
        int     21h
        call    probe_capture
        EMIT    "int21.6523.q", "AX,CF"

        ; ---- AH=38h: GET country 44 (UK)
        mov     ax, 382Ch
        mov     dx, cbuf
        int     21h
        call    probe_capture
        EMIT    "int21.382C.get", "AX,CF"   ; BX: DOS leaves it, so it is leftovers

        ; ---- SET country: DX=FFFFh, AL=country (1 = US, 2Ch = 44)
        mov     ax, 3801h
        mov     dx, 0FFFFh
        int     21h
        call    probe_capture
        EMIT    "int21.38.set1", "AX,CF"
        mov     ax, 382Ch
        mov     dx, 0FFFFh
        int     21h
        call    probe_capture
        EMIT    "int21.38.set44", "AX,CF"
        mov     ax, 3801h               ; put the country back
        mov     dx, 0FFFFh
        int     21h

        ; ---- AH=71h: the LONG-FILENAME API (s81). Not a 6.22 function; XP's EDIT.COM
        ; calls 716Ch first and took our "nothing happened" answer as a handle. What
        ; does a DOS without LFN answer? AX and CF are the whole question -- an LFN
        ; client decides "unsupported" from exactly these two.
        ; 716Ch extended open: BX=mode 0 (read), CX=attr 0, DX=1 (open existing),
        ; DS:SI -> name. A file that exists, so a DOS WITH LFN would open it.
        mov     ax, 716Ch
        mov     bx, 0
        xor     cx, cx
        mov     dx, 1
        mov     si, fname
        mov     di, 0
        int     21h
        call    probe_capture
        EMIT    "int21.716C", "AX,CF"
        ; 7147h get current directory: DL=0, DS:SI -> 64-byte buffer
        mov     ax, 7147h
        xor     dl, dl
        mov     si, cbuf
        int     21h
        call    probe_capture
        EMIT    "int21.7147", "AX,CF"

        PROBE_END

; ---- data -----------------------------------------------------------------
brk0    db 0
fname   db "P_SUBFN.COM", 0
s21     db "ab", 81h, "z1x", "##"
s22     db "q", 84h, "r", 0, "zzzz"
cbuf    times 48 db 0
