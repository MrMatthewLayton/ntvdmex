/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Protected mode: the DPMI host -- descriptors and the LDT, fault trampolines,
 *   code patching and breakpoints, callbacks, PM IRQ injection, client teardown.
 *
 * Its own translation unit (#335): declared in host_dpmi.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "host_dpmi.h"
#include "host_state.h"
#include "log.h"
#include "ne.h"
#include "wow32.h"
#include "wowanchors.h"
#include "wowsched.h"
#include "wowcall.h"
#include "wowmsg.h"
#include "wowres.h"
#include "wowwin.h"
#include "wowgdi.h"
#include "wowuser.h"
#include "main.h"
#include "host_dpmi_int.h"
#include "host_bios.h"
#include "host_io.h"
#include "host_irq.h"
#include "host_mouse.h"
#include "host_timing.h"
#include "host_wow.h"

/* Highest linear address XP's LDT descriptor validator will accept for base+limit
 * (MmHighestUserAddress on a 2GB-user build). Kernel RE session 7 recovered the rule
 * from PspIsDescriptorValid; run 30 confirmed base 0 / limit 0x7FFEF / G=1 installs
 * while a true 4GB selector does not. Used to clamp a client's flat selector.
 */
#define XP_LDT_MAX_LINEAR   0x7FFEFFFFu

#define DPMI_FAULT_BOP      0x57    /* BOP number planted at the handler code:COFF */

/* Where the client's exception handler's FAR RETURN lands. DPMI 0.9 puts a return CS:IP
 * at the bottom of the exception frame and the handler exits through it with a `retf`
 * -- krnl386's handler exits exactly that way (observed), having first rewritten the
 * frame's CS:IP slots to say where it wants execution to resume. The kernel leaves those
 * two words ZERO for the host to fill (measured, session 34), so this is the address we
 * fill them with, and the arm that catches it completes the resume.
 */
#define DPMI_FLTRET_BOP     0x5A

/* GUEST BREAKPOINTS IN PROTECTED MODE:
 * THE PROBLEM THIS EXISTS FOR, because it has now cost three sessions. When a PM
 * client dies, the kernel terminates the whole VDM: no exception reaches our VEH, the
 * fault trampoline does not catch, and the log simply stops. All we ever learn is the
 * last INT the client executed -- and between two INTs there can be thousands of
 * instructions. Session 17 broke one such wall by dumping the guest stack and reading
 * DOS/4GW's handoff frame, but that worked only because the dead stretch happened to
 * end in a far transfer whose operands were in memory. The next stretch is straight-
 * line code, and that trick does not generalise.
 * A breakpoint does. The INT->BOP patch already proves we can make the guest stop at a
 * chosen byte and hand us its full register file; a breakpoint is the same mechanism
 * pointed at an address WE choose rather than one the client's INT happens to sit on.
 * Bisecting a dead stretch then costs one run per step instead of one rebuild per idea.
 * DRIVEN FROM A FILE ON THE SHARE, deliberately: addresses come from disassembling the
 * client, they change every time you halve the interval, and a rebuild-and-deploy cycle
 * per guess is the thing that makes people stop bisecting and start speculating.
 * ONE-SHOT by design: on hit we restore the original bytes and do NOT advance EIP, so
 * the real instruction executes and the client runs on undisturbed. A loop therefore
 * reports its first pass, not its ten-thousandth.
 */
#define PMBP_PATH           CFG_("pmbp.txt")
UINT g_DpmiCpMaximum = 8;
static DWORD g_BreakpointLinear[DPMI_BP_MAX];     /* requested linear addresses (from PMBP_PATH) */
DWORD g_BreakpointDump[DPMI_BP_MAX];    /* optional 2nd column: linear addr to dump on hit */
/* Optional 3rd column: bytes to SKIP on hit instead of re-executing the instruction.
 * This turns a breakpoint into a one-instruction PATCH, which is how you test "would
 * the client survive if this instruction simply did not happen?" without a rebuild.
 * Session 17 needed exactly that for a `STI`: at CPL 3 with IOPL 0 it raises the one
 * #GP XP will not reflect, so it kills the VDM -- and the question "is STI the only
 * thing in the way" is answerable in one run by skipping it.
 */
DWORD g_BreakpointSkip[DPMI_BP_MAX];
/* Optional 4th column: 1 = plant a ONE-BYTE INT3 (0xCC) instead of the two-byte BOP.
 * This exists to answer one question that forks the whole CLI/STI strategy: does a
 * protected-mode TRAP reach our VEH at all? Runs 20-34 had the kernel reflecting PM
 * SOFTWARE interrupts to us (the SegCs==0x1B arm), and #BP is software-generated, so
 * there is reason to hope -- but hoping is not measuring. An INT3 in guest code arrives
 * with the GUEST's CS, so it falls to DpmiCrashVeh's fatal arm and prints
 * "DPMI FATAL: exception code=0x80000003". Seeing that line instead of a silent death
 * is the answer. A one-byte trap is also the only patch that FITS over CLI/STI.
 */
DWORD g_BreakpointMode[DPMI_BP_MAX];
BYTE  g_BreakpointPending[DPMI_BP_MAX];  /* skipped -> needs re-arming once EIP moves on */
/* Optional 5th column: 1 = REPEATING. One-shot is right for a "how far did it get"
 * sweep, and useless for a loop -- the first pass eats every breakpoint and the failing
 * iteration is the thousandth. A repeating breakpoint cannot re-plant itself while the
 * guest is standing on its footprint, so it re-arms the way skip mode does: mark it
 * pending and let the NEXT event (typically the other breakpoint in the same loop) put
 * it back. Put at least TWO repeating breakpoints in a loop and they alternate, which
 * gives a register dump per iteration.
 */
DWORD g_BreakpointReport[DPMI_BP_MAX];
static BYTE  g_BreakpointOriginal[DPMI_BP_MAX][DPMI_PM_BOP_LENGTH]; /* the two bytes we displaced */
static BYTE  g_BreakpointArmed[DPMI_BP_MAX];
/* [CAUTION]: A REFUSAL IS A STANDING CONDITION, NOT AN EVENT. (session 59) (Importance = 2):
 * DpmiBreakpointArm() runs before EVERY PM entry, and both REFUSED arms below `continue`
 * without incrementing g_BreakpointArms -- so DPMI_BP_ARM_MAX, the ceiling that exists to
 * stop exactly this, never applies to them. A breakpoint on a one-byte instruction
 * therefore re-reports itself for the whole run: **489,200 lines and an 88 MB log**,
 * measured, from ONE misplaced breakpoint. The message is worth having (it names the
 * fix), but the guest cannot act on it and neither can the reader after the first.
 * - So say it ONCE PER BREAKPOINT and then stay quiet. Not once globally: two
 *   breakpoints refused for two different reasons must both be legible, which is the
 *   same per-key argument the reflected-INT trace needed this session.
 */
static BYTE  g_BreakpointRefused[DPMI_BP_MAX];
/* How many times each breakpoint has been PLANTED, and the ceiling. A repeating
 * breakpoint on a hot instruction is a log bomb: one mis-sequenced re-arm fired
 * 340,808 times and produced a 268 MB log in a single run. Past the ceiling the site
 * is simply left unarmed -- it has stopped answering a question by then.
 */
#define DPMI_BP_ARM_MAX     512
static DWORD g_BreakpointArms[DPMI_BP_MAX];
INT g_DpmiBlockCount = 0;
DWORD g_DpmiOwned[DPMI_OWNED_MAX];  /* live 0501 blocks (VirtualAlloc bases) */
INT   g_DpmiOwnedCount = 0;
#define DPMI_LE_MIN_CODE_SIZE   0x10000u    /* Only code objects this big are matched (see g_LeCodeSize) */
INT   g_LeCodeCount = 0;

/* How far an injected protected-mode ISR may run before we stop waiting for its IRET.
 * See the commentary at the phase loop in DpmiInjectPmIrq(): a phase is one PM entry,
 * not a unit of time, so the real bound is the clock; the phase count is only a backstop
 * against a handler that traps forever without making progress.
 */
#define DPMI_IRQ0_PHASE_MAX     65536u
#define DPMI_IRQ0_MS_MAX        500u
WORD  g_PmDefaultSelector  = 0;      /* code selector over the stub block */
static DWORD g_PmDefaultBase = 0;      /* its linear base */
static INT   g_PmDefaultIndex  = -1;     /* its LDT slot, so its D/B can follow the client */
static INT   g_PmDefaultFromDos = 0; /* the block came from the DOS arena, not the host pool */
/* [INFO]: The host's private LDT pool sits between the reserved entries and the
 * client's arena; see the long note at DpmiHostIndex(). The range is MEASURED
 * (the guest never touches 0x09..0x2b across three full runs), and the
 * client-facing counter starts ABOVE it so the two can never meet.
 */
#define DPMI_HOSTPOOL_LO    0x09
WORD  g_LdtFree[DPMI_LDT_MAX];   /* recycled indices, LIFO */
INT   g_LdtFreeCount = 0;
INT   g_PmWatchCount      = 0;
static DWORD g_PmIrq0Done   = 0;           /* cooperative injections that reached an IRET */
/* THE OTHER HALF OF THE DELIVERY ACCOUNT:
 * `pit budget` reads "raises 144/s ... delivered 56/s" and concludes the guest's clock
 * runs at 39% of the rate it programmed. But `delivered` is g_AsyncInjectedLine[0] -- the
 * ASYNCHRONOUS arm only -- and the client's INT 08h handler is entered by TWO
 * mechanisms: that one, and the cooperative DpmiInjectPmIrq() the PM loop runs (the
 * per-pass latch at #2b, and the catch-up batch on the catcher's return). Putting an
 * async-only counter next to `raises` invites reading it as the total, which is a
 * units error of exactly the kind that has cost this project rig runs before.
 * Count the cooperative arm per VECTOR and print both arms against `raises`, so the
 * line answers the question it appears to answer.
 */
DWORD g_PmCooperativeLine[PIC_LINES_PER_CHIP];
WORD      g_PmTransferParagraphs = 0;

/* True if selector `sel`'s descriptor has the D/B (32-bit default) bit set. The bit
 * lives in g_Ldt[].flags bit 2 (descriptor byte-6 bit 6). All 16-bit DPMI clients leave
 * it 0; a DOS/4GW-class 32-bit code selector sets it (via INT 31h 0009).
 */
INT DpmiSelectorIs32(WORD selector)
{
    INT index = DPMI_SELECTOR_INDEX(selector & WORD_MASK);

    if (index < 1 || index >= DPMI_LDT_LEGACY_LIMIT)
        return 0;
    return (g_Ldt[index].Flags & DPMI_DESCRIPTOR_FLAG_BIG) != 0;
}

/* A HOST-PRIVATE LDT POOL, BECAUSE krnl386 IS A SECOND ALLOCATOR (Importance = 5):
 * (session 48) MS Paint and Notepad both died on `File > Save As` with a #GP in
 * KRNL386.EXE, reading the current-drive byte out of the DOS structures krnl386
 * located at boot. For those it asks DPMI Segment-to-Descriptor (`INT 31h
 * AX=0002`) at start-up, and that call succeeded (our log):
 *
 *   INT31h AX=0002 BX=0x50    -> sel 0x018f          ; idx 0x31, limit 0xFFFF
 *   INT31h AX=000C BX=0x018f  <- base=0x0002a800 limit=0x031f
 *   INT31h AX=000C BX=0x018f  <- base=0x03b4c1c0 limit=0x003f
 *   LDTSYNC idx 0x31 <- guest wrote ... INSTALL FAILED
 *
 * **krnl386 reuses the index we took.** It never asked for it: it keeps its
 * own idea of which LDT entries are free, reaches them through `DPMI 000C` and
 * by writing the descriptor shadow directly, and cannot know `g_LdtNext` had
 * already handed 0x31 out. Two allocators over one table.
 *
 * [CAUTION]: NOT "freed and recycled". That was the first explanation and it is refuted
 * by the same logs: `grep -c recycled` is 0 across three full runs and there is
 * no `DPMI 0001` on that selector anywhere. Nothing was freed.
 *
 * [INFO]: THE POOL IS MEASURED, NOT CHOSEN. Every LDT index the guest touches was
 * harvested out of three logs already on disk (a WOW bootstrap, a Paint
 * session, a Save As) -- 348 distinct indices, `0x001..0x189`, dense from
 * `0x30` upward with a few singletons at `0x1`, `0x2`, `0x3`, `0x8`, `0x2c`.
 * The largest untouched run is **`0x09..0x2b`, 35 entries**, and a whole run
 * mints only **11** host selectors, so the headroom is 3x.
 *
 * [INFO]: And it explains why this took until now to bite: `g_LdtNext` starts at 6 and
 * krnl386's arena starts at 0x30, so the two only collide for the handful of
 * allocations made while the counter is passing through the low 0x30s. The
 * SysVars selector was one; `seg 0x1ef3 -> sel 0x17f` (idx 0x2f) missed by one.
 *
 * [CAUTION]: ON EXHAUSTION IT FALLS BACK to the shared counter and says so, because a
 * host that stops minting selectors is worse than one that risks the old bug.
 */
static INT g_HostPoolNext = DPMI_HOSTPOOL_LO;
INT g_HostPoolSpill = 0;

INT DpmiHostIndex(VOID)
{
    if (g_HostPoolNext <= DPMI_HOSTPOOL_HI)
        return g_HostPoolNext++;
    if (g_LdtNext >= DPMI_LDT_MAX)
        return -1;
    g_HostPoolSpill = 1;
    return g_LdtNext++;
}

/* A 16-bit CODE selector based on DOS_HDLR_SEG, so the host's own stubs (the 0306
 * protected-to-real entry, the 0305 save/restore no-op) have a protected-mode address
 * to hand the client. Allocated once, from the host-private pool above, and cached --
 * 0305 and 0306 both want it and a client may call either more than once.
 */
WORD g_DpmiHandlerSelector = 0;
WORD DpmiHandlerCodeSelector(VOID)
{
    INT index;

    if (g_DpmiHandlerSelector)
        return g_DpmiHandlerSelector;
    index = DpmiHostIndex();
    if (index < 0)
        return 0;
    g_Ldt[index].Base   = (DWORD)DOS_HDLR_SEG << PARAGRAPH_SHIFT;
    g_Ldt[index].Limit  = X86_SEGMENT_LIMIT_64K;
    g_Ldt[index].Access = DPMI_ACCESS_CODE;                 /* present, DPL3, code, readable */
    g_Ldt[index].Flags  = 0;                    /* 16-bit: the stubs are 16-bit code */
    DpmiInstall(index);
    g_DpmiHandlerSelector = (WORD)DPMI_LDT_SELECTOR(index);
    return g_DpmiHandlerSelector;
}

/* DPMI 0002: SEGMENT TO DESCRIPTOR:
 * Hand back a selector whose base is a real-mode paragraph address and whose limit
 * is 64K-1. Trivially small, and it was missing -- which mattered more than it
 * sounds: it is the DPMI function krnl386 calls MOST at start-up (per our INT 31h
 * log), because that is how the 16-bit
 * Windows kernel reaches the BIOS data area, the DOS list-of-lists, and everything
 * else it knows only as a paragraph.
 *
 * [CAUTION]: THE SAME SEGMENT MUST GIVE THE SAME SELECTOR. The descriptor belongs to the HOST,
 * not the client -- the client is told never to modify or free it -- so handing out
 * a fresh LDT entry per call would leak a descriptor per call and let a client
 * modify a mapping another part of it is still using. Hence the cache.
 * If a client frees one anyway (0001), the cache entry is dropped there, so the
 * next 0002 builds a fresh one rather than returning a selector that is no longer
 * present. That is the failure this avoids: a stale mapping faults far from here.
 */
#define DPMI_S2D_MAX    64
static WORD g_SegmentToDescriptorSegment[DPMI_S2D_MAX];
static WORD g_SegmentToDescriptorSelector[DPMI_S2D_MAX];
static INT  g_SegmentToDescriptorCount = 0;

WORD DpmiSegmentToDescriptor(WORD segment)
{
    INT index;
    INT ldtIndex;

    for (index = 0; index < g_SegmentToDescriptorCount; ++index)
        if (g_SegmentToDescriptorSegment[index] == segment)
            return g_SegmentToDescriptorSelector[index];
    /* [INFO]: THE HOST-PRIVATE POOL, and this is the call that proved it necessary --
     * krnl386 keeps the selector this returns for the life of the VDM.
     */
    ldtIndex = DpmiHostIndex();
    if (ldtIndex < 0)
        return 0;
    g_Ldt[ldtIndex].Base   = (DWORD)segment << PARAGRAPH_SHIFT;
    g_Ldt[ldtIndex].Limit  = X86_SEGMENT_LIMIT_64K;
    g_Ldt[ldtIndex].Access = DPMI_ACCESS_DATA;                /* present, DPL3, data, read/write */
    g_Ldt[ldtIndex].Flags  = 0;                   /* 16-bit */
    DpmiInstall(ldtIndex);
    if (g_SegmentToDescriptorCount < DPMI_S2D_MAX)
    {
        g_SegmentToDescriptorSegment[g_SegmentToDescriptorCount] = segment;
        g_SegmentToDescriptorSelector[g_SegmentToDescriptorCount] = (WORD)DPMI_LDT_SELECTOR(ldtIndex);
        ++g_SegmentToDescriptorCount;
    }
    return (WORD)DPMI_LDT_SELECTOR(ldtIndex);
}

VOID DpmiSegmentToDescriptorForget(WORD selector)
{
    INT index;

    for (index = 0; index < g_SegmentToDescriptorCount; ++index)
        if (g_SegmentToDescriptorSelector[index] == selector)
        {
            g_SegmentToDescriptorSegment[index] = g_SegmentToDescriptorSegment[g_SegmentToDescriptorCount - 1];
            g_SegmentToDescriptorSelector[index] = g_SegmentToDescriptorSelector[g_SegmentToDescriptorCount - 1];
            --g_SegmentToDescriptorCount;
            return;
        }
}

enum
{
    DPMI_PMDEF_PARAS = 0x40
};   /* the 256 default PM stubs, DPMI_PMDEF_STRIDE bytes each */
/* Plant the default PM interrupt handlers and point every vector at them. See the
 * note on g_PmDefaultSelector. Called once, at the mode switch, before the client runs.
 */
VOID DpmiInstallDefaultPmHandlers(DOS_MACHINE *machine)
{
    WORD segment = 0;
    WORD maximum = 0;
    volatile BYTE *stub;
    INT vector;
    INT index;
    CHAR lineBuffer[160];
    CHAR *cursor = lineBuffer;

    /* A SECOND CLIENT (s80): the table is the host's and outlives the first client.
     * If its memory survived teardown (the host pool), point every vector back at it.
     * If it came from DOS memory, teardown gave the block back to the parent and kept
     * only the selector: fall through, allocate again, and rebase that same slot --
     * allocating a new one would leak an LDT slot per program run.
     */
    if (g_PmDefaultSelector && g_PmDefaultBase)
    {
        for (vector = 0; vector < IVT_VECTORS; ++vector)
        {
            g_PmInt[vector].Selector = g_PmDefaultSelector;
            g_PmInt[vector].Offset = (DWORD)vector * DPMI_PMDEF_STRIDE;
            g_PmInt[vector].Client = 0;
        }
        cursor = LogPut(cursor, "DPMI: default PM handlers reused at 0x"); cursor = LogHex(cursor, g_PmDefaultSelector);
        cursor = LogPut(cursor, " (a previous client's table)\r\n");
        LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
        return;
    }
    /* 256 vectors x 3 bytes = 768; one 0x40-paragraph block covers it with room over.
     * On the WOW path this MUST come from the host pool -- krnl386 owns the rest of
     * conventional memory by now, and a silent failure here shows up as INT 21h AH=35h
     * reporting vector 0x21 as 0000:0000, which reads like a guest bug.
     */
    /* [CAUTION]: AND SAY SO WHEN IT FAILS. Both of these used to `return` in silence, which is
     * how the note above got to be true twice: growing the host pool's other tenant
     * pushed this allocation out, the table was never built, and the run died at PM
     * step 0x0d with nothing in the log pointing here. A silent return from the
     * function that installs 256 interrupt vectors is not a small omission.
     */
    segment = WowHostAllocate(DPMI_PMDEF_PARAS);
    g_PmDefaultFromDos = !segment;
    if (!segment && (DosMcbAllocate(NULL, machine->FirstMcb, DPMI_PMDEF_PARAS, &segment, &maximum) || !segment))
    {
        cursor = LogPut(cursor, "DPMI: NO MEMORY for the 256-vector default PM handler table "
                    "(host pool exhausted AND dos_alloc failed) -- every PM vector will "
                    "read back 0000:0000\r\n");
        LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
        return;
    }
    if (g_PmDefaultIndex < 0 && g_LdtNext >= DPMI_LDT_MAX)
    {
        cursor = LogPut(cursor, "DPMI: NO LDT SLOT for the default PM handler table\r\n");
        LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
        return;
    }
    g_PmDefaultBase = (DWORD)segment << PARAGRAPH_SHIFT;
    stub = (volatile BYTE *)(ULONG_PTR)g_PmDefaultBase;
    index = (g_PmDefaultIndex >= 0) ? g_PmDefaultIndex : g_LdtNext++;   /* a previous client's slot, rebased */
    g_Ldt[index].Base   = g_PmDefaultBase;
    g_Ldt[index].Limit  = DPMI_PMDEF_PARAS * PARAGRAPH_SIZE - 1;
    g_Ldt[index].Access = DPMI_ACCESS_CODE;                    /* present, DPL3, code, readable */
    g_Ldt[index].Flags  = 0;                       /* 16-bit: the stubs are 16-bit */
    DpmiInstall(index);
    g_PmDefaultSelector = (WORD)DPMI_LDT_SELECTOR(index);
    for (vector = 0; vector < IVT_VECTORS; ++vector)
    {
        DWORD offset = (DWORD)vector * DPMI_PMDEF_STRIDE;
        stub[offset + 0] = VDM_BOP0;
        stub[offset + 1] = VDM_BOP1;
        stub[offset + DPMI_PM_BOP_LENGTH] = X86_OP_IRET;                    /* BOP immediate AND the IRET */
        PatchMapSet(g_PmDefaultBase + offset, (BYTE)vector);   /* resolves the chained BOP -> vector v */
        g_PmInt[vector].Selector = g_PmDefaultSelector;
        g_PmInt[vector].Offset = offset;
        g_PmInt[vector].Client = 0;
    }
    g_PmDefaultIndex = index;      /* so the width can be corrected once the client declares it */
    cursor = LogPut(cursor, "DPMI: default PM handlers at 0x"); cursor = LogHex(cursor, g_PmDefaultSelector);
    cursor = LogPut(cursor, ":0 (linear 0x"); cursor = LogHex(cursor, g_PmDefaultBase);
    cursor = LogPut(cursor, ", 256 vectors)\r\n");
    LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
}

/* (Re)build g_Ldt[idx]'s descriptor and install it in the process LDT via svc 10. */
VOID DpmiInstall(INT index)
{
    DWORD low;
    DWORD high;
    DWORD descriptorLimit = g_Ldt[index].Limit;
    BYTE flags = g_Ldt[index].Flags;
    BYTE accessRights = g_Ldt[index].Access;
    WORD selector = (WORD)DPMI_LDT_SELECTOR(index);

    if (descriptorLimit > X86_DESCRIPTOR_LIMIT_MAX) /* >1MB -> page granular */
    {
        descriptorLimit >>= PAGE_SHIFT;
        flags = (BYTE)(flags | DPMI_DESCRIPTOR_FLAG_GRANULARITY);
    }
    /* Run 69 (option C: avoid the kernel PM-fault reflect). On the REAL CPU a data/stack
     * selector that a client retypes to CODE (or non-writable) faults the moment it is used
     * -- exactly i310102's wall: its C runtime does INT 31h 0009 to set SS (sel 0x1F, idx 3)
     * access = 0xFB (code), then the next stack write #GPs and the kernel cannot reflect it
     * (runs 51/52), so the VDM silently terminates. The interpreter (runs 53+) survived this
     * by NOT enforcing descriptor types. We do the same on the real CPU: the initial DATA (idx
     * 2 = DS) and STACK (idx 3 = SS) selectors are ALWAYS installed as present writable-data,
     * so they stay usable no matter how the client retypes them. g_Ldt[idx].access keeps the
     * client's requested value, so LAR/LSL introspection (DpmiSelectorDescriptor) still reports it.
     */
    if (index == DPMI_INITIAL_DATA_INDEX || index == DPMI_INITIAL_STACK_INDEX)
        accessRights = DPMI_ACCESS_DATA;                                                                                             /* present, DPL3, data R/W */
    /* DPL IS THE HOST'S, NOT THE CLIENT'S. (s74c, ZAR's VESA modes):
     * ZAR builds its LFB selector with 0009 CX=8092: DPL 0. On real DOS that is
     * legal because DOS/4GW runs the client at ring 0; under a ring-3 DPMI host
     * it is a descriptor nobody could load, and NT's PspIsDescriptorValid
     * REJECTS any LDT entry whose DPL is not 3 (0xC000011A), so the selector was
     * never installed and the first `rep stosd` into the frame buffer #GP'd.
     * The DPMI spec has 0009 take the client's CPL for the DPL; here that is
     * always 3. g_Ldt[idx].access keeps the requested byte for LAR. Only a
     * PRESENT descriptor is touched: a freed selector is installed as the all-zero
     * null descriptor, which is the one non-DPL-3 entry NT accepts.
     */
    if (accessRights & X86_DESCRIPTOR_PRESENT)
        accessRights = (BYTE)(accessRights | X86_DESCRIPTOR_DPL3);
    DpmiBuildDescriptor(g_Ldt[index].Base, descriptorLimit & X86_DESCRIPTOR_LIMIT_MAX, accessRights, flags, &low, &high);
    {
        /* #3 (DOS/4GW flat model): XP's LDT validator caps base+limit <= MmHighestUserAddress
         * (~2GB); a base-0 ~2GB G=1 selector installs, a true 4GB one is REJECTED (Kernel RE
         * session 7). Surface the NTSTATUS so a rejected flat/large descriptor is visible
         * instead of silently leaving a stale selector that faults on first use (run 84).
         */
        LONG status;
        /* [CAUTION]: HOST-SIDE CHANGES MUST REACH THE SHADOW TOO, OR THE SYNC EATS THEM.
         * WowShadowSync() treats "shadow differs from g_Ldt[]" as "the guest wrote
         * this". A descriptor WE allocate after the shadow was created is zero in the
         * shadow and non-zero in g_Ldt[], which reads as the guest having zeroed it --
         * and the sync then pushes those zeros into the real LDT, destroying a live
         * descriptor. Measured on the rig the moment the shadow shipped: three
         * LDTSYNC lines saying "guest wrote base=0 limit=0 acc=0" for indices the
         * guest had never touched. Keeping both sides in step here is what makes the
         * difference test mean what it says.
         */
        WowShadowPut(index);
        status = VdmInstallLdtEntries(selector, low, high, selector, low, high); /* idempotent single-entry */
        if (status != 0)
        {
            /* CLAMP AND RETRY, don't just report:
             * DOS/4GW (Doom) allocates a base-0 4GB G=1 FLAT selector and XP rejects it
             * (0xC000011A): PspIsDescriptorValid requires
             *   base + (G ? ((limit<<12)|0xFFF) : limit) <= MmHighestUserAddress.
             * Merely logging the refusal leaves the client holding a selector that is
             * not installed, so its first flat access faults -- which is a silent death,
             * exactly the failure mode this project keeps being bitten by. XP cannot be
             * talked into a true 4GB LDT selector (Kernel RE session 7: stock ntvdm is
             * under the same cap), so the honest best effort is the largest descriptor
             * the validator WILL take, which run 30 already showed installs: base 0,
             * limit 0x7FFEF, G=1.
             * The clamp is LOUD -- a client that then walks off the end of a segment it
             * believes is 4GB must not look like a mystery.
             */
            DWORD cap = XP_LDT_MAX_LINEAR;
            DWORD descriptorLow = low;
            DWORD descriptorHigh = high;
            DWORD want = g_Ldt[index].Limit;
            DWORD cl = 0;
            LONG state2 = status;
            if (g_Ldt[index].Base < cap)
            {
                DWORD room = cap - g_Ldt[index].Base;
                if (flags & DPMI_DESCRIPTOR_FLAG_GRANULARITY)
                    cl = (room > PAGE_LAST_BYTE_U) ? ((room - PAGE_LAST_BYTE_U) >> PAGE_SHIFT) : 0;                                            /* G=1 */
                else
                    cl = room;                                                  /* G=0 */
                if (cl > X86_DESCRIPTOR_LIMIT_MAX)
                    cl = X86_DESCRIPTOR_LIMIT_MAX;
                DpmiBuildDescriptor(g_Ldt[index].Base, cl, accessRights, flags, &descriptorLow, &descriptorHigh);
                state2 = VdmInstallLdtEntries(selector, descriptorLow, descriptorHigh, selector, descriptorLow, descriptorHigh);
            }
            {
                CHAR lineBuffer[256];
                CHAR *cursor = lineBuffer;
                cursor = LogPut(cursor, "DPMI-LDT: install REJECTED sel 0x"); cursor = LogHex(cursor, selector);
                cursor = LogPut(cursor, " base 0x"); cursor = LogHex(cursor, g_Ldt[index].Base);
                cursor = LogPut(cursor, " limit 0x"); cursor = LogHex(cursor, want);
                cursor = LogPut(cursor, " g="); cursor = LogHex(cursor, (DWORD)((flags >> 3) & 1));
                cursor = LogPut(cursor, " status 0x"); cursor = LogHex(cursor, (DWORD)status);
                if (state2 == 0) { cursor = LogPut(cursor, " -> CLAMPED to limit 0x"); cursor = LogHex(cursor, cl);
                                cursor = LogPut(cursor, " (XP LDT cap) and installed"); }
                else          { cursor = LogPut(cursor, " -> clamp ALSO refused, status 0x");
                                cursor = LogHex(cursor, (DWORD)state2); }
                cursor = LogPut(cursor, "\r\n");
                LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
            }
        }
    }
}

#define DPMI_FAULT_TRAMPOLINE_LDT_LIMIT     510     /* The trampoline takes two descriptors */

/* GH #18 (run 67 corrected): install the PM-fault reflect machinery. Two LDT selectors:
 * a writable-DATA stack selector (g_DpmiFaultSelector, based at the host's g_FaultStack so
 * its :0x1000 is a valid scratch stack top) written to [TIB+0x638]; and a CODE selector
 * (g_DpmiFaultCodeSelector, based at DOS_HDLR_SEG<<4) with a BOP at DPMI_FAULT_COFF. The
 * handler table g_FaultTable[class]=({code_sel,COFF}) is what the kernel reads via
 * [VDM_TIB+8] to set the reflected CS:EIP. Allocated once from the g_Ldt[] pool.
 */
VOID DpmiInstallFaultTrampoline(VOID)
{
    INT stackIndex;
    INT codeIndex;
    UINT index;
    volatile BYTE *handlerArea = (volatile BYTE *)(ULONG_PTR)((DWORD)DOS_HDLR_SEG << PARAGRAPH_SHIFT);

    if (g_DpmiFaultSelector)
        return;                                       /* already installed */
    if (g_LdtNext >= DPMI_FAULT_TRAMPOLINE_LDT_LIMIT)
        return;
    /* the handler CODE selector + its BOP */
    codeIndex = g_LdtNext++;
    g_Ldt[codeIndex].Base   = (DWORD)DOS_HDLR_SEG << PARAGRAPH_SHIFT;   /* 0x500 -> code:COFF = linear 0x580 */
    g_Ldt[codeIndex].Limit  = X86_SEGMENT_LIMIT_64K;
    g_Ldt[codeIndex].Access = DPMI_ACCESS_CODE;                       /* code exec/read, DPL3, present */
    g_Ldt[codeIndex].Flags  = 0;
    DpmiInstall(codeIndex);
    g_DpmiFaultCodeSelector = (WORD)DPMI_LDT_SELECTOR(codeIndex);
    handlerArea[DPMI_FAULT_COFF + 0] = VDM_BOP0;          /* plant C4 C4 57 at code:COFF (0x580) */
    handlerArea[DPMI_FAULT_COFF + 1] = VDM_BOP1;
    handlerArea[DPMI_FAULT_COFF + VDM_BOP_NUMBER_OFFSET] = DPMI_FAULT_BOP;
    /* the handler STACK selector (writable-data) */
    stackIndex = g_LdtNext++;
    g_Ldt[stackIndex].Base   = (DWORD)(ULONG_PTR)g_FaultStack;   /* #205: not guest memory */
    g_Ldt[stackIndex].Limit  = X86_SEGMENT_LIMIT_64K;
    g_Ldt[stackIndex].Access = DPMI_ACCESS_DATA;                       /* data read/write, DPL3, present */
    g_Ldt[stackIndex].Flags  = 0;
    DpmiInstall(stackIndex);
    g_DpmiFaultSelector = (WORD)DPMI_LDT_SELECTOR(stackIndex);
    /* the per-fault-class handler table: entry N (stride 0x10) = {CS@+0 word, EIP@+4 dword}.
     * Fill the #GP class (6) with our code selector + BOP offset; zero the rest.
     */
    for (index = 0; index < sizeof g_FaultTable; ++index)
        g_FaultTable[index] = 0;
    /* EVERY CLASS, NOT JUST #GP (Importance = 3):
     * Only class 6 was filled, and a class left ZERO is a class the kernel has
     * nowhere to send -- so it terminates the VDM instead. That is not a theory:
     * session 32 measured the WOW run stopping dead and then proved, by asking the
     * rig for a `tasklist` four seconds in, that **ntvdmhost.exe is already gone**.
     * The process is killed, silently, with no fault line and no teardown line --
     * which is also the whole explanation for the "watchdog logs one sample and
     * stops" that has been on the books since session 31: wd[0] lands at 250 ms and
     * there is no process left to log wd[1].
     *
     * A #GP was the only fault we could ever see. Anything else -- a not-present
     * selector load (#NP), a stack fault (#SS), an invalid TSS, a page fault --
     * killed us invisibly, and krnl386 has just started loading its own segments and
     * writing its own descriptors, which is exactly the code that produces those.
     *
     * [CAUTION]: THIS IS A DIAGNOSTIC WIDENING, NOT A CLAIM THAT WE CAN SERVICE THEM. The
     * handler logs the reflect and dumps the TIB window; it does not resume. Trading
     * a silent process kill for a logged stop is the whole point -- one of them can
     * be debugged. If a DOS guest regresses, this is the first thing to narrow, so
     * selftest.com is run against it deliberately.
     */
    /* ONE SITE PER CLASS, SO THE CLASS SURVIVES THE REFLECT (Importance = 1):
     * Filling every entry with the same {selector, offset} stopped the silent
     * kills, and threw away the only thing that says WHICH exception fired.
     * Give class i its own 4-byte BOP; the reflected EIP then names it.
     */
    { volatile BYTE *sites = (volatile BYTE *)(ULONG_PTR)((DWORD)DOS_CTAB_SEG << PARAGRAPH_SHIFT);
      for (index = 0; index < DOS_FLTSITE_N; ++index)
      {
          sites[DOS_FLTSITE_OFF + index * DOS_FLTSITE_SIZE + 0] = VDM_BOP0;
          sites[DOS_FLTSITE_OFF + index * DOS_FLTSITE_SIZE + 1] = VDM_BOP1;
          sites[DOS_FLTSITE_OFF + index * DOS_FLTSITE_SIZE + VDM_BOP_NUMBER_OFFSET] = DPMI_FAULT_BOP;
          sites[DOS_FLTSITE_OFF + index * DOS_FLTSITE_SIZE + VDM_BOP_LENGTH] = X86_OP_RETF;      /* RETF, never reached */
      }
      sites[DOS_FLTRET_OFF + 0] = VDM_BOP0;               /* the handler's retf lands here */
      sites[DOS_FLTRET_OFF + 1] = VDM_BOP1;
      sites[DOS_FLTRET_OFF + VDM_BOP_NUMBER_OFFSET] = DPMI_FLTRET_BOP;
      sites[DOS_FLTRET_OFF + VDM_BOP_LENGTH] = X86_OP_RETF; }
    for (index = 0; index * DPMI_FAULT_TABLE_ENTRY < sizeof g_FaultTable; ++index)
    {
        *(WORD  *)(g_FaultTable + index * DPMI_FAULT_TABLE_ENTRY) = g_DpmiFaultCodeSelector;
        *(DWORD *)(g_FaultTable + index * DPMI_FAULT_TABLE_ENTRY + DPMI_FAULT_TABLE_OFFSET) = (index < DOS_FLTSITE_N)
                                             ? (DWORD)DPMI_FAULT_SITE(index) : DPMI_FAULT_COFF;
    }
}

/* GH #18 (run 67): arm the VDM_TIB PM-fault reflect state before each PM entry. The kernel
 * takes the "first level, save" path only when the nest counter is 0 (then inc's it), so
 * this runs before EVERY DpmiEnterProtectedMode. Sets: nest=0, the 16/32 flag, the handler STACK
 * selector at [TIB+0x638], and the handler-table pointer at [VDM_TIB+8].
 */
VOID DpmiArmFaultTrampoline(volatile BYTE *tib, WORD flag)
{
    if (!g_DpmiFaultSelector)
        return;
    *(volatile WORD  *)(tib + VTIB_FLT_NEST)  = 0;
    *(volatile WORD  *)(tib + VTIB_FLT_FLAG)  = flag;
    *(volatile WORD  *)(tib + VTIB_FLT_HSEL)  = g_DpmiFaultSelector;         /* stack selector */
    *(volatile DWORD *)(tib + DPMI_TIB_FLTTBL) = (DWORD)(ULONG_PTR)g_FaultTable;
}

/* Linear base of a selector, for INT 21h pointer thunks. */
DWORD DpmiSelectorBase(WORD selector)
{
    INT index = DPMI_SELECTOR_INDEX(selector & WORD_MASK);

    /* Indices 1..3 are the switch's code/data/stack selectors (recorded in g_Ldt[]
     * from g_DpmiSegmentBase); 3+ are client allocations. For a .COM all three bases
     * equal g_DpmiCodeBase; for a real .EXE (CS!=DS!=SS) they differ, so a per-
     * selector lookup is required to translate DS:/ES: buffers correctly.
     */
    if (index >= 1 && index < DPMI_LDT_MAX)
        return g_Ldt[index].Base;
    return g_DpmiCodeBase;                                    /* null / unknown selector */
}

/* Descriptor introspection for the PM interpreter's LAR/LSL (run 55). Returns 1 if
 * `sel` names a populated descriptor (access byte != 0) and fills the access-rights
 * in LAR format (access byte at bits 8-15, the G/D/AVL flag nibble at 20-23) + the
 * byte-granular limit. The null selector (idx 0) and unallocated slots are invalid.
 */
/* RECOVER A FLAT 32-BIT CLIENT'S FAULTING ADDRESS FROM A 16-BIT FRAME. (s74):
 * NT's exception frame is 16-bit whatever the client is, so when the faulting CS has
 * base 0 -- the flat 32-bit case -- the EIP it reports *is* a linear address with its
 * top 16 bits gone. Those bits are recoverable WITHOUT GUESSING, because we know
 * exactly which memory is the client's: every block handed to it through INT 31h 0501
 * (g_DpmiBlock[]). Only one address per 64 KB window can carry the low bits we were
 * given, so the candidate set is tiny; require that the bytes there actually BE
 * `CD <vec>`, and that the answer is UNIQUE.
 * Unique-or-nothing is the whole point. One site that holds the very instruction the
 * CPU just told us it executed is EVIDENCE; taking the first of several would be the
 * same kind of guess as the eager scan's length vote, which has broken five guests.
 * Returns the linear address, or 0 if absent or ambiguous. *pcand gets the count.
 */
DWORD DpmiRecoverFlatEip(DWORD lo16, BYTE vector, INT *candidateCount)
{
    DWORD found = 0;
    INT matches = 0;
    INT index;

    lo16 &= WORD_MASK_U;
    for (index = 0; index < g_DpmiBlockCount; ++index)
    {
        DWORD base = g_DpmiBlock[index].Base;
        DWORD size = g_DpmiBlock[index].Size;
        DWORD address;
        if (!size)
            continue;
        for (address = (base & ~WORD_MASK_U) | lo16; address < base + size; address += X86_SEGMENT_SIZE_U)
        {
            const volatile BYTE *bytes;
            if (address < base || address + 1 >= base + size)
                continue;
            if (!HostReadable((const VOID *)(ULONG_PTR)address, 2))
                continue;
            bytes = (const volatile BYTE *)(ULONG_PTR)address;
            if (bytes[0] != X86_OP_INT || bytes[1] != vector)
                continue;
            if (matches == 0)
            {
                found = address;
                matches = 1;
            }
            else if (address != found)
                ++matches;
        }
    }
    if (candidateCount)
        *candidateCount = matches;
    return (matches == 1) ? found : 0;
}

INT DpmiSelectorDescriptor(WORD selector, UINT32 *accessRights, UINT32 *limit)
{
    INT index = DPMI_SELECTOR_INDEX(selector & WORD_MASK);

    if (index < 1 || index >= DPMI_LDT_LEGACY_LIMIT || g_Ldt[index].Access == 0)
        return 0;
    if (accessRights)
        *accessRights    = ((UINT32)g_Ldt[index].Access << BYTE_SHIFT) | ((UINT32)(g_Ldt[index].Flags & DPMI_DESCRIPTOR_FLAGS_MASK) << X86_DESCRIPTOR_FLAGS_SHIFT);
    if (limit)
        *limit = (UINT32)g_Ldt[index].Limit;
    return 1;
}

/* Resolve a PM BOP at CS:EIP back to the original interrupt vector.
 * - WHY THIS IS NOT JUST g_int_vec[eip]. The switch-time scan rewrote every `CD nn` in
 *   a 64K window at g_DpmiCodeBase, and g_int_vec[] is keyed by OFFSET INTO THAT
 *   WINDOW. A client may reach the very same physical bytes through a DIFFERENT
 *   selector, and DOS/4GW does exactly that: it builds a second code selector (0x57,
 *   base 0xd9b0) for its protected-mode interrupt handlers, which overlaps the patched
 *   window. A BOP then fires at 0x57:0x558c -- linear 0xF33C, i.e. window offset
 *   0x969C -- and an EIP-keyed lookup asks for g_int_vec[0x558c], finds nothing, and
 *   the run dies as "unexpected PM stop". Resolve through the LINEAR address so any
 *   alias of the patched memory maps back to the right vector.
 */
/* A PATCH MAP KEYED BY ADDRESS CANNOT SURVIVE THE GUEST COPYING THE CODE (Importance = 3):
 * We rewrite `CD nn` (two bytes) as `C4 C4` (two bytes) and keep the vector in a side
 * map keyed by LINEAR ADDRESS -- there is nowhere else to put it, the instruction is
 * the same length. That is sound until the guest MOVES the patched bytes, and krnl386
 * does exactly that: it copies each of its segments to a block of its own. The C4 C4
 * travels; the map entry stays behind at the address it was patched at.
 *
 * The symptom is an unrecognised BOP whose code byte is really the NEXT instruction's
 * first byte -- measured twice in one run, both of them krnl386 installing its INT 10h
 * handler:
 *
 *     WOWBOP 0xb8   file: cd 21   (AX=0x3510, get vector 10h)
 *     WOWBOP 0x1f   file: cd 21   (AX=0x2510, set vector 10h)
 *
 * -- i.e. two INT 21h calls silently swallowed, and the "BOP codes" 0xb8 and 0x1f were
 * just the opcode bytes that happened to follow each patched INT.
 *
 * Recover the vector from the MODULE'S OWN FILE IMAGE, which is the one copy of those
 * bytes nothing has rewritten. Self-verifying, and it fires only when all three hold:
 * the memory really is C4 C4, the address really is inside a segment we know the base
 * of, and the file really has CD there. Then record it, so it costs one lookup once.
 */
DWORD DpmiBopVector(DWORD csValue, DWORD eip)
{
    DWORD base = DpmiSelectorBase((WORD)csValue);
    DWORD linear  = base + eip;
    BYTE  vector    = PatchMapGet(linear);
    INT   segmentIndex;

    if (vector)
        return vector;
    if (!g_WowModuleCount || !g_WowImage[0])
        return 0;
    if (!HostReadable((const VOID *)(ULONG_PTR)linear, 2))
        return 0;
    {   const volatile BYTE *bytes = (const volatile BYTE *)(ULONG_PTR)linear;
        if (bytes[0] != VDM_BOP0 || bytes[1] != VDM_BOP1)
            return 0;
    }
    for (segmentIndex = 0; segmentIndex < (INT)g_WowModule[0].SegmentCount && segmentIndex < WOW_PMBASE_MAX; ++segmentIndex)
    {
        NE_SEGMENT *segment = &g_WowModule[0].Segments[segmentIndex];
        if (!segment->Sector || eip + 1 >= segment->Length)
            continue;
        if (base != g_WowPmBase[segmentIndex] && base != ((DWORD)segment->Selector << PARAGRAPH_SHIFT))
            continue;
        if (segment->FileOffset + eip + 1 >= g_WowModule[0].ImageLength)
            continue;
        {   const BYTE *fileBytes = g_WowImage[0] + segment->FileOffset + eip;
            if (fileBytes[0] != X86_OP_INT)
                return 0;
            PatchMapSet(linear, fileBytes[1]);                     /* one lookup, once */
            return fileBytes[1];
        }
    }
    return 0;
}

#define DPMI_PATCH_REGION_MAX_U     0x00400000u     /* Larger = a flat selector, not a region */

/* AN EIP IS ONLY 16 BITS WIDE WHEN ITS CODE SELECTOR IS:
 * Every PM stop used to read `VDM_REG(tib, VTIB_EIP) & 0xFFFF`, which is right for the
 * 16-bit selectors this host grew up on and WRONG the moment a client runs 32-bit code
 * in a flat selector -- there, EIP is a full linear address and the top sixteen bits
 * are the address, not junk to be discarded.
 * It cost a whole diagnosis to see. Doom's first `int 21h` (AH=30h, get DOS version,
 * at obj1+0x40be5) fired our BOP exactly as intended at linear 0x03b10be5, and the
 * masked EIP turned the lookup into `PatchMapGet(0x0be5)` -- a different address, in low
 * memory, with nothing recorded there. The run then died as "unexpected PM stop" AT THE
 * VERY INSTRUCTION THAT PROVED THE PATCH WORKED, and the log said 0x187:0x0be7, which
 * reads like a wild jump into the BIOS data area rather than what it was.
 * This is the same rule the interrupt-frame width already follows: ask the descriptor,
 * not the host's habits. DpmiSelectorIs32() reads the D/B bit we store for the selector.
 */
DWORD DpmiPmEip(volatile BYTE *tib)
{
    DWORD currentEip = VDM_REG(tib, VTIB_EIP);

    return DpmiSelectorIs32((WORD)VDM_REG16(tib, VTIB_CS)) ? currentEip : (currentEip & WORD_MASK);
}

#define DPMI_PATCH_FLOOR    0x600   /* Below: the IVT, the BDA and DOS's own area */

/* PATCH A REGION THE CLIENT HAS JUST DECLARED TO BE CODE:
 * Called from INT 31h 0009/000C when the resulting descriptor is a CODE type. The
 * TIMING is the client's, not ours, and it is right: Doom's trace shows AH=48h
 * allocate -> AH=3Fh read the module in -> 0009 retype to code -> far jump. So at the
 * moment the client says "code", the bytes are already in memory and have not yet been
 * executed -- the only window in which patching is both possible and safe.
 * Scanning a data region would be the dangerous thing (a `CD 21` byte pair that is
 * really data gets corrupted); scanning only what the client itself calls code is the
 * narrowest rule that covers the case, and g_int_vec[] remains the revert map.
 */
/* `d32` is the region's DEFAULT OPERAND SIZE -- the D/B bit of the descriptor that named
 * it code -- and it is not optional: instruction lengths differ between the two, so
 * decoding DOS/4GW's 16-bit modules as 32-bit rejects obvious real sites (`mov ax,4c00h
 * / int 21h` scored zero votes, measured). Where the width is genuinely unknown the scan
 * is idempotent and self-healing: a site rejected under the wrong width is not recorded
 * in the patch map, so the next pass -- and there is always a next pass, because the
 * client declares its regions repeatedly while it loads -- gets another chance at it.
 */
VOID DpmiPatchCodeRegion(DWORD base, DWORD limit, INT is32BitRegion)
{
    volatile BYTE *memory;
    /* jumpTableSkipped: jump-table entries skipped -- see the guard */
    DWORD end;
    DWORD patched = 0;
    DWORD rejected = 0;
    DWORD jumpTableSkipped = 0;
    /* the first few patched offsets, for the log */
    DWORD patchedOffsets[12];
    DWORD patchedOffsetCount = 0;
    DWORD sitesDumped = 0;                      /* how many sites we have dumped BYTES for */
    CHAR lineBuffer[320];
    CHAR *cursor = lineBuffer;

    /* - NEVER PATCH THE IVT / BIOS DATA AREA, even when the client declares a base-0
     * code selector -- and Doom does exactly that (sel 0x67, base 0, limit 0xFFFF),
     * which made the first version of this scan rewrite 16 sites in linear 0..0xFFFF.
     * An interrupt vector is a word pair, so `CD 21` is a perfectly ordinary VALUE
     * down there: vector n = 0x21CD would be silently turned into 0xC4C4 and the
     * guest would jump into hyperspace on the next INT n. Below 0x600 is IVT + BDA +
     * our own handler segment prologue: definitionally data, never executed as the
     * client's code. Start the scan above it.
     */
    end = base + limit;                              /* limit is the LAST valid byte */
    if (g_NoPmPatch && (!g_NoPmPatchMinimum || (end - base) >= g_NoPmPatchMinimum))
    {
        cursor = LogPut(cursor, "DPMI: code region 0x"); cursor = LogHex(cursor, base);      /* see NOPMPATCH_FLAG */
        cursor = LogPut(cursor, "..0x"); cursor = LogHex(cursor, end);
        cursor = LogPut(cursor, " size=0x"); cursor = LogHex(cursor, end - base);
        cursor = LogPut(cursor, " -- NOT SCANNED (nopmpatch.flag");
        if (g_NoPmPatchMinimum)
        {
            cursor = LogPut(cursor, " min=0x");
            cursor = LogHex(cursor, g_NoPmPatchMinimum);
        }
        cursor = LogPut(cursor, ")\r\n");
        LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
        return;
    }
    /* a near-pointer table INTO this region is DATA -- see the loop */
    DWORD pointerLow = base;
    DWORD pointerHigh = end;
    /* A 32-BIT CODE REGION IS SCANNED BUT NEVER WRITTEN. (s74) (Importance = 5):
     * The sixth guest this scan corrupted was heaven7, and the failure is not one the
     * vote can be tuned out of: its LE code object is a PACKED payload, and x86's
     * self-synchronisation converges on random bytes exactly as well as on code --
     * the six false sites scored 44..48 votes out of 48, indistinguishable from the
     * real `int 31h` beside them (measured offline against h7.EXE with this very
     * header; all seven are in the FILE at object+0x2d8, not runtime-generated as
     * first written up). Byte entropy separates them only at 1 KB windows by 0.3
     * bits over 281 real sites -- a heuristic on a heuristic, not a rule.
     * What the scan BUYS a 32-bit client is one #GP per site: since session 39 the
     * `#GP(IDT) is a RAW INT` arm services a raw INT and patches it on the way past,
     * on the CPU's evidence rather than a guess, and since s74 it does so for a flat
     * base-0 CS as well (heaven7: 72 serviced, none declined, with the scan off).
     * What the scan COSTS is Doom's six sessions (a jump table written after the
     * first pass) and heaven7 outright. Wrong trade. So for `is32BitRegion` regions the scan
     * still runs and still logs what it would have done -- the count and the sites
     * remain the instrument they were -- but writes nothing; the first execution of
     * each site takes the lazy path once and is fast thereafter.
     *
     * [CAUTION]: 16-BIT REGIONS ARE UNCHANGED. DOS/4GW's own modules, DOS16M (ZAR) and krnl386
     * are all confirmed on the eager path, and the lazy arm's 16-bit resume has a
     * longer record than its 32-bit one. One change per by-hand test.
     */
    const INT isDryRun = is32BitRegion;
    if (base < DPMI_PATCH_FLOOR)
        base = DPMI_PATCH_FLOOR;                                           /* ...but the region END is unchanged */
    /* No upper bound any more: the regions that matter live in EXTENDED memory, which is
     * where a working extender puts its modules. The size cap is a sanity bound rather
     * than a policy -- a multi-megabyte "code" region is a flat alias, not a module.
     */
    if (end <= base)
        return;
    /* - A FLAT CODE SELECTOR CANNOT BE SCANNED, BUT THE CLIENT'S MEMORY CAN. Doom's own
     * 32-bit code selector is `setaccess 0xC7FA` -- present, DPL3, code, G=1, D/B=1,
     * base 0, limit 4 GB. Scanning that range is impossible and would mis-patch every
     * byte of the address space, so we used to refuse it outright and the application's
     * raw `CD 21` / `CD 31` were left unpatched -- which is the one fault XP will not
     * reflect. The usable answer is that WE KNOW WHICH MEMORY IS THE CLIENT'S: it is
     * what we handed out through 0501. So for an oversized region, scan those blocks
     * (clipped to the region) instead of the range.
     * Timing is on our side: the client declares its code selector once its image is
     * loaded and long before it reads data files, so the blocks in play at that moment
     * are code. The count is logged -- thousands of "INT sites" would mean we are
     * patching data, and that is the number to look at if something later reads wrong.
     */
    if ((end - base) > DPMI_PATCH_REGION_MAX_U)
    {
        /* - A FLAT CODE SELECTOR, AND WE DO NOT HAVE AN ANSWER FOR IT YET. Doom's own
         * 32-bit code selector is `setaccess 0xC7FA` -- present, DPL3, code, G=1,
         * D/B=1, base 0, limit 4 GB. Scanning that range is impossible, so the
         * application's raw `CD 21` / `CD 31` go unpatched, and a raw INT in protected
         * mode is the one fault XP will not reflect.
         * - SCANNING THE CLIENT'S OWN 0501 BLOCKS INSTEAD WAS TRIED AND IS WORSE
         *   (measured, session 17): it patched three extra byte pairs inside the 1 MB
         *   block that already held the extender's modules -- i.e. DATA -- and the run
         *   ended EARLIER than without it. The client's memory is code and data mixed;
         *   "everything we handed out" is not a code region.
         * - What is needed is a way to know which parts of the client's memory are
         *   code. The client knows: it loads objects with a flag. We do not see that
         *   flag, but we DO see every window descriptor it builds over each object
         *   (0006 + 000C) while relocating -- that is the shape of the answer.
         * g_DpmiBlock[] is recorded and ready for whoever takes this on.
         */
        cursor = LogPut(cursor, "DPMI: FLAT code region 0x"); cursor = LogHex(cursor, base);
        cursor = LogPut(cursor, "..0x"); cursor = LogHex(cursor, end);
        cursor = LogPut(cursor, " -- not scanned as a range (");
        cursor = LogHex(cursor, (DWORD)g_DpmiBlockCount); cursor = LogPut(cursor, " blocks known, ");
        cursor = LogHex(cursor, (DWORD)g_LeCodeCount); cursor = LogPut(cursor, " LE code sizes)\r\n");
        LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
        return;
    }
    /* - WALK THE COMMITTED REGIONS, DO NOT WALK THE ADDRESS RANGE. A declared code
     * region is the CLIENT's idea of what is code, and a client hands us ranges that
     * span memory nobody has mapped -- Doom's own runtime declares a base-0 1 MB
     * selector, and the low megabyte has holes. Scanning straight through faulted
     * inside this very loop (caught by the VEH: `cmp al,0xcd` with EDX=0xd4000), i.e.
     * our own scanner killing the run it exists to help. Probing every N bytes is not
     * good enough either -- it guesses at a boundary the memory manager already knows.
     * Ask it: VirtualQuery hands back exactly the committed, readable extents.
     */
    { DWORD address = base;
      while (address < end)
      {
          MEMORY_BASIC_INFORMATION memoryInfo;
          DWORD rend;
          DWORD index;
          if (VirtualQuery((LPCVOID)(ULONG_PTR)address, &memoryInfo, sizeof memoryInfo) != sizeof memoryInfo)
              break;
          rend = (DWORD)((ULONG_PTR)memoryInfo.BaseAddress + memoryInfo.RegionSize);
          if (rend <= address)
              break;                                       /* no progress -> stop, never spin */
          if (rend > end)
              rend = end;
          if (memoryInfo.State == MEM_COMMIT && !(memoryInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
          {
              memory = (volatile BYTE *)(ULONG_PTR)address;
              for (index = 0; index + 1 < (rend - address); ++index)
              {
                  /* [CAUTION]: ADDING 0x32/0x34/0x35/0x36 HERE WAS TRIED AND IS NOT THE ANSWER.
                   * Doom's Watcom thunk table at obj1+0x41e41 is `[cd nn][c3]` triples and
                   * the log dump shows only INT 33h patched:
                   *   c3  cd 32 c3  c4 c4 c3  cd 34 c3  cd 35 c3  cd 36 c3
                   * A raw `CD nn` in PM is the fault XP will not reflect, so those looked
                   * like the silent teardown. Measured (session 19): patching them changes
                   * NOTHING -- same tick count, same INT 31h total, and vectors 32/34/35/36
                   * are NEVER SERVICED, i.e. the guest never calls those thunks. Reverted
                   * rather than left in as unverified scan surface. The guest enters the
                   * table only at the `c3` tail of the already-patched INT 31h thunk.
                   */
                  /* EVERY VECTOR, NOT A LIST OF THE ONES WE EXPECTED (Importance = 3):
                   * This tested `mem[i+1]` against seven vectors -- 31h, 21h, 10h, 16h,
                   * 33h, 1Ah, 08h -- which are the ones a DOS extender running a game
                   * uses. It is an ALLOW-LIST, and an allow-list of "interrupts we think
                   * the guest will use" is exactly the assumption a new guest breaks.
                   *
                   * [CAUTION]: MEASURED, session 32. krnl386 executes an `INT 2` (`cd 02`) in
                   * protected mode during bring-up. A breakpoint armed on that site reported
                   * `displaced cd 02` -- proof it
                   * was RAW, because DpmiBreakpointArm() refuses a site that is already an INT
                   * site (PatchMapGet), so it could not have armed at all otherwise. And a raw
                   * `CD nn` in protected mode is the one fault XP will not reflect: it tears
                   * the VDM down silently, which is precisely the death being chased.
                   *
                   * [CAUTION]: AND THE FIRST DIAGNOSIS OF THAT WAS WRONG, WHICH IS WHY THIS NOTE IS
                   * HERE. It was written up as "the boundary vote produces false negatives
                   * on real instructions". The vote never ran: 0x02 is not in the list, so
                   * the site was never a candidate. The vote is fine; the FILTER IN FRONT
                   * OF IT was the defect.
                   *
                   * So test for `CD` alone and let X86IsIntSiteReal() decide, which is
                   * what it exists for and what makes this safe -- it decodes forward from
                   * each of the preceding 48 bytes and counts how many instruction streams
                   * land here (measured separation: real sites 19-48 votes, false pairs
                   * 0-3). The old allow-list was a second, cruder filter that predates the
                   * vote (x86len.h arrived in session 21) and was never revisited after it.
                   * Servicing is harmless for any vector: our default PM handlers cover all
                   * 256 and simply IRET.
                   *
                   * - The note below about 0x32/0x34/0x35/0x36 stands as history but no longer
                   *   as policy: those were *reverted for lack of evidence*, and the evidence
                   *   now exists. Watch the patched-site count and the "NOT an INT site"
                   *   rejects across this change -- that is how a false positive would show.
                   */
                  if (memory[index] == X86_OP_INT)
                  {
                      DWORD linear = address + index;
                      if (PatchMapGet(linear))
                          continue;                        /* already patched (aliased region) */
                      /* -- A JUMP/CALL TABLE IS DATA, EVEN INSIDE A CODE OBJECT.
                       * (session 72) The FOURTH instance of the patcher rewriting a
                       * non-code byte pair -- the ZAR call displacement, the
                       * R_InitTextureMapping `jle` displacement and the FP range are the
                       * other three, all above/below -- and the first found by a debugger
                       * reading the dead guest's OWN CORE (bm\vdmwatch.exe).
                       * Doom's platform-type dispatch is a table of near pointers at
                       * obj1+0x2cc6c. Three of its entries point into obj1+0x2cdXX, whose
                       * little-endian bytes are `XX cd 16 04`, so the middle pair reads as
                       * `CD 16` = INT 16h -- a serviced vector -- and X86IsIntSiteReal()
                       * passes, because a table of code pointers decodes into plausible
                       * instruction streams. The table is written by the guest AFTER the
                       * first scan, so the SECOND scan (DOS/4GW re-declares its code selector
                       * on every file load, which re-runs this patcher over the same range)
                       * is the one that corrupts it: `0x0416cdfa` -> `0x04c4c4fa`. Killing
                       * that platform type later makes the guest `jmp` through the mangled
                       * entry into unmapped memory -- the ACCESS_VIOLATION at 0x04c4c4fa that
                       * XP would not reflect and that tore the VDM down with no VEH, no
                       * watchdog line and nothing after the log's last byte, for six sessions.
                       *
                       * An aligned dword that points back INTO this very code region is a
                       * pointer, not two instructions. All five of Doom's table entries point
                       * here; skip any candidate that lands inside one. Safe like every arm
                       * below: a real INT so aligned is still serviced out of the #GP. The
                       * read is 4-aligned (VirtualQuery bases are page-aligned), so it cannot
                       * itself fault the scanner.
                       */
                      { DWORD doff = (linear & ~X86_DWORD_ALIGN_MASK_U) - address;
                        if (doff + X86_DWORD_SIZE <= (rend - address))
                        {
                            DWORD word = *(const volatile DWORD *)(const volatile VOID *)(memory + doff);
                            if (word >= pointerLow && word < pointerHigh)
                            {
                                /* [CAUTION]: ITS OWN BUDGET, NOT THE SHARED ONE. Counted separately
                                 * AND always reported in the scan line below, because the
                                 * first version shared `rejected`'s 16-line cap -- and the
                                 * corrupting scan rejects 334 byte pairs before it ever
                                 * reaches the table, so the one line that proves this guard
                                 * works was suppressed EXACTLY when it mattered. The live
                                 * confirming run logged it zero times and the guard had in
                                 * fact fired three times; only `patched 3 -> 0` and
                                 * `rejected 0x14e -> 0x151` gave it away. An absence in the
                                 * report means nothing unless the report says what it left
                                 * out -- this project's most repeated lesson.
                                 */
                                ++jumpTableSkipped;
                                if (jumpTableSkipped <= 8)
                                {
                                    CHAR pointerLine[192];
                                    CHAR *pointerCursor = pointerLine;
                                    pointerCursor = LogPut(pointerCursor, "DPMI: NOT patching 0x"); pointerCursor = LogHex(pointerCursor, linear);
                                    pointerCursor = LogPut(pointerCursor, " vec=0x"); pointerCursor = LogHexByte(pointerCursor, memory[index+1]);
                                    pointerCursor = LogPut(pointerCursor, " -- inside aligned pointer 0x"); pointerCursor = LogHex(pointerCursor, word);
                                    pointerCursor = LogPut(pointerCursor, " into this code region: a jump-table entry, DATA\r\n");
                                    LogAppend(LOG_PATH, pointerLine, pointerCursor); SerialOut(pointerLine, pointerCursor);
                                }
                                continue;
                            }
                        } }
                      /* -- INT 34h..3Fh IS NOT AN INTERRUPT RANGE, IT IS
                       *   FLOATING-POINT CODE. DO NOT TOUCH IT. (session 55) ------
                       * The note above says patching any vector is safe because
                       * "our default PM handlers cover all 256 and simply IRET".
                       * That is true of interrupts. These are not interrupts.
                       * Microsoft's floating-point emulator encodes x87
                       * instructions AS `CD nn` in this range -- 0x34..0x3B are
                       * ESC opcodes D8..DF, 0x3C is a segment override + ESC and
                       * 0x3D is FWAIT -- and the emulator library REWRITES those
                       * sites into real x87 instructions at load time when a
                       * coprocessor is present. 0x3E/0x3F are the far-call and
                       * overlay fixups and are equally not ours.
                       * So a `CD 39` in guest code is an FP operation, and
                       * rewriting it to a BOP destroys the operation; "handled by
                       * an IRET" then means the arithmetic silently did not
                       * happen.
                       *
                       * [INFO]: MEASURED, and it is what killed CALC.EXE -- a CALCULATOR,
                       * which imports WIN87EM. It built its whole dialog, ran its
                       * entire WM_INITDIALOG, and then died at
                       * `bytes@eip-2 = 3d cb c4 c4 07 cd 3d cb cd 39` -- FWAIT,
                       * RETF, our BOP, then more FP -- with the run's own new
                       * diagnostic reading `pmap[eip]=INT 0x39 THIS IS A SITE WE
                       * PATCHED`. TASKMAN and CARDFILE end the same way.
                       *
                       * [CAUTION]: NOT PATCHING THEM IS SAFE HERE and is the whole point: a
                       * raw INT in protected mode is serviced out of the #GP
                       * (session 34), so a guest that really does execute one
                       * still reaches whatever PM handler it installed through
                       * DPMI 0205h -- which for this range is its own emulator's.
                       * Patching was the thing taking that away.
                       */
                      if (memory[index+1] >= VECTOR_FLOATING_POINT_FIRST && memory[index+1] <= VECTOR_FLOATING_POINT_LAST)
                      {
                          if (rejected < 16)
                          {
                              CHAR fixupLine[160];
                              CHAR *fixupCursor = fixupLine;
                              fixupCursor = LogPut(fixupCursor, "DPMI: NOT patching 0x"); fixupCursor = LogHex(fixupCursor, linear);
                              fixupCursor = LogPut(fixupCursor, " vec=0x"); fixupCursor = LogHexByte(fixupCursor, memory[index+1]);
                              fixupCursor = LogPut(fixupCursor, " -- FP EMULATOR / fixup range 34..3F,"
                                            " this is an x87 instruction not an"
                                            " interrupt\r\n");
                              LogAppend(LOG_PATH, fixupLine, fixupCursor); SerialOut(fixupLine, fixupCursor);
                          }
                          continue;
                      }
                      /* AND ONLY A VECTOR WE CAN ACTUALLY SERVICE (Importance = 5):
                       * THE THIRD INSTANCE OF THIS DEFECT, and the first two are already
                       * written up above and in x86len.h: a naive `CD nn` pair is not an
                       * INT instruction, and rewriting one that is really an operand
                       * destroys the instruction it belongs to.
                       *
                       * [INFO]: MEASURED, ZAR (GH #23), and it is the whole reason that guest
                       * did not run. The pair `cd e4` in memory was the DISPLACEMENT of
                       * a near call (`e8 cd e4`). The vote passed it -- correctly, on its
                       * own terms: everything in front of it is DATA (a run of zero
                       * bytes), and an odd-aligned stream through zeros lands exactly
                       * here. The vote can only ever be a heuristic about where
                       * instructions START; it cannot know this region is not code.
                       * Patched, the call's target moved and landed MID-INSTRUCTION, so
                       * the start of the real routine was skipped and the guest went on
                       * down a path that ends in the `#GP` at 01A7:1B7D that DOS/4GW
                       * reports on the desktop.
                       *
                       * THE 64K SCANNER ALREADY HAS THE ANSWER and this scanner never
                       * got it: "evidence only -- add a vector only with a guest that
                       * provably needs it, and only with a service arm to receive it".
                       * The set below is exactly the vectors DpmiServicePmInt has an
                       * arm for. 0xE4 is not one of them; nothing could have been gained
                       * by patching it and a working guest was lost.
                       *
                       * [CAUTION]: NOT PATCHING IS SAFE -- the same argument the FP-range arm just
                       * above relies on: a raw INT in protected mode is serviced out of
                       * the #GP (session 34), so a guest that really does execute one
                       * still reaches its handler. Patching is an OPTIMISATION, and it
                       * has now cost three guests.
                       *
                       * [CAUTION]: 0x15 IS IN THE LIST, so this segment's other two sites (+0x1b3e
                       * and +0x1b4a, both real `cd 15`) are patched exactly as before.
                       */
                      { BYTE vector = memory[index+1];
                        INT serviced = (vector == VECTOR_DPMI || vector == VECTOR_DOS || vector == VECTOR_VIDEO
                                        || vector == VECTOR_KEYBOARD_SERVICES || vector == VECTOR_MOUSE || vector == VECTOR_MULTIPLEX
                                        || vector == VECTOR_EQUIPMENT || vector == VECTOR_KERNEL_DEBUGGER || vector == VECTOR_TIME
                                        || vector == VECTOR_TIMER || vector == VECTOR_SYSTEM);
                        if (!serviced)
                        {
                            if (rejected++ < 16)
                            {
                                CHAR unservicedLine[176];
                                CHAR *unservicedCursor = unservicedLine;
                                unservicedCursor = LogPut(unservicedCursor, "DPMI: NOT patching 0x"); unservicedCursor = LogHex(unservicedCursor, linear);
                                unservicedCursor = LogPut(unservicedCursor, " vec=0x"); unservicedCursor = LogHexByte(unservicedCursor, vector);
                                unservicedCursor = LogPut(unservicedCursor, " -- no PM service arm for it, so a BOP here"
                                              " could only ever lose an instruction\r\n");
                                LogAppend(LOG_PATH, unservicedLine, unservicedCursor); SerialOut(unservicedLine, unservicedCursor);
                            }
                            continue;
                        } }
                      /* Record WHERE, not just how many -- see the push further down,
                       * which happens AFTER the vote so the list and the count describe
                       * the same set. "patched 2 INT sites" in a 55 KB code segment is
                       * either a correct count or a broken scan, and the count alone
                       * cannot tell you which; the offsets can, because they are
                       * checkable against a disassembly of the same bytes.
                       */
                      /* ONLY IF IT IS AN INSTRUCTION:
                       * This scan used to take the byte pair as proof, and in Doom's
                       * code object that is wrong in three places -- one of them
                       * fatally. At obj1+0x3593f the pair `cd 31` straddles two
                       * instructions -- a short jump's DISPLACEMENT followed by the next
                       * instruction's opcode. Patching it bent that jump into the middle
                       * of another instruction, the guest read an unmapped address, and
                       * XP tore the VDM down with no VEH, no watchdog line and no last
                       * log entry. Sessions 16-20 hunted that
                       * as a fault in Doom. It was this line.
                       * X86IsInstructionStart() decodes forward from each of the preceding
                       * 48 bytes and asks how many streams land here; see x86len.h for
                       * the measured separation (real sites 19-48 votes, false pairs
                       * 0-3) and why the threshold leans toward keeping.
                       */
                      if (!X86IsIntSiteReal((const BYTE *)(ULONG_PTR)address, index,
                                                rend - address, is32BitRegion))
                      {
                          if (rejected++ < 16)
                          {
                              CHAR midInstructionLine[128];
                              CHAR *midInstructionCursor = midInstructionLine;
                              midInstructionCursor = LogPut(midInstructionCursor, "DPMI: NOT an INT site (mid-instruction) 0x");
                              midInstructionCursor = LogHex(midInstructionCursor, linear);
                              midInstructionCursor = LogPut(midInstructionCursor, " vec=0x"); midInstructionCursor = LogHexByte(midInstructionCursor, memory[index+1]);
                              midInstructionCursor = LogPut(midInstructionCursor, is32BitRegion ? " d32=1" : " d32=0");
                              midInstructionCursor = LogPut(midInstructionCursor, " ctx="); midInstructionCursor = LogDump(midInstructionCursor, (const BYTE *)(ULONG_PTR)(linear - 4), 10);
                              midInstructionCursor = LogPut(midInstructionCursor, "\r\n");
                              LogAppend(LOG_PATH, midInstructionLine, midInstructionCursor); SerialOut(midInstructionLine, midInstructionCursor);
                          }
                          continue;
                      }
                      /* [CAUTION]: RECORDED HERE, AFTER THE VOTE, NOT AT CANDIDATE TIME.
                       * This push used to sit next to the `mem[i] == X86_OP_INT` test, so
                       * the line below read "patched 4 INT sites, rejected 16 ... at
                       * +0x13a3 +0x2052 ..." with TWELVE offsets -- candidates
                       * rendered as if they were the patched ones. Session 39 spent
                       * a step ruling out the patcher on exactly that line while the
                       * offset it needed was sitting in it. The count and the list
                       * have to be claims about the same set.
                       */
                      if (patchedOffsetCount < 12)
                          patchedOffsets[patchedOffsetCount++] = linear - base;
                      /* -- SAY WHAT WE ARE ABOUT TO OVERWRITE, BEFORE WE DO. (s74)
                       * "ON ANY SILENT VDM DEATH, GET THE BYTES FIRST" -- and for five
                       * sessions the one thing this patcher never recorded was the bytes
                       * IT clobbered, so every investigation had to reconstruct them from
                       * the guest binary (and for a guest that builds code or tables at
                       * RUNTIME, as heaven7 does, they are not in the binary at all: the
                       * file has `0b c3` where we patched, because the memory was
                       * generated, not loaded). A site list without bytes is a claim you
                       * cannot check. Bounded to the first 12, like po[].
                       */
                      if (sitesDumped < 24)                /* its OWN counter: patchedOffsetCount saturates at 12 */
                      {
                          CHAR siteLine[192];
                          CHAR *siteCursor = siteLine;
                          ++sitesDumped;
                          siteCursor = LogPut(siteCursor, isDryRun ? "DPMI: WOULD PATCH 0x" : "DPMI: PATCHING 0x"); siteCursor = LogHex(siteCursor, linear);
                          siteCursor = LogPut(siteCursor, " (+0x"); siteCursor = LogHex(siteCursor, linear - base);
                          siteCursor = LogPut(siteCursor, ") vec=0x"); siteCursor = LogHexByte(siteCursor, memory[index+1]);
                          siteCursor = LogPut(siteCursor, isDryRun ? " (32-bit region: left for the #GP(IDT) arm)  bytes="
                                            : " -> C4 C4  was=");
                          siteCursor = LogDump(siteCursor, (const BYTE *)(ULONG_PTR)(linear - 6), 16);
                          siteCursor = LogPut(siteCursor, "\r\n");
                          LogAppend(LOG_PATH, siteLine, siteCursor); SerialOut(siteLine, siteCursor);
                      }
                      if (!isDryRun)
                      {
                          PatchMapSet(linear, memory[index+1]);
                          memory[index] = VDM_BOP0;
                          memory[index+1] = VDM_BOP1;
                      }
                      ++patched;
                  }
              }
          }
          address = rend;
      } }
    cursor = LogPut(cursor, "DPMI: code region 0x"); cursor = LogHex(cursor, base);
    cursor = LogPut(cursor, "..0x"); cursor = LogHex(cursor, end);
    cursor = LogPut(cursor, isDryRun ? " -> 32-bit, NOT WRITTEN: would patch " : " -> patched ");
    cursor = LogHex(cursor, patched); cursor = LogPut(cursor, " INT sites, rejected ");
    cursor = LogHex(cursor, rejected); cursor = LogPut(cursor, " mid-instruction byte pairs, ");
    cursor = LogHex(cursor, jumpTableSkipped); cursor = LogPut(cursor, " jump-table entries");
    if (patchedOffsetCount)
    {
        DWORD index2;
        cursor = LogPut(cursor, " at +0x");
        for (index2 = 0; index2 < patchedOffsetCount; ++index2)
        {
            if (index2)
                cursor = LogPut(cursor, " +0x");
            cursor = LogHex(cursor, patchedOffsets[index2]);
        }
    }
    cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
    DpmiBreakpointArm();          /* a module that has just appeared may hold a requested BP */
}

enum
{
    LE_SCAN_FILE_MIN = 0x200, LE_SCAN_HEADER_ROOM = 0x100
};   /* DpmiLeLearn: shortest file searched; header room left at the end */
/* Learn the program's EXECUTABLE object sizes from its own LE header. See the commentary
 * on g_LeCodeSize. The image is already in `filebuf` -- the loader read it to run the
 * MZ stub -- so this costs one pass over memory we are holding anyway and no file I/O.
 * - THE HEADER IS FOUND BY SEARCH, NOT BY e_lfanew. In a bound executable the MZ stub is
 *   the extender (DOS/4GW), and its e_lfanew is not a pointer to the LE at all -- on
 *   DOOM.EXE it reads 0x09b40000, i.e. off the end of a 0xad511-byte file. Every offset
 *   INSIDE the LE header is relative to the header, not the file, for the same reason.
 * - AND THE CANDIDATE IS VALIDATED, because "LE\0\0" is two ASCII letters and two zeroes
 *   and occurs in data by chance. Byte/word order little-endian, format level 0, a 386+
 *   CPU, a plausible OS and object count: five agreeing fields, which no accident of
 *   data passed on any binary tried here.
 */
VOID DpmiLeLearn(const BYTE *buffer, DWORD length)
{
    DWORD index;
    CHAR lineBuffer[192];
    CHAR *cursor;

    if (!buffer || length < LE_SCAN_FILE_MIN)
        return;
#define LE32(o)     ((DWORD)buffer[(o)] | ((DWORD)buffer[(o)+1] << BYTE_SHIFT) | ((DWORD)buffer[(o)+2] << WORD_SHIFT) | ((DWORD)buffer[(o)+3] << TOP_BYTE_SHIFT))
#define LE16(o)     ((DWORD)buffer[(o)] | ((DWORD)buffer[(o)+1] << BYTE_SHIFT))
    for (index = 0; index + LE_SCAN_HEADER_ROOM < length; ++index)
    {
        DWORD objectTableOffset;
        DWORD objectCount;
        DWORD object;
        if (buffer[index] != 'L' || buffer[index+1] != 'E' || buffer[index+2] || buffer[index+3])
            continue;
        if (LE32(index + LE_FORMAT_LEVEL) != 0)
            continue;                                                         /* format level */
        { DWORD cpu = LE16(index + LE_CPU_TYPE), targetOs = LE16(index + LE_TARGET_OS);
          if (cpu < LE_FIELD_TYPE_FIRST || cpu > LE_FIELD_TYPE_LAST || targetOs < LE_FIELD_TYPE_FIRST || targetOs > LE_FIELD_TYPE_LAST)
              continue; }
        objectCount   = LE32(index + LE_OBJECT_COUNT);
        objectTableOffset = LE32(index + LE_OBJECT_TABLE);
        if (objectCount < 1 || objectCount > LE_OBJECTS_MAX)
            continue;
        if (objectTableOffset < LE_HEADER_MIN || index + objectTableOffset + objectCount * LE_OBJECT_ENTRY_SIZE > length)
            continue;
        cursor = lineBuffer; cursor = LogPut(cursor, "DPMI: LE image at file 0x"); cursor = LogHex(cursor, index);
        cursor = LogPut(cursor, ", "); cursor = LogHex(cursor, objectCount); cursor = LogPut(cursor, " objects\r\n");
        LogAppend(LOG_PATH, lineBuffer, cursor);
        for (object = 0; object < objectCount; ++object)
        {
            DWORD entry     = index + objectTableOffset + object * LE_OBJECT_ENTRY_SIZE;
            DWORD virtualSize = LE32(entry + LE_OBJECT_VIRTUAL_SIZE);
            DWORD flags = LE32(entry + LE_OBJECT_FLAGS);
            DWORD pageRounded    = (virtualSize + PAGE_LAST_BYTE_U) & ~PAGE_LAST_BYTE_U;          /* what 0501 will ask for */
            INT   isExecutable  = (flags & LE_OBJECT_EXECUTABLE) != 0;
            cursor = lineBuffer; cursor = LogPut(cursor, "DPMI:   obj"); cursor = LogHex(cursor, object + 1);
            cursor = LogPut(cursor, " vsize 0x"); cursor = LogHex(cursor, virtualSize);
            cursor = LogPut(cursor, " flags 0x"); cursor = LogHex(cursor, flags);
            cursor = LogPut(cursor, isExecutable ? " EXEC" : " data");
            if (isExecutable && pageRounded >= DPMI_LE_MIN_CODE_SIZE && g_LeCodeCount < DPMI_LE_MAX)
            {
                g_LeCodeSize[g_LeCodeCount++] = pageRounded;
                cursor = LogPut(cursor, " -- code block size 0x"); cursor = LogHex(cursor, pageRounded);
            }
            else if (isExecutable)
            {
                cursor = LogPut(cursor, " -- too small to key on (a based descriptor will reach it)");
            }
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, lineBuffer, cursor);
        }
        return;                                                /* first valid LE wins */
    }
#undef LE32
#undef LE16
}

/* Patch the INT sites in every block that holds an EXEC object. Called whenever the
 * picture may have changed -- a new block, or the client naming a region code.
 * - WHY IT IS CALLED REPEATEDLY RATHER THAN ONCE. The block is allocated EMPTY and
 *   filled afterwards, and we never see the fill: DOS/4GW reads through a low-memory
 *   transfer buffer and copies up in its own code. On DOOM.EXE the code object's block
 *   is allocated ~1100 log lines before its last page arrives. There is no single event
 *   that means "loaded", so this is idempotent and cheap instead: sites already in the
 *   patch map are skipped, and the map drops entries whose bytes the guest has since
 *   overwritten, so a later pass re-patches what a copy undid.
 */
VOID DpmiScanCodeBlocks(VOID)
{
    INT index;

    for (index = 0; index < g_DpmiBlockCount; ++index)
        if (g_DpmiBlock[index].Code)
            DpmiPatchCodeRegion(g_DpmiBlock[index].Base, g_DpmiBlock[index].Size - 1,
                                   g_DpmiIsClient32);
}

enum
{
    BREAKPOINT_COLUMN_LINEAR = 0, BREAKPOINT_COLUMN_DUMP = 1, BREAKPOINT_COLUMN_SKIP = 2, BREAKPOINT_COLUMN_MODE = 3, BREAKPOINT_COLUMN_REPORT = 4, BREAKPOINT_COLUMNS = 5
};   /* pmbreak.txt: one breakpoint a line */
/* Read PMBP_PATH. One line per breakpoint:
 *   <addr>  [dump linear]  [skip bytes]  [mode]  [rep]   # comment
 * mode is a bit field: bit 0 (1) = the site is a ONE-BYTE instruction, so plant a
 * one-byte 0xCC rather than the two-byte BOP; bit 2 (4) = the DUMP column is an offset
 * from DS's base rather than a linear address; bit 1 (2) = <addr> is an OFFSET INTO
 * krnl386's protected-mode copy of a segment, resolved when that selector is committed
 * (see DpmiBreakpointResolveSegment -- the copies move between runs, so a segment-relative
 * offset is the only form of that address worth writing down), with mode bits 4..7 naming WHICH
 * segment:
 * `2` = seg 1 (0 reads as 1, so old lists still work), `0x22` = seg 2, `0x32` = seg 3.
 *   <hex linear addr to break on>  [hex linear addr to DUMP on hit]   # comment
 * The optional second column is what makes this a debugger rather than a tracer:
 * "stop here and show me that memory" is the question you actually have when a
 * register holds a value you cannot explain -- as BX=0x8b17 did, read from a PSP
 * field whose contents nothing in the log could show. '#' comments, blanks ignored.
 * Absent file = no breakpoints and no cost, like every other knob here.
 *
 * [CAUTION]: THE BUFFER IS PART OF THE INSTRUMENT, AND IT LIED. (session 37) (Importance = 1):
 * This read `char buf[1024]` and said nothing about what did not fit. A list of
 * eleven breakpoints whose header comment explained the two 0x0B sites it was
 * bisecting came to ~740 bytes of comment, so the first FIVE entries were parsed
 * and the other six -- including every site the run existed to observe -- were
 * silently dropped. The log showed five confident arms and 42 hits, and answered
 * nothing: the same shape as the one-shot that fired 512 times.
 *
 * 8 KB, and SAY what was loaded: the count, and a loud line if the file was longer
 * than the buffer or if DPMI_BP_MAX was reached. A parser that discards input
 * without a word is not an instrument.
 */
VOID DpmiBreakpointLoad(VOID)
{
    HANDLE handle = CreateFileA(PMBP_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    CHAR buffer[8192];
    DWORD bytesRead = 0;
    DWORD index = 0;
    CHAR lineBuffer[256];
    CHAR *cursor = lineBuffer;
    INT countBefore = g_BreakpointCount;

    if (handle == INVALID_HANDLE_VALUE)
        return;
    ReadFile(handle, buffer, sizeof buffer - 1, &bytesRead, NULL);
    CloseHandle(handle);
    while (index < bytesRead && g_BreakpointCount < DPMI_BP_MAX)
    {
        DWORD values[BREAKPOINT_COLUMNS] = { 0, 0, 0, 0, 0 };
        INT column = 0;
        /* consume one LINE, taking up to two hex fields from it */
        while (index < bytesRead && (buffer[index] == '\r' || buffer[index] == '\n'))
            ++index;                                                                             /* line breaks */
        if (index >= bytesRead)
            break;
        if (buffer[index] == '#')
        {
            while (index < bytesRead && buffer[index] != '\n')
                ++index;
            continue;
        }
        while (index < bytesRead && buffer[index] != '\r' && buffer[index] != '\n')
        {
            INT digits = 0;
            while (index < bytesRead && (buffer[index] == ' ' || buffer[index] == '\t'))
                ++index;
            if (index >= bytesRead || buffer[index] == '\r' || buffer[index] == '\n')
                break;
            if (buffer[index] == '#')
            {
                while (index < bytesRead && buffer[index] != '\n')
                    ++index;
                break;
            }
            while (index < bytesRead)
            {
                CHAR character = buffer[index];
                INT digit = (character >= '0' && character <= '9') ? character - '0'
                      : (character >= 'a' && character <= 'f') ? character - 'a' + HEX_DIGIT_A_VALUE
                      : (character >= 'A' && character <= 'F') ? character - 'A' + HEX_DIGIT_A_VALUE : -1;
                if (digit < 0)
                    break;
                if (column < BREAKPOINT_COLUMNS)
                    values[column] = (values[column] << NIBBLE_SHIFT) | (DWORD)digit;
                ++digits;
                ++index;
            }
            if (digits)
                ++column;
            else
                ++index;                               /* junk byte: skip, never spin */
        }
        if (column > BREAKPOINT_COLUMN_LINEAR)
        {
            g_BreakpointLinear[g_BreakpointCount] = values[BREAKPOINT_COLUMN_LINEAR];
            g_BreakpointDump[g_BreakpointCount] = (column > BREAKPOINT_COLUMN_DUMP) ? values[BREAKPOINT_COLUMN_DUMP] : 0;
            g_BreakpointSkip[g_BreakpointCount] = (column > BREAKPOINT_COLUMN_SKIP) ? values[BREAKPOINT_COLUMN_SKIP] : 0;
            g_BreakpointMode[g_BreakpointCount] = (column > BREAKPOINT_COLUMN_MODE) ? values[BREAKPOINT_COLUMN_MODE] : 0;
            g_BreakpointReport[g_BreakpointCount]  = (column > BREAKPOINT_COLUMN_REPORT) ? values[BREAKPOINT_COLUMN_REPORT] : 0;
            ++g_BreakpointCount;
        }
    }
    cursor = LogPut(cursor, "DPMI-BP: pmbp.txt read 0x"); cursor = LogHex(cursor, bytesRead);
    cursor = LogPut(cursor, " bytes, loaded "); cursor = LogHex(cursor, (DWORD)(g_BreakpointCount - countBefore));
    cursor = LogPut(cursor, " entries (total "); cursor = LogHex(cursor, (DWORD)g_BreakpointCount); cursor = LogPut(cursor, ")");
    if (bytesRead >= sizeof buffer - 1)
        cursor = LogPut(cursor, "  !! FILE TRUNCATED AT THE BUFFER -- ENTRIES WERE DROPPED");
    if (g_BreakpointCount >= DPMI_BP_MAX && index < bytesRead)
        cursor = LogPut(cursor, "  !! DPMI_BP_MAX REACHED -- REMAINING LINES IGNORED");
    cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
}

/* Turn `seg1:0xNNNN` breakpoints into linear ones, now that we know where krnl386 put
 * its protected-mode copy of segment 1. Rewrites the entry in place and clears the flag,
 * so everything downstream (arming, hit matching, disarming) stays absolute and unchanged.
 * Logged, because a breakpoint that silently lands somewhere else is worse than none.
 */
/* AND THE SAME FOR krnl386's OTHER SEGMENTS. (session 36) (Importance = 1):
 * Bit 1 alone meant "segment 1", which was enough while everything interesting was
 * in seg1. It is not any more: the module-load path runs in **segment 2** --
 * krnl386 calls a module's entry point from there (observed), and the COMM.DRV
 * investigation needs the register file either side of that call. `g_WowPmBase[]`
 * has recorded every segment's base all along; only the resolver was seg1-only.
 *
 * Mode bits 4..7 now carry the SEGMENT NUMBER when bit 1 is set. `2` still means
 * segment 1 (0 is read as 1, so every existing pmbp.txt keeps working); `0x22` is
 * segment 2, `0x32` segment 3, and so on.
 */
/* -- AND THE SAME FOR AN EXTENDED DOS GUEST: MODE BIT 8 = "OFFSET FROM THE LE
 * CODE-OBJECT LOAD BASE". (session 59) --------------------------------------------
 * The argument is the one DpmiBreakpointResolveSegment already makes, arriving from the other
 * direction. krnl386's segment copies move every run; so does a DOS/4GW client's
 * image -- ZAR's LE code object came up at 0x03f70000 on one run and 0x03b70000 on
 * the next. Every interesting address in a #23 investigation is `obj1+0xNNNN` read
 * off a disassembly, and an absolute breakpoint list is a list that is wrong by the
 * next run. Session 32 lost readings to exactly this on the WOW side; `pmwatch.txt`
 * grew a `+` for it this session; this is the third instance and the last place that
 * still demanded a hand-copied address.
 *
 * `<addr> <dump> <skip> 8 <rep>` means `addr` is an offset from the base the host
 * itself prints as `[LE CODE OBJECT] -> mem 0x...`. Resolved when that allocation
 * happens, in place, clearing the bit -- so arming, hit matching and disarming stay
 * absolute and completely unchanged, exactly as the segment resolver does it.
 *
 * [CAUTION]: Bit 8, not bit 2: bit 2 is taken and its meaning (krnl386's segments, with the
 * segment number in bits 4..7) must not be disturbed. The two cannot both apply --
 * one guest is WOW, the other is an extended DOS program -- but they are checked
 * independently so a nonsense combination resolves once and stops, rather than
 * resolving twice into a wild address.
 */
VOID DpmiBreakpointResolveCodeBase(DWORD base)
{
    CHAR lineBuffer[200];
    CHAR *cursor;
    INT index;

    for (index = 0; index < g_BreakpointCount; ++index)
    {
        DWORD offset;
        if (!(g_BreakpointMode[index] & BREAKPOINT_MODE_LE_CODE))
            continue;
        offset = g_BreakpointLinear[index];
        g_BreakpointLinear[index]   = base + offset;
        g_BreakpointMode[index] &= ~(DWORD)BREAKPOINT_MODE_LE_CODE;
        cursor = lineBuffer;
        cursor = LogPut(cursor, "DPMI-BP: codebase+0x"); cursor = LogHex(cursor, offset);
        cursor = LogPut(cursor, " -> linear 0x");        cursor = LogHex(cursor, g_BreakpointLinear[index]);
        cursor = LogPut(cursor, " (the client's LE code object is at 0x"); cursor = LogHex(cursor, base);
        cursor = LogPut(cursor, ")\r\n");
        LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
    }
}

VOID DpmiBreakpointResolveSegment(UINT segmentNumber, DWORD base)
{
    CHAR lineBuffer[200];
    CHAR *cursor;
    INT index;

    for (index = 0; index < g_BreakpointCount; ++index)
    {
        DWORD offset;
        UINT want;
        if (!(g_BreakpointMode[index] & BREAKPOINT_MODE_WOW_SEGMENT))
            continue;
        want = (g_BreakpointMode[index] >> NIBBLE_SHIFT) & NIBBLE_MASK;
        if (!want)
            want = 1;                              /* bare `2` is segment 1, as before */
        if (want != segmentNumber)
            continue;
        offset = g_BreakpointLinear[index] & WORD_MASK;
        g_BreakpointLinear[index]   = base + offset;
        g_BreakpointMode[index] &= ~(DWORD)BREAKPOINT_MODE_WOW_SEGMENT;
        cursor = lineBuffer;
        cursor = LogPut(cursor, "DPMI-BP: seg");    cursor = LogHex(cursor, (DWORD)segmentNumber);
        cursor = LogPut(cursor, ":0x");             cursor = LogHex(cursor, offset);
        cursor = LogPut(cursor, " -> linear 0x");   cursor = LogHex(cursor, g_BreakpointLinear[index]);
        cursor = LogPut(cursor, " (krnl386's PM copy of segment "); cursor = LogHex(cursor, (DWORD)segmentNumber);
        cursor = LogPut(cursor, " is at 0x"); cursor = LogHex(cursor, base);
        cursor = LogPut(cursor, ")\r\n");
        LogAppend(LOG_PATH, lineBuffer, cursor);
    }
}

/* Arm any requested breakpoint whose address is now present in guest memory. Called
 * after the up-front INT scan and after every code-region patch, because a module the
 * client loads at runtime does not exist to be patched before then.
 */
VOID DpmiBreakpointArm(VOID)
{
    INT index;

    for (index = 0; index < g_BreakpointCount; ++index)
    {
        DWORD linear = g_BreakpointLinear[index];
        volatile BYTE *bytes;
        /* 320, not 128: the REFUSED-one-byte-instruction message below is ~180
         * bytes and overflowed the old buffer, corrupting this function's stack and
         * taking the host down with an access violation before the guest ever ran.
         * The fault dump named it outright -- EDX held 0x33746e69, the ASCII "int3"
         * from that very string. zput/zhex are caller-sized and check nothing, so
         * the buffer has to be big enough for the LONGEST line, not the usual one.
         */
        CHAR lineBuffer[320];
        CHAR *cursor = lineBuffer;
        if (linear < DPMI_PATCH_FLOOR)
            continue;
        /* [CAUTION]: AN UNRESOLVED SEGMENT-RELATIVE ADDRESS IS NOT A LINEAR ADDRESS (Importance =
         * 2): Mode bit 1 means "<addr> is an OFFSET into krnl386's PM copy of a segment", and
         * DpmiBreakpointResolveSegment() rewrites it to a linear address when that selector is
         * committed. Until then the field holds a bare offset like 0x0e11 -- and this loop was
         * arming it AS a linear address, planting BOPs into CONVENTIONAL MEMORY: our own DOS kernel
         * and krnl386's V86 image.
         *
         * [CAUTION]: MEASURED, and it killed a run: sixteen seg2-relative breakpoints armed at
         * linear 0x0642..0x0f99 before krnl386 had committed segment 2, and the guest
         * died in its own bring-up with zero breakpoint hits. It went unnoticed while
         * only ONE such breakpoint was ever used, because a single stray BOP happened
         * to land where `b[0]==0 && b[1]==0` skipped it.
         *
         * Not armed until resolved. An address we cannot place yet is not an address.
         */
        if (g_BreakpointMode[index] & BREAKPOINT_MODE_WOW_SEGMENT)
            continue;
        /* NEVER RE-PLANT A BREAKPOINT THE GUEST IS STANDING ON (Importance = 2):
         * A hit removes the BOP and, for a repeating breakpoint, sets g_BreakpointPending;
         * DpmiBreakpointRearmPending() then clears that flag only once the guest's EIP has
         * moved off the site, and re-arms. That protocol is correct -- but it lived
         * entirely in the CALLER, so this function would happily re-plant the BOP
         * under a guest that has not executed the instruction yet.
         *
         * [CAUTION]: MEASURED, IMMEDIATELY: arming before every PM entry (needed because krnl386
         * loads its own segments late, so a breakpoint inside them is skipped at setup
         * while the memory still reads 00 00) hit one krnl386 site **340,808 times** with
         * byte-identical registers and one millisecond on the clock, and wrote a
         * **268 MB** log. The guest was not looping; the debugger was holding it in
         * place. Honouring `pending` here makes DpmiBreakpointArm() safe to call from
         * anywhere, which is what the late-loading case needs.
         */
        if (g_BreakpointPending[index])
            continue;
        if (g_BreakpointDone[index])
            continue;                                       /* one-shot, already fired -- see g_BreakpointDone */
        /* And a hard ceiling, because the failure above cost a run and a quarter of a
         * gigabyte before anything noticed. A breakpoint that has fired this often is
         * not answering a question any more.
         */
        if (g_BreakpointArms[index] > DPMI_BP_ARM_MAX)
            continue;
        bytes = (volatile BYTE *)(ULONG_PTR)linear;
        /* - THE TARGET NEED NOT EXIST YET, and reading it blindly faults in OUR OWN
         * process. A breakpoint is usually placed on a module the client has not
         * loaded at the time the list is read -- extended-memory addresses are not
         * even allocated until the client asks for them -- so this arming pass runs
         * repeatedly and must tolerate an address that is currently nothing. Same
         * lesson as IsBadReadPtr: an instrument that faults kills the run it exists
         * to observe.
         */
        if (!HostReadable((const VOID *)(ULONG_PTR)linear, 2))
            continue;
        /* - RE-ARM IF THE CLIENT OVERWROTE US, and skip empty memory entirely. The
         * first version armed at mode-switch time into memory the client had not
         * loaded yet -- every breakpoint reported "was 00 00", the module was then
         * READ IN OVER THE TOP, and not one of them fired. The instrument looked
         * like it worked (twelve confident "armed at" lines) and measured nothing,
         * which is this project's most familiar failure mode.
         * So: an armed breakpoint whose bytes are no longer our BOP has been
         * clobbered and must be re-armed against the NEW contents; and a site that
         * is still 00 00 holds nothing to break on, so leave it unarmed and try
         * again after the next region is loaded.
         */
        if (g_BreakpointArmed[index])
        {
            if (g_BreakpointMode[index] == BREAKPOINT_MODE_INT3 ? (bytes[0] == X86_OP_INT3)
                                  : (bytes[0] == VDM_BOP0 && bytes[1] == VDM_BOP1))
                continue;                                                                      /* still planted */
            /* [CAUTION]: A HALF-CLOBBERED BOP MUST NOT BE RE-CAPTURED AS THE ORIGINAL (Importance =
             * 2): Falling straight through to the re-arm below saves whatever is at the site RIGHT
             * NOW as `the guest's instruction` -- and when only the SECOND byte was overwritten,
             * byte 0 is still OUR OWN 0xC4. Disarming then writes `C4 <x>` back into the guest
             * permanently: a `LES` where an instruction used to be.
             *
             * [INFO]: MEASURED ON ZAR (#23), and the mechanism is exact. The breakpoint sat
             * on obj1+0x8cdf2 `b8 24 77 5e 00` (mov eax,0x5e7724), so the 2-byte BOP
             * covers the opcode AND THE LOW BYTE OF A RELOCATABLE OPERAND. The LE
             * fixup pass ran AFTER we armed, rewrote the whole dword, and put 0x24
             * back over our second byte -- the log shows `armed ... (displaced b8 24)`
             * followed by `armed ... (displaced c4 24)`, the second being our own BOP
             * recorded as the guest's code.
             *
             * ARMING BEFORE A CLIENT HAS FINISHED ITS FIXUPS IS NORMAL AND CANNOT BE
             * AVOIDED -- the whole reason this runs before every PM entry is that
             * modules appear late. So handle the partial clobber instead of racing
             * it: byte 0 is still ours, therefore the byte 0 we saved first is still
             * the truth; only byte 1's new value is news. Update that half, re-plant
             * ours, and leave the entry armed.
             */
            if (g_BreakpointMode[index] != BREAKPOINT_MODE_INT3 && bytes[0] == VDM_BOP0 && g_BreakpointOriginal[index][0] != VDM_BOP0)
            {
                g_BreakpointOriginal[index][1] = bytes[1];                    /* the guest's new byte 1 */
                bytes[1] = VDM_BOP1;                               /* re-plant the lost half */
                cursor = LogPut(cursor, "DPMI-BP: 0x"); cursor = LogHex(cursor, linear);
                cursor = LogPut(cursor, " half-clobbered (a fixup rewrote the byte under our BOP);"
                            " byte 1 re-planted, saved original now ");
                cursor = LogDump(cursor, (const BYTE *)g_BreakpointOriginal[index], 2);
                cursor = LogPut(cursor, "\r\n");
                LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
                continue;
            }
            g_BreakpointArmed[index] = 0;
            PatchMapClear(linear);            /* clobbered -> re-arm below */
        }
        if (bytes[0] == 0x00 && bytes[1] == 0x00)
            continue;                                              /* nothing loaded here yet */
        if (PatchMapGet(linear))
            continue;                                   /* an INT site already lives here */
        /* - A BREAKPOINT HAS A TWO-BYTE FOOTPRINT, and that is not a detail. It
         * displaces the byte AFTER the one you named, so a breakpoint on a ONE-BYTE
         * instruction eats its neighbour. Session 17 put one on a `c3` (ret) at
         * 0x4a0b; the next byte, 0x4a0c, was the entry point of the routine being
         * called two instructions earlier. `call 0x4a0c` therefore landed on the
         * second half of our BOP, decoded as `LES DX,[BX+0x8b]`, read past the
         * segment limit and killed the VDM -- a death the log presented as the
         * client's, in the middle of a bisection hunting exactly that.
         *
         * [INFO]: SESSION 31: WE CAN NOW PREVENT IT, and the note above saying we cannot
         * is out of date -- x86len.h arrived in session 21 (for the INT-site patcher,
         * which had the same disease) and it decodes instruction LENGTH. So measure
         * the instruction at the site and REFUSE a two-byte BOP over a one-byte one.
         * This is not hypothetical: three of four breakpoints placed on krnl386 this
         * session sat on one-byte instructions, and one of them ate the first byte of
         * the following instruction -- which was on the path that sets AX for
         * krnl386's FIRST INT 31h. The guest asked for 0x0000 instead of 0x000A and
         * died at PM step 1 instead of step 0x31, and it took a cross-run comparison
         * of the step counter to notice that the debugger was the bug.
         * Use mode 1 (a one-byte 0xCC) for a one-byte instruction, or move the
         * breakpoint. Either way the guest is no longer silently corrupted.
         */
        { INT other, clash = 0;
          for (other = 0; other < g_BreakpointCount; ++other)
              if (other != index && g_BreakpointArmed[other] &&
                  (g_BreakpointLinear[other] == linear + 1 || g_BreakpointLinear[other] + 1 == linear))
                  clash = 1;
          if (clash)
          {
              if (!g_BreakpointRefused[index])
              {
                  g_BreakpointRefused[index] = 1;
                  cursor = LogPut(cursor, "DPMI-BP: REFUSED 0x"); cursor = LogHex(cursor, linear);
                  cursor = LogPut(cursor, " -- its 2-byte footprint overlaps another breakpoint"
                              " (said once; re-checked every PM entry)\r\n");
                  LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
              }
              continue;
          } }
        if (g_BreakpointMode[index] != BREAKPOINT_MODE_INT3 && HostReadable((const VOID *)(ULONG_PTR)linear, 16))
        {
            UINT instructionLength = X86InstructionLength((const BYTE *)(ULONG_PTR)linear, 0, X86_MAX_INSTRUCTION,
                                         g_DpmiIsClient32);
            if (instructionLength == 1)
            {
                if (!g_BreakpointRefused[index])
                {
                    g_BreakpointRefused[index] = 1;
                    cursor = LogPut(cursor, "DPMI-BP: REFUSED 0x"); cursor = LogHex(cursor, linear);
                    cursor = LogPut(cursor, " -- ONE-BYTE instruction (");
                    cursor = LogDump(cursor, (const BYTE *)(ULONG_PTR)linear, 1);
                    cursor = LogPut(cursor, "); a 2-byte BOP would eat the NEXT instruction and "
                                "silently change what the guest does. Use mode 1 (int3) "
                                "or move it. (said once; re-checked every PM entry)\r\n");
                    LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
                }
                continue;
            }
        }
        g_BreakpointOriginal[index][0] = bytes[0];
        g_BreakpointOriginal[index][1] = bytes[1];
        ++g_BreakpointArms[index];
        if (g_BreakpointMode[index] == BREAKPOINT_MODE_INT3)
        {
            bytes[0] = X86_OP_INT3;                      /* INT3: one byte, fits over CLI/STI */
        }
        else
        {
            bytes[0] = VDM_BOP0;
            bytes[1] = VDM_BOP1;
            PatchMapSet(linear, DPMI_BP_VEC);       /* only a BOP is resolvable by vector */
        }
        g_BreakpointArmed[index] = 1;
        g_BreakpointPending[index] = 0;
        cursor = LogPut(cursor, "DPMI-BP: armed at linear 0x"); cursor = LogHex(cursor, linear);
        cursor = LogPut(cursor, "..0x"); cursor = LogHex(cursor, linear + 1);       /* say the 2-byte footprint */
        cursor = LogPut(cursor, " (displaced "); cursor = LogDump(cursor, (const BYTE *)g_BreakpointOriginal[index], 2);
        cursor = LogPut(cursor, ")\r\n");
        LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
    }
}

/* Re-plant any breakpoint that was stepped over, once the guest is no longer standing
 * on its footprint. Called at every PM event, which is the first safe moment.
 */
VOID DpmiBreakpointRearmPending(DWORD currentLinear)
{
    INT index;
    INT any = 0;

    for (index = 0; index < g_BreakpointCount; ++index)
        if (g_BreakpointPending[index] && currentLinear != g_BreakpointLinear[index] && currentLinear != g_BreakpointLinear[index] + 1)
        {
            g_BreakpointPending[index] = 0;
            any = 1;
        }
    if (any)
        DpmiBreakpointArm();
}

/* Disarm the breakpoint at `lin` (restore its bytes). Returns its index, or -1. */
INT DpmiBreakpointDisarm(DWORD linear)
{
    INT index;

    for (index = 0; index < g_BreakpointCount; ++index)
        if (g_BreakpointArmed[index] && g_BreakpointLinear[index] == linear)
        {
            volatile BYTE *bytes = (volatile BYTE *)(ULONG_PTR)linear;
            bytes[0] = g_BreakpointOriginal[index][0];
            if (g_BreakpointMode[index] != BREAKPOINT_MODE_INT3)
                bytes[1] = g_BreakpointOriginal[index][1];
            PatchMapClear(linear);
            g_BreakpointArmed[index] = 0;
            return index;
        }
    return -1;
}

/* Un-patch / re-patch the shared code segment around a V86 excursion (INT 31h 0301/
 * 0303). The switch-time scan rewrote every `CD 31`/`CD 21` in the code segment to a
 * BOP so PM software-ints reflect to us -- but that segment is ALSO the V86 view, so a
 * real-mode INT inside a 0301 proc would hit a corrupted BOP instead of vectoring
 * natively. g_int_vec[] is the revert map (offset -> original vector), so we restore
 * the real `CD nn` bytes before running V86 (real-mode ints then vector through the
 * IVT to our BOP stubs and are serviced normally) and re-apply the BOP patch before
 * resuming the PM client.
 */
/* THE PATCH MAP MUST VERIFY BEFORE IT WRITES:
 * These used to rewrite every recorded site unconditionally, and that CORRUPTS LIVE
 * DATA. Doom found it: the client declares a base-0 64K code selector, so the scan
 * records `CD nn` pairs all over low memory -- including addresses that later become
 * DOS/4GW's FILE TRANSFER BUFFER. Every INT 31h 0301/0302 unpatches, runs the real-mode
 * read (which fills that buffer with image bytes), then repatches -- stamping C4 C4 over
 * two bytes of freshly-read program image. The client copied that up to extended memory
 * and its relocation pass then read 0xC4C4 where an object index belonged:
 *   file    9a a4 59 80 | 00 83 | c4 08
 *   memory  9a a4 59 80 | c4 c4 | c4 08
 * Verifying first makes the map SELF-CORRECTING: if the bytes are no longer what we put
 * there, the guest has reused that memory, so the site is stale -- drop it and never
 * touch those bytes again. That is strictly better than trying to predict which regions
 * the guest will reuse, which is not knowable.
 */
VOID DpmiUnpatch(VOID)
{
    DWORD slot;

    for (slot = 0; slot < DPMI_PMAP_SLOTS; ++slot)
    {
        DWORD address = g_PatchMapLinear[slot];
        BYTE vector = g_PatchMapVector[slot];
        if (!address || !vector || vector == DPMI_BP_VEC)
            continue;                                                         /* a BP is not an INT site */
        { volatile BYTE *bytes = (volatile BYTE *)(ULONG_PTR)address;
          if (bytes[0] == VDM_BOP0 && bytes[1] == VDM_BOP1)
          {
              bytes[0] = X86_OP_INT;
              bytes[1] = vector;
          }
          else
              g_PatchMapVector[slot] = 0; }                          /* stale: guest reused it */
    }
}

VOID DpmiRepatch(VOID)
{
    DWORD slot;

    for (slot = 0; slot < DPMI_PMAP_SLOTS; ++slot)
    {
        DWORD address = g_PatchMapLinear[slot];
        BYTE vector = g_PatchMapVector[slot];
        if (!address || !vector || vector == DPMI_BP_VEC)
            continue;                                                         /* a BP stays planted */
        { volatile BYTE *bytes = (volatile BYTE *)(ULONG_PTR)address;
          if (bytes[0] == X86_OP_INT && bytes[1] == vector)
          {
              bytes[0] = VDM_BOP0;
              bytes[1] = VDM_BOP1;
          }
          else
              g_PatchMapVector[slot] = 0; }                          /* stale: guest reused it */
    }
}

/* Forward decl: the shared PM-interrupt dispatcher (defined after this fn). A callback
 * handler that issues its own INT 31h/21h routes through it, same as the main PM loop.
 */
/* THE DEFAULT PM STUBS MUST BE AS WIDE AS THE CLIENT:
 * Each default vector is `C4 C4 CF`: a BOP we service, then an IRET that returns to
 * whoever chained here. The selector was built 16-bit unconditionally ("the stubs are
 * 16-bit"), which is right for a 16-bit client and WRONG for DOS/4GW -- and Doom chains
 * to it. Measured: Doom's timer ISR runs clean for five ticks, then on the sixth it
 * chains to the previous vector-8 handler, which INT 31h 0204 reported as our default
 * stub (`0x37:0x18`). We service the BOP, advance past it, and the guest is left at
 * `0x37:0x1a` about to execute `CF` -- a SIXTEEN-BIT IRET popping the TWELVE-byte frame
 * a 32-bit ISR pushed. It takes 6 bytes, lands on garbage, and the kernel tears the VDM
 * down with no diagnostic. That is the whole "dies ~5 ticks in".
 * One bit fixes it: with D/B set, the same `CF` is an IRETD. This is the third time this
 * project has paid for "frame width and descriptor width are the same question" -- see
 * the initial-selector and PM-return-catcher notes. The client's width is not known when
 * the table is built, so sync it at use.
 */
static VOID DpmiSyncDefaultSelectorWidth(VOID)
{
    BYTE want;

    if (g_PmDefaultIndex < 0)
        return;
    want = (BYTE)(g_DpmiIsClient32 ? DPMI_DESCRIPTOR_FLAG_BIG : 0);      /* 0x4 = D/B, same idiom as the handler code sel */
    if (g_Ldt[g_PmDefaultIndex].Flags == want)
        return;
    g_Ldt[g_PmDefaultIndex].Flags = want;
    DpmiInstall(g_PmDefaultIndex);
}

enum
{
    DPMI_CALLBACK_STACK_TOP = 0xF400, DPMI_CALLBACK_PHASE_MAX = 64
};   /* DpmiInvokeCallback: the PM stack it lends, the run's bound */
/* Invoke a DPMI 0303 real-mode callback: the guest (running in V86 during a 0301
 * excursion) far-called a planted callback BOP -- switch V86->PM, run the client's
 * PM handler with the real-mode register state marshalled into its RMCS, then resume
 * V86 at the far-call's return address. The inverse of 0301's PM->V86 direction.
 * On entry the CONTEXT holds the V86 state at the far-call (segment un-patched).
 */
VOID DpmiInvokeCallback(DOS_MACHINE *machine, volatile BYTE *tib, INT slot)
{
    CHAR lineBuffer[256];
    PSTR lineCursor = lineBuffer;
    WORD realSs = (WORD)VDM_REG(tib, VTIB_SS);
    WORD realSp = (WORD)VDM_REG(tib, VTIB_ESP);
    DWORD realStack = ((DWORD)realSs << PARAGRAPH_SHIFT) + realSp;
    /* the far-call return frame */
    WORD retIP = PeekWord(realStack);
    WORD retCS = PeekWord(realStack + X86_FRAME16_CS);
    WORD newSP = (WORD)(realSp + X86_FAR_RETURN16_SIZE);                          /* pop it */
    DWORD callStructure = DpmiSelectorBase(g_Callbacks[slot].RmEs) + g_Callbacks[slot].RmDi;
    volatile BYTE *callStructureBytes = (volatile BYTE *)(ULONG_PTR)callStructure;
    WORD machineStatusWord = *(volatile WORD *)(tib + VTIB_MSW);
    UINT phase;
    INT callbackDone = 0;

    /* fill the callback's RMCS with the real-mode register file + the return CS:IP:SS:SP */
    *(volatile WORD*)(callStructureBytes+RMCS_EDI)=VDM_REG(tib,VTIB_EDI);
    *(volatile WORD*)(callStructureBytes+RMCS_ESI)=VDM_REG(tib,VTIB_ESI);
    *(volatile WORD*)(callStructureBytes+RMCS_EBP)=VDM_REG(tib,VTIB_EBP);
    *(volatile WORD*)(callStructureBytes+RMCS_EBX)=VDM_REG(tib,VTIB_EBX);
    *(volatile WORD*)(callStructureBytes+RMCS_EDX)=VDM_REG(tib,VTIB_EDX);
    *(volatile WORD*)(callStructureBytes+RMCS_ECX)=VDM_REG(tib,VTIB_ECX);
    *(volatile WORD*)(callStructureBytes+RMCS_EAX)=VDM_REG(tib,VTIB_EAX);
    *(volatile WORD*)(callStructureBytes+RMCS_ES)=VDM_REG(tib,VTIB_ES);
    *(volatile WORD*)(callStructureBytes+RMCS_DS)=VDM_REG(tib,VTIB_DS);
    *(volatile WORD*)(callStructureBytes+RMCS_FLAGS)=(WORD)VDM_REG(tib,VTIB_EFLAGS);
    *(volatile WORD*)(callStructureBytes+RMCS_IP)=retIP;
    *(volatile WORD*)(callStructureBytes+RMCS_CS)=retCS;   /* CS:IP = far-call return */
    *(volatile WORD*)(callStructureBytes+RMCS_SP)=newSP;
    *(volatile WORD*)(callStructureBytes+RMCS_SS)=realSs;     /* SS:SP after popping it */
    lineCursor = LogPut(lineCursor, "  0303-cb slot "); lineCursor = LogHex(lineCursor, slot); lineCursor = LogPut(lineCursor, " ret=0x"); lineCursor = LogHex(lineCursor, retCS);
    lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, retIP); lineCursor = LogPut(lineCursor, " -> PM handler 0x"); lineCursor = LogHex(lineCursor, g_Callbacks[slot].PmSelector);
    lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, g_Callbacks[slot].PmOffset); lineCursor = LogPut(lineCursor, "\r\n");
    LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor); lineCursor = lineBuffer;
    /* re-arm the BOP patch (the PM handler is protected-mode code), enter PM */
    DpmiRepatch();
    *(volatile WORD *)(tib + VTIB_MSW) = (WORD)(machineStatusWord | MSW_PE_BIT);
    /* PM handler stack (data selector 0x17, scratch SP) with an IRET frame -> PM-return catcher.
     * The IRET operand size follows the HANDLER's CS D-bit: a 32-bit PM handler pops a dword
     * FLAGS/CS/EIP frame, a 16-bit one pops a word frame (GH #18 run 83).
     */
    { WORD protectedSs = DPMI_INITIAL_DATA_SELECTOR, protectedSp = DPMI_CALLBACK_STACK_TOP;
    DWORD stackBase = DpmiSelectorBase(protectedSs);
      if (DpmiSelectorIs32(g_Callbacks[slot].PmSelector))
      {
          protectedSp -= X86_DWORD_SIZE;
          PokeDword(stackBase + protectedSp, EFLAGS_IF | EFLAGS_RESERVED_ONE);        /* EFLAGS */
          protectedSp -= X86_DWORD_SIZE;
          PokeDword(stackBase + protectedSp, g_PmReturnSelector);       /* CS (dword; hi16=0) */
          protectedSp -= X86_DWORD_SIZE;
          PokeDword(stackBase + protectedSp, DPMI_PMRET_OFF);    /* EIP */
      }
      else
      {
          protectedSp -= X86_WORD_SIZE;
          PokeWord(stackBase + protectedSp, EFLAGS_IF | EFLAGS_RESERVED_ONE);            /* FLAGS */
          protectedSp -= X86_WORD_SIZE;
          PokeWord(stackBase + protectedSp, g_PmReturnSelector);       /* CS */
          protectedSp -= X86_WORD_SIZE;
          PokeWord(stackBase + protectedSp, DPMI_PMRET_OFF);    /* IP */
      }
      VDM_SET16(tib, VTIB_SS, protectedSs);
      VDM_REG(tib, VTIB_ESP) = protectedSp; }
    VDM_REG(tib, VTIB_EFLAGS) = VTIB_EFLAGS_PM;
    VDM_SET16(tib, VTIB_CS, g_Callbacks[slot].PmSelector);
    VDM_REG(tib, VTIB_EIP) = g_Callbacks[slot].PmOffset;
    VDM_SET16(tib, VTIB_ES, g_Callbacks[slot].RmEs);
    VDM_REG(tib, VTIB_EDI) = g_Callbacks[slot].RmDi;  /* ES:DI = RMCS */
    VDM_SET16(tib, VTIB_DS, DPMI_INITIAL_DATA_SELECTOR);
    VDM_REG(tib, VTIB_ESI) = 0;
    /* run the PM handler until it IRETs onto the PM-return catcher (g_PmReturnSelector:PMRET_OFF).
     * A handler that itself issues INT 31h/21h now routes through the shared dispatcher
     * DpmiServicePmInt() -- the same full surface the main PM loop gets (GH #2), so a
     * callback can allocate descriptors, print, sim-real-mode-int, etc.
     */
    for (phase = 0; phase < DPMI_CALLBACK_PHASE_MAX && !callbackDone; ++phase)
    {
        DWORD event;
        DWORD eip;
        DWORD vector;
        DpmiArmFaultTrampoline(tib, 0);   /* GH #18: re-arm the PM-fault reflect (no-op on interp path) */
        DpmiEnterProtectedMode(tib);
        event = VDM_REG(tib, VTIB_EVENT);
        eip = DpmiPmEip(tib);
        if (event == VDM_EVENT_BOP && eip == DPMI_PMRET_OFF
            && VDM_REG16(tib, VTIB_CS) == g_PmReturnSelector)
        {
            callbackDone = 1;
            break;
        }
        if (event == 3)
            continue;               /* DpmiEnterProtectedMode reports "interrupt pending, not entered" -- retry */
        vector = (event == VDM_EVENT_BOP) ? DpmiBopVector(VDM_REG16(tib, VTIB_CS), eip) : 0;   /* a patched INT the handler issued */
        if (vector == VECTOR_DPMI || vector == VECTOR_DOS)
        {
            if (DpmiServicePmInt(machine, tib, vector, phase) > 0)
                continue;                                                      /* serviced -> resume handler */
            callbackDone = 1;
            break;   /* client-exit or unexpected from inside a callback: end the loop */
        }
        lineCursor = LogPut(lineCursor, "  0303-cb: unexpected PM stop ev=0x"); lineCursor = LogHex(lineCursor, event);
        lineCursor = LogPut(lineCursor, " CS:IP=0x"); lineCursor = LogHex(lineCursor, VDM_REG16(tib, VTIB_CS));
        lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, eip); lineCursor = LogPut(lineCursor, "\r\n");
        LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor); lineCursor = lineBuffer;
        break;
    }
    /* handler done: un-patch again and resume the RM proc in V86 at the RMCS CS:IP */
    DpmiUnpatch();
    *(volatile WORD *)(tib + VTIB_MSW) = machineStatusWord;
    VDM_REG(tib, VTIB_EFLAGS) = EFLAGS_VM | EFLAGS_IF | EFLAGS_RESERVED_ONE;
    VDM_REG(tib,VTIB_EDI)=*(volatile WORD*)(callStructureBytes+RMCS_EDI);
    VDM_REG(tib,VTIB_ESI)=*(volatile WORD*)(callStructureBytes+RMCS_ESI);
    VDM_REG(tib,VTIB_EBP)=*(volatile WORD*)(callStructureBytes+RMCS_EBP);
    VDM_REG(tib,VTIB_EBX)=*(volatile WORD*)(callStructureBytes+RMCS_EBX);
    VDM_REG(tib,VTIB_EDX)=*(volatile WORD*)(callStructureBytes+RMCS_EDX);
    VDM_REG(tib,VTIB_ECX)=*(volatile WORD*)(callStructureBytes+RMCS_ECX);
    VDM_REG(tib,VTIB_EAX)=*(volatile WORD*)(callStructureBytes+RMCS_EAX);
    VDM_SET16(tib,VTIB_ES,*(volatile WORD*)(callStructureBytes+RMCS_ES));
    VDM_SET16(tib,VTIB_DS,*(volatile WORD*)(callStructureBytes+RMCS_DS));
    VDM_SET16(tib,VTIB_CS,*(volatile WORD*)(callStructureBytes+RMCS_CS));
    VDM_REG(tib,VTIB_EIP)=*(volatile WORD*)(callStructureBytes+RMCS_IP);
    VDM_SET16(tib,VTIB_SS,*(volatile WORD*)(callStructureBytes+RMCS_SS));
    VDM_REG(tib,VTIB_ESP)=*(volatile WORD*)(callStructureBytes+RMCS_SP);
    VDM_SET16(tib,VTIB_FS,*(volatile WORD*)(callStructureBytes+RMCS_SS));
    VDM_SET16(tib,VTIB_GS,*(volatile WORD*)(callStructureBytes+RMCS_SS));
    lineCursor = LogPut(lineCursor, callbackDone ? "  0303-cb: PM handler returned (OK)\r\n" : "  0303-cb: PM handler NO-RET\r\n");
    LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor); lineCursor = lineBuffer;
}

