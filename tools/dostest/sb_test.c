/* sb_test.c -- off-VM unit battery for the Sound Blaster 16 VDD (vdd_sb.c).
 *
 * T1 is the one that matters most right now: the DSP reset handshake. Skyroads
 * sweeps every standard base address (0x210-0x260) writing 1 then 0 to base+6 and
 * reading base+A, looking for 0xAA. Finding nothing, it waits forever. If T1
 * passes, that sweep finds a card at 0x220 for the right reason.
 *
 * The rest covers what a game does next: ask the DSP version (it must look like a
 * real SB16 or the 16-bit and auto-init commands are never used), set a sample
 * rate, program a DMA block, and -- the part everything depends on -- get an IRQ
 * when the block completes, then another, and another, from an auto-init ring.
 *
 * Runs entirely off-VM: a plain array is guest memory, and the IRQ sink counts
 * interrupts instead of raising them.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_sb.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

static uint8_t g_flat[0x100000];
static vdd_bus bus;
static dma_state dma;
static opl_state opl;
static sb_state  sb;
static int g_irq_count, g_irq_last;

static void irq_sink(void *ctx, uint8_t irq)
{ (void)ctx; g_irq_count++; g_irq_last = irq; }

static void wr(uint16_t port, uint8_t v) { uint32_t x = v; vdd_bus_io(&bus, port, 1, 0, &x); }
static uint8_t rd(uint16_t port) { uint32_t x = 0; vdd_bus_io(&bus, port, 1, 1, &x); return (uint8_t)x; }

#define BASE 0x220

/* The canonical SB detect, exactly as a DOS game performs it. */
static int dsp_reset(void)
{
    wr(BASE + 0x6, 1);
    wr(BASE + 0x6, 0);
    if (!(rd(BASE + 0xE) & 0x80)) return 0;     /* no byte waiting -> no card      */
    return rd(BASE + 0xA) == 0xAA;
}

/* Program DMA channel 1 for `len` bytes at `phys`, auto-init optional.
   NOTE the mode byte carries the CHANNEL in bits 0-1: 0x48 alone programs
   channel 0, which silently leaves channel 1 single-cycle. */
static void dma_program(uint32_t phys, uint16_t len, int autoinit)
{
    uint32_t v;
    v = 0;                 vdd_bus_io(&bus, 0x0C, 1, 0, &v);   /* clear flip-flop  */
    v = phys & 0xFF;       vdd_bus_io(&bus, 0x02, 1, 0, &v);
    v = (phys >> 8) & 0xFF;vdd_bus_io(&bus, 0x02, 1, 0, &v);
    v = (len - 1) & 0xFF;  vdd_bus_io(&bus, 0x03, 1, 0, &v);
    v = ((len - 1) >> 8) & 0xFF; vdd_bus_io(&bus, 0x03, 1, 0, &v);
    v = (phys >> 16) & 0xFF; vdd_bus_io(&bus, 0x83, 1, 0, &v);
    v = (uint32_t)(0x48 | 0x01 | (autoinit ? 0x10 : 0)); vdd_bus_io(&bus, 0x0B, 1, 0, &v);
    v = 0x01;              vdd_bus_io(&bus, 0x0A, 1, 0, &v);   /* unmask channel 1 */
}

