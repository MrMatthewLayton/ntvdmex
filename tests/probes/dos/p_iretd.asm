; p_iretd.com -- a 32-bit IRET (66 CF) in real/V86 mode, as THIS machine runs it. GH #194.
;
; Separate from p_o32 on purpose: in V86 mode IRETD is IOPL-sensitive, so on the test machine it
; is not the CPU that answers but XP's V86 monitor emulating it -- and if the monitor
; refuses, the VDM may die, which must not take p_o32's answers with it.
;
; The frame is pushed by hand: FLAGS image (CF set, so a reload is visible), CS, EIP,
; each a dword. After the IRETD: SP must be back where it started (12 bytes popped),
; CF must be what the frame said, and we must have arrived at .after.
;
; nasm -f bin -I tests/probes/dos/ p_iretd.asm -o p_iretd.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "iretd"
        mov     dx, sp
        pushfd
        pop     eax
        or      eax, 1                          ; CF=1 in the frame
        push    eax
        o32 push cs
        push    dword .after
        clc                                     ; CF=0 now: only the IRETD can set it
        iretd
        mov     ax, 0BADh                       ; not reached
        jmp     short .rep
.after: sbb     ax, ax                          ; AX = FFFF if CF came back from the frame
        sub     dx, sp                          ; 0 if all 12 bytes were popped
.rep:   mov     bx, 0
        mov     cx, 0
        mov     si, 0
        mov     di, 0
        call    probe_capture
        EMIT    "iretd.v86", "AX,DX"            ; expect AX=FFFF DX=0000
        PROBE_END
