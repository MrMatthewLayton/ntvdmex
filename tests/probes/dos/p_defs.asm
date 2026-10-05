; p_defs.com -- WHERE DOES MS-DOS 6.22's INT 21h FUNCTION TABLE END?
;
; Feeds GH #27/#29-#38.  We need to tell two things apart in our own unhandled
; tail, because they want opposite answers:
;
;   * a function MS-DOS 6.22 does NOT define  -> match DOS: do nothing quietly
;   * a function it DOES define that we have not written yet -> fail loudly,
;     because a quiet "success" makes the guest believe a no-op worked
;
; THE DISCRIMINATOR: POISON leaves recognisable junk in the output registers, and
; an undefined function returns with AX *unchanged* (we measured AH=FFh/73h/88h
; on the oracle: AX comes back exactly as passed, CF=0).  A defined function does
; something -- an error code, a result, anything.  So "AX came back as AH<<8" is
; the signature of "nobody is home".
;
; SAFETY.  These are called with poisoned BX/CX/DX and a DS:DX that points into
; our own PSP, so only functions that cannot destroy anything on a live DOS are
; listed.  Deliberately NOT probed, and why:
;   00h 4Ch 31h  terminate the probe          4Bh   executes something
;   25h 26h      overwrite a vector / PSP     41h   deletes a file
;   39h 3Ah 3Bh  mkdir/rmdir/chdir on junk    3Ch   creates/truncates a file
;   13h 16h 17h  FCB delete/create/rename     5Ah 5Bh creates a file
;   6Ch          extended open/CREATE         2Bh 2Dh set the clock
; The oracle also runs with snapshot=on so its disk cannot be modified, but the
; same binary runs on the rig and under DOSBox where that protection does not
; apply -- hence the list above rather than a blind 00h-FFh sweep.
;
; nasm -f bin p_defs.asm -o p_defs.com

        org     100h
        jmp     start
%include "probe.inc"

; TRYFN <ah>, "<case>" -- call INT 21h AH=<ah>, AL=0 with poisoned outputs.
%macro TRYFN 2
        POISON
        mov     ax, (%1 << 8)
        int     21h
        call    probe_capture
        EMIT    %2, "AX,CF"
%endmacro

; As TRYFN, but the result is host-specific: dumped for the log, not compared.
%macro TRYFN_INFO 2
        POISON
        mov     ax, (%1 << 8)
        int     21h
        call    probe_capture
        EMIT    %2, ""
%endmacro

start:
        PROBE_BEGIN "defs"

        ; ---- CONTROLS: documented, safe, and certainly present on 6.22.
        ; If any of these comes back with AX unchanged the discriminator is
        ; broken and nothing below can be trusted.
        ; 19h returns the DEFAULT DRIVE in AL, which says where the probe is
        ; running, not what DOS does -- so it is dumped, not compared. It still
        ; earns its place as a control: AL comes back changed from the poison on
        ; every host, which is what proves the discriminator works.
        TRYFN_INFO 019h, "def.19.getdrive"
        TRYFN 02Ch, "def.2C.gettime"            ; -> CX:DX = time
        TRYFN 054h, "def.54.getverify"          ; -> AL = verify flag
        TRYFN 030h, "def.30.version"            ; -> AX = version

        ; ---- DOCUMENTED "NULL"/INTERNAL FUNCTIONS inside the table.  If these
        ; also do nothing, they belong with the undefined ones for our purposes:
        ; matching DOS means no-op quietly, not fail loudly.
        TRYFN 018h, "null.18"
        TRYFN 01Dh, "null.1D"
        TRYFN 01Eh, "null.1E"
        TRYFN 020h, "null.20"

        ; ---- THE BOUNDARY.  6Ch is the highest function documented for 6.22,
        ; so 6Dh upward is expected to be nobody's.
        TRYFN 061h, "edge.61.reserved"
        TRYFN 06Bh, "edge.6B.null"
        TRYFN 06Dh, "edge.6D"
        TRYFN 06Eh, "edge.6E"
        TRYFN 06Fh, "edge.6F"
        TRYFN 070h, "edge.70"
        TRYFN 071h, "edge.71"                   ; LFN group (Win95), absent on 6.22
        TRYFN 072h, "edge.72"
        TRYFN 074h, "edge.74"

        ; ---- well past the end
        TRYFN 080h, "far.80"
        TRYFN 0A0h, "far.A0"
        TRYFN 0E0h, "far.E0"

        PROBE_END
