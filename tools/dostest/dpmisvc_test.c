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
 *   cc -std=c99 -I src/host -o dpmisvc_test tools/dostest/dpmisvc_test.c && ./dpmisvc_test
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

    /* ---- #268: 0008h limit ---- */
    CHECK(dpmi_limit_ok(0) && dpmi_limit_ok(0xFFFF) && dpmi_limit_ok(0xFFFFF),
          "0008h: any limit up to 1 MB is byte-granular and legal");
    CHECK(!dpmi_limit_ok(0x100000), "0008h: 1 MB exactly (low 12 bits clear) is 8021h");
    CHECK(!dpmi_limit_ok(0x123456), "0008h: > 1 MB with low 12 bits 456h is 8021h");
    CHECK(!dpmi_limit_ok(0x1FFFFE), "0008h: > 1 MB with low 12 bits FFEh is 8021h (all twelve, not 'some')");
    CHECK(dpmi_limit_ok(0x1FFFFF) && dpmi_limit_ok(0x4AFFF0 | 0xF),
          "0008h: > 1 MB with low 12 bits all set is legal");
    CHECK(dpmi_limit_ok(0xFFFFFFFFu), "0008h: DOS/4GW's flat 4 GB passes (the LDT cap clamp is separate)");

    /* ---- #268: 0009h access word ---- */
    CHECK(dpmi_access_ok(0x00F2) && dpmi_access_ok(0x00FA) && dpmi_access_ok(0x00FB),
          "0009h: DPL-3 data RW, code ER, code ER accessed: legal");
    CHECK(dpmi_access_ok(0x40FA) && dpmi_access_ok(0xC0F2) && dpmi_access_ok(0xCFF2),
          "0009h: CH with G / B/D and a limit nibble: legal");
    CHECK(dpmi_access_ok(0x8092), "0009h: ZAR's 8092h (DPL 0) PASSES -- the deliberate DPL deviation");
    CHECK(dpmi_access_ok(0x0072), "0009h: not present (P=0) is legal");
    CHECK(dpmi_access_ok(0x00F6), "0009h: expand-down data is legal");
    CHECK(dpmi_access_ok(0x10F2), "0009h: CH bit 4 (AVL) is legal");
    CHECK(!dpmi_access_ok(0x00E2), "0009h: S = 0 (a system descriptor) is 8021h");
    CHECK(!dpmi_access_ok(0x00FE), "0009h: conforming code is 8021h");
    CHECK(!dpmi_access_ok(0x20F2), "0009h: CH bit 5 set is 8021h");

    /* ---- #268: 0007h base ---- */
    CHECK(dpmi_base_ok(0, 0x7FFEFFFFu) && dpmi_base_ok(0x7FFEFFFFu, 0x7FFEFFFFu),
          "0007h: base 0 and base = the cap are legal");
    CHECK(!dpmi_base_ok(0x7FFF0000u, 0x7FFEFFFFu) && !dpmi_base_ok(0xFFFFF000u, 0x7FFEFFFFu),
          "0007h: a base past the cap is 8025h");

    /* ---- #268: 0100h chains ---- */
    { uint32_t off, lim;
      CHECK(dpmi_dosmem_count(0) == 1 && dpmi_dosmem_count(1) == 1 && dpmi_dosmem_count(0x1000) == 1,
            "0100h: up to 64 KB (BX <= 1000h) is one descriptor");
      CHECK(dpmi_dosmem_count(0x1001) == 2 && dpmi_dosmem_count(0x2000) == 2 && dpmi_dosmem_count(0x2001) == 3,
            "0100h: one descriptor per started 64 KB");
      CHECK(dpmi_dosmem_count(0xA000) == 10, "0100h: 640 KB is ten descriptors");
      dpmi_dosmem_desc(0x10, 0, &off, &lim);
      CHECK(off == 0 && lim == 0xFF, "0100h: 16 paragraphs -> base +0, limit FFh");
      dpmi_dosmem_desc(0x2000, 0, &off, &lim);
      CHECK(off == 0 && lim == 0x1FFFF, "0100h 128 KB: the first descriptor spans the whole block (32-bit host)");
      dpmi_dosmem_desc(0x2000, 1, &off, &lim);
      CHECK(off == 0x10000 && lim == 0xFFFF, "0100h 128 KB: the second is +64 KB, limit FFFFh");
      dpmi_dosmem_desc(0x2800, 2, &off, &lim);
      CHECK(off == 0x20000 && lim == 0x7FFF, "0100h 160 KB: the last holds the remainder (32 KB)");
      dpmi_dosmem_desc(0x1001, 1, &off, &lim);
      CHECK(off == 0x10000 && lim == 0xF, "0100h 64 KB + 1 paragraph: the last is 16 bytes");
      dpmi_dosmem_desc(0, 0, &off, &lim);
      CHECK(off == 0 && lim == 0, "0100h: zero paragraphs keeps limit 0"); }

    /* ---- #268: 0500h ---- */
    { uint32_t mi[DPMI_MEMINFO_DWORDS]; int i, res = 1;
      dpmi_meminfo(mi, 0x4000, 0);
      CHECK(mi[0] == 0x04000000u && mi[1] == 0x4000 && mi[2] == 0x4000 && mi[5] == 0x4000 && mi[7] == 0x4000,
            "0500h idle: largest free = 64 MB in bytes, the page maxima and free counts = the pool");
      CHECK(mi[3] == 0x4000 && mi[4] == 0x4000 && mi[6] == 0x4000, "0500h: totals = the pool");
      CHECK(mi[8] == 0, "0500h: paging file size 0 (no virtual memory), not -1");
      for (i = 9; i < DPMI_MEMINFO_DWORDS; ++i) if (mi[i] != 0xFFFFFFFFu) res = 0;
      CHECK(res && DPMI_MEMINFO_DWORDS * 4 == 0x30, "0500h: +24..+2F reserved = FFFFFFFFh, 30h bytes in all");
      dpmi_meminfo(mi, 0x4000, 0x100);
      CHECK(mi[0] == (0x4000u - 0x100) << 12 && mi[5] == 0x3F00 && mi[1] == 0x3F00 && mi[7] == 0x3F00,
            "0500h: 1 MB held -> every FREE field drops by 100h pages (the defect: they never moved)");
      CHECK(mi[3] == 0x4000 && mi[4] == 0x4000 && mi[6] == 0x4000, "0500h: ...and the TOTALS do not");
      dpmi_meminfo(mi, 0x4000, 0x5000);
      CHECK(mi[0] == 0 && mi[5] == 0, "0500h: more held than the nominal pool -> free 0, not a wrapped number"); }

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
