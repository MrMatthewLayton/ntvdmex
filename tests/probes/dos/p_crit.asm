; p_crit.com -- the INT 24h critical-error contract, for a drive that is NOT READY.
; ORACLE-ALSO: pcem
;
; GH #34. When a disk call fails for a hardware reason, DOS does not just return an
; error: it calls the program's INT 24h handler with AH = what was being done and
; what the handler may answer, AL = the drive, DI = the error, and does what the
; handler says -- Retry (re-issue), Fail (return an error), Ignore, or Abort (end the
; program). That is the "Not ready reading drive A / Abort, Retry, Fail?" prompt.
;
; HOW "NOT READY" IS MADE THE SAME ON EVERY HOST:
;   * on the 6.22 oracles A: is the boot floppy, so the probe HOOKS INT 13h and, only
;     while armed, answers drive 0 with "changed" (AH=16h) and "timeout" (everything
;     else) -- DOS's own driver then reports drive-not-ready through INT 24h;
;   * on the test machine A: is a real, EMPTY floppy drive: Win32 says ERROR_NOT_READY, and the
;     hook is inert because NTVDMEX's file I/O never goes through INT 13h.
;   DOSBox-X does not reach a BIOS disk for its mounts and has no A: -- abstained.
; ⚠ NOTHING IS PRINTED WHILE ARMED: on the oracles stdout is a file on A: itself.
;   Results are kept in memory and emitted after the hook is disarmed.
; ⚠ INT 13h IS PUT BACK on every path. INT 24h is put back by DOS (it lives in the PSP).
;
; nasm -f bin p_crit.asm -o p_crit.com       (child: p_critc.asm -> p_critc.com)

        org     100h
        jmp     start
%include "probe.inc"

; ---- INT 13h: fail drive 0 while armed -------------------------------------------
hook13:
        cmp     byte [cs:armed], 0
        je      .chain
        cmp     dl, 0
        jne     .chain
        inc     word [cs:n13]
        cmp     ah, 16h
        jne     .timeout
        mov     ah, 06h                         ; change line: "the disk was changed"
        stc
        retf    2
.timeout:
        mov     ah, 80h                         ; timeout = drive not ready
        stc
        retf    2
.chain: jmp     far [cs:old13]

; ---- INT 24h: record the first call, answer act1 then act2 ----------------------
hook24:
        push    ds
        push    bx
        push    es
        push    cs
        pop     ds
        inc     word [n24]
        cmp     word [n24], 1
        jne     .later
        mov     [r24ax], ax
        mov     [r24di], di
        les     bx, [sda]
        mov     bl, [es:bx]                     ; SDA+0: the critical-error flag
        mov     [r24em], bl
        les     bx, [sda]
        mov     bl, [es:bx + 1]                 ; SDA+1: InDOS
        mov     [r24id], bl
        mov     al, [act1]
        jmp     .out
.later: mov     al, [act2]
.out:   pop     es
        pop     bx
        pop     ds
        iret

; arm, then run the call at [callp]; CF/AX of it land in c_ax / c_fl
%macro PROVOKE 3                                 ; case, act1, act2
        mov     byte [act1], %2
        mov     byte [act2], %3
        mov     word [n24], 0
        mov     word [n13], 0
        mov     word [r24ax], 0EEEEh
        mov     word [r24di], 0EEEEh
        mov     byte [r24em], 0EEh
        mov     byte [r24id], 0EEh
        mov     ah, 0Dh                          ; flush first: nothing dirty on A:
        int     21h
        mov     byte [armed], 1
        call    [callp]
        pushf
        mov     byte [armed], 0
        mov     [c_ax], ax
        pop     ax
        mov     [c_fl], ax
        ; 59h straight away, before any output can disturb it
        POISON
        mov     ax, 5900h
        xor     bx, bx
        int     21h
        mov     [e_ax], ax
        mov     [e_bx], bx
        mov     [e_cx], cx
        ; ---- report
        mov     ax, [c_ax]
        mov     [__ax], ax
        mov     ax, [c_fl]
        mov     [__fl], ax
        %strcat %%cn_call %1, ".call"
        EMIT    %%cn_call, "AX,CF"
        mov     ax, [n24]
        mov     [__ax], ax
        mov     ax, [r24ax]
        mov     [__bx], ax
        mov     ax, [r24di]
        and     ax, 00FFh                        ; DI's high byte is undefined
        mov     [__cx], ax
        %strcat %%cn_int24 %1, ".int24"
        EMIT    %%cn_int24, "AX,BX,CX"       ; count / AH:AL / DI low byte
        mov     al, [r24em]
        mov     ah, [r24id]
        mov     [__ax], ax
        %strcat %%cn_sda %1, ".sda"
        EMIT    %%cn_sda, "AX"               ; InDOS:crit-flag inside the handler
        mov     ax, [e_ax]
        mov     [__ax], ax
        mov     ax, [e_bx]
        mov     [__bx], ax
        mov     ax, [e_cx]
        and     ax, 0FF00h                       ; CH = locus; CL is not written
        mov     [__cx], ax
        %strcat %%cn_59 %1, ".59"
        EMIT    %%cn_59, "AX,BX,CX"