/* Service one protected-mode interrupt the DPMI client raised (a patched INT nn
 * that reflected as a BOP). `vec` = the ORIGINAL vector (0x31 = DPMI, 0x21 = DOS).
 * Updates the guest CONTEXT with the results (regs + CF) and advances EIP past the
 * 2-byte INT. Returns  1 = serviced, keep running;  0 = client terminated (INT 21h
 * AH=4Ch);  -1 = unexpected / unserviceable stop (already logged).
 * Extracted from the main PM loop so nested handlers -- a 0303 real-mode callback or
 * a 0301 excursion proc that itself issues INT 31h/21h -- get the SAME full dispatch
 * (GH #2). `steps` is only used for a log line. `mp` aliases the machine as `m` so the
 * moved body is byte-for-byte the original (localized, #undef'd immediately).
 */
/* VECTOR A PROTECTED-MODE SOFTWARE INTERRUPT TO THE CLIENT'S OWN HANDLER:
 *
 * THE GAP THIS CLOSES, and it is architectural rather than a missing service.
 * A DPMI client may install its own protected-mode handler for any interrupt (INT 31h
 * 0205), and a host that then services the interrupt ITSELF has taken the client's
 * interrupt away from it. We did exactly that for every PM INT, and the old comment on
 * g_PmInt[] admitted it: "We still service patched INT 21h/31h ourselves -- routing to
 * a client-installed PM handler is a deeper item".
 *
 * It is not deep, it is load-bearing. DOS/4GW installs a PM INT 21h handler at
 * 0x67:0x84 (inside the aliased code window at base 0xd9b0) and then calls its OWN
 * private extender functions through it -- `mov ax,0xff80 / mov dx,0x1301 / mov es,sel
 * / int 21h`, and on CF it prints "DOS/16M error: [34] DPMI host error (cannot lock
 * stack)" and dies. AX=FF80h is not a DOS function and never was; it is DOS/4GW talking
 * to itself, and every answer WE invent for it is wrong. Measured both ways: leaving the
 * flags alone let it limp on to a later failure, returning CF=1 killed it here. The only
 * right answer is to let its handler run.
 *
 * Mechanics are the proven ones from DpmiInjectPmIrq(): push an IRET frame on the
 * client's own stack pointing at the PM-return catcher, vector to the handler, and run
 * it through the SAME dispatcher the main loop uses so a handler that issues INT 31h,
 * port I/O or a nested DOS call still works.
 *
 * - WHAT WE DELIBERATELY DO **NOT** DO IS RESTORE THE REGISTER FILE. An async IRQ is
 *   transparent, so DpmiInjectPmIrq restores everything; a SOFTWARE interrupt is a
 *   call, and its whole purpose is to return AX/BX/CF to the caller. We keep what the
 *   handler produced -- including EFLAGS, which after its IRET holds whatever it wrote
 *   into the stack frame, which is precisely how a DOS handler returns CF.
 *
 * - RE-ENTRANCY: g_PmDispatch[vec] guards the case where the handler issues the same INT
 *   again (a chain back to the host). We then service it ourselves, which is the
 *   correct meaning of "chain to the previous handler" when the previous one is us.
 */
