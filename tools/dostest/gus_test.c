/* gus_test.c -- off-VM battery for the Gravis UltraSound (src/vdd/vdd_gus.c).
 *
 * Every expectation is from docs/ref/gus.md (the UltraSound SDK v2.22), written before
 * the model was run: detection exactly as the SDK's UltraProbe/UltraPing do it, the
 * register file both ways, a voice running to its end and interrupting through the 8Fh
 * FIFO, looping, the volume curve against the SDK's own linear table, a ramp, the 2XB
 * lock-out, a DMA upload through the 8237, and timer 1.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_gus.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

static uint8_t g_flat[0x100000];
static uint8_t g_dram[GUS_DRAM_SIZE];
static vdd_bus bus;
static dma_state dma;
static gus_state gus;
static int g_irq_count, g_irq_last;
static void irq_sink(void *ctx, uint8_t irq) { (void)ctx; g_irq_count++; g_irq_last = irq; }

#define B 0x240
static void wr(uint16_t p, uint8_t v) { uint32_t x = v; vdd_bus_io(&bus, p, 1, 0, &x); }
static void wrw(uint16_t p, uint16_t v) { uint32_t x = v; vdd_bus_io(&bus, p, 2, 0, &x); }
static uint8_t rd(uint16_t p) { uint32_t x = 0; vdd_bus_io(&bus, p, 1, 1, &x); return (uint8_t)x; }
static uint16_t rdw(uint16_t p) { uint32_t x = 0; vdd_bus_io(&bus, p, 2, 1, &x); return (uint16_t)x; }
static void reg8(uint8_t r, uint8_t v)   { wr(B + 0x103, r); wr(B + 0x105, v); }
static void reg16(uint8_t r, uint16_t v) { wr(B + 0x103, r); wrw(B + 0x104, v); }
static uint8_t  rreg8(uint8_t r)  { wr(B + 0x103, r); return rd(B + 0x105); }
static uint16_t rreg16(uint8_t r) { wr(B + 0x103, r); return rdw(B + 0x104); }
static void poke(uint32_t a, uint8_t v)
{ reg16(0x43, (uint16_t)a); reg8(0x44, (uint8_t)(a >> 16)); wr(B + 0x107, v); }
static uint8_t peek(uint32_t a)
{ reg16(0x43, (uint16_t)a); reg8(0x44, (uint8_t)(a >> 16)); return rd(B + 0x107); }
/* A voice address in the start/end register layout (ref §2.2). */
static void vaddr(uint8_t rhi, uint32_t a)
{ reg16(rhi, (uint16_t)((a >> 7) & 0x1FFF)); reg16((uint8_t)(rhi + 1), (uint16_t)((a & 0x7F) << 9)); }

