; p_child.com -- the child half of the EXEC test.  Prints a marker and exits
; with a distinctive code so the parent can prove BOTH that it ran and that its
; exit status came back.
;
; s74: it also reports THE ENVIRONMENT IT WAS GIVEN, because that is where
; Heaven7 died.  DOS/4GW (as a separate DOS4GW.EXE, launched by the stub in
; h7.EXE through INT 21h/4Bh with "inherit the parent's environment") reads the
; program name that DOS appends to the child's environment block -- and got
; `PEC=C:\COMMAND.COM`: our host had SHARED the parent's block instead of
; copying it, and the PSP builder had then zeroed its first three bytes.
; Everything below is a RELATION between the child and its parent, or a
; property of the block's shape, so it grades the same on A: under 6.22 and on
; C:\...\ under NTVDMEX; nothing here names a drive or a path.
;
;   BUF=child.env.copy      [segs differ, first 16 bytes equal]
;   BUF=child.env.count     the word after the double NUL (DOS 3+: 0001)
;   BUF=child.env.nametail  the last 11 bytes of the program name
;   BUF=child.env.namekind  [byte 1 is ':', contains a backslash]
;
; nasm -f bin p_child.asm -o p_child.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        mov     ah, 09h
        mov     dx, msg
        int     21h

        ; --- the two environment segments: ours and the parent's
        mov     ax, [2Ch]
        mov     [cenv], ax
        mov     bx, [16h]                       ; parent PSP
        mov     es, bx
        mov     ax, [es:2Ch]
        mov     [penv], ax

        mov     byte [r_copy], 0
        cmp     ax, [cenv]
        je      .seg_same
        mov     byte [r_copy], 1
.seg_same:
        ; first 16 bytes equal?  DS:SI = child env, ES:DI = parent env
        mov     byte [r_copy + 1], 0
        push    ds
        mov     es, [penv]
        mov     ds, [cenv]
        xor     si, si
        xor     di, di
        mov     cx, 16
        repe    cmpsb
        pop     ds
        jne     .head_differs
        mov     byte [r_copy + 1], 1
.head_differs:

        ; --- walk the child's env to the double NUL, then the count word and
        ;     the program name.  Copy the name into our own segment first: every
        ;     EMIT below is DS-relative and needs DS back on us.
        push    ds
        mov     ds, [cenv]
        xor     si, si
.scan:  lodsb
        test    al, al
        jnz     .scan
        cmp     byte [si], 0
        jne     .scan
        inc     si                              ; SI -> count word
        lodsw
        mov     bx, ax                          ; the count word
        xor     di, di                          ; DI = length of the name so far
.copy:  lodsb
        mov     [cs:name + di], al
        test    al, al
        jz      .copied
        inc     di
        cmp     di, 127
        jb      .copy
.copied:
        pop     ds
        mov     [r_count], bx
        mov     [namelen], di

        ; --- last 11 bytes of the name (whole name, NUL-padded, if shorter)
        mov     cx, 11
        mov     si, name
        mov     ax, di
        cmp     ax, 11
        jb      .short
        sub     ax, 11
        add     si, ax
        jmp     .tail
.short: mov     di, r_tail
        mov     cx, 11
        push    ds
        pop     es
        xor     al, al
        rep     stosb                           ; pad with NULs
        mov     cx, [namelen]
        mov     si, name
.tail:  mov     di, r_tail
        push    ds
        pop     es
        rep     movsb

        ; --- shape: drive-qualified?  contains a backslash?
        mov     byte [r_kind], 0
        cmp     byte [name + 1], ':'
        jne     .nocolon
        mov     byte [r_kind], 1
.nocolon:
        mov     byte [r_kind + 1], 0
        mov     si, name
.bs:    lodsb
        test    al, al
        jz      .nobs
        cmp     al, '\'
        jne     .bs
        mov     byte [r_kind + 1], 1
.nobs:

        EMIT_BUF "child.env.copy",     r_copy,  2
        EMIT_BUF "child.env.count",    r_count, 2
        EMIT_BUF "child.env.nametail", r_tail,  11
        EMIT_BUF "child.env.namekind", r_kind,  2

        mov     ax, 4C2Ah               ; exit code 0x2A -- recognisable
        int     21h

msg     db      '[child ran]', 13, 10, '$'
cenv    dw      0
penv    dw      0
namelen dw      0
r_copy  db      0, 0
r_count dw      0
r_tail  times 11 db 0
r_kind  db      0, 0
name    times 128 db 0
