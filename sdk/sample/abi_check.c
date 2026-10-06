/*
 * abi_check.c -- fail the BUILD if the SDK header and the in-tree ABI drift.
 * (GH #11)
 *
 * The SDK header is deliberately self-contained: a third-party developer must be
 * able to compile against it with nothing from src/. That duplication is the
 * price, and an unchecked duplicate is a lie waiting to happen -- change a
 * callback signature in src/vdd/ntvdd.h and every existing driver silently
 * mis-calls the host, with no diagnostic anywhere.
 *
 * So: include BOTH, and make the compiler compare them. This file produces no
 * code; its only job is to stop compiling when someone changes one side. It is
 * built by tests/probes/dos/run.sh.
 *
 * ⚠ A RUNTIME CHECK WOULD BE TOO LATE. By the time a mismatched driver is
 *   loaded, the wrong bytes are already on the stack.
 */
#include <stddef.h>
#include "ntvdmex-vdd.h"
#include "ntvdd.h"

/* C89-compatible static assert: a negative-width array is a hard error. */
#define ABI_ASSERT(name, cond) typedef char abi_assert_##name[(cond) ? 1 : -1]

/* ── the register struct, which crosses the ABI by pointer ────────────────── */
ABI_ASSERT(regs_size, sizeof(ntvdmex_regs) == sizeof(NTVDD_REGISTERS));

/* The SDK keeps its own (frozen, public) member names; the in-tree ones follow the
   house style since #333, so each pair is named on both sides. */
#define ABI_OFF(sdkMember, treeMember) \
    ABI_ASSERT(off_##sdkMember, offsetof(ntvdmex_regs, sdkMember) == offsetof(NTVDD_REGISTERS, treeMember))
ABI_OFF(eax, Eax); ABI_OFF(ebx, Ebx); ABI_OFF(ecx, Ecx); ABI_OFF(edx, Edx);
ABI_OFF(esi, Esi); ABI_OFF(edi, Edi); ABI_OFF(ebp, Ebp);
ABI_OFF(ds,  Ds);  ABI_OFF(es,  Es);  ABI_OFF(cf,  CarryFlag);

/* ── the callback signatures ──────────────────────────────────────────────────
     sizeof cannot see a parameter list, so the real proof is ASSIGNMENT: each
     line below only compiles if the two declarations are compatible, and the
     compiler checks both directions. */
static void probe_in (void *s, uint16_t p, uint8_t w, uint32_t *v)
{ (void)s; (void)p; (void)w; (void)v; }
static void probe_out(void *s, uint16_t p, uint8_t w, uint32_t v)
{ (void)s; (void)p; (void)w; (void)v; }
static uint8_t probe_rd(void *s, uint32_t o) { (void)s; (void)o; return 0; }
static void probe_wr(void *s, uint32_t o, uint8_t v) { (void)s; (void)o; (void)v; }
static void probe_frame(void *s) { (void)s; }

int ntvdmex_abi_check(void);
int ntvdmex_abi_check(void)
{
    PVDD_PORT_IN_ROUTINE  a1 = probe_in;    ntvdmex_in_fn    b1 = probe_in;
    PVDD_PORT_OUT_ROUTINE  a2 = probe_out;   ntvdmex_out_fn   b2 = probe_out;
    PVDD_MEMORY_READ_ROUTINE  a3 = probe_rd;    ntvdmex_rd_fn    b3 = probe_rd;
    PVDD_MEMORY_WRITE_ROUTINE  a4 = probe_wr;    ntvdmex_wr_fn    b4 = probe_wr;
    PVDD_FRAME_ROUTINE  a5 = probe_frame; ntvdmex_frame_fn b5 = probe_frame;
    return (a1 && b1 && a2 && b2 && a3 && b3 && a4 && b4 && a5 && b5) ? 0 : 1;
}
