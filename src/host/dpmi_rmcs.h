/* dpmi_rmcs.h -- the DPMI real-mode call structure, and how INT 31h 0300h routes a vector.
 *
 * GH #247. Header-only and dependency-free (stdint, no windows.h), so the off-VM battery
 * (tests/unit/rmcs_test.c) compiles exactly the code the host runs.
 *
 * ── THE STRUCTURE (DPMI 0.9, INT 31h 0300h/0301h/0302h and the 0303h callbacks) ──────
 *     +00 EDI  +04 ESI  +08 EBP  +0C reserved  +10 EBX  +14 EDX  +18 ECX  +1C EAX
 *     +20 FLAGS  +22 ES  +24 DS  +26 FS  +28 GS  +2A IP  +2C CS  +2E SP  +30 SS
 *   50 bytes. The general registers are 32 bits WIDE: a real-mode handler on a 386
 *   can take and return 32-bit values (INT 15h E820h answers in EAX/EBX/ECX), and a
 *   32-bit client reads the whole field back -- Doom's I_ReadMouse does
 *   `ev.data1 = dpmiregs.ebx` on the full DWORD.
 *
 * ── WHAT IS AN INPUT AND WHAT IS AN OUTPUT ───────────────────────────────────────────
 *   In:  every general register, FLAGS, and all four segment registers. CS:IP is the
 *        target for 0301h/0302h and IGNORED by 0300h (the target is IVT[BL]); SS:SP is
 *        the real-mode stack, zero meaning "host, provide one".
 *   Out: every general register, FLAGS, ES, DS, FS, GS. ⚠ NOT CS, IP, SS or SP -- the
 *        spec says those "are not modified", and a client is entitled to reuse one
 *        structure for a second call without re-filling its target and stack.
 *   ★ RmcsWrite() therefore CANNOT write those four: it has no parameter for them.
 *     The old 0300h retarget wrote IVT[BL] into the caller's CS:IP to borrow the 0302h
 *     arm; that is exactly the write this shape makes impossible.
 */
#ifndef NTVDMEX_DPMI_RMCS_H
#define NTVDMEX_DPMI_RMCS_H

#include "../ntvdmex_types.h"

#define RMCS_EDI    0x00
#define RMCS_ESI    0x04
#define RMCS_EBP    0x08
#define RMCS_EBX    0x10
#define RMCS_EDX    0x14
#define RMCS_ECX    0x18
#define RMCS_EAX    0x1C
#define RMCS_FLAGS  0x20
#define RMCS_ES     0x22
#define RMCS_DS     0x24
#define RMCS_FS     0x26
#define RMCS_GS     0x28
#define RMCS_IP     0x2A
#define RMCS_CS     0x2C
#define RMCS_SP     0x2E
#define RMCS_SS     0x30
#define RMCS_SIZE   0x32

/* The register file a real-mode service sees and answers in. No CS:IP/SS:SP: those
   are the CALL's plumbing, not its inputs or outputs (see above). */
typedef struct _RMCS_REGS {
    UINT32 Edi, Esi, Ebp, Ebx, Edx, Ecx, Eax;
    UINT16 Flags, Es, Ds, Fs, Gs;
} RMCS_REGS, *PRMCS_REGS; typedef const RMCS_REGS *PCRMCS_REGS;

#define RMCS_BYTE1_SHIFT 8
#define RMCS_BYTE2_SHIFT 16
#define RMCS_BYTE3_SHIFT 24

/* Byte-assembled: the structure lives wherever the client put it, at any alignment. */
static DWORD RmcsRead32(const volatile BYTE *structure, UINT offset)
{
    return (DWORD)structure[offset] | ((DWORD)structure[offset + 1] << RMCS_BYTE1_SHIFT)
         | ((DWORD)structure[offset + 2] << RMCS_BYTE2_SHIFT) | ((DWORD)structure[offset + 3] << RMCS_BYTE3_SHIFT);
}
static WORD RmcsRead16(const volatile BYTE *structure, UINT offset)
{
    return (WORD)(structure[offset] | (structure[offset + 1] << RMCS_BYTE1_SHIFT));
}
static VOID RmcsWrite32(volatile BYTE *structure, UINT offset, DWORD value)
{
    structure[offset] = (BYTE)value; structure[offset + 1] = (BYTE)(value >> RMCS_BYTE1_SHIFT);
    structure[offset + 2] = (BYTE)(value >> RMCS_BYTE2_SHIFT); structure[offset + 3] = (BYTE)(value >> RMCS_BYTE3_SHIFT);
}
static VOID RmcsWrite16(volatile BYTE *structure, UINT offset, WORD value)
{
    structure[offset] = (BYTE)value; structure[offset + 1] = (BYTE)(value >> RMCS_BYTE1_SHIFT);
}

static VOID RmcsRead(const volatile BYTE *structure, PRMCS_REGS out)
{
    out->Edi = RmcsRead32(structure, RMCS_EDI); out->Esi = RmcsRead32(structure, RMCS_ESI);
    out->Ebp = RmcsRead32(structure, RMCS_EBP); out->Ebx = RmcsRead32(structure, RMCS_EBX);
    out->Edx = RmcsRead32(structure, RMCS_EDX); out->Ecx = RmcsRead32(structure, RMCS_ECX);
    out->Eax = RmcsRead32(structure, RMCS_EAX);
    out->Flags = RmcsRead16(structure, RMCS_FLAGS);
    out->Es = RmcsRead16(structure, RMCS_ES); out->Ds = RmcsRead16(structure, RMCS_DS);
    out->Fs = RmcsRead16(structure, RMCS_FS); out->Gs = RmcsRead16(structure, RMCS_GS);
}