BYTE g_PmDispatch[IVT_VECTORS];                 /* 1 while inside vec's client handler */

/* A REFLECTED INTERRUPT IS THE ONE TRACE THAT CAN OUTRUN THE GUEST (Importance = 4):
 * Every dispatch below writes two LogAppend lines (~350 bytes) and does two
 * HostReadable() probes to print the caller's pointer. That is the right amount of
 * detail for a handler called a dozen times, and a firehose for one the guest POLLS.
 *
 * [INFO]: MEASURED ON ZAR (GH #23), and it is most of why "Game loading..." crawls: while it
 * decompresses ZARN0.SFS the game asks its OWN INT 21h hook for AH=2Ch about 3,800
 * times a second. A 45 s run is ~170,000 reflected dispatches, ~340,000 WriteFile
 * calls and 53 MB of log -- and a histogram of that log is 100% one line, one vector,
 * one AX value, repeated. The reads it is there to explain are 501 lines of it.
 *
 * [INFO]: THIS PROJECT HAS PAID FOR THIS EXACT SHAPE TWICE. Per-line LogAppend under the
 * device lock cost SKYROADS 24% of its delivered timer ticks and only a player's ear
 * caught it (see HostIrqSink); the same argument is written out again over
 * wowquiet.txt. Both times the lesson was "stop writing a kilobyte per event". This
 * is the third site, and the first one on a path a guest can drive at will.
 * - SO BOUND IT PER (VECTOR, AH) -- NOT GLOBALLY. A global cap goes quiet on a RARE
 *   vector because a common one already spent the budget, which is how a bounded log
 *   becomes worse than no log at all (it is the "instrument lying by omission" that
 *   LOG_MAX_BYTES was raised three times to avoid). Keyed this way, INT 21h AH=2Ch
 *   falls silent after its first few and an INT 21h AH=3Fh arriving ten minutes later
 *   still prints in full.
 * - ...AND BOUND IT BY RATE, NOT ONLY BY COUNT. A pure count cap answers the wrong
 *   question: it silences a pair after N lines however slowly they arrive, so the run
 *   that matters -- a guest that is still going ten minutes in -- goes dark exactly
 *   where the evidence is. ZAR is both cases at once: AH=2Ch at ~3,800/s must be
 *   stopped, AH=3Fh at ~6/s (2,553 reads in a seven-minute run) is the ACTUAL SIGNAL
 *   and must not be. So: the first PM_DISP_LOG_MAX of a pair always print (a rare call
 *   is fully visible from its first appearance), and after that a pair may print again
 *   once every PM_DISP_QUIET_MS. A 6/s caller stays fully traced; a 3,800/s one drops
 *   to ten lines a second, ~400x smaller, and never goes silent.
 *
 * [CAUTION]: NOTHING IS LOST, only un-written: every pair is COUNTED whether logged or not and
 * the totals are reported at STAGE2. And a pair says so when it crosses into the
 * rate-limited regime, because a trace that simply stops reads as a crash (the rule
 * LogAppend's own cap follows).
 */
