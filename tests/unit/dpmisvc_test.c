/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for src/host/dpmi_svc.h (GH #248): the INT 31h answers
 * the DPMI specification decides.
 *
 * WHAT IS PINNED, AND WHY EACH ONE IS HERE.
 *   - 0400h and 1687h describe ONE machine: the CPU class both report is one constant,
 *     and it is the s79 value (4). #248 was 0400h still saying 3.
 *   - The selector rule: GDT, null, off-the-table and never-allocated are all 8022h --
 *     and under WOW (the guest owns the table) only the RANGE is ours to check, because
 *     krnl386 calls 0007h on indices it allocated itself (measured, s84 logs).
 *   - Sixteen callbacks, at sixteen distinct addresses inside DOS_HDLR_SEG, each decoded
 *     back to its own slot by BOTH the trap path and 0304h; 0304h refuses an address
 *     that is merely inside a stub, in another segment, or past the last slot.
 *   - The 0503h plan: size 0 is invalid, a size that fits the committed pages stays put,
 *     a bigger one moves and copies all of the old block.
 *
 *   cc -std=c99 -I src/host -o dpmisvc_test tests/unit/dpmisvc_test.c && ./dpmisvc_test
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <stdint.h>
#include "../../src/host/dpmi_svc.h"
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

#define HDLR    0x0050  /* DOS_HDLR_SEG -- passed in, so the test needs no layout header */
#define CBBASE  0x0090  /* DPMI_CB_BASE_OFF in main.c */
#define LDTMAX  2048

static INT g_Total = 0;
static INT g_Failures = 0;

INT main(VOID)
{
    INT slot;
    INT isOk;

    printf("== DPMI INT 31h service rules (GH #248) ==\n");

    /* 0400h / 1687h: */
    CHECK(DPMI_CPU_CLASS == 4, "CPU class is 4 -- the value 1687h has reported since s79");
    CHECK(DPMI_VERSION_090 == 0x005A, "0400h AX = 005Ah (version 0.90)");
    CHECK((DPMI_VER_DX >> 8) == 0x08 && (DPMI_VER_DX & 0xFF) == 0x70,
          "0400h DH = master PIC base 08h, DL = slave PIC base 70h");
    CHECK(DPMI_VER_BX & 1, "0400h BX bit 0: a 32-bit host");

    /* the selector rule: */
    CHECK(!DpmiIsSelectorValid(0x0000, LDTMAX, 1, 0), "null selector: invalid");
    CHECK(!DpmiIsSelectorValid(0x0007, LDTMAX, 1, 0), "index 0 with TI/RPL bits: still the null descriptor");
    CHECK(!DpmiIsSelectorValid(0x0028, LDTMAX, 1, 0), "a GDT selector (TI=0): invalid even if 'allocated'");
    CHECK(!DpmiIsSelectorValid((LDTMAX << 3) | 7, LDTMAX, 1, 0), "the first index past the table: invalid");
    CHECK(DpmiIsSelectorValid(((LDTMAX - 1) << 3) | 7, LDTMAX, 1, 0), "the last index of the table: valid");
    CHECK(!DpmiIsSelectorValid(0x0057, LDTMAX, 0, 0), "an index we never allocated: invalid (8022h)");
    CHECK(DpmiIsSelectorValid(0x0057, LDTMAX, 1, 0), "an allocated index: valid");
    CHECK(DpmiIsSelectorValid(0x0054, LDTMAX, 1, 0), "RPL is not checked (0x54 = index 10, TI=1, RPL 0)");
    CHECK(DpmiIsSelectorValid(0x02EF, LDTMAX, 0, 1),
          "WOW: krnl386's own index (0x2ef, measured) is valid though we never allocated it");
    CHECK(!DpmiIsSelectorValid(0x0000, LDTMAX, 0, 1) && !DpmiIsSelectorValid((LDTMAX << 3) | 7, LDTMAX, 0, 1)
          && !DpmiIsSelectorValid(0x0028, LDTMAX, 0, 1),
          "WOW: the range is still checked (null, past the end, GDT)");

    /* callbacks: */
    CHECK(DPMI_CB_SLOTS >= 16, "at least 16 callbacks (the spec's minimum; was 4)");
    isOk = 1;

    for (slot = 0; slot < DPMI_CB_SLOTS; ++slot)
    {
        WORD entry = DpmiCallbackEntry(CBBASE, slot);

        if (DpmiCallbackSlotAt(CBBASE, HDLR, HDLR, entry) != slot)
            isOk = 0;                                                                 /* trap at the BOP */

        if (DpmiCallbackSlotAt(CBBASE, HDLR, HDLR, (WORD)(entry + 3)) != slot)
            isOk = 0;                                                                    /* past the BOP */

        if (DpmiCallbackSlotOf(CBBASE, HDLR, HDLR, entry) != slot)
            isOk = 0;                                                                 /* 0304h */
    }

    CHECK(isOk, "every slot's address decodes back to that slot (trap at BOP, past BOP, 0304h)");
    CHECK(DpmiCallbackEntry(CBBASE, DPMI_CB_SLOTS - 1) + 3 <= 0xD0,
          "all 16 stubs end inside 0x90..0xCF, below MS_CB_RET_OFF (0xE0)");
    CHECK(DpmiCallbackEntry(CBBASE, 0) > 0x82, "slot 0 is past the fault BOP (0x80-0x82)");
    CHECK(DpmiCallbackSlotOf(CBBASE, HDLR, HDLR, (WORD)(CBBASE + 1)) < 0,
          "0304h: an address INSIDE a stub is not a callback address");
    CHECK(DpmiCallbackSlotOf(CBBASE, 0x0051, HDLR, CBBASE) < 0,
          "0304h: the right offset in the wrong segment is refused");
    CHECK(DpmiCallbackSlotOf(CBBASE, HDLR, HDLR, DpmiCallbackEntry(CBBASE, DPMI_CB_SLOTS)) < 0,
          "0304h: the address one past the last slot is refused");
    CHECK(DpmiCallbackSlotAt(CBBASE, HDLR, HDLR, (WORD)(CBBASE - 4)) < 0,
          "trap: below the first slot is not a callback (the fault BOP and friends)");
    CHECK(DpmiCallbackSlotAt(CBBASE, HDLR, HDLR, 0xE0) < 0,
          "trap: the INT 33h return stub at 0xE0 is not a callback");

    /* 0503h plan: */
    { UINT32 copySize = 77;
      CHECK(DpmiResizePlan(0, 0x1000, &copySize) == DPMI_RESIZE_BAD && copySize == 0, "0503h: size 0 is 8021h");
      CHECK(DpmiResizePlan(0x800, 0x1000, &copySize) == DPMI_RESIZE_INPLACE, "0503h: shrink stays put");
      CHECK(DpmiResizePlan(0x1000, 0x1000, &copySize) == DPMI_RESIZE_INPLACE,
            "0503h: growing into pages the block already has stays put");
      CHECK(DpmiResizePlan(0x1001, 0x1000, &copySize) == DPMI_RESIZE_MOVE && copySize == 0x1000,
            "0503h: one byte past the committed pages moves, copying all of the old block");
      CHECK(DpmiResizePlan(0x40000, 0x3000, &copySize) == DPMI_RESIZE_MOVE && copySize == 0x3000,
            "0503h: a large grow copies exactly the old committed size"); }

    /* #268: values, not just selectors. */
    CHECK(DpmiLimitOk(0) && DpmiLimitOk(0xFFFFF), "0008h: any byte limit up to 1 MB");
    CHECK(DpmiLimitOk(0x100FFF) && DpmiLimitOk(0xFFFFFFFF), "0008h: above 1 MB with the low 12 bits set (flat 4 GB too)");
    CHECK(!DpmiLimitOk(0x100000) && !DpmiLimitOk(0x123456), "0008h: above 1 MB without them -> 8021h");
    CHECK(DpmiAccessOk(0x00F2) && DpmiAccessOk(0x40FA), "0009h: data, and 32-bit readable code, pass");
    CHECK(DpmiAccessOk(0x8092), "0009h: ZAR's 8092h (DPL 0) passes -- DPL is not enforced");
    CHECK(!DpmiAccessOk(0x00E2), "0009h: S = 0 (a system descriptor) -> 8021h");
    CHECK(!DpmiAccessOk(0x00FE), "0009h: conforming code -> 8021h");
    CHECK(DpmiAccessOk(0x00F6), "0009h: expand-down DATA (bit 2 without code) passes");
    CHECK(!DpmiAccessOk(0x20F2), "0009h: CH bit 5 set -> 8021h");
    CHECK(DpmiBaseOk(0, DPMI_XP_LDT_MAX_LINEAR) && DpmiBaseOk(DPMI_XP_LDT_MAX_LINEAR, DPMI_XP_LDT_MAX_LINEAR),
          "0007h: a base up to the cap");
    CHECK(!DpmiBaseOk(0x80000000u, DPMI_XP_LDT_MAX_LINEAR), "0007h: a base past the cap -> 8025h");

    /* 0100h chains: */
    { DWORD baseOffset;
    DWORD limit;
      CHECK(DpmiDosChainCount(0) == 1 && DpmiDosChainCount(0x1000) == 1 && DpmiDosChainCount(0x1001) == 2
            && DpmiDosChainCount(0x2800) == 3, "0100h: one descriptor per started 64 KB");
      DpmiDosChainDescriptor(0x2800, 0, &baseOffset, &limit);
      CHECK(baseOffset == 0 && limit == 0x27FFF, "0100h: the first spans the whole block (a 32-bit host)");
      DpmiDosChainDescriptor(0x2800, 1, &baseOffset, &limit);
      CHECK(baseOffset == 0x10000 && limit == 0xFFFF, "0100h: the next is +64 KB, 64 KB long");
      DpmiDosChainDescriptor(0x2800, 2, &baseOffset, &limit);
      CHECK(baseOffset == 0x20000 && limit == 0x7FFF, "0100h: the last holds the remainder");
      DpmiDosChainDescriptor(0, 0, &baseOffset, &limit);
      CHECK(limit == 0, "0100h: BX 0 is one descriptor, limit 0"); }

    /* 0500h: */
    { DWORD info[DPMI_FREE_INFO_DWORDS];
      DpmiFreeMemoryInfo(info, 0x4000, 0);
      CHECK(info[DPMI_FREE_INFO_LARGEST_BLOCK] == 0x4000000 && info[DPMI_FREE_INFO_FREE] == 0x4000,
            "0500h: nothing allocated -> the whole pool is free");
      DpmiFreeMemoryInfo(info, 0x4000, 0x100);
      CHECK(info[DPMI_FREE_INFO_FREE] == 0x3F00 && info[DPMI_FREE_INFO_LARGEST_BLOCK] == 0x3F00000
            && info[DPMI_FREE_INFO_MAX_UNLOCKED] == 0x3F00 && info[DPMI_FREE_INFO_LINEAR_FREE] == 0x3F00,
            "0500h: the free counts drop by what 0501h holds");
      CHECK(info[DPMI_FREE_INFO_LINEAR_TOTAL] == 0x4000 && info[DPMI_FREE_INFO_PHYSICAL] == 0x4000,
            "0500h: the totals stay");
      CHECK(info[DPMI_FREE_INFO_PAGING_FILE] == 0 && info[9] == 0xFFFFFFFFu && info[11] == 0xFFFFFFFFu,
            "0500h: no paging file; the reserved DWORDs are -1");
      DpmiFreeMemoryInfo(info, 0x4000, 0x5000);
      CHECK(info[DPMI_FREE_INFO_FREE] == 0, "0500h: holding more than the pool reads as none free, not a wrap"); }

    /* 0306h raw switch: */
    { DPMI_RAW_SWITCH target;
      DpmiRawSwitchPlan(0x1111, 0x12345678, 0x2222, 0x3333, 0x4444, 0x9ABCDEF0, 0, &target);
      CHECK(target.Ds == 0x1111 && target.Es == 0x2222 && target.Ss == 0x3333 && target.Cs == 0x4444,
            "0306h: AX/CX/DX/SI -> DS/ES/SS/CS");
      CHECK(target.Esp == 0x5678 && target.Eip == 0xDEF0, "0306h: into real mode, SP and IP are 16 bits");
      DpmiRawSwitchPlan(0x1111, 0x12345678, 0x2222, 0x3333, 0x4444, 0x9ABCDEF0, 1, &target);
      CHECK(target.Esp == 0x12345678 && target.Eip == 0x9ABCDEF0, "0306h: a 32-bit client's EBX/EDI full width"); }

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
