/* dpmi_svc.h -- the INT 31h answers that are decided by the DPMI specification, not by
 * the host's plumbing. GH #248.
 *
 * Header-only and dependency-free (stdint, no windows.h), the same shape as dpmi_rmcs.h,
 * so the off-VM battery (tests/unit/dpmisvc_test.c) compiles exactly the code the host
 * runs. What lives here is the part of each service with a RIGHT ANSWER: which selector
 * is invalid, which callback address names which slot, whether a resize can stay put.
 * The part that touches the LDT, VirtualAlloc or the guest's memory stays in main.c.
 *
 * ⚠ THE SPECIFICATION IS NOT IN THE REPO (docs/ref/SOURCES.md). Everything below that
 *   says "the spec" is the DPMI 0.9/1.0 text as published by the DPMI Committee; the
 *   error numbers are 1.0's, which 0.9 hosts return too (0.9 only promises CF).
 * ⚠ NO ORACLE HERE ANSWERS INT 31h. MS-DOS 6.22, DOSBox-X and PCem all run without a
 *   DPMI host (tests/probes/dos/p_dpmins.com: AX comes back 1687h untouched on all three),
 *   so the spec is the only authority for this file, and stock ntvdm -- the one machine
 *   that could answer -- needs the IFEO bracket.
 */
#ifndef NTVDMEX_DPMI_SVC_H
#define NTVDMEX_DPMI_SVC_H

#include "../ntvdmex_types.h"

/* ── ERROR CODES (DPMI 1.0 numbering; returned in AX with CF=1). ─────────────────────── */
#define DPMI_E_DESC_UNAVAIL   0x8011   /* descriptor unavailable                        */
#define DPMI_E_LIN_UNAVAIL    0x8012   /* linear memory unavailable                     */
#define DPMI_E_PHYS_UNAVAIL   0x8013   /* physical memory unavailable                   */
#define DPMI_E_CB_UNAVAIL     0x8015   /* callback unavailable                          */
#define DPMI_E_HANDLE_UNAVAIL 0x8016   /* handle unavailable                            */
#define DPMI_E_INVALID_VALUE  0x8021   /* invalid value                                 */
#define DPMI_E_INVALID_SEL    0x8022   /* invalid selector                              */
#define DPMI_E_INVALID_HANDLE 0x8023   /* invalid handle                                */
#define DPMI_E_INVALID_CB     0x8024   /* invalid callback                              */

/* ── ONE MACHINE, DESCRIBED ONCE: 1687h AND 0400h. ──────────────────────────────────────
     Both report the processor class in CL, and they disagreed: 1687h said 4 (s79, the
     GetWinFlags fix -- see the long note at DPMI_CPU_CLASS's old home in main.c and
     getwinflags-is-a-dpmi-answer) while 0400h still said 3, the value that fix replaced.
     A client that asks both was told two different machines. Every site now reads these.
   ► CL: the spec's processor type (02h 286, 03h 386, 04h 486, 05h Pentium). The value is
     the s79 measurement's -- stock answers CL > 3 to krnl386 -- not a claim about the
     host CPU, which is past a 486 several times over.
   ► 0400h BX: bit 0 = a 32-bit host (true: 32-bit clients run); bit 1 = interrupts are
     reflected to VIRTUAL 8086 mode (false: we report what 0400h always reported, and a
     client that cares reads it only as a hint); bit 2 = virtual memory (false: 0501h
     memory is committed at allocation).
   ► 0400h DX: DH = master PIC base 08h, DL = slave PIC base 70h -- where the IRQs land. */
#define DPMI_CPU_CLASS   0x04
#define DPMI_VER_AX      0x005A        /* AH = 0 major, AL = 90 (5Ah) minor -> "0.90"   */
#define DPMI_VER_BX      0x0001
#define DPMI_VER_DX      0x0870