#define PM_DISP_LOG_MAX     24      /* Always-loud lines per (vec, AH) */
#define PM_DISP_QUIET_MS    100u    /* ...then at most one more per pair per this */
static BYTE  g_PmDispatchLogged[IVT_VECTORS][BYTE_VALUES];    /* always-loud lines emitted for (vec, AH) */
DWORD g_PmDispatchCount[IVT_VECTORS][BYTE_VALUES];     /* every dispatch, logged or not */
static DWORD g_PmDispatchMs[IVT_VECTORS][BYTE_VALUES];        /* GetTickCount of the last line for the pair */
enum
{
    DPMI_DISPATCH_PHASE_MAX = 4096
};   /* DpmiDispatchToPmHandler: the nested run's bound */
INT DpmiDispatchToPmHandler(DOS_MACHINE *machine, volatile BYTE *tib, DWORD vector, UINT steps)
{
    /* [CAUTION]: 256 WAS NINE BYTES OF HEADROOM, AND ADDING ONE FIELD BLEW IT (Importance = 2):
     * The entry line below is built in one pass with no bound: vector, handler
     * sel:off, AX, DS:EDX, its linear address, a SIXTEEN-BYTE zdump (48 chars on its
     * own), then SS/ESP/CS with their D/B annotations and isClient32. That is 247 characters
     * of a 256-byte stack buffer. Adding the caller's `from CS:EIP lin=` -- 42 more --
     * overflowed it by 33 and smashed this frame; the host died with
     * `DPMI FATAL: exception code=0xc0000005 ... bytes@fault: 0f b6 01 c0 e8 04` --
     * which is zdump's own nibble-to-hex lookup running on a wrecked pointer, i.e. the
     * formatter faulting several calls downstream of the frame the overflow ruined.
     * - SO SIZE IT FOR THE LINE, NOT FOR THE HABIT. Nothing here counts characters, and
     *   the next field added would have hit this again -- 512 leaves room for one. If
     *   this line ever grows a second dump, split it rather than raising this again.
     */
    CHAR lineBuffer[512];
    CHAR *lineCursor = lineBuffer;
    /* Captured at ENTRY and kept in a LOCAL: the handler's whole job is to change AX, so
     * the exit line must be keyed on what the caller ASKED, not on the answer -- and this
     * function re-enters itself when a handler chains, so a static would cross-talk.
     */
    UINT dispatchVector = vector & BYTE_MASK;
    UINT dispatchAh = (VDM_REG(tib, VTIB_EAX) >> BYTE_SHIFT) & BYTE_MASK;
    DWORD dispatchTick = GetTickCount();
    INT   isFirstDispatch = (g_PmDispatchLogged[dispatchVector][dispatchAh] < PM_DISP_LOG_MAX);
    INT   loud = isFirstDispatch || (dispatchTick - g_PmDispatchMs[dispatchVector][dispatchAh]) >= PM_DISP_QUIET_MS;

    if (g_PmDispatchCount[dispatchVector][dispatchAh] != MAXDWORD)
        ++g_PmDispatchCount[dispatchVector][dispatchAh];
    if (loud)
        g_PmDispatchMs[dispatchVector][dispatchAh] = dispatchTick;
    DWORD sEIP = VDM_REG(tib, VTIB_EIP);
    DWORD sESP = VDM_REG(tib, VTIB_ESP);
    WORD savedCs  = (WORD)VDM_REG(tib, VTIB_CS);
    WORD savedSs = (WORD)VDM_REG(tib, VTIB_SS);
    DWORD sEFL = VDM_REG(tib, VTIB_EFLAGS);
    /* -- THE FRAME WIDTH FOLLOWS THE CLIENT'S MODE, NOT THE HANDLER SELECTOR'S D BIT.
     * This is measured, and getting it wrong is invisible until the handler RETURNS.
     * DOS/4GW's PM INT 21h handler lives in a 16-BIT code selector (0x67, D/B=0) --
     * so DpmiSelectorIs32() says "16-bit" and we pushed a 6-byte frame -- but it ends
     * with `66 cf`, an operand-size-prefixed IRETD, which pops TWELVE bytes. It
     * therefore returned to a wild address and the VDM died, with the last breakpoint
     * sitting on the instruction before it.
     * The client declared itself 32-bit at the mode switch (g_DpmiIsClient32), and an
     * interrupt frame is a DPMI API width -- exactly the distinction the switch code
     * already draws: the declared width is "the right input for DPMI API register
     * widths, not for D/B". A 16-bit client's handler ends in a plain IRET and gets a
     * 6-byte frame, which is the same rule.
     */
    INT isClient32 = g_DpmiIsClient32;
    UINT phase;
    INT done = 0;

    DpmiEnsurePmReturnSelector();
    DpmiSyncDefaultSelectorWidth();     /* the ISR may chain into the default stubs -- see the helper */
    if (g_PmReturnSelector == 0)
        return 0;                                          /* caller falls back to servicing */

    /* - FRAME WIDTH AND STACK-POINTER WIDTH ARE TWO DIFFERENT QUESTIONS, and conflating
     * them faults in OUR OWN process. How many bytes the handler pops is the client's
     * mode (isClient32, above). How the stack is ADDRESSED is the SS descriptor's B bit: with
     * a 16-bit stack selector the CPU maintains SP only, and the top half of ESP holds
     * whatever junk was last there -- 0xb350 in the run that caught this. Using that as
     * an offset from the segment base walked straight off the end of guest memory and
     * the VEH reported an access violation inside pokew(). A 32-bit frame on a 16-bit
     * stack is perfectly ordinary and is exactly what DOS/4GW uses.
     */
    { DWORD stackBase = DpmiSelectorBase(savedSs);
      INT ss32 = DpmiSelectorIs32(savedSs);
      DWORD sp = ss32 ? sESP : (sESP & WORD_MASK);
      if (isClient32)
      {
          sp = ss32 ? sp - X86_DWORD_SIZE : ((sp - X86_DWORD_SIZE) & WORD_MASK);
          PokeDword(stackBase + sp, sEFL);
          sp = ss32 ? sp - X86_DWORD_SIZE : ((sp - X86_DWORD_SIZE) & WORD_MASK);
          PokeDword(stackBase + sp, g_PmReturnSelector);
          sp = ss32 ? sp - X86_DWORD_SIZE : ((sp - X86_DWORD_SIZE) & WORD_MASK);
          PokeDword(stackBase + sp, DPMI_PMRET_OFF);
      }
      else
      {
          sp = ss32 ? sp - X86_WORD_SIZE : ((sp - X86_WORD_SIZE) & WORD_MASK);
          PokeWord(stackBase + sp, (WORD)sEFL);
          sp = ss32 ? sp - X86_WORD_SIZE : ((sp - X86_WORD_SIZE) & WORD_MASK);
          PokeWord(stackBase + sp, g_PmReturnSelector);
          sp = ss32 ? sp - X86_WORD_SIZE : ((sp - X86_WORD_SIZE) & WORD_MASK);
          PokeWord(stackBase + sp, DPMI_PMRET_OFF);
      }
      VDM_REG(tib, VTIB_ESP) = ss32 ? sp : ((sESP & HIGH_WORD_MASK_U) | sp); }

    VDM_SET16(tib, VTIB_CS, g_PmInt[vector].Selector);
    VDM_REG(tib, VTIB_EIP) = isClient32 ? g_PmInt[vector].Offset : (g_PmInt[vector].Offset & WORD_MASK);

    if (!loud)
        goto quietEntry;
    if (isFirstDispatch)
        ++g_PmDispatchLogged[dispatchVector][dispatchAh];
    lineCursor = LogPut(lineCursor, "PM INT 0x"); lineCursor = LogHex(lineCursor, vector);
    lineCursor = LogPut(lineCursor, " -> CLIENT handler 0x"); lineCursor = LogHex(lineCursor, g_PmInt[vector].Selector);
    lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, g_PmInt[vector].Offset);
    lineCursor = LogPut(lineCursor, " AX=0x"); lineCursor = LogHex(lineCursor, VDM_REG16(tib, VTIB_EAX));
    /* - ...AND WHO ASKED. The interrupted CS:EIP is already saved right here (it has to
     * be, to resume the client past its INT) and was never printed -- so a reflected
     * call named the HANDLER and never the CALLER, and "which of the guest's own code
     * is doing this?" needed a separate dig every time. It is the question session 58's
     * handoff left open for ZAR ("catch the last AH=3F read's caller"), and the answer
     * was two saved registers away. Linear too: a flat client's CS base is 0, but a
     * 16-bit one's is not, and the file image is only comparable in linear terms.
     */
    lineCursor = LogPut(lineCursor, " from 0x"); lineCursor = LogHex(lineCursor, savedCs);
    lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, sEIP);
    lineCursor = LogPut(lineCursor, " lin=0x"); lineCursor = LogHex(lineCursor, DpmiSelectorBase(savedCs) + sEIP);
    /* - WHAT THE CALLER ACTUALLY PASSED. A DOS call that takes a pointer takes it in
     * DS:(E)DX, and when one fails the FIRST question is whether the caller's pointer
     * was good -- i.e. whether the fault is the client's or ours. Print the selector,
     * the full 32-bit EDX (a flat client's offset does not fit in a word) and the
     * bytes at the resulting linear address.
     */
    { DWORD dsValue = VDM_REG16(tib, VTIB_DS);
      DWORD edx = VDM_REG(tib, VTIB_EDX);
      DWORD offset = DpmiSelectorIs32((WORD)dsValue) ? edx : (edx & WORD_MASK);
      DWORD linear = DpmiSelectorBase((WORD)dsValue) + offset;
      const BYTE *stackBytes = (const BYTE *)(ULONG_PTR)linear;
      lineCursor = LogPut(lineCursor, " DS:EDX=0x"); lineCursor = LogHex(lineCursor, dsValue); lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, edx);
      lineCursor = LogPut(lineCursor, " lin=0x"); lineCursor = LogHex(lineCursor, linear); lineCursor = LogPut(lineCursor, " @=");
      if (!HostReadable(stackBytes, 16))
          lineCursor = LogPut(lineCursor, "<unreadable>");
      else
          lineCursor = LogDump(lineCursor, stackBytes, 16);
      /* - AND THE CALLER'S STACK WIDTH, because that is what the client's dispatcher
       * ASKS. DOS/4GW's common handler (mod:0x550) begins `LAR eax,SS` + `bt eax,22`
       * -- it reads the D/B bit of the interrupted SS descriptor to decide whether the
       * caller was 16- or 32-bit, and therefore whether a pointer argument is a word
       * or a dword. If that bit is wrong, the extender truncates a flat pointer to its
       * low 16 bits, which is exactly the failure being chased here.
       */
      { WORD ssValue = (WORD)VDM_REG(tib, VTIB_SS);
        lineCursor = LogPut(lineCursor, " SS=0x"); lineCursor = LogHex(lineCursor, ssValue);
        lineCursor = LogPut(lineCursor, DpmiSelectorIs32(ssValue) ? " (SS D/B=1)" : " (SS D/B=0)");
        lineCursor = LogPut(lineCursor, " ESP=0x"); lineCursor = LogHex(lineCursor, VDM_REG(tib, VTIB_ESP));
        lineCursor = LogPut(lineCursor, " CS=0x"); lineCursor = LogHex(lineCursor, (WORD)VDM_REG(tib, VTIB_CS));
        lineCursor = LogPut(lineCursor, DpmiSelectorIs32((WORD)VDM_REG(tib, VTIB_CS)) ? " (CS D/B=1)" : " (CS D/B=0)");
        lineCursor = LogPut(lineCursor, " h32=");
        lineCursor = LogHex(lineCursor, (DWORD)isClient32); } }
    lineCursor = LogPut(lineCursor, "\r\n"); LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor); lineCursor = lineBuffer;
    /* Say so on the last always-loud one, not silently on the first suppressed one. */
    if (isFirstDispatch && g_PmDispatchLogged[dispatchVector][dispatchAh] >= PM_DISP_LOG_MAX)
    {
        lineCursor = LogPut(lineCursor, "PM INT 0x"); lineCursor = LogHex(lineCursor, vector);
        lineCursor = LogPut(lineCursor, " AH=0x"); lineCursor = LogHexByte(lineCursor, (BYTE)dispatchAh);
        lineCursor = LogPut(lineCursor, ": reflected-dispatch trace RATE-LIMITED from here (this vector/AH"
                      " pair only, one line per 100 ms; every one is still counted --"
                      " total at STAGE2)\r\n");
        LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor); lineCursor = lineBuffer;
    }
