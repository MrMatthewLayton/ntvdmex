/* rmcs_test.c -- off-VM battery for src/host/dpmi_rmcs.h (GH #247): the DPMI real-mode
 * call structure and how INT 31h 0300h routes a vector.
 *
 * WHAT IS PINNED, AND WHY EACH ONE IS HERE.
 *   - The layout, field by field, with a DIFFERENT value in every byte: a swapped pair
 *     of offsets (EBX/EDX, ES/DS) passes any test that fills both with the same thing.
 *   - The write-back covers EVERY output -- 32-bit general registers, FLAGS, ES DS FS GS.
 *     #247 was precisely a write-back that dropped BP, ES and DS, and wrote the low
 *     words only.
 *   - ...and NOTHING ELSE: CS, IP, SS, SP and the reserved dword are left exactly as the
 *     client put them. The spec says they "are not modified", and the pre-#247 0300h
 *     retarget broke that by storing IVT[BL] into the caller's CS:IP.
 *   - The routing table: INT 21h is always host-side; 33h/10h only while the IVT holds
 *     our stub; every other vector -- ours or a guest's -- RUNS; a null vector and the
 *     simintrefl_off.flag rollback do not.
 *   - The CX stack-copy plan, including the refusal that keeps a stale CX harmless and
 *     SP = 0 meaning a full 64 KB.
 *
 *   cc -std=c99 -I src/host -o rmcs_test tools/dostest/rmcs_test.c && ./rmcs_test
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../../src/host/dpmi_rmcs.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

#define OURS 0x0050          /* DOS_HDLR_SEG -- passed in, so the test needs no layout header */

static void fill_pattern(uint8_t *b, unsigned n) { unsigned i; for (i = 0; i < n; ++i) b[i] = (uint8_t)(0x11 + i * 7); }