/* ── IS THIS A SELECTOR THE CLIENT MAY NAME? (0001h, 0007h-000Ah, 0101h) ───────────────
     These all reported a bad selector as SUCCESS -- 0001h kept it and said nothing,
     0007h-0009h ignored the call, 000Ah aliased the code segment instead. A client that
     checks CF (most C runtimes do, through their int386x wrapper) then went on believing
     a descriptor had been set that never was; the fault arrives later, somewhere else.
   The spec's 8022h is "the selector is not a valid LDT selector, or is not allocated".
     Three things make one invalid here:
       - TI = 0: a GDT selector is never the client's;
       - index 0 (the null selector) or at/after the end of our table;
       - an index the host never allocated -- UNLESS THE GUEST OWNS THE TABLE.
   ★ THAT LAST EXCEPTION IS krnl386. Under WOW the client manages the LDT itself through
     NTVDM's vendor window (04F1h/04F2h, the descriptor shadow): it picks free indices
     out of its OWN free list, writes them, and calls 0007h/0008h/000Ch on them -- often
     before (or without) the 04F2h commit that would teach our table about them. Measured
     in s84 logs: 0007h on 0x2ef, 0xc97, 0xcaf ... none of which we handed out. There the
     allocation record is the guest's, not ours, and refusing would kill Win16 -- so in
     that mode only the range is checked. (`isGuestOwnedTable` = the shadow exists.) */
#define DPMI_SELECTOR_TI          4    /* the table indicator: 1 = the LDT         */
#define DPMI_SELECTOR_RPL_USER    3    /* requested privilege level 3: a client's  */
#define DPMI_SELECTOR_INDEX_SHIFT 3
/* A client's LDT selector for descriptor `index`, and the descriptor index of a selector. */
#define DPMI_LDT_SELECTOR(index)      (((index) << DPMI_SELECTOR_INDEX_SHIFT) | (DPMI_SELECTOR_TI | DPMI_SELECTOR_RPL_USER))
#define DPMI_SELECTOR_INDEX(selector) ((selector) >> DPMI_SELECTOR_INDEX_SHIFT)
/* The flags nibble (G, D/B, L, AVL) is bits 20-23 of a descriptor's high dword. */
#define DPMI_DESCRIPTOR_FLAGS_SHIFT 20
#define DPMI_DESCRIPTOR_FLAGS_MASK  0x0F

/* A PM INT 21h's transfer buffer (WOW only): one 1 KB window per pointer register, so an LFN
   call can carry DS:DX, DS:SI and ES:DI at once (PmInt21Lfn). */
#define PM_TRANSFER_WINDOW_SIZE      0x400
#define PM_TRANSFER_STRING_MAX       0x3FF   /* a string in, less room for its NUL        */
#define PM_TRANSFER_WINDOW_DX        0x000
#define PM_TRANSFER_WINDOW_SI        0x400
#define PM_TRANSFER_WINDOW_DI        0x800
/* How a pointer register's bytes move (PmInt21Lfn). */
#define PM_LFN_COPY_NONE             0       /* not a pointer for this call               */
#define PM_LFN_COPY_STRING_IN        1
#define PM_LFN_COPY_BLOCK_OUT        2       /* a block of `length` bytes out             */
#define PM_LFN_COPY_BLOCK_IN         3
#define PM_LFN_COPY_STRING_OUT       4       /* at most `length`, copied back to its NUL  */
/* PmLfnCopy's direction. */
#define PM_LFN_INTO_TRANSFER         1       /* guest -> transfer buffer                  */
#define PM_LFN_BACK_TO_GUEST         0       /* transfer buffer -> guest                  */

