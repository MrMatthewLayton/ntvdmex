/* opl_test.c -- off-VM unit battery for the AdLib/OPL2 VDD (vdd_opl.c).
 *
 * The centrepiece is T4: the canonical AdLib detection sequence, run exactly as a
 * DOS game runs it. That sequence is a TIMER MEASUREMENT, which is why the old
 * "toggle the status bits on every read" stub was worse than useless -- it made
 * games believe a card was present and commit to a music path, while a detect that
 * looks at the flags in a different order would have been told nonsense. If T4
 * passes, a real game's detect passes for the real reason.
 *
 * The rest pins down the things that are quietly easy to get wrong: the
 * non-contiguous operator mapping (channel 3 is offsets 0x08/0x0B, not 0x06/0x09),
 * timer periods of (256 - preset) steps, mask bits suppressing flags, and the
 * key-on/key-off edges that drive the envelope generator.
 *
 * T11-T17 are the OPL3 (#232): what an OPL2 must NOT answer (0x38A/0x38B float,
 * ID bits 0x06), the second array behind 0x38A, the single 9-bit latch, and NEW
 * gating the 4-operator pairing. What the pairing and NEW do to the SOUND is
 * opl_synth_test's job.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_opl.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

static uint8_t g_flat[0x100000];
static VDD_BUS bus;

/* Write an OPL register the way hardware is driven: address to 0x388, data to
   0x389 -- so the battery exercises the port path, not just the helper. */
static void wr(uint8_t reg, uint8_t val)
{
    uint32_t v = reg; VddBusIo(&bus, 0x388, 1, 0, &v);
    v = val;          VddBusIo(&bus, 0x389, 1, 0, &v);
}
static uint8_t status(void)
{
    uint32_t v = 0; VddBusIo(&bus, 0x388, 1, 1, &v); return (uint8_t)v;
}
/* The OPL3's array-1 pair: address to 0x38A, data to 0x38B. */
static void wr3(uint8_t reg, uint8_t val)
{
    uint32_t v = reg; VddBusIo(&bus, 0x38A, 1, 0, &v);
    v = val;          VddBusIo(&bus, 0x38B, 1, 0, &v);
}
static uint8_t rdp(uint16_t port)
{
    uint32_t v = 0; VddBusIo(&bus, port, 1, 1, &v); return (uint8_t)v;
}