int main(void)
{
    int16_t out[4096];
    uint32_t i;

    printf("== Gravis UltraSound (GF1) battery ==\n");
    memset(&dma, 0, sizeof dma); memset(&gus, 0, sizeof gus);
    vdd_bus_init(&bus, g_flat);
    vdd_bus_set_sinks(&bus, irq_sink, 0, 0, 0);
    { ntvdd d = vdd_dma_device(&dma); CHECK(vdd_bus_add(&bus, &d) == 0, "add: dma"); }
    gus.dma = &dma; gus.dram = g_dram; gus.base = B;
    { ntvdd d = vdd_gus_device(&gus); CHECK(vdd_bus_add(&bus, &d) == 0, "add: gus at 240h (two port ranges)"); }

    /* ---- T1: detection, exactly as the SDK's UltraProbe + UltraPing (ref §8) ---- */
    reg8(0x4C, 0x00); reg8(0x4C, 0x01);
    poke(0, 0xAA); poke(1, 0x55);
    CHECK(peek(0) == 0xAA && peek(1) == 0x55, "detect: AAh/55h through 43h/44h/3X7 read back  <-- THE TEST");
    poke(0xFFFFF, 0x5A);
    CHECK(peek(0xFFFFF) == 0x5A, "dram: the 20th address bit reaches the top of 1 MB");

    /* ---- T2: the register file ---- */
    wr(B + 0x102, 3);
    reg16(0x01, 0x1234);
    CHECK(rreg16(0x81) == 0x1234, "voice 3 freq: 16-bit OUT to 3X4, read back at 81h");
    wr(B + 0x103, 0x01); wr(B + 0x104, 0x78); wr(B + 0x105, 0x56);
    CHECK(rreg16(0x81) == 0x5678, "voice 3 freq: low byte at 3X4 then high at 3X5");
    wr(B + 0x102, 4);
    CHECK(rreg16(0x81) != 0x5678, "the page selects the voice: voice 4 has its own");
    reg8(0x0E, 0xC0 | 31);
    CHECK(rreg8(0x8E) == (0xC0 | 31), "active voices: 32");
    reg8(0x0E, 0xC0 | 3);
    CHECK(rreg8(0x8E) == (0xC0 | 13), "active voices: fewer than 14 is forced to 14");
    CHECK(vdd_gus_rate_hz(&gus) >= 44090 && vdd_gus_rate_hz(&gus) <= 44110, "14 voices -> 44.1 kHz");

    /* ---- T3: a voice plays to its end, stops, and interrupts through 8Fh ---- */
    for (i = 0; i < 256; ++i) g_dram[0x1000 + i] = (uint8_t)(i < 128 ? 0x40 : 0xC0);
    reg8(0x4C, 0x07);                                  /* run, DAC, master IRQ */
    wr(B + 0x102, 0);
    vaddr(0x02, 0x1000); vaddr(0x04, 0x1000 + 200); vaddr(0x0A, 0x1000);
    reg16(0x01, 0x0400);                               /* 1.0: one sample per service */
    reg16(0x09, 0xFFF0);                               /* full volume */
    reg8(0x0D, 0x03);                                  /* no ramp */
    g_irq_count = 0;
    reg8(0x00, 0x20);                                  /* go, 8-bit, IRQ at end */
    vdd_gus_render(&gus, out, 64);
    { int nz = 0; for (i = 0; i < 64; ++i) if (out[i]) nz = 1;
      CHECK(nz && out[10] > 0, "a playing voice reaches the output with its sign"); }
    vdd_gus_render(&gus, out, 400);
    CHECK((rreg8(0x80) & 0x01) != 0, "at end with no loop: the voice STOPPED");
    CHECK(g_irq_count >= 1 && g_irq_last == 11, "wavetable IRQ raised on IRQ 11");
    CHECK((rd(B + 0x006) & 0x20) != 0, "2X6 bit 5: a wavetable IRQ is pending");
    { uint8_t f = rreg8(0x8F);
      CHECK((f & 0x1F) == 0 && !(f & 0x80) && (f & 0x40) && (f & 0x20), "8Fh: voice 0, wavetable (bit 7 active-low)");
      CHECK(rreg8(0x8F) == 0xE0, "8Fh: a second read -- nothing left (E0h)"); }

    /* ---- T4: looping never stops, and stays inside the loop ---- */
    wr(B + 0x102, 1);
    vaddr(0x02, 0x1000); vaddr(0x04, 0x1000 + 50); vaddr(0x0A, 0x1000);
    reg16(0x01, 0x0400); reg16(0x09, 0xFFF0); reg8(0x0D, 0x03);
    reg8(0x00, 0x08);                                  /* go, loop */
    vdd_gus_render(&gus, out, 500);
    { uint32_t pos = ((uint32_t)rreg16(0x8A) << 7) | (rreg16(0x8B) >> 9);
      CHECK(!(rreg8(0x80) & 0x01) && pos >= 0x1000 && pos <= 0x1000 + 50, "loop: still running, position inside [start,end]"); }

    /* ---- T5: the volume curve against the SDK's linear table (VOL1.C) ---- */
    /* index 1 = 700h, 2 = 7FFh, 4 = 8FFh, 256 = EFFh: amplitude doubles per exponent. */
    { uint32_t g1 = vdd_gus_vol_gain(0x700), g256 = vdd_gus_vol_gain(0xF00);
      CHECK(g1 > 0 && g256 / g1 == 256, "volume: 700h -> F00h is x256 (8 octaves, one per exponent)");
      CHECK(vdd_gus_vol_gain(0x880) * 2 == vdd_gus_vol_gain(0x700) * 6, "volume: 880h is 3x 700h (index 3 in the SDK table)");
      CHECK(vdd_gus_vol_gain(0) == 0, "volume: 0 is silence"); }

    /* ---- T6: a volume ramp runs up, stops, and raises the volume IRQ ---- */
    wr(B + 0x102, 2);
    reg8(0x00, 0x03);                                  /* voice itself stopped */
    reg16(0x09, 0x1000);                               /* current = 100h */
    reg8(0x07, 0x10); reg8(0x08, 0xE0);                /* ramp 100h -> E00h */
    reg8(0x06, 0x3F);                                  /* fastest */
    g_irq_count = 0;
    reg8(0x0D, 0x20);                                  /* go up, IRQ at end */
    vdd_gus_render(&gus, out, 200);
    CHECK((rreg8(0x8D) & 0x01) && (rreg16(0x89) >> 4) == 0xE00, "ramp: reached E00h and stopped");
    { uint8_t f = rreg8(0x8F);
      CHECK((f & 0x1F) == 2 && !(f & 0x40) && (f & 0x80), "8Fh: voice 2, volume IRQ (bit 6 active-low)"); }

    /* ---- T7: the 2XB lock-out (ref §5) ---- */
    wr(B + 0x000, 0x0B | 0x40); wr(B + 0x00B, 0x05);   /* IRQ latch: GF1 on IRQ 11 (5) */
    CHECK(gus.irq_latch == 0x05, "2X0 bit 6 then 2XB: the IRQ latch");
    wr(B + 0x000, 0x0B | 0x40); wr(B + 0x102, 0); wr(B + 0x00B, 0x07);
    CHECK(gus.irq_latch == 0x05 && gus.latch_locked_out >= 1, "2XB not the NEXT write: locked out");
    wr(B + 0x000, 0x0B); wr(B + 0x00B, 0x02);          /* DMA latch: DMA 3 (code 2) */
    CHECK(gus.dma_latch == 0x02, "2X0 bit 6 clear then 2XB: the DMA latch");

    /* ---- T8: DRAM DMA upload through the 8237 channel 3 (ref §3) ---- */
    for (i = 0; i < 64; ++i) g_flat[0x20000 + i] = (uint8_t)(0x80 + i);
    { uint32_t v;
      v = 0;    vdd_bus_io(&bus, 0x0C, 1, 0, &v);
      v = 0x00; vdd_bus_io(&bus, 0x06, 1, 0, &v);  v = 0x00; vdd_bus_io(&bus, 0x06, 1, 0, &v);
      v = 63;   vdd_bus_io(&bus, 0x07, 1, 0, &v);  v = 0;    vdd_bus_io(&bus, 0x07, 1, 0, &v);
      v = 0x02; vdd_bus_io(&bus, 0x82, 1, 0, &v);          /* page 2 -> 20000h */
      v = 0x48 | 0x03; vdd_bus_io(&bus, 0x0B, 1, 0, &v);   /* single, read, ch 3 */
      v = 0x03; vdd_bus_io(&bus, 0x0A, 1, 0, &v); }        /* unmask ch 3 */
    reg16(0x42, 0x2000 >> 4);                              /* DRAM 2000h */
    g_irq_count = 0;
    reg8(0x41, 0x21 | 0x80);                               /* go, TC IRQ, invert MSB */
    CHECK(g_dram[0x2000] == 0x00 && g_dram[0x2000 + 63] == (uint8_t)(0x80 + 63) ^ 0x80, "DMA: 64 bytes into DRAM at 2000h, MSB inverted");
    CHECK(g_irq_count >= 1, "DMA: terminal-count IRQ raised");
    CHECK((rreg8(0x41) & 0x40) && !(rreg8(0x41) & 0x40), "41h bit 6: TC pending, cleared by the read");

    /* ---- T9: timer 1 (80 us ticks, counts up to FFh) ---- */
    reg8(0x46, 0xFE);                                      /* two ticks to overflow */
    reg8(0x45, 0x04);                                      /* timer 1 IRQ enable */
    wr(B + 0x008, 0x04); wr(B + 0x009, 0x01);              /* start timer 1 */
    g_irq_count = 0;
    vdd_gus_render(&gus, out, 16);                         /* 16 x ~22.7 us > 160 us */
    CHECK((rd(B + 0x006) & 0x04) && g_irq_count >= 1, "timer 1: expired, 2X6 bit 2, IRQ");

    /* ---- #189: PAN. The same kind of voice, looped, at three pan positions, rendered
       in stereo. Balance law: the near side keeps full level, the far side falls. */
    {   static int16_t st2[2 * 64];
        int p, l0 = 0, r0 = 0, l7 = 0, r7 = 0, l15 = 0, r15 = 0, vv;
        /* All other voices silent: a STOPPED GF1 voice still outputs its held sample at
           its volume (programs ramp to zero), so stop AND mute. */
        for (vv = 0; vv < 32; ++vv) { wr(B + 0x102, (uint8_t)vv); reg8(0x00, 0x03); reg16(0x09, 0); }
        for (p = 0; p < 3; ++p) {
            uint8_t pan = (uint8_t)(p == 0 ? 0 : p == 1 ? 7 : 15);
            wr(B + 0x102, 5);
            vaddr(0x02, 0x1000); vaddr(0x04, 0x1000 + 120); vaddr(0x0A, 0x1000);
            reg16(0x01, 0x0400); reg16(0x09, 0xFFF0); reg8(0x0D, 0x03);
            reg8(0x0C, pan);
            reg8(0x00, 0x08);                          /* go, 8-bit, LOOP, no IRQ */
            vdd_gus_render_st(&gus, st2, 64);
            if (p == 0) { l0 = st2[20]; r0 = st2[21]; }
            if (p == 1) { l7 = st2[20]; r7 = st2[21]; }
            if (p == 2) { l15 = st2[20]; r15 = st2[21]; }
            reg8(0x00, 0x03);                          /* stop it again */
        }
        printf("        pan 0: L=%d R=%d   pan 7: L=%d R=%d   pan 15: L=%d R=%d\n",
               l0, r0, l7, r7, l15, r15);
        CHECK(l0 > 0 && r0 == 0, "pan 0: hard LEFT -- the right channel is silent");
        CHECK(r15 > 0 && l15 == 0, "pan 15: hard RIGHT -- the left channel is silent");
        CHECK(l7 == l0 && r7 > 0 && r7 < l7, "pan 7: left at full level, right just below it");
        CHECK(r15 == l0, "the near side of a hard pan is as loud as a centred voice's");
    }

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
