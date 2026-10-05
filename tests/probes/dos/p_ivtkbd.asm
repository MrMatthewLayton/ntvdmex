; p_ivtkbd.com -- what the keyboard BIOS's side-calls land on. GH #254.
;
; The BIOS INT 09h calls INT 1Bh (Ctrl-Break), INT 05h (Print Screen) and INT 15h
; AH=85h (SysReq). Before our INT 09h makes those calls it has to know what the
; vectors hold on a fresh VDM: the VDM's own ROM leaves many vectors pointing into
; F000, and a ROM handler that is an NTVDM BOP would end the run. This DUMPS the
; vectors and the first 8 bytes at each target -- a per-host fact, not a contract.
;
; nasm -f bin p_ivtkbd.asm -o p_ivtkbd.com
        org     100h
        jmp     start
%include "probe.inc"

%macro VEC 2                                    ; vector, name
        xor     ax, ax
        mov     es, ax
        mov     si, [es:%1*4]
        mov     ax, [es:%1*4+2]
        mov     [vbuf], si
        mov     [vbuf+2], ax
        push    ds
        mov     ds, ax
        mov     di, vbuf+4
        push    cs
        pop     es
        mov     cx, 8
        rep     movsb
        pop     ds
        EMIT_BUF %2, vbuf, 12
%endmacro

start:
        PROBE_BEGIN "ivtkbd"
        cld
        VEC     05h, "ivt.05"
        VEC     1Bh, "ivt.1b"
        VEC     15h, "ivt.15"
        VEC     23h, "ivt.23"
        PROBE_END

vbuf    times 12 db 0
