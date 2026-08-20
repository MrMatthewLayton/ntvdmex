; p_ctab.com -- INT 21h AH=65h table subfunctions AL=02..07.
;
; ATTRIB.EXE asks for AL=07 and COMMAND.COM for AL=04, so the AL=01 country
; block alone was not enough.  These subfunctions return ES:DI -> a 5-byte
; descriptor: an id byte followed by a FAR POINTER to a character table.
;
; The tables are CP437 character data.  They could be generated from what one
; "knows" about code page 437 -- which is exactly the from-memory guess the
; programme forbids -- so each one is followed to its pointer and dumped from
; the oracle instead.  Feeds GH #38.
;
; nasm -f bin p_ctab.asm -o p_ctab.com

        org     100h
        jmp     start
%include "probe.inc"

; GETTAB <al>, "<case>", <bytes to dump from the table>
%macro GETTAB 3
        ; poison the 5-byte descriptor so we see exactly what DOS fills in
        push    es
        mov     ax, cs
        mov     es, ax
        mov     di, blk
        mov     cx, 8
        mov     al, 0EEh
        cld
        rep     stosb
        mov     di, tab
        mov     cx, 288
        mov     al, 0EEh
        rep     stosb
        pop     es

        POISON
        mov     ax, 6500h | %1
        mov     bx, 0FFFFh
        mov     dx, 0FFFFh
        mov     cx, 5
        push    ds
        pop     es
        mov     di, blk
        int     21h
        call    probe_capture
        EMIT    %2, "AX,CF"
        EMIT_BUF {%2, ".desc"}, blk, 5

        ; follow the far pointer at blk+1 and copy the table locally
        push    ds
        mov     si, [blk + 1]
        mov     ax, [blk + 3]
        mov     ds, ax
        mov     ax, cs
        mov     es, ax
        mov     di, tab
        mov     cx, %3
        cld
        rep     movsb
        pop     ds
        EMIT_BUF {%2, ".tab"}, tab, %3
%endmacro

start:
        PROBE_BEGIN "ctab"

        GETTAB 02h, "t.02.upper",     130
        GETTAB 04h, "t.04.fnupper",   130
        GETTAB 05h, "t.05.fnterm",    24
        GETTAB 06h, "t.06.collate",   258
        GETTAB 07h, "t.07.dbcs",      8

        PROBE_END

blk:
        times 8 db 0
tab:
        times 288 db 0
