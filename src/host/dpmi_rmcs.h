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
 *   ★ rmcs_write() therefore CANNOT write those four: it has no parameter for them.
 *     The old 0300h retarget wrote IVT[BL] into the caller's CS:IP to borrow the 0302h
 *     arm; that is exactly the write this shape makes impossible.
 */
#ifndef NTVDMEX_DPMI_RMCS_H
#define NTVDMEX_DPMI_RMCS_H

#include <stdint.h>

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
typedef struct {
    uint32_t edi, esi, ebp, ebx, edx, ecx, eax;
    uint16_t flags, es, ds, fs, gs;
} rmcs_regs;

/* Byte-assembled: the structure lives wherever the client put it, at any alignment. */
static uint32_t rmcs_rd32(const volatile uint8_t *r, unsigned o)
{
    return (uint32_t)r[o] | ((uint32_t)r[o + 1] << 8)
         | ((uint32_t)r[o + 2] << 16) | ((uint32_t)r[o + 3] << 24);
}
static uint16_t rmcs_rd16(const volatile uint8_t *r, unsigned o)
{
    return (uint16_t)(r[o] | (r[o + 1] << 8));
}
static void rmcs_wr32(volatile uint8_t *r, unsigned o, uint32_t v)
{
    r[o] = (uint8_t)v; r[o + 1] = (uint8_t)(v >> 8);
    r[o + 2] = (uint8_t)(v >> 16); r[o + 3] = (uint8_t)(v >> 24);
}
static void rmcs_wr16(volatile uint8_t *r, unsigned o, uint16_t v)
{
    r[o] = (uint8_t)v; r[o + 1] = (uint8_t)(v >> 8);
}

static void rmcs_read(const volatile uint8_t *r, rmcs_regs *o)
{
    o->edi = rmcs_rd32(r, RMCS_EDI); o->esi = rmcs_rd32(r, RMCS_ESI);
    o->ebp = rmcs_rd32(r, RMCS_EBP); o->ebx = rmcs_rd32(r, RMCS_EBX);
    o->edx = rmcs_rd32(r, RMCS_EDX); o->ecx = rmcs_rd32(r, RMCS_ECX);
    o->eax = rmcs_rd32(r, RMCS_EAX);
    o->flags = rmcs_rd16(r, RMCS_FLAGS);
    o->es = rmcs_rd16(r, RMCS_ES); o->ds = rmcs_rd16(r, RMCS_DS);
    o->fs = rmcs_rd16(r, RMCS_FS); o->gs = rmcs_rd16(r, RMCS_GS);
}

/* Everything the spec returns, nothing it does not. +0C (reserved) is not touched
   either: it is the client's. */
static void rmcs_write(volatile uint8_t *r, const rmcs_regs *i)
{
    rmcs_wr32(r, RMCS_EDI, i->edi); rmcs_wr32(r, RMCS_ESI, i->esi);
    rmcs_wr32(r, RMCS_EBP, i->ebp); rmcs_wr32(r, RMCS_EBX, i->ebx);
    rmcs_wr32(r, RMCS_EDX, i->edx); rmcs_wr32(r, RMCS_ECX, i->ecx);
    rmcs_wr32(r, RMCS_EAX, i->eax);
    rmcs_wr16(r, RMCS_FLAGS, i->flags);
    rmcs_wr16(r, RMCS_ES, i->es); rmcs_wr16(r, RMCS_DS, i->ds);
    rmcs_wr16(r, RMCS_FS, i->fs); rmcs_wr16(r, RMCS_GS, i->gs);
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

static int simint_route(unsigned vec, uint16_t ivt_seg, uint16_t ivt_off,
                        int reflect_on, uint16_t our_seg)
{
    if (vec == 0x21) return SIMINT_FAST;
    if (vec == 0x33 || vec == 0x10)
        if (ivt_seg == our_seg || !reflect_on) return SIMINT_FAST;
    if (!reflect_on) return SIMINT_NONE;
    if (ivt_seg == 0 && ivt_off == 0) return SIMINT_NONE;
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
static int rmcs_stack_plan(uint16_t sp, unsigned words, unsigned frame, uint16_t *sp_after)
{
    uint32_t avail = sp ? (uint32_t)sp : 0x10000u;
    uint32_t need = (uint32_t)words * 2u;
    if (need + frame > avail) { *sp_after = sp; return 0; }
    *sp_after = (uint16_t)(avail - need);
    return 1;
}

#endif /* NTVDMEX_DPMI_RMCS_H */
