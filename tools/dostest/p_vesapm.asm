; p_vesapm.com -- VBE function 4F0Ah, the protected-mode interface.  GH #53.
;
; 4F0Ah is how a protected-mode client switches banks without going back to real
; mode: the card hands back a small block of its OWN code plus a table of offsets
; into it, and the client copies that block and calls it directly.
;
; ── WHY THIS IS MEASURED AND NOT DESIGNED ────────────────────────────────────
; There is no MS-DOS ground truth for a VESA BIOS -- it is the card's firmware,
; not DOS's. But the answer is not therefore free to invent: the shape of the
; table, which entries are provided, and the calling convention of the code are
; all things a real client is written against. So this dumps what a REAL VBE
; implementation returns (the VGA BIOS under QEMU) and we build to that shape
; rather than to a plausible reading of the spec.
;
; ⚠ The oracle here is a VGA BIOS, NOT MS-DOS 6.22 -- epic #24's rule that the
;   6.22 kernel is definitive does not apply to BIOS answers, and this file is
;   evidence about what VBE implementations do, not about what DOS does.
;
; The returned table (VBE 2.0):
;   +0  WORD  offset to Set Window
;   +2  WORD  offset to Set Display Start
;   +4  WORD  offset to Set Primary Palette Data (0 = not provided)
;   +6  WORD  offset to a list of ports/memory the client needs (0 = none)
; CX = the length of the whole block, code included.
;
; nasm -f bin p_vesapm.asm -o p_vesapm.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "vesapm"

        ; ---- is there a VBE at all? 4F00 fills a 256-byte block at ES:DI and
        ; returns AX=004F. Without this the 4F0A answer below means nothing.
        push    ds
        pop     es
        mov     di, infob
        mov     ax, 4F00h
        int     10h
        call    probe_capture
        EMIT    "int10.4F00.vbe.present", "AX"
        EMIT_BUF "vesapm.sig", infob, 8         ; 'VESA' + version word

        ; ---- ★ 4F0A BL=00: return the protected-mode interface.
        ; AX=004F means supported. ES:DI is the block and CX its length; both are
        ; card addresses, so neither is in the signature -- what travels between
        ; hosts is AX, the LENGTH, and the shape of the table.
        POISON
        mov     ax, 4F0Ah
        mov     bl, 0
        mov     di, 0
        mov     ax, 4F0Ah
        int     10h
        call    probe_capture
        EMIT    "int10.4F0A.pm.iface", "AX,CX"

        ; ⚠ GUARD ON AX BEFORE FOLLOWING ES:DI. The first run of this probe did
        ; not, and QEMU's VGA BIOS answers 4F0A with AX=0100 CF=1 (not supported)
        ; leaving ES:DI untouched -- so the dump below faithfully reported OUR OWN
        ; PSP as if it were the card's table (CD20C09F..., the INT 20h at PSP+0).
        ; A pointer that was never set is not a pointer.
        cmp     word [__ax], 004Fh
        je      .have
        EMIT_BUF "vesapm.table", zeros, 8
        EMIT_BUF "vesapm.setwindow.code", zeros, 8
        jmp     .done
.have:
        ; ---- copy the table out of the card's space so it can be dumped.
        ; The four words are OFFSETS INTO THE RETURNED BLOCK, so they are
        ; comparable across hosts in a way the block's address is not.
        cld
        push    ds
        pop     es
        mov     di, tbl
        mov     cx, 8
        push    ds
        mov     ds, [cs:__es]
        mov     si, [cs:__di]
        rep     movsb
        pop     ds
        EMIT_BUF "vesapm.table", tbl, 8

        ; ---- and the first bytes of the Set Window routine itself, which is
        ; what the client actually executes. Its shape says how a bank switch is
        ; expected to be performed -- port writes, and which ports.
        push    ds
        mov     ds, [cs:__es]
        mov     si, [cs:__di]
        mov     ax, [si]                        ; offset to Set Window
        add     si, ax
        push    cs
        pop     es
        mov     di, code
        mov     cx, 20h
        cld
        rep     movsb
        pop     ds
        EMIT_BUF "vesapm.setwindow.code", code, 20h
.done:

        PROBE_END

zeros    times 8 db 0
infob    times 8 db 0
tbl      times 8 db 0
code     times 20h db 0
