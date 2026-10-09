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

static INT g_Total = 0, g_Failures = 0;
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

#define HDLR    0x0050  /* DOS_HDLR_SEG -- passed in, so the test needs no layout header */
#define CBBASE  0x0090  /* DPMI_CB_BASE_OFF in main.c */
#define LDTMAX  2048

INT main(VOID)
{
    INT slot, isOk;
    printf("== DPMI INT 31h service rules (GH #248) ==\n");

    /* ---- 0400h / 1687h ---- */
    CHECK(DPMI_CPU_CLASS == 4, "CPU class is 4 -- the value 1687h has reported since s79");
    CHECK(DPMI_VERSION_090 == 0x005A, "0400h AX = 005Ah (version 0.90)");
    CHECK((DPMI_VER_DX >> 8) == 0x08 && (DPMI_VER_DX & 0xFF) == 0x70,
          "0400h DH = master PIC base 08h, DL = slave PIC base 70h");
    CHECK(DPMI_VER_BX & 1, "0400h BX bit 0: a 32-bit host");

    /* ---- the selector rule ---- */
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

    /* ---- callbacks ---- */
    CHECK(DPMI_CB_SLOTS >= 16, "at least 16 callbacks (the spec's minimum; was 4)");
    isOk = 1;
    for (slot = 0; slot < DPMI_CB_SLOTS; ++slot)
    {
        WORD entry = DpmiCallbackEntry(CBBASE, slot);
        if (DpmiCallbackSlotAt(CBBASE, HDLR, HDLR, entry) != slot) isOk = 0;          /* trap at the BOP */
        if (DpmiCallbackSlotAt(CBBASE, HDLR, HDLR, (WORD)(entry + 3)) != slot) isOk = 0; /* past the BOP */
        if (DpmiCallbackSlotOf(CBBASE, HDLR, HDLR, entry) != slot) isOk = 0;          /* 0304h */
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

    /* ---- 0503h plan ---- */
    { UINT32 copySize = 77;
      CHECK(DpmiResizePlan(0, 0x1000, &copySize) == DPMI_RESIZE_BAD && copySize == 0, "0503h: size 0 is 8021h");
      CHECK(DpmiResizePlan(0x800, 0x1000, &copySize) == DPMI_RESIZE_INPLACE, "0503h: shrink stays put");
      CHECK(DpmiResizePlan(0x1000, 0x1000, &copySize) == DPMI_RESIZE_INPLACE,
            "0503h: growing into pages the block already has stays put");
      CHECK(DpmiResizePlan(0x1001, 0x1000, &copySize) == DPMI_RESIZE_MOVE && copySize == 0x1000,
            "0503h: one byte past the committed pages moves, copying all of the old block");
      CHECK(DpmiResizePlan(0x40000, 0x3000, &copySize) == DPMI_RESIZE_MOVE && copySize == 0x3000,
            "0503h: a large grow copies exactly the old committed size"); }

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