%endmacro

do4e:   POISON
        mov     ah, 4Eh
        xor     cx, cx
        mov     dx, pfind
        int     21h
        ret

do3d:   POISON
        mov     ax, 3D00h
        mov     dx, popen
        int     21h
        ret

do3c:   POISON
        mov     ah, 3Ch
        xor     cx, cx
        mov     dx, pcreat
        int     21h
        ret

start:
        PROBE_BEGIN "crit"

        ; the SDA, so the handler can read the crit flag and InDOS
        push    ds
        mov     ax, 5D06h
        int     21h
        mov     ax, ds
        pop     ds
        mov     [sda], si
        mov     [sda + 2], ax

        ; hook INT 13h (inert unless armed) and INT 24h
        mov     ax, 3513h
        int     21h
        mov     [old13], bx
        mov     [old13 + 2], es
        push    ds
        pop     es
        mov     ax, 2513h
        mov     dx, hook13
        int     21h
        mov     ax, 2524h
        mov     dx, hook24
        int     21h

        ; ---- find-first on A:, handler answers FAIL (3)
        mov     word [callp], do4e
        PROVOKE "crit.4e.fail", 3, 3
        ; ---- the same, RETRY once then FAIL: the handler must be called twice
        mov     word [callp], do4e
        PROVOKE "crit.4e.retry", 1, 3
        ; ---- IGNORE (0)
        mov     word [callp], do4e
        PROVOKE "crit.4e.ignore", 0, 3
        ; ---- open for read, FAIL
        mov     word [callp], do3d
        PROVOKE "crit.3d.fail", 3, 3
        ; ---- create (a write), FAIL
        mov     word [callp], do3c
        PROVOKE "crit.3c.fail", 3, 3

        ; ---- ABORT, in a child (it ends the program that was running): the parent
        ;      keeps the INT 13h hook, the child installs an INT 24h that answers 2.
        mov     ah, 4Ah                          ; make room for the child
        mov     bx, 1000h
        push    cs
        pop     es
        int     21h
        mov     ax, cs
        mov     [pb_tail + 2], ax
        mov     [pb_fcb1 + 2], ax
        mov     [pb_fcb2 + 2], ax
        mov     ah, 0Dh
        int     21h
        ; the CHILD arms the hook, through a mailbox: it must be LOADED from A: first,
        ; and A: is "not ready" while armed. 0000:0184 (INT 61h, a user vector) holds
        ; a far pointer to `armed` for the child's duration.
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:184h]
        mov     [sv61], ax
        mov     ax, [es:186h]
        mov     [sv61 + 2], ax
        mov     word [es:184h], armed
        mov     [es:186h], cs
        push    cs
        pop     es
        mov     bx, pblock
        mov     dx, child
        mov     ax, 4B00h
        int     21h
        pushf
        push    cs
        pop     ds                               ; (DOS 2 lost DS across EXEC; be safe)
        mov     byte [armed], 0
        push    ax
        push    es
        xor     ax, ax
        mov     es, ax
        mov     ax, [sv61]
        mov     [es:184h], ax
        mov     ax, [sv61 + 2]
        mov     [es:186h], ax
        pop     es
        pop     ax
        mov     [c_ax], ax
        pop     ax
        mov     [c_fl], ax
        mov     ax, [c_ax]
        mov     [__ax], ax
        mov     ax, [c_fl]
        mov     [__fl], ax
        EMIT    "crit.abort.exec", "CF"
        POISON
        mov     ax, 4D00h
        int     21h
        call    probe_capture
        EMIT    "crit.abort.4d", "AX"            ; AH=02: ended by a critical error

        ; ---- put INT 13h back
        push    ds
        lds     dx, [old13]
        mov     ax, 2513h
        int     21h
        pop     ds

        PROBE_END

pfind   db      'A:\*.*', 0
popen   db      'A:\ZZCRIT.TXT', 0
pcreat  db      'A:\ZZCRIT2.TXT', 0
child   db      'P_CRITC.COM', 0
tail    db      0, 0Dh
fcb     times 16 db 0
pblock  dw      0
pb_tail dw      tail, 0
pb_fcb1 dw      fcb, 0
pb_fcb2 dw      fcb, 0
old13   dw      0, 0
sv61    dw      0, 0
sda     dw      0, 0
callp   dw      0
armed   db      0
act1    db      0
act2    db      0
n24     dw      0
n13     dw      0
r24ax   dw      0
r24di   dw      0
r24em   db      0
r24id   db      0
c_ax    dw      0
c_fl    dw      0
e_ax    dw      0
e_bx    dw      0
e_cx    dw      0
