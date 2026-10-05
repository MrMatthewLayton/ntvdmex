; p_curdir.com -- AH=47h current directory, per drive, and across an EXEC.  GH #134.
;
; The symptom: COMMAND.COM's `$p` prompt degrades from `C:\dostest>` to `C>`
; AFTER a child process runs, i.e. AH=47h stops answering. The issue records that
; as spotted and NOT diagnosed, with a lead -- AH=47h is unimplemented for any
; drive but the current one, and per-drive current directories are real DOS
; behaviour.
;
; So this asks the two questions that separate those explanations:
;   * does 47h still answer for the CURRENT drive after an EXEC?  (state clobbered)
;   * does 47h answer for an EXPLICIT drive letter at all?        (never implemented)
; A host that fails only the second is failing because COMMAND.COM switches to
; asking explicitly after an EXEC, which is a different bug from losing the path.
;
; It builds and runs its own child (two bytes: INT 20h) so nothing needs staging.
;
; nasm -f bin p_curdir.asm -o p_curdir.com

        org     100h
        jmp     start
%include "probe.inc"

; ask47 -- DL = drive (0 = current, 3 = C:). Path -> DS:SI buffer at `path`.
; DOS writes the path WITHOUT the drive letter or the leading backslash.
ask47:
        mov     byte [path], 0
        mov     byte [path + 1], 0
        push    ds
        pop     es
        mov     si, path
        mov     ah, 47h
        int     21h
        call    probe_capture
        ret

start:
        PROBE_BEGIN "curdir"

        ; ---- SHRINK FIRST. A .COM owns all of memory, so the EXEC below fails
        ; with error 8 without this -- and it fails QUIETLY as far as the cases
        ; after it are concerned: they still run, still pass, and test nothing,
        ; because no child ever ran. The first version of this probe did exactly
        ; that on the oracle (int21.4B00.exec AX=0008 CF=1).
        mov     ax, 4A00h
        mov     bx, 800h
        push    ds
        pop     es
        int     21h
        call    probe_capture
        EMIT    "int21.4A.shrink", "CF"

        ; ---- which drive are we on? Everything below is relative to this.
        POISON
        mov     ah, 19h
        int     21h
        call    probe_capture
        EMIT    "int21.19.curdrive", "AX"
        mov     al, [__ax]
        add     al, 1                           ; 0=A -> 1=A for 47h's DL
        mov     [curdl], al

        ; ---- make a directory and enter it, so there is a path to lose.
        mov     ah, 39h
        mov     dx, dname
        int     21h                             ; may already exist; ignored
        mov     ah, 3Bh
        mov     dx, dname
        int     21h
        call    probe_capture
        EMIT    "int21.3B.chdir", "CF"

        ; ---- BEFORE the EXEC: the current drive, then the SAME drive named
        ; explicitly. Real DOS answers both identically.
        mov     dl, 0
        call    ask47
        EMIT    "int21.47.before.dl0", "CF"
        EMIT_BUF "curdir.before.dl0", path, 10h

        mov     dl, [curdl]
        call    ask47
        EMIT    "int21.47.before.explicit", "CF"
        EMIT_BUF "curdir.before.explicit", path, 10h

        ; ---- write and run a child that does nothing but exit.
        mov     ax, 3C00h
        mov     cx, 0
        mov     dx, cname
        int     21h
        mov     bx, ax
        mov     ax, 4000h
        mov     cx, 2
        mov     dx, cimg
        int     21h
        mov     ax, 3E00h
        int     21h

        mov     word [pb + 0], 0
        mov     word [pb + 2], tailz
        mov     [pb + 4], ds
        mov     word [pb + 6], 5Ch
        mov     [pb + 8], ds
        mov     word [pb + 10], 6Ch
        mov     [pb + 12], ds
        push    ds
        pop     es
        mov     bx, pb
        mov     dx, cname
        mov     ax, 4B00h
        int     21h
        call    probe_capture
        EMIT    "int21.4B00.exec", "CF"

        ; ---- ★ AFTER the EXEC. This is the case #134 is about.
        mov     dl, 0
        call    ask47
        EMIT    "int21.47.after.dl0", "CF"
        EMIT_BUF "curdir.after.dl0", path, 10h

        mov     dl, [curdl]
        call    ask47
        EMIT    "int21.47.after.explicit", "CF"
        EMIT_BUF "curdir.after.explicit", path, 10h

        ; ---- and a drive that does not exist, so "answers everything" and
        ; "answers correctly" cannot be confused.
        mov     dl, 25                          ; Y:
        call    ask47
        EMIT    "int21.47.baddrive", "AX,CF"

        ; ---- clean up: back up, remove the directory and the child.
        mov     ah, 3Bh
        mov     dx, dotdot
        int     21h
        mov     ah, 3Ah
        mov     dx, dname
        int     21h
        mov     ax, 4100h
        mov     dx, cname
        int     21h

        PROBE_END

dname    db 'ZZCD', 0
dotdot   db '..', 0
cname    db 'ZZCHILD.COM', 0
tailz    db 0, 0Dh
curdl    db 0
pb       times 14 db 0
cimg     db 0CDh, 20h                           ; INT 20h -- exit immediately
path     times 80 db 0
