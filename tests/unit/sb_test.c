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
static VDD_BUS bus;
static dma_state dma;
static opl_state opl;
static sb_state  sb;
static int g_irq_count, g_irq_last;

static void irq_sink(void *ctx, uint8_t irq)
{ (void)ctx; g_irq_count++; g_irq_last = irq; }

static void wr(uint16_t port, uint8_t v) { uint32_t x = v; VddBusIo(&bus, port, 1, 0, &x); }
static uint8_t rd(uint16_t port) { uint32_t x = 0; VddBusIo(&bus, port, 1, 1, &x); return (uint8_t)x; }

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
    v = 0;                 VddBusIo(&bus, 0x0C, 1, 0, &v);   /* clear flip-flop  */
    v = phys & 0xFF;       VddBusIo(&bus, 0x02, 1, 0, &v);
    v = (phys >> 8) & 0xFF;VddBusIo(&bus, 0x02, 1, 0, &v);
    v = (len - 1) & 0xFF;  VddBusIo(&bus, 0x03, 1, 0, &v);
    v = ((len - 1) >> 8) & 0xFF; VddBusIo(&bus, 0x03, 1, 0, &v);
    v = (phys >> 16) & 0xFF; VddBusIo(&bus, 0x83, 1, 0, &v);
    v = (uint32_t)(0x48 | 0x01 | (autoinit ? 0x10 : 0)); VddBusIo(&bus, 0x0B, 1, 0, &v);
    v = 0x01;              VddBusIo(&bus, 0x0A, 1, 0, &v);   /* unmask channel 1 */
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
    VddBusInitialize(&bus, g_flat);
    VddBusSetSinks(&bus, irq_sink, 0, 0, 0);
    { NTVDD_DEVICE d = vdd_dma_device(&dma); CHECK(VddBusAdd(&bus, &d) == 0, "add: dma ok"); }
    { NTVDD_DEVICE d = vdd_opl_device(&opl); CHECK(VddBusAdd(&bus, &d) == 0, "add: opl ok"); }
    sb.dma = &dma; sb.opl = &opl; sb.base = BASE;
    { NTVDD_DEVICE d = vdd_sb_device(&sb); CHECK(VddBusAdd(&bus, &d) == 0, "add: sb16 ok"); }

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

    /* T11b: 2x2/2x3 -- array 1 on an OPL3, another array-0 mirror on an OPL2 (#232) */
    wr(BASE + 0x2, 0xA5); wr(BASE + 0x3, 0x5A);         /* OPL2 fitted (opl3 = 0) */
    CHECK(opl.reg[0xA5] == 0x5A && opl.reg[0x1A5] == 0, "FM mirror, OPL2: 2x2/2x3 write array 0");
    CHECK(rd(BASE + 0x2) == 0xFF, "FM mirror, OPL2: 2x2 reads 0xFF");
    opl.opl3 = 1;
    wr(BASE + 0x2, 0xA6); wr(BASE + 0x3, 0x77);
    CHECK(opl.reg[0x1A6] == 0x77 && opl.reg[0xA6] == 0, "FM mirror, OPL3: 2x2/2x3 write ARRAY 1 (0x1A6)");
    wr(BASE + 0x0, 0xA7); wr(BASE + 0x1, 0x33);
    CHECK(opl.reg[0xA7] == 0x33 && opl.reg[0x1A7] == 0, "FM mirror, OPL3: 2x0/2x1 stay array 0");
    CHECK((rd(BASE + 0x0) & 0x06) == 0 && (rd(BASE + 0x2) & 0xE0) == (rd(BASE + 0x0) & 0xE0),
          "FM mirror, OPL3: status at 2x0 and 2x2, ID bits clear");
    opl.opl3 = 0;

    /* T12: a reset mid-transfer stops everything ---------------------------- */
    dma_program(0x31000, 64, 1);
    wr(BASE + 0xC, 0x1C);
    CHECK(vdd_sb_active(&sb), "reset: transfer running before reset");
    CHECK(dsp_reset(), "reset: handshake still works mid-transfer");
    CHECK(!vdd_sb_active(&sb), "reset: transfer stopped");

    /* #231: as an SB Pro (DSP 3.02) the SB16-only commands are unknown opcodes -- ignored
       and taking NO argument bytes -- so the byte after C6h is a command in its own right. */
    {   extern uint8_t g_sb_ver_major, g_sb_ver_minor;
        uint8_t om = g_sb_ver_major, on = g_sb_ver_minor, maj, min;
        g_sb_ver_major = 3; g_sb_ver_minor = 2;
        sb.model = SB_MODEL_SBPRO;
        dsp_reset();
        wr(BASE + 0xC, 0xC6);                   /* SB16 8-bit auto-init: not on an SB Pro */
        wr(BASE + 0xC, 0xE1);                   /* ...so this is read as a command        */
        maj = rd(BASE + 0xA); min = rd(BASE + 0xA);
        CHECK(maj == 3 && min == 2, "SB Pro: C6h is ignored with no arguments; E1h answers 3.02");
        CHECK(sb.xfer_mode == SB_XFER_IDLE, "SB Pro: ...and no transfer started");
        sb.model = SB_MODEL_SB16; g_sb_ver_major = om; g_sb_ver_minor = on;
        dsp_reset(); }

    /* ── T13: #176 -- NO DACK, NO SAMPLE, AND NO END OF BLOCK. ─────────────────
       With the 8237 refusing the channel (command bit 2, or the mask bit) the DSP
       waits on its DREQ: silence, no IRQ, the transfer still armed, the 8237's
       address standing still -- and the DREQ visible in status bit 5. Re-enabling
       resumes at the byte it stopped on. Before #176 the refused fetch was taken
       for the end of the block: an IRQ the card never raises, and the transfer
       dropped to IDLE so re-enabling resumed nothing. */
    {   uint32_t nd0, s;
        for (i = 0; i < 64; ++i) g_flat[0x33000 + i] = (uint8_t)i;
        dma_program(0x33000, 64, 1);
        g_irq_count = 0;
        wr(BASE + 0xC, 0x48); wr(BASE + 0xC, 0x1F); wr(BASE + 0xC, 0x00);  /* block=32 */
        wr(BASE + 0xC, 0x1C);                                              /* auto-init */
        vdd_sb_render(&sb, pcm, 8);
        CHECK(pcm[0] == (int16_t)(-128 * 256) && pcm[7] == (int16_t)((7 - 128) * 256),
              "8237 disable: 8 samples play while the controller is enabled");
        (void)rd(0x08);                                  /* drop TC1 the earlier rings latched */

        wr(0x08, DMA_CMD_DISABLE);                       /* command: disable ctrl 1 */
        nd0 = sb.out_nodack;
        vdd_sb_render(&sb, pcm, 64);
        CHECK(pcm[0] == 0 && pcm[63] == 0, "8237 disable: the DSP renders silence");
        CHECK(sb.out_nodack - nd0 == 64, "8237 disable: ...counted as no-DACK, every sample");
        CHECK(g_irq_count == 0, "8237 disable: NO IRQ -- the block did not end");
        CHECK(vdd_sb_active(&sb), "8237 disable: the transfer is still armed");
        CHECK(vdd_dma_cur_phys(&dma, 1) == 0x33008, "8237 disable: the 8237 address stood still");
        s = rd(0x08);
        CHECK((s & 0x20) != 0 && (s & 0x02) == 0, "8237 disable: status shows DRQ1 pending, no TC1");

        wr(0x08, 0x00);                                  /* re-enable               */
        vdd_sb_render(&sb, pcm, 1);
        CHECK(pcm[0] == (int16_t)((8 - 128) * 256), "8237 re-enable: resumes at byte 8, not the base");
        vdd_sb_render(&sb, pcm, 23);
        CHECK(g_irq_count == 1, "8237 re-enable: the block ends where it would have -- one IRQ");

        wr(0x0A, 0x05);                                  /* mask channel 1          */
        vdd_sb_render(&sb, pcm, 16);
        CHECK(g_irq_count == 1 && vdd_sb_active(&sb) && pcm[0] == 0,
              "8237 mask: the same hold -- silence, no IRQ, still armed");
        wr(0x0A, 0x01);                                  /* unmask                  */
        vdd_sb_render(&sb, pcm, 1);
        CHECK(pcm[0] == (int16_t)((32 - 128) * 256), "8237 unmask: resumes where it stopped");

        CHECK(dsp_reset(), "dreq: reset the DSP");
        s = rd(0x08);
        CHECK((s & 0xF0) == 0, "dreq: no transfer armed -> no DRQ in status");
    }
    printf("-- %d checks, %d failures --\n", total, fails);
    return fails ? 1 : 0;
}
