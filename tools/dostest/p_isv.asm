; p_isv.com -- a DOS program and its own VDD (the third-party BOP), vs stock.  GH #11 (s91).
;
; isvbop.inc's three calls, `C4 C4 58 nn`:
;   RegisterModule   nn=0  DS:SI = DLL, DS:DI = init routine, DS:BX = dispatch routine
;                          -> CF clear, AX = handle | CF set, AX = 1 DLL / 2 dispatch / 3 init
;   UnRegisterModule nn=1  AX = handle
;   DispatchCall     nn=2  AX = handle; the VDD reads/writes our registers
; against tools/dostest/isvtest/ISVTEST.DLL (an MS-ABI VDD -- see isvtest.c), which
; must sit beside this .COM. The handle's VALUE is the host's; it is printed as a BOOL.
; nasm -f bin p_isv.asm -o p_isv.com

        org     100h
        jmp     start
%include "probe.inc"

%macro REGMOD 3                         ; dll, init, dispatch
        mov     si, %1
        mov     di, %2
        mov     bx, %3
        db      0C4h, 0C4h, 58h, 00h
%endmacro
%macro DISPATCH 0
        mov     ax, [hnd]
        db      0C4h, 0C4h, 58h, 02h
%endmacro

start:
        PROBE_BEGIN "isv"

        ; ---- no such DLL
        REGMOD  s_nodll, s_init, s_disp
        sbb     cx, cx
        call    probe_capture
        EMIT    "isv.reg.nodll", "AX,CX"

        ; ---- no such dispatch routine
        REGMOD  s_dll, s_init, s_nodisp
        sbb     cx, cx
        call    probe_capture
        EMIT    "isv.reg.nodispatch", "AX,CX"

        ; ---- the real thing (its init hooks ports 2F0h-2F1h)
        REGMOD  s_dll, s_init, s_disp
        sbb     cx, cx
        mov     [hnd], ax
        mov     [regcf], cx
        or      ax, ax
        jz      .z
        mov     ax, 1
.z:     call    probe_capture
        EMIT    "isv.reg.ok", "AX,CX"
        cmp     word [regcf], 0
        je      .go
        jmp     .done
.go:
        ; ---- dispatch DX=1: CX = BX ^ FFFF
        mov     dx, 1
        mov     bx, 1234h
        mov     cx, 0C1C1h
        DISPATCH
        sbb     ax, ax
        call    probe_capture
        EMIT    "isv.disp.xor", "AX,CX"

        ; ---- the I/O hook its init installed
        mov     dx, 2F0h
        in      al, dx
        xor     ah, ah
        call    probe_capture
        EMIT    "isv.io.in2f0", "AX"
        mov     dx, 2F1h
        in      al, dx
        xor     ah, ah
        call    probe_capture
        EMIT    "isv.io.in2f1", "AX"
        mov     dx, 2F0h
        mov     al, 77h
        out     dx, al
        mov     dx, 2
        mov     cx, 0C1C1h
        DISPATCH
        sbb     ax, ax
        call    probe_capture
        EMIT    "isv.disp.latch", "AX,CX"

        ; ---- dispatch DX=3: the VDD reads our memory through VdmMapFlat
        mov     si, abc
        mov     cx, 3
        mov     dx, 3
        DISPATCH
        sbb     ax, ax
        call    probe_capture
        EMIT    "isv.disp.sum", "AX,CX"

        ; ---- an unknown function: CF set by the VDD
        mov     dx, 9
        DISPATCH
        sbb     ax, ax
        call    probe_capture
        EMIT    "isv.disp.unknown.cf", "AX"

        ; ---- UnRegisterModule; then a dispatch on the dead handle
        mov     ax, [hnd]
        db      0C4h, 0C4h, 58h, 01h
        sbb     ax, ax
        call    probe_capture
        EMIT    "isv.unreg.cf", "AX"
.done:
        PROBE_END

hnd     dw      0
regcf   dw      0
abc     db      'ABC'
s_nodll  db     'NOSUCH91.DLL', 0
s_dll    db     'ISVTEST.DLL', 0
s_init   db     'IsvInit', 0
s_disp   db     'IsvDispatch', 0
s_nodisp db     'NoSuchRoutine', 0
