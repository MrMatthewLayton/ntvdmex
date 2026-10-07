/* gus_test.c -- off-VM battery for the Gravis UltraSound (src/vdd/vdd_gus.c).
 *
 * Every expectation is from docs/ref/gus.md (the UltraSound SDK v2.22), written before
 * the model was run: detection exactly as the SDK's UltraProbe/UltraPing do it, the
 * register file both ways, a voice running to its end and interrupting through the 8Fh
 * FIFO, looping, the volume curve against the SDK's own linear table, a ramp, the 2XB
 * lock-out, a DMA upload through the 8237, and timer 1.
 * #190 (T10-T17): the latches DRIVE the lines and channels they select, the 6850 UART
 * (to a byte sink, and through VddMpuFeed to a message), 2XF banks 5/6, 2X0's driver
 * power and line-out mute, card -> PC DRAM reads, and the record path's paced silence.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_gus.h"
#include "vdd_mpu.h"

static INT g_Total = 0, g_Failures = 0;
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static BYTE g_GuestMemory[0x100000];
static BYTE g_Dram[GUS_DRAM_SIZE];
static VDD_BUS g_Bus;
static DMA_STATE g_Dma;
static GUS_STATE g_Gus;
static INT g_IrqCount, g_IrqLast;
static VOID GusTestIrqSink(PVOID context, BYTE irq) { (VOID)context; g_IrqCount++; g_IrqLast = irq; }

/* #190: the host's wiring, reproduced -- the UART's bytes into a PRIVATE MPU assembler
   whose sink is "the synth". */
static BYTE   g_Transmitted[64];
static INT       g_CapturedCount;
static MPU_STATE g_Assembler;
static UINT32  g_Message; static INT g_MessageCount;
static VOID GusTestMessageSink(PVOID context, UINT32 message) { (VOID)context; g_Message = message; g_MessageCount++; }
static VOID GusTestCapture(PVOID context, BYTE byteValue)
{ (VOID)context; if (g_CapturedCount < 64) g_Transmitted[g_CapturedCount] = byteValue; g_CapturedCount++; VddMpuFeed(&g_Assembler, byteValue); }

#define B 0x240
static VOID GusTestWrite(WORD port, BYTE byteValue) { UINT32 value = byteValue; VddBusIo(&g_Bus, port, 1, 0, &value); }
static VOID GusTestWriteWord(WORD port, WORD wordValue) { UINT32 value = wordValue; VddBusIo(&g_Bus, port, 2, 0, &value); }
static BYTE GusTestRead(WORD port) { UINT32 value = 0; VddBusIo(&g_Bus, port, 1, 1, &value); return (BYTE)value; }
static WORD GusTestReadWord(WORD port) { UINT32 value = 0; VddBusIo(&g_Bus, port, 2, 1, &value); return (WORD)value; }
static VOID GusTestSetRegister8(BYTE registerIndex, BYTE byteValue)   { GusTestWrite(B + 0x103, registerIndex); GusTestWrite(B + 0x105, byteValue); }
static VOID GusTestSetRegister16(BYTE registerIndex, WORD wordValue) { GusTestWrite(B + 0x103, registerIndex); GusTestWriteWord(B + 0x104, wordValue); }
static BYTE  GusTestGetRegister8(BYTE registerIndex)  { GusTestWrite(B + 0x103, registerIndex); return GusTestRead(B + 0x105); }
static WORD GusTestGetRegister16(BYTE registerIndex) { GusTestWrite(B + 0x103, registerIndex); return GusTestReadWord(B + 0x104); }
static VOID GusTestPoke(UINT32 address, BYTE byteValue)
{ GusTestSetRegister16(0x43, (WORD)address); GusTestSetRegister8(0x44, (BYTE)(address >> 16)); GusTestWrite(B + 0x107, byteValue); }
static BYTE GusTestPeek(UINT32 address)
{ GusTestSetRegister16(0x43, (WORD)address); GusTestSetRegister8(0x44, (BYTE)(address >> 16)); return GusTestRead(B + 0x107); }
/* A voice address in the start/end register layout (ref §2.2). */
static VOID GusTestVoiceAddress(BYTE highRegister, UINT32 address)
{ GusTestSetRegister16(highRegister, (WORD)((address >> 7) & 0x1FFF)); GusTestSetRegister16((BYTE)(highRegister + 1), (WORD)((address & 0x7F) << 9)); }