/* Everything the spec returns, nothing it does not. +0C (reserved) is not touched
   either: it is the client's. */
static VOID RmcsWrite(volatile BYTE *structure, PCRMCS_REGS in)
{
    RmcsWrite32(structure, RMCS_EDI, in->Edi); RmcsWrite32(structure, RMCS_ESI, in->Esi);
    RmcsWrite32(structure, RMCS_EBP, in->Ebp); RmcsWrite32(structure, RMCS_EBX, in->Ebx);
    RmcsWrite32(structure, RMCS_EDX, in->Edx); RmcsWrite32(structure, RMCS_ECX, in->Ecx);
    RmcsWrite32(structure, RMCS_EAX, in->Eax);
    RmcsWrite16(structure, RMCS_FLAGS, in->Flags);
    RmcsWrite16(structure, RMCS_ES, in->Es); RmcsWrite16(structure, RMCS_DS, in->Ds);
    RmcsWrite16(structure, RMCS_FS, in->Fs); RmcsWrite16(structure, RMCS_GS, in->Gs);
}

/* ── HOW 0300h SERVICES VECTOR `vec`, GIVEN WHAT IVT[vec] HOLDS. ───────────────────────
     The spec has ONE rule: run whatever the REAL-MODE IVT points at -- our BIOS/DOS stub,
     a TSR's hook, a driver the guest installed itself. SIMINT_RUN is that rule, done
     for real through the 0302h nested-V86 machinery.
   ► SIMINT_FAST is the SAME ANSWER, computed without the trip into V86, and it is only
     taken where it provably is the same: the IVT still holds our own stub (`our_seg`),
     so "run the stub" and "call what the stub calls" are one thing. 0300h with BL=33h
     is Doom's mouse, twice a frame; a full PM->V86->PM round trip with the INT-site
     unpatch/repatch is not something to pay there for nothing.
   ⚠ INT 21h IS ALWAYS FAST, EVEN WHEN A GUEST HAS HOOKED THE REAL-MODE VECTOR. That is a
     known deviation, kept deliberately: every DOS/4GW, DOS/16M and krnl386 guest on the
     shelf was validated through the host-side INT 21h path, and a hooked real-mode INT 21h
     is exactly the case none of them has been seen to exercise. Changing it is its own
     ticket, with its own gate.
   ⚠ `reflect_on` = 0 (cfg\simintrefl_off.flag) is the pre-#247 shape, kept as the rig's
     rollback lever: 21h/33h/10h host-side whoever owns them, everything else not run.
   ⚠ A NULL VECTOR IS NOT RUN. 0000:0000 is the IVT itself; executing it as code is the
     GH #27 landmine. Startup points every null vector at our IRET stub, so this only
     fires if a guest zeroed one -- and then "not serviced" is the honest answer. */
#define SIMINT_FAST 1   /* host-side: dos_int21 / mouse_int33 / the video VDD          */
#define SIMINT_RUN  2   /* run IVT[vec] in V86 with an IRET frame (the 0302h machinery) */
#define SIMINT_NONE 3   /* not run; counted in `STAGE2: simInt (DPMI 0300) UNHANDLED`   */
#define SIMINT_VECTOR_VIDEO 0x10
#define SIMINT_VECTOR_DOS   0x21
#define SIMINT_VECTOR_MOUSE 0x33

static INT RmcsSimIntRoute(UINT vector, WORD ivtSegment, WORD ivtOffset,
                           INT isReflectOn, WORD ourSegment)
{
    if (vector == SIMINT_VECTOR_DOS) return SIMINT_FAST;
    if (vector == SIMINT_VECTOR_MOUSE || vector == SIMINT_VECTOR_VIDEO)
        if (ivtSegment == ourSegment || !isReflectOn) return SIMINT_FAST;
    if (!isReflectOn) return SIMINT_NONE;
    if (ivtSegment == 0 && ivtOffset == 0) return SIMINT_NONE;
    return SIMINT_RUN;
}

/* ── CX WORDS OF THE PROTECTED-MODE STACK, COPIED TO THE REAL-MODE ONE. ────────────────
     DPMI 0.9: 0300h/0301h/0302h take CX = "number of words to copy from the protected-
     mode stack to the real-mode stack". They go ABOVE the return frame, in the same
     order, so a procedure that reads its arguments at [bp+6]... finds them where a
     real-mode caller would have pushed them.
   ► Returns the new SP (below the copied words, above the frame) in *sp_after, and 1 --
     or 0 when `words` plus the `frame` bytes do not fit below `sp`. The caller then
     copies NOTHING and carries on exactly as before #247 (when CX was ignored), and
     says so in the log: a client passing a stale CX by accident must not be refused a
     call that has always worked, and one passing 30000 words is not asking for a copy
     any host could make.
   ⚠ SP = 0 IS A FULL 64 KB, not an empty stack: the first push wraps it to FFFEh. */
#define RMCS_STACK_FULL  0x10000u   /* SP = 0: all 64 KB                            */
#define RMCS_WORD_BYTES  2u
static INT RmcsStackPlan(WORD stackPointer, UINT words, UINT frame, PWORD stackPointerAfter)
{
    DWORD available = stackPointer ? (DWORD)stackPointer : RMCS_STACK_FULL;
    DWORD needed = (DWORD)words * RMCS_WORD_BYTES;
    if (needed + frame > available) { *stackPointerAfter = stackPointer; return 0; }
    *stackPointerAfter = (WORD)(available - needed);
    return 1;
}

#endif /* NTVDMEX_DPMI_RMCS_H */
