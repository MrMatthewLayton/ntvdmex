; p_ioctl2.com -- INT 21h AH=44h sub-functions beyond 00/08/09/0E (GH #251).
;
; We answered CF=0 for every sub-function we do not implement -- "success" with
; nothing done. This asks the reference kernels what each READ-ONLY / QUERY form
; really answers, so the fallback can say what DOS says. Nothing here writes a
; device: 02h/04h read with CX=0, 0Ch/0Dh use GET minor codes, 10h/11h only ask.
; Handle 1 is CON (unredirected under the harness? -- see probe.inc: output goes
; to a file, so handle 1 here is the probe's REPORT FILE, a disk file).
;
; nasm -f bin p_ioctl2.asm -o p_ioctl2.com
        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "ioctl2"

        POISON                                  ; 02h: read control data, char device
        mov     ax, 4402h
        mov     bx, 0                           ; stdin = CON
        xor     cx, cx
        mov     dx, buf
        int     21h
        call    probe_capture
        EMIT    "int21.4402.con", "AX,CF"

        POISON                                  ; 04h: read control data, block device
        mov     ax, 4404h
        mov     bl, 3                           ; C:
        xor     cx, cx
        mov     dx, buf
        int     21h
        call    probe_capture
        EMIT    "int21.4404.C", "AX,CF"

        POISON                                  ; 0Ah: is handle remote (stdin)
        mov     ax, 440Ah
        mov     bx, 0
        int     21h
        call    probe_capture
        EMIT    "int21.440A.stdin", "CF"

        POISON                                  ; 0Ch: generic char IOCTL, CON, get display info
        mov     ax, 440Ch
        mov     bx, 0
        mov     cx, 037Fh
        mov     dx, buf
        int     21h
        call    probe_capture
        EMIT    "int21.440C.con.7F", "CF"

        POISON                                  ; 0Dh: generic block IOCTL, C:, get device params
        mov     ax, 440Dh
        mov     bl, 3
        mov     cx, 0860h
        mov     dx, buf
        mov     byte [buf], 0
        int     21h
        call    probe_capture
        EMIT    "int21.440D.C.60", "CF"

        POISON                                  ; 10h: query generic IOCTL capability, handle
        mov     ax, 4410h
        mov     bx, 0
        mov     cx, 037Fh
        int     21h
        call    probe_capture
        EMIT    "int21.4410.con.7F", "AX,CF"

        POISON                                  ; 11h: query generic IOCTL capability, drive
        mov     ax, 4411h
        mov     bl, 3
        mov     cx, 0860h
        int     21h
        call    probe_capture
        EMIT    "int21.4411.C.60", "AX,CF"

        POISON                                  ; 12h: unassigned
        mov     ax, 4412h
        mov     bx, 0
        int     21h
        call    probe_capture
        EMIT    "int21.4412.unassigned", "AX,CF"

        POISON                                  ; 1Fh: unassigned
        mov     ax, 441Fh
        mov     bx, 0
        int     21h
        call    probe_capture
        EMIT    "int21.441F.unassigned", "AX,CF"

        PROBE_END

buf     times 64 db 0
