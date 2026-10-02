/* auxprn_test.c -- off-VM battery for DOS's AUX/PRN driver code (GH #251).
 *
 * src/dos/dos_auxprn.h is guest code the host resumes the V86 guest in for INT 21h
 * AH=03h/04h/05h and AH=3Fh/40h on handles 3/4. It is RUN here, in the host's own
 * 8086 interpreter (v86interp.h), with INT 14h and INT 17h pointed at recorders --
 * the same arrangement tools/dostest/p_auxprn makes on the oracles -- and the call
 * sequences are held to what MS-DOS 6.22 (QEMU and PCem) logged:
 *
 *   05h:     17h/0200 17h/0200 17h/00cc            AX=05cc
 *   04h:     14h/0300 14h/01cc                     AX=04cc
 *   03h:     14h/0300 14h/02xx (AL from status)    AX=03 + byte
 *   40h h4:  per byte 17h/0200 17h/00cc            AX=CX CF=0
 *   40h h3:  per byte 14h/01cc                     AX=CX CF=0
 *   3Fh h3:  14h/0200 per byte, stop after CR      AX=n  CF=0
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
#include "dos_auxprn.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

/* Layout: the driver at DOS_CTAB_SEG:DOS_AUXPRN_OFF; the BIOS "handlers" are a HLT
   (unmodelled -> the interpreter stops there) followed by an IRET; the INT 21h
   caller's return address is another HLT. */
#define H17_SEG 0x2000
#define H14_SEG 0x2100
#define RET_SEG 0x3000
#define DAT_SEG 0x4000
#define STK_SEG 0x5000

typedef struct { int vec; uint16_t ax, dx; } call_t;
static call_t calls[64];
static int ncalls;
static const char *rxscript;
static int rxi;

/* Run from the driver entry until the caller's return HLT, answering the BIOS. */
static int run_entry(icpu *c, unsigned entry, uint16_t flags_in)
{
    uint32_t ssb = (uint32_t)STK_SEG << 4;
    int guard = 0;
    ncalls = 0;
    /* the INT 21h frame the stub leaves on the stack: IP, CS, FLAGS */
    c->seg[2] = STK_SEG; c->r[4] = 0xFFF0 - 6;
    MEM[ssb + 0xFFF0 - 6] = 0x00; MEM[ssb + 0xFFF0 - 5] = 0x00;          /* IP  0000 */
    MEM[ssb + 0xFFF0 - 4] = RET_SEG & 0xFF; MEM[ssb + 0xFFF0 - 3] = RET_SEG >> 8;
    MEM[ssb + 0xFFF0 - 2] = (BYTE)flags_in; MEM[ssb + 0xFFF0 - 1] = (BYTE)(flags_in >> 8);
    c->seg[1] = DOS_CTAB_SEG; c->ip = (uint16_t)(DOS_AUXPRN_OFF + entry);
    for (;;) {
        if (++guard > 100000) return -1;
        if (istep(c)) continue;
        if (c->seg[1] == RET_SEG && c->ip == 0) return 0;                /* back home */
        if ((c->seg[1] == H17_SEG || c->seg[1] == H14_SEG) && c->ip == 0) {
            int v = (c->seg[1] == H17_SEG) ? 0x17 : 0x14;
            uint16_t ax = (uint16_t)c->r[0], ah = ax >> 8;
            if (ncalls < 64) { calls[ncalls].vec = v; calls[ncalls].ax = ax;
                               calls[ncalls].dx = (uint16_t)c->r[2]; ++ncalls; }
            if (v == 0x17) ax = (uint16_t)(0x9000 | (ax & 0xFF));
            else if (ah == 1) ax = (uint16_t)(0x6000 | (ax & 0xFF));
            else if (ah == 2) ax = (uint16_t)(rxscript[rxi++] & 0xFF);
            else if (ah == 3) ax = 0x6130;
            c->r[0] = (c->r[0] & 0xFFFF0000u) | ax;
            c->ip = 1;                                                    /* the IRET */
            continue;
        }
        return -2;                                                        /* derailed */
    }
}