int main(void)
{
    printf("== DPMI real-mode call structure (GH #247) ==\n");

    CHECK(RMCS_SIZE == 50, "the structure is 50 bytes");
    CHECK(RMCS_EDI == 0x00 && RMCS_ESI == 0x04 && RMCS_EBP == 0x08 && RMCS_EBX == 0x10
          && RMCS_EDX == 0x14 && RMCS_ECX == 0x18 && RMCS_EAX == 0x1C,
          "general registers at 00/04/08/10/14/18/1C (0C reserved)");
    CHECK(RMCS_FLAGS == 0x20 && RMCS_ES == 0x22 && RMCS_DS == 0x24 && RMCS_FS == 0x26
          && RMCS_GS == 0x28 && RMCS_IP == 0x2A && RMCS_CS == 0x2C && RMCS_SP == 0x2E
          && RMCS_SS == 0x30, "FLAGS ES DS FS GS IP CS SP SS at 20..30");

    /* ---- read: every field, full width, little-endian, any alignment ---- */
    { uint8_t buf[RMCS_SIZE + 1]; rmcs_regs g; uint8_t *r = buf + 1;   /* odd address */
      fill_pattern(buf, sizeof buf);
      rmcs_read(r, &g);
      CHECK(g.edi == (uint32_t)(r[0] | r[1] << 8 | r[2] << 16 | (uint32_t)r[3] << 24), "EDI read as a full dword");
      CHECK(g.eax == (uint32_t)(r[0x1C] | r[0x1D] << 8 | r[0x1E] << 16 | (uint32_t)r[0x1F] << 24),
            "EAX read as a full dword (the top half is an input on a 386)");
      CHECK(g.ebx != g.edx && g.ebx == (uint32_t)(r[0x10] | r[0x11] << 8 | r[0x12] << 16 | (uint32_t)r[0x13] << 24),
            "EBX from +10, not +14");
      CHECK(g.es == (uint16_t)(r[0x22] | r[0x23] << 8) && g.ds == (uint16_t)(r[0x24] | r[0x25] << 8),
            "ES from +22, DS from +24");
      CHECK(g.fs == (uint16_t)(r[0x26] | r[0x27] << 8) && g.gs == (uint16_t)(r[0x28] | r[0x29] << 8),
            "FS from +26, GS from +28 (they were never read: the 0301 arm set FS=GS=SS)");
      CHECK(g.flags == (uint16_t)(r[0x20] | r[0x21] << 8), "FLAGS from +20"); }

    /* ---- write: every output, and nothing that is not one ---- */
    { uint8_t r[RMCS_SIZE], before[RMCS_SIZE]; rmcs_regs g;
      fill_pattern(r, sizeof r); memcpy(before, r, sizeof r);
      g.edi = 0xD1D2D3D4u; g.esi = 0x51525354u; g.ebp = 0xB1B2B3B4u; g.ebx = 0x0B0C0D0Eu;
      g.edx = 0xDDCCBBAAu; g.ecx = 0xC0C1C2C3u; g.eax = 0xA0A1A2A3u;
      g.flags = 0x0247; g.es = 0xE5E5; g.ds = 0xD5D5; g.fs = 0xF5F5; g.gs = 0x6565;
      rmcs_write(r, &g);
      CHECK(rmcs_rd32(r, RMCS_EBP) == 0xB1B2B3B4u, "EBP written back (pre-#247 0300h dropped it)");
      CHECK(rmcs_rd16(r, RMCS_ES) == 0xE5E5 && rmcs_rd16(r, RMCS_DS) == 0xD5D5,
            "ES and DS written back (pre-#247 0300h dropped both -- AH=35h's ES:BX came back as the caller's ES)");
      CHECK(rmcs_rd16(r, RMCS_FS) == 0xF5F5 && rmcs_rd16(r, RMCS_GS) == 0x6565, "FS and GS written back");
      CHECK(rmcs_rd32(r, RMCS_EAX) == 0xA0A1A2A3u && rmcs_rd32(r, RMCS_EBX) == 0x0B0C0D0Eu
            && rmcs_rd32(r, RMCS_ECX) == 0xC0C1C2C3u && rmcs_rd32(r, RMCS_EDX) == 0xDDCCBBAAu
            && rmcs_rd32(r, RMCS_ESI) == 0x51525354u && rmcs_rd32(r, RMCS_EDI) == 0xD1D2D3D4u,
            "all seven general registers written back at full width");
      CHECK(rmcs_rd16(r, RMCS_FLAGS) == 0x0247, "FLAGS written back (CF is the DOS answer)");
      CHECK(memcmp(r + RMCS_IP, before + RMCS_IP, 8) == 0,
            "IP, CS, SP, SS NOT modified -- the spec's rule; the old 0300 retarget wrote CS:IP");
      CHECK(memcmp(r + 0x0C, before + 0x0C, 4) == 0, "the reserved dword at +0C is not touched");
      { rmcs_regs h; rmcs_read(r, &h);
        CHECK(memcmp(&g, &h, sizeof g) == 0, "write then read is the identity"); } }

    /* ---- 0300h routing ---- */
    CHECK(simint_route(0x21, OURS, 0x0000, 1, OURS) == SIMINT_FAST, "21h, ours: host-side");
    CHECK(simint_route(0x21, 0x1234, 0x0010, 1, OURS) == SIMINT_FAST,
          "21h, HOOKED by a guest: still host-side (the documented deviation, kept for the shelf)");
    CHECK(simint_route(0x33, OURS, 0x0030, 1, OURS) == SIMINT_FAST, "33h, ours: host-side (Doom's mouse)");
    CHECK(simint_route(0x10, OURS, 0x0020, 1, OURS) == SIMINT_FAST, "10h, ours: host-side (ZAR's int86 video)");
    CHECK(simint_route(0x33, 0x2000, 0x0100, 1, OURS) == SIMINT_RUN, "33h hooked by a guest driver: RUN it");
    CHECK(simint_route(0x10, 0xC000, 0x1234, 1, OURS) == SIMINT_RUN, "10h hooked (a VESA TSR): RUN it");
    CHECK(simint_route(0x16, OURS, 0x0028, 1, OURS) == SIMINT_RUN,
          "16h, OUR stub: RUN it (pre-#247: echoed with CF=0)");
    CHECK(simint_route(0x1A, OURS, 0x003C, 1, OURS) == SIMINT_RUN, "1Ah, our stub: RUN it");
    CHECK(simint_route(0x15, 0x0090, 0x0010, 1, OURS) == SIMINT_RUN,
          "15h, our BIOS stub at DOS_CTAB_SEG: RUN it");
    CHECK(simint_route(0x66, 0x34D3, 0x01D1, 1, OURS) == SIMINT_RUN, "66h, ZAR's Miles trampoline: RUN it");
    CHECK(simint_route(0x66, 0x0000, 0x0000, 1, OURS) == SIMINT_NONE, "a NULL vector is never executed");
    CHECK(simint_route(0x33, 0x0000, 0x0000, 1, OURS) == SIMINT_NONE,
          "a null 33h is neither ours nor runnable: NOT run, never 0:0");
    CHECK(simint_route(0x16, OURS, 0x0028, 0, OURS) == SIMINT_NONE
          && simint_route(0x66, 0x34D3, 0x01D1, 0, OURS) == SIMINT_NONE,
          "simintrefl_off.flag: nothing but 21/33/10 runs (pre-#247 routing)");
    CHECK(simint_route(0x33, 0x2000, 0x0100, 0, OURS) == SIMINT_FAST
          && simint_route(0x10, 0xC000, 0x1234, 0, OURS) == SIMINT_FAST,
          "simintrefl_off.flag: 33h/10h host-side whoever owns them (pre-#247)");

    /* ---- CX words of stack ---- */
    { uint16_t sp;
      CHECK(rmcs_stack_plan(0xFF00, 0, 6, &sp) && sp == 0xFF00, "CX=0: nothing copied, SP unchanged");
      CHECK(rmcs_stack_plan(0xFF00, 3, 6, &sp) && sp == 0xFEFA, "CX=3: six bytes below SP");
      CHECK(!rmcs_stack_plan(0x0100, 0x80, 6, &sp) && sp == 0x0100,
            "CX that does not fit: refused, SP unchanged (the call runs as before #247)");
      CHECK(rmcs_stack_plan(0x0106, 0x80, 6, &sp) && sp == 0x0006, "exactly fits with the frame below");
      CHECK(!rmcs_stack_plan(0x0105, 0x80, 6, &sp), "one byte short: refused");
      CHECK(rmcs_stack_plan(0x0000, 4, 6, &sp) && sp == 0xFFF8, "SP=0 is a full 64 KB, not an empty stack");
      CHECK(!rmcs_stack_plan(0xFF00, 0xFFFF, 6, &sp), "CX=FFFFh (128 KB) is never copied"); }

    /* ---- #267: the 0303h callback entry and exit ---- */
    { volatile uint8_t r[RMCS_SIZE + 2];
      rmcs_regs in, back;
      unsigned k;
      for (k = 0; k < sizeof r; ++k) r[k] = 0xEE;
      in.edi = 0x11223344; in.esi = 0x55667788; in.ebp = 0x99AABBCC; in.ebx = 0x01020304;
      in.edx = 0x05060708; in.ecx = 0x090A0B0C; in.eax = 0x0D0E0F10;
      in.flags = 0x0247; in.es = 0x1111; in.ds = 0x2222; in.fs = 0x3333; in.gs = 0x4444;
      rmcs_cb_enter(r, &in, 0x0050, 0x0094, 0x9000, 0xFFF0);
      rmcs_read(r, &back);
      CHECK(back.edi == in.edi && back.esi == in.esi && back.ebp == in.ebp && back.ebx == in.ebx
            && back.edx == in.edx && back.ecx == in.ecx && back.eax == in.eax,
            "cb entry: every general register, all 32 bits (the old fill wrote low words only)");
      CHECK(back.flags == 0x0247 && back.es == 0x1111 && back.ds == 0x2222
            && back.fs == 0x3333 && back.gs == 0x4444,
            "cb entry: FLAGS and ES DS FS GS (FS/GS were not filled at all)");
      CHECK(rmcs_rd16(r, RMCS_SS) == 0x9000 && rmcs_rd16(r, RMCS_SP) == 0xFFF0,
            "cb entry: SS:SP is the stack AT the call -- NOT popped (#267: the old code added 4)");
      CHECK(rmcs_rd16(r, RMCS_CS) == 0x0050 && rmcs_rd16(r, RMCS_IP) == 0x0094,
            "cb entry: CS:IP names the callback itself (the old code stored the far return)");
      CHECK(r[0x0C] == 0xEE && r[0x0D] == 0xEE && r[0x0E] == 0xEE && r[0x0F] == 0xEE
            && r[RMCS_SIZE] == 0xEE,
            "cb entry: the reserved dword and the byte past the structure are untouched");
      CHECK(rmcs_cb_v86_flags(0x0000) == 0x20202u, "cb exit: VM and IF always set");
      CHECK(rmcs_cb_v86_flags(0x0CD5) == (0x20202u | 0x0CD5u),
            "cb exit: CF PF AF ZF SF DF OF are the procedure's (CF was dropped before #267)");
      CHECK((rmcs_cb_v86_flags(0xFFFF) & 0x100) == 0, "cb exit: TF is never taken from the structure");
      CHECK((rmcs_cb_v86_flags(0xFFFF) & 0x3000) == 0, "cb exit: IOPL is never taken from the structure"); }

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