/* INT 31h functions (AX), as the dispatcher's comments name them; 04F1h/04F2h are NTVDM's own. */
#define DPMI_FN_ALLOCATE_DESCRIPTORS         0x0000
#define DPMI_FN_FREE_DESCRIPTOR              0x0001
#define DPMI_FN_SEGMENT_TO_DESCRIPTOR        0x0002
#define DPMI_FN_GET_SELECTOR_INCREMENT       0x0003
#define DPMI_FN_GET_SEGMENT_BASE             0x0006
#define DPMI_FN_SET_SEGMENT_BASE             0x0007
#define DPMI_FN_SET_SEGMENT_LIMIT            0x0008
#define DPMI_FN_SET_ACCESS_RIGHTS            0x0009
#define DPMI_FN_CREATE_ALIAS                 0x000A
#define DPMI_FN_GET_DESCRIPTOR               0x000B
#define DPMI_FN_SET_DESCRIPTOR               0x000C
#define DPMI_FN_ALLOCATE_SPECIFIC_DESCRIPTOR 0x000D
#define DPMI_FN_ALLOCATE_DOS_MEMORY          0x0100
#define DPMI_FN_FREE_DOS_MEMORY              0x0101
#define DPMI_FN_RESIZE_DOS_MEMORY            0x0102
#define DPMI_FN_GET_REAL_MODE_VECTOR         0x0200
#define DPMI_FN_SET_REAL_MODE_VECTOR         0x0201
#define DPMI_FN_GET_EXCEPTION_HANDLER        0x0202
#define DPMI_FN_SET_EXCEPTION_HANDLER        0x0203
#define DPMI_FN_GET_PROTECTED_MODE_VECTOR    0x0204
#define DPMI_FN_SET_PROTECTED_MODE_VECTOR    0x0205
#define DPMI_FN_SIMULATE_REAL_MODE_INTERRUPT 0x0300
#define DPMI_FN_CALL_REAL_MODE_FAR           0x0301
#define DPMI_FN_CALL_REAL_MODE_IRET          0x0302
#define DPMI_FN_ALLOCATE_CALLBACK            0x0303
#define DPMI_FN_FREE_CALLBACK                0x0304
#define DPMI_FN_GET_STATE_SAVE_ADDRESSES     0x0305
#define DPMI_FN_GET_RAW_SWITCH_ADDRESSES     0x0306
#define DPMI_FN_GET_VERSION                  0x0400
#define DPMI_FN_NTVDM_ALLOCATE               0x04F1
#define DPMI_FN_NTVDM_COMMIT                 0x04F2
#define DPMI_FN_GET_FREE_MEMORY_INFO         0x0500
#define DPMI_FN_ALLOCATE_MEMORY              0x0501
#define DPMI_FN_FREE_MEMORY                  0x0502
#define DPMI_FN_RESIZE_MEMORY                0x0503
#define DPMI_FN_LOCK_LINEAR_REGION           0x0600
#define DPMI_FN_UNLOCK_LINEAR_REGION         0x0601
#define DPMI_FN_UNLOCK_REAL_MODE_REGION      0x0602
#define DPMI_FN_RELOCK_REAL_MODE_REGION      0x0603
#define DPMI_FN_GET_PAGE_SIZE                0x0604
#define DPMI_FN_DISCARD_PAGES                0x0701
#define DPMI_FN_MARK_DEMAND_PAGING           0x0702
#define DPMI_FN_DISCARD_PAGE_CONTENTS        0x0703
#define DPMI_FN_MAP_PHYSICAL_ADDRESS         0x0800
#define DPMI_FN_FREE_PHYSICAL_MAPPING        0x0801
#define DPMI_FN_GET_AND_DISABLE_VI           0x0900
#define DPMI_FN_GET_AND_ENABLE_VI            0x0901
#define DPMI_FN_GET_VI_STATE                 0x0902
#define DPMI_FN_GET_VENDOR_API               0x0A00
static INT DpmiIsSelectorValid(WORD selector, INT indexLimit, INT isAllocated, INT isGuestOwnedTable)
{
    INT index = DPMI_SELECTOR_INDEX(selector);
    if (!(selector & DPMI_SELECTOR_TI)) return 0;    /* TI = 0: GDT          */
    if (index < 1 || index >= indexLimit) return 0;  /* null, or off the end */
    if (!isAllocated && !isGuestOwnedTable) return 0;   /* never handed out     */
    return 1;
}

