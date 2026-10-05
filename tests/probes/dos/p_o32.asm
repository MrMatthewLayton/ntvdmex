; p_o32.com -- the 32-bit operand-size (0x66) forms of 16-bit code, as THIS CPU runs them.
;              GH #194.
;
; WHY. The real-mode interpreter (src/host/v86interp.h) stands in for the rig's CPU in
; planar video modes, and it declined PUSHFD/POPFD, 32-bit segment push/pop, the 32-bit
; string ops, CALL/RET/RETF/LEAVE/IRETD with 0x66 -- every one a hand-back to the real
; CPU with A0000 unprotected. Filling them in needs the CPU's answer, including the
; parts the manual leaves to the implementation (what a 32-bit PUSH DS writes in the
; upper half of its slot; what MOV EAX,DS leaves in EAX[31:16]; which EFLAGS bits a
; V86 POPFD may change). Those are MEASURED here, not recalled.
;
; HOW THE ANSWER IS USED TWICE. Everything between `measure` and `measure_end` is pure
; computation into `res` -- no INT, no I/O, no absolute segment value stored -- so the
; SAME BYTES can be run through the interpreter off-VM (tests/unit/interp_test.c
; reads this .COM, runs measure..measure_end, and compares `res` with the rig's dump in
; p_o32.ref.txt). The table at 0x103 gives it the offsets. The rig's row is the real
; CPU under XP's V86 monitor -- exactly the machine the interpreter replaces; the
; oracles (real mode, not V86) vote on the parts that do not depend on V86.
;
; nasm -f bin -I tests/probes/dos/ p_o32.asm -o p_o32.com

        org     100h
        jmp     near start
        dw      measure, measure_end, res, RES_LEN      ; at 0x103: for interp_test.c
%include "probe.inc"

%define R(n) res + (n) * 4

start:
        PROBE_BEGIN "o32"
        call    measure
        EMIT_BUF "res", res, RES_LEN
        PROBE_END

; ---------------------------------------------------------------------------------
measure:
        mov     [savesp], sp
        mov     ax, ds
        mov     [dsval], ax
        movzx   edi, ax                         ; EDI = DS zero-extended, for the XORs below

        ; 0: PUSHFD after a defined compare (1-2: CF SF AF PF set, ZF OF clear), DF=0
        cld
        mov     ax, 1
        cmp     ax, 2
        pushfd
        pop     eax
        mov     [R(0)], eax

        ; 1: which of AC (bit 18) / ID (bit 21) a POPFD can flip -- XOR of before/after
        pushfd
        pop     eax
        mov     ecx, eax
        xor     eax, 00240000h
        push    eax
        popfd
        pushfd
        pop     eax
        xor     eax, ecx
        mov     [R(1)], eax
        push    ecx                             ; put them back
        popfd

        ; 2: o32 PUSH DS over a sentinel slot: what lands in the upper half
        sub     sp, 4
        mov     bp, sp
        mov     dword [bp], 0DEADBEEFh
        add     sp, 4
        o32 push ds
        pop     eax
        xor     eax, edi                        ; low half cancels; upper = what was written
        mov     [R(2)], eax

        ; 3: o32 PUSH DS / o32 POP ES: SP back where it was, ES = DS
        mov     dx, sp
        o32 push ds
        o32 pop es
        sub     dx, sp                          ; 0
        mov     ax, es
        xor     ax, [cs:dsval]                  ; 0 if ES = DS (dsval set below)
        mov     [R(3)], dx
        mov     [R(3) + 2], ax

        ; 4: MOV EAX,DS (66 8C D8) over EAX = DEADBEEF: EAX[31:16] afterwards
        mov     eax, 0DEADBEEFh
        mov     eax, ds
        xor     eax, edi
        mov     [R(4)], eax

        ; 5: PUSH FS (0F A0) = 2 bytes; 6: o32 PUSH FS = 4 bytes (SP deltas)
        mov     dx, sp
        push    fs
        sub     dx, sp
        mov     [R(5)], dx
        pop     fs
        mov     dx, sp
        o32 push fs
        sub     dx, sp
        mov     [R(6)], dx
        o32 pop fs

        ; 7-9: REP MOVSD, CX=3, DF=0: the copy; 10: SI/DI advance, CX left
        push    ds
        pop     es
        mov     si, srcbuf
        mov     di, R(7)
        mov     cx, 3
        rep movsd
        sub     si, srcbuf
        sub     di, R(7)
        mov     [R(10)], si
        mov     [R(10) + 2], di
        mov     [R(35)], cx

        ; 11: MOVSD with DF=1: SI/DI deltas (both -4)
        mov     si, srcbuf + 8
        mov     di, scratch + 8
        std
        movsd
        cld
        sub     si, srcbuf + 8
        sub     di, scratch + 8
        mov     [R(11)], si
        mov     [R(11) + 2], di

        ; 12-13: REP STOSD, CX=2, EAX=11223344; 14: DI advance
        mov     eax, 11223344h
        mov     di, R(12)
        mov     cx, 2
        rep stosd
        sub     di, R(12)
        mov     [R(14)], di

        ; 15: LODSD
        mov     si, srcbuf + 4
        lodsd
        mov     [R(15)], eax

        ; 16: REPE CMPSD over srcbuf/cmpbuf (differ at dword 1): CX left | SI delta<<16
        ; 17: its flags (CF PF AF ZF SF OF)
        mov     si, srcbuf
        mov     di, cmpbuf
        mov     cx, 3
        repe cmpsd
        pushf
        sub     si, srcbuf
        mov     [R(16)], cx
        mov     [R(16) + 2], si
        pop     ax
        and     ax, 08D5h
        mov     [R(17)], ax

        ; 18: REPNE SCASD for srcbuf's dword 2, CX=5: CX left | DI delta<<16; 19: flags
        mov     eax, [srcbuf + 8]
        mov     di, srcbuf
        mov     cx, 5
        repne scasd
        pushf
        sub     di, srcbuf
        mov     [R(18)], cx
        mov     [R(18) + 2], di
        pop     ax
        and     ax, 08D5h
        mov     [R(19)], ax

        ; 20: CALL rel32 (66 E8) pushes a 4-byte return EIP
        call    dword .c1
