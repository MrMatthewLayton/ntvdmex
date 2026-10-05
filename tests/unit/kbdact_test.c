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
static int out_port = -1, out_val = -1;
static void iio_out(uint16_t port, int width, uint32_t val) { (void)width; out_port = port; out_val = (int)val; }

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
static int clear_cf_vec = -1;     /* #244: this vector's handler returns CF=0 (a swallow) */
static int stop_ip = -1;          /* #244/#274: stop at this DOS_CTAB_SEG offset (a BOP site) */

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
        if (stop_ip >= 0 && c->seg[1] == DOS_CTAB_SEG && c->ip == (uint16_t)stop_ip) return 1;
        if (istep(c)) continue;
        if (c->seg[1] == RET_SEG && c->ip == 0) return 0;
        if (c->seg[1] == HND_SEG) {
            lastvec = c->ip / 2; lastax = (uint16_t)c->r[0]; ++ncalls;
            if_at_call = 0;
            if (lastvec == clear_cf_vec)          /* the IRET pops FLAGS at SS:SP+4 */
                MEM[((uint32_t)c->seg[2] << 4) + (uint16_t)(c->r[4] + 4)] &= (BYTE)~0x01;
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

    /* ── #244: k4f, the INT 15h AH=4Fh call. The host has pushed the interrupted AX
         (1234h) above the INT 09h frame and loaded AX = 4F00h | scancode. */
    CHECK(bios_kbdact_code[KBDACT_IRET] == 0xCF, "k4f: KBDACT_IRET names a bare IRET");
    CHECK(bios_kbdact_code[KBDACT_K4F_BOP] == 0xC4 && bios_kbdact_code[KBDACT_K4F_BOP + 1] == 0xC4
          && bios_kbdact_code[KBDACT_K4F_BOP + 2] == 0x09, "k4f: KBDACT_K4F_BOP names a BOP 09h");
    c = setup(KBDACT_K4F);
    c.r[4] -= 2;
    MEM[((uint32_t)STK_SEG << 4) + c.r[4]] = 0x34; MEM[((uint32_t)STK_SEG << 4) + c.r[4] + 1] = 0x12;
    c.r[0] = 0x4F1E;
    stop_ip = DOS_KBDACT_OFF + KBDACT_K4F_BOP;
    CHECK(run(&c, 1000, 0) == 1 && ncalls == 1 && lastvec == 0x15 && lastax == 0x4F1E,
          "k4f: calls INT 15h with AH=4Fh AL=scancode (CF=1 on entry) ...");
    CHECK((c.r[0] & 0xFF) == 0x1E && (c.flags & 1) && c.r[4] == 0xFFF0 - 8,
          "k4f: ...a CF=1 answer reaches the translate BOP with AL as left, the saved AX still pushed");
    c = setup(KBDACT_K4F);
    c.r[4] -= 2;
    MEM[((uint32_t)STK_SEG << 4) + c.r[4]] = 0x34; MEM[((uint32_t)STK_SEG << 4) + c.r[4] + 1] = 0x12;
    c.r[0] = 0x4F1E; clear_cf_vec = 0x15; out_port = out_val = -1;
    CHECK(run(&c, 1000, 0) == 0 && ncalls == 1, "k4f: a CF=0 answer (swallow) never reaches the BOP...");
    CHECK(out_port == 0x20 && out_val == 0x20, "k4f: ...sends the BIOS's own EOI (20h to port 20h)...");
    CHECK((uint16_t)c.r[0] == 0x1234 && c.r[4] == 0xFFF0, "k4f: ...restores AX and IRETs, stack balanced");
    clear_cf_vec = -1;

    /* ── #274: p5, the default INT 05h. The host's two BOPs are simulated here: begin
         hands back one byte ('X'), next says "done" (CF=1). */
    CHECK(bios_kbdact_code[KBDACT_P5_BEGIN] == 0xC4 && bios_kbdact_code[KBDACT_P5_NEXT] == 0xC4
          && bios_kbdact_code[KBDACT_P5_BEGIN + 2] == 0x09 && bios_kbdact_code[KBDACT_P5_NEXT + 2] == 0x09,
          "p5: both sites are BOP 09h");
    c = setup(KBDACT_P5);
    stop_ip = DOS_KBDACT_OFF + KBDACT_P5_BEGIN;
    CHECK(run(&c, 1000, 0) == 1 && (c.flags & 0x200), "p5: STI, then the begin BOP");
    c.r[0] = 0x0058; c.r[2] = 0; c.flags &= ~1u; c.ip += 3;          /* host: emit 'X' */
    stop_ip = DOS_KBDACT_OFF + KBDACT_P5_NEXT;
    CHECK(run(&c, 1000, 0) == 1 && ncalls == 1 && lastvec == 0x17 && lastax == 0x0058,
          "p5: the byte goes out through INT 17h AH=00h, then the next BOP");
    c.r[0] = 0x1234; c.flags |= 1u; c.ip += 3;                        /* host: done      */
    stop_ip = -1;
    CHECK(run(&c, 1000, 0) == 0 && ncalls == 1 && c.r[4] == 0xFFF0 && !(c.flags & 0x200),
          "p5: CF=1 from the host ends it: IRET, stack balanced, caller's IF back");
    c = setup(KBDACT_P5);
    stop_ip = DOS_KBDACT_OFF + KBDACT_P5_BEGIN;
    (void)run(&c, 1000, 0);
    c.flags |= 1u; c.ip += 3; stop_ip = -1;                           /* host: busy      */
    CHECK(run(&c, 1000, 0) == 0 && ncalls == 0, "p5: CF=1 at begin (already printing) -> straight back, no INT 17h");

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