quietEntry:

    g_PmDispatch[vector] = 1;
    for (phase = 0; phase < DPMI_DISPATCH_PHASE_MAX && !done; ++phase)
    {
        DWORD event;
        DWORD eip;
        DWORD vectorNumber;
        INT status;
        /* - CHECKPOINT INSIDE THE HANDLER TOO. The main loop's DPMI-CP lines stop at the
         * moment we hand control to the client's handler, so without this a death in
         * there is exactly the blind stretch this session spent hours removing -- one
         * log line, then nothing. Bounded to the first 64 entries per dispatch so a
         * handler that loops pays nothing.
         */
        if (phase < 64 && g_DpmiCpMaximum > 8)
        {
            DWORD callerCs = VDM_REG16(tib, VTIB_CS);
            DWORD codeBase  = DpmiSelectorBase((WORD)callerCs);
            DWORD callerIp = VDM_REG(tib, VTIB_EIP);
            const BYTE *codeBytes = (const BYTE *)(ULONG_PTR)(codeBase + callerIp);
            lineCursor = LogPut(lineCursor, "  PMH["); lineCursor = LogHex(lineCursor, phase);
            lineCursor = LogPut(lineCursor, "] cs:eip=0x"); lineCursor = LogHex(lineCursor, callerCs);
            lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, callerIp);
            lineCursor = LogPut(lineCursor, " ss:esp=0x"); lineCursor = LogHex(lineCursor, VDM_REG16(tib, VTIB_SS));
            lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, VDM_REG(tib, VTIB_ESP));
            lineCursor = LogPut(lineCursor, " EAX=0x"); lineCursor = LogHex(lineCursor, VDM_REG(tib, VTIB_EAX));
            lineCursor = LogPut(lineCursor, " DS=0x"); lineCursor = LogHex(lineCursor, VDM_REG16(tib, VTIB_DS));
            lineCursor = LogPut(lineCursor, " ES=0x"); lineCursor = LogHex(lineCursor, VDM_REG16(tib, VTIB_ES));
            lineCursor = LogPut(lineCursor, " b=");
            if (!HostReadable(codeBytes, 16))
                lineCursor = LogPut(lineCursor, "<unreadable>");
            else
                lineCursor = LogDump(lineCursor, codeBytes, 16);
            lineCursor = LogPut(lineCursor, "\r\n");
            LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor); lineCursor = lineBuffer;
        }
        DpmiArmFaultTrampoline(tib, 0);
        DpmiEnterProtectedMode(tib);
        event  = VDM_REG(tib, VTIB_EVENT);
        eip = DpmiPmEip(tib);
        if (event == VDM_EVENT_BOP && eip == DPMI_PMRET_OFF
            && VDM_REG16(tib, VTIB_CS) == g_PmReturnSelector)
        {
            done = 1;
            break;
        }
        if (event == 3)
            continue;                                  /* "interrupt pending" -> retry */
        if (event == VDM_EVENT_IO || event == VDM_EVENT_IO_HW || event == VDM_EVENT_GPFAULT)
        {
            INT ioHandled;
            HOST_LOCK();
            ioHandled = HostTryIoPm(tib, &g_Bus);
            HOST_UNLOCK();
            if (ioHandled)
                continue;
        }
        /* -- A DPMI FAULT SITE IS NOT AN INTERRUPT, AND THIS LOOP MUST NOT EAT IT.
         * This is the SECOND protected-mode run loop -- it exists to run the client's
         * own INT handler to completion -- and it never had an arm for the case where
         * THAT HANDLER FAULTS. The kernel reflects such a fault onto our fault-site
         * stubs exactly as it does in the main loop, but here the BOP fell through to
         * DpmiServicePmInt(), which read the site's `C4 C4 57` bytes and handed them
         * to the WOW dispatcher -- where 0x57 is the WOW callback id, a genuine
         * collision with DPMI_FAULT_BOP. The verdict was "UNIMPLEMENTED, STEPPED OVER",
         * so the exception was never delivered, the guest re-executed the same site,
         * and it looped until the dispatch budget ran out ("handler NO-RET").
         * - MEASURED, ZAR (GH #23): cs=0x017f == g_DpmiFaultCodeSelector, eip=0x694 ==
         *   DPMI_FAULT_SITE(13) -- i.e. a #GP raised inside DOS/16M's own INT 21h
         *   handler while ZAR asked for the DOS version (AX=0x3067). It was swallowed
         *   here, which is why ZAR printed nothing: the error text it was composing
         *   ("\VMD.", "program must be built -AUTO for DPMI") never reached the screen.
         *
         * Hand it back to the MAIN loop, which owns the whole exception-delivery
         * path (frame width, handler lookup, the FLTRET catcher). Returning 1 leaves
         * the guest parked ON the fault site with the INT frame still on its stack, so
         * the outer loop's gate fires on the next entry and delivers it properly; the
         * resume-past-the-INT epilogue below is deliberately NOT run, because the INT
         * has not finished -- its handler is mid-fault.
         *
         * [CAUTION]: THIS CANNOT AFFECT WOW. A real WOW `C4 C4 57` trampoline lives in a 16-bit
         * code selector of the guest's, never in g_DpmiFaultCodeSelector, so the CS test
         * excludes it by construction.
         */
        if (event == VDM_EVENT_BOP
            && VDM_REG16(tib, VTIB_CS) == (g_DpmiFaultCodeSelector & WORD_MASK)
            && (eip == DPMI_FAULT_COFF
                || (eip >= DPMI_FAULT_SITE(0)
                    && eip <  DPMI_FAULT_SITE(DOS_FLTSITE_N)
                    && ((eip - DPMI_FAULT_SITE(0)) & (DOS_FLTSITE_SIZE - 1)) == 0)))
        {
            lineCursor = LogPut(lineCursor, "  PM INT 0x"); lineCursor = LogHex(lineCursor, vector);
            lineCursor = LogPut(lineCursor, " handler FAULTED at fault-site eip=0x"); lineCursor = LogHex(lineCursor, eip);
            lineCursor = LogPut(lineCursor, " -- returning to the main loop so the exception is DELIVERED"
                          " (this loop has no fault arm of its own)\r\n");
            LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor); lineCursor = lineBuffer;
            g_PmDispatch[vector] = 0;
            return 1;
        }
        vectorNumber = (event == VDM_EVENT_BOP) ? DpmiBopVector(VDM_REG16(tib, VTIB_CS), eip) : 0;
        status = DpmiServicePmInt(machine, tib, vectorNumber, steps);
        if (status > 0)
            continue;
        g_PmDispatch[vector] = 0;
        return status;                                  /* 0 = client exited, -1 = stop */
    }
    g_PmDispatch[vector] = 0;

    /* Resume the client past its INT. CS/SS and the stack pointer go back to what they
     * were; the GPRs and EFLAGS are the handler's answer and are left alone.
     */
    VDM_SET16(tib, VTIB_CS, savedCs);
    VDM_REG(tib, VTIB_EIP) = sEIP + X86_INT_LENGTH;               /* past the 2-byte patched INT */
    VDM_SET16(tib, VTIB_SS, savedSs);
    VDM_REG(tib, VTIB_ESP) = sESP;
    /* [CAUTION]: PAIRED WITH THE ENTRY LINE BY `loud`, NOT RE-TESTED. Re-testing the counter here
     * would print an exit with no entry (the entry line consumed the last of the
     * budget), and an unmatched "<- handler IRET" is exactly the shape that reads as a
     * re-entrancy bug. A NO-RET is a different matter and is never suppressed: it means
     * the handler did not come back, which is a fault report, not a trace line.
     */
    if (loud || !done)
    {
        lineCursor = LogPut(lineCursor, "PM INT 0x"); lineCursor = LogHex(lineCursor, vector);
        lineCursor = LogPut(lineCursor, done ? " <- handler IRET, AX=0x" : " <- handler NO-RET, AX=0x");
        lineCursor = LogHex(lineCursor, VDM_REG16(tib, VTIB_EAX));
        lineCursor = LogPut(lineCursor, " CF="); lineCursor = LogHex(lineCursor, VDM_REG(tib, VTIB_EFLAGS) & EFLAGS_CF_U);
        lineCursor = LogPut(lineCursor, "\r\n"); LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor);
    }
    return 1;
}

/* WHICH ADDRESS IS THE RMCS? PRINT BOTH; DO NOT PICK ONE AND HOPE:
 * The real-mode call structure is passed in ES:(E)DI, and the "(E)" is the whole
 * question: a 16-bit client passes DI, a 32-bit one passes the full EDI. Every arm
 * that takes an RMCS here has always masked it to 16 bits.
 * What made this worth asking: Doom calls 0300 with BL=33h 2915 times in a 45 s
 * headless run, and the AX we read out of the RMCS is 0xffff EVERY TIME -- a value
 * I_ReadMouse never writes, and precisely the value OUR OWN INT 33h reset returned
 * into that structure on the first call. Reading back exactly what we last wrote is
 * the signature of a location that is nobody's but ours.
 *
 * [CAUTION]: THIS FUNCTION CHANGES NOTHING. It reports the masked and unmasked candidates side
 * by side with the EAX field at each, for the first few calls of each vector. If the
 * unmasked address holds 3 or 0x0b -- I_ReadMouse's two functions -- the mask is the
 * bug and the fix is one expression. If it does not, the mask is innocent and the
 * next guess would have cost a session. The same probe rides on 0301/0302 because
 * they share the structure and DOS/4GW's INT 21h forwarding depends on them: if
 * those show a 32-bit EDI too, they are already wrong and only look right.
 */
/* ES:(E)DI, AND THE (E) IS NOT DECORATION:
 * THE MEASUREMENT (headless Doom, 45 s), from DpmiRmcsProbe below:
 *     RMCS 0300 int=0x33 es=0x18f edi=0x03dc9158 esb=0x0 cl32=1
 *          masked@0x00009158 eax=0x4e800000     <- junk in the guest's low memory
 *          full  @0x03dc9158 eax=0x00000003     <- I_ReadMouse, "read buttons"
 *     ...and the next call, full@ eax=0x0000000b <- "read counters"
 * So Doom WAS asking, 2915 times in 45 seconds, and it was never the "unimplemented
 * function" or the "mis-patched CD 33" this was filed as. The offset is a full
 * 32-bit EDI, every arm here masked it to 16 bits, and the address that produced
 * was somebody else's memory: we read a function number out of it (0xffff, then
 * whatever we had written there LAST call -- reading back your own answer is what
 * a self-owned scratch address looks like) and we wrote our results into it, i.e.
 * we were also corrupting two words of the guest's low memory 2915 times.
 * - THE RULE IS THE CALLER'S D/B BIT, NOT A CLIENT-WIDE FLAG. DPMI passes the
 *   structure in ES:DI from 16-bit code and ES:EDI from 32-bit code, because that is
 *   simply what `mov di,x` versus `mov edi,x` leaves behind -- a 16-bit caller's top
 *   half is stale, not zero. DpmiSelectorIs32(CS) asks exactly that question, per call.
 *
 * [CAUTION]: AND THAT IS WHY THIS IS SAFE FOR THE EXTENDER. DOS/4GW's own 0301/0302 traffic
 * comes from 16-bit code, where this reduces to the mask that is already there --
 * measured, not assumed: its RMCS pointers log as es=0x1f edi=0x00004b54, masked
 * and full identical. Only a 32-bit caller changes, and for a 32-bit caller the
 * old behaviour was never right.
 */
/* The same rule for ANY offset register a client hands us: (E)SI, (E)DI, (E)DX are
 * full-width from 32-bit code, 16 bits from 16-bit code. Duke3D (s74c) is the
 * guest that made this a rule rather than an RMCS special case: it calls 0500
 * from flat code with the 30h-byte block on its 32-bit stack (ES:EDI =
 * 0x2f7:0x045d53xx), and the masked address put our answer at linear 0x53xx --
 * the guest read an uninitialised block, concluded "You don't have enough memory
 * to run Duke Nukem 3D", and exited 0.
 */
DWORD DpmiCallerOffset(volatile BYTE *tib, DWORD offset)
{
    return DpmiSelectorIs32((WORD)VDM_REG16(tib, VTIB_CS)) ? offset : (offset & WORD_MASK);
}

DWORD DpmiRmcsPointer(volatile BYTE *tib, DWORD esBase)
{
    return esBase + DpmiCallerOffset(tib, VDM_REG(tib, VTIB_EDI));
}

/* The RMCS register file into / out of the TIB (GH #247; layout and rules in dpmi_rmcs.h).
 * FLAGS is deliberately NOT moved by RmcsToTib: each caller decides what the live
 * EFLAGS are (V86 entry state for the nested call, a carrier word for the host-side
 * fast path), and passes the word to report back to TibToRmcs.
 */
VOID RmcsToTib(volatile BYTE *tib, const RMCS_REGS *registers)
{
    VDM_REG(tib, VTIB_EDI) = registers->Edi;
    VDM_REG(tib, VTIB_ESI) = registers->Esi;
    VDM_REG(tib, VTIB_EBP) = registers->Ebp;
    VDM_REG(tib, VTIB_EBX) = registers->Ebx;
    VDM_REG(tib, VTIB_EDX) = registers->Edx;
    VDM_REG(tib, VTIB_ECX) = registers->Ecx;
    VDM_REG(tib, VTIB_EAX) = registers->Eax;
    VDM_REG(tib, VTIB_ES) = registers->Es;
    VDM_REG(tib, VTIB_DS) = registers->Ds;
    VDM_REG(tib, VTIB_FS) = registers->Fs;
    VDM_REG(tib, VTIB_GS) = registers->Gs;
}

VOID TibToRmcs(volatile BYTE *tib, RMCS_REGS *registers, WORD flags)
{
    registers->Edi = VDM_REG(tib, VTIB_EDI);
    registers->Esi = VDM_REG(tib, VTIB_ESI);
    registers->Ebp = VDM_REG(tib, VTIB_EBP);
    registers->Ebx = VDM_REG(tib, VTIB_EBX);
    registers->Edx = VDM_REG(tib, VTIB_EDX);
    registers->Ecx = VDM_REG(tib, VTIB_ECX);
    registers->Eax = VDM_REG(tib, VTIB_EAX);
    registers->Flags = flags;
    registers->Es = (WORD)VDM_REG(tib, VTIB_ES);
    registers->Ds = (WORD)VDM_REG(tib, VTIB_DS);
    registers->Fs = (WORD)VDM_REG(tib, VTIB_FS);
    registers->Gs = (WORD)VDM_REG(tib, VTIB_GS);
}

VOID DpmiRmcsProbe(volatile BYTE *tib, DWORD esBase, UINT slot, DWORD interruptNumber)
{
    static BYTE seen[5][256];
    DWORD edi = VDM_REG(tib, VTIB_EDI);
    DWORD es = VDM_REG16(tib, VTIB_ES);
    DWORD maskedAddress  = esBase + (edi & WORD_MASK);
    DWORD fullAddress = esBase + edi;
    CHAR buffer[512];
    CHAR *cursor = buffer;
    static PCSTR const tags[5] = { "0300", "0301", "0302", "000b", "000c" };
    if (slot > 4 || interruptNumber > 255)
        return;
    if (seen[slot][interruptNumber] >= 3)
        return;
    ++seen[slot][interruptNumber];
    cursor = LogPut(cursor, "RMCS ");     cursor = LogPut(cursor, tags[slot]);
    cursor = LogPut(cursor, " int=0x");   cursor = LogHexByte(cursor, interruptNumber);
    cursor = LogPut(cursor, " es=0x");    cursor = LogHex(cursor, es);
    cursor = LogPut(cursor, " edi=0x");   cursor = LogHex(cursor, edi);
    cursor = LogPut(cursor, " esb=0x");   cursor = LogHex(cursor, esBase);
    cursor = LogPut(cursor, " cl32=");    cursor = LogHex(cursor, (DWORD)g_DpmiIsClient32);
    /* The RMCS fields that name the CALL: EAX (+0x1C) is the function number, and
     * EBX/ECX/EDX (+0x10/+0x18/+0x14) are where an INT 33h answer goes back.
     */
    cursor = LogPut(cursor, " masked@0x"); cursor = LogHex(cursor, maskedAddress);
    if (MemoryReadable((ULONG_PTR)maskedAddress, 0x20))
    {
        volatile DWORD *masked = (volatile DWORD *)(ULONG_PTR)maskedAddress;
        cursor = LogPut(cursor, " eax=0x"); cursor = LogHex(cursor, masked[0x1C/4]);
        cursor = LogPut(cursor, " ebx=0x"); cursor = LogHex(cursor, masked[0x10/4]);
        cursor = LogPut(cursor, " d0=0x");  cursor = LogHex(cursor, masked[0]);
    }
    else
        cursor = LogPut(cursor, " unreadable");
    cursor = LogPut(cursor, " || full@0x"); cursor = LogHex(cursor, fullAddress);
    if (MemoryReadable((ULONG_PTR)fullAddress, 0x20))
    {
        volatile DWORD *full = (volatile DWORD *)(ULONG_PTR)fullAddress;
        cursor = LogPut(cursor, " eax=0x"); cursor = LogHex(cursor, full[0x1C/4]);
        cursor = LogPut(cursor, " ebx=0x"); cursor = LogHex(cursor, full[0x10/4]);
        cursor = LogPut(cursor, " d0=0x");  cursor = LogHex(cursor, full[0]);
    }
    else
        cursor = LogPut(cursor, " unreadable");
    cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, buffer, cursor);
}

/* #248: the index of a live 0501h handle in g_DpmiOwned[], or -1. */
INT DpmiOwnedFind(DWORD handle)
{
    INT index;

    if (!handle)
        return -1;
    for (index = 0; index < g_DpmiOwnedCount; ++index)
        if (g_DpmiOwned[index] == handle)
            return index;
    return -1;
}

/* #248: give LDT index `idx` back -- null descriptor installed, index on the free list.
 * One body for 0001h and 0101h (0101h used to zero the base and keep the index for ever).
 */
VOID DpmiLdtRelease(INT index)
{
    g_Ldt[index].Base = g_Ldt[index].Limit = 0;
    g_Ldt[index].Access = 0;                          /* not present */
    g_Ldt[index].Flags = 0;
    DpmiInstall(index);
    if (g_LdtFreeCount < DPMI_LDT_MAX)
        g_LdtFree[g_LdtFreeCount++] = (WORD)index;
}

/* ...and take one: the free list first, as 0000h does for a single descriptor, then the
 * high-water mark. -1 = the table is full. Without the free list, 0101h giving indices
 * back would not stop a 0100h/0101h loop from running the table dry.
 */
INT DpmiLdtTake(VOID)
{
    if (g_LdtFreeCount > 0)
        return g_LdtFree[--g_LdtFreeCount];
    if (g_LdtNext >= DPMI_LDT_MAX)
        return -1;
    return g_LdtNext++;
}

/* #248: may the client name `sel` in 0001h/0007h-000Ah/0101h? The rule and the krnl386
 * exception (under WOW the guest owns the table, so only the range is ours to check) are
 * in dpmi_svc.h; this binds it to our table. "Allocated" = a non-zero access byte, which
 * is what 0001h's free (access = 0) and 0000h's allocate (0xF2) maintain.
 */
INT DpmiClientSelectorOk(WORD selector)
{
    INT index = DPMI_SELECTOR_INDEX(selector);
    INT alloc = (index >= 1 && index < DPMI_LDT_MAX) && g_Ldt[index].Access != 0;

    return DpmiIsSelectorValid(selector, DPMI_LDT_MAX, alloc, g_WowShadow != NULL);
}

/* THE PM->V86 TRANSFER BUFFER FOR POINTER-TAKING INT 21h CALLS. (GH #128):
 * dos_int21.c resolves a guest pointer as `(DS << 4) + DX`. That is exactly right
 * for a V86 guest and completely wrong for a protected-mode one, where DS is a
 * SELECTOR -- so every INT 21h function that takes a pointer was excluded from the
 * PM path and answered "PM thunk TODO". For DOS/4GW that was harmless, because the
 * extender services its own DOS calls internally. krnl386 is the first guest to
 * chain them to us, and it needs the whole file API.
 *
 * So: a block of conventional memory the V86 DOS layer can address normally, into
 * which the caller's arguments are copied before the call and out of which its
 * results are copied after. This is the same shape as DPMI's own translation
 * buffer, and for the same reason.
 *
 * [CAUTION]: ALLOCATED ONLY ON THE WOW PATH (see WowPlaceV86), and every arm below is gated
 * on it being present. A DOS or DOS/4GW run therefore takes byte-identical paths to
 * before -- which matters, because a fix measured on one guest is a fix for none and
 * this one is measured on krnl386.
 */
/* Copy [len] bytes out of a PM guest pointer sel:off into the transfer buffer. */
static INT PmTransferIn(WORD selector, DWORD offset, DWORD length)
{
    DWORD base = DpmiSelectorBase(selector);
    const volatile BYTE *source = (const volatile BYTE *)(ULONG_PTR)(base + offset);
    volatile BYTE *destination = (volatile BYTE *)(ULONG_PTR)((DWORD)g_PmTransferSegment << PARAGRAPH_SHIFT);
    DWORD index;

    if (!base || length > (DWORD)g_PmTransferParagraphs * PARAGRAPH_SIZE_U)
        return -1;
    if (!HostReadable((const VOID *)source, length))
        return -1;
    for (index = 0; index < length; ++index)
        destination[index] = source[index];
    return 0;
}

static INT PmTransferOut(WORD selector, DWORD offset, DWORD length)
{
    DWORD base = DpmiSelectorBase(selector);
    volatile BYTE *destination = (volatile BYTE *)(ULONG_PTR)(base + offset);
    const volatile BYTE *source = (const volatile BYTE *)(ULONG_PTR)((DWORD)g_PmTransferSegment << PARAGRAPH_SHIFT);
    DWORD index;

    if (!base || length > (DWORD)g_PmTransferParagraphs * PARAGRAPH_SIZE_U)
        return -1;
    if (!HostReadable((const VOID *)destination, length))
        return -1;
    for (index = 0; index < length; ++index)
        destination[index] = source[index];
    return 0;
}