/* ── REAL-MODE CALLBACKS: SIXTEEN, AND THEY CAN BE GIVEN BACK. (0303h/0304h) ───────────
     The spec has a host provide AT LEAST 16 callbacks per client; we had 4, and no 0304h,
     so a client that allocates and frees -- a mouse driver hook per mode set, a
     Ctrl-Break handler installed and removed -- ran out on its fifth and could never
     recover one. Each slot is a 4-byte real-mode entry `C4 C4 55 xx` (a BOP the host
     traps) in DOS_HDLR_SEG; the slot is named by WHERE the BOP executes, so the address
     IS the slot and 0304h decodes it back with no table lookup.
   ⚠ THEY MOVED, from 0x60..0x6F to DOS_HDLR_SEG:0x90..0xCF. Sixteen do not fit where four
     did (0x70 is the PM-return catcher, 0x74 the raw-switch entry), and 0x84..0xDF was the
     only 64-byte run the segment map had left. The old place was also borrowed by the
     opt-in entry trampoline (0x60..0x65) -- that collision is gone with the move.
   ► `ip` is the BOP's own address at the trap (the nested loops step past a BOP with
     `EIP += 3` themselves); an address inside the 4-byte stub maps to the same slot by
     the integer division, so a caller that has already stepped is not misread. */
#define DPMI_CB_SLOTS      16
#define DPMI_CB_STRIDE     4

static WORD DpmiCallbackEntry(WORD base, INT slot)
{
    return (WORD)(base + slot * DPMI_CB_STRIDE);
}
/* The slot whose BOP is executing at CS:IP, or -1. For the trap path. */
static INT DpmiCallbackSlotAt(WORD base, WORD codeSegment, WORD wantedSegment, WORD instructionPointer)
{
    INT slot;
    if (codeSegment != wantedSegment || instructionPointer < base) return -1;
    slot = (instructionPointer - base) / DPMI_CB_STRIDE;
    return (slot < DPMI_CB_SLOTS) ? slot : -1;
}
/* 0304h: the slot whose ADDRESS is exactly CX:DX, or -1. Stricter than the trap path:
   the client must hand back the address it was given, not something inside the stub. */
static INT DpmiCallbackSlotOf(WORD base, WORD codeSegment, WORD wantedSegment, WORD offset)
{
    INT slot = DpmiCallbackSlotAt(base, codeSegment, wantedSegment, offset);
    return (slot >= 0 && DpmiCallbackEntry(base, slot) == offset) ? slot : -1;
}

/* ── 0503h RESIZE MEMORY BLOCK: STAY PUT IF IT FITS, OTHERWISE MOVE AND COPY. ──────────
     The spec lets the host move the block (it returns a new linear address AND a new
     handle) and leaves fixing up descriptors to the client. A 0501h block here is one
     VirtualAlloc, committed to the page; it can grow in place only into pages it already
     has, because a reservation cannot be extended. So:
       new size 0                      -> 8021h (the spec's invalid value)
       new size <= committed bytes     -> in place, same address, same handle
       otherwise                       -> a new block, copy min(old, new), free the old.
     `committed` is the allocation's page-rounded size as the OS reports it. */
#define DPMI_RESIZE_BAD    0
#define DPMI_RESIZE_INPLACE 1
#define DPMI_RESIZE_MOVE   2
static INT DpmiResizePlan(UINT32 newSize, UINT32 committed, UINT32 *copy)
{
    if (copy) *copy = 0;
    if (newSize == 0) return DPMI_RESIZE_BAD;
    if (newSize <= committed) return DPMI_RESIZE_INPLACE;
    if (copy) *copy = committed;                     /* new > committed: all of the old */
    return DPMI_RESIZE_MOVE;
}

#endif /* NTVDMEX_DPMI_SVC_H */
