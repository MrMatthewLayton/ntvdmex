; p_ver.com -- differential probe: how does DOS report its version?
;
; Feeds GH #28 (configurable DOS version).  Runs unchanged on every host; the
; harness (scripts/dosdiff.py) diffs the dumps.
;
; nasm -f bin p_ver.asm -o p_ver.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "dosver"

        ; ---- INT 21h AH=30h, AL=00.  AL=00 asks for the OEM number in BH.
        ; AL=major, AH=minor, BH=OEM, BL:CX=24-bit user serial.
        mov     ax, 3000h
        int     21h
        call    probe_capture
        EMIT    "int21.30", "AX,BX,CX"

        ; ---- INT 21h AX=3306h, "get true version" (DOS 5.0+).
        ; BL=major, BH=minor, DL=revision, DH=flags.  Not present on every DOS,
        ; so CF is part of the answer rather than an error to hide.
        ;
        ; DH is deliberately NOT significant: bit 4 means "DOS is in the HMA",
        ; which is a property of the host's CONFIG.SYS, not of the DOS version.
        ; Asserting on it would flag a configuration difference as a defect.
        mov     ax, 3306h
        int     21h
        call    probe_capture
        EMIT    "int21.3306", "BX,DL,CF"

        PROBE_END
