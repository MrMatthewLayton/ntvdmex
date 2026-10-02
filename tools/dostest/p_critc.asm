; p_critc.com -- the child half of p_crit.asm's ABORT case.  GH #34.
;
; Installs an INT 24h that answers ABORT (2), arms the parent's INT 13h hook through
; the mailbox at 0000:0184 (a far pointer to the parent's `armed` byte), and does a
; find-first on A:. A critical error answered with Abort ENDS this program -- so it
; should never get past the INT 21h; if it does, it exits with 07 to say so.
; Prints nothing: on the oracles stdout is a file on the drive being failed.
;
; nasm -f bin p_critc.asm -o p_critc.com

        org     100h
        mov     ax, 2524h
        mov     dx, h24
        int     21h
        xor     ax, ax
        mov     es, ax
        les     bx, [es:184h]
        mov     byte [es:bx], 1                 ; arm
        mov     ah, 4Eh
        xor     cx, cx
        mov     dx, pfind
        int     21h
        mov     ax, 4C07h                       ; not aborted
        int     21h

h24:    mov     al, 2                           ; ABORT
        iret

pfind   db      'A:\*.*', 0
