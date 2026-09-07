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
 * built by tools/dostest/run.sh.
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
ABI_ASSERT(regs_size, sizeof(ntvdmex_regs) == sizeof(ntvdd_regs));

#define ABI_OFF(name, m) \
    ABI_ASSERT(off_##name, offsetof(ntvdmex_regs, m) == offsetof(ntvdd_regs, m))
ABI_OFF(eax, eax); ABI_OFF(ebx, ebx); ABI_OFF(ecx, ecx); ABI_OFF(edx, edx);
ABI_OFF(esi, esi); ABI_OFF(edi, edi); ABI_OFF(ebp, ebp);
ABI_OFF(ds,  ds);  ABI_OFF(es,  es);  ABI_OFF(cf,  cf);

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
    ntvdd_in_fn      a1 = probe_in;    ntvdmex_in_fn    b1 = probe_in;
    ntvdd_out_fn     a2 = probe_out;   ntvdmex_out_fn   b2 = probe_out;
    ntvdd_rd_fn      a3 = probe_rd;    ntvdmex_rd_fn    b3 = probe_rd;
    ntvdd_wr_fn      a4 = probe_wr;    ntvdmex_wr_fn    b4 = probe_wr;
    ntvdd_frame_fn   a5 = probe_frame; ntvdmex_frame_fn b5 = probe_frame;
    return (a1 && b1 && a2 && b2 && a3 && b3 && a4 && b4 && a5 && b5) ? 0 : 1;
}
