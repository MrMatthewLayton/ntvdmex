; p_child.com -- the child half of the EXEC test.  Prints a marker and exits
; with a distinctive code so the parent can prove BOTH that it ran and that its
; exit status came back.
;
; nasm -f bin p_child.asm -o p_child.com

        org     100h
start:
        mov     ah, 09h
        mov     dx, msg
        int     21h
        mov     ax, 4C2Ah               ; exit code 0x2A -- recognisable
        int     21h
msg     db      '[child ran]', 13, 10, '$'
