/* dpmisvc_test.c -- off-VM battery for src/host/dpmi_svc.h (GH #248): the INT 31h answers
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
 */
#include <stdio.h>
#include <stdint.h>

#include "../../src/host/dpmi_svc.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

#define HDLR   0x0050          /* DOS_HDLR_SEG -- passed in, so the test needs no layout header */
#define CBBASE 0x0090          /* DPMI_CB_BASE_OFF in main.c */
#define LDTMAX 2048

int main(void)
{
    int s, ok;
    printf("== DPMI INT 31h service rules (GH #248) ==\n");

    /* ---- 0400h / 1687h ---- */
    CHECK(DPMI_CPU_CLASS == 4, "CPU class is 4 -- the value 1687h has reported since s79");
    CHECK(DPMI_VER_AX == 0x005A, "0400h AX = 005Ah (version 0.90)");
    CHECK((DPMI_VER_DX >> 8) == 0x08 && (DPMI_VER_DX & 0xFF) == 0x70,
          "0400h DH = master PIC base 08h, DL = slave PIC base 70h");
    CHECK(DPMI_VER_BX & 1, "0400h BX bit 0: a 32-bit host");

    /* ---- the selector rule ---- */
    CHECK(!dpmi_sel_valid(0x0000, LDTMAX, 1, 0), "null selector: invalid");
    CHECK(!dpmi_sel_valid(0x0007, LDTMAX, 1, 0), "index 0 with TI/RPL bits: still the null descriptor");
    CHECK(!dpmi_sel_valid(0x0028, LDTMAX, 1, 0), "a GDT selector (TI=0): invalid even if 'allocated'");
    CHECK(!dpmi_sel_valid((LDTMAX << 3) | 7, LDTMAX, 1, 0), "the first index past the table: invalid");
    CHECK(dpmi_sel_valid(((LDTMAX - 1) << 3) | 7, LDTMAX, 1, 0), "the last index of the table: valid");
    CHECK(!dpmi_sel_valid(0x0057, LDTMAX, 0, 0), "an index we never allocated: invalid (8022h)");
    CHECK(dpmi_sel_valid(0x0057, LDTMAX, 1, 0), "an allocated index: valid");
    CHECK(dpmi_sel_valid(0x0054, LDTMAX, 1, 0), "RPL is not checked (0x54 = index 10, TI=1, RPL 0)");
    CHECK(dpmi_sel_valid(0x02EF, LDTMAX, 0, 1),
          "WOW: krnl386's own index (0x2ef, measured) is valid though we never allocated it");
    CHECK(!dpmi_sel_valid(0x0000, LDTMAX, 0, 1) && !dpmi_sel_valid((LDTMAX << 3) | 7, LDTMAX, 0, 1)
          && !dpmi_sel_valid(0x0028, LDTMAX, 0, 1),
          "WOW: the range is still checked (null, past the end, GDT)");

    /* ---- callbacks ---- */
    CHECK(DPMI_CB_SLOTS >= 16, "at least 16 callbacks (the spec's minimum; was 4)");
    ok = 1;
    for (s = 0; s < DPMI_CB_SLOTS; ++s) {
        uint16_t e = dpmi_cb_entry(CBBASE, s);
        if (dpmi_cb_slot_at(CBBASE, HDLR, HDLR, e) != s) ok = 0;          /* trap at the BOP   */
        if (dpmi_cb_slot_at(CBBASE, HDLR, HDLR, (uint16_t)(e + 3)) != s) ok = 0; /* past the BOP */
        if (dpmi_cb_slot_of(CBBASE, HDLR, HDLR, e) != s) ok = 0;          /* 0304h            */
    }
    CHECK(ok, "every slot's address decodes back to that slot (trap at BOP, past BOP, 0304h)");
    CHECK(dpmi_cb_entry(CBBASE, DPMI_CB_SLOTS - 1) + 3 <= 0xD0,
          "all 16 stubs end inside 0x90..0xCF, below MS_CB_RET_OFF (0xE0)");
    CHECK(dpmi_cb_entry(CBBASE, 0) > 0x82, "slot 0 is past the fault BOP (0x80-0x82)");
    CHECK(dpmi_cb_slot_of(CBBASE, HDLR, HDLR, (uint16_t)(CBBASE + 1)) < 0,
          "0304h: an address INSIDE a stub is not a callback address");
    CHECK(dpmi_cb_slot_of(CBBASE, 0x0051, HDLR, CBBASE) < 0,
          "0304h: the right offset in the wrong segment is refused");
    CHECK(dpmi_cb_slot_of(CBBASE, HDLR, HDLR, dpmi_cb_entry(CBBASE, DPMI_CB_SLOTS)) < 0,
          "0304h: the address one past the last slot is refused");
    CHECK(dpmi_cb_slot_at(CBBASE, HDLR, HDLR, (uint16_t)(CBBASE - 4)) < 0,
          "trap: below the first slot is not a callback (the fault BOP and friends)");
    CHECK(dpmi_cb_slot_at(CBBASE, HDLR, HDLR, 0xE0) < 0,
          "trap: the INT 33h return stub at 0xE0 is not a callback");

    /* ---- 0503h plan ---- */
    { uint32_t cp = 77;
      CHECK(dpmi_resize_plan(0, 0x1000, &cp) == DPMI_RESIZE_BAD && cp == 0, "0503h: size 0 is 8021h");
      CHECK(dpmi_resize_plan(0x800, 0x1000, &cp) == DPMI_RESIZE_INPLACE, "0503h: shrink stays put");
      CHECK(dpmi_resize_plan(0x1000, 0x1000, &cp) == DPMI_RESIZE_INPLACE,
            "0503h: growing into pages the block already has stays put");
      CHECK(dpmi_resize_plan(0x1001, 0x1000, &cp) == DPMI_RESIZE_MOVE && cp == 0x1000,
            "0503h: one byte past the committed pages moves, copying all of the old block");
      CHECK(dpmi_resize_plan(0x40000, 0x3000, &cp) == DPMI_RESIZE_MOVE && cp == 0x3000,
            "0503h: a large grow copies exactly the old committed size"); }

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