int main(void)
{
    OPL_STATE opl; memset(&opl, 0, sizeof opl);
    NTVDD_DEVICE dev = VddOplDevice(&opl);
    int m, c;

    printf("== sound epic: AdLib/OPL2 + OPL3 register + timer battery ==\n");

    VddBusInitialize(&bus, g_flat);
    VddBusSetSinks(&bus, 0, 0, 0, 0);
    CHECK(VddBusAdd(&bus, &dev) == 0, "add: opl device ok");
    CHECK(status() == 0x06, "reset: OPL2 status = flags clear + ID bits 1-2 set (0x06)");

    /* T1: the operator mapping ---------------------------------------------- */
    CHECK(VddOplOperatorIndex(0,0) == 0  && VddOplOperatorIndex(0,1) == 3,  "ch0 -> ops 0/3");
    CHECK(VddOplOperatorIndex(2,0) == 2  && VddOplOperatorIndex(2,1) == 5,  "ch2 -> ops 2/5");
    CHECK(VddOplOperatorIndex(3,0) == 6  && VddOplOperatorIndex(3,1) == 9,  "ch3 -> ops 6/9 (bank skip)");
    CHECK(VddOplOperatorIndex(8,0) == 14 && VddOplOperatorIndex(8,1) == 17, "ch8 -> ops 14/17");

    /* register offset 0x08 must land on operator 6, and the gap 0x06 nowhere    */
    wr(0x28, 0x0A);                                  /* 0x20 block, offset 0x08   */
    CHECK(opl.Operators[6].Multiplier == 0x0A, "reg 0x28 -> operator 6 (offset 0x08)");
    wr(0x26, 0x0F);                                  /* offset 0x06 = a gap       */
    CHECK(opl.Operators[6].Multiplier == 0x0A, "reg 0x26 is a gap: no operator touched");

    /* T2: operator parameter decode ----------------------------------------- */
    wr(0x20, 0xB7);       /* AM=1 VIB=0 EGT=1 KSR=1 MULT=7                        */
    m = 0;
    CHECK(opl.Operators[m].AmplitudeModulation == 1 && opl.Operators[m].Vibrato == 0 && opl.Operators[m].EnvelopeType == 1 &&
          opl.Operators[m].KeyScaleRate == 1 && opl.Operators[m].Multiplier == 7, "0x20: AM/VIB/EGT/KSR/MULT decoded");
    wr(0x40, 0x9F);       /* KSL=2 TL=0x1F                                        */
    CHECK(opl.Operators[m].KeyScaleLevel == 2 && opl.Operators[m].TotalLevel == 0x1F, "0x40: KSL/TL decoded");
    wr(0x60, 0xF2);       /* AR=15 DR=2                                           */
    CHECK(opl.Operators[m].AttackRate == 15 && opl.Operators[m].DecayRate == 2, "0x60: AR/DR decoded");
    wr(0x80, 0x5C);       /* SL=5 RR=12                                           */
    CHECK(opl.Operators[m].SustainLevel == 5 && opl.Operators[m].ReleaseRate == 12, "0x80: SL/RR decoded");
    wr(0xE0, 0x02);
    CHECK(opl.Operators[m].Waveform == 2, "0xE0: waveform select decoded");

    /* T3: channel registers + key-on / key-off edges ------------------------- */
    wr(0xA0, 0x98);                                  /* F-num low                 */
    wr(0xB0, 0x2D);                                  /* key-on, block=3, F-num hi=1 */
    c = 0;
    CHECK(opl.Channels[c].FNumber == 0x198, "0xA0/0xB0: 10-bit F-number assembled");
    CHECK(opl.Channels[c].Block == 3, "0xB0: block decoded");
    CHECK(opl.Channels[c].IsKeyOn == 1, "0xB0: key-on latched");
    CHECK(opl.Operators[0].EnvelopeState == OPL_ENVELOPE_ATTACK && opl.Operators[3].EnvelopeState == OPL_ENVELOPE_ATTACK,
          "key-on: both operators enter attack");
    CHECK(opl.Operators[0].Phase == 0, "key-on: phase reset");
    wr(0xB0, 0x0D);                                  /* key-off                   */
    CHECK(opl.Operators[0].EnvelopeState == OPL_ENVELOPE_RELEASE && opl.Operators[3].EnvelopeState == OPL_ENVELOPE_RELEASE,
          "key-off: both operators enter release");
    wr(0xC0, 0x0D);                                  /* FB=6 CNT=1                */
    CHECK(opl.Channels[c].Feedback == 6 && opl.Channels[c].Connection == 1, "0xC0: feedback/connection decoded");

    /* T4: THE ADLIB DETECTION SEQUENCE (what a real game does) --------------- */
    wr(0x04, 0x60);                                  /* mask both timers          */
    wr(0x04, 0x80);                                  /* reset IRQ + flags         */
    /* A detect masks with 0xE0 -- the low bits are the chip ID (see T11).        */
    CHECK((status() & 0xE0) == 0x00, "detect: status & 0xE0 reads 0x00 after reset");

    wr(0x02, 0xFF);                                  /* T1 preset: one 80us tick  */
    wr(0x04, 0x21);                                  /* start T1, T1 unmasked     */
    CHECK((status() & 0xE0) == 0x00, "detect: status still 0x00 before the delay");
    VddOplAddMicroseconds(&opl, 80);                        /* the game's ~80us delay    */
    CHECK((status() & 0xE0) == 0xC0, "detect: status & 0xE0 reads 0xC0 (IRQ|T1) after 80us  <-- THE TEST");
    CHECK(status() == 0xC6, "detect: an OPL2 reads 0xC6 whole (ID bits say YM3812)");

    wr(0x04, 0x60);
    wr(0x04, 0x80);
    CHECK((status() & 0xE0) == 0x00, "detect: flags reset again -> card confirmed present");

    /* T5: timer period is (256 - preset) steps ------------------------------- */
    VddOplReset(&opl);
    wr(0x02, 0xFE);                                  /* two 80us ticks            */
    wr(0x04, 0x01);
    VddOplAddMicroseconds(&opl, 80);
    CHECK((status() & OPL_STATUS_TIMER1) == 0, "timer1: no flag after 1 of 2 ticks");
    VddOplAddMicroseconds(&opl, 80);
    CHECK((status() & OPL_STATUS_TIMER1) != 0, "timer1: flag after 2 ticks (256-preset)");

    /* it reloads and fires again, which is how music keeps its tempo            */
    wr(0x04, 0x80);
    VddOplAddMicroseconds(&opl, 160);
    CHECK((status() & OPL_STATUS_TIMER1) != 0, "timer1: reloads and fires repeatedly");

    /* T6: timer 2 runs at 320us --------------------------------------------- */
    VddOplReset(&opl);
    wr(0x03, 0xFF);
    wr(0x04, 0x02);                                  /* start T2                  */
    VddOplAddMicroseconds(&opl, 160);
    CHECK((status() & OPL_STATUS_TIMER2) == 0, "timer2: nothing at 160us");
    VddOplAddMicroseconds(&opl, 160);
    CHECK((status() & OPL_STATUS_TIMER2) != 0, "timer2: fires at 320us");
    CHECK((status() & OPL_STATUS_IRQ) != 0, "timer2: raises the IRQ bit too");

    /* T7: mask bits suppress the flag --------------------------------------- */
    VddOplReset(&opl);
    wr(0x02, 0xFF);
    wr(0x04, 0x41);                                  /* start T1 but MASK it      */
    VddOplAddMicroseconds(&opl, 400);
    CHECK((status() & 0xE0) == 0x00, "masked timer1: overflow raises no flag");

    /* T8: a stopped timer does not advance ----------------------------------- */
    VddOplReset(&opl);
    wr(0x02, 0xFF);
    VddOplAddMicroseconds(&opl, 4000);
    CHECK((status() & 0xE0) == 0x00, "stopped timer: no flag no matter how much time passes");

    /* T9: the data port is write-only on an OPL2 ---------------------------- */
    { uint32_t v = 0; VddBusIo(&bus, 0x389, 1, 1, &v);
      CHECK(v == 0xFF, "0x389 reads 0xFF (write-only data port)"); }

    /* T10: the bus frame tick advances the timers --------------------------- */
    VddOplReset(&opl);
    opl.FrameUs = 16667;
    wr(0x02, 0x00);                                  /* 256 ticks = 20.48ms       */
    wr(0x04, 0x01);
    VddBusFrame(&bus);
    CHECK((status() & OPL_STATUS_TIMER1) == 0, "frame tick: 16.7ms is short of 20.48ms");
    VddBusFrame(&bus);
    CHECK((status() & OPL_STATUS_TIMER1) != 0, "frame tick: two frames pass 20.48ms -> flag");

    /* ── OPL3 (YMF262), GH #232 ─────────────────────────────────────────────── */

    /* T11: OPL2 fitted -- 0x38A/0x38B are not there, exactly as on an AdLib -- */
    VddOplReset(&opl);                             /* opl.opl3 == 0 (zeroed)    */
    CHECK(status() == 0x06 && (status() & 0x06) != 0, "OPL2: the OPL3 detect ((status & 6) == 0) FAILS");
    wr3(0xE0, 0x07);                                 /* array-1 waveform, op 18   */
    wr(0x20, 0x01);                                  /* put something in array 0  */
    wr3(0x20, 0x0F);
    CHECK(opl.Registers[0x1E0] == 0 && opl.Registers[0x120] == 0 && opl.Operators[18].Multiplier == 0,
          "OPL2: writes through 0x38A/0x38B go nowhere");
    CHECK(opl.Operators[0].Multiplier == 1, "OPL2: ...and do not leak into array 0");
    CHECK(rdp(0x38A) == 0xFF && rdp(0x38B) == 0xFF, "OPL2: 0x38A/0x38B read 0xFF (floating bus)");
    VddOplWriteRegister(&opl, 0x1B0, 0x20);
    CHECK(opl.Registers[0x1B0] == 0 && opl.Channels[9].IsKeyOn == 0, "OPL2: a direct array-1 write is dropped too");

    /* T12: OPL3 fitted -- the ID bits and the detect ------------------------- */
    opl.IsOpl3 = 1; VddOplReset(&opl);
    CHECK(opl.IsOpl3 == 1, "OPL3: chip type survives vdd_opl_reset");
    CHECK(status() == 0x00, "OPL3: status idles at 0x00 (ID bits 1-2 clear)");
    wr(0x04, 0x60); wr(0x04, 0x80);
    wr(0x02, 0xFF); wr(0x04, 0x21);
    VddOplAddMicroseconds(&opl, 80);
    CHECK(status() == 0xC0, "OPL3: the AdLib detect still reads 0xC0 -- an OPL3 IS an AdLib");
    CHECK(rdp(0x38A) == 0xC0, "OPL3: status readable at 0x38A too");
    CHECK(rdp(0x38B) == 0xFF && rdp(0x389) == 0xFF, "OPL3: the data ports stay write-only");
    wr(0x04, 0x80);

    /* T13: array 1 through 0x38A/0x38B lands on operators 18-35, channels 9-17 - */
    wr3(0x20, 0x0A);                                 /* array 1, op offset 0      */
    CHECK(opl.Registers[0x120] == 0x0A && opl.Operators[18].Multiplier == 0x0A, "array 1: 0x120 -> operator 18");
    CHECK(opl.Operators[0].Multiplier != 0x0A, "array 1: ...not operator 0");
    wr3(0x2B, 0x05);                                 /* offset 0x0B = ch3 carrier */
    CHECK(opl.Operators[VddOplOperatorIndex(12, 1)].Multiplier == 5 && VddOplOperatorIndex(12, 1) == 27,
          "array 1: 0x12B -> operator 27 (channel 12's carrier)");
    wr3(0xA2, 0x44); wr3(0xB2, 0x0D);
    CHECK(opl.Channels[11].FNumber == 0x144 && opl.Channels[11].Block == 3, "array 1: 0x1A2/0x1B2 -> channel 11");
    CHECK(VddOplOperatorIndex(9, 0) == 18 && VddOplOperatorIndex(17, 1) == 35, "ch9 -> op 18 ... ch17 -> op 35");
    /* ONE latch: address written at 0x38A, data through EITHER data port        */
    { uint32_t v = 0x40; VddBusIo(&bus, 0x38A, 1, 0, &v);
      v = 0x3F;          VddBusIo(&bus, 0x389, 1, 0, &v); }
    CHECK(opl.Registers[0x140] == 0x3F && opl.Operators[18].TotalLevel == 0x3F, "one 9-bit latch: 0x38A then 0x389 writes array 1");
    /* array 1's 0x104 is the 4-op register, NOT timer control                   */
    wr(0x04, 0x00);                                  /* T1 stopped (T12 ran it)   */
    wr3(0x04, 0x01);
    CHECK(opl.IsTimer1Running == 0 && opl.Registers[0x104] == 0x01, "0x104 is 4-op connect, not array 0's timer control");
    wr3(0x04, 0x00);

    /* T14: NEW gates the OPL3's extensions ---------------------------------- */
    CHECK(!VddOplIsNewMode(&opl), "NEW: clear after reset (the OPL3 powers up OPL2-compatible)");
    wr3(0x04, 0x01);                                 /* pair 0+3 -- but NEW clear */
    CHECK(OplFourOperatorRole(&opl, 0) == 0, "NEW clear: 0x104 latches but pairs nothing");
    wr3(0x05, 0x01);
    CHECK(VddOplIsNewMode(&opl), "NEW: 0x105 bit 0 sets it");
    CHECK(OplFourOperatorRole(&opl, 0) == 1 && OplFourOperatorRole(&opl, 3) == 2 && OplFourOperatorRole(&opl, 1) == 0,
          "NEW set: 0x104 bit 0 pairs channel 0 (lead) with 3");
    wr3(0x04, 0x38);
    CHECK(OplFourOperatorRole(&opl, 9) == 1 && OplFourOperatorRole(&opl, 12) == 2 &&
          OplFourOperatorRole(&opl, 11) == 1 && OplFourOperatorRole(&opl, 14) == 2 && OplFourOperatorRole(&opl, 0) == 0,
          "0x104 bits 3-5 pair array 1's 9+12, 10+13, 11+14");
    wr3(0x04, 0x01);

    /* T15: a 4-op voice is keyed from its FIRST channel, all four operators -- */
    wr(0xB3, 0x20);                                  /* the second channel's key  */
    CHECK(opl.Operators[VddOplOperatorIndex(3, 0)].EnvelopeState == OPL_ENVELOPE_OFF,
          "4-op: the second channel's key-on is ignored");
    wr(0xB0, 0x31);
    CHECK(opl.Operators[0].EnvelopeState == OPL_ENVELOPE_ATTACK && opl.Operators[3].EnvelopeState == OPL_ENVELOPE_ATTACK &&
          opl.Operators[6].EnvelopeState == OPL_ENVELOPE_ATTACK && opl.Operators[9].EnvelopeState == OPL_ENVELOPE_ATTACK,
          "4-op: key-on at 0xB0 starts operators 0, 3, 6 and 9");
    wr(0xB0, 0x11);
    CHECK(opl.Operators[6].EnvelopeState == OPL_ENVELOPE_RELEASE && opl.Operators[9].EnvelopeState == OPL_ENVELOPE_RELEASE,
          "4-op: key-off releases all four");

    /* T16: all-notes-off reaches array 1 ------------------------------------ */
    wr3(0xB4, 0x2A);                                 /* channel 13, an ordinary one */
    CHECK(opl.Channels[13].IsKeyOn == 1 && opl.Operators[VddOplOperatorIndex(13, 1)].EnvelopeState == OPL_ENVELOPE_ATTACK,
          "array 1: 0x1B4 keys channel 13");
    VddOplAllNotesOff(&opl);
    CHECK((opl.Registers[0x1B4] & 0x20) == 0 && opl.Channels[13].FNumber == 0x200 &&
          opl.Operators[VddOplOperatorIndex(13, 1)].EnvelopeState == OPL_ENVELOPE_RELEASE,
          "all-notes-off: array-1 voices released too, F-number kept");

    /* T17: reset clears NEW but keeps the chip ------------------------------- */
    VddOplReset(&opl);
    CHECK(opl.IsOpl3 == 1 && !VddOplIsNewMode(&opl) && opl.Registers[0x104] == 0, "reset: NEW and 0x104 clear, chip still OPL3");
    opl.IsOpl3 = 0; VddOplReset(&opl);

    printf("-- %d checks, %d failures --\n", total, fails);
    return fails ? 1 : 0;
}
