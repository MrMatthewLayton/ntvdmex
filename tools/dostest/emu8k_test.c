/* emu8k_test.c -- off-VM battery for the AWE32's EMU8000 (src/vdd/vdd_emu8k.c, #233).
 *
 * Every expectation is from the AWE32/EMU8000 Programmer's Guide rev 1.00 (§n, p.n), written
 * before the model was run, and every access goes through the PORTS exactly as a DOS program
 * makes it -- the pointer at E+802h, then the data port:
 *   T1  detection as the period drivers do it (HWCF1/HWCF2 read-back) and the §4 procedure
 *   T2  the wall clock WC advancing at the sample rate
 *   T3  register read-back, per channel, word and doubleword, and E+402h's two meanings
 *   T4  sound memory: DMA-stream upload through SMALW/SMLD and SMARW/SMRD, read-back through
 *       SMALR/SMLD with the stale prefetch word, the ROM region, a stream with no channel
 *   T5  a looping channel started by §6's recipe: pitch from IP, octave up/down, pan
 *   T6  the volume envelope: attack shape, IFATN attenuation, release shape and end
 *   T7  the filter: low cutoff attenuates, Q resonates, Q0/FFh is transparent
 *   T8  output gating: HWCF3's audio enable, a DMA channel is silent
 *   T9  the mixer hook: vdd_audio_set_emu8k puts the chip into the host's stereo mix
 */
#include <stdio.h>
#include <string.h>
#include "vdd_emu8k.h"
#include "vdd_audio.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

static uint8_t  g_flat[0x10000];
static uint16_t g_dram[EMU8K_DRAM_WORDS];
static vdd_bus bus;
static emu8k_state emu;

#define E 0x620
static void outw(uint16_t p, uint16_t v) { uint32_t x = v; vdd_bus_io(&bus, p, 2, 0, &x); }
static uint16_t inw(uint16_t p) { uint32_t x = 0; vdd_bus_io(&bus, p, 2, 1, &x); return (uint16_t)x; }
static void outd(uint16_t p, uint32_t v) { uint32_t x = v; vdd_bus_io(&bus, p, 4, 0, &x); }
static uint32_t ind(uint16_t p) { uint32_t x = 0; vdd_bus_io(&bus, p, 4, 1, &x); return x; }
static void ptr(int reg, int ch) { outw(E + 0x802, (uint16_t)((reg << 5) | ch)); }
/* a doubleword is the LS word to the port, then the MS word two higher (§2) */
static void d0w(int r, int c, uint32_t v) { ptr(r, c); outw(E, (uint16_t)v); outw(E + 2, (uint16_t)(v >> 16)); }
static uint32_t d0r(int r, int c) { uint32_t lo; ptr(r, c); lo = inw(E); return lo | ((uint32_t)inw(E + 2) << 16); }
static void d1dw(int r, int c, uint32_t v) { ptr(r, c); outw(E + 0x400, (uint16_t)v); outw(E + 0x402, (uint16_t)(v >> 16)); }
static uint32_t d1dr(int r, int c) { uint32_t lo; ptr(r, c); lo = inw(E + 0x400); return lo | ((uint32_t)inw(E + 0x402) << 16); }
static void d1w(int r, int c, uint16_t v) { ptr(r, c); outw(E + 0x400, v); }
static uint16_t d1r(int r, int c) { ptr(r, c); return inw(E + 0x400); }
static void d2w(int r, int c, uint16_t v) { ptr(r, c); outw(E + 0x402, v); }
static uint16_t d2r(int r, int c) { ptr(r, c); return inw(E + 0x402); }
static void d3w(int r, int c, uint16_t v) { ptr(r, c); outw(E + 0x800, v); }
static uint16_t d3r(int r, int c) { ptr(r, c); return inw(E + 0x800); }

