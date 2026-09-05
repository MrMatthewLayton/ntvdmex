; p_err.com -- INT 21h AH=59h, get extended error, for each error we can provoke.
;
; 59h reports a failure as four things, not one: the extended code (AX), an error
; CLASS (BH), a suggested ACTION (BL) and a LOCUS (CH).  Only the code is
; obvious; class/action/locus for a given error are exactly the sort of value
; that gets written from memory and is wrong.  So each case provokes a specific,
; harmless failure and then asks DOS what it made of it.
;
; CL is deliberately included in one case: it came back still poisoned on the
; first run, i.e. DOS does not write it, and that is worth pinning down rather
; than assuming.
;
; Feeds GH #34.
;
; nasm -f bin p_err.asm -o p_err.com

        org     100h
        jmp     start
%include "probe.inc"

; PROVOKE runs a call expected to fail, then reads 59h and reports it.
%macro ASKERR 1
        POISON
        mov     ax, 5900h
        mov     bx, 0
        int     21h
        call    probe_capture
        EMIT    %1, "AX,BX,CX"
%endmacro

start:
        PROBE_BEGIN "err"

        ; ---- find-first on a pattern that matches nothing -> 18
        mov     ax, 4E00h
        mov     cx, 0
        mov     dx, nofile
        int     21h
        ASKERR  "err.after.4E.nofile"

        ; ---- find-first into a missing directory -> 3
        mov     ax, 4E00h
        mov     cx, 0
        mov     dx, nopath
        int     21h
        ASKERR  "err.after.4E.nopath"

        ; ---- open a file that does not exist -> 2
        mov     ax, 3D00h
        mov     dx, nofile
        int     21h
        ASKERR  "err.after.3D.missing"

        ; ---- chdir to a directory that does not exist
        mov     ax, 3B00h
        mov     dx, nopath2
        int     21h
        ASKERR  "err.after.3B.missing"

        ; ---- read from a handle that was never opened -> 6
        mov     ax, 3F00h
        mov     bx, 20                          ; never opened
        mov     cx, 1
        mov     dx, scratch
        int     21h
        ASKERR  "err.after.3F.badhandle"

        ; ---- close a handle that was never opened
        mov     ax, 3E00h
        mov     bx, 20
        int     21h
        ASKERR  "err.after.3E.badhandle"

        ; ---- ACCESS DENIED (5). Provoked properly rather than guessed: create a
        ; scratch file, mark it read-only with 4301h, then ask to OPEN IT FOR
        ; WRITING. This is the code a program gets for a write-protected target,
        ; and its class/action/locus were never measured -- the handler says so
        ; in as many words ("class/action/locus UNMEASURED for code 0x...").
        mov     ax, 3C00h
        mov     cx, 0
        mov     dx, rofile
        int     21h
        mov     bx, ax
        mov     ax, 3E00h
        int     21h
        mov     ax, 4301h                       ; set attributes
        mov     cx, 1                           ; read-only
        mov     dx, rofile
        int     21h

        mov     ax, 3D01h                       ; open for WRITE -> denied
        mov     dx, rofile
        int     21h
        ASKERR  "err.after.3D.readonly"

        ; ---- FILE EXISTS (80). 5Bh is create-NEW, so pointing it at the file we
        ; just made is a clean, harmless provocation. The code itself is already
        ; pinned (int21.5B.exists in p_file); this asks what DOS CLASSIFIES it as.
        mov     ax, 5B00h
        mov     cx, 0
        mov     dx, rofile
        int     21h
        ASKERR  "err.after.5B.exists"

        ; ---- put the attributes back so 41h can delete it: a probe that leaves
        ; a read-only file behind is not self-cleaning on the rig or DOSBox.
        mov     ax, 4301h
        mov     cx, 0
        mov     dx, rofile
        int     21h
        mov     ax, 4100h
        mov     dx, rofile
        int     21h

        ; ---- INVALID DRIVE (15). A drive letter with no drive behind it.
        ; ⚠ Z: is NOT usable here -- DOSBox-X always mounts Z: as its own utility
        ; drive, which is why oracle-rules.json already abstains it for
        ; int21.3600.baddrive. Y: is unclaimed on every host in the panel.
        mov     ax, 3D00h
        mov     dx, baddrv
        int     21h
        ASKERR  "err.after.3D.baddrive"

        ; ---- AH=34h: THE InDOS FLAG. Returns ES:BX pointing at it.
        ; The ADDRESS is host-specific and means nothing across hosts, so it is
        ; not in the signature; what is comparable is the SHAPE around it. The
        ; byte AT ES:BX is InDOS (the DOS re-entrancy depth) and the byte BEFORE
        ; it is the CRITICAL ERROR FLAG -- that adjacency is the whole reason a
        ; TSR asks for this pointer, and it is exactly the sort of layout claim
        ; that must be measured rather than recalled.
        POISON
        mov     ax, 3400h
        int     21h
        call    probe_capture
        EMIT    "int21.34.indos", "CF"

        ; copy the two bytes around the returned pointer into our own space --
        ; EMIT_BUF dumps DS:SI, and ES:BX is DOS's segment, not ours.
        push    ds
        mov     ds, [__es]                      ; probe_capture saved the real ES
        mov     si, [__bx]
        dec     si                              ; -1 = critical error flag
        mov     al, [si]
        inc     si
        mov     ah, [si]                        ; +0 = InDOS
        pop     ds
        mov     [indos], ax                     ; AL=crit, AH=InDOS
        EMIT_BUF "err.indos.pair", indos, 2

        PROBE_END

nofile   db 'ZZNOSUCH.XYZ', 0
nopath   db '\ZZNODIR\*.*', 0
nopath2  db '\ZZNODIR', 0
rofile   db 'ZZRDONLY.TMP', 0
baddrv   db 'Y:\ZZNOSUCH.XYZ', 0
indos    dw 0
scratch  db 0, 0, 0, 0
