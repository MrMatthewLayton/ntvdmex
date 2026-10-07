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

static INT g_Total = 0, g_Failures = 0;
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static BYTE g_GuestMemory[0x100000];
static VDD_BUS g_Bus;
static DMA_STATE g_Dma;
static OPL_STATE g_Opl;
static SB_STATE  g_Sb;
static INT g_IrqCount, g_IrqLast;

static VOID SbTestIrqSink(PVOID context, BYTE irq)
{ (VOID)context; g_IrqCount++; g_IrqLast = irq; }

static VOID SbTestWrite(WORD port, BYTE byteValue) { UINT32 value = byteValue; VddBusIo(&g_Bus, port, 1, 0, &value); }
static BYTE SbTestRead(WORD port) { UINT32 value = 0; VddBusIo(&g_Bus, port, 1, 1, &value); return (BYTE)value; }

#define BASE 0x220

/* The canonical SB detect, exactly as a DOS game performs it. */
static INT SbTestDspReset(VOID)
{
    SbTestWrite(BASE + 0x6, 1);
    SbTestWrite(BASE + 0x6, 0);
    if (!(SbTestRead(BASE + 0xE) & 0x80)) return 0;     /* no byte waiting -> no card      */
    return SbTestRead(BASE + 0xA) == 0xAA;
}

/* Program DMA channel 1 for `len` bytes at `phys`, auto-init optional.
   NOTE the mode byte carries the CHANNEL in bits 0-1: 0x48 alone programs
   channel 0, which silently leaves channel 1 single-cycle. */
static VOID SbTestDmaProgram(UINT32 physical, WORD length, INT isAutoInit)
{
    UINT32 value;
    value = 0;                 VddBusIo(&g_Bus, 0x0C, 1, 0, &value);   /* clear flip-flop  */
    value = physical & 0xFF;       VddBusIo(&g_Bus, 0x02, 1, 0, &value);
    value = (physical >> 8) & 0xFF;VddBusIo(&g_Bus, 0x02, 1, 0, &value);
    value = (length - 1) & 0xFF;  VddBusIo(&g_Bus, 0x03, 1, 0, &value);
    value = ((length - 1) >> 8) & 0xFF; VddBusIo(&g_Bus, 0x03, 1, 0, &value);
    value = (physical >> 16) & 0xFF; VddBusIo(&g_Bus, 0x83, 1, 0, &value);
    value = (UINT32)(0x48 | 0x01 | (isAutoInit ? 0x10 : 0)); VddBusIo(&g_Bus, 0x0B, 1, 0, &value);
    value = 0x01;              VddBusIo(&g_Bus, 0x0A, 1, 0, &value);   /* unmask channel 1 */
}

