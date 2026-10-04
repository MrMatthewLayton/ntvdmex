; p_stdin.com -- console input from a REDIRECTED stdin, past end of file.  (stdio item)
;
; `prog < file` is COMMAND.COM opening the file and AH=46h-ing it onto handle 0.
; This does the same itself, so the question can be put to every host -- including
; MS-DOS 6.22, where no harness can type a `<`:
;   a 2-byte file "AB", two AH=08h reads (both real), then past EOF: AH=0Bh, AH=06h
;   DL=FFh, a 4-byte AH=3Fh on handle 0, AH=08h, AH=01h -- the non-blocking ones first.
; Each result is printed as it returns: s91's first cut held them to the end, and on
; MS-DOS 6.22, PCem and stock NTVDM it printed NOTHING -- some read past EOF BLOCKS.
; The open question it settles: what AH=08h hands back once the file is used up
; (a cat-style loop hung under stock AND ours -- neither said what EOF looks like).
; ⚠ Each line is COMMITTED (AH=68h on handle 1) as it is written: an oracle killed at
;   its timeout would otherwise lose the lines still sitting in DOS's disk buffers.
; nasm -f bin p_stdin.asm -o p_stdin.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "stdin"

        ; the input file
        mov     ah, 3Ch
        xor     cx, cx
        mov     dx, fname
        int     21h
        jc      .fail
        mov     bx, ax
        mov     ah, 40h
        mov     cx, 2
        mov     dx, ab
        int     21h
        mov     ah, 3Eh
        int     21h
        ; save stdin, open the file, put it on handle 0
        mov     ah, 45h
        xor     bx, bx
        int     21h
        mov     [saved], ax
        mov     ax, 3D00h
        mov     dx, fname
        int     21h
        jc      .fail
        mov     [fh], ax
        mov     bx, ax
        xor     cx, cx
        mov     ah, 46h
        int     21h

        ; the reads -- each EMITted the moment it returns (stdout is not redirected),
        ; so on a host that BLOCKS the last line printed names the call that did.
%macro ONE 1
        xor     ah, ah
        call    probe_capture
        EMIT    %1, "AX"
        mov     ah, 68h
        mov     bx, 1
        int     21h
%endmacro
        mov     ah, 08h
        int     21h
        ONE     "ah08.1"
        mov     ah, 08h
        int     21h
        ONE     "ah08.2"
        mov     ah, 0Bh                 ; status at EOF, before any blocking read
        int     21h
        ONE     "ah0b.eof"
        mov     ah, 06h
        mov     dl, 0FFh
        int     21h
        lahf
        mov     [flg], ah
        ONE     "ah06.eof.al"
        mov     al, [flg]
        and     al, 40h
        ONE     "ah06.eof.zf"
        mov     ah, 3Fh
        xor     bx, bx
        mov     cx, 4
        mov     dx, rbuf
        int     21h
        sbb     bl, bl
        mov     [flg], bl
        ONE     "ah3f.eof.n"
        mov     al, [flg]
        ONE     "ah3f.eof.cf"
        mov     ah, 08h
        int     21h
        ONE     "ah08.eof1"
        mov     ah, 01h
        int     21h
        ONE     "ah01.eof"

        ; stdin back
        mov     bx, [saved]
        xor     cx, cx
        mov     ah, 46h
        int     21h
        mov     bx, [saved]
        mov     ah, 3Eh
        int     21h
        mov     bx, [fh]
        mov     ah, 3Eh
        int     21h
        mov     ah, 41h
        mov     dx, fname
        int     21h
        jmp     .done
.fail:  mov     ax, 0FFFFh
        call    probe_capture
        EMIT    "setup.failed", "AX"
.done:
        PROBE_END

fname   db      'STDIN.TMP', 0
ab      db      'AB'
saved   dw      0
flg     db      0
fh      dw      0
res     times 16 db 0
rbuf    times 8 db 0