int main(void)
{
    int16_t pcm[512];
    uint32_t i;

    printf("== sound epic: Sound Blaster 16 battery ==\n");

    memset(g_flat, 0, sizeof g_flat);
    memset(&dma, 0, sizeof dma);
    memset(&opl, 0, sizeof opl);
    memset(&sb,  0, sizeof sb);
    vdd_bus_init(&bus, g_flat);
    vdd_bus_set_sinks(&bus, irq_sink, 0, 0, 0);
    { ntvdd d = vdd_dma_device(&dma); CHECK(vdd_bus_add(&bus, &d) == 0, "add: dma ok"); }
    { ntvdd d = vdd_opl_device(&opl); CHECK(vdd_bus_add(&bus, &d) == 0, "add: opl ok"); }
    sb.dma = &dma; sb.opl = &opl; sb.base = BASE;
    { ntvdd d = vdd_sb_device(&sb); CHECK(vdd_bus_add(&bus, &d) == 0, "add: sb16 ok"); }

    /* T1: THE DETECTION HANDSHAKE ------------------------------------------ */
    CHECK(dsp_reset(), "detect: reset handshake returns 0xAA  <-- THE TEST");
    CHECK(!(rd(BASE + 0xE) & 0x80), "detect: status clear once the byte is read");
    /* a bare read with no reset must NOT look like a card                     */
    CHECK(rd(BASE + 0xA) == 0xFF, "detect: empty DSP queue reads 0xFF");

    /* T2: DSP version must look like an SB16 ------------------------------- */
    wr(BASE + 0xC, 0xE1);
    CHECK((rd(BASE + 0xE) & 0x80) != 0, "version: byte available");
    { uint8_t maj = rd(BASE + 0xA), min = rd(BASE + 0xA);
      CHECK(maj == 4 && min == 5, "version: reports 4.05 (Sound Blaster 16)"); }

    /* T3: identify command complements its argument ------------------------ */
    wr(BASE + 0xC, 0xE0); wr(BASE + 0xC, 0x5A);
    CHECK(rd(BASE + 0xA) == (uint8_t)~0x5A, "identify: returns the complement");

    /* T4: sample rate, both ways ------------------------------------------- */
    wr(BASE + 0xC, 0x40); wr(BASE + 0xC, 165);          /* time constant        */
    CHECK(sb.rate_hz > 10000 && sb.rate_hz < 12000, "rate: time constant 165 -> ~11 kHz");
    wr(BASE + 0xC, 0x41); wr(BASE + 0xC, 0x56); wr(BASE + 0xC, 0x22);  /* 22050 = 0x5622 BE */
    CHECK(sb.rate_hz == 22050, "rate: command 0x41 is big-endian -> 22050 Hz");

    /* T5: single-cycle 8-bit DMA playback ---------------------------------- */
    for (i = 0; i < 256; ++i) g_flat[0x30000 + i] = (uint8_t)i;   /* ramp        */
    dma_program(0x30000, 256, 0);
    g_irq_count = 0;
    wr(BASE + 0xC, 0x14); wr(BASE + 0xC, 0xFF); wr(BASE + 0xC, 0x00);  /* 256 bytes */
    CHECK(vdd_sb_active(&sb), "single-cycle: transfer is running");

    vdd_sb_render(&sb, pcm, 128);
    CHECK(g_irq_count == 0, "single-cycle: no IRQ half way through the block");
    /* 8-bit SB data is UNSIGNED: 0x00 is the bottom of the range, 0x80 silence  */
    CHECK(pcm[0] == (int16_t)(-128 * 256), "single-cycle: unsigned 0x00 maps to full negative");

    vdd_sb_render(&sb, pcm, 128);               /* bytes 128..255 of the ramp     */
    CHECK(pcm[0] == 0, "single-cycle: unsigned 0x80 maps to silence");
    CHECK(g_irq_count == 1, "single-cycle: exactly one IRQ at end of block");
    CHECK(g_irq_last == SB_DEFAULT_IRQ, "single-cycle: raised on IRQ 5");
    CHECK(!vdd_sb_active(&sb), "single-cycle: stops after the block");

    /* T6: the IRQ is acknowledged by reading 2xE --------------------------- */
    CHECK(sb.irq_pending, "irq: pending until acknowledged");
    { uint8_t s = rd(BASE + 0x5); (void)s; }
    wr(BASE + 0x4, 0x82);
    CHECK((rd(BASE + 0x5) & 0x01) != 0, "irq: mixer 0x82 reports the 8-bit IRQ");
    rd(BASE + 0xE);
    CHECK(!sb.irq_pending, "irq: reading 2xE acknowledges it");

    /* T7: auto-init keeps streaming and IRQs per block ---------------------- */
    for (i = 0; i < 64; ++i) g_flat[0x31000 + i] = 0x80;
    dma_program(0x31000, 64, 1);
    g_irq_count = 0;
    wr(BASE + 0xC, 0x48); wr(BASE + 0xC, 0x3F); wr(BASE + 0xC, 0x00);  /* block=64 */
    wr(BASE + 0xC, 0x1C);                                              /* auto-init */
    CHECK(vdd_sb_active(&sb), "auto-init: transfer is running");
    vdd_sb_render(&sb, pcm, 64);
    CHECK(g_irq_count == 1, "auto-init: IRQ after the first block");
    CHECK(vdd_sb_active(&sb), "auto-init: still running after the IRQ");
    vdd_sb_render(&sb, pcm, 128);
    CHECK(g_irq_count == 3, "auto-init: an IRQ per block, continuously");

    /* T8: pause / continue -------------------------------------------------- */
    wr(BASE + 0xC, 0xD0);
    CHECK(!vdd_sb_active(&sb), "pause: 0xD0 halts playback");
    vdd_sb_render(&sb, pcm, 16);
    CHECK(pcm[0] == 0, "pause: renders silence");
    wr(BASE + 0xC, 0xD4);
    CHECK(vdd_sb_active(&sb), "continue: 0xD4 resumes playback");

    /* T9: SB16 16-bit signed transfer --------------------------------------- */
    for (i = 0; i < 64; i += 2) {
        g_flat[0x32000 + i]     = 0x00;
        g_flat[0x32000 + i + 1] = 0x40;                 /* 0x4000 = +16384       */
    }
    dma_program(0x32000, 64, 0);
    sb.dma16 = 1;                                       /* point 16-bit at ch 1  */
    g_irq_count = 0;
    wr(BASE + 0xC, 0xB0); wr(BASE + 0xC, 0x10);         /* 16-bit, signed, mono  */
    wr(BASE + 0xC, 0x1F); wr(BASE + 0xC, 0x00);         /* 32 samples            */
    vdd_sb_render(&sb, pcm, 32);
    CHECK(pcm[0] == 16384, "16-bit: signed little-endian sample decoded");
    CHECK(g_irq_count == 1, "16-bit: IRQ at end of block");

    /* T10: force-IRQ command (drivers use it to verify their wiring) --------- */
    g_irq_count = 0;
    wr(BASE + 0xC, 0xF2);
    CHECK(g_irq_count == 1, "0xF2: forces an IRQ immediately");

    /* T11: the FM mirror reaches the OPL ------------------------------------ */
    wr(BASE + 0x8, 0x02);                               /* OPL timer-1 preset    */
    wr(BASE + 0x9, 0xFF);
    CHECK(opl.t1_preset == 0xFF, "FM mirror: 2x8/2x9 writes reach the OPL");
    wr(BASE + 0x8, 0x04); wr(BASE + 0x9, 0x01);         /* start timer 1         */
    vdd_opl_add_us(&opl, 80);
    CHECK((rd(BASE + 0x8) & OPL_ST_T1) != 0, "FM mirror: OPL status readable at 2x8");

    /* T12: a reset mid-transfer stops everything ---------------------------- */
    dma_program(0x31000, 64, 1);
    wr(BASE + 0xC, 0x1C);
    CHECK(vdd_sb_active(&sb), "reset: transfer running before reset");
    CHECK(dsp_reset(), "reset: handshake still works mid-transfer");
    CHECK(!vdd_sb_active(&sb), "reset: transfer stopped");

    printf("-- %d checks, %d failures --\n", total, fails);
    return fails ? 1 : 0;
}
