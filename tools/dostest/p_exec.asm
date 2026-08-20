; p_exec.com -- INT 21h AH=4Bh EXEC.  GH #30.
;
; Runs P_CHILD.COM and then checks the three things that make EXEC actually
; work, as opposed to merely not crashing:
;   1. the child RAN            -- its marker appears in the output
;   2. the parent RESUMED       -- these lines print at all
;   3. the exit code CAME BACK  -- AH=4Dh reports the child's 0x2A
;
; The child is expected to be in the same directory as this program.  The path
; is deliberately relative so the same binary works on the oracle's A:, on the
; rig's C:\test and under DOSBox's mount.
;
; nasm -f bin p_exec.asm -o p_exec.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "exec"

        ; A .COM is given ALL of free memory, so there is none left for a child:
        ; EXEC returns AX=0008 until the parent gives some back. Shrinking first
        ; is what every real shell does, not a workaround -- COMMAND.COM does
        ; exactly this before it launches anything.
        mov     ax, cs
        mov     es, ax
        mov     bx, 1000h                       ; keep 64 KB for ourselves
        mov     ah, 4Ah
        int     21h

        ; The parameter block wants a far pointer to the command tail and to two
        ; FCBs; a bare CR is a valid empty tail.
        mov     ax, cs
        mov     [pb_tail + 2], ax
        mov     [pb_fcb1 + 2], ax
        mov     [pb_fcb2 + 2], ax

        POISON
        push    ds
        pop     es
        mov     bx, pblock
        mov     dx, child
        mov     ax, 4B00h
        int     21h
        call    probe_capture
        EMIT    "int21.4B.exec", "CF"

        ; ---- 4Dh: the child's exit code should be here now.
        POISON
        mov     ax, 4D00h
        int     21h
        call    probe_capture
        EMIT    "int21.4D.childcode", "AX,CF"

        ; ---- and EXEC on something that does not exist -> AX=2
        POISON
        push    ds
        pop     es
        mov     bx, pblock
        mov     dx, nofile
        mov     ax, 4B00h
        int     21h
        call    probe_capture
        EMIT    "int21.4B.missing", "AX,CF"

        PROBE_END

child    db 'P_CHILD.COM', 0
nofile   db 'ZZNOEXEC.COM', 0
pblock:
         dw 0                           ; inherit the parent's environment
pb_tail  dw tail, 0
pb_fcb1  dw dfcb, 0
pb_fcb2  dw dfcb, 0
tail     db 0, 13
dfcb     times 16 db 0