static icpu setup(void)
{
    icpu c; memset(&c, 0, sizeof c);
    memset(MEM, 0, sizeof MEM);
    memcpy(MEM + ((uint32_t)DOS_CTAB_SEG << 4) + DOS_AUXPRN_OFF, dos_auxprn_code, sizeof dos_auxprn_code);
    MEM[(uint32_t)H17_SEG << 4] = 0xF4; MEM[((uint32_t)H17_SEG << 4) + 1] = 0xCF;
    MEM[(uint32_t)H14_SEG << 4] = 0xF4; MEM[((uint32_t)H14_SEG << 4) + 1] = 0xCF;
    MEM[(uint32_t)RET_SEG << 4] = 0xF4;
    MEM[0x17 * 4 + 2] = H17_SEG & 0xFF; MEM[0x17 * 4 + 3] = H17_SEG >> 8;
    MEM[0x14 * 4 + 2] = H14_SEG & 0xFF; MEM[0x14 * 4 + 3] = H14_SEG >> 8;
    c.flags = 0x0002;
    c.r[3] = 0xB1B1; c.r[1] = 0xC1C1; c.r[6] = 0x5151; c.r[5] = 0xBBBB;
    c.seg[3] = DAT_SEG; c.seg[0] = 0xE5E5;
    rxscript = "Qr\rwxyz"; rxi = 0;
    return c;
}

static int is(int i, int v, uint16_t ax, uint16_t dx)
{ return i < ncalls && calls[i].vec == v && calls[i].ax == ax && calls[i].dx == dx; }

