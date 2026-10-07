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
static void AbiProbeIn(void *self, uint16_t port, uint8_t width, uint32_t *value)
{ (void)self; (void)port; (void)width; (void)value; }
static void AbiProbeOut(void *self, uint16_t port, uint8_t width, uint32_t value)
{ (void)self; (void)port; (void)width; (void)value; }
static uint8_t AbiProbeRead(void *self, uint32_t offset) { (void)self; (void)offset; return 0; }
static void AbiProbeWrite(void *self, uint32_t offset, uint8_t value) { (void)self; (void)offset; (void)value; }
static void AbiProbeFrame(void *self) { (void)self; }

int NtvdmexAbiCheck(void);
int NtvdmexAbiCheck(void)
{
    PVDD_PORT_IN_ROUTINE      treeIn    = AbiProbeIn;    ntvdmex_in_fn    sdkIn    = AbiProbeIn;
    PVDD_PORT_OUT_ROUTINE     treeOut   = AbiProbeOut;   ntvdmex_out_fn   sdkOut   = AbiProbeOut;
    PVDD_MEMORY_READ_ROUTINE  treeRead  = AbiProbeRead;  ntvdmex_rd_fn    sdkRead  = AbiProbeRead;
    PVDD_MEMORY_WRITE_ROUTINE treeWrite = AbiProbeWrite; ntvdmex_wr_fn    sdkWrite = AbiProbeWrite;
    PVDD_FRAME_ROUTINE        treeFrame = AbiProbeFrame; ntvdmex_frame_fn sdkFrame = AbiProbeFrame;
    return (treeIn && sdkIn && treeOut && sdkOut && treeRead && sdkRead && treeWrite && sdkWrite && treeFrame && sdkFrame) ? 0 : 1;
}
