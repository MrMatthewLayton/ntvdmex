/* gus_test.c -- off-VM battery for the Gravis UltraSound (src/vdd/vdd_gus.c).
 *
 * Every expectation is from docs/ref/gus.md (the UltraSound SDK v2.22), written before
 * the model was run: detection exactly as the SDK's UltraProbe/UltraPing do it, the
 * register file both ways, a voice running to its end and interrupting through the 8Fh
 * FIFO, looping, the volume curve against the SDK's own linear table, a ramp, the 2XB
 * lock-out, a DMA upload through the 8237, and timer 1.
 * #190 (T10-T17): the latches DRIVE the lines and channels they select, the 6850 UART
 * (to a byte sink, and through vdd_mpu_feed to a message), 2XF banks 5/6, 2X0's driver
 * power and line-out mute, card -> PC DRAM reads, and the record path's paced silence.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_gus.h"
#include "vdd_mpu.h"

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

/* #190: the host's wiring, reproduced -- the UART's bytes into a PRIVATE MPU assembler
   whose sink is "the synth". */
static uint8_t   g_tx[64];
static int       gus_test_captured;
static mpu_state g_asm;
static uint32_t  g_msg; static int g_nmsg;
static void msg_sink(void *ctx, uint32_t m) { (void)ctx; g_msg = m; g_nmsg++; }
static void gus_test_capture(void *ctx, uint8_t b)
{ (void)ctx; if (gus_test_captured < 64) g_tx[gus_test_captured] = b; gus_test_captured++; vdd_mpu_feed(&g_asm, b); }

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

    /* #190: the card as ULTRINIT leaves it -- ULTRASND=240,3,3,11,11 latched, combined. */
    CHECK(gus.irq_latch == (0x05 | 0x40) && gus.dma_latch == (0x02 | 0x40) && gus.mix == 0x09,
          "reset: latches hold ULTRASND's IRQ 11 / DMA 3 (combined), 2X0 = 09h");

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
    /* 2X0 = 09h: line in off, LINE OUT ON (bit 1 clear), latches on -- the SDK's final
       write. (This used 0Bh, which per ref §5 turns line out OFF and now mutes.) */
    wr(B + 0x000, 0x09 | 0x40); wr(B + 0x00B, 0x05);   /* IRQ latch: GF1 on IRQ 11 (5) */
    CHECK(gus.irq_latch == 0x05, "2X0 bit 6 then 2XB: the IRQ latch");
    wr(B + 0x000, 0x09 | 0x40); wr(B + 0x102, 0); wr(B + 0x00B, 0x07);
    CHECK(gus.irq_latch == 0x05 && gus.latch_locked_out >= 1, "2XB not the NEXT write: locked out");
    wr(B + 0x000, 0x09); wr(B + 0x00B, 0x02);          /* DMA latch: DMA 3 (code 2) */
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
    CHECK(g_dram[0x2000] == 0x00 && g_dram[0x2000 + 63] == (uint8_t)((0x80 + 63) ^ 0x80), "DMA: 64 bytes into DRAM at 2000h, MSB inverted");
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

    /* ==== #190: the remainder -- latches applied, mix control, the UART, record, reads ==== */

    /* Quiet the card: timer 1 stopped and its flag cleared, DMA TC read away, all
       voices stopped and silent. */
    wr(B + 0x008, 0x04); wr(B + 0x009, 0x00); wr(B + 0x009, 0x80);
    reg8(0x45, 0x00); (void)rreg8(0x41); (void)rreg8(0x49);
    { int vv; for (vv = 0; vv < 32; ++vv) { wr(B + 0x102, (uint8_t)vv); reg8(0x00, 0x03); reg8(0x0D, 0x03); reg16(0x09, 0); } }
    while (rreg8(0x8F) != 0xE0) {}
    CHECK(rd(B + 0x006) == 0x00, "#190 setup: 2X6 reads nothing pending");

    /* ---- T10: the IRQ latch decides the line (ref §5) ---- */
    /* GF1 on IRQ 5 (code 2), MIDI on IRQ 7 (code 4 in bits 5-3), not combined. */
    wr(B + 0x000, 0x09 | 0x40); wr(B + 0x00B, (uint8_t)(0x02 | (0x04 << 3)));
    CHECK(gus.gf1_irq_line == 5 && gus.midi_irq_line == 7, "IRQ latch decodes: GF1 -> IRQ 5, MIDI -> IRQ 7");
    reg8(0x46, 0xFF); reg8(0x45, 0x04);
    wr(B + 0x008, 0x04); wr(B + 0x009, 0x01);
    g_irq_count = 0; g_irq_last = 0;
    vdd_gus_render(&gus, out, 16);
    CHECK(g_irq_count == 1 && g_irq_last == 5, "timer 1 interrupts on the LATCHED line, IRQ 5 -- not the default 11");
    wr(B + 0x009, 0x00); wr(B + 0x009, 0x80); reg8(0x45, 0x00);

    /* ---- T11: the MIDI UART (ref §9) ---- */
    {   memset(&g_asm, 0, sizeof g_asm); g_asm.sink = msg_sink;
        gus.midi_sink = 0;
        wr(B + 0x100, 0x03);                                   /* master reset */
        CHECK(rd(B + 0x100) == GUS_ACIA_TDRE, "6850 after master reset: transmitter empty, nothing else");
        wr(B + 0x100, 0x00);                                   /* released, no IRQs */
        gus.midi_sink = gus_test_capture; gus.midi_sink_ctx = 0;
        g_irq_count = 0;
        wr(B + 0x101, 0x90); wr(B + 0x101, 0x3C); wr(B + 0x101, 0x64);
        CHECK(gus.midi_tx == 3 && g_irq_count == 0, "6850 transmit: three bytes out, no IRQ with CR6-5 = 00");
        CHECK(gus_test_captured == 3 && g_tx[0] == 0x90 && g_tx[1] == 0x3C && g_tx[2] == 0x64,
              "the sink sees the bytes as the wire carries them");
        CHECK(g_nmsg == 1 && g_msg == (0x90u | (0x3Cu << 8) | (0x64u << 16)),
              "...and vdd_mpu_feed assembles them into the synth's note-on");
        wr(B + 0x100, 0x20);                                   /* CR6-5 = 01: transmit IRQ on */
        CHECK(g_irq_count == 1 && g_irq_last == 7, "transmit IRQ enabled with TDRE: interrupts on the MIDI line (IRQ 7)");
        CHECK((rd(B + 0x100) & 0x82) == 0x82, "6850 status: TDRE and IRQ (bit 7)");
        CHECK((rd(B + 0x006) & 0x03) == 0x01, "2X6 bit 0: the MIDI transmit source");
        g_irq_count = 0;
        wr(B + 0x101, 0xF8);
        CHECK(g_irq_count == 1 && g_irq_last == 7, "each transmitted byte is a fresh transmit-empty edge");
        wr(B + 0x100, 0x40);                                   /* CR6-5 = 10: RTS high, IRQ off */
        CHECK(!(rd(B + 0x006) & 0x01) && !(rd(B + 0x100) & 0x80), "CR6-5 = 10: no transmit IRQ");
        /* Combine: IRQ latch bit 6 puts MIDI on the GF1 line. */
        wr(B + 0x000, 0x09 | 0x40); wr(B + 0x00B, (uint8_t)(0x02 | 0x40));
        g_irq_count = 0;
        wr(B + 0x100, 0x20);
        CHECK(gus.midi_irq_line == 5 && g_irq_count == 1 && g_irq_last == 5, "IRQ latch bit 6: MIDI combined onto the GF1 line (IRQ 5)");
        wr(B + 0x100, 0x00);
        /* Loopback: 2X0 bit 5 -- received, not sent. */
        {   uint32_t before = gus.midi_tx;
            int cap0 = gus_test_captured;
            wr(B + 0x000, 0x09 | 0x20);
            wr(B + 0x100, 0x80);                               /* receive IRQ on */
            g_irq_count = 0;
            wr(B + 0x101, 0x5A);
            CHECK(gus_test_captured == cap0 && gus.midi_tx == before + 1, "2X0 bit 5 loopback: the byte does not reach MIDI OUT");
            CHECK((rd(B + 0x100) & 0x81) == 0x81 && (rd(B + 0x006) & 0x02), "loopback: RDRF + IRQ, 2X6 bit 1 (MIDI receive)");
            CHECK(g_irq_count == 1 && g_irq_last == 5, "receive IRQ on the (combined) line");
            wr(B + 0x101, 0xA5);
            CHECK(rd(B + 0x100) & GUS_ACIA_OVRN, "a second byte before the first is read: overrun");
            CHECK(rd(B + 0x101) == 0xA5 && !(rd(B + 0x100) & (GUS_ACIA_RDRF | GUS_ACIA_OVRN)), "reading data returns the byte and clears RDRF/OVRN");
            CHECK(!(rd(B + 0x006) & 0x02), "2X6 bit 1 clears with it");
            wr(B + 0x100, 0x00); wr(B + 0x000, 0x09);
        }
        /* The jumper register (2XF = 6): MIDI decode off -> an empty bus. */
        wr(B + 0x00F, 6); wr(B + 0x000, 0x09); wr(B + 0x00B, 0x04);
        CHECK(gus.jumper == 0x04 && rd(B + 0x100) == 0xFF, "2XF=6 bank: MIDI decode off, 3X0 floats FFh");
        wr(B + 0x000, 0x09); wr(B + 0x00B, 0x06); wr(B + 0x00F, 0);
        CHECK(rd(B + 0x100) == GUS_ACIA_TDRE, "MIDI decode back on");
        CHECK(gus.irq_latch == (0x02 | 0x40), "banks 5/6 writes do not touch the IRQ latch");
    }

    /* ---- T12: 2XF = 5, "write 0 to clear power-up IRQs" ---- */
    wr(B + 0x100, 0x20);                                       /* a held MIDI transmit IRQ */
    g_irq_count = 0;
    wr(B + 0x00F, 5); wr(B + 0x000, 0x09); wr(B + 0x00B, 0x00); wr(B + 0x00F, 0);
    CHECK(gus.line_up == 0, "bank 5 write 0: the asserted line is let go");
    wr(B + 0x100, 0x20);                                       /* re-evaluated: a fresh edge */
    CHECK(g_irq_count == 1, "...so a source still pending interrupts afresh");
    wr(B + 0x100, 0x00);

    /* ---- T13: 2X0 bit 3 powers the drivers: off, no IRQ and no DMA ---- */
    wr(B + 0x000, 0x01);                                       /* line out on, drivers OFF */
    g_irq_count = 0;
    wr(B + 0x100, 0x20);
    CHECK(g_irq_count == 0, "2X0 bit 3 clear: the card drives no IRQ line");
    wr(B + 0x100, 0x00);
    wr(B + 0x000, 0x09);

    /* ---- T14: the DMA latch decides the channel; 16 bytes up on DMA 1 ---- */
    {   uint32_t v;
        #define DMAW(p,x) do { v = (x); vdd_bus_io(&bus, (p), 1, 0, &v); } while (0)
        wr(B + 0x000, 0x09); wr(B + 0x00B, (uint8_t)(0x01 | (0x02 << 3)));  /* DRAM DMA 1, record DMA 3 */
        CHECK(gus.dram_dma_line == 1 && gus.rec_dma_line == 3, "DMA latch decodes: DRAM -> DMA 1, record -> DMA 3");
        for (i = 0; i < 16; ++i) g_flat[0x30000 + i] = (uint8_t)(0x10 + i);
        memset(g_dram + 0x4000, 0, 16);
        DMAW(0x0C, 0); DMAW(0x02, 0x00); DMAW(0x02, 0x00); DMAW(0x03, 15); DMAW(0x03, 0);
        DMAW(0x83, 0x03);                                      /* ch 1 page 3 -> 30000h */
        DMAW(0x0B, 0x48 | 0x01);                               /* single, read, ch 1 */
        DMAW(0x0A, 0x05);                                      /* ch 1 MASKED first */
        reg16(0x42, 0x4000 >> 4);
        reg8(0x41, 0x21);
        CHECK(gus.dma_waiting && g_dram[0x4000] == 0, "8237 channel masked: the card holds DRQ and waits");
        DMAW(0x0A, 0x01);                                      /* unmask */
        vdd_gus_render(&gus, out, 1);
        CHECK(!gus.dma_waiting && g_dram[0x4000] == 0x10 && g_dram[0x400F] == 0x1F, "unmasked: the upload runs on the LATCHED channel 1");
        (void)rreg8(0x41);

        /* ---- T15: card -> PC, a DRAM read through the 8237 (41h bit 1) ---- */
        for (i = 0; i < 32; ++i) g_dram[0x5000 + i] = (uint8_t)(0xC0 + i);
        memset(g_flat + 0x31000, 0xEE, 40);
        DMAW(0x0C, 0); DMAW(0x02, 0x00); DMAW(0x02, 0x10); DMAW(0x03, 31); DMAW(0x03, 0);
        DMAW(0x83, 0x03);
        DMAW(0x0B, 0x44 | 0x01);                               /* single, WRITE (dev->mem), ch 1 */
        DMAW(0x0A, 0x01);
        reg16(0x42, 0x5000 >> 4);
        g_irq_count = 0;
        reg8(0x41, 0x23);                                      /* go, card->PC, TC IRQ */
        CHECK(g_flat[0x31000] == 0xC0 && g_flat[0x3101F] == 0xDF && g_flat[0x31020] == 0xEE,
              "card -> PC: 32 DRAM bytes land in guest memory, and not one more");
        CHECK(g_irq_count == 1 && g_irq_last == 5 && gus.dma_downloads == 1, "card -> PC: terminal-count IRQ");
        CHECK(rreg8(0x41) & 0x40, "41h bit 6: TC pending after a read too");
        DMAW(0x0C, 0); DMAW(0x02, 0x00); DMAW(0x02, 0x10); DMAW(0x03, 0); DMAW(0x03, 0);
        DMAW(0x0A, 0x01);
        reg8(0x41, 0x83);                                      /* invert MSB on the way out */
        CHECK(g_flat[0x31000] == (0xC0 ^ 0x80), "card -> PC with 41h bit 7: MSB inverted");
        (void)rreg8(0x41);

        /* ---- T16: the record path -- silence, at the 48h rate, on the RECORD channel ---- */
        memset(g_flat + 0x32000, 0xEE, 120);
        DMAW(0x0C, 0); DMAW(0x06, 0x00); DMAW(0x06, 0x20); DMAW(0x07, 99); DMAW(0x07, 0);
        DMAW(0x82, 0x03);                                      /* ch 3 page 3 -> 32000h */
        DMAW(0x0B, 0x44 | 0x03);                               /* single, write, ch 3 */
        DMAW(0x0A, 0x03);
        reg8(0x48, 46);                                        /* 9878400/(16*48) = 12 862 Hz */
        g_irq_count = 0;
        reg8(0x49, 0x21);                                      /* go, mono, TC IRQ */
        vdd_gus_render(&gus, out, 100);                        /* ~2.3 ms: ~29 samples */
        CHECK(gus.samp_bytes >= 20 && gus.samp_bytes <= 40 && g_irq_count == 0,
              "record: paced by the 48h rate (~29 samples in 100 GF1 samples), not instantaneous");
        vdd_gus_render(&gus, out, 400);
        CHECK(gus.samp_bytes == 100, "record: exactly the 8237's 100 bytes");
        CHECK(g_flat[0x32000] == 0x80 && g_flat[0x32063] == 0x80 && g_flat[0x32064] == 0xEE,
              "record: every byte is the ADC's midscale (80h) -- silence, unsigned");
        CHECK(g_irq_count == 1 && g_irq_last == 5, "record: terminal-count IRQ on the GF1 line");
        CHECK((rd(B + 0x006) & 0x80), "2X6 bit 7: DMA TC (record)");
        { uint8_t r49 = rreg8(0x49);
          CHECK((r49 & 0x40) && !(r49 & 0x01), "49h: TC pending in bit 6, the take stopped (bit 0 dropped)");
          CHECK(!(rreg8(0x49) & 0x40), "49h: TC cleared by the read"); }
        /* signed (invert MSB) and stereo */
        DMAW(0x0C, 0); DMAW(0x06, 0x00); DMAW(0x06, 0x20); DMAW(0x07, 9); DMAW(0x07, 0);
        DMAW(0x0A, 0x03);
        reg8(0x49, 0x83);                                      /* go, stereo, invert MSB */
        vdd_gus_render(&gus, out, 200);
        CHECK(g_flat[0x32000] == 0x00 && g_flat[0x32009] == 0x00 && g_flat[0x3200A] == 0x80,
              "record with 49h bit 7: silence is 00h (signed), 10 bytes");
        (void)rreg8(0x49);

        /* ---- T16b: #176 -- the 8237's CONTROLLER DISABLE holds the card exactly as the
           mask does, and the waiting DREQ shows in the 8237's status bits 7:4 ---- */
        {   uint32_t s;
            for (i = 0; i < 16; ++i) g_flat[0x33000 + i] = (uint8_t)(0x50 + i);
            memset(g_dram + 0x6000, 0, 16);
            DMAW(0x08, 0x04);                                  /* command: disable ctrl 1 */
            DMAW(0x0C, 0); DMAW(0x02, 0x00); DMAW(0x02, 0x30); DMAW(0x03, 15); DMAW(0x03, 0);
            DMAW(0x83, 0x03);                                  /* ch 1 -> 33000h */
            DMAW(0x0B, 0x48 | 0x01);                           /* single, read, ch 1 */
            DMAW(0x0A, 0x01);                                  /* UNMASKED -- only disabled */
            reg16(0x42, 0x6000 >> 4);
            reg8(0x41, 0x01);                                  /* go, PC -> card */
            vdd_gus_render(&gus, out, 1);
            CHECK(gus.dma_waiting && g_dram[0x6000] == 0, "8237 disabled: the upload holds DRQ and waits");
            v = 0; vdd_bus_io(&bus, 0x08, 1, 1, &v); s = v;
            CHECK((s & 0x20) != 0, "8237 status 08h: DRQ1 pending while the controller refuses it");
            DMAW(0x08, 0x00);                                  /* re-enable */
            vdd_gus_render(&gus, out, 1);
            CHECK(!gus.dma_waiting && g_dram[0x6000] == 0x50 && g_dram[0x600F] == 0x5F,
                  "8237 re-enabled: the upload runs");
            v = 0; vdd_bus_io(&bus, 0x08, 1, 1, &v); s = v;
            CHECK((s & 0xF0) == 0 && (s & 0x02), "8237 status 08h: DRQ1 gone, TC1 latched");
            (void)rreg8(0x41);

            /* the ADC: sampling on, controller disabled -> no byte, DRQ3 pending */
            memset(g_flat + 0x32000, 0xEE, 16);
            DMAW(0x0C, 0); DMAW(0x06, 0x00); DMAW(0x06, 0x20); DMAW(0x07, 7); DMAW(0x07, 0);
            DMAW(0x0A, 0x03);
            DMAW(0x08, 0x04);
            reg8(0x49, 0x01);                                  /* go, mono */
            vdd_gus_render(&gus, out, 200);
            CHECK(g_flat[0x32000] == 0xEE, "8237 disabled: the ADC moves no byte");
            v = 0; vdd_bus_io(&bus, 0x08, 1, 1, &v); s = v;
            CHECK((s & 0x80) != 0, "8237 status 08h: DRQ3 pending for the record channel");
            DMAW(0x08, 0x00);
            vdd_gus_render(&gus, out, 200);
            CHECK(g_flat[0x32000] == 0x80 && g_flat[0x32007] == 0x80 && g_flat[0x32008] == 0xEE,
                  "8237 re-enabled: the take completes, 8 bytes");
            (void)rreg8(0x49);
        }
        #undef DMAW
    }

    /* ---- T17: 2X0 bit 1 = line out OFF mutes the output (ref §5) ---- */
    {   int nz_on = 0, nz_off = 0;
        wr(B + 0x102, 5);
        vaddr(0x02, 0x1000); vaddr(0x04, 0x1000 + 120); vaddr(0x0A, 0x1000);
        reg16(0x01, 0x0400); reg16(0x09, 0xFFF0); reg8(0x0D, 0x03); reg8(0x0C, 7);
        reg8(0x00, 0x08);
        vdd_gus_render(&gus, out, 64);
        for (i = 0; i < 64; ++i) if (out[i]) nz_on++;
        wr(B + 0x000, 0x09 | 0x02);
        vdd_gus_render(&gus, out, 64);
        for (i = 0; i < 64; ++i) if (out[i]) nz_off++;
        CHECK(nz_on > 0 && nz_off == 0 && gus.out_muted >= 64, "2X0 bit 1: line out disabled -> silence");
        CHECK(!(rreg8(0x80) & 0x01), "...while the voice itself keeps running");
        wr(B + 0x000, 0x09);
        reg8(0x00, 0x03);
    }

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