/* How long is the NUL-terminated string at sel:off? Bounded, and it never reads past
 * what the descriptor covers -- a filename we cannot read is a failure to report, not
 * a reason to walk off the end of a segment.
 */
static DWORD PmTransferStringLength(WORD selector, DWORD offset, DWORD cap)
{
    DWORD base = DpmiSelectorBase(selector);
    DWORD length = 0;
    const volatile BYTE *source = (const volatile BYTE *)(ULONG_PTR)(base + offset);

    if (!base)
        return 0;
    while (length < cap && HostReadable((const VOID *)(source + length), 1) && source[length])
        ++length;
    return length + 1;                                     /* include the NUL */
}

/* Service one pointer-taking INT 21h call made from protected mode. Returns the log
 * cursor. EIP is advanced by the caller, as for every other arm.
 */
PSTR PmInt21Transfer(DOS_MACHINE *machine, volatile BYTE *tib, DWORD ah, PSTR cursor)
{
#define m   (*machine)
    WORD  dsValue = (WORD)VDM_REG16(tib, VTIB_DS);
    DWORD dxValue = VDM_REG16(tib, VTIB_EDX);
    DWORD cxValue = VDM_REG16(tib, VTIB_ECX);
    DWORD savedDs = VDM_REG(tib, VTIB_DS);
    DWORD savedDx = VDM_REG(tib, VTIB_EDX);
    DWORD cap = (DWORD)g_PmTransferParagraphs * PARAGRAPH_SIZE_U;
    INT status = 0;

    cursor = LogPut(cursor, "INT21h AH=0x"); cursor = LogHex(cursor, ah);
    cursor = LogPut(cursor, " (PM->V86 via xfer buf 0x"); cursor = LogHex(cursor, g_PmTransferSegment);
    cursor = LogPut(cursor, ") ds:dx=0x"); cursor = LogHex(cursor, dsValue); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, dxValue);

    switch (ah)
    {
    /* A FILENAME IN. 3Dh open, 41h delete, 43h get/set attributes, 4Eh find-first,
     * 39h/3Ah/3Bh mkdir/rmdir/chdir -- all DS:DX -> ASCIIZ path.
     */
    case DOS_FN_OPEN:
    case DOS_FN_DELETE:
    case DOS_FN_FILE_ATTRIBUTES:
    case DOS_FN_FIND_FIRST:
    case DOS_FN_MKDIR:
    case DOS_FN_RMDIR:
    case DOS_FN_CHDIR:
    {
        DWORD length = PmTransferStringLength(dsValue, dxValue, cap < MAX_PATH ? cap - 1 : MAX_PATH - 1);
        status = PmTransferIn(dsValue, dxValue, length);
        if (!status)
        {
            PCSTR name = (PCSTR)(ULONG_PTR)((DWORD)g_PmTransferSegment << PARAGRAPH_SHIFT);
            cursor = LogPut(cursor, " name=\""); cursor = LogPut(cursor, name); cursor = LogPut(cursor, "\"");
        }
        break; }

    /* A BUFFER OUT. 3Fh read CX bytes into DS:DX -- nothing to copy in. */
    case DOS_FN_READ:
        if (cxValue > cap)
        {
            /* Say so rather than silently short-reading: a caller that asked for
             * 0x4000 and got 0x1000 with CF=0 would believe it had the whole file.
             */
            cursor = LogPut(cursor, " READ 0x"); cursor = LogHex(cursor, cxValue);
            cursor = LogPut(cursor, " EXCEEDS xfer buffer 0x"); cursor = LogHex(cursor, cap);
            cursor = LogPut(cursor, " -- CLAMPED (see wow_place_v86)");
            cxValue = cap;
            VDM_SET16(tib, VTIB_ECX, (WORD)cxValue);
        }
        break;

    /* A BUFFER IN. 40h write CX bytes from DS:DX. */
    case DOS_FN_WRITE:
        if (cxValue > cap)
        {
            cxValue = cap;
            VDM_SET16(tib, VTIB_ECX, (WORD)cxValue);
        }
        status = PmTransferIn(dsValue, dxValue, cxValue);
        break;

    default:
        status = -1;
        break;
    }

    if (status)
    {
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;              /* CF=1 */
        VDM_SET16(tib, VTIB_EAX, DOS_ERR_ACCESS_DENIED);             /* access denied */
        cursor = LogPut(cursor, " -> XFER FAILED (unreadable pointer or oversize) CF=1\r\n");
        return cursor;
    }

    /* Point the DOS layer at the buffer, in the terms it understands. */
    VDM_SET16(tib, VTIB_DS, g_PmTransferSegment);
    VDM_SET16(tib, VTIB_EDX, 0);
    m.TraceCursor = cursor;
    DosInt21SetProtectedMode(TRUE);
    DosInt21(&m);
    DosInt21SetProtectedMode(FALSE);
    cursor = m.TraceCursor;
    VDM_REG(tib, VTIB_DS) = savedDs;
    VDM_REG(tib, VTIB_EDX) = savedDx;

    /* Results back out. A short read is what AX says, not what CX asked for. */
    if (ah == DOS_FN_READ && !(VDM_REG(tib, VTIB_EFLAGS) & EFLAGS_CF_U))
    {
        DWORD got = VDM_REG16(tib, VTIB_EAX);
        if (got > cxValue)
            got = cxValue;
        if (got)
            PmTransferOut(dsValue, dxValue, got);
        cursor = LogPut(cursor, " read=0x"); cursor = LogHex(cursor, got);
    }
    cursor = LogPut(cursor, " -> AX=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EAX));
    cursor = LogPut(cursor, " CX=0x");    cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ECX));
    cursor = LogPut(cursor, " CF=");      cursor = LogHex(cursor, VDM_REG(tib, VTIB_EFLAGS) & EFLAGS_CF_U);
    cursor = LogPut(cursor, "\r\n");
    return cursor;
#undef m
}

#define DPMI_REFLECT_STACK_TOP  0xFB00  /* DpmiReflectIrqToRm: the real-mode stack it lends the ISR */

/* #210: THE LONG-FILENAME API (INT 21h AH=71h) FROM PROTECTED MODE:
 * Same bridge as PmInt21Transfer, but an LFN call can carry THREE pointers at once (7156h:
 * DS:DX and ES:DI; 714Eh: DS:DX in, ES:DI out) and uses SI/DI/DX as plain numbers in
 * others (714Eh SI = time format, 7143h DI = a date, 716Ch DX = the action word). So
 * each pointer register the call actually uses gets its own window in the transfer
 * buffer, the V86 arm in dos_int21.c runs against those, and the outputs are copied
 * back:
 *     +0000h  DS:DX   (in: a path; out: 71A6h's 52-byte record, 71AAh BH=2's path)
 *     +0400h  DS:SI   (in: a path, 71A7h BL=0's FILETIME; out: 7147h's directory)
 *     +0800h  ES:DI   (in: 7156h's new name; out: a find record, a path, a name, a
 *                      FILETIME, 71A0h's file-system name)
 * Offsets follow the caller's D/B bit (DpmiCallerOffset), so a flat 32-bit client's
 * EDX/ESI/EDI work as its 16-bit cousins' DX/SI/DI do.
 *
 * [CAUTION]: WOW-ONLY, like every pointer-taking PM INT 21h here: the transfer buffer exists only
 * on the WOW path (see WowPlaceV86). A DPMI client without one is told AX=7100h CF=1
 * by the caller -- "no LFN API", which is what this host told everyone before #210 and
 * the answer every LFN client is written to fall back from. DOS/4GW-style extenders
 * that pass 71xxh down in REAL mode reach the V86 arm directly and are not affected.
 *
 * [CAUTION]: krnl386 is the PM client this serves, and nothing measured shows it issuing 71xxh;
 * this is the API made reachable, not a fix for an observed call.
 */
static INT PmLfnCopy(WORD selector, DWORD offset, DWORD transferOffset, DWORD length, INT isIn)
{
    DWORD base = DpmiSelectorBase(selector);
    DWORD index;
    volatile BYTE *guest = (volatile BYTE *)(ULONG_PTR)(base + offset);
    volatile BYTE *transfer = (volatile BYTE *)(ULONG_PTR)(((DWORD)g_PmTransferSegment << PARAGRAPH_SHIFT) + transferOffset);

    if (!base || transferOffset + length > (DWORD)g_PmTransferParagraphs * PARAGRAPH_SIZE_U || length > PM_TRANSFER_WINDOW_SIZE)
        return -1;
    if (!length)
        return 0;
    if (!HostReadable((const VOID *)guest, length))
        return -1;
    if (isIn)
        for (index = 0; index < length; ++index)
            transfer[index] = guest[index];
    else
        for (index = 0; index < length; ++index)
            guest[index] = transfer[index];
    return 0;
}

/* How many bytes of an output window go back: all `len` of a block (kind 2), or a
 * string's length + its NUL, never more than `len` (kind 4).
 */
static DWORD PmLfnOutLength(DWORD transferOffset, INT kind, DWORD length)
{
    const volatile BYTE *transfer = (const volatile BYTE *)(ULONG_PTR)(((DWORD)g_PmTransferSegment << PARAGRAPH_SHIFT) + transferOffset);
    DWORD used = 0;

    if (kind != PM_LFN_COPY_STRING_OUT)
        return length;
    while (used < length && transfer[used])
        ++used;
    return (used < length) ? used + 1 : length;
}

PSTR PmInt21Lfn(DOS_MACHINE *machine, volatile BYTE *tib, PSTR cursor)
{
#define m   (*machine)
    DWORD al = VDM_REG(tib, VTIB_EAX) & BYTE_MASK;
    DWORD bl = VDM_REG(tib, VTIB_EBX) & BYTE_MASK;
    DWORD bh = (VDM_REG(tib, VTIB_EBX) >> BYTE_SHIFT) & BYTE_MASK;
    WORD dsValue = (WORD)VDM_REG16(tib, VTIB_DS);
    WORD esValue = (WORD)VDM_REG16(tib, VTIB_ES);
    DWORD dxOffset = DpmiCallerOffset(tib, VDM_REG(tib, VTIB_EDX));
    DWORD siOffset = DpmiCallerOffset(tib, VDM_REG(tib, VTIB_ESI));
    DWORD diOffset = DpmiCallerOffset(tib, VDM_REG(tib, VTIB_EDI));
    DWORD savedDs = VDM_REG(tib, VTIB_DS);
    DWORD savedEs = VDM_REG(tib, VTIB_ES);
    DWORD savedDx = VDM_REG(tib, VTIB_EDX);
    DWORD savedSi = VDM_REG(tib, VTIB_ESI);
    DWORD savedDi = VDM_REG(tib, VTIB_EDI);
    /* Which registers are pointers for this call, and which way their bytes go:
     * 0 = not a pointer, 1 = a string in, 2 = a block out of `len`, 3 = a block in,
     * 4 = a string out of at most `len` -- copied back only up to its NUL, so a caller's
     *     buffer shorter than RBIL's 261 bytes is not overwritten past the answer.
     */
    INT copyDx = 0;
    INT copySi = 0;
    INT copyDi = 0;
    INT status = 0;
    DWORD dxLength = 0;
    DWORD siLength = 0;
    DWORD diLength = 0;

    switch (al)
    {
    case DOS_FN_MKDIR:
    case DOS_FN_RMDIR:
    case DOS_FN_CHDIR:
    case DOS_FN_DELETE:
    case DOS_FN_FILE_ATTRIBUTES:
    case DOS_FN_FIND_FIRST:
    case DOS_INT21_LFN_VOLUME_INFO:
        copyDx = PM_LFN_COPY_STRING_IN;
        break;

    case DOS_FN_RENAME:
        copyDx = PM_LFN_COPY_STRING_IN;
    copyDi = PM_LFN_COPY_STRING_IN;
    break;

    case DOS_FN_EXTENDED_OPEN:
    case DOS_INT21_LFN_SERVER_OPEN:
    case DOS_FN_TRUENAME:
    case DOS_INT21_LFN_SHORT_NAME:
        copySi = PM_LFN_COPY_STRING_IN;
    break;

    case DOS_FN_GET_CURRENT_DIRECTORY:
        copySi = PM_LFN_COPY_STRING_OUT;
    siLength = DOS_LFN_PATH_BUFFER_SIZE;
    break;

    case DOS_INT21_LFN_HANDLE_INFO:
        copyDx = PM_LFN_COPY_BLOCK_OUT;
    dxLength = DOS_INT21_HANDLE_INFO_SIZE;
    break;

    case DOS_INT21_LFN_TIME_CONVERT:
        if (bl == DOS_INT21_TIME_TO_DOS)
    {
        copySi = PM_LFN_COPY_BLOCK_IN;
        siLength = DOS_LFN_FILETIME_SIZE;
    }
    else
    {
        copyDi = PM_LFN_COPY_BLOCK_OUT;
        diLength = DOS_LFN_FILETIME_SIZE;
    } break;

    case DOS_INT21_LFN_SUBST:
        if (bh == DOS_INT21_SUBST_CREATE)
            copyDx = PM_LFN_COPY_STRING_IN;
    else if (bh == DOS_INT21_SUBST_QUERY)
    {
        copyDx = PM_LFN_COPY_STRING_OUT;
        dxLength = DOS_LFN_PATH_BUFFER_SIZE;
    } break;

    default:
        break;
    }
    if (al == DOS_FN_FIND_FIRST || al == DOS_FN_FIND_NEXT)
    {
        copyDi = PM_LFN_COPY_BLOCK_OUT;
        diLength = DOS_LFN_FIND_RECORD_SIZE;
    }
    if (al == DOS_FN_TRUENAME)
    {
        copyDi = PM_LFN_COPY_STRING_OUT;
        diLength = DOS_LFN_PATH_BUFFER_SIZE;
    }
    if (al == DOS_INT21_LFN_SHORT_NAME) { if (((VDM_REG(tib, VTIB_EDX) >> BYTE_SHIFT) & BYTE_MASK) == 0)
    {
        copyDi = PM_LFN_COPY_BLOCK_OUT;
        diLength = DOS_FCB_NAME_SIZE;
    }
                      else
                      {
                          copyDi = PM_LFN_COPY_STRING_OUT;
                          diLength = DOS_SHORT_NAME_SIZE;
                      }
                      }
    if (al == DOS_INT21_LFN_VOLUME_INFO)
    {
        copyDi = PM_LFN_COPY_STRING_OUT;
        diLength = VDM_REG16(tib, VTIB_ECX);
        if (diLength > PM_TRANSFER_WINDOW_SIZE)
            diLength = PM_TRANSFER_WINDOW_SIZE;
    }

    cursor = LogPut(cursor, "INT21h AX=71"); cursor = LogHexByte(cursor, (BYTE)al);
    cursor = LogPut(cursor, " (PM LFN -> V86 via xfer buf 0x"); cursor = LogHex(cursor, g_PmTransferSegment); cursor = LogPut(cursor, ")");

    /* In. A string's length is found first (bounded, never past what is readable). */
    if (copyDx == PM_LFN_COPY_STRING_IN)
    {
        dxLength = PmTransferStringLength(dsValue, dxOffset, PM_TRANSFER_STRING_MAX);
        status |= PmLfnCopy(dsValue, dxOffset, PM_TRANSFER_WINDOW_DX, dxLength, PM_LFN_INTO_TRANSFER);
    }
    if (copySi == PM_LFN_COPY_STRING_IN)
    {
        siLength = PmTransferStringLength(dsValue, siOffset, PM_TRANSFER_STRING_MAX);
        status |= PmLfnCopy(dsValue, siOffset, PM_TRANSFER_WINDOW_SI, siLength, PM_LFN_INTO_TRANSFER);
    }
    if (copyDi == PM_LFN_COPY_STRING_IN)
    {
        diLength = PmTransferStringLength(esValue, diOffset, PM_TRANSFER_STRING_MAX);
        status |= PmLfnCopy(esValue, diOffset, PM_TRANSFER_WINDOW_DI, diLength, PM_LFN_INTO_TRANSFER);
    }
    if (copySi == PM_LFN_COPY_BLOCK_IN)
        status |= PmLfnCopy(dsValue, siOffset, PM_TRANSFER_WINDOW_SI, siLength, PM_LFN_INTO_TRANSFER);
    if (status)
    {
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
        VDM_SET16(tib, VTIB_EAX, DOS_ERR_ACCESS_DENIED);
        cursor = LogPut(cursor, " -> XFER FAILED (unreadable pointer) CF=1\r\n");
        return cursor;
    }
    if (copyDx == PM_LFN_COPY_STRING_IN || copySi == PM_LFN_COPY_STRING_IN || copyDi == PM_LFN_COPY_STRING_IN)
    {
        PCSTR name = (PCSTR)(ULONG_PTR)(((DWORD)g_PmTransferSegment << PARAGRAPH_SHIFT)
                                                   + (copyDx == PM_LFN_COPY_STRING_IN ? PM_TRANSFER_WINDOW_DX : copySi == PM_LFN_COPY_STRING_IN ? PM_TRANSFER_WINDOW_SI : PM_TRANSFER_WINDOW_DI));
        cursor = LogPut(cursor, " name=\""); cursor = LogPut(cursor, name); cursor = LogPut(cursor, "\"");
    }

    VDM_SET16(tib, VTIB_DS, g_PmTransferSegment);
    VDM_SET16(tib, VTIB_ES, g_PmTransferSegment);
    if (copyDx)
        VDM_SET16(tib, VTIB_EDX, PM_TRANSFER_WINDOW_DX);
    if (copySi)
        VDM_SET16(tib, VTIB_ESI, PM_TRANSFER_WINDOW_SI);
    if (copyDi)
        VDM_SET16(tib, VTIB_EDI, PM_TRANSFER_WINDOW_DI);
    m.TraceCursor = cursor;
    DosInt21SetProtectedMode(TRUE);
    DosInt21(&m);
    DosInt21SetProtectedMode(FALSE);
    cursor = m.TraceCursor;
    /* Restore what we re-pointed -- except a register the call ANSWERS in: 71A0h returns
     * the maximum path in DX, 7143h BL=2 the size's high word.
     */
    VDM_REG(tib, VTIB_DS) = savedDs;
    VDM_REG(tib, VTIB_ES) = savedEs;
    if (copyDx && !(al == DOS_INT21_LFN_VOLUME_INFO) && !(al == DOS_FN_FILE_ATTRIBUTES && bl == DOS_INT21_LFN_ATTR_GET_COMPRESSED_SIZE))
        VDM_REG(tib, VTIB_EDX) = savedDx;
    if (copySi)
        VDM_REG(tib, VTIB_ESI) = savedSi;
    if (copyDi)
        VDM_REG(tib, VTIB_EDI) = savedDi;

    if (!(VDM_REG(tib, VTIB_EFLAGS) & EFLAGS_CF_U))
    {
        if (copyDx == PM_LFN_COPY_BLOCK_OUT || copyDx == PM_LFN_COPY_STRING_OUT)
            PmLfnCopy(dsValue, dxOffset, PM_TRANSFER_WINDOW_DX, PmLfnOutLength(PM_TRANSFER_WINDOW_DX, copyDx, dxLength), PM_LFN_BACK_TO_GUEST);
        if (copySi == PM_LFN_COPY_BLOCK_OUT || copySi == PM_LFN_COPY_STRING_OUT)
            PmLfnCopy(dsValue, siOffset, PM_TRANSFER_WINDOW_SI, PmLfnOutLength(PM_TRANSFER_WINDOW_SI, copySi, siLength), PM_LFN_BACK_TO_GUEST);
        if (copyDi == PM_LFN_COPY_BLOCK_OUT || copyDi == PM_LFN_COPY_STRING_OUT)
            PmLfnCopy(esValue, diOffset, PM_TRANSFER_WINDOW_DI, PmLfnOutLength(PM_TRANSFER_WINDOW_DI, copyDi, diLength), PM_LFN_BACK_TO_GUEST);
    }
    cursor = LogPut(cursor, " -> AX=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EAX));
    cursor = LogPut(cursor, " CF=");      cursor = LogHex(cursor, VDM_REG(tib, VTIB_EFLAGS) & EFLAGS_CF_U);
    cursor = LogPut(cursor, "\r\n");
    return cursor;
#undef m
}

INT DpmiReflectIrqToRm(DOS_MACHINE *machine, volatile BYTE *tib, UINT vector)
{
    CHAR lineBuffer[256];
    CHAR *lineCursor = lineBuffer;
    DWORD savedEax=VDM_REG(tib,VTIB_EAX);
    DWORD savedEbx=VDM_REG(tib,VTIB_EBX);
    DWORD savedEcx=VDM_REG(tib,VTIB_ECX);
    DWORD savedEdx=VDM_REG(tib,VTIB_EDX);
    DWORD savedEsi=VDM_REG(tib,VTIB_ESI);
    DWORD savedEdi=VDM_REG(tib,VTIB_EDI);
    DWORD pmBp=VDM_REG(tib,VTIB_EBP);
    DWORD pmDs=VDM_REG(tib,VTIB_DS);
    DWORD pmEs=VDM_REG(tib,VTIB_ES);
    DWORD pmFs=VDM_REG(tib,VTIB_FS);
    DWORD pmGs=VDM_REG(tib,VTIB_GS);
    DWORD pmCs=VDM_REG(tib,VTIB_CS);
    DWORD pmIp=VDM_REG(tib,VTIB_EIP);
    DWORD pmSs=VDM_REG(tib,VTIB_SS);
    DWORD pmSp=VDM_REG(tib,VTIB_ESP);
    DWORD pFlags=VDM_REG(tib,VTIB_EFLAGS);
    WORD machineStatusWord = *(volatile WORD *)(tib + VTIB_MSW);
    WORD realSs = (WORD)(g_DpmiCodeBase >> PARAGRAPH_SHIFT);
    WORD realSp = DPMI_REFLECT_STACK_TOP;
    WORD realCs = PeekWord(IVT_SEGMENT_ADDRESS(vector));
    WORD rip = PeekWord(IVT_OFFSET_ADDRESS(vector));
    UINT round;
    INT done = 0;

    InterlockedExchange(&g_SimIntBusy, 1);
    realSp -= X86_WORD_SIZE;
    PokeWord(((DWORD)realSs << PARAGRAPH_SHIFT) + realSp, EFLAGS_IF | EFLAGS_RESERVED_ONE);          /* FLAGS: IF set, restored by IRET */
    realSp -= X86_WORD_SIZE;
    PokeWord(((DWORD)realSs << PARAGRAPH_SHIFT) + realSp, DOS_HDLR_SEG);
    realSp -= X86_WORD_SIZE;
    PokeWord(((DWORD)realSs << PARAGRAPH_SHIFT) + realSp, DPMI_RMRET_OFF);
    DpmiUnpatch();
    *(volatile WORD *)(tib + VTIB_MSW) = (WORD)(machineStatusWord & ~MSW_PE_BIT);
    VDM_REG(tib, VTIB_EFLAGS) = EFLAGS_VM | EFLAGS_RESERVED_ONE;                        /* VM, interrupts OFF */
    VDM_SET16(tib, VTIB_DS, realSs);
    VDM_SET16(tib, VTIB_ES, realSs);
    VDM_SET16(tib, VTIB_FS, realSs);
    VDM_SET16(tib, VTIB_GS, realSs);
    VDM_SET16(tib, VTIB_CS, realCs);
    VDM_REG(tib, VTIB_EIP) = rip;
    VDM_SET16(tib, VTIB_SS, realSs);
    VDM_REG(tib, VTIB_ESP) = realSp;
    for (round = 0; round < NESTED_V86_ROUNDS_MAX && !done; ++round)
    {
        LONG runStatus;
        DWORD rev;
        DWORD info;
        InterlockedExchange(&g_NestedRm, 1);
        InterlockedExchange(&g_InExec, 1);                     /* see g_NestedRm */
        rev = VdmRunGuest(tib, &runStatus);
        InterlockedExchange(&g_InExec, 0);
        InterlockedExchange(&g_NestedRm, 0);
        info = VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK;
        if (rev == VDM_EVENT_BOP && info == DPMI_RMRET_BOP)
        {
            done = 1;
            break;
        }
        if (rev == VDM_EVENT_BOP && info == DOS_BOP_INT21)               /* INT 21h from the ISR */
        {
            CHAR dropBuffer[2048];
            machine->TraceCursor = dropBuffer;
            DosInt21(machine);        /* its log text is dropped */
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
            continue;
        }
        if (rev == VDM_EVENT_BOP && info == DPMI_CB_BOP)        /* the ISR far-called a 0303 callback */
        {
            INT callbackSlot = DpmiCallbackSlotAt(DPMI_CB_BASE_OFF, (WORD)VDM_REG(tib,VTIB_CS), DOS_HDLR_SEG,
                                         (WORD)VDM_REG(tib,VTIB_EIP));
            if (callbackSlot >= 0 && g_Callbacks[callbackSlot].IsUsed)
            {
                DpmiInvokeCallback(machine, tib, callbackSlot);
                continue;
            }
        }
        if (rev == VDM_EVENT_IO || rev == VDM_EVENT_IO_HW || rev == VDM_EVENT_GPFAULT)
        {
            INT handled;
            HOST_LOCK();
            handled = HostTryIo(tib, &g_Bus);
            HOST_UNLOCK();
            if (handled)
                continue;
        }
        if (g_PmIrqRmFail < 16)
        {
            lineCursor = LogPut(lineCursor, "PM IRQ reflect vec=0x"); lineCursor = LogHexByte(lineCursor, (BYTE)vector);
            lineCursor = LogPut(lineCursor, " -> RM ISR: unexpected event=0x"); lineCursor = LogHex(lineCursor, rev);
            lineCursor = LogPut(lineCursor, " info=0x"); lineCursor = LogHex(lineCursor, info);
            lineCursor = LogPut(lineCursor, " CS:IP=0x"); lineCursor = LogHex(lineCursor, VDM_REG16(tib,VTIB_CS));
            lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, VDM_REG16(tib,VTIB_EIP));
            lineCursor = LogPut(lineCursor, "\r\n"); LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor); lineCursor = lineBuffer;
        }
        break;
    }
    DpmiRepatch();
    *(volatile WORD *)(tib + VTIB_MSW) = machineStatusWord;
    VDM_REG(tib,VTIB_EAX)=savedEax;
    VDM_REG(tib,VTIB_EBX)=savedEbx;
    VDM_REG(tib,VTIB_ECX)=savedEcx;
    VDM_REG(tib,VTIB_EDX)=savedEdx;
    VDM_REG(tib,VTIB_ESI)=savedEsi;
    VDM_REG(tib,VTIB_EDI)=savedEdi;
    VDM_REG(tib,VTIB_EBP)=pmBp;
    VDM_SET16(tib,VTIB_DS,pmDs);
    VDM_SET16(tib,VTIB_ES,pmEs);
    VDM_SET16(tib,VTIB_FS,pmFs);
    VDM_SET16(tib,VTIB_GS,pmGs);
    VDM_SET16(tib,VTIB_CS,pmCs);
    VDM_REG(tib,VTIB_EIP)=pmIp;
    VDM_SET16(tib,VTIB_SS,pmSs);
    VDM_REG(tib,VTIB_ESP)=pmSp;
    VDM_REG(tib,VTIB_EFLAGS)=pFlags;
    InterlockedExchange(&g_SimIntBusy, 0);
    if (done)
        ++g_PmIrqRmReflects;
    else
        ++g_PmIrqRmFail;
    return done;
}

/* -- THE HANDLER'S ANSWER IS ON THE STACK, NOT IN EFLAGS. (GH #128, session 41)
 * Every DOS service above reports failure the way DOS does -- `CF` -- and writes it
 * into the guest's LIVE EFLAGS before stepping EIP past the BOP. For a client that
 * reached us through a PATCHED INT SITE in its own code, or through the `#GP` on a
 * raw `INT nn`, that is the only channel there is and it is right: no interrupt
 * frame was ever built, so the instruction after the call reads the flags register.
 *
 * It is WRONG for the other way in, and that way is the whole of WOW. Our default
 * protected-mode handler for all 256 vectors is THREE BYTES -- `C4 C4 CF`: the BOP,
 * and then an **IRET**. krnl386 hooks INT 21h in protected mode, asks its 32-bit
 * companion first, and when the companion declines it chains to the vector it saved,
 * which is that stub -- arriving with an interrupt-style frame on the stack (the
 * standard way to chain to a saved handler: flags pushed, then a far call).
 *
 * So the very next instruction the guest executes after we advance past the BOP --
 * the stub's IRET -- throws our CF away and restores the flags image in the frame. A real INT
 * 21h handler does not have
 * that problem, because on real hardware it answers by writing the flags image the
 * caller pushed -- which is exactly what the V86 arm of this host already does
 * (`*pfl |= 1`, at SS:SP+4) and what the protected-mode arm never did.
 *
 * [INFO]: MEASURED, and the measurement is an A/B inside one run. A breakpoint on the
 * point in krnl386's `_lread` where a set CF becomes `AX = -1` (HFILE_ERROR),
 * across SYSEDIT loading its four files:
 *     SYSTEM.INI   0x0e7 bytes   efl=0x00010206   CF=0
 *     WIN.INI      0x1dd bytes   efl=0x00010206   CF=0
 *     CONFIG.SYS   0x000 bytes   efl=0x00010207   CF=1   -> AX=0xffff
 *     AUTOEXEC.BAT 0x000 bytes   efl=0x00010207   CF=1   -> AX=0xffff
 * Our AH=3Fh answered `AX=0 CF=0` for all four. The two zeroes are the ones whose CF
 * never reached the guest and a STALE one was tested instead: on a non-zero read,
 * other work inside `_lread` happened to clear that stale CF first (observed: only
 * the zero-length reads came back -1).
 *
 * a defect that was ALWAYS here needed an empty file to expose it, and the stock
 * oracle -- SYSEDIT under stock ntvdm opening all four with no message box -- is what
 * ruled out "the application is right".
 *
 * [CAUTION]: CF ONLY, AND THAT IS NOT TIMIDITY. CF is the flag these services compute; ZF, SF,
 * OF, AF and PF in live EFLAGS at this moment are the GUEST's, left over from
 * whatever krnl386 executed on its way to the BOP. Copying those into the frame
 * would not be returning an answer, it would be inventing one -- and it would do it
 * silently, in the one place a wrong bit cannot be seen.
 *
 * [CAUTION]: Keyed on the guest STANDING IN THE STUB with the IRET as its next instruction, not
 * on the vector or on how we were entered. Both facts are exact and read out of the
 * live machine: a service that parked a different context (a wowcall.h callback, an
 * EXEC) is no longer in the stub and is correctly skipped.
 *
 * [CAUTION]: The IRET's width follows the STUB's D bit (DpmiSyncDefaultSelectorWidth makes it track
 * the client), and the stack's address width follows SS's -- the same "frame width
 * and descriptor width are the same question" this host has paid for three times.
 */

static VOID DpmiPmCarryToFrame(volatile BYTE *tib)
{
    WORD  cs = (WORD)VDM_REG16(tib, VTIB_CS);
    WORD  ss;
    DWORD base;
    DWORD eip;
    DWORD sp;
    DWORD linear;
    DWORD image;
    DWORD now;
    const volatile BYTE *instruction;
    volatile BYTE *flagsBytes;

    if (!g_PmDefaultSelector || cs != g_PmDefaultSelector)
        return;                                                         /* not in our stub at all */
    base = DpmiSelectorBase(cs);
    if (!base)
        return;
    eip = VDM_REG(tib, VTIB_EIP);
    instruction  = (const volatile BYTE *)(ULONG_PTR)(base + eip);
    if (!MemoryReadable((ULONG_PTR)instruction, 1) || *instruction != X86_OP_IRET)
        return;                                                                              /* not about to IRET */

    ss  = (WORD)VDM_REG16(tib, VTIB_SS);
    sp  = DpmiSelectorIs32(ss) ? VDM_REG(tib, VTIB_ESP) : VDM_REG16(tib, VTIB_ESP);
    linear = DpmiSelectorBase(ss) + sp + (DpmiSelectorIs32(cs) ? X86_FRAME32_FLAGS : X86_FRAME16_FLAGS);
    if (!MemoryReadable((ULONG_PTR)linear, 2))
        return;
    flagsBytes  = (volatile BYTE *)(ULONG_PTR)linear;

    image = (DWORD)flagsBytes[0] | ((DWORD)flagsBytes[1] << BYTE_SHIFT);
    now = (image & ~EFLAGS_CF_U) | (VDM_REG(tib, VTIB_EFLAGS) & EFLAGS_CF_U);
    if (now == image)
        return;                                  /* the frame already says what we do */
    flagsBytes[0] = (BYTE)(now & BYTE_MASK);
    flagsBytes[1] = (BYTE)((now >> BYTE_SHIFT) & BYTE_MASK);
    {   CHAR lineBuffer[192], *cursor = lineBuffer;
        cursor = LogPut(cursor, "PM CF -> IRET frame at 0x"); cursor = LogHex(cursor, linear);
        cursor = LogPut(cursor, ": 0x"); cursor = LogHex(cursor, image); cursor = LogPut(cursor, " -> 0x"); cursor = LogHex(cursor, now);
        cursor = LogPut(cursor, " (the stub's IRET would have restored the caller's CF)\r\n");
        LogAppend(LOG_PATH, lineBuffer, cursor);
        SerialOut(lineBuffer, cursor); }
}

