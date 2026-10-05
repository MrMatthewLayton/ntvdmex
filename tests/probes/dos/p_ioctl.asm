; p_ioctl.com -- INT 21h AH=44h IOCTL: the sub-functions that answer in REGISTERS.
;
; WHY THIS PROBE EXISTS, and it is a correction rather than a new idea.  Session
; 37 implemented AL=08h/09h/0Eh -- "is this block device removable", "is it
; remote", "get the logical drive map" -- because krnl386 probes every drive with
; all three and our host was answering the worst possible thing: carry clear,
; meaning SUCCESS, with the caller's own registers as the answer.  They were
; written from the DOCUMENTED interface, not from a run, and this project's
; cardinal rule for M9 is that a test expectation is never written from memory:
; the oracle is a panel, and NTVDMEX does not vote.  So: ask the panel.
;
; ONLY THE DEFAULT DRIVE (BL=0) IS PROBED, deliberately.  p_dir.asm already
; records what naming a specific letter costs -- DOSBox-X always mounts Z: as its
; own utility drive, so "a drive that does not exist" is a different question
; there and produces a DISPUTED row that is pure host geometry.  The default
; drive is a fixed disk on every host in the panel, which is the one thing they
; can be expected to agree about.
;
; AH IS MASKED OFF where RBIL documents it as destroyed (19h, 440Eh).  p_dir.asm
; records the same discipline for 47h: comparing a register the interface does
; not define manufactures a disagreement that says nothing.  The first run of this
; probe compared the whole AX for 440Eh and duly produced one.
;
; AL=09h's DX is MASKED TO BIT 12 before capture.  DX is documented as the whole
; device attribute word, and most of its bits describe the driver rather than the
; drive -- comparing all sixteen would manufacture disagreements that say nothing
; about whether the drive is remote, which is the only thing any caller asks.
; Bit 12 IS the answer, so bit 12 is what is compared.
;
; nasm -f bin p_ioctl.asm -o p_ioctl.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "ioctl"

        ; ---- 19h: WHICH DRIVE ARE WE EVEN ASKING ABOUT?
        ; Every case below says "the default drive", and the panel does not agree
        ; what that is -- the 6.22 oracle boots from a floppy image.  Without this
        ; row a reader cannot tell a difference in DOS BEHAVIOUR from a difference
        ; in host GEOMETRY, and the first run of this probe produced exactly that
        ; ambiguity.  AL = 0 for A:, 2 for C:.  AH is not part of the answer.
        POISON
        mov     ah, 19h
        int     21h
        and     ax, 00FFh
        call    probe_capture
        EMIT    "int21.19.curdrive", "AX"

        ; ---- 4400h: get device info for a HANDLE (BX=0, stdin).
        ; The one sub-function a C runtime's isatty() uses, and the one we have
        ; answered since M2 -- included so a regression in the old arm shows up
        ; in the same run as the new ones.
        mov     cx, 0C1C1h
        mov     dx, 0D1D1h
        mov     ax, 4400h
        mov     bx, 0
        int     21h
        call    probe_capture
        EMIT    "int21.4400.stdin", "CF"

        ; ── ⚠ BL=3 (C:), NOT BL=0 ("the default drive"). ────────────────────────
        ; These three asked about whatever drive the program happened to be
        ; STARTED on -- A: on the oracle, which boots from a floppy, and C: on the
        ; rig. So 4408 asked "is a floppy removable?" on one host and "is a hard
        ; disk removable?" on the other, and the two CORRECT answers (0 and 1)
        ; were reported as NTVDMEX disagreeing with DOS.
        ;
        ; 440E was worse, because the comment below asserted the thing that was
        ; false: "no host has a single-floppy alias on its DEFAULT drive" -- the
        ; oracle's default IS A:, and a machine with one physical floppy maps both
        ; A: and B: to it, which is exactly the alias 440E exists to report. The
        ; oracle answered 1 and was right.
        ;
        ; C: is a fixed disk on every host in the panel, so all three become the
        ; same question everywhere: removable=1 (fixed), not remote, no alias.

        ; ---- 4408h: is C:'s block device removable?  AX = 0 removable, 1 fixed.
        POISON
        mov     ax, 4408h
        mov     bl, 3                   ; C:
        int     21h
        call    probe_capture
        EMIT    "int21.4408.C", "AX,CF"

        ; ---- 4409h: is C: remote?
        ; DX = the device attribute word; bit 12 (0x1000) is "remote".  Masked --
        ; see the header note.
        POISON
        mov     ax, 4409h
        mov     bl, 3                   ; C:
        int     21h
        pushf
        and     dx, 1000h
        popf
        call    probe_capture
        EMIT    "int21.4409.C", "DX,CF"

        ; ---- 440Eh: get the logical drive map for C:.
        ; AL = 0 when only one letter maps to the block device, else the letter
        ; number.  A hard disk has no SUBST or single-floppy alias on any host in
        ; the panel, so 0 is the answer they should share -- which is what the old
        ; comment claimed for the DEFAULT drive and could not deliver.
        POISON
        mov     ax, 440Eh
        mov     bl, 3                   ; C:
        int     21h
        pushf
        and     ax, 00FFh               ; AL is the answer; RBIL destroys AH, and
        popf                            ;   the panel duly differs on it
        call    probe_capture
        EMIT    "int21.440E.C", "AX,CF"

        PROBE_END
