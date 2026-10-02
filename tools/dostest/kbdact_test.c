/* kbdact_test.c -- off-VM battery for the BIOS INT 09h side-calls (GH #254).
 *
 * src/dos/bios_kbdact.h is guest code the V86 INT 09h arm resumes the guest in for
 * Ctrl-Break (INT 1Bh), Print Screen (INT 05h), SysReq (INT 15h AX=8500h/8501h) and
 * Pause (spin until 0040:0018 bit 3 clears). Run here in v86interp with the vectors
 * pointed at recorders, from a stack holding the INT 09h frame.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef unsigned char BYTE;
static BYTE MEM[0x110000];
static uint8_t imem_r8(uint32_t lin) { return (lin < sizeof MEM) ? MEM[lin] : 0; }
static void    imem_w8(uint32_t lin, uint8_t v) { if (lin < sizeof MEM) MEM[lin] = v; }
static uint32_t iio_in(uint16_t port, int width) { (void)port; (void)width; return 0xFF; }
static void iio_out(uint16_t port, int width, uint32_t val) { (void)port; (void)width; (void)val; }

#include "../../src/host/v86interp.h"
#include "dos_layout.h"
#include "bios_kbdact.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

#define HND_SEG 0x2000          /* handler for vector v at HND_SEG:v*2 = HLT, IRET */
#define RET_SEG 0x3000
#define STK_SEG 0x5000
static int lastvec; static uint16_t lastax; static int ncalls; static int if_at_call;

static icpu setup(unsigned entry)
{
    icpu c; int v;
    memset(&c, 0, sizeof c); memset(MEM, 0, sizeof MEM);
    memcpy(MEM + ((uint32_t)DOS_CTAB_SEG << 4) + DOS_KBDACT_OFF, bios_kbdact_code, sizeof bios_kbdact_code);
    for (v = 0; v < 256; ++v) {
        MEM[v * 4] = (BYTE)(v * 2); MEM[v * 4 + 1] = (BYTE)((v * 2) >> 8);
        MEM[v * 4 + 2] = HND_SEG & 0xFF; MEM[v * 4 + 3] = HND_SEG >> 8;
        MEM[((uint32_t)HND_SEG << 4) + v * 2] = 0xF4;
        MEM[((uint32_t)HND_SEG << 4) + v * 2 + 1] = 0xCF;
    }
    MEM[(uint32_t)RET_SEG << 4] = 0xF4;
    c.seg[2] = STK_SEG; c.r[4] = 0xFFF0 - 6;
    MEM[((uint32_t)STK_SEG << 4) + 0xFFEC] = RET_SEG & 0xFF;
    MEM[((uint32_t)STK_SEG << 4) + 0xFFED] = RET_SEG >> 8;
    MEM[((uint32_t)STK_SEG << 4) + 0xFFEE] = 0x02;              /* caller FLAGS: IF=0 */
    c.seg[1] = DOS_CTAB_SEG; c.ip = (uint16_t)(DOS_KBDACT_OFF + entry);
    c.flags = 0x0002; c.r[0] = 0x1234; c.seg[3] = 0x7777;
    ncalls = 0; lastvec = -1;
    return c;
}

/* Run until home; `budget` steps max. A HLT in the handler block is a call. */
static int run(icpu *c, int budget, int clear_pause_after)
{
    int n = 0;
    for (;;) {
        if (++n > budget) return -1;
        if (clear_pause_after && n == clear_pause_after) MEM[0x418] &= (BYTE)~0x08;
        if (istep(c)) continue;
        if (c->seg[1] == RET_SEG && c->ip == 0) return 0;
        if (c->seg[1] == HND_SEG) {
            lastvec = c->ip / 2; lastax = (uint16_t)c->r[0]; ++ncalls;
            if_at_call = 0;
            c->ip = (uint16_t)(c->ip + 1);
            continue;
        }
        return -2;
    }
}

int main(void)
{
    icpu c;
    printf("== kbdact_test: BIOS INT 09h side-calls (#254)\n");
    CHECK(sizeof bios_kbdact_code <= DOS_KBDACT_LEN
          && DOS_AUXPRN_OFF + DOS_AUXPRN_LEN <= DOS_KBDACT_OFF
          && DOS_KBDACT_OFF + DOS_KBDACT_LEN <= 0x6F0, "fits between the AUX/PRN code and the block's end");

    c = setup(KBDACT_BRK);
    CHECK(run(&c, 1000, 0) == 0 && ncalls == 1 && lastvec == 0x1B, "brk: calls INT 1Bh once, returns");
    CHECK((uint16_t)c.r[0] == 0x1234 && c.r[4] == 0xFFF0 && !(c.flags & 0x200),
          "brk: AX kept, stack balanced, caller's IF restored");

    c = setup(KBDACT_PRT);
    CHECK(run(&c, 1000, 0) == 0 && ncalls == 1 && lastvec == 0x05, "prt: calls INT 05h once");

    c = setup(KBDACT_SYSD);
    CHECK(run(&c, 1000, 0) == 0 && ncalls == 1 && lastvec == 0x15 && lastax == 0x8500
          && (uint16_t)c.r[0] == 0x1234, "sysd: INT 15h AX=8500h, AX restored");
    c = setup(KBDACT_SYSU);
    CHECK(run(&c, 1000, 0) == 0 && ncalls == 1 && lastvec == 0x15 && lastax == 0x8501,
          "sysu: INT 15h AX=8501h");

    c = setup(KBDACT_PAUSE);
    MEM[0x418] = 0x08;
    CHECK(run(&c, 5000, 0) == -1, "pause: spins while 0040:0018 bit 3 is set");
    CHECK(c.flags & 0x200, "pause: ...with interrupts ON");
    c = setup(KBDACT_PAUSE);
    MEM[0x418] = 0x08;
    CHECK(run(&c, 5000, 300) == 0 && ncalls == 0, "pause: returns once the bit clears");
    CHECK((uint16_t)c.r[0] == 0x1234 && c.seg[3] == 0x7777 && c.r[4] == 0xFFF0,
          "pause: AX and DS restored, stack balanced");

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
