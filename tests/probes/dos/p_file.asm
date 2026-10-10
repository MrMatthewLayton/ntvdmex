; p_file.com -- the file/handle/attribute batch: 43h, 57h, 45h, 46h, 54h, 34h,
; 4Dh, 5Ch, 67h, 6Ch, 5Bh, 41h, 39h, 3Ah, 56h.
;
; SELF-CONTAINED AND SELF-CLEANING: it creates its own file and directory, works
; on those, and removes them.  It therefore leaves no trace on the test machine or under
; DOSBox, where there is no snapshot to roll back -- the oracle's copy-on-write
; disk protects only the oracle.
;
; nasm -f bin p_file.asm -o p_file.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "file"

        ; ---- 3Ch: create the scratch file, write 4 bytes, close
        mov     ax, 3C00h
        mov     cx, 0
        mov     dx, fname
        int     21h
        mov     [fh], ax

        mov     bx, [fh]
        mov     ax, 4000h
        mov     cx, 4
        mov     dx, fname
        int     21h

        ; ---- 45h: duplicate the handle
        POISON
        mov     bx, [fh]
        mov     ax, 4500h
        int     21h
        call    probe_capture
        mov     [fh2], ax
        EMIT    "int21.45.dup", "CF"

        ; ---- 3Eh: close the duplicate, then the original
        mov     bx, [fh2]
        mov     ax, 3E00h
        int     21h
        mov     bx, [fh]
        mov     ax, 3E00h
        int     21h

        ; ---- 4300h: get the file's attributes
        POISON
        mov     ax, 4300h
        mov     dx, fname
        int     21h
        call    probe_capture
        EMIT    "int21.4300.get", "CX,CF"

        ; ---- 4300h on a file that does not exist
        POISON
        mov     ax, 4300h
        mov     dx, nofile
        int     21h
        call    probe_capture
        EMIT    "int21.4300.missing", "AX,CF"

        ; ---- 5Bh: create-new on a file that ALREADY exists -> must fail
        POISON
        mov     ax, 5B00h
        mov     cx, 0
        mov     dx, fname
        int     21h
        call    probe_capture
        EMIT    "int21.5B.exists", "AX,CF"

        ; ---- 57h: get the file's date and time via a fresh handle
        mov     ax, 3D00h
        mov     dx, fname
        int     21h
        mov     [fh], ax

        POISON
        mov     bx, [fh]
        mov     ax, 5700h
        int     21h
        call    probe_capture
        EMIT    "int21.5700.getdate", "CF"

        ; ---- 5701h then 5700h: a stamp we CHOSE, so the answer is comparable (#168).
        ; The file's own stamp is "now" on every host and can only be compared by CF;
        ; setting a fixed one, closing (DOS writes the entry at close) and reading it
        ; back through a fresh handle makes CX/DX a contract: 12:34:56 on 2001-09-17.
        mov     bx, [fh]
        mov     ax, 5701h
        mov     cx, STAMP_T
        mov     dx, STAMP_D
        int     21h
        mov     bx, [fh]
        mov     ax, 3E00h
        int     21h
        mov     ax, 3D00h
        mov     dx, fname
        int     21h
        mov     [fh], ax

        POISON
        mov     bx, [fh]
        mov     ax, 5700h
        int     21h
        call    probe_capture
        EMIT    "int21.5700.stamp", "CX,DX,CF"

        ; ---- 6800h: commit the file (flush)
        POISON
        mov     bx, [fh]
        mov     ax, 6800h
        int     21h
        call    probe_capture
        EMIT    "int21.68.commit", "CF"

        mov     bx, [fh]
        mov     ax, 3E00h
        int     21h

        ; ---- 5400h: get the verify flag
        POISON
        mov     ax, 5400h
        int     21h
        call    probe_capture
        EMIT    "int21.5400.verify", "AX,CF"

        ; ---- 3400h: get the InDOS flag address -> ES:BX.  The ADDRESS is
        ; host-specific, so only CF is compared; the pointer is proof the call
        ; is answered at all.
        POISON
        mov     ax, 3400h
        int     21h
        call    probe_capture
        EMIT    "int21.34.indos", "CF"

        ; ---- 34h again, and READ THE BYTES it points at (#168). Called from a program
        ; (not from inside DOS) the InDOS flag is 0, and on DOS 3.1+ the critical-error
        ; flag sits in the byte before it, also 0. A pointer that answers but points at
        ; junk is the failure the CF-only row above cannot see. The two MOVs touch no
        ; flags, so capturing after them still reports the call's CF.
        POISON
        mov     ax, 3400h
        int     21h
        mov     al, [es:bx]
        mov     ah, [es:bx-1]
        call    probe_capture
        EMIT    "int21.34.flags", "AX,CF"

        ; ---- 4Dh: exit code of the last child.  No child has run, so this is
        ; "what does DOS say when there is nothing to report".
        POISON
        mov     ax, 4D00h
        int     21h
        call    probe_capture
        EMIT    "int21.4D.noexit", "AX,CF"

        ; ---- 6700h: raise the handle limit to 30
        POISON
        mov     ax, 6700h
        mov     bx, 30
        int     21h
        call    probe_capture
        ; SIG is empty on purpose: whether raising the handle limit SUCCEEDS
        ; depends on the host's free memory at that instant (the oracle failed
        ; with AX=8 asking for 30), so it is not a fact about DOS. Dumped for the
        ; log, not compared.
        EMIT    "int21.67.handles", ""

        ; ---- 46h: force-duplicate. The header always claimed this; nothing emitted it
        ; (#168). CF alone says the call was answered; the ALIAS is the contract -- after
        ; 46h, handle B is the same open file as A, so moving A's position moves B's.
        mov     ax, 3D00h
        mov     dx, fname
        int     21h
        mov     [fh], ax
        mov     ax, 3D00h
        mov     dx, fname
        int     21h
        mov     [fh2], ax

        POISON
        mov     bx, [fh]
        mov     cx, [fh2]
        mov     ax, 4600h
        int     21h
        call    probe_capture
        EMIT    "int21.46.force", "CF"

        mov     bx, [fh]                        ; A -> offset 2
        mov     ax, 4200h
        xor     cx, cx
        mov     dx, 2
        int     21h
        POISON
        mov     bx, [fh2]                       ; where is B? 2 if it IS A, 0 if not
        mov     ax, 4201h
        xor     cx, cx
        xor     dx, dx
        int     21h
        call    probe_capture
        EMIT    "int21.46.alias", "AX,DX,CF"

        POISON
        mov     bx, 0FEh                        ; not an open handle
        mov     cx, [fh2]
        mov     ax, 4600h
        int     21h
        call    probe_capture
        EMIT    "int21.46.badhandle", "AX,CF"

        mov     bx, [fh2]
        mov     ax, 3E00h
        int     21h
        mov     bx, [fh]
        mov     ax, 3E00h
        int     21h

        ; ---- 5Ch: lock and unlock bytes 0-3 (#168). Without SHARE loaded, 6.22 has
        ; no lock table; what it answers then is exactly the question.
        mov     ax, 3D02h
        mov     dx, fname
        int     21h
        mov     [fh], ax

        POISON
        mov     bx, [fh]
        mov     ax, 5C00h
        xor     cx, cx
        xor     dx, dx
        xor     si, si
        mov     di, 4
        int     21h
        call    probe_capture
        EMIT    "int21.5C.lock", "AX,CF"

        POISON
        mov     bx, [fh]
        mov     ax, 5C01h
        xor     cx, cx
        xor     dx, dx
        xor     si, si
        mov     di, 4
        int     21h
        call    probe_capture
        EMIT    "int21.5C.unlock", "AX,CF"

        mov     bx, [fh]
        mov     ax, 3E00h
        int     21h

        ; ---- 6Ch: extended open/create (#168). CX on success is the ACTION TAKEN
        ; (1 opened, 2 created, 3 replaced) -- the part a naive 3Dh wrapper gets wrong.
        ; AX is a handle number, host-specific, so it is compared only on failure.
        POISON
        mov     ax, 6C00h
        xor     bx, bx                          ; read-only, compatibility sharing
        xor     cx, cx
        mov     dx, 0001h                       ; exists: open; missing: fail
        mov     si, fname
        int     21h
        call    probe_capture
        mov     [fh], ax
        EMIT    "int21.6C.open", "CX,CF"
        mov     bx, [fh]
        mov     ax, 3E00h
        int     21h

        POISON
        mov     ax, 6C00h
        xor     bx, bx
        xor     cx, cx
        mov     dx, 0010h                       ; exists: FAIL; missing: create
        mov     si, fname
        int     21h
        call    probe_capture
        EMIT    "int21.6C.exists", "AX,CF"

        POISON
        mov     ax, 6C00h
        xor     bx, bx
        xor     cx, cx
        mov     dx, 0001h
        mov     si, nofile
        int     21h
        call    probe_capture
        EMIT    "int21.6C.missing", "AX,CF"

        ; ---- 39h/3Ah: make and remove a directory
        POISON
        mov     ax, 3900h
        mov     dx, dname
        int     21h
        call    probe_capture
        EMIT    "int21.39.mkdir", "CF"

        POISON
        mov     ax, 3900h                       ; again: it now exists
        mov     dx, dname
        int     21h
        call    probe_capture
        EMIT    "int21.39.exists", "AX,CF"

        POISON
        mov     ax, 3A00h
        mov     dx, dname
        int     21h
        call    probe_capture
        EMIT    "int21.3A.rmdir", "CF"

        POISON
        mov     ax, 3A00h                       ; again: it is gone
        mov     dx, dname
        int     21h
        call    probe_capture
        EMIT    "int21.3A.missing", "AX,CF"

        ; ---- 56h: rename, then rename back
        POISON
        mov     ax, 5600h
        mov     dx, fname
        push    ds
        pop     es
        mov     di, fname2
        int     21h
        call    probe_capture
        EMIT    "int21.56.rename", "CF"

        mov     ax, 5600h
        mov     dx, fname2
        push    ds
        pop     es
        mov     di, fname
        int     21h

        ; ---- 41h: delete the scratch file, then delete it again
        POISON
        mov     ax, 4100h
        mov     dx, fname
        int     21h
        call    probe_capture
        EMIT    "int21.41.delete", "CF"

        POISON
        mov     ax, 4100h
        mov     dx, fname
        int     21h
        call    probe_capture
        EMIT    "int21.41.missing", "AX,CF"

        PROBE_END

STAMP_T equ (12 << 11) | (34 << 5) | (56 / 2)
STAMP_D equ ((2001 - 1980) << 9) | (9 << 5) | 17

fname   db 'PZTEST.TMP', 0
fname2  db 'PZTEST2.TMP', 0
dname   db 'PZTESTD', 0
nofile  db 'ZZNOSUCH.XYZ', 0
fh      dw 0
fh2     dw 0