/* the register map (p.6-7) */
#define CPF(c,v)     d0w(0,c,v)
#define PTRX(c,v)    d0w(1,c,v)
#define CVCF(c,v)    d0w(2,c,v)
#define VTFT(c,v)    d0w(3,c,v)
#define PSST(c,v)    d0w(6,c,v)
#define CSL(c,v)     d0w(7,c,v)
#define CCCA(c,v)    d1dw(0,c,v)
#define ENVVOL(c,v)  d1w(4,c,v)
#define DCYSUSV(c,v) d1w(5,c,v)
#define ENVVAL(c,v)  d1w(6,c,v)
#define DCYSUS(c,v)  d1w(7,c,v)
#define ATKHLDV(c,v) d2w(4,c,v)
#define LFO1VAL(c,v) d2w(5,c,v)
#define ATKHLD(c,v)  d2w(6,c,v)
#define LFO2VAL(c,v) d2w(7,c,v)
#define IP(c,v)      d3w(0,c,v)
#define IFATN(c,v)   d3w(1,c,v)
#define PEFE(c,v)    d3w(2,c,v)
#define FMMOD(c,v)   d3w(3,c,v)
#define TREMFRQ(c,v) d3w(4,c,v)
#define FM2FRQ2(c,v) d3w(5,c,v)
#define SMALR(v) d1dw(1,20,v)
#define SMARR(v) d1dw(1,21,v)
#define SMALW(v) d1dw(1,22,v)
#define SMARW(v) d1dw(1,23,v)
#define WC()     d2r(1,27)

static int16_t buf[2 * 44100];
static void render(uint32_t n) { while (n) { uint32_t k = n > 44100 ? 44100 : n; vdd_emu8k_render_st(&emu, buf, k); n -= k; } }

/* §5: allocate channel `c` to a DMA stream -- the guide's seven steps in its order. */
static void dma_alloc(int c, uint32_t mode)
{
    DCYSUSV(c, 0x0080); VTFT(c, 0); CVCF(c, 0);
    PTRX(c, 0x40000000u); CPF(c, 0x40000000u);
    PSST(c, 0); CSL(c, 0);
    CCCA(c, mode);
}

/* §6: start a sound on channel `c`. Addresses are the ACTUAL audio locations; the
   registers get them minus one (the interpolator offset). */
static void note_on(int c, uint32_t start, uint32_t ls, uint32_t le, uint8_t pan,
                    uint16_t ip, uint16_t ifatn, uint16_t atkhldv, uint16_t dcysusv, uint8_t q)
{
    DCYSUSV(c, 0x0080); VTFT(c, 0); CVCF(c, 0); PTRX(c, 0); CPF(c, 0);   /* silent and idle */
    ENVVOL(c, 0x8000); ENVVAL(c, 0x8000); DCYSUS(c, 0x7F7F); ATKHLDV(c, atkhldv);
    LFO1VAL(c, 0x8000); ATKHLD(c, 0x7F7F); LFO2VAL(c, 0x8000); IP(c, ip); IFATN(c, ifatn);
    PEFE(c, 0); FMMOD(c, 0); TREMFRQ(c, 0x0010); FM2FRQ2(c, 0x0010);
    PSST(c, ((uint32_t)pan << 24) | (ls - 1));
    CSL(c, le - 1);
    CCCA(c, ((uint32_t)q << 28) | (start - 1));
    VTFT(c, 0x0000FFFFu); CVCF(c, 0x0000FFFFu);
    DCYSUSV(c, dcysusv);
    PTRX(c, 0x40000000u); CPF(c, 0x40000000u);
}
static void note_kill(int c) { DCYSUSV(c, 0x0080); VTFT(c, 0x0000FFFFu); CVCF(c, 0x0000FFFFu); }  /* §7 */

/* rising zero crossings of (L+R) over `n` frames = the frequency in Hz when n = 44100 */
static int crossings(uint32_t n)
{
    uint32_t i; int c = 0, prev = 0;
    vdd_emu8k_render_st(&emu, buf, n);
    for (i = 0; i < n; ++i) { int s = buf[2*i] + buf[2*i+1]; if (prev < 0 && s >= 0) c++; prev = s; }
    return c;
}
static void peaks(uint32_t n, int *pl, int *pr)
{
    uint32_t i; int l = 0, r = 0;
    vdd_emu8k_render_st(&emu, buf, n);
    for (i = 0; i < n; ++i) {
        int a = buf[2*i] < 0 ? -buf[2*i] : buf[2*i], b = buf[2*i+1] < 0 ? -buf[2*i+1] : buf[2*i+1];
        if (a > l) l = a; if (b > r) r = b;
    }
    *pl = l; *pr = r;
}
static uint16_t cv(int c) { return (uint16_t)(d0r(2, c) >> 16); }