int main(void)
{
    icpu c;
    printf("== auxprn_test: DOS AUX/PRN driver code (#251)\n");
    CHECK(sizeof dos_auxprn_code <= DOS_AUXPRN_LEN, "fits its reservation");
    CHECK(DOS_AUXPRN_OFF >= DOS_SYSCONF_OFF + 10 && DOS_AUXPRN_OFF + DOS_AUXPRN_LEN <= 0x6F0,
          "between the C0h table and the block's end");

    c = setup(); c.r[0] = 0x05A5; c.r[2] = 0xD150;
    CHECK(run_entry(&c, DOS_AUXPRN_T05, 0x0003) == 0, "05h: returns to the caller");
    CHECK(ncalls == 3 && is(0, 0x17, 0x0200, 0) && is(1, 0x17, 0x0200, 0) && is(2, 0x17, 0x0050, 0),
          "05h: INT 17h 02h, 02h, 00h AL='P' DX=0 (6.22's sequence)");
    CHECK((uint16_t)c.r[0] == 0x0550, "05h: AX = 05:char");
    CHECK((uint16_t)c.r[3] == 0xB1B1 && (uint16_t)c.r[1] == 0xC1C1 && (uint16_t)c.r[2] == 0xD150,
          "05h: BX CX DX unchanged");
    CHECK(c.r[4] == 0xFFF0, "05h: stack balanced (the INT 21h frame popped)");
    CHECK(c.flags & 1, "05h: the caller's flags come back as they were (CF set in)");

    c = setup(); c.r[0] = 0x04A5; c.r[2] = 0xD141;
    CHECK(run_entry(&c, DOS_AUXPRN_T04, 0x0002) == 0, "04h: returns");
    CHECK(ncalls == 2 && is(0, 0x14, 0x0300, 0) && is(1, 0x14, 0x0141, 0),
          "04h: INT 14h 03h, then 01h AL='A' DX=0");
    CHECK((uint16_t)c.r[0] == 0x0441 && (uint16_t)c.r[2] == 0xD141, "04h: AX = 04:char, DX kept");

    c = setup(); c.r[0] = 0x03A5; c.r[2] = 0xD1D1;
    CHECK(run_entry(&c, DOS_AUXPRN_T03, 0x0002) == 0, "03h: returns");
    CHECK(ncalls == 2 && is(0, 0x14, 0x0300, 0) && is(1, 0x14, 0x0230, 0),
          "03h: INT 14h 03h, then 02h with AL left from the status (0230h)");
    CHECK((uint16_t)c.r[0] == 0x0351 && (uint16_t)c.r[2] == 0xD1D1, "03h: AX = 03:'Q', DX kept");

    c = setup(); c.r[0] = 0x4000; c.r[3] = 4; c.r[1] = 2; c.r[2] = 0x0100;
    MEM[((uint32_t)DAT_SEG << 4) + 0x100] = 'P'; MEM[((uint32_t)DAT_SEG << 4) + 0x101] = 'Q';
    CHECK(run_entry(&c, DOS_AUXPRN_W4, 0x0003) == 0, "40h h4: returns");
    CHECK(ncalls == 4 && is(0, 0x17, 0x0200, 0) && is(1, 0x17, 0x0050, 0)
          && is(2, 0x17, 0x0200, 0) && is(3, 0x17, 0x0051, 0),
          "40h h4: per byte INT 17h 02h then 00h");
    CHECK((uint16_t)c.r[0] == 2 && !(c.flags & 1), "40h h4: AX=2, CF cleared");
    CHECK((uint16_t)c.r[1] == 2 && (uint16_t)c.r[2] == 0x0100 && (uint16_t)c.r[6] == 0x5151,
          "40h h4: CX DX SI unchanged");

    c = setup(); c.r[0] = 0x4000; c.r[3] = 3; c.r[1] = 0; c.r[2] = 0x0100;
    CHECK(run_entry(&c, DOS_AUXPRN_W4, 0x0003) == 0 && ncalls == 0
          && (uint16_t)c.r[0] == 0 && !(c.flags & 1), "40h h4: CX=0 writes nothing, AX=0");

    c = setup(); c.r[0] = 0x4000; c.r[3] = 3; c.r[1] = 2; c.r[2] = 0x0100;
    MEM[((uint32_t)DAT_SEG << 4) + 0x100] = 'A'; MEM[((uint32_t)DAT_SEG << 4) + 0x101] = 'B';
    CHECK(run_entry(&c, DOS_AUXPRN_W3, 0x0003) == 0, "40h h3: returns");
    CHECK(ncalls == 2 && is(0, 0x14, 0x0141, 0) && is(1, 0x14, 0x0142, 0),
          "40h h3: per byte INT 14h 01h, no status");
    CHECK((uint16_t)c.r[0] == 2 && !(c.flags & 1), "40h h3: AX=2, CF cleared");

    c = setup(); c.r[0] = 0x3F00; c.r[3] = 3; c.r[1] = 6; c.r[2] = 0x0200; rxi = 1;
    memset(MEM + ((uint32_t)DAT_SEG << 4) + 0x200, 0xEE, 8);
    CHECK(run_entry(&c, DOS_AUXPRN_R3, 0x0003) == 0, "3Fh h3: returns");
    CHECK(ncalls == 2 && is(0, 0x14, 0x0200, 0) && is(1, 0x14, 0x0200, 0),
          "3Fh h3: INT 14h 02h per byte, stops after the CR");
    CHECK((uint16_t)c.r[0] == 2 && !(c.flags & 1), "3Fh h3: AX=2, CF cleared");
    CHECK(MEM[((uint32_t)DAT_SEG << 4) + 0x200] == 'r' && MEM[((uint32_t)DAT_SEG << 4) + 0x201] == 0x0D
          && MEM[((uint32_t)DAT_SEG << 4) + 0x202] == 0xEE, "3Fh h3: buffer 'r' CR, nothing after");
    CHECK((uint16_t)c.r[3] == 3 && (uint16_t)c.r[1] == 6 && (uint16_t)c.r[2] == 0x0200,
          "3Fh h3: BX CX DX unchanged");

    c = setup(); c.r[0] = 0x3F00; c.r[3] = 3; c.r[1] = 3; c.r[2] = 0x0200; rxi = 3;
    CHECK(run_entry(&c, DOS_AUXPRN_R3, 0x0002) == 0 && ncalls == 3 && (uint16_t)c.r[0] == 3,
          "3Fh h3: no CR -> stops at CX");

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