INT main(VOID)
{
    INT16 output[4096];
    UINT32 index;

    printf("== Gravis UltraSound (GF1) battery ==\n");
    memset(&g_Dma, 0, sizeof g_Dma); memset(&g_Gus, 0, sizeof g_Gus);
    VddBusInitialize(&g_Bus, g_GuestMemory);
    VddBusSetSinks(&g_Bus, GusTestIrqSink, 0, 0, 0);
    { NTVDD_DEVICE device = VddDmaDevice(&g_Dma); CHECK(VddBusAdd(&g_Bus, &device) == 0, "add: dma"); }
    g_Gus.Dma = &g_Dma; g_Gus.Dram = g_Dram; g_Gus.BasePort = B;
    { NTVDD_DEVICE device = VddGusDevice(&g_Gus); CHECK(VddBusAdd(&g_Bus, &device) == 0, "add: gus at 240h (two port ranges)"); }

    /* ---- T1: detection, exactly as the SDK's UltraProbe + UltraPing (ref §8) ---- */
    GusTestSetRegister8(0x4C, 0x00); GusTestSetRegister8(0x4C, 0x01);
    GusTestPoke(0, 0xAA); GusTestPoke(1, 0x55);
    CHECK(GusTestPeek(0) == 0xAA && GusTestPeek(1) == 0x55, "detect: AAh/55h through 43h/44h/3X7 read back  <-- THE TEST");
    GusTestPoke(0xFFFFF, 0x5A);
    CHECK(GusTestPeek(0xFFFFF) == 0x5A, "dram: the 20th address bit reaches the top of 1 MB");

    /* #190: the card as ULTRINIT leaves it -- ULTRASND=240,3,3,11,11 latched, combined. */
    CHECK(g_Gus.IrqLatch == (0x05 | 0x40) && g_Gus.DmaLatch == (0x02 | 0x40) && g_Gus.MixControl == 0x09,
          "reset: latches hold ULTRASND's IRQ 11 / DMA 3 (combined), 2X0 = 09h");

    /* ---- T2: the register file ---- */
    GusTestWrite(B + 0x102, 3);
    GusTestSetRegister16(0x01, 0x1234);
    CHECK(GusTestGetRegister16(0x81) == 0x1234, "voice 3 freq: 16-bit OUT to 3X4, read back at 81h");
    GusTestWrite(B + 0x103, 0x01); GusTestWrite(B + 0x104, 0x78); GusTestWrite(B + 0x105, 0x56);
    CHECK(GusTestGetRegister16(0x81) == 0x5678, "voice 3 freq: low byte at 3X4 then high at 3X5");
    GusTestWrite(B + 0x102, 4);
    CHECK(GusTestGetRegister16(0x81) != 0x5678, "the page selects the voice: voice 4 has its own");
    GusTestSetRegister8(0x0E, 0xC0 | 31);
    CHECK(GusTestGetRegister8(0x8E) == (0xC0 | 31), "active voices: 32");
    GusTestSetRegister8(0x0E, 0xC0 | 3);
    CHECK(GusTestGetRegister8(0x8E) == (0xC0 | 13), "active voices: fewer than 14 is forced to 14");
    CHECK(VddGusRateHz(&g_Gus) >= 44090 && VddGusRateHz(&g_Gus) <= 44110, "14 voices -> 44.1 kHz");

    /* ---- T3: a voice plays to its end, stops, and interrupts through 8Fh ---- */
    for (index = 0; index < 256; ++index) g_Dram[0x1000 + index] = (BYTE)(index < 128 ? 0x40 : 0xC0);
    GusTestSetRegister8(0x4C, 0x07);                                  /* run, DAC, master IRQ */
    GusTestWrite(B + 0x102, 0);
    GusTestVoiceAddress(0x02, 0x1000); GusTestVoiceAddress(0x04, 0x1000 + 200); GusTestVoiceAddress(0x0A, 0x1000);
    GusTestSetRegister16(0x01, 0x0400);                               /* 1.0: one sample per service */
    GusTestSetRegister16(0x09, 0xFFF0);                               /* full volume */
    GusTestSetRegister8(0x0D, 0x03);                                  /* no ramp */
    g_IrqCount = 0;
    GusTestSetRegister8(0x00, 0x20);                                  /* go, 8-bit, IRQ at end */
    VddGusRender(&g_Gus, output, 64);
    { INT isNonZero = 0; for (index = 0; index < 64; ++index) if (output[index]) isNonZero = 1;
      CHECK(isNonZero && output[10] > 0, "a playing voice reaches the output with its sign"); }
    VddGusRender(&g_Gus, output, 400);
    CHECK((GusTestGetRegister8(0x80) & 0x01) != 0, "at end with no loop: the voice STOPPED");
    CHECK(g_IrqCount >= 1 && g_IrqLast == 11, "wavetable IRQ raised on IRQ 11");
    CHECK((GusTestRead(B + 0x006) & 0x20) != 0, "2X6 bit 5: a wavetable IRQ is pending");
    { BYTE irqSource = GusTestGetRegister8(0x8F);
      CHECK((irqSource & 0x1F) == 0 && !(irqSource & 0x80) && (irqSource & 0x40) && (irqSource & 0x20), "8Fh: voice 0, wavetable (bit 7 active-low)");
      CHECK(GusTestGetRegister8(0x8F) == 0xE0, "8Fh: a second read -- nothing left (E0h)"); }

    /* ---- T4: looping never stops, and stays inside the loop ---- */
    GusTestWrite(B + 0x102, 1);
    GusTestVoiceAddress(0x02, 0x1000); GusTestVoiceAddress(0x04, 0x1000 + 50); GusTestVoiceAddress(0x0A, 0x1000);
    GusTestSetRegister16(0x01, 0x0400); GusTestSetRegister16(0x09, 0xFFF0); GusTestSetRegister8(0x0D, 0x03);
    GusTestSetRegister8(0x00, 0x08);                                  /* go, loop */
    VddGusRender(&g_Gus, output, 500);
    { UINT32 position = ((UINT32)GusTestGetRegister16(0x8A) << 7) | (GusTestGetRegister16(0x8B) >> 9);
      CHECK(!(GusTestGetRegister8(0x80) & 0x01) && position >= 0x1000 && position <= 0x1000 + 50, "loop: still running, position inside [start,end]"); }

    /* ---- T5: the volume curve against the SDK's linear table (VOL1.C) ---- */
    /* index 1 = 700h, 2 = 7FFh, 4 = 8FFh, 256 = EFFh: amplitude doubles per exponent. */
    { UINT32 gainLow = VddGusVolumeGain(0x700), gainHigh = VddGusVolumeGain(0xF00);
      CHECK(gainLow > 0 && gainHigh / gainLow == 256, "volume: 700h -> F00h is x256 (8 octaves, one per exponent)");
      CHECK(VddGusVolumeGain(0x880) * 2 == VddGusVolumeGain(0x700) * 6, "volume: 880h is 3x 700h (index 3 in the SDK table)");
      CHECK(VddGusVolumeGain(0) == 0, "volume: 0 is silence"); }

    /* ---- T6: a volume ramp runs up, stops, and raises the volume IRQ ---- */
    GusTestWrite(B + 0x102, 2);
    GusTestSetRegister8(0x00, 0x03);                                  /* voice itself stopped */
    GusTestSetRegister16(0x09, 0x1000);                               /* current = 100h */
    GusTestSetRegister8(0x07, 0x10); GusTestSetRegister8(0x08, 0xE0);                /* ramp 100h -> E00h */
    GusTestSetRegister8(0x06, 0x3F);                                  /* fastest */
    g_IrqCount = 0;
    GusTestSetRegister8(0x0D, 0x20);                                  /* go up, IRQ at end */
    VddGusRender(&g_Gus, output, 200);
    CHECK((GusTestGetRegister8(0x8D) & 0x01) && (GusTestGetRegister16(0x89) >> 4) == 0xE00, "ramp: reached E00h and stopped");
    { BYTE irqSource = GusTestGetRegister8(0x8F);
      CHECK((irqSource & 0x1F) == 2 && !(irqSource & 0x40) && (irqSource & 0x80), "8Fh: voice 2, volume IRQ (bit 6 active-low)"); }

    /* ---- T7: the 2XB lock-out (ref §5) ---- */
    /* 2X0 = 09h: line in off, LINE OUT ON (bit 1 clear), latches on -- the SDK's final
       write. (This used 0Bh, which per ref §5 turns line out OFF and now mutes.) */
    GusTestWrite(B + 0x000, 0x09 | 0x40); GusTestWrite(B + 0x00B, 0x05);   /* IRQ latch: GF1 on IRQ 11 (5) */
    CHECK(g_Gus.IrqLatch == 0x05, "2X0 bit 6 then 2XB: the IRQ latch");
    GusTestWrite(B + 0x000, 0x09 | 0x40); GusTestWrite(B + 0x102, 0); GusTestWrite(B + 0x00B, 0x07);
    CHECK(g_Gus.IrqLatch == 0x05 && g_Gus.LatchLockedOut >= 1, "2XB not the NEXT write: locked out");
    GusTestWrite(B + 0x000, 0x09); GusTestWrite(B + 0x00B, 0x02);          /* DMA latch: DMA 3 (code 2) */
    CHECK(g_Gus.DmaLatch == 0x02, "2X0 bit 6 clear then 2XB: the DMA latch");

    /* ---- T8: DRAM DMA upload through the 8237 channel 3 (ref §3) ---- */
    for (index = 0; index < 64; ++index) g_GuestMemory[0x20000 + index] = (BYTE)(0x80 + index);
    { UINT32 value;
      value = 0;    VddBusIo(&g_Bus, 0x0C, 1, 0, &value);
      value = 0x00; VddBusIo(&g_Bus, 0x06, 1, 0, &value);  value = 0x00; VddBusIo(&g_Bus, 0x06, 1, 0, &value);
      value = 63;   VddBusIo(&g_Bus, 0x07, 1, 0, &value);  value = 0;    VddBusIo(&g_Bus, 0x07, 1, 0, &value);
      value = 0x02; VddBusIo(&g_Bus, 0x82, 1, 0, &value);          /* page 2 -> 20000h */
      value = 0x48 | 0x03; VddBusIo(&g_Bus, 0x0B, 1, 0, &value);   /* single, read, ch 3 */
      value = 0x03; VddBusIo(&g_Bus, 0x0A, 1, 0, &value); }        /* unmask ch 3 */
    GusTestSetRegister16(0x42, 0x2000 >> 4);                              /* DRAM 2000h */
    g_IrqCount = 0;
    GusTestSetRegister8(0x41, 0x21 | 0x80);                               /* go, TC IRQ, invert MSB */
    CHECK(g_Dram[0x2000] == 0x00 && g_Dram[0x2000 + 63] == (BYTE)((0x80 + 63) ^ 0x80), "DMA: 64 bytes into DRAM at 2000h, MSB inverted");
    CHECK(g_IrqCount >= 1, "DMA: terminal-count IRQ raised");
    CHECK((GusTestGetRegister8(0x41) & 0x40) && !(GusTestGetRegister8(0x41) & 0x40), "41h bit 6: TC pending, cleared by the read");

    /* ---- T9: timer 1 (80 us ticks, counts up to FFh) ---- */
    GusTestSetRegister8(0x46, 0xFE);                                      /* two ticks to overflow */
    GusTestSetRegister8(0x45, 0x04);                                      /* timer 1 IRQ enable */
    GusTestWrite(B + 0x008, 0x04); GusTestWrite(B + 0x009, 0x01);              /* start timer 1 */
    g_IrqCount = 0;
    VddGusRender(&g_Gus, output, 16);                         /* 16 x ~22.7 us > 160 us */
    CHECK((GusTestRead(B + 0x006) & 0x04) && g_IrqCount >= 1, "timer 1: expired, 2X6 bit 2, IRQ");

    /* ---- #189: PAN. The same kind of voice, looped, at three pan positions, rendered
       in stereo. Balance law: the near side keeps full level, the far side falls. */
    {   static INT16 stereo[2 * 64];
        INT panIndex, left0 = 0, right0 = 0, left7 = 0, right7 = 0, left15 = 0, right15 = 0, voice;
        /* All other voices silent: a STOPPED GF1 voice still outputs its held sample at
           its volume (programs ramp to zero), so stop AND mute. */
        for (voice = 0; voice < 32; ++voice) { GusTestWrite(B + 0x102, (BYTE)voice); GusTestSetRegister8(0x00, 0x03); GusTestSetRegister16(0x09, 0); }
        for (panIndex = 0; panIndex < 3; ++panIndex) {
            BYTE pan = (BYTE)(panIndex == 0 ? 0 : panIndex == 1 ? 7 : 15);
            GusTestWrite(B + 0x102, 5);
            GusTestVoiceAddress(0x02, 0x1000); GusTestVoiceAddress(0x04, 0x1000 + 120); GusTestVoiceAddress(0x0A, 0x1000);
            GusTestSetRegister16(0x01, 0x0400); GusTestSetRegister16(0x09, 0xFFF0); GusTestSetRegister8(0x0D, 0x03);
            GusTestSetRegister8(0x0C, pan);
            GusTestSetRegister8(0x00, 0x08);                          /* go, 8-bit, LOOP, no IRQ */
            VddGusRenderStereo(&g_Gus, stereo, 64);
            if (panIndex == 0) { left0 = stereo[20]; right0 = stereo[21]; }
            if (panIndex == 1) { left7 = stereo[20]; right7 = stereo[21]; }
            if (panIndex == 2) { left15 = stereo[20]; right15 = stereo[21]; }
            GusTestSetRegister8(0x00, 0x03);                          /* stop it again */
        }
        printf("        pan 0: L=%d R=%d   pan 7: L=%d R=%d   pan 15: L=%d R=%d\n",
               left0, right0, left7, right7, left15, right15);
        CHECK(left0 > 0 && right0 == 0, "pan 0: hard LEFT -- the right channel is silent");
        CHECK(right15 > 0 && left15 == 0, "pan 15: hard RIGHT -- the left channel is silent");
        CHECK(left7 == left0 && right7 > 0 && right7 < left7, "pan 7: left at full level, right just below it");
        CHECK(right15 == left0, "the near side of a hard pan is as loud as a centred voice's");
    }

    /* ==== #190: the remainder -- latches applied, mix control, the UART, record, reads ==== */

    /* Quiet the card: timer 1 stopped and its flag cleared, DMA TC read away, all
       voices stopped and silent. */
    GusTestWrite(B + 0x008, 0x04); GusTestWrite(B + 0x009, 0x00); GusTestWrite(B + 0x009, 0x80);
    GusTestSetRegister8(0x45, 0x00); (VOID)GusTestGetRegister8(0x41); (VOID)GusTestGetRegister8(0x49);
    { INT voice; for (voice = 0; voice < 32; ++voice) { GusTestWrite(B + 0x102, (BYTE)voice); GusTestSetRegister8(0x00, 0x03); GusTestSetRegister8(0x0D, 0x03); GusTestSetRegister16(0x09, 0); } }
    while (GusTestGetRegister8(0x8F) != 0xE0) {}
    CHECK(GusTestRead(B + 0x006) == 0x00, "#190 setup: 2X6 reads nothing pending");

    /* ---- T10: the IRQ latch decides the line (ref §5) ---- */
    /* GF1 on IRQ 5 (code 2), MIDI on IRQ 7 (code 4 in bits 5-3), not combined. */
    GusTestWrite(B + 0x000, 0x09 | 0x40); GusTestWrite(B + 0x00B, (BYTE)(0x02 | (0x04 << 3)));
    CHECK(g_Gus.Gf1IrqLine == 5 && g_Gus.MidiIrqLine == 7, "IRQ latch decodes: GF1 -> IRQ 5, MIDI -> IRQ 7");
    GusTestSetRegister8(0x46, 0xFF); GusTestSetRegister8(0x45, 0x04);
    GusTestWrite(B + 0x008, 0x04); GusTestWrite(B + 0x009, 0x01);
    g_IrqCount = 0; g_IrqLast = 0;
    VddGusRender(&g_Gus, output, 16);
    CHECK(g_IrqCount == 1 && g_IrqLast == 5, "timer 1 interrupts on the LATCHED line, IRQ 5 -- not the default 11");
    GusTestWrite(B + 0x009, 0x00); GusTestWrite(B + 0x009, 0x80); GusTestSetRegister8(0x45, 0x00);

    /* ---- T11: the MIDI UART (ref §9) ---- */
    {   memset(&g_Assembler, 0, sizeof g_Assembler); g_Assembler.Sink = GusTestMessageSink;
        g_Gus.MidiSink = 0;
        GusTestWrite(B + 0x100, 0x03);                                   /* master reset */
        CHECK(GusTestRead(B + 0x100) == GUS_ACIA_TRANSMIT_EMPTY, "6850 after master reset: transmitter empty, nothing else");
        GusTestWrite(B + 0x100, 0x00);                                   /* released, no IRQs */
        g_Gus.MidiSink = GusTestCapture; g_Gus.MidiSinkContext = 0;
        g_IrqCount = 0;
        GusTestWrite(B + 0x101, 0x90); GusTestWrite(B + 0x101, 0x3C); GusTestWrite(B + 0x101, 0x64);
        CHECK(g_Gus.MidiTransmitted == 3 && g_IrqCount == 0, "6850 transmit: three bytes out, no IRQ with CR6-5 = 00");
        CHECK(g_CapturedCount == 3 && g_Transmitted[0] == 0x90 && g_Transmitted[1] == 0x3C && g_Transmitted[2] == 0x64,
              "the sink sees the bytes as the wire carries them");
        CHECK(g_MessageCount == 1 && g_Message == (0x90u | (0x3Cu << 8) | (0x64u << 16)),
              "...and vdd_mpu_feed assembles them into the synth's note-on");
        GusTestWrite(B + 0x100, 0x20);                                   /* CR6-5 = 01: transmit IRQ on */
        CHECK(g_IrqCount == 1 && g_IrqLast == 7, "transmit IRQ enabled with TDRE: interrupts on the MIDI line (IRQ 7)");
        CHECK((GusTestRead(B + 0x100) & 0x82) == 0x82, "6850 status: TDRE and IRQ (bit 7)");
        CHECK((GusTestRead(B + 0x006) & 0x03) == 0x01, "2X6 bit 0: the MIDI transmit source");
        g_IrqCount = 0;
        GusTestWrite(B + 0x101, 0xF8);
        CHECK(g_IrqCount == 1 && g_IrqLast == 7, "each transmitted byte is a fresh transmit-empty edge");
        GusTestWrite(B + 0x100, 0x40);                                   /* CR6-5 = 10: RTS high, IRQ off */
        CHECK(!(GusTestRead(B + 0x006) & 0x01) && !(GusTestRead(B + 0x100) & 0x80), "CR6-5 = 10: no transmit IRQ");
        /* Combine: IRQ latch bit 6 puts MIDI on the GF1 line. */
        GusTestWrite(B + 0x000, 0x09 | 0x40); GusTestWrite(B + 0x00B, (BYTE)(0x02 | 0x40));
        g_IrqCount = 0;
        GusTestWrite(B + 0x100, 0x20);
        CHECK(g_Gus.MidiIrqLine == 5 && g_IrqCount == 1 && g_IrqLast == 5, "IRQ latch bit 6: MIDI combined onto the GF1 line (IRQ 5)");
        GusTestWrite(B + 0x100, 0x00);
        /* Loopback: 2X0 bit 5 -- received, not sent. */
        {   UINT32 before = g_Gus.MidiTransmitted;
            INT capturedBefore = g_CapturedCount;
            GusTestWrite(B + 0x000, 0x09 | 0x20);
            GusTestWrite(B + 0x100, 0x80);                               /* receive IRQ on */
            g_IrqCount = 0;
            GusTestWrite(B + 0x101, 0x5A);
            CHECK(g_CapturedCount == capturedBefore && g_Gus.MidiTransmitted == before + 1, "2X0 bit 5 loopback: the byte does not reach MIDI OUT");
            CHECK((GusTestRead(B + 0x100) & 0x81) == 0x81 && (GusTestRead(B + 0x006) & 0x02), "loopback: RDRF + IRQ, 2X6 bit 1 (MIDI receive)");
            CHECK(g_IrqCount == 1 && g_IrqLast == 5, "receive IRQ on the (combined) line");
            GusTestWrite(B + 0x101, 0xA5);
            CHECK(GusTestRead(B + 0x100) & GUS_ACIA_OVERRUN, "a second byte before the first is read: overrun");
            CHECK(GusTestRead(B + 0x101) == 0xA5 && !(GusTestRead(B + 0x100) & (GUS_ACIA_RECEIVE_FULL | GUS_ACIA_OVERRUN)), "reading data returns the byte and clears RDRF/OVRN");
            CHECK(!(GusTestRead(B + 0x006) & 0x02), "2X6 bit 1 clears with it");
            GusTestWrite(B + 0x100, 0x00); GusTestWrite(B + 0x000, 0x09);
        }
        /* The jumper register (2XF = 6): MIDI decode off -> an empty bus. */
        GusTestWrite(B + 0x00F, 6); GusTestWrite(B + 0x000, 0x09); GusTestWrite(B + 0x00B, 0x04);
        CHECK(g_Gus.Jumper == 0x04 && GusTestRead(B + 0x100) == 0xFF, "2XF=6 bank: MIDI decode off, 3X0 floats FFh");
        GusTestWrite(B + 0x000, 0x09); GusTestWrite(B + 0x00B, 0x06); GusTestWrite(B + 0x00F, 0);
        CHECK(GusTestRead(B + 0x100) == GUS_ACIA_TRANSMIT_EMPTY, "MIDI decode back on");
        CHECK(g_Gus.IrqLatch == (0x02 | 0x40), "banks 5/6 writes do not touch the IRQ latch");
    }

    /* ---- T12: 2XF = 5, "write 0 to clear power-up IRQs" ---- */
    GusTestWrite(B + 0x100, 0x20);                                       /* a held MIDI transmit IRQ */
    g_IrqCount = 0;
    GusTestWrite(B + 0x00F, 5); GusTestWrite(B + 0x000, 0x09); GusTestWrite(B + 0x00B, 0x00); GusTestWrite(B + 0x00F, 0);
    CHECK(g_Gus.IsLineUp == 0, "bank 5 write 0: the asserted line is let go");
    GusTestWrite(B + 0x100, 0x20);                                       /* re-evaluated: a fresh edge */
    CHECK(g_IrqCount == 1, "...so a source still pending interrupts afresh");
    GusTestWrite(B + 0x100, 0x00);

    /* ---- T13: 2X0 bit 3 powers the drivers: off, no IRQ and no DMA ---- */
    GusTestWrite(B + 0x000, 0x01);                                       /* line out on, drivers OFF */
    g_IrqCount = 0;
    GusTestWrite(B + 0x100, 0x20);
    CHECK(g_IrqCount == 0, "2X0 bit 3 clear: the card drives no IRQ line");
    GusTestWrite(B + 0x100, 0x00);
    GusTestWrite(B + 0x000, 0x09);

    /* ---- T14: the DMA latch decides the channel; 16 bytes up on DMA 1 ---- */
    {   UINT32 value;
        #define DMAW(panIndex,byteValue) do { value = (byteValue); VddBusIo(&g_Bus, (panIndex), 1, 0, &value); } while (0)
        GusTestWrite(B + 0x000, 0x09); GusTestWrite(B + 0x00B, (BYTE)(0x01 | (0x02 << 3)));  /* DRAM DMA 1, record DMA 3 */
        CHECK(g_Gus.DramDmaLine == 1 && g_Gus.RecordDmaLine == 3, "DMA latch decodes: DRAM -> DMA 1, record -> DMA 3");
        for (index = 0; index < 16; ++index) g_GuestMemory[0x30000 + index] = (BYTE)(0x10 + index);
        memset(g_Dram + 0x4000, 0, 16);
        DMAW(0x0C, 0); DMAW(0x02, 0x00); DMAW(0x02, 0x00); DMAW(0x03, 15); DMAW(0x03, 0);
        DMAW(0x83, 0x03);                                      /* ch 1 page 3 -> 30000h */
        DMAW(0x0B, 0x48 | 0x01);                               /* single, read, ch 1 */
        DMAW(0x0A, 0x05);                                      /* ch 1 MASKED first */
        GusTestSetRegister16(0x42, 0x4000 >> 4);
        GusTestSetRegister8(0x41, 0x21);
        CHECK(g_Gus.IsDmaWaiting && g_Dram[0x4000] == 0, "8237 channel masked: the card holds DRQ and waits");
        DMAW(0x0A, 0x01);                                      /* unmask */
        VddGusRender(&g_Gus, output, 1);
        CHECK(!g_Gus.IsDmaWaiting && g_Dram[0x4000] == 0x10 && g_Dram[0x400F] == 0x1F, "unmasked: the upload runs on the LATCHED channel 1");
        (VOID)GusTestGetRegister8(0x41);

        /* ---- T15: card -> PC, a DRAM read through the 8237 (41h bit 1) ---- */
        for (index = 0; index < 32; ++index) g_Dram[0x5000 + index] = (BYTE)(0xC0 + index);
        memset(g_GuestMemory + 0x31000, 0xEE, 40);
        DMAW(0x0C, 0); DMAW(0x02, 0x00); DMAW(0x02, 0x10); DMAW(0x03, 31); DMAW(0x03, 0);
        DMAW(0x83, 0x03);
        DMAW(0x0B, 0x44 | 0x01);                               /* single, WRITE (dev->mem), ch 1 */
        DMAW(0x0A, 0x01);
        GusTestSetRegister16(0x42, 0x5000 >> 4);
        g_IrqCount = 0;
        GusTestSetRegister8(0x41, 0x23);                                      /* go, card->PC, TC IRQ */
        CHECK(g_GuestMemory[0x31000] == 0xC0 && g_GuestMemory[0x3101F] == 0xDF && g_GuestMemory[0x31020] == 0xEE,
              "card -> PC: 32 DRAM bytes land in guest memory, and not one more");
        CHECK(g_IrqCount == 1 && g_IrqLast == 5 && g_Gus.DmaDownloads == 1, "card -> PC: terminal-count IRQ");
        CHECK(GusTestGetRegister8(0x41) & 0x40, "41h bit 6: TC pending after a read too");
        DMAW(0x0C, 0); DMAW(0x02, 0x00); DMAW(0x02, 0x10); DMAW(0x03, 0); DMAW(0x03, 0);
        DMAW(0x0A, 0x01);
        GusTestSetRegister8(0x41, 0x83);                                      /* invert MSB on the way out */
        CHECK(g_GuestMemory[0x31000] == (0xC0 ^ 0x80), "card -> PC with 41h bit 7: MSB inverted");
        (VOID)GusTestGetRegister8(0x41);

        /* ---- T16: the record path -- silence, at the 48h rate, on the RECORD channel ---- */
        memset(g_GuestMemory + 0x32000, 0xEE, 120);
        DMAW(0x0C, 0); DMAW(0x06, 0x00); DMAW(0x06, 0x20); DMAW(0x07, 99); DMAW(0x07, 0);
        DMAW(0x82, 0x03);                                      /* ch 3 page 3 -> 32000h */
        DMAW(0x0B, 0x44 | 0x03);                               /* single, write, ch 3 */
        DMAW(0x0A, 0x03);
        GusTestSetRegister8(0x48, 46);                                        /* 9878400/(16*48) = 12 862 Hz */
        g_IrqCount = 0;
        GusTestSetRegister8(0x49, 0x21);                                      /* go, mono, TC IRQ */
        VddGusRender(&g_Gus, output, 100);                        /* ~2.3 ms: ~29 samples */
        CHECK(g_Gus.SampleBytes >= 20 && g_Gus.SampleBytes <= 40 && g_IrqCount == 0,
              "record: paced by the 48h rate (~29 samples in 100 GF1 samples), not instantaneous");
        VddGusRender(&g_Gus, output, 400);
        CHECK(g_Gus.SampleBytes == 100, "record: exactly the 8237's 100 bytes");
        CHECK(g_GuestMemory[0x32000] == 0x80 && g_GuestMemory[0x32063] == 0x80 && g_GuestMemory[0x32064] == 0xEE,
              "record: every byte is the ADC's midscale (80h) -- silence, unsigned");
        CHECK(g_IrqCount == 1 && g_IrqLast == 5, "record: terminal-count IRQ on the GF1 line");
        CHECK((GusTestRead(B + 0x006) & 0x80), "2X6 bit 7: DMA TC (record)");
        { BYTE register49 = GusTestGetRegister8(0x49);
          CHECK((register49 & 0x40) && !(register49 & 0x01), "49h: TC pending in bit 6, the take stopped (bit 0 dropped)");
          CHECK(!(GusTestGetRegister8(0x49) & 0x40), "49h: TC cleared by the read"); }
        /* signed (invert MSB) and stereo */
        DMAW(0x0C, 0); DMAW(0x06, 0x00); DMAW(0x06, 0x20); DMAW(0x07, 9); DMAW(0x07, 0);
        DMAW(0x0A, 0x03);
        GusTestSetRegister8(0x49, 0x83);                                      /* go, stereo, invert MSB */
        VddGusRender(&g_Gus, output, 200);
        CHECK(g_GuestMemory[0x32000] == 0x00 && g_GuestMemory[0x32009] == 0x00 && g_GuestMemory[0x3200A] == 0x80,
              "record with 49h bit 7: silence is 00h (signed), 10 bytes");
        (VOID)GusTestGetRegister8(0x49);

        /* ---- T16b: #176 -- the 8237's CONTROLLER DISABLE holds the card exactly as the
           mask does, and the waiting DREQ shows in the 8237's status bits 7:4 ---- */
        {   UINT32 status;
            for (index = 0; index < 16; ++index) g_GuestMemory[0x33000 + index] = (BYTE)(0x50 + index);
            memset(g_Dram + 0x6000, 0, 16);
            DMAW(0x08, 0x04);                                  /* command: disable ctrl 1 */
            DMAW(0x0C, 0); DMAW(0x02, 0x00); DMAW(0x02, 0x30); DMAW(0x03, 15); DMAW(0x03, 0);
            DMAW(0x83, 0x03);                                  /* ch 1 -> 33000h */
            DMAW(0x0B, 0x48 | 0x01);                           /* single, read, ch 1 */
            DMAW(0x0A, 0x01);                                  /* UNMASKED -- only disabled */
            GusTestSetRegister16(0x42, 0x6000 >> 4);
            GusTestSetRegister8(0x41, 0x01);                                  /* go, PC -> card */
            VddGusRender(&g_Gus, output, 1);
            CHECK(g_Gus.IsDmaWaiting && g_Dram[0x6000] == 0, "8237 disabled: the upload holds DRQ and waits");
            value = 0; VddBusIo(&g_Bus, 0x08, 1, 1, &value); status = value;
            CHECK((status & 0x20) != 0, "8237 status 08h: DRQ1 pending while the controller refuses it");
            DMAW(0x08, 0x00);                                  /* re-enable */
            VddGusRender(&g_Gus, output, 1);
            CHECK(!g_Gus.IsDmaWaiting && g_Dram[0x6000] == 0x50 && g_Dram[0x600F] == 0x5F,
                  "8237 re-enabled: the upload runs");
            value = 0; VddBusIo(&g_Bus, 0x08, 1, 1, &value); status = value;
            CHECK((status & 0xF0) == 0 && (status & 0x02), "8237 status 08h: DRQ1 gone, TC1 latched");
            (VOID)GusTestGetRegister8(0x41);

            /* the ADC: sampling on, controller disabled -> no byte, DRQ3 pending */
            memset(g_GuestMemory + 0x32000, 0xEE, 16);
            DMAW(0x0C, 0); DMAW(0x06, 0x00); DMAW(0x06, 0x20); DMAW(0x07, 7); DMAW(0x07, 0);
            DMAW(0x0A, 0x03);
            DMAW(0x08, 0x04);
            GusTestSetRegister8(0x49, 0x01);                                  /* go, mono */
            VddGusRender(&g_Gus, output, 200);
            CHECK(g_GuestMemory[0x32000] == 0xEE, "8237 disabled: the ADC moves no byte");
            value = 0; VddBusIo(&g_Bus, 0x08, 1, 1, &value); status = value;
            CHECK((status & 0x80) != 0, "8237 status 08h: DRQ3 pending for the record channel");
            DMAW(0x08, 0x00);
            VddGusRender(&g_Gus, output, 200);
            CHECK(g_GuestMemory[0x32000] == 0x80 && g_GuestMemory[0x32007] == 0x80 && g_GuestMemory[0x32008] == 0xEE,
                  "8237 re-enabled: the take completes, 8 bytes");
            (VOID)GusTestGetRegister8(0x49);
        }
        #undef DMAW
    }

    /* ---- T17: 2X0 bit 1 = line out OFF mutes the output (ref §5) ---- */
    {   INT isNonZeroOn = 0, isNonZeroOff = 0;
        GusTestWrite(B + 0x102, 5);
        GusTestVoiceAddress(0x02, 0x1000); GusTestVoiceAddress(0x04, 0x1000 + 120); GusTestVoiceAddress(0x0A, 0x1000);
        GusTestSetRegister16(0x01, 0x0400); GusTestSetRegister16(0x09, 0xFFF0); GusTestSetRegister8(0x0D, 0x03); GusTestSetRegister8(0x0C, 7);
        GusTestSetRegister8(0x00, 0x08);
        VddGusRender(&g_Gus, output, 64);
        for (index = 0; index < 64; ++index) if (output[index]) isNonZeroOn++;
        GusTestWrite(B + 0x000, 0x09 | 0x02);
        VddGusRender(&g_Gus, output, 64);
        for (index = 0; index < 64; ++index) if (output[index]) isNonZeroOff++;
        CHECK(isNonZeroOn > 0 && isNonZeroOff == 0 && g_Gus.OutputMuted >= 64, "2X0 bit 1: line out disabled -> silence");
        CHECK(!(GusTestGetRegister8(0x80) & 0x01), "...while the voice itself keeps running");
        GusTestWrite(B + 0x000, 0x09);
        GusTestSetRegister8(0x00, 0x03);
    }

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
