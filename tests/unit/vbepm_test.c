/* vbepm_test.c -- off-VM battery for the VBE 2.0 protected-mode interface (#53).
 *
 * INT 10h AX=4F0Ah hands a protected-mode client a block of 32-bit code to COPY and CALL.
 * The only honest test of that is to do what a client does: take ES:DI and CX, copy the
 * block somewhere else entirely, and execute its three entry points -- here with the
 * host's own flat 32-bit interpreter (src/host/pm32interp.h), whose port hooks go to the
 * real video VDD on a bus. Then compare with what the INT 10h forms (4F05h/4F07h/4F09h)
 * do to the same machine. No oracle runs this: both real BIOSes we can execute answer
 * 4F0Ah with AX=0100h. The expectations are VBE 2.0 §4.13's.
 *
 * Every expectation was written before the 4F0Ah arm existed and fails on that code
 * (4F0Ah -> AX=0100h, no block, no ports at 01CEh/01CFh).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "vdd_video.h"

static uint8_t g_flat[0x200000];          /* guest memory: real-mode MB + room to copy into */
static uint8_t g_vmem[VIDEO_APERTURE_SIZE];
static VIDEO_STATE vid;
static VDD_BUS bus;
static uint64_t g_fake_us = 1000000;
static uint64_t fake_clock(void) { return g_fake_us; }

/* The interpreter's host hooks: flat memory, and port I/O onto the VDD bus. Each IN
   advances the fake clock 50 us, so a retrace wait on 3DAh makes progress. */
static uint32_t g_ins, g_outs;
static uint8_t  p32_rd8(uint32_t lin) { return lin < sizeof g_flat ? g_flat[lin] : 0xFF; }
static void     p32_wr8(uint32_t lin, uint8_t v) { if (lin < sizeof g_flat) g_flat[lin] = v; }
static int      p32_ok(uint32_t lin, int w, int wr) { (void)wr; return lin + (uint32_t)w <= sizeof g_flat; }
static uint32_t p32_in(uint16_t port, int w)
{ uint32_t v = 0; g_fake_us += 50; ++g_ins; VddBusIo(&bus, port, (uint8_t)w, 1, &v); return v; }
static void     p32_out(uint16_t port, int w, uint32_t v)
{ uint32_t x = v; ++g_outs; VddBusIo(&bus, port, (uint8_t)w, 0, &x); }
#include "../../src/host/pm32interp.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

#define COPY   0x150000u                   /* where the "client" copies the block       */
#define STACK  0x180000u
#define RETADR 0x1F0000u                   /* a return address nothing executes          */

/* Near-call entry `off` of the copied block with these registers; run to the RET.
   1 = returned to RETADR with ESP balanced; 0 = the interpreter declined something
   (the code used an instruction outside its set) or it never returned. */
static int call_pm(uint32_t off, uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t edi,
                   uint32_t es_base, p32cpu *out)
{
    p32cpu c; long n;
    memset(&c, 0, sizeof c);
    c.r[0] = 0xA5A5A5A5u; c.r[1] = ecx; c.r[2] = edx; c.r[3] = ebx;
    c.r[5] = 0xB5B5B5B5u; c.r[6] = 0xC6C6C6C6u; c.r[7] = edi;
    c.r[4] = STACK - 4; g_flat[STACK - 4] = (uint8_t)RETADR; g_flat[STACK - 3] = (uint8_t)(RETADR >> 8);
    g_flat[STACK - 2] = (uint8_t)(RETADR >> 16); g_flat[STACK - 1] = (uint8_t)(RETADR >> 24);
    c.eip = COPY + off; c.flags = 0x202;
    c.base[0] = es_base;                    /* ES; CS/SS/DS flat 0 */
    for (n = 0; n < 2000000; ++n) {
        if (c.eip == RETADR) break;
        if (!p32_step(&c)) { printf("    declined at +%#x (op %02X)\n", c.eip - COPY, g_flat[c.eip]); return 0; }
    }
    if (out) *out = c;
    return c.eip == RETADR && c.r[4] == STACK;
}

static void int10(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx, NTVDD_REGISTERS *r)
{
    memset(r, 0, sizeof *r);
    r->Eax = eax; r->Ebx = ebx; r->Ecx = ecx; r->Edx = edx;
    VddBusDeliverInterrupt(&bus, 0x10, r);
}

