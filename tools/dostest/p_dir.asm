; p_dir.com -- INT 21h 47h (get current directory), 3Bh (chdir), 36h (free space).
;
; Ranked by the evidence pass: running the real 6.22 tools under NTVDMEX and
; reading the STAGE2 to-do lists, AH=47h was wanted by FOUR of five programs
; (TREE, ATTRIB, XCOPY, COMMAND.COM).  Feeds GH #32 and #35.
;
; 3Bh is probed with a path that is expected to FAIL, so the probe changes no
; state -- it runs on the rig and under DOSBox too, where nothing rolls back.
;
; nasm -f bin p_dir.asm -o p_dir.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "dir"

        ; ---- 47h: get current directory of the default drive (DL=0) into DS:SI.
        ; Poison the buffer so "DOS wrote a NUL" and "DOS wrote nothing" differ:
        ; at the root the answer is an EMPTY string, which is otherwise
        ; indistinguishable from the call having done nothing at all.
        mov     di, cwdbuf
        mov     cx, 64
        mov     al, 0EEh
        push    es
        mov     bx, cs
        mov     es, bx
        cld
        rep     stosb
        pop     es

        POISON
        mov     ax, 4700h
        mov     dl, 0
        mov     si, cwdbuf
        int     21h
        call    probe_capture
        ; AX is NOT compared: RBIL documents it as destroyed by 47h, and the
        ; hosts duly differ (0100 vs 0000) once they run from different
        ; directories. Only CF is a fact about the call.
        EMIT    "int21.4700", "CF"
        EMIT_BUF "cwd", cwdbuf, 16

        ; ---- 36h: free space on the default drive (DL=0).
        ; AX=sectors/cluster BX=free clusters CX=bytes/sector DX=total clusters.
        ; ONLY CX IS SIGNIFICANT.  Free and total cluster counts obviously vary
        ; per disk -- but so does AX (sectors per cluster): the oracle's 504 MB
        ; FAT16 volume gives 16, DOSBox gives 64, our rig gives 8.  Declaring AX
        ; significant produced a DISPUTED row that was pure volume geometry, not
        ; a difference in DOS behaviour.  Bytes-per-sector is 512 everywhere.
        POISON
        mov     ax, 3600h
        mov     dl, 0
        int     21h
        call    probe_capture
        EMIT    "int21.3600", "CX,CF"

        ; ---- 36h on a drive that does not exist -> AX=FFFF
        POISON
        mov     ax, 3600h
        mov     dl, 26                          ; drive Z:
        int     21h
        call    probe_capture
        EMIT    "int21.3600.baddrive", "AX,CF"

        ; ---- 3Bh: chdir to a directory that does not exist -> error
        POISON
        mov     ax, 3B00h
        mov     dx, nodir
        int     21h
        call    probe_capture
        EMIT    "int21.3B.missing", "AX,CF"

        PROBE_END

nodir   db '\NO_SUCH_DIR_XYZ', 0
cwdbuf:
        times 64 db 0