.c1r:                                           ; (never reached: .c1 pops the frame)
.c1:    pop     eax
        sub     eax, .c1r
        mov     [R(20)], eax

        ; 21: o32 RET pops 4: SP delta across PUSH imm32 / RET
        mov     dx, sp
        push    dword .r1
        retd
.r1:    sub     dx, sp
        mov     [R(21)], dx

        ; 22: o32 RET 4 (66 C2): CALL rel32 + one dword argument, SP delta 0
        mov     dx, sp
        push    dword 99999999h
        call    dword .r2
        jmp     short .r2d
.r2:    retd    4
.r2d:   sub     dx, sp
        mov     [R(22)], dx

        ; 23: o32 RETF (66 CB): o32 PUSH CS / PUSH imm32 / RETF, SP delta 0
        mov     dx, sp
        o32 push cs
        push    dword .r3
        retfd
.r3:    sub     dx, sp
        mov     [R(23)], dx

        ; 24: CALL dword [m32] (66 FF /2): return EIP - label = 0
        mov     dword [tmp], .c2
        call    dword [tmp]
.c2r:
.c2:    pop     eax
        sub     eax, .c2r
        mov     [R(24)], eax

        ; 25: JMP dword [m32] (66 FF /4) arrives
        mov     dword [R(25)], 0
        mov     dword [tmp], .j1
        jmp     dword [tmp]
        mov     dword [R(25)], 0BADh
.j1:    inc     dword [R(25)]

        ; 26: PUSH dword [m32] (66 FF /6)
        mov     dword [tmp], 12345678h
        push    dword [tmp]
        pop     eax
        mov     [R(26)], eax

        ; 27: CALL FAR [m16:32] (66 FF /3): EIP slot - label; 28: CS slot's upper half
        mov     dword [farp], .c3
        mov     [farp + 4], cs
        call    far dword [farp]
.c3r:
.c3:    pop     eax
        pop     edx
        sub     eax, .c3r
        mov     [R(27)], eax
        mov     ax, cs
        movzx   ecx, ax
        xor     edx, ecx
        mov     [R(28)], edx

        ; 29: JMP FAR ptr16:32 (66 EA), its segment patched in at run time
        mov     dword [R(29)], 0
        mov     [.jf + 6], cs
.jf:    jmp     dword 0:.j2
        mov     dword [R(29)], 0BADh
.j2:    inc     dword [R(29)]

        ; 30: CALL FAR ptr16:32 (66 9A): EIP slot - label; 31: CS slot's upper half,
        ;     over a sentinel left in that slot (item 28's slot held 0x1234 from item 26)
        push    dword 0FACE0000h
        pop     eax
        mov     [.cf + 6], cs
.cf:    call    dword 0:.c4
.c4r:
.c4:    pop     eax
        pop     edx
        sub     eax, .c4r
        mov     [R(30)], eax
        mov     ax, cs
        movzx   ecx, ax
        xor     edx, ecx
        mov     [R(31)], edx

        ; 32: o32 POP dword [m] (66 8F /0)
        push    dword 0CAFEBABEh
        o32 pop dword [R(32)]

        ; 33: LES ECX, m16:32 (66 C4): ECX; 34 low: ES = DS ?
        mov     dword [farp], 87654321h
        mov     [farp + 4], ds
        les     ecx, [farp]
        mov     [R(33)], ecx
        mov     ax, es
        xor     ax, [cs:dsval]
        mov     [R(34)], ax

        ; 36: o32 LEAVE (66 C9): EBP popped; 37: SP back to before the PUSH
        mov     dx, sp
        push    dword 11223344h
        mov     ebp, 77770000h
        mov     bp, sp
        sub     sp, 10
        db      66h
        leave
        mov     [R(36)], ebp
        sub     dx, sp
        mov     [R(37)], dx

        ; 38: PUSH GS / POP GS (0F A8 / 0F A9): SP delta after the push
        mov     dx, sp
        push    gs
        sub     dx, sp
        pop     gs
        mov     [R(38)], dx

        ; 39: PUSHFD after CLI -- does the image's IF (and VIF) follow the virtual IF?
        cli
        pushfd
        pop     eax
        sti
        mov     [R(39)], eax

        mov     sp, [savesp]
        ret
measure_end:

; ---------------------------------------------------------------------------------
dsval   dw      0                               ; = DS, set on entry to measure
savesp  dw      0
tmp     dd      0
farp    dd      0
        dw      0
srcbuf  dd      0A1A2A3A4h, 0B1B2B3B4h, 0C1C2C3C4h, 0D1D2D3D4h
cmpbuf  dd      0A1A2A3A4h, 0B1B2B300h, 0C1C2C3C4h
scratch dd      0, 0, 0, 0
RES_N   equ     40
RES_LEN equ     RES_N * 4
res     times RES_LEN db 0