INT main(VOID)
{
    INT16 pcm[512];
    UINT32 index;

    printf("== sound epic: Sound Blaster 16 battery ==\n");

    memset(g_GuestMemory, 0, sizeof g_GuestMemory);
    memset(&g_Dma, 0, sizeof g_Dma);
    memset(&g_Opl, 0, sizeof g_Opl);
    memset(&g_Sb,  0, sizeof g_Sb);
    VddBusInitialize(&g_Bus, g_GuestMemory);
    VddBusSetSinks(&g_Bus, SbTestIrqSink, 0, 0, 0);
    { NTVDD_DEVICE device = VddDmaDevice(&g_Dma); CHECK(VddBusAdd(&g_Bus, &device) == 0, "add: dma ok"); }
    { NTVDD_DEVICE device = VddOplDevice(&g_Opl); CHECK(VddBusAdd(&g_Bus, &device) == 0, "add: opl ok"); }
    g_Sb.Dma = &g_Dma; g_Sb.Opl = &g_Opl; g_Sb.BasePort = BASE;
    { NTVDD_DEVICE device = VddSbDevice(&g_Sb); CHECK(VddBusAdd(&g_Bus, &device) == 0, "add: sb16 ok"); }

    /* T1: THE DETECTION HANDSHAKE ------------------------------------------ */
    CHECK(SbTestDspReset(), "detect: reset handshake returns 0xAA  <-- THE TEST");
    CHECK(!(SbTestRead(BASE + 0xE) & 0x80), "detect: status clear once the byte is read");
    /* a bare read with no reset must NOT look like a card                     */
    CHECK(SbTestRead(BASE + 0xA) == 0xFF, "detect: empty DSP queue reads 0xFF");

    /* T2: DSP version must look like an SB16 ------------------------------- */
    SbTestWrite(BASE + 0xC, 0xE1);
    CHECK((SbTestRead(BASE + 0xE) & 0x80) != 0, "version: byte available");
    { BYTE major = SbTestRead(BASE + 0xA), minor = SbTestRead(BASE + 0xA);
      CHECK(major == 4 && minor == 5, "version: reports 4.05 (Sound Blaster 16)"); }

    /* T3: identify command complements its argument ------------------------ */
    SbTestWrite(BASE + 0xC, 0xE0); SbTestWrite(BASE + 0xC, 0x5A);
    CHECK(SbTestRead(BASE + 0xA) == (BYTE)~0x5A, "identify: returns the complement");

    /* T4: sample rate, both ways ------------------------------------------- */
    SbTestWrite(BASE + 0xC, 0x40); SbTestWrite(BASE + 0xC, 165);          /* time constant        */
    CHECK(g_Sb.RateHz > 10000 && g_Sb.RateHz < 12000, "rate: time constant 165 -> ~11 kHz");
    SbTestWrite(BASE + 0xC, 0x41); SbTestWrite(BASE + 0xC, 0x56); SbTestWrite(BASE + 0xC, 0x22);  /* 22050 = 0x5622 BE */
    CHECK(g_Sb.RateHz == 22050, "rate: command 0x41 is big-endian -> 22050 Hz");

    /* T5: single-cycle 8-bit DMA playback ---------------------------------- */
    for (index = 0; index < 256; ++index) g_GuestMemory[0x30000 + index] = (BYTE)index;   /* ramp        */
    SbTestDmaProgram(0x30000, 256, 0);
    g_IrqCount = 0;
    SbTestWrite(BASE + 0xC, 0x14); SbTestWrite(BASE + 0xC, 0xFF); SbTestWrite(BASE + 0xC, 0x00);  /* 256 bytes */
    CHECK(VddSbIsActive(&g_Sb), "single-cycle: transfer is running");

    VddSbRender(&g_Sb, pcm, 128);
    CHECK(g_IrqCount == 0, "single-cycle: no IRQ half way through the block");
    /* 8-bit SB data is UNSIGNED: 0x00 is the bottom of the range, 0x80 silence  */
    CHECK(pcm[0] == (INT16)(-128 * 256), "single-cycle: unsigned 0x00 maps to full negative");

    VddSbRender(&g_Sb, pcm, 128);               /* bytes 128..255 of the ramp     */
    CHECK(pcm[0] == 0, "single-cycle: unsigned 0x80 maps to silence");
    CHECK(g_IrqCount == 1, "single-cycle: exactly one IRQ at end of block");
    CHECK(g_IrqLast == SB_DEFAULT_IRQ, "single-cycle: raised on IRQ 5");
    CHECK(!VddSbIsActive(&g_Sb), "single-cycle: stops after the block");

    /* T6: the IRQ is acknowledged by reading 2xE --------------------------- */
    CHECK(g_Sb.IsIrqPending, "irq: pending until acknowledged");
    { BYTE status = SbTestRead(BASE + 0x5); (VOID)status; }
    SbTestWrite(BASE + 0x4, 0x82);
    CHECK((SbTestRead(BASE + 0x5) & 0x01) != 0, "irq: mixer 0x82 reports the 8-bit IRQ");
    SbTestRead(BASE + 0xE);
    CHECK(!g_Sb.IsIrqPending, "irq: reading 2xE acknowledges it");

    /* T7: auto-init keeps streaming and IRQs per block ---------------------- */
    for (index = 0; index < 64; ++index) g_GuestMemory[0x31000 + index] = 0x80;
    SbTestDmaProgram(0x31000, 64, 1);
    g_IrqCount = 0;
    SbTestWrite(BASE + 0xC, 0x48); SbTestWrite(BASE + 0xC, 0x3F); SbTestWrite(BASE + 0xC, 0x00);  /* block=64 */
    SbTestWrite(BASE + 0xC, 0x1C);                                              /* auto-init */
    CHECK(VddSbIsActive(&g_Sb), "auto-init: transfer is running");
    VddSbRender(&g_Sb, pcm, 64);
    CHECK(g_IrqCount == 1, "auto-init: IRQ after the first block");
    CHECK(VddSbIsActive(&g_Sb), "auto-init: still running after the IRQ");
    VddSbRender(&g_Sb, pcm, 128);
    CHECK(g_IrqCount == 3, "auto-init: an IRQ per block, continuously");

    /* T8: pause / continue -------------------------------------------------- */
    SbTestWrite(BASE + 0xC, 0xD0);
    CHECK(!VddSbIsActive(&g_Sb), "pause: 0xD0 halts playback");
    VddSbRender(&g_Sb, pcm, 16);
    CHECK(pcm[0] == 0, "pause: renders silence");
    SbTestWrite(BASE + 0xC, 0xD4);
    CHECK(VddSbIsActive(&g_Sb), "continue: 0xD4 resumes playback");

    /* T9: SB16 16-bit signed transfer --------------------------------------- */
    for (index = 0; index < 64; index += 2) {
        g_GuestMemory[0x32000 + index]     = 0x00;
        g_GuestMemory[0x32000 + index + 1] = 0x40;                 /* 0x4000 = +16384       */
    }
    SbTestDmaProgram(0x32000, 64, 0);
    g_Sb.Dma16 = 1;                                       /* point 16-bit at ch 1  */
    g_IrqCount = 0;
    SbTestWrite(BASE + 0xC, 0xB0); SbTestWrite(BASE + 0xC, 0x10);         /* 16-bit, signed, mono  */
    SbTestWrite(BASE + 0xC, 0x1F); SbTestWrite(BASE + 0xC, 0x00);         /* 32 samples            */
    VddSbRender(&g_Sb, pcm, 32);
    CHECK(pcm[0] == 16384, "16-bit: signed little-endian sample decoded");
    CHECK(g_IrqCount == 1, "16-bit: IRQ at end of block");

    /* T10: force-IRQ command (drivers use it to verify their wiring) --------- */
    g_IrqCount = 0;
    SbTestWrite(BASE + 0xC, 0xF2);
    CHECK(g_IrqCount == 1, "0xF2: forces an IRQ immediately");

    /* T11: the FM mirror reaches the OPL ------------------------------------ */
    SbTestWrite(BASE + 0x8, 0x02);                               /* OPL timer-1 preset    */
    SbTestWrite(BASE + 0x9, 0xFF);
    CHECK(g_Opl.Timer1Preset == 0xFF, "FM mirror: 2x8/2x9 writes reach the OPL");
    SbTestWrite(BASE + 0x8, 0x04); SbTestWrite(BASE + 0x9, 0x01);         /* start timer 1         */
    VddOplAddMicroseconds(&g_Opl, 80);
    CHECK((SbTestRead(BASE + 0x8) & OPL_STATUS_TIMER1) != 0, "FM mirror: OPL status readable at 2x8");

    /* T11b: 2x2/2x3 -- array 1 on an OPL3, another array-0 mirror on an OPL2 (#232) */
    SbTestWrite(BASE + 0x2, 0xA5); SbTestWrite(BASE + 0x3, 0x5A);         /* OPL2 fitted (opl3 = 0) */
    CHECK(g_Opl.Registers[0xA5] == 0x5A && g_Opl.Registers[0x1A5] == 0, "FM mirror, OPL2: 2x2/2x3 write array 0");
    CHECK(SbTestRead(BASE + 0x2) == 0xFF, "FM mirror, OPL2: 2x2 reads 0xFF");
    g_Opl.IsOpl3 = 1;
    SbTestWrite(BASE + 0x2, 0xA6); SbTestWrite(BASE + 0x3, 0x77);
    CHECK(g_Opl.Registers[0x1A6] == 0x77 && g_Opl.Registers[0xA6] == 0, "FM mirror, OPL3: 2x2/2x3 write ARRAY 1 (0x1A6)");
    SbTestWrite(BASE + 0x0, 0xA7); SbTestWrite(BASE + 0x1, 0x33);
    CHECK(g_Opl.Registers[0xA7] == 0x33 && g_Opl.Registers[0x1A7] == 0, "FM mirror, OPL3: 2x0/2x1 stay array 0");
    CHECK((SbTestRead(BASE + 0x0) & 0x06) == 0 && (SbTestRead(BASE + 0x2) & 0xE0) == (SbTestRead(BASE + 0x0) & 0xE0),
          "FM mirror, OPL3: status at 2x0 and 2x2, ID bits clear");
    g_Opl.IsOpl3 = 0;

    /* T12: a reset mid-transfer stops everything ---------------------------- */
    SbTestDmaProgram(0x31000, 64, 1);
    SbTestWrite(BASE + 0xC, 0x1C);
    CHECK(VddSbIsActive(&g_Sb), "reset: transfer running before reset");
    CHECK(SbTestDspReset(), "reset: handshake still works mid-transfer");
    CHECK(!VddSbIsActive(&g_Sb), "reset: transfer stopped");

    /* #231: as an SB Pro (DSP 3.02) the SB16-only commands are unknown opcodes -- ignored
       and taking NO argument bytes -- so the byte after C6h is a command in its own right. */
    {   extern BYTE g_SbVersionMajor, g_SbVersionMinor;
        BYTE oldMajor = g_SbVersionMajor, oldMinor = g_SbVersionMinor, major, minor;
        g_SbVersionMajor = 3; g_SbVersionMinor = 2;
        g_Sb.Model = SB_MODEL_SBPRO;
        SbTestDspReset();
        SbTestWrite(BASE + 0xC, 0xC6);                   /* SB16 8-bit auto-init: not on an SB Pro */
        SbTestWrite(BASE + 0xC, 0xE1);                   /* ...so this is read as a command        */
        major = SbTestRead(BASE + 0xA); minor = SbTestRead(BASE + 0xA);
        CHECK(major == 3 && minor == 2, "SB Pro: C6h is ignored with no arguments; E1h answers 3.02");
        CHECK(g_Sb.TransferMode == SB_TRANSFER_IDLE, "SB Pro: ...and no transfer started");
        g_Sb.Model = SB_MODEL_SB16; g_SbVersionMajor = oldMajor; g_SbVersionMinor = oldMinor;
        SbTestDspReset(); }

    /* ── T13: #176 -- NO DACK, NO SAMPLE, AND NO END OF BLOCK. ─────────────────
       With the 8237 refusing the channel (command bit 2, or the mask bit) the DSP
       waits on its DREQ: silence, no IRQ, the transfer still armed, the 8237's
       address standing still -- and the DREQ visible in status bit 5. Re-enabling
       resumes at the byte it stopped on. Before #176 the refused fetch was taken
       for the end of the block: an IRQ the card never raises, and the transfer
       dropped to IDLE so re-enabling resumed nothing. */
    {   UINT32 noDackBefore, status;
        for (index = 0; index < 64; ++index) g_GuestMemory[0x33000 + index] = (BYTE)index;
        SbTestDmaProgram(0x33000, 64, 1);
        g_IrqCount = 0;
        SbTestWrite(BASE + 0xC, 0x48); SbTestWrite(BASE + 0xC, 0x1F); SbTestWrite(BASE + 0xC, 0x00);  /* block=32 */
        SbTestWrite(BASE + 0xC, 0x1C);                                              /* auto-init */
        VddSbRender(&g_Sb, pcm, 8);
        CHECK(pcm[0] == (INT16)(-128 * 256) && pcm[7] == (INT16)((7 - 128) * 256),
              "8237 disable: 8 samples play while the controller is enabled");
        (VOID)SbTestRead(0x08);                                  /* drop TC1 the earlier rings latched */

        SbTestWrite(0x08, DMA_COMMAND_DISABLE);                       /* command: disable ctrl 1 */
        noDackBefore = g_Sb.OutputNoDack;
        VddSbRender(&g_Sb, pcm, 64);
        CHECK(pcm[0] == 0 && pcm[63] == 0, "8237 disable: the DSP renders silence");
        CHECK(g_Sb.OutputNoDack - noDackBefore == 64, "8237 disable: ...counted as no-DACK, every sample");
        CHECK(g_IrqCount == 0, "8237 disable: NO IRQ -- the block did not end");
        CHECK(VddSbIsActive(&g_Sb), "8237 disable: the transfer is still armed");
        CHECK(VddDmaCurrentPhysical(&g_Dma, 1) == 0x33008, "8237 disable: the 8237 address stood still");
        status = SbTestRead(0x08);
        CHECK((status & 0x20) != 0 && (status & 0x02) == 0, "8237 disable: status shows DRQ1 pending, no TC1");

        SbTestWrite(0x08, 0x00);                                  /* re-enable               */
        VddSbRender(&g_Sb, pcm, 1);
        CHECK(pcm[0] == (INT16)((8 - 128) * 256), "8237 re-enable: resumes at byte 8, not the base");
        VddSbRender(&g_Sb, pcm, 23);
        CHECK(g_IrqCount == 1, "8237 re-enable: the block ends where it would have -- one IRQ");

        SbTestWrite(0x0A, 0x05);                                  /* mask channel 1          */
        VddSbRender(&g_Sb, pcm, 16);
        CHECK(g_IrqCount == 1 && VddSbIsActive(&g_Sb) && pcm[0] == 0,
              "8237 mask: the same hold -- silence, no IRQ, still armed");
        SbTestWrite(0x0A, 0x01);                                  /* unmask                  */
        VddSbRender(&g_Sb, pcm, 1);
        CHECK(pcm[0] == (INT16)((32 - 128) * 256), "8237 unmask: resumes where it stopped");

        CHECK(SbTestDspReset(), "dreq: reset the DSP");
        status = SbTestRead(0x08);
        CHECK((status & 0xF0) == 0, "dreq: no transfer armed -> no DRQ in status");
    }
    printf("-- %d checks, %d failures --\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
