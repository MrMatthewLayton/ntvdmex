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
    opl_state opl; memset(&opl, 0, sizeof opl);
    NTVDD_DEVICE dev = vdd_opl_device(&opl);
    int m, c;

    printf("== sound epic: AdLib/OPL2 + OPL3 register + timer battery ==\n");

    VddBusInitialize(&bus, g_flat);
    VddBusSetSinks(&bus, 0, 0, 0, 0);
    CHECK(VddBusAdd(&bus, &dev) == 0, "add: opl device ok");
    CHECK(status() == 0x06, "reset: OPL2 status = flags clear + ID bits 1-2 set (0x06)");

    /* T1: the operator mapping ---------------------------------------------- */
    CHECK(vdd_opl_op_index(0,0) == 0  && vdd_opl_op_index(0,1) == 3,  "ch0 -> ops 0/3");
    CHECK(vdd_opl_op_index(2,0) == 2  && vdd_opl_op_index(2,1) == 5,  "ch2 -> ops 2/5");
    CHECK(vdd_opl_op_index(3,0) == 6  && vdd_opl_op_index(3,1) == 9,  "ch3 -> ops 6/9 (bank skip)");
    CHECK(vdd_opl_op_index(8,0) == 14 && vdd_opl_op_index(8,1) == 17, "ch8 -> ops 14/17");

    /* register offset 0x08 must land on operator 6, and the gap 0x06 nowhere    */
    wr(0x28, 0x0A);                                  /* 0x20 block, offset 0x08   */
    CHECK(opl.op[6].mult == 0x0A, "reg 0x28 -> operator 6 (offset 0x08)");
    wr(0x26, 0x0F);                                  /* offset 0x06 = a gap       */
    CHECK(opl.op[6].mult == 0x0A, "reg 0x26 is a gap: no operator touched");

    /* T2: operator parameter decode ----------------------------------------- */
    wr(0x20, 0xB7);       /* AM=1 VIB=0 EGT=1 KSR=1 MULT=7                        */
    m = 0;
    CHECK(opl.op[m].am == 1 && opl.op[m].vib == 0 && opl.op[m].egt == 1 &&
          opl.op[m].ksr == 1 && opl.op[m].mult == 7, "0x20: AM/VIB/EGT/KSR/MULT decoded");
    wr(0x40, 0x9F);       /* KSL=2 TL=0x1F                                        */
    CHECK(opl.op[m].ksl == 2 && opl.op[m].tl == 0x1F, "0x40: KSL/TL decoded");
    wr(0x60, 0xF2);       /* AR=15 DR=2                                           */
    CHECK(opl.op[m].ar == 15 && opl.op[m].dr == 2, "0x60: AR/DR decoded");
    wr(0x80, 0x5C);       /* SL=5 RR=12                                           */
    CHECK(opl.op[m].sl == 5 && opl.op[m].rr == 12, "0x80: SL/RR decoded");
    wr(0xE0, 0x02);
    CHECK(opl.op[m].wave == 2, "0xE0: waveform select decoded");

    /* T3: channel registers + key-on / key-off edges ------------------------- */
    wr(0xA0, 0x98);                                  /* F-num low                 */
    wr(0xB0, 0x2D);                                  /* key-on, block=3, F-num hi=1 */
    c = 0;
    CHECK(opl.ch[c].fnum == 0x198, "0xA0/0xB0: 10-bit F-number assembled");
    CHECK(opl.ch[c].block == 3, "0xB0: block decoded");
    CHECK(opl.ch[c].keyon == 1, "0xB0: key-on latched");
    CHECK(opl.op[0].eg_state == OPL_EG_ATTACK && opl.op[3].eg_state == OPL_EG_ATTACK,
          "key-on: both operators enter attack");
    CHECK(opl.op[0].phase == 0, "key-on: phase reset");
    wr(0xB0, 0x0D);                                  /* key-off                   */
    CHECK(opl.op[0].eg_state == OPL_EG_RELEASE && opl.op[3].eg_state == OPL_EG_RELEASE,
          "key-off: both operators enter release");
    wr(0xC0, 0x0D);                                  /* FB=6 CNT=1                */
    CHECK(opl.ch[c].fb == 6 && opl.ch[c].cnt == 1, "0xC0: feedback/connection decoded");

    /* T4: THE ADLIB DETECTION SEQUENCE (what a real game does) --------------- */
    wr(0x04, 0x60);                                  /* mask both timers          */
    wr(0x04, 0x80);                                  /* reset IRQ + flags         */
    /* A detect masks with 0xE0 -- the low bits are the chip ID (see T11).        */
    CHECK((status() & 0xE0) == 0x00, "detect: status & 0xE0 reads 0x00 after reset");

    wr(0x02, 0xFF);                                  /* T1 preset: one 80us tick  */
    wr(0x04, 0x21);                                  /* start T1, T1 unmasked     */
    CHECK((status() & 0xE0) == 0x00, "detect: status still 0x00 before the delay");
    vdd_opl_add_us(&opl, 80);                        /* the game's ~80us delay    */
    CHECK((status() & 0xE0) == 0xC0, "detect: status & 0xE0 reads 0xC0 (IRQ|T1) after 80us  <-- THE TEST");
    CHECK(status() == 0xC6, "detect: an OPL2 reads 0xC6 whole (ID bits say YM3812)");

    wr(0x04, 0x60);
    wr(0x04, 0x80);
    CHECK((status() & 0xE0) == 0x00, "detect: flags reset again -> card confirmed present");

    /* T5: timer period is (256 - preset) steps ------------------------------- */
    vdd_opl_reset(&opl);
    wr(0x02, 0xFE);                                  /* two 80us ticks            */
    wr(0x04, 0x01);
    vdd_opl_add_us(&opl, 80);
    CHECK((status() & OPL_ST_T1) == 0, "timer1: no flag after 1 of 2 ticks");
    vdd_opl_add_us(&opl, 80);
    CHECK((status() & OPL_ST_T1) != 0, "timer1: flag after 2 ticks (256-preset)");

    /* it reloads and fires again, which is how music keeps its tempo            */
    wr(0x04, 0x80);
    vdd_opl_add_us(&opl, 160);
    CHECK((status() & OPL_ST_T1) != 0, "timer1: reloads and fires repeatedly");

    /* T6: timer 2 runs at 320us --------------------------------------------- */
    vdd_opl_reset(&opl);
    wr(0x03, 0xFF);
    wr(0x04, 0x02);                                  /* start T2                  */
    vdd_opl_add_us(&opl, 160);
    CHECK((status() & OPL_ST_T2) == 0, "timer2: nothing at 160us");
    vdd_opl_add_us(&opl, 160);
    CHECK((status() & OPL_ST_T2) != 0, "timer2: fires at 320us");
    CHECK((status() & OPL_ST_IRQ) != 0, "timer2: raises the IRQ bit too");

    /* T7: mask bits suppress the flag --------------------------------------- */
    vdd_opl_reset(&opl);
    wr(0x02, 0xFF);
    wr(0x04, 0x41);                                  /* start T1 but MASK it      */
    vdd_opl_add_us(&opl, 400);
    CHECK((status() & 0xE0) == 0x00, "masked timer1: overflow raises no flag");

    /* T8: a stopped timer does not advance ----------------------------------- */
    vdd_opl_reset(&opl);
    wr(0x02, 0xFF);
    vdd_opl_add_us(&opl, 4000);
    CHECK((status() & 0xE0) == 0x00, "stopped timer: no flag no matter how much time passes");

    /* T9: the data port is write-only on an OPL2 ---------------------------- */
    { uint32_t v = 0; VddBusIo(&bus, 0x389, 1, 1, &v);
      CHECK(v == 0xFF, "0x389 reads 0xFF (write-only data port)"); }

    /* T10: the bus frame tick advances the timers --------------------------- */
    vdd_opl_reset(&opl);
    opl.frame_us = 16667;
    wr(0x02, 0x00);                                  /* 256 ticks = 20.48ms       */
    wr(0x04, 0x01);
    VddBusFrame(&bus);
    CHECK((status() & OPL_ST_T1) == 0, "frame tick: 16.7ms is short of 20.48ms");
    VddBusFrame(&bus);
    CHECK((status() & OPL_ST_T1) != 0, "frame tick: two frames pass 20.48ms -> flag");

    /* ── OPL3 (YMF262), GH #232 ─────────────────────────────────────────────── */

    /* T11: OPL2 fitted -- 0x38A/0x38B are not there, exactly as on an AdLib -- */
    vdd_opl_reset(&opl);                             /* opl.opl3 == 0 (zeroed)    */
    CHECK(status() == 0x06 && (status() & 0x06) != 0, "OPL2: the OPL3 detect ((status & 6) == 0) FAILS");
    wr3(0xE0, 0x07);                                 /* array-1 waveform, op 18   */
    wr(0x20, 0x01);                                  /* put something in array 0  */
    wr3(0x20, 0x0F);
    CHECK(opl.reg[0x1E0] == 0 && opl.reg[0x120] == 0 && opl.op[18].mult == 0,
          "OPL2: writes through 0x38A/0x38B go nowhere");
    CHECK(opl.op[0].mult == 1, "OPL2: ...and do not leak into array 0");
    CHECK(rdp(0x38A) == 0xFF && rdp(0x38B) == 0xFF, "OPL2: 0x38A/0x38B read 0xFF (floating bus)");
    vdd_opl_write_reg(&opl, 0x1B0, 0x20);
    CHECK(opl.reg[0x1B0] == 0 && opl.ch[9].keyon == 0, "OPL2: a direct array-1 write is dropped too");

    /* T12: OPL3 fitted -- the ID bits and the detect ------------------------- */
    opl.opl3 = 1; vdd_opl_reset(&opl);
    CHECK(opl.opl3 == 1, "OPL3: chip type survives vdd_opl_reset");
    CHECK(status() == 0x00, "OPL3: status idles at 0x00 (ID bits 1-2 clear)");
    wr(0x04, 0x60); wr(0x04, 0x80);
    wr(0x02, 0xFF); wr(0x04, 0x21);
    vdd_opl_add_us(&opl, 80);
    CHECK(status() == 0xC0, "OPL3: the AdLib detect still reads 0xC0 -- an OPL3 IS an AdLib");
    CHECK(rdp(0x38A) == 0xC0, "OPL3: status readable at 0x38A too");
    CHECK(rdp(0x38B) == 0xFF && rdp(0x389) == 0xFF, "OPL3: the data ports stay write-only");
    wr(0x04, 0x80);

    /* T13: array 1 through 0x38A/0x38B lands on operators 18-35, channels 9-17 - */
    wr3(0x20, 0x0A);                                 /* array 1, op offset 0      */
    CHECK(opl.reg[0x120] == 0x0A && opl.op[18].mult == 0x0A, "array 1: 0x120 -> operator 18");
    CHECK(opl.op[0].mult != 0x0A, "array 1: ...not operator 0");
    wr3(0x2B, 0x05);                                 /* offset 0x0B = ch3 carrier */
    CHECK(opl.op[vdd_opl_op_index(12, 1)].mult == 5 && vdd_opl_op_index(12, 1) == 27,
          "array 1: 0x12B -> operator 27 (channel 12's carrier)");
    wr3(0xA2, 0x44); wr3(0xB2, 0x0D);
    CHECK(opl.ch[11].fnum == 0x144 && opl.ch[11].block == 3, "array 1: 0x1A2/0x1B2 -> channel 11");
    CHECK(vdd_opl_op_index(9, 0) == 18 && vdd_opl_op_index(17, 1) == 35, "ch9 -> op 18 ... ch17 -> op 35");
    /* ONE latch: address written at 0x38A, data through EITHER data port        */
    { uint32_t v = 0x40; VddBusIo(&bus, 0x38A, 1, 0, &v);
      v = 0x3F;          VddBusIo(&bus, 0x389, 1, 0, &v); }
    CHECK(opl.reg[0x140] == 0x3F && opl.op[18].tl == 0x3F, "one 9-bit latch: 0x38A then 0x389 writes array 1");
    /* array 1's 0x104 is the 4-op register, NOT timer control                   */
    wr(0x04, 0x00);                                  /* T1 stopped (T12 ran it)   */
    wr3(0x04, 0x01);
    CHECK(opl.t1_run == 0 && opl.reg[0x104] == 0x01, "0x104 is 4-op connect, not array 0's timer control");
    wr3(0x04, 0x00);

    /* T14: NEW gates the OPL3's extensions ---------------------------------- */
    CHECK(!vdd_opl_new_mode(&opl), "NEW: clear after reset (the OPL3 powers up OPL2-compatible)");
    wr3(0x04, 0x01);                                 /* pair 0+3 -- but NEW clear */
    CHECK(opl_4op_role(&opl, 0) == 0, "NEW clear: 0x104 latches but pairs nothing");
    wr3(0x05, 0x01);
    CHECK(vdd_opl_new_mode(&opl), "NEW: 0x105 bit 0 sets it");
    CHECK(opl_4op_role(&opl, 0) == 1 && opl_4op_role(&opl, 3) == 2 && opl_4op_role(&opl, 1) == 0,
          "NEW set: 0x104 bit 0 pairs channel 0 (lead) with 3");
    wr3(0x04, 0x38);
    CHECK(opl_4op_role(&opl, 9) == 1 && opl_4op_role(&opl, 12) == 2 &&
          opl_4op_role(&opl, 11) == 1 && opl_4op_role(&opl, 14) == 2 && opl_4op_role(&opl, 0) == 0,
          "0x104 bits 3-5 pair array 1's 9+12, 10+13, 11+14");
    wr3(0x04, 0x01);

    /* T15: a 4-op voice is keyed from its FIRST channel, all four operators -- */
    wr(0xB3, 0x20);                                  /* the second channel's key  */
    CHECK(opl.op[vdd_opl_op_index(3, 0)].eg_state == OPL_EG_OFF,
          "4-op: the second channel's key-on is ignored");
    wr(0xB0, 0x31);
    CHECK(opl.op[0].eg_state == OPL_EG_ATTACK && opl.op[3].eg_state == OPL_EG_ATTACK &&
          opl.op[6].eg_state == OPL_EG_ATTACK && opl.op[9].eg_state == OPL_EG_ATTACK,
          "4-op: key-on at 0xB0 starts operators 0, 3, 6 and 9");
    wr(0xB0, 0x11);
    CHECK(opl.op[6].eg_state == OPL_EG_RELEASE && opl.op[9].eg_state == OPL_EG_RELEASE,
          "4-op: key-off releases all four");

    /* T16: all-notes-off reaches array 1 ------------------------------------ */
    wr3(0xB4, 0x2A);                                 /* channel 13, an ordinary one */
    CHECK(opl.ch[13].keyon == 1 && opl.op[vdd_opl_op_index(13, 1)].eg_state == OPL_EG_ATTACK,
          "array 1: 0x1B4 keys channel 13");
    vdd_opl_all_notes_off(&opl);
    CHECK((opl.reg[0x1B4] & 0x20) == 0 && opl.ch[13].fnum == 0x200 &&
          opl.op[vdd_opl_op_index(13, 1)].eg_state == OPL_EG_RELEASE,
          "all-notes-off: array-1 voices released too, F-number kept");

    /* T17: reset clears NEW but keeps the chip ------------------------------- */
    vdd_opl_reset(&opl);
    CHECK(opl.opl3 == 1 && !vdd_opl_new_mode(&opl) && opl.reg[0x104] == 0, "reset: NEW and 0x104 clear, chip still OPL3");
    opl.opl3 = 0; vdd_opl_reset(&opl);

    printf("-- %d checks, %d failures --\n", total, fails);
    return fails ? 1 : 0;
}