/* The service, and then the one thing it cannot say in a register. Kept as a wrapper
 * rather than repeated at the ~90 `return 1` sites above, because a rule enforced in
 * one place is a rule and a rule repeated ninety times is a lottery.
 */
INT DpmiServicePmInt(DOS_MACHINE *machine, volatile BYTE *tib, DWORD vector, UINT steps)
{
    INT status;

    /* #256: only THIS dispatch is top-level; anything it nests (a callback, an
     * injected ISR) dispatches with the flag down.
     */
    g_PmDispatchTop = g_PmTopDispatch;
    g_PmTopDispatch = 0;
    status = DpmiServicePmIntBody(machine, tib, vector, steps);
    if (status == 1)
        DpmiPmCarryToFrame(tib);
    return status;
}

/* Lazily install the PM-return catcher selector (g_PmReturnSelector): a code selector based
 * at DOS_HDLR_SEG so a PM handler's IRET lands on the planted DPMI_PMRET BOP. Shared by
 * the 0303 real-mode-callback path and the async-IRQ injector (#2b).
 */
VOID DpmiEnsurePmReturnSelector(VOID)
{
    if (g_PmReturnSelector == 0 && g_LdtNext < DPMI_LDT_MAX)
    {
        INT index = g_LdtNext++;
        g_Ldt[index].Base = (DWORD)DOS_HDLR_SEG << PARAGRAPH_SHIFT;
        g_Ldt[index].Limit = X86_SEGMENT_LIMIT_64K;
        g_Ldt[index].Access = DPMI_ACCESS_CODE;                         /* code exec/read */
        /* THE CATCHER'S D/B BIT IS PART OF THE CALLER'S IDENTITY:
         * We push this selector as the RETURN CS of the interrupt frame the client's
         * handler runs on, so that its IRET lands back on our BOP. But a DOS extender
         * READS that return CS: it is how the handler learns whether the code it
         * interrupted was 16- or 32-bit, and therefore whether a pointer argument in
         * (E)DX is a word or a dword. Leaving it 16-bit told DOS/4GW that every caller
         * was 16-bit, and it TRUNCATED the application's flat pointers to their low
         * word -- measured, on Doom's open of default.cfg:
         *   app passed   DS:EDX = 0x18f:0x03b69b80  -> "default.cfg"
         *   RMCS got     DS:DX  = 0x000:0x9b80      -> garbage, open failed with
         *                                              "file not found"
         * and its 16-bit stack frame was ALREADY correct (SS D/B=1), which is what
         * made this hard to see: the frame width and the caller's advertised width are
         * two different questions, and only one of them was being answered.
         * Follow g_DpmiIsClient32, exactly as the frame width does (h32). A 16-bit
         * client is unaffected: flags stay 0 and every existing test keeps its
         * 6-byte frame and 16-bit catcher.
         */
        g_Ldt[index].Flags = g_DpmiIsClient32 ? DPMI_DESCRIPTOR_FLAG_BIG : 0;   /* 0x4 = D/B */
        DpmiInstall(index);
        g_PmReturnSelector = (WORD)DPMI_LDT_SELECTOR(index);
    }
}

/* GH #18 #2b: asynchronously inject a hardware interrupt (IRQ0 -> INT `iv`) into the
 * client's INSTALLED protected-mode vector (g_PmInt[iv], set via INT 31h 0205). This is
 * the mechanism timer-hooking games (e.g. Doom) rely on: they hook INT 08h/1Ch and expect
 * it to fire ~18.2x/s. We snapshot the interrupted PM context, build an IRET frame on the
 * client's own PM stack pointing at the PM-return catcher, vector to the client's handler,
 * run it through the SAME dispatcher the main loop uses (so a handler that itself does INT
 * 21h/31h or port I/O still works), and on its IRET restore the interrupted context verbatim.
 * Faithful to a real INT: clears the virtual-IF (g_DpmiVi) for the duration, restores it
 * after. Returns 1 if the handler ran to its IRET, 0 otherwise.
 */
/* Build a protected-mode interrupt frame ON THE SUSPENDED THREAD'S OWN CONTEXT and vector
 * it at the client's handler. Runs on the TIMER thread with the CPU thread suspended, so
 * it may touch guest memory and the context but must not log, allocate an LDT entry, or
 * block. Returns 1 if `cx` was rewritten and should be committed.
 * - THE RETURN PATH IS THE CATCHER, exactly as for the synchronous injector: the frame's
 *   return CS:EIP is g_PmReturnSelector:DPMI_PMRET_OFF, so the handler's IRET lands on a BOP and
 *   the main loop gets control back. What the main loop CANNOT recover from the IRET is
 *   where the guest actually was -- that return address was overwritten with the catcher's
 *   -- so the interrupted CS:EIP:SS:ESP:EFLAGS are saved here and restored there.
 * - ONE IN FLIGHT AT A TIME, claimed with an interlocked compare-exchange, because this
 *   runs on a different thread from the one that clears it.
 */
INT DpmiAsyncInjectPm(UINT irq, CONTEXT *context)
{
    if (g_PmClientExited)
        return 0;                            /* nothing left to interrupt */
    UINT interruptVector = IrqPmVector(irq);               /* 08h-0Fh, or 70h-77h for the slave */
    DWORD eflags = context->EFlags;
    WORD  ss;
    if (irq >= PIC_LINES)
    {
        g_AsyncWhy = ASYNC_WHY_BAD_VECTOR;
        return 0;
    }
    if (g_PmNoIrq || g_InPmIrq) /* knob off, or a sync injection is running */
    {
        g_AsyncWhy = g_InPmIrq ? ASYNC_WHY_IN_PM_IRQ : ASYNC_WHY_PM_NO_IRQ;
        return 0;
    }
    if (g_PmReturnSelector == 0) /* no catcher yet -> no way back */
    {
        g_AsyncWhy = ASYNC_WHY_NO_CATCHER;
        return 0;
    }
    if (!g_PmInt[interruptVector].Client) /* the client has not hooked this line */
    {
        g_AsyncWhy = ASYNC_WHY_UNHOOKED_PM;
        return 0;
    }
    /* Not before the application has an ISR; see DpmiInjectPmIrq(). */
    if (g_DpmiIsClient32 && interruptVector == VECTOR_TIMER && !g_PmAppHookedTimer)
    {
        /* BUT THE BIOS TICK STILL HAS TO ADVANCE. (s74c, Duke3D's SETUP):
         * A flat application that never hooks INT 08h still reads 0040:006C: Watcom's
         * delay()/clock() spin on it. The PM loop bumps it "polled", i.e. only when
         * the guest traps back in -- and a tick-spin never traps, so SETMAIN sat at
         * 0x042d16f1 with time stopped until the watchdog killed it (1,665 refusals
         * here, why=6). We are the timer: with the CPU thread suspended in guest code
         * this is the one place that can do the BIOS's bookkeeping while it spins.
         * Billed against the owed-tick count so time is never manufactured, and the
         * pending flag is consumed so the polled path does not count it again.
         */
        if (PmTickTake())
        {
            /* the one BIOS tick body, witness included (#262 -- vdd_pit.h) */
            VddPitBiosTick(&g_Pit, (volatile UINT32 *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_TICK_COUNT),
                          (volatile BYTE *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_MIDNIGHT_FLAG));
            if (g_Irq0Pending > 0)
                InterlockedDecrement(&g_Irq0Pending);
        }
        g_AsyncWhy = ASYNC_WHY_NO_APP_TIMER;
        return 0;
    }
    if (!g_DpmiVi) /* the client has interrupts masked */
    {
        g_AsyncWhy = ASYNC_WHY_VIF_OFF;
        return 0;
    }
    if (!(eflags & (EFLAGS_IF_U | EFLAGS_VIF))) /* ...and the CPU agrees */
    {
        g_AsyncWhy = ASYNC_WHY_IF_OFF;
        return 0;
    }
    /* Same hold-off the cooperative path uses: a vector installed microseconds ago is an
     * arming pass, and real IRQ0 could not have arrived yet. See INT 31h 0205.
     */
    if ((GetTickCount() - g_PmVector8ArmedMs) < DPMI_IRQ0_ARM_QUIET_MS)
    {
        g_AsyncWhy = ASYNC_WHY_ARM_QUIET;
        return 0;
    }
    if (InterlockedCompareExchange(&g_AsyncPmActive, 1, 0) != 0)
    {
        g_AsyncWhy = ASYNC_WHY_IN_FLIGHT;
        return 0;
    }

    ss = (WORD)(context->SegSs & WORD_MASK);
    if (!(ss & DPMI_SELECTOR_TI)) /* not a client stack -> not safe */
    {
        g_AsyncPmActive = 0;
        g_AsyncWhy = ASYNC_WHY_HOST_STACK;
        return 0;
    }
    /* Same rule as the cooperative path: interrupt the APPLICATION, never the extender
     * mid-service. See DpmiInjectPmIrq() for what that cost to learn.
     */
    if (g_DpmiIsClient32 && !DpmiSelectorIs32((WORD)(context->SegCs & WORD_MASK)))
    {
        g_AsyncPmActive = 0;
        g_AsyncWhy = ASYNC_WHY_NOT_32;
        return 0;
    }

    /* Save what we are interrupting; the catcher BOP is where it gets put back. */
    g_AsyncPmCs  = (WORD)(context->SegCs & WORD_MASK);
    g_AsyncPmEip = context->Eip;
    g_AsyncPmSs  = ss;
    g_AsyncPmEsp = context->Esp;
    g_AsyncPmEflags = eflags;

    /* Frame width is the CLIENT's mode; stack addressing is the SS descriptor's B bit.
     * Two different questions -- see DpmiDispatchToPmHandler() for what conflating
     * them costs.
     */
    { DWORD stackBase = DpmiSelectorBase(ss);
      INT ss32 = DpmiSelectorIs32(ss);
      INT isClient32 = g_DpmiIsClient32;
      DWORD sp = ss32 ? context->Esp : (context->Esp & WORD_MASK);
      if (isClient32)
      {
          sp = ss32 ? sp - X86_DWORD_SIZE : ((sp - X86_DWORD_SIZE) & WORD_MASK);
          PokeDword(stackBase + sp, eflags);
          sp = ss32 ? sp - X86_DWORD_SIZE : ((sp - X86_DWORD_SIZE) & WORD_MASK);
          PokeDword(stackBase + sp, g_PmReturnSelector);
          sp = ss32 ? sp - X86_DWORD_SIZE : ((sp - X86_DWORD_SIZE) & WORD_MASK);
          PokeDword(stackBase + sp, DPMI_PMRET_OFF);
      }
      else
      {
          sp = ss32 ? sp - X86_WORD_SIZE : ((sp - X86_WORD_SIZE) & WORD_MASK);
          PokeWord(stackBase + sp, (WORD)eflags);
          sp = ss32 ? sp - X86_WORD_SIZE : ((sp - X86_WORD_SIZE) & WORD_MASK);
          PokeWord(stackBase + sp, g_PmReturnSelector);
          sp = ss32 ? sp - X86_WORD_SIZE : ((sp - X86_WORD_SIZE) & WORD_MASK);
          PokeWord(stackBase + sp, DPMI_PMRET_OFF);
      }
      context->Esp = ss32 ? sp : ((context->Esp & HIGH_WORD_MASK_U) | sp); }

    context->SegCs  = DPMI_IRQ_TARGET_SEL(interruptVector);
    context->Eip    = g_DpmiIsClient32 ? DPMI_IRQ_TARGET_OFF(interruptVector) : (DPMI_IRQ_TARGET_OFF(interruptVector) & WORD_MASK);
    /* CLEAR **VIP**, NOT JUST VIF -- OR THE GUEST'S NEXT `STI` FAULTS:
     * The guest runs at CPL 3 with PVI, which is why CLI/STI are survivable there at
     * all (measured: both SURVIVE, while INT3 and HLT kill the VDM). Under PVI, `STI`
     * sets VIF -- but if **VIP (bit 20) is already set it raises #GP instead**, by
     * design, so that the OS can deliver the interrupt it had pending. And a raw
     * protected-mode #GP is the one fault XP will not reflect: run 71 watched it with
     * a kernel debugger attached and the kernel just tears the VDM down, silently, no
     * bugcheck and no break.
     * That is exactly this failure. We deliver the tick OURSELVES, behind the
     * kernel's back, so the kernel's own pending IRQ0 stays queued: it sets VIP and
     * defers (see the EFLAGS_VIP note in ntvdm.h). Doom's timer ISR executes
     * `sti` at obj1+0x1356d on EVERY tick, so the first tick that lands with VIP set
     * dies there -- which is why it survived a handful of entries and then vanished
     * with no fault, no watchdog line, and nothing after the log's last byte.
     * We ARE the delivery, so the interrupt is no longer pending: clear VIP with VIF,
     * and clear the kernel's own pending bits too, exactly as the event-3 guard in the
     * main loop already does for the stale-pending case.
     */
    context->EFlags = eflags & ~(EFLAGS_IF_U | EFLAGS_VIF | EFLAGS_VIP);
    *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR &= ~VDM_INT_PENDING;      /* the kernel's pending-IRQ bits */
    g_DpmiVi  = 0;                                  /* ...and our model of it */
    return 1;
}

enum
{
    DPMI_IRQ_TIME_CHECK_MASK = 0x3F
};   /* DpmiInjectPmIrq: look at the clock every 64 phases */
/* A FAULT INSIDE A NESTED RUN IS STILL A FAULT. (s90, found tracing #278) (Importance = 2):
 * The main PM loop owns exception delivery: the kernel reflects a fault onto one
 * of our fault sites (`C4 C4 57` in g_DpmiFaultCodeSelector), and the main loop hands
 * it to the client's registered handler with our FLTRET BOP as the return
 * address. The NESTED runs -- WowCall16SyncEx (WM_INITDIALOG, WM_DRAWITEM,
 * WM_CTLCOLOR sent from inside a Win32 call) and the PM IRQ/mouse-callback
 * injectors -- had no such arm: they passed the site's BOP to the service
 * routine, where 0x57 is ALSO the WOW callback id, so it was logged
 * "UNIMPLEMENTED, STEPPED OVER", never delivered, and the guest re-faulted on the
 * same instruction until the loop's budget ran out.
 * - MEASURED, Sound Recorder (runs/s90/sr1_host.log): MMSYSTEM's init far-calls
 *   its segment 8 before it is loaded -- #NP, error code 0x0C44, at 0C77:041E --
 *   inside a nested run. krnl386's #NP handler is what LOADS the segment; it was
 *   never called, and the BOP was "stepped over" 309,601 times.
 *
 * This is the main loop's 16-bit path, reduced to what a nested run needs:
 * deliver a reflected fault to a REGISTERED handler, and resume from the
 * (possibly rewritten) frame at FLTRET. A 32-bit client, a class nobody
 * registered, and a software INT reflected as #GP (IDT bit) are left exactly as
 * before -- the main loop's long arm handles those, and this does not guess.
 */
INT DpmiNestedFault(volatile BYTE *tib, DWORD event, DWORD eip)
{
    DWORD csValue = VDM_REG16(tib, VTIB_CS);
    DWORD stackBase;
    DWORD esp;
    volatile WORD *frame;
    CHAR lineBuffer[256];
    CHAR *lineCursor = lineBuffer;

    /* [CAUTION]: WOW ONLY. The evidence is a WOW run; DOS clients (ZAR's DOS/16M, Doom's
     * DOS/4GW) go through the IRQ injector too and keep their old behaviour until
     * a DOS case is measured.
     */
    if (!g_WowLaunch)
        return 0;
    if (event != VDM_EVENT_BOP || csValue != (g_DpmiFaultCodeSelector & WORD_MASK) || g_DpmiIsClient32)
        return 0;
    stackBase  = DpmiSelectorBase(g_DpmiFaultSelector);
    esp = VDM_REG16(tib, VTIB_ESP);
    frame  = (volatile WORD *)(ULONG_PTR)(stackBase + esp);
    if (!stackBase || !HostReadable((const VOID *)frame, DPMI_FRAME16_SIZE))
        return 0;
    if (eip == DPMI_FLTRET_COFF)
    {
        /* the handler's RETF popped the two return words: SP is on the error code */
        VDM_SET16(tib, VTIB_SS, frame[DPMI_RETURNED_SS]);
        VDM_REG(tib, VTIB_ESP) = frame[DPMI_RETURNED_SP];
        VDM_SET16(tib, VTIB_CS, frame[DPMI_RETURNED_CS]);
        VDM_REG(tib, VTIB_EIP) = frame[DPMI_RETURNED_IP];
        VDM_SET16(tib, VTIB_EFLAGS, frame[DPMI_RETURNED_FLAGS]);
        lineCursor = LogPut(lineCursor, "NESTED EXC RETURN -> resume 0x"); lineCursor = LogHex(lineCursor, frame[DPMI_RETURNED_CS]);
        lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, frame[DPMI_RETURNED_IP]); lineCursor = LogPut(lineCursor, "\r\n");
        LogAppend(LOG_PATH, lineBuffer, lineCursor);
        return 1;
    }
    if (eip >= DPMI_FAULT_SITE(0) && eip < DPMI_FAULT_SITE(DOS_FLTSITE_N)
        && ((eip - DPMI_FAULT_SITE(0)) & (DOS_FLTSITE_SIZE - 1)) == 0)
    {
        INT exception = (INT)((eip - DPMI_FAULT_SITE(0)) / DOS_FLTSITE_SIZE);
        /* -- s92: A RAW `INT nn` INSIDE A NESTED RUN. A #GP through an IDT gate (error
         * code bit 1, vector in bits 3..15) is an interrupt nobody intercepted -- the
         * main loop's long arm services those (search "A #GP THROUGH AN IDT GATE").
         * Here it fell to the service routine, which ran the INT and then resumed the
         * guest ON THE FAULT SITE, whose `C4 C4 57` the WOW dispatcher stepped over
         * (0x57 is also a WOW id) -- and the #GP at the next byte went to krnl386's
         * handler: "Application Error", the task killed. MEASURED, Media Player's
         * WM_INITDIALOG (sent through the nested run): `INT 21h AX=5700h` at
         * 0B77:0D07, err 0x010A (runs/s92/drv/w16drive_s92mpo_host.log).
         *
         * The main loop's arm, reduced to the case measured: a 16-bit, BASED code and
         * stack selector, the bytes really `CD vec`, not the FP-emulator range. The
         * guest goes back ON the INT, the site is patched to our BOP with its vector in
         * the map -- as the main loop does -- and the loop's ordinary BOP path services
         * it on the next turn. Everything else is declined, as before.
         */
        if (exception == X86_EXCEPTION_GP && (frame[DPMI_FRAME_ERROR] & X86_ERROR_CODE_IDT))
        {
            DWORD gateVector = (DPMI_SELECTOR_INDEX((DWORD)frame[DPMI_FRAME_ERROR])) & BYTE_MASK;
            DWORD guestCodeBase  = DpmiSelectorBase(frame[DPMI_FRAME_CS]);
            volatile BYTE *guestInstruction = (volatile BYTE *)(ULONG_PTR)(guestCodeBase + frame[DPMI_FRAME_IP]);
            if (!guestCodeBase || DpmiSelectorIs32(frame[DPMI_FRAME_CS]) || DpmiSelectorIs32(frame[DPMI_FRAME_SS])
                || (gateVector >= VECTOR_FLOATING_POINT_FIRST && gateVector <= VECTOR_FLOATING_POINT_LAST)
                || !HostReadable((const VOID *)guestInstruction, X86_INT_LENGTH) || guestInstruction[0] != X86_OP_INT || guestInstruction[1] != (BYTE)gateVector
                || !HostWritable((VOID *)guestInstruction, X86_INT_LENGTH))
                return 0;
            VDM_SET16(tib, VTIB_SS, frame[DPMI_FRAME_SS]);
            VDM_REG(tib, VTIB_ESP) = frame[DPMI_FRAME_SP];
            VDM_SET16(tib, VTIB_CS, frame[DPMI_FRAME_CS]);
            VDM_REG(tib, VTIB_EIP) = frame[DPMI_FRAME_IP];
            VDM_SET16(tib, VTIB_EFLAGS, frame[DPMI_FRAME_FLAGS]);
            guestInstruction[0] = VDM_BOP0;
            guestInstruction[1] = VDM_BOP1;
            PatchMapSet(guestCodeBase + frame[DPMI_FRAME_IP], (BYTE)gateVector);
            lineCursor = LogPut(lineCursor, "NESTED #GP(IDT): a raw INT 0x"); lineCursor = LogHex(lineCursor, gateVector);
            lineCursor = LogPut(lineCursor, " at 0x"); lineCursor = LogHex(lineCursor, frame[DPMI_FRAME_CS]); lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, frame[DPMI_FRAME_IP]);
            lineCursor = LogPut(lineCursor, " -- patched; serviced on the next turn\r\n");
            LogAppend(LOG_PATH, lineBuffer, lineCursor);
            return 1;
        }
        if (exception < 0 || exception >= X86_EXCEPTIONS || !g_PmException[exception].IsSet || (frame[DPMI_FRAME_ERROR] & X86_ERROR_CODE_IDT))
            return 0;
        frame[DPMI_FRAME_RETURN_IP] = (WORD)DPMI_FLTRET_COFF;
        frame[DPMI_FRAME_RETURN_CS] = g_DpmiFaultCodeSelector;
        VDM_SET16(tib, VTIB_CS, g_PmException[exception].Selector);
        VDM_REG(tib, VTIB_EIP) = g_PmException[exception].Offset;
        lineCursor = LogPut(lineCursor, "NESTED EXC 0x"); lineCursor = LogHex(lineCursor, (DWORD)exception);
        lineCursor = LogPut(lineCursor, " err=0x"); lineCursor = LogHex(lineCursor, frame[DPMI_FRAME_ERROR]);
        lineCursor = LogPut(lineCursor, " at 0x"); lineCursor = LogHex(lineCursor, frame[DPMI_FRAME_CS]); lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, frame[DPMI_FRAME_IP]);
        lineCursor = LogPut(lineCursor, " -> client handler 0x"); lineCursor = LogHex(lineCursor, g_PmException[exception].Selector);
        lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, g_PmException[exception].Offset);
        /* s93: a stack fault is about SS:SP -- say them, and the limit, and which
         * nested call was running (Terminal and Program Manager both took #SS here).
         */
        lineCursor = LogPut(lineCursor, " ss:sp=0x"); lineCursor = LogHex(lineCursor, frame[DPMI_FRAME_SS]); lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, frame[DPMI_FRAME_SP]);
        if (DPMI_SELECTOR_INDEX(frame[DPMI_FRAME_SS]) < DPMI_LDT_MAX)
        {
            lineCursor = LogPut(lineCursor, " ss.limit=0x"); lineCursor = LogHex(lineCursor, g_Ldt[DPMI_SELECTOR_INDEX(frame[DPMI_FRAME_SS])].Limit);
        }
        if (g_WowCallDepth > 0)
        {
            lineCursor = LogPut(lineCursor, " call hwnd=0x"); lineCursor = LogHex(lineCursor, g_WowCallFrames[g_WowCallDepth - 1].Window);
            lineCursor = LogPut(lineCursor, " msg=0x"); lineCursor = LogHex(lineCursor, g_WowCallFrames[g_WowCallDepth - 1].Message);
            lineCursor = LogPut(lineCursor, " depth=0x"); lineCursor = LogHex(lineCursor, (DWORD)g_WowCallDepth);
        }
        lineCursor = LogPut(lineCursor, " nested=0x"); lineCursor = LogHex(lineCursor, (DWORD)g_WowWindowNested);
        lineCursor = LogPut(lineCursor, "\r\n");
        LogAppend(LOG_PATH, lineBuffer, lineCursor);
        return 1;
    }
    return 0;
}