int main(void)
{
    NTVDD_DEVICE dev;
    NTVDD_REGISTERS r;
    p32cpu c;
    uint32_t blk, len, i;
    uint16_t win, start, pal, ports;
    printf("== VBE 4F0Ah protected-mode interface battery (#53) ==\n");
    memset(&vid, 0, sizeof vid);
    vid.VideoMemory = g_vmem;
    dev = VddVideoDevice(&vid);
    VddBusInitialize(&bus, g_flat);
    VddBusSetSinks(&bus, 0, 0, 0, 0);
    CHECK(VddBusAdd(&bus, &dev) == 0, "video VDD on the bus (01CEh/01CFh claimed with the rest)");
    vid.TimeUs = fake_clock;

    /* ---- 4F0Ah BL=00h: the table ---- */
    int10(0x4F0A, 0x0000, 0xC1C1, 0, &r);
    CHECK((r.Eax & 0xFFFF) == 0x004F, "4F0Ah BL=00h: AX=004Fh (was 0100h, 'no such function')");
    blk = ((uint32_t)r.Es << 4) + (r.Edi & 0xFFFF);
    len = r.Ecx & 0xFFFF;
    CHECK(r.Es == VDD_VBEPM_SEG && (r.Edi & 0xFFFF) == 0 && len >= 16 && len < 0x100,
          "4F0Ah: ES:DI = B260:0000, CX = the block's length (code included)");
    win   = (uint16_t)(g_flat[blk + 0] | (g_flat[blk + 1] << 8));
    start = (uint16_t)(g_flat[blk + 2] | (g_flat[blk + 3] << 8));
    pal   = (uint16_t)(g_flat[blk + 4] | (g_flat[blk + 5] << 8));
    ports = (uint16_t)(g_flat[blk + 6] | (g_flat[blk + 7] << 8));
    CHECK(win >= 8 && start >= 8 && pal >= 8 && win < len && start < len && pal < len,
          "table: the three entry offsets lie inside the block, past the 4-word header");
    {   /* the port list: words, FFFFh-terminated, then an empty memory list */
        int has1ce = 0, has1cf = 0, has3c9 = 0, has3da = 0, n = 0;
        uint32_t p = blk + ports;
        for (;;) { uint16_t w = (uint16_t)(g_flat[p] | (g_flat[p + 1] << 8)); p += 2;
                   if (w == 0xFFFF || ++n > 16) break;
                   has1ce |= w == 0x1CE; has1cf |= w == 0x1CF; has3c9 |= w == 0x3C9; has3da |= w == 0x3DA; }
        CHECK(has1ce && has1cf && has3c9 && has3da && g_flat[p] == 0xFF && g_flat[p + 1] == 0xFF,
              "table +6: every port the code touches, FFFFh, then an empty memory list (FFFFh)");
    }
    int10(0x4F0A, 0x0001, 0, 0, &r);
    CHECK((r.Eax & 0xFFFF) == 0x014F, "4F0Ah BL=01h: 014Fh (the subfunction does not exist)");

    /* ---- the client copies the block somewhere else entirely ---- */
    memcpy(g_flat + COPY, g_flat + blk, len);
    memset(g_flat + blk, 0xCC, len);          /* and the original is gone: nothing may point back */

    /* ---- outside a VESA mode the ports refuse and change nothing ---- */
    {   uint32_t rej = vid.VbePmRejected;
        CHECK(call_pm(win, 0x0000, 0, 3, 0, 0, NULL) && vid.VbePmRejected == rej + 1 && vid.VesaBank == 0,
              "SetWindow in mode 3: returns, refused, counted (vbe_pm_rej)"); }

    /* ---- 640x480x8 banked ---- */
    int10(0x4F02, 0x0101, 0, 0, &r);
    CHECK((r.Eax & 0xFFFF) == 0x004F && vid.IsVesa && !vid.IsVesaLfb, "4F02h 0101h: banked 640x480x8");
    memset(g_vmem, 0x11, VIDEO_VESA_WINDOW);       /* the client draws bank 0 ...                */
    CHECK(call_pm(win, 0x0000, 0, 2, 0, 0, &c), "SetWindow (copied, near-called) returns to the caller, ESP balanced");
    CHECK(vid.VesaBank == 2 && vid.VbePmBankCount == 1, "SetWindow DX=2: window A is bank 2");
    CHECK(vid.VesaVram[0] == 0x11 && vid.VesaVram[VIDEO_VESA_WINDOW - 1] == 0x11,
          "SetWindow: the old window was flushed into bank 0, as 4F05h does");
    CHECK(c.r[0] == 0xA5A5A5A5u && c.r[2] == 2 && c.r[3] == 0 && c.r[5] == 0xB5B5B5B5u && c.r[6] == 0xC6C6C6C6u,
          "SetWindow: EAX/EDX/EBX/EBP/ESI preserved");
    int10(0x4F05, 0x0100, 0, 0, &r);
    CHECK((r.Edx & 0xFFFF) == 2, "4F05h BH=01h (get) agrees: bank 2");
    CHECK(call_pm(win, 0x0001, 0, 1, 0, 0, NULL) && vid.VesaBank == 2,
          "SetWindow BL=01h (window B, which does not exist): no change");
    CHECK(call_pm(win, 0x0000, 0, 0x7FFF, 0, 0, NULL) && vid.VesaBank == 2,
          "SetWindow past the end of VRAM: refused, bank unchanged");
    CHECK(call_pm(win, 0x0000, 0, 0, 0, 0, NULL) && vid.VesaBank == 0 && g_vmem[0] == 0x11,
          "SetWindow back to 0: bank 0's bytes come back into the window");

    /* ---- SetDisplayStart: CX/DX = start in DWORDs; 4F07h BL=01h reads it back ---- */
    {   uint32_t org = 640u * 100u + 64u;     /* (64,100) */
        CHECK(call_pm(start, 0x0000, (org / 4) & 0xFFFF, (org / 4) >> 16, 0, 0, NULL)
              && vid.VesaOrigin == org && vid.VesaOriginLive == org && vid.VbePmStartCount == 1,
              "SetDisplayStart BL=00h: start = DX:CX * 4, shown at once");
        int10(0x4F07, 0x0001, 0, 0, &r);
        CHECK((r.Ecx & 0xFFFF) == 64 && (r.Edx & 0xFFFF) == 100, "4F07h BL=01h agrees: x=64, y=100");
    }
    {   uint32_t org = 640u * 480u, ins = g_ins;          /* page 2 */
        CHECK(call_pm(start, 0x0080, (org / 4) & 0xFFFF, (org / 4) >> 16, 0, 0, NULL)
              && vid.VesaOrigin == org && g_ins > ins + 2,
              "SetDisplayStart BL=80h: waits on 3DAh for the retrace, then sets it");
    }
    {   uint32_t org = vid.VesaOrigin, big = 0x3FFFFFu;     /* far past 4 MB */
        CHECK(call_pm(start, 0x0000, big & 0xFFFF, big >> 16, 0, 0, NULL) && vid.VesaOrigin == org,
              "SetDisplayStart past VRAM: refused, start unchanged");
    }

    /* ---- SetPalette: ES:EDI = B,G,R,pad entries; 4F09h BL=01h reads them back ---- */
    {   static const uint8_t ent[3 * 4] = { 0x01, 0x02, 0x03, 0, 0x3F, 0x00, 0x20, 0, 0x10, 0x11, 0x12, 0 };
        uint32_t data = 0x160000u, back = 0x9000u;      /* ES base 0x100000 + EDI 0x60000 */
        memcpy(g_flat + data, ent, sizeof ent);
        CHECK(call_pm(pal, 0x0000, 3, 0x40, 0x60000, 0x100000, &c), "SetPalette (ES:EDI, 3 entries at 40h) returns");
        CHECK(c.r[1] == 3 && c.r[2] == 0x40 && c.r[7] == 0x60000, "SetPalette: ECX/EDX/EDI preserved");
        memset(&r, 0, sizeof r);
        r.Eax = 0x4F09; r.Ebx = 0x0001; r.Ecx = 3; r.Edx = 0x40; r.Es = (uint16_t)(back >> 4); r.Edi = 0;
        VddBusDeliverInterrupt(&bus, 0x10, &r);
        CHECK((r.Eax & 0xFFFF) == 0x004F && memcmp(g_flat + back, ent, sizeof ent) == 0,
              "4F09h BL=01h reads back exactly what SetPalette wrote (B,G,R, 6-bit)");
        CHECK(call_pm(pal, 0x0080, 1, 0x41, 0x60004, 0x100000, NULL), "SetPalette BL=80h (retrace wait) returns");
    }

    /* ---- the LFB form of the mode has no window to switch ---- */
    int10(0x4F02, 0x4101, 0, 0, &r);
    {   uint32_t rej = vid.VbePmRejected;
        CHECK(call_pm(win, 0x0000, 0, 1, 0, 0, NULL) && vid.VbePmRejected == rej + 1,
              "SetWindow in an LFB mode: refused (4F05h answers 03h there)"); }

    /* ---- the block in guest memory is restored by every 4F0Ah call ---- */
    int10(0x4F0A, 0x0000, 0, 0, &r);
    for (i = 0; i < len; ++i) if (g_flat[blk + i] != g_flat[COPY + i]) break;
    CHECK(i == len, "4F0Ah again: the block at B260:0000 is rewritten, byte for byte");

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