static uint64_t g_fake_us;
static uint64_t fake_clock(void *ctx) { (void)ctx; return g_fake_us; }

#define A 0x201000u   /* where the test tone lives in DRAM */

int main(void)
{
    uint32_t i;
    int c;

    printf("== AWE32 EMU8000 battery ==\n");
    memset(&emu, 0, sizeof emu);
    vdd_bus_init(&bus, g_flat);
    emu.dram = g_dram; emu.dram_words = EMU8K_DRAM_WORDS; emu.base = E;
    { ntvdd d = vdd_emu8k_device(&emu); CHECK(vdd_bus_add(&bus, &d) == 0, "add: emu8k at 620h/A20h/E20h (three port groups)"); }

    /* ---- T1: detection -- as the period drivers do it: write HWCF1/2/3, read 1 and 2 back ---- */
    d1w(1, 29, 0x0059); d1w(1, 30, 0x0020); d1w(1, 31, 0x0000);
    CHECK((d1r(1, 29) & 0x007E) == 0x0058, "detect: HWCF1 & 7Eh reads 58h after 59h  <-- THE TEST");
    CHECK((d1r(1, 30) & 0x0003) == 0x0003, "detect: HWCF2 & 03h reads 03h after 20h");
    ptr(5, 17);
    CHECK((inw(E + 0x802) & 0xFF) == ((5 << 5) | 17), "pointer: reg/channel read back in the low byte");
    /* §4, the whole initialisation procedure, as AWEUTIL /S would run it */
    for (c = 0; c < 32; ++c) DCYSUSV(c, 0x0080);
    for (c = 0; c < 32; ++c) {
        ENVVOL(c, 0); ENVVAL(c, 0); DCYSUS(c, 0); ATKHLDV(c, 0); LFO1VAL(c, 0); ATKHLD(c, 0);
        LFO2VAL(c, 0); IP(c, 0); IFATN(c, 0); PEFE(c, 0); FMMOD(c, 0); TREMFRQ(c, 0); FM2FRQ2(c, 0);
        PTRX(c, 0); VTFT(c, 0); PSST(c, 0); CSL(c, 0); CCCA(c, 0);
    }
    for (c = 0; c < 32; ++c) { CPF(c, 0); CVCF(c, 0); }
    SMALR(0); SMARR(0); SMALW(0); SMARW(0);
    for (c = 0; c < 32; ++c) { d1w(2, c, (uint16_t)(0x1000 + c)); d2w(2, c, (uint16_t)(0x2000 + c));
                               d1w(3, c, (uint16_t)(0x3000 + c)); d2w(3, c, (uint16_t)(0x4000 + c)); }
    { uint16_t w0 = WC(); int spins = 0;                    /* "wait 1024 sample periods" */
      while ((uint16_t)(WC() - w0) < 1024 && spins < 1000) { render(64); spins++; }
      CHECK(spins == 16, "§4: a guest waiting 1024 samples on WC gets out, in 1024 samples"); }
    d1dw(1, 9, 0); d1dw(1, 10, 0x00000083u); d1dw(1, 13, 0x00008000u);
    d1w(1, 31, 0x0004);
    CHECK(d1r(2, 7) == 0x1007 && d2r(2, 7) == 0x2007 && d1r(3, 31) == 0x301F && d2r(3, 0) == 0x4000,
          "INIT1-4: each of the four arrays holds its own 32 words");
    CHECK(d1dr(1, 10) == 0x83 && d1dr(1, 13) == 0x8000, "HWCF5/HWCF6: doublewords read back");

    /* ---- T2: the wall clock ---- */
    { uint16_t w0 = WC(), w1;
      render(1000); w1 = WC();
      CHECK((uint16_t)(w1 - w0) == 1000, "WC: 1000 rendered samples advance it by exactly 1000");
      emu.clock = fake_clock; g_fake_us = 0; w0 = WC();
      g_fake_us = 1000000u; w1 = WC();
      CHECK((uint16_t)(w1 - w0) == 44100, "WC on a host clock: one second = 44100 counts");
      g_fake_us = 1486000u; w1 = WC();
      CHECK((uint16_t)(w1 - w0) < 100 || (uint16_t)(w1 - w0) > 65436, "WC: wraps every 1.486 s (p.13)");
      emu.clock = 0; }

    /* ---- T3: register read-back ---- */
    IP(0, 0xE123); IP(31, 0xD456); IFATN(0, 0xAB12); PEFE(0, 0x1234); FMMOD(0, 0x5678);
    TREMFRQ(0, 0x9ABC); FM2FRQ2(0, 0xDEF0); ENVVOL(0, 0x7123); ENVVAL(0, 0x7456);
    LFO1VAL(0, 0x7789); LFO2VAL(0, 0x7ABC);
    CHECK(d3r(0, 0) == 0xE123 && d3r(0, 31) == 0xD456, "IP: channel 0 and channel 31 are separate registers");
    CHECK(d3r(1, 0) == 0xAB12 && d3r(2, 0) == 0x1234 && d3r(3, 0) == 0x5678 && d3r(4, 0) == 0x9ABC && d3r(5, 0) == 0xDEF0,
          "Data3: IFATN PEFE FMMOD TREMFRQ FM2FRQ2 read back");
    CHECK(d1r(4, 0) == 0x7123 && d1r(6, 0) == 0x7456 && d2r(5, 0) == 0x7789 && d2r(7, 0) == 0x7ABC,
          "ENVVOL ENVVAL LFO1VAL LFO2VAL read back");
    ATKHLDV(0, 0x12FF); ATKHLD(0, 0x34FF); DCYSUS(0, 0x56FF);
    CHECK(d2r(4, 0) == 0x127F && d2r(6, 0) == 0x347F && d1r(7, 0) == 0x567F,
          "ATKHLDV/ATKHLD/DCYSUS: bit 7 reads as zero (p.15-16)");
    PSST(5, 0x80123456u); CSL(5, 0x40234567u);
    CHECK(d0r(6, 5) == 0x80123456u && d0r(7, 5) == 0x40234567u, "PSST/CSL: doublewords through two word transfers");
    ptr(6, 6); outd(E, 0xC0345678u);
    CHECK(d0r(6, 6) == 0xC0345678u && (ptr(6, 6), ind(E)) == 0xC0345678u, "PSST: one 32-bit OUT/IN = the two word transfers");
    CCCA(7, 0xF0ABCDEFu);
    CHECK(d1dr(0, 7) == 0xF0ABCDEFu, "CCCA: Q, control bits and address read back");
    CCCA(7, 0);
    /* E+402h: CCCA's MS word when r0 is selected, the Data2 word register otherwise */
    ptr(0, 8); outw(E + 0x402, 0x1234);
    CHECK((d1dr(0, 8) >> 16) == 0x1234 && d2r(4, 8) == 0, "E+402h with r0 selected is CCCA's MS word, not ATKHLDV");
    CCCA(8, 0);

    /* ---- T4: sound memory (§5) ---- */
    vdd_emu8k_reset(&emu);
    dma_alloc(30, 0x06000000u);                             /* left write */
    CHECK(!(d1dr(1, 22) & 0x80000000u), "SMALW: FULL clear before the address is set");
    SMALW(0x200000u);
    for (i = 0; i < 256; ++i) d1w(1, 26, (uint16_t)(0xA500 + i));
    CHECK(d1dr(1, 22) == 0x200100u, "SMALW: advanced one word per SMLD write, FULL clear");
    CHECK(g_dram[0] == 0xA500 && g_dram[255] == 0xA5FF, "DRAM: 256 words landed at 200000h");
    dma_alloc(29, 0x07000000u);                             /* right write */
    SMARW(0x200200u);
    for (i = 0; i < 16; ++i) d2w(1, 26, (uint16_t)(0x5A00 + i));
    CHECK(g_dram[0x200] == 0x5A00 && g_dram[0x20F] == 0x5A0F && d1dr(1, 23) == 0x200210u, "SMRD/SMARW: the right stream writes too");
    dma_alloc(28, 0x04000000u);                             /* left read */
    SMALR(0x200000u);
    (void)d1r(1, 26);                                        /* the stale word (§5) */
    { int ok = 1; for (i = 0; i < 256; ++i) if (d1r(1, 26) != (uint16_t)(0xA500 + i)) ok = 0;
      CHECK(ok, "SMALR/SMLD: after one stale read, 256 words read back in order  <-- UPLOAD/READBACK"); }
    CHECK(!(d1dr(1, 20) & 0x80000000u), "SMALR: EMPTY clear while a channel serves the stream");
    { uint16_t stale;
      SMALR(0x200010u); (void)d1r(1, 26);
      CHECK(d1r(1, 26) == 0xA510, "SMLD: reading from 200010h");      /* prefetched 200011h */
      SMALR(0x200005u);
      stale = d1r(1, 26);
      CHECK(stale == 0xA511 && d1r(1, 26) == 0xA505,
            "SMLD read is a PREFETCH: the first word after a new SMALR is the OLD stream's next word"); }
    dma_alloc(27, 0x05000000u);                             /* right read */
    SMARR(0x200200u); (void)d2r(1, 26);
    CHECK(d2r(1, 26) == 0x5A00 && d2r(1, 26) == 0x5A01, "SMARR/SMRD: the right stream reads too");
    SMALR(0x000100u); (void)d1r(1, 26);
    CHECK(d1r(1, 26) == 0, "ROM region (below 200000h): reads zero -- no GM ROM image fitted");
    SMALW(0x000100u); d1w(1, 26, 0x7777);
    CHECK(emu.sm_rom_writes == 1, "ROM region: a write goes nowhere");
    SMALW(0x23FFFFu); d1w(1, 26, 0x4242);
    SMALW(0x240000u); d1w(1, 26, 0x4343);
    SMALR(0x23FFFFu); (void)d1r(1, 26);
    CHECK(d1r(1, 26) == 0x4242 && d1r(1, 26) == 0, "DRAM: the last of 512 KB is there, the word past it reads zero");
    for (c = 27; c <= 30; ++c) CCCA(c, 0);                   /* deallocate every stream */
    SMALW(0x200400u); d1w(1, 26, 0x1234);
    CHECK((d1dr(1, 22) & 0x80000000u) && g_dram[0x400] != 0x1234, "no channel on the stream: the word waits, FULL set");
    dma_alloc(30, 0x06000000u);
    CHECK(g_dram[0x400] == 0x1234 && !(d1dr(1, 22) & 0x80000000u), "allocating a channel completes it, FULL clears");
    CCCA(30, 0);

    /* ---- T5: a looping channel: pitch and pan ---- */
    /* Three periods of a 64-word triangle, ±16384, so the loop seam is continuous. */
    SMALW(A); dma_alloc(30, 0x06000000u);
    for (i = 0; i < 192; ++i) {
        uint32_t p = i & 63; int32_t t = p < 16 ? (int32_t)p * 1024 : p < 48 ? 32768 - (int32_t)p * 1024 : (int32_t)p * 1024 - 65536;
        d1w(1, 26, (uint16_t)(int16_t)(t > 16383 ? 16383 : t < -16383 ? -16383 : t));
    }
    CCCA(30, 0);
    CHECK(g_dram[A - EMU8K_DRAM_BASE + 16] == 16383 && g_dram[A - EMU8K_DRAM_BASE + 64] == 0, "tone uploaded");
    note_on(0, A, A + 64, A + 128, 0x80, 0xE000, 0xFF00, 0x7F7F, 0x7F7F, 0);
    render(4410);
    CHECK((d0r(0, 0) >> 16) == 0x4000 && (d0r(1, 0) >> 16) == 0x4000, "IP E000h: pitch target and current pitch = 4000h (unity)");
    { uint32_t ca = d1dr(0, 0) & 0xFFFFFF;
      CHECK(ca >= A + 63 && ca < A + 127, "CCCA: the current address stays inside the loop"); }
    { int f = crossings(44100); char m[96];
      sprintf(m, "unity pitch: a 64-word loop plays at 44100/64 = 689 Hz (got %d)", f);
      CHECK(f >= 687 && f <= 691, m); }
    IP(0, 0xF000);
    { int f; char m[96]; render(64); f = crossings(44100);
      sprintf(m, "IP F000h (+1 octave): 1378 Hz (got %d)", f);
      CHECK(f >= 1375 && f <= 1381, m); }
    IP(0, 0xD000);
    { int f; char m[96]; render(64); f = crossings(44100);
      sprintf(m, "IP D000h (-1 octave): 345 Hz (got %d)", f);
      CHECK(f >= 343 && f <= 346, m); }
    IP(0, 0xE000);
    { int l, r, l2, r2, lc, rc;
      note_on(0, A, A + 64, A + 128, 0xFF, 0xE000, 0xFF00, 0x7F7F, 0x7F7F, 0); render(4410); peaks(4410, &l, &r);
      note_on(0, A, A + 64, A + 128, 0x00, 0xE000, 0xFF00, 0x7F7F, 0x7F7F, 0); render(4410); peaks(4410, &l2, &r2);
      note_on(0, A, A + 64, A + 128, 0x80, 0xE000, 0xFF00, 0x7F7F, 0x7F7F, 0); render(4410); peaks(4410, &lc, &rc);
      printf("        pan FFh: L=%d R=%d   pan 00h: L=%d R=%d   pan 80h: L=%d R=%d\n", l, r, l2, r2, lc, rc);
      CHECK(l > 15000 && r == 0, "pan FFh: extreme LEFT (p.9) -- the right channel is silent");
      CHECK(r2 > 15000 && l2 == 0, "pan 00h: extreme RIGHT -- the left channel is silent");
      CHECK(lc > l * 45 / 100 && lc < l * 55 / 100 && rc > l * 45 / 100 && rc < l * 55 / 100,
            "pan 80h: the middle of a linear crossfade -- half on each side"); }

    /* ---- T6: the volume envelope ---- */
    { uint32_t a1 = vdd_emu8k_attack_us(1), a7f = vdd_emu8k_attack_us(0x7F);
      uint32_t d1 = vdd_emu8k_decay_us_per_db(1), d7f = vdd_emu8k_decay_us_per_db(0x7F);
      printf("        attack 01h=%u us 7Fh=%u us   decay 01h=%u us/dB 7Fh=%u us/dB\n", a1, a7f, d1, d7f);
      CHECK(a1 > 11760000 && a1 < 12000000 && a7f > 5900 && a7f < 6100, "attack: 01h = 11.88 s, 7Fh = 6 ms (p.16)");
      CHECK(d1 > 465000 && d1 < 475000 && d7f > 235 && d7f < 245, "decay: 01h = 470 ms/dB, 7Fh = 240 us/dB (p.15)"); }
    { uint32_t T = (uint32_t)(((uint64_t)vdd_emu8k_attack_us(0x40) * 441u) / 10000u);   /* in samples */
      uint16_t q1, q2, q4;
      note_on(0, A, A + 64, A + 128, 0x80, 0xE000, 0xFF00, 0x7F40, 0x7F7F, 0);
      render(T / 4); q1 = cv(0); render(T / 4); q2 = cv(0); render(T / 2 + 64); q4 = cv(0);
      printf("        attack 40h (%u samples): CV at T/4=%u T/2=%u T=%u\n", T, q1, q2, q4);
      CHECK(q1 > 0xFFFF * 20 / 100 && q1 < 0xFFFF * 30 / 100 && q2 > 0xFFFF * 45 / 100 && q2 < 0xFFFF * 55 / 100,
            "attack: LINEAR in amplitude -- a quarter at T/4, a half at T/2");
      CHECK(q4 >= 0xFFF0, "attack: full volume at T"); }
    { uint16_t c0, c1, c2; char m[128];
      DCYSUSV(0, 0x805C);                                     /* §7's release example */
      render(32); c0 = cv(0); render(882); c1 = cv(0); render(882); c2 = cv(0);
      sprintf(m, "release 5Ch: dB-linear -- the 2nd 20 ms falls by the same ratio as the 1st (%u %u %u)", c0, c1, c2);
      CHECK(c1 < c0 && c2 < c1 && c0 > 60000
            && (double)c2 / c1 > 0.85 * (double)c1 / c0 && (double)c2 / c1 < 1.15 * (double)c1 / c0, m);
      CHECK((double)c1 / c0 > 0.25 && (double)c1 / c0 < 0.37, "release 5Ch: ~10 dB per 20 ms (1.97 ms/dB)");
      render(44100 / 4);
      CHECK(cv(0) == 0 && (d0r(3, 0) >> 16) == 0, "release: silent (CV = VT = 0) within 250 ms"); }
    note_on(0, A, A + 64, A + 128, 0x80, 0xE000, 0xFF20, 0x7F7F, 0x7F7F, 0);
    render(4410);
    { uint16_t v = cv(0); char m[96];
      sprintf(m, "IFATN 20h: 32 x 0.375 = 12 dB -> CV = FFFFh x 0.251 (got %u)", v);
      CHECK(v > 16100 && v < 16800, m); }
    note_kill(0);
    CHECK(cv(0) == 0 && (d0r(3, 0) >> 16) == 0, "§7 abrupt end: engine off, VT and CV zero at once");

    /* ---- T7: the filter ---- */
    { int p0, p1, p2, p3, x;
      note_on(0, A, A + 64, A + 128, 0x80, 0xE000, 0xFF00, 0x7F7F, 0x7F7F, 0); render(4410); peaks(4410, &p0, &x);
      IFATN(0, 0x0000); render(4410); peaks(4410, &p1, &x);
      IFATN(0, 0x6800); render(4410); peaks(4410, &p2, &x);
      { uint32_t cc = d1dr(0, 0); ptr(0, 0); outw(E + 0x402, (uint16_t)((0xF << 12) | ((cc >> 16) & 0x0FFF))); }
      render(8820); peaks(4410, &p3, &x);
      printf("        peak: open %d   cutoff 00h %d   cutoff 68h Q0 %d   Q15 %d\n", p0, p1, p2, p3);
      /* the negative peak: -16383 x FFFFh >> 16 = -16383 (the shift floors), x pan 80h's
         128/256 = -8192. Bit-exact, or something filtered it. */
      CHECK(p0 == 8192, "Q 0 + cutoff FFh: the filter does not alter the signal (p.17) -- bit-exact peak");
      CHECK(p1 < p0 * 15 / 100, "cutoff 00h (125 Hz): a 689 Hz tone is cut to under 15%");
      CHECK(p3 > p2 * 3, "Q 15 at a cutoff on the tone: resonance lifts it more than 3x");
      CHECK(d0r(2, 0) != 0 && (d0r(2, 0) & 0xFFFF) == 0x6800, "CVCF: the current cutoff follows IFATN's byte (6800h)"); }
    note_kill(0);

    /* ---- T8: output gating ---- */
    { int l, r;
      note_on(0, A, A + 64, A + 128, 0x80, 0xE000, 0xFF00, 0x7F7F, 0x7F7F, 0); render(4410);
      d1w(1, 31, 0x0000); peaks(2000, &l, &r);
      CHECK(l == 0 && r == 0, "HWCF3 = 0: audio output disabled (§4)");
      d1w(1, 31, 0x0004); peaks(2000, &l, &r);
      CHECK(l > 0, "HWCF3 = 4: audio back");
      note_kill(0);
      note_on(1, A, A + 64, A + 128, 0x80, 0xE000, 0xFF00, 0x7F7F, 0x7F7F, 0); render(4410);
      { uint32_t cc = d1dr(0, 1); CCCA(1, cc | 0x04000000u); }
      peaks(2000, &l, &r);
      CHECK(l == 0 && r == 0, "a channel in DMA mode makes no sound");
      CCCA(1, 0); note_kill(1); }

    /* ---- T9: the mixer hook ---- */
    { static audio_state au; static int16_t mo[2 * 1024]; int l = 0, r = 0;
      vdd_audio_init(&au, NULL, NULL, 44100);
      vdd_audio_set_emu8k(&au, &emu);
      note_on(0, A, A + 64, A + 128, 0xFF, 0xE000, 0xFF00, 0x7F7F, 0x7F7F, 0);
      vdd_audio_mix_st(&au, mo, 1024);
      for (i = 0; i < 1024; ++i) { if (mo[2*i]) l = 1; if (mo[2*i+1]) r = 1; }
      CHECK(l && !r, "mixer: vdd_audio_set_emu8k -- a hard-left voice reaches the host's LEFT channel only");
      vdd_audio_set_emu8k(&au, NULL); note_kill(0); }

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