enum
{
    DPMI_HOST_SELECTORS = 4
};   /* DpmiClientTeardown: the host's own LDT entries it keeps */
INT DpmiInjectPmIrq(DOS_MACHINE *machine, volatile BYTE *tib, UINT interruptVector, UINT steps)
{
    CHAR lineBuffer[256];
    CHAR *lineCursor = lineBuffer;

    if (g_PmClientExited)
        return 0;                                /* nothing left to interrupt */
    /* snapshot the interrupted PM register file */
    DWORD sEAX=VDM_REG(tib,VTIB_EAX);
    DWORD sEBX=VDM_REG(tib,VTIB_EBX);
    DWORD sECX=VDM_REG(tib,VTIB_ECX);
    DWORD sEDX=VDM_REG(tib,VTIB_EDX);
    DWORD sESI=VDM_REG(tib,VTIB_ESI);
    DWORD sEDI=VDM_REG(tib,VTIB_EDI);
    DWORD sEBP=VDM_REG(tib,VTIB_EBP);
    DWORD sEIP=VDM_REG(tib,VTIB_EIP);
    DWORD sESP=VDM_REG(tib,VTIB_ESP);
    DWORD sEFL=VDM_REG(tib,VTIB_EFLAGS);
    WORD savedCs=(WORD)VDM_REG(tib,VTIB_CS);
    WORD savedSs=(WORD)VDM_REG(tib,VTIB_SS);
    WORD savedDs=(WORD)VDM_REG(tib,VTIB_DS);
    WORD savedEs=(WORD)VDM_REG(tib,VTIB_ES);
    WORD savedFs=(WORD)VDM_REG(tib,VTIB_FS);
    WORD savedGs=(WORD)VDM_REG(tib,VTIB_GS);
    INT previousVi = g_DpmiVi;
    UINT phase;
    INT done = 0;
    DWORD isrStartTick = GetTickCount();
    DWORD watchBefore[DPMI_WATCH_MAX];
    INT watchIndex;
    for (watchIndex = 0; watchIndex < g_PmWatchCount; ++watchIndex)
    {
        const BYTE *watchStart = (const BYTE *)(ULONG_PTR)PmWatchAddress(watchIndex);
        watchBefore[watchIndex] = (watchStart && HostReadable(watchStart, 4)) ? *(const DWORD *)watchStart : 0xDEADDEADu;
    }

    DpmiEnsurePmReturnSelector();
    if (g_PmReturnSelector == 0)
        return 0;
    /* #172 census: which of the two refusals below, when, and where the guest was. */
    if (interruptVector == VECTOR_TIMER && g_DpmiIsClient32 && (!DpmiSelectorIs32(savedCs) || !g_PmAppHookedTimer))
        PmInjectDeclineNote(!DpmiSelectorIs32(savedCs) ? 0 : 1, savedCs, sEIP);
    /* DO NOT INTERRUPT THE EXTENDER, ONLY THE APPLICATION:
     * Measured: the first injection landed at mod:0x4b81 -- inside DOS/4GW's own INT
     * 21h thunk epilogue, on ITS internal 16-bit stack (SS=0xcf, SP=0x1a74) -- and the
     * run ended with no further output. Its dispatcher (mod:0x550) immediately reads
     * `LAR SS` and then walks an internal stack table at [0xa42], switching stacks and
     * bounds-checking the result; arriving there on a stack it did not expect, in the
     * middle of servicing a call it had not finished, is not a state it is written to
     * survive.
     * The case that MATTERS is the other one: the application spinning on its own timer
     * counter, in its own 32-bit flat code, which is precisely where a tick has to land
     * for the game to advance. So require the interrupted code to be 32-bit whenever
     * the client is -- the extender's modules are all 16-bit selectors, so this
     * separates "the game is running" from "the extender is mid-service" exactly.
     * A 16-bit client keeps the previous behaviour unchanged.
     */
    if (g_DpmiIsClient32 && !DpmiSelectorIs32(savedCs))
        return 0;
    /* ...and not before the application actually has an ISR. Delivery still goes through
     * the extender's stub, because the extender owns the IDT and must do the dispatching
     * (bypassing it produced "fatal error (1001): error in interrupt chain").
     */
    if (g_DpmiIsClient32 && interruptVector == VECTOR_TIMER && !g_PmAppHookedTimer)
        return 0;

    /* push an INT frame (FLAGS/CS/IP) on the client's current PM stack so the handler's IRET
     * lands on the catcher; keep the client's own SS so the handler has a valid stack. Frame
     * width + entry-offset mask follow the handler CS D-bit: a 32-bit PM INT handler wants a
     * dword frame and a full 32-bit entry offset (GH #18 run 83).
     */
    /* Same rule as DpmiDispatchToPmHandler(): the frame width is the CLIENT's, not
     * the handler selector's. For every case confirmed so far the two agree (a 16-bit
     * client with a 16-bit handler), so this is a consistency fix rather than a
     * behaviour change -- but they diverge exactly where DOS/4GW lives.
     */
    INT isClient32 = g_DpmiIsClient32;
    { WORD ss = savedSs;
    DWORD stackBase = DpmiSelectorBase(ss);
      INT ss32 = DpmiSelectorIs32(ss);                /* see the note in the dispatch path */
      DWORD sp = ss32 ? sESP : (sESP & WORD_MASK);
      if (isClient32)
      {
          sp = ss32 ? sp - X86_DWORD_SIZE : ((sp - X86_DWORD_SIZE) & WORD_MASK);
          PokeDword(stackBase + sp, sEFL);
          sp = ss32 ? sp - X86_DWORD_SIZE : ((sp - X86_DWORD_SIZE) & WORD_MASK);
          PokeDword(stackBase + sp, g_PmReturnSelector);
          sp = ss32 ? sp - X86_DWORD_SIZE : ((sp - X86_DWORD_SIZE) & WORD_MASK);
          PokeDword(stackBase + sp, DPMI_PMRET_OFF);
      }
      else
      {
          sp = ss32 ? sp - X86_WORD_SIZE : ((sp - X86_WORD_SIZE) & WORD_MASK);
          PokeWord(stackBase + sp, (WORD)sEFL);
          sp = ss32 ? sp - X86_WORD_SIZE : ((sp - X86_WORD_SIZE) & WORD_MASK);
          PokeWord(stackBase + sp, g_PmReturnSelector);
          sp = ss32 ? sp - X86_WORD_SIZE : ((sp - X86_WORD_SIZE) & WORD_MASK);
          PokeWord(stackBase + sp, DPMI_PMRET_OFF);
      }
      VDM_SET16(tib, VTIB_SS, ss);
      VDM_REG(tib, VTIB_ESP) = ss32 ? sp : ((sESP & HIGH_WORD_MASK_U) | sp); }
    g_DpmiVi = 0;                                 /* mask further virtual interrupts */
    /* An interrupt gate CLEARS IF and leaves everything else alone. This used to assign
     * VTIB_EFLAGS_PM (0x202) outright, which both sets IF -- the opposite of what a gate
     * does -- and discards the guest's own flags, DF included, so a handler that returned
     * through a string operation would run it in the wrong direction.
     */
    /* VIP too -- see DpmiAsyncInjectPm() for why an STI with VIP set is fatal here. */
    VDM_REG(tib, VTIB_EFLAGS) = (sEFL & ~(EFLAGS_IF_U | EFLAGS_VIF | EFLAGS_VIP)) | EFLAGS_RESERVED_ONE_U;
    *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR &= ~VDM_INT_PENDING;
    VDM_SET16(tib, VTIB_CS, DPMI_IRQ_TARGET_SEL(interruptVector));
    VDM_REG(tib, VTIB_EIP) = isClient32 ? DPMI_IRQ_TARGET_OFF(interruptVector) : (DPMI_IRQ_TARGET_OFF(interruptVector) & WORD_MASK);

    lineCursor = LogPut(lineCursor, "  IRQ0->PM INT 0x"); lineCursor = LogHex(lineCursor, interruptVector);
    lineCursor = LogPut(lineCursor, " handler 0x"); lineCursor = LogHex(lineCursor, DPMI_IRQ_TARGET_SEL(interruptVector));
    lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, DPMI_IRQ_TARGET_OFF(interruptVector));
    /* What we are about to RUN, and the stack we are about to run it on. This path dies
     * with no further output, so anything not printed here is unrecoverable afterwards.
     */
    { WORD handlerSelector = DPMI_IRQ_TARGET_SEL(interruptVector);
      DWORD handlerLinear = DpmiSelectorBase(handlerSelector) + (g_DpmiIsClient32 ? DPMI_IRQ_TARGET_OFF(interruptVector) : (DPMI_IRQ_TARGET_OFF(interruptVector) & WORD_MASK));
      const BYTE *handlerBytes = (const BYTE *)(ULONG_PTR)handlerLinear;
      lineCursor = LogPut(lineCursor, " lin=0x"); lineCursor = LogHex(lineCursor, handlerLinear);
      lineCursor = LogPut(lineCursor, DpmiSelectorIs32(handlerSelector) ? " (h CS D/B=1)" : " (h CS D/B=0)");
      lineCursor = LogPut(lineCursor, " h32="); lineCursor = LogHex(lineCursor, (DWORD)isClient32);
      lineCursor = LogPut(lineCursor, " SS:ESP=0x"); lineCursor = LogHex(lineCursor, VDM_REG16(tib, VTIB_SS));
      lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, VDM_REG(tib, VTIB_ESP));
      lineCursor = LogPut(lineCursor, DpmiSelectorIs32(savedSs) ? " (SS D/B=1)" : " (SS D/B=0)");
      lineCursor = LogPut(lineCursor, " from 0x"); lineCursor = LogHex(lineCursor, savedCs); lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, sEIP);
      lineCursor = LogPut(lineCursor, " bytes@handler=");
      if (!HostReadable(handlerBytes, 16))
          lineCursor = LogPut(lineCursor, "<unreadable>");
      else
          lineCursor = LogDump(lineCursor, handlerBytes, 16); }
    lineCursor = LogPut(lineCursor, "\r\n");
    LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor); lineCursor = lineBuffer;

    /* GIVING UP HALFWAY THROUGH SOMEONE ELSE'S INTERRUPT HANDLER CORRUPTS THEM:
     * This loop used to stop after 64 phases and then restore the interrupted context
     * verbatim, abandoning the client's ISR wherever it had got to. That is not a
     * timeout, it is a silent state corruption, and it is what stopped Doom's clock:
     *   IRQ0<-PM done=0 phases=0x40   [DMX depth]=3<-4   [DMX stack]=...4300<-...5300
     * one abandoned dispatch leaked DMX's re-entrancy counter and one 4KB frame of its
     * private interrupt stack, permanently. Doom's ticcount froze at 0x61 while 3,000
     * more ticks were delivered into a dispatcher that would never call its service
     * again, so I_GetTime() stopped, so TryRunTics() spun forever, so the title screen
     * sat there for the rest of the run.
     * A PHASE IS NOT A UNIT OF TIME -- it is one PM entry, and an ISR pays one for
     * every trapped port access. The handler that blew the cap was the MIDI driver
     * feeding the MPU-401: ~11 status polls per byte written, so a single music update
     * is hundreds of phases. 64 was never a bound on anything real.
     * So: bound it by WALL CLOCK, which is the thing actually at risk, keep the phase
     * count only as a runaway backstop, and if we ever do stop early SAY SO -- it is a
     * corruption event, not housekeeping. A genuinely wedged ISR is the watchdog's
     * problem; it already terminates a VDM that stops making progress.
     */
    for (phase = 0; phase < DPMI_IRQ0_PHASE_MAX && !done; ++phase)
    {
        DWORD event;
        DWORD eip;
        DWORD vector;
        INT status;
        if ((phase & DPMI_IRQ_TIME_CHECK_MASK) == DPMI_IRQ_TIME_CHECK_MASK && (GetTickCount() - isrStartTick) > DPMI_IRQ0_MS_MAX)
            break;
        DpmiArmFaultTrampoline(tib, 0);
        /* - THE LAST THING BEFORE THE CLIFF. Doom takes five of these injections and
         * dies inside the SIXTH: its "IRQ0->PM INT" entry line is the final line in
         * the log, DpmiEnterProtectedMode() never returns, and the VDM is gone. The entry line
         * above is printed once per injection, so it cannot show what changed BETWEEN
         * the fifth and the sixth -- and the five that work are byte-identical in
         * every field it prints. Log the state at each PM entry instead, bounded, so
         * the fatal one can be DIFFED against its five healthy predecessors.
         */
        if (g_PmIrq0Done < 12)
        {
            CHAR errorLine[192];
            CHAR *errorCursor = errorLine;
            errorCursor = LogPut(errorCursor, "   PMENT tick="); errorCursor = LogHex(errorCursor, (DWORD)g_PmIrq0Done);
            errorCursor = LogPut(errorCursor, " ph="); errorCursor = LogHex(errorCursor, (DWORD)phase);
            errorCursor = LogPut(errorCursor, " cs:eip=0x"); errorCursor = LogHex(errorCursor, VDM_REG16(tib, VTIB_CS));
            errorCursor = LogPut(errorCursor, ":0x"); errorCursor = LogHex(errorCursor, VDM_REG(tib, VTIB_EIP));
            errorCursor = LogPut(errorCursor, " ss:esp=0x"); errorCursor = LogHex(errorCursor, VDM_REG16(tib, VTIB_SS));
            errorCursor = LogPut(errorCursor, ":0x"); errorCursor = LogHex(errorCursor, VDM_REG(tib, VTIB_ESP));
            errorCursor = LogPut(errorCursor, " efl=0x"); errorCursor = LogHex(errorCursor, VDM_REG(tib, VTIB_EFLAGS));
            errorCursor = LogPut(errorCursor, " [714]=0x"); errorCursor = LogHex(errorCursor, *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR);
            errorCursor = LogPut(errorCursor, " vi="); errorCursor = LogHex(errorCursor, (DWORD)g_DpmiVi);
            errorCursor = LogPut(errorCursor, " apa="); errorCursor = LogHex(errorCursor, (DWORD)g_AsyncPmActive);
            errorCursor = LogPut(errorCursor, "\r\n"); LogAppend(LOG_PATH, errorLine, errorCursor); SerialOut(errorLine, errorCursor);
        }
        DpmiEnterProtectedMode(tib);
        event  = VDM_REG(tib, VTIB_EVENT);
        eip = DpmiPmEip(tib);
        if (event == VDM_EVENT_BOP && eip == DPMI_PMRET_OFF
            && VDM_REG16(tib, VTIB_CS) == g_PmReturnSelector)
        {
            done = 1;
            break;
        }
        if (event == 3)
            continue;                                 /* "interrupt pending, not entered" -> retry */
        if (event == VDM_EVENT_IO || event == VDM_EVENT_IO_HW || event == VDM_EVENT_GPFAULT)
        {
            INT ioHandled;
            HOST_LOCK();
            ioHandled = HostTryIoPm(tib, &g_Bus);
            HOST_UNLOCK();
            if (ioHandled)
                continue;
        }
        if (DpmiNestedFault(tib, event, eip))
            continue;                                        /* s90 */
        vector = (event == VDM_EVENT_BOP) ? DpmiBopVector(VDM_REG16(tib, VTIB_CS), eip) : 0;
        status = DpmiServicePmInt(machine, tib, vector, steps);
        if (status > 0)
            continue;
        break;                                     /* handler exited / unexpected stop */
    }

    /* - DID THE HANDLER ACTUALLY FINISH? The entry line alone cannot distinguish "the
     * ISR ran and returned" from "the ISR was entered and the run ended inside it",
     * and those need completely different fixes. `done` is set only by the catcher
     * BOP, i.e. by the handler's own IRET.
     */
    /* - AN ABANDONED HANDLER IS A LOUD EVENT. It leaves the client's interrupt
     * bookkeeping permanently wrong -- see the phase loop -- so it must never again
     * be readable as a routine "done=0".
     */
    if (!done && g_PmClientExited)
    {
        CHAR afterLine[128];
        CHAR *afterCursor = afterLine;
        afterCursor = LogPut(afterCursor, "DPMI: the client EXITED inside its vec 0x"); afterCursor = LogHexByte(afterCursor, interruptVector);
        afterCursor = LogPut(afterCursor, " handler after "); afterCursor = LogHex(afterCursor, (DWORD)phase);
        afterCursor = LogPut(afterCursor, " phases -- not resuming it\r\n");
        LogAppend(LOG_PATH, afterLine, afterCursor); SerialOut(afterLine, afterCursor);
    }
    else if (!done)
    {
        CHAR afterLine[192];
        CHAR *afterCursor = afterLine;
        afterCursor = LogPut(afterCursor, "DPMI: *** PM ISR ABANDONED after "); afterCursor = LogHex(afterCursor, (DWORD)phase);
        afterCursor = LogPut(afterCursor, " phases / "); afterCursor = LogHex(afterCursor, GetTickCount() - isrStartTick);
        afterCursor = LogPut(afterCursor, " ms -- the client's interrupt state is now INCONSISTENT"
                      " (vec 0x");
                      afterCursor = LogHexByte(afterCursor, interruptVector);
        afterCursor = LogPut(afterCursor, ", last cs:eip=0x"); afterCursor = LogHex(afterCursor, VDM_REG16(tib, VTIB_CS));
        afterCursor = LogPut(afterCursor, ":0x"); afterCursor = LogHex(afterCursor, DpmiPmEip(tib));
        afterCursor = LogPut(afterCursor, ")\r\n");
        LogAppend(LOG_PATH, afterLine, afterCursor); SerialOut(afterLine, afterCursor);
    }
    /* - NAME THE VECTOR. This line said "IRQ0" whatever it had just injected, and since
     * session 23 gave the DEVICE lines a cooperative path it is used for IRQ5 too --
     * so counting these lines to measure the TIMER's delivery over-counts by however
     * much the Sound Blaster contributed. Same fault as ASYNC-PM's hardcoded
     * "vec=0x08", one function over, and the same fix.
     */
    if (interruptVector <= VECTOR_IRQ7) { g_PmCooperativeLine[interruptVector & (PIC_LINES_PER_CHIP - 1)]++;
                      if (interruptVector == VECTOR_TIMER)
                          TickDeliveredNote(); }
    { CHAR changeLine[128], *changeCursor = changeLine;
      changeCursor = LogPut(changeCursor, "  PMIRQ vec=0x"); changeCursor = LogHexByte(changeCursor, interruptVector);
      changeCursor = LogPut(changeCursor, " done="); changeCursor = LogHex(changeCursor, (DWORD)done);
      changeCursor = LogPut(changeCursor, " phases="); changeCursor = LogHex(changeCursor, (DWORD)phase);
      changeCursor = LogPut(changeCursor, " ticks="); changeCursor = LogHex(changeCursor, ++g_PmIrq0Done);
      for (watchIndex = 0; watchIndex < g_PmWatchCount; ++watchIndex)
      {
          DWORD watchAddress = PmWatchAddress(watchIndex);
          const BYTE *watchBytes = (const BYTE *)(ULONG_PTR)watchAddress;
          changeCursor = LogPut(changeCursor, " ["); changeCursor = LogHex(changeCursor, watchAddress); changeCursor = LogPut(changeCursor, "]=0x");
          if (!watchAddress || !HostReadable(watchBytes, 4))
              changeCursor = LogPut(changeCursor, "????????");
          else
              changeCursor = LogHex(changeCursor, *(const DWORD *)watchBytes);
          changeCursor = LogPut(changeCursor, "<-0x"); changeCursor = LogHex(changeCursor, watchBefore[watchIndex]);
      }
      changeCursor = LogPut(changeCursor, "\r\n");
      LogAppend(LOG_PATH, changeLine, changeCursor);
      SerialOut(changeLine, changeCursor); }
    /* restore the interrupted PM context verbatim + unmask */
    VDM_REG(tib,VTIB_EAX)=sEAX;
    VDM_REG(tib,VTIB_EBX)=sEBX;
    VDM_REG(tib,VTIB_ECX)=sECX;
    VDM_REG(tib,VTIB_EDX)=sEDX;
    VDM_REG(tib,VTIB_ESI)=sESI;
    VDM_REG(tib,VTIB_EDI)=sEDI;
    VDM_REG(tib,VTIB_EBP)=sEBP;
    VDM_REG(tib,VTIB_EIP)=sEIP;
    VDM_REG(tib,VTIB_ESP)=sESP;
    VDM_REG(tib,VTIB_EFLAGS)=sEFL;
    VDM_SET16(tib,VTIB_CS,savedCs);
    VDM_SET16(tib,VTIB_SS,savedSs);
    VDM_SET16(tib,VTIB_DS,savedDs);
    VDM_SET16(tib,VTIB_ES,savedEs);
    VDM_SET16(tib,VTIB_FS,savedFs);
    VDM_SET16(tib,VTIB_GS,savedGs);
    g_DpmiVi = previousVi;
    return done;
}

/* THE INT 33h EVENT HANDLER, FOR A PROTECTED-MODE CLIENT. (s74c: ZAR's clicks):
 * MouseCallbackTry() delivers 0Ch callbacks to V86 guests only; for a DPMI client it
 * DROPPED the queue ("not this path (yet)", counted as cb_pm). ZAR installs its
 * handler from flat 32-bit code -- 0Ch with ES:EDX = 0x347:0x0044xxxx, mask 0x7e =
 * button press/release only -- and polls motion with 0Bh. So aiming worked and no
 * click ever reached the game: the buttons only travel through the callback.
 * On real hardware DOS/4GW turns the driver's real-mode call into a PM far call of
 * the client's handler with the same registers (AX event bits, BX buttons, CX/DX
 * position, SI/DI mickeys) and the handler RETFs. Do the same here, with the
 * mechanics of DpmiInjectPmIrq(): interrupt the APPLICATION only (32-bit CS when
 * the client is), on its own stack, a far-return frame onto the PM-return catcher,
 * the same phase loop, the interrupted context restored verbatim. The frame is a
 * RETF frame (CS:EIP, no FLAGS) because that is what the handler pops.
 */
INT DpmiInjectPmMouseCallback(DOS_MACHINE *machine, volatile BYTE *tib, UINT steps)
{
    if (g_PmClientExited)
        return 0;                            /* nothing left to call into */
    DWORD sEAX=VDM_REG(tib,VTIB_EAX);
    DWORD sEBX=VDM_REG(tib,VTIB_EBX);
    DWORD sECX=VDM_REG(tib,VTIB_ECX);
    DWORD sEDX=VDM_REG(tib,VTIB_EDX);
    DWORD sESI=VDM_REG(tib,VTIB_ESI);
    DWORD sEDI=VDM_REG(tib,VTIB_EDI);
    DWORD sEBP=VDM_REG(tib,VTIB_EBP);
    DWORD sEIP=VDM_REG(tib,VTIB_EIP);
    DWORD sESP=VDM_REG(tib,VTIB_ESP);
    DWORD sEFL=VDM_REG(tib,VTIB_EFLAGS);
    WORD savedCs=(WORD)VDM_REG(tib,VTIB_CS);
    WORD savedSs=(WORD)VDM_REG(tib,VTIB_SS);
    WORD savedDs=(WORD)VDM_REG(tib,VTIB_DS);
    WORD savedEs=(WORD)VDM_REG(tib,VTIB_ES);
    WORD savedFs=(WORD)VDM_REG(tib,VTIB_FS);
    WORD savedGs=(WORD)VDM_REG(tib,VTIB_GS);
    INT previousVi = g_DpmiVi;
    UINT phase;
    INT done = 0;
    DWORD start = GetTickCount();
    LONG pend = 0;
    MOUSE_EVENT_ENTRY event = { 0, 0, 0, 0 };
    WORD handlerSelector = 0;
    DWORD handlerOffset = 0;
    INT isClient32 = g_DpmiIsClient32;

    if (!MouseAnyHandler())
        return 0;                                              /* 0Ch's or 18h's (#265) */
    DpmiEnsurePmReturnSelector();
    if (g_PmReturnSelector == 0)
        return 0;
    if (g_DpmiIsClient32 && !DpmiSelectorIs32(savedCs))
        return 0;                                                      /* the extender mid-service */
    if (!(savedSs & DPMI_SELECTOR_TI))
        return 0;                                                                   /* not a client stack */
    /* The oldest queued event a handler asked for, and which handler (MouseEventQueueTake). */
    if (!MouseEventQueueTake(&event, &pend, &handlerSelector, &handlerOffset) || !pend)
        return 0;
    /* A far-return frame (CS:EIP) onto the catcher, on the client's own stack. Frame
     * width is the CLIENT's; stack addressing is the SS descriptor's B bit.
     */
    { DWORD stackBase = DpmiSelectorBase(savedSs);
      INT ss32 = DpmiSelectorIs32(savedSs);
      DWORD sp = ss32 ? sESP : (sESP & WORD_MASK);
      if (isClient32)
      {
          sp = ss32 ? sp - X86_DWORD_SIZE : ((sp - X86_DWORD_SIZE) & WORD_MASK);
          PokeDword(stackBase + sp, g_PmReturnSelector);
          sp = ss32 ? sp - X86_DWORD_SIZE : ((sp - X86_DWORD_SIZE) & WORD_MASK);
          PokeDword(stackBase + sp, DPMI_PMRET_OFF);
      }
      else
      {
          sp = ss32 ? sp - X86_WORD_SIZE : ((sp - X86_WORD_SIZE) & WORD_MASK);
          PokeWord(stackBase + sp, g_PmReturnSelector);
          sp = ss32 ? sp - X86_WORD_SIZE : ((sp - X86_WORD_SIZE) & WORD_MASK);
          PokeWord(stackBase + sp, DPMI_PMRET_OFF);
      }
      VDM_REG(tib, VTIB_ESP) = ss32 ? sp : ((sESP & HIGH_WORD_MASK_U) | sp); }
    VDM_SET16(tib, VTIB_EAX, (WORD)pend);
    VDM_SET16(tib, VTIB_EBX, (WORD)event.Buttons);
    VDM_SET16(tib, VTIB_ECX, (WORD)I33ClampX(I33VirtualX(event.X)));
    VDM_SET16(tib, VTIB_EDX, (WORD)I33ClampY(I33VirtualY(event.Y)));
    VDM_REG(tib, VTIB_ESI) = 0;
    VDM_REG(tib, VTIB_EDI) = 0;
    /* DS stays the interrupted application's: for a flat client that IS its data
     * selector, and a Watcom `__loadds` handler reloads its own anyway.
     */
    g_DpmiVi = 0;
    VDM_REG(tib, VTIB_EFLAGS) = (sEFL & ~(EFLAGS_IF_U | EFLAGS_VIF | EFLAGS_VIP)) | EFLAGS_RESERVED_ONE_U;
    *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR &= ~VDM_INT_PENDING;
    VDM_SET16(tib, VTIB_CS, handlerSelector);
    VDM_REG(tib, VTIB_EIP) = isClient32 ? handlerOffset : (handlerOffset & WORD_MASK);
    ++g_MouseCallbackInjected;
    if (g_MouseCallbackInjected <= 16)
    {
        CHAR lineBuffer[256];
        CHAR *lineCursor = lineBuffer;
        lineCursor = LogPut(lineCursor, "  MOUSECB->PM #"); lineCursor = LogHex(lineCursor, g_MouseCallbackInjected);
        lineCursor = LogPut(lineCursor, " ev=0x"); lineCursor = LogHex(lineCursor, (DWORD)pend);
        lineCursor = LogPut(lineCursor, " btn=0x"); lineCursor = LogHex(lineCursor, (DWORD)event.Buttons);
        lineCursor = LogPut(lineCursor, " handler 0x"); lineCursor = LogHex(lineCursor, handlerSelector); lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, handlerOffset);
        lineCursor = LogPut(lineCursor, " from 0x"); lineCursor = LogHex(lineCursor, savedCs); lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, sEIP);
        lineCursor = LogPut(lineCursor, " ss:esp=0x"); lineCursor = LogHex(lineCursor, savedSs); lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, VDM_REG(tib, VTIB_ESP));
        lineCursor = LogPut(lineCursor, " h32="); lineCursor = LogHex(lineCursor, (DWORD)isClient32);
        lineCursor = LogPut(lineCursor, "\r\n"); LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor);
    }
    for (phase = 0; phase < DPMI_IRQ0_PHASE_MAX && !done; ++phase)
    {
        DWORD runEvent;
        DWORD eip;
        DWORD vector;
        INT status;
        if ((phase & DPMI_IRQ_TIME_CHECK_MASK) == DPMI_IRQ_TIME_CHECK_MASK && (GetTickCount() - start) > DPMI_IRQ0_MS_MAX)
            break;
        DpmiArmFaultTrampoline(tib, 0);
        DpmiEnterProtectedMode(tib);
        runEvent   = VDM_REG(tib, VTIB_EVENT);
        eip = DpmiPmEip(tib);
        if (runEvent == VDM_EVENT_BOP && eip == DPMI_PMRET_OFF
            && VDM_REG16(tib, VTIB_CS) == g_PmReturnSelector)
        {
            done = 1;
            break;
        }
        if (runEvent == 3)
            continue;
        if (runEvent == VDM_EVENT_IO || runEvent == VDM_EVENT_IO_HW || runEvent == VDM_EVENT_GPFAULT)
        {
            INT ioHandled;
            HOST_LOCK();
            ioHandled = HostTryIoPm(tib, &g_Bus);
            HOST_UNLOCK();
            if (ioHandled)
                continue;
        }
        if (DpmiNestedFault(tib, runEvent, eip))
            continue;                                            /* s90 */
        vector = (runEvent == VDM_EVENT_BOP) ? DpmiBopVector(VDM_REG16(tib, VTIB_CS), eip) : 0;
        status = DpmiServicePmInt(machine, tib, vector, steps);
        if (status > 0)
            continue;
        break;
    }
    if (done)
        ++g_MouseCallbackDone;
    else
        ++g_MouseCallbackLost;
    if (!done)
    {
        CHAR afterLine[192];
        CHAR *afterCursor = afterLine;
        afterCursor = LogPut(afterCursor, "DPMI: *** PM MOUSE CALLBACK ABANDONED after "); afterCursor = LogHex(afterCursor, (DWORD)phase);
        afterCursor = LogPut(afterCursor, " phases (last cs:eip=0x"); afterCursor = LogHex(afterCursor, VDM_REG16(tib, VTIB_CS));
        afterCursor = LogPut(afterCursor, ":0x"); afterCursor = LogHex(afterCursor, DpmiPmEip(tib)); afterCursor = LogPut(afterCursor, ")\r\n");
        LogAppend(LOG_PATH, afterLine, afterCursor); SerialOut(afterLine, afterCursor);
    }
    VDM_REG(tib,VTIB_EAX)=sEAX;
    VDM_REG(tib,VTIB_EBX)=sEBX;
    VDM_REG(tib,VTIB_ECX)=sECX;
    VDM_REG(tib,VTIB_EDX)=sEDX;
    VDM_REG(tib,VTIB_ESI)=sESI;
    VDM_REG(tib,VTIB_EDI)=sEDI;
    VDM_REG(tib,VTIB_EBP)=sEBP;
    VDM_REG(tib,VTIB_EIP)=sEIP;
    VDM_REG(tib,VTIB_ESP)=sESP;
    VDM_REG(tib,VTIB_EFLAGS)=sEFL;
    VDM_SET16(tib,VTIB_CS,savedCs);
    VDM_SET16(tib,VTIB_SS,savedSs);
    VDM_SET16(tib,VTIB_DS,savedDs);
    VDM_SET16(tib,VTIB_ES,savedEs);
    VDM_SET16(tib,VTIB_FS,savedFs);
    VDM_SET16(tib,VTIB_GS,savedGs);
    g_DpmiVi = previousVi;
    return done;
}

/* THE CLIENT EXITED: GIVE BACK WHAT IT HAD AND LEAVE PROTECTED MODE. (s80) (Importance = 3):
 * A DPMI client's `AH=4Ch` in protected mode ended the WHOLE VDM, because the switch
 * into PM was one-way: DpmiSwitchToProtectedMode() builds global state and nothing ever took
 * it down. That is fine for a program launched on its own and wrong for every other
 * shape -- the user's "save settings and run Doom" from SETUP, `doom` typed at
 * COMMAND.COM -- where a parent is waiting in real mode for its child to return.
 * DPMI 0.9 has the host release the client's resources on its exit and let DOS
 * terminate the real-mode half; this is that release. The caller then clears PE and
 * runs the real-mode terminate (DosTerminate), which restores the parent's frame.
 * - WHAT IT KEEPS is as important as what it drops: the HOST's own selectors and the
 *   default-handler table are used again by the next client. Everything else the
 *   client made -- its LDT slots, its 0501/0100 memory, its vector hooks, callbacks,
 *   exception handlers -- goes, or the next client inherits a dead one's state.
 *
 * [WARNING]: THE PATCH MAP IS REBUILT, NOT KEPT. pmap holds the address of every INT site we
 * rewrote in the client's code, and that code lives in the 0501 blocks released
 * below. DpmiUnpatch/DpmiRepatch dereference every entry: left in place, the next
 * client's first 0301 would read freed memory and fault the host.
 */
VOID DpmiClientTeardown(VOID)
{
    INT index;
    INT keepHigh = g_LdtClientMark;
    INT freedLdt = 0;
    INT freedMemory = 0;
    INT freedDos = 0;
    INT hostIndex[DPMI_HOST_SELECTORS];
    CHAR lineBuffer[256];
    CHAR *cursor = lineBuffer;

    /* [WARNING]: PUT EVERY PATCHED INT SITE BACK FIRST, while all of them are still mapped. The
     * patcher's scans are not confined to the client's own blocks: the first version
     * of this function only FORGOT the sites, and the parent came back to its own
     * `int 21h` (AH=4Dh, COMMAND.COM at 0100:0a6e) still reading `C4 C4` -- an
     * orphaned BOP with no client behind it, and the shell died on its first call.
     * Restore `CD nn` wherever the bytes are still ours -- which also reverts the
     * host's default stubs; those are re-planted below.
     *
     * [CAUTION]: GUARDED, unlike DpmiUnpatch(): this runs once, so it can afford to ask
     * whether each site is still mapped and writable rather than trust the map.
     */
    {   DWORD slot;
        for (slot = 0; slot < DPMI_PMAP_SLOTS; ++slot)
        {
            DWORD address = g_PatchMapLinear[slot];
            BYTE vector = g_PatchMapVector[slot];
            MEMORY_BASIC_INFORMATION memoryInfo;
            volatile BYTE *bytes;
            if (!address || !vector || vector == DPMI_BP_VEC)
                continue;
            if (VirtualQuery((LPCVOID)(ULONG_PTR)address, &memoryInfo, sizeof memoryInfo) != sizeof memoryInfo
                || memoryInfo.State != MEM_COMMIT || (memoryInfo.Protect & PAGE_GUARD)
                || !(memoryInfo.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE
                                   | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))
                || address + 2 > (DWORD)(ULONG_PTR)memoryInfo.BaseAddress + (DWORD)memoryInfo.RegionSize)
                continue;
            bytes = (volatile BYTE *)(ULONG_PTR)address;
            if (bytes[0] == VDM_BOP0 && bytes[1] == VDM_BOP1)
            {
                bytes[0] = X86_OP_INT;
                bytes[1] = vector;
            }
        }
    }

    /* Memory: whatever the client did not free itself. */
    for (index = 0; index < g_DpmiOwnedCount; ++index)
        if (g_DpmiOwned[index] && VirtualFree((VOID *)(ULONG_PTR)g_DpmiOwned[index], 0, MEM_RELEASE))
            ++freedMemory;
    g_DpmiOwnedCount = 0;
    g_DpmiBlockCount = 0;
    for (index = 0; index < g_DpmiDosBlockCount; ++index)
        if (DosMcbFree(NULL, g_DpmiDosBlock[index]) == 0)
            ++freedDos;
    g_DpmiDosBlockCount = 0;
    g_LeCodeCount = 0;
    g_LeLoadBase = 0;

    /* The patch map: forget every client site, keep the host's 256 default stubs. */
    for (index = 0; index < (INT)DPMI_PMAP_SLOTS; ++index)
    {
        g_PatchMapLinear[index] = 0;
        g_PatchMapVector[index] = 0;
    }
    g_PatchMapCount = 0;
    /* ...unless the table itself lives in the DOS arena. Then it is a 0x40-paragraph
     * block in the middle of the parent's memory: measured, the next `doom` loaded
     * above it at 0x169f instead of 0x242. Give it back; keep the selector, which the
     * next client's install rebases (see DpmiInstallDefaultPmHandlers).
     */
    if (g_PmDefaultBase && g_PmDefaultFromDos)
    {
        DosMcbFree(NULL, (WORD)(g_PmDefaultBase >> PARAGRAPH_SHIFT));
        g_PmDefaultBase = 0;
        g_PmDefaultFromDos = 0;
    }
    if (g_PmDefaultBase)
        for (index = 0; index < IVT_VECTORS; ++index)
        {
            volatile BYTE *stub = (volatile BYTE *)(ULONG_PTR)(g_PmDefaultBase + (DWORD)index * DPMI_PMDEF_STRIDE);
            stub[0] = VDM_BOP0;
            stub[1] = VDM_BOP1;
            stub[DPMI_PM_BOP_LENGTH] = X86_OP_IRET;       /* BOP ; IRET, as installed */
            PatchMapSet(g_PmDefaultBase + (DWORD)index * DPMI_PMDEF_STRIDE, (BYTE)index);
        }

    /* LDT: every slot the client allocated goes back. The host's own selectors made
     * during the client's life (default table, fault trampoline, PM-return catcher)
     * stay, so the watermark can only come down as far as the highest of them; any
     * client slot below that goes on the free list instead.
     */
    hostIndex[0] = g_PmDefaultIndex;
    hostIndex[1] = g_PmReturnSelector ? (DPMI_SELECTOR_INDEX(g_PmReturnSelector)) : -1;
    hostIndex[2] = g_DpmiFaultSelector ? (DPMI_SELECTOR_INDEX(g_DpmiFaultSelector)) : -1;
    hostIndex[3] = g_DpmiFaultCodeSelector ? (DPMI_SELECTOR_INDEX(g_DpmiFaultCodeSelector)) : -1;
    for (index = 0; index < DPMI_HOST_SELECTORS; ++index)
        if (hostIndex[index] >= keepHigh)
            keepHigh = hostIndex[index] + 1;
    g_LdtFreeCount = 0;
    for (index = g_LdtClientMark; index < keepHigh; ++index)
    {
        INT isHostIndex = 0;
        INT index2;
        for (index2 = 0; index2 < DPMI_HOST_SELECTORS; ++index2)
            if (hostIndex[index2] == index)
                isHostIndex = 1;
        if (isHostIndex)
            continue;
        g_Ldt[index].Base = g_Ldt[index].Limit = 0;
        g_Ldt[index].Access = 0;
        g_Ldt[index].Flags = 0;
        if (g_LdtFreeCount < DPMI_LDT_MAX)
            g_LdtFree[g_LdtFreeCount++] = (WORD)index;
        ++freedLdt;
    }
    for (index = keepHigh; index < g_LdtNext; ++index)
    {
        g_Ldt[index].Base = g_Ldt[index].Limit = 0;
        g_Ldt[index].Access = 0;
        g_Ldt[index].Flags = 0;
        ++freedLdt;
    }
    if (g_LdtClientMark > 0)
        g_LdtNext = keepHigh;

    /* The client's hooks and handlers. */
    for (index = 0; index < IVT_VECTORS; ++index)
    {
        if (g_PmDefaultSelector)
        {
            g_PmInt[index].Selector = g_PmDefaultSelector;
            g_PmInt[index].Offset = (DWORD)index * DPMI_PMDEF_STRIDE;
        }
        g_PmInt[index].Client = 0;
        g_PmDispatch[index] = 0;
    }
    for (index = 0; index < X86_EXCEPTIONS; ++index)
        g_PmException[index].IsSet = 0;
    for (index = 0; index < DPMI_CB_SLOTS; ++index)
        g_Callbacks[index].IsUsed = 0;
    g_PmAppHookedTimer = 0;
    g_PmAppTimerSelector = 0;
    g_PmAppTimerOffset = 0;
    g_PmVector8ArmedMs = 0;

    /* Interrupt and mode state: back to "a real-mode program is running". */
    g_DpmiVi = 1;
    g_PmIrq0Latch = 0;
    InterlockedExchange(&g_PmTickOwed, 0);
    InterlockedExchange(&g_AsyncPmActive, 0);
    InterlockedExchange(&g_SimIntBusy, 0);
    g_InPmIrq = 0;
    InterlockedExchange(&g_PmEntryEip, -1);
    g_DpmiPm = 0;
    g_PmClientExited = 0;

    cursor = LogPut(cursor, "DPMI: client teardown -- released 0x"); cursor = LogHex(cursor, (DWORD)freedMemory);
    cursor = LogPut(cursor, " memory blocks, 0x"); cursor = LogHex(cursor, (DWORD)freedDos);
    cursor = LogPut(cursor, " DOS blocks, 0x"); cursor = LogHex(cursor, (DWORD)freedLdt);
    cursor = LogPut(cursor, " LDT slots (next=0x"); cursor = LogHex(cursor, (DWORD)g_LdtNext);
    cursor = LogPut(cursor, ", free-list 0x"); cursor = LogHex(cursor, (DWORD)g_LdtFreeCount);
    cursor = LogPut(cursor, ")\r\n");
    LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
}
