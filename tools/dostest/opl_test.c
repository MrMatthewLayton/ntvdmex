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
 */
#include <stdio.h>
#include <string.h>
#include "vdd_opl.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

static uint8_t g_flat[0x100000];
static vdd_bus bus;

/* Write an OPL register the way hardware is driven: address to 0x388, data to
   0x389 -- so the battery exercises the port path, not just the helper. */
static void wr(uint8_t reg, uint8_t val)
{
    uint32_t v = reg; vdd_bus_io(&bus, 0x388, 1, 0, &v);
    v = val;          vdd_bus_io(&bus, 0x389, 1, 0, &v);
}
static uint8_t status(void)
{
    uint32_t v = 0; vdd_bus_io(&bus, 0x388, 1, 1, &v); return (uint8_t)v;
}

int main(void)
{
    opl_state opl; memset(&opl, 0, sizeof opl);
    ntvdd dev = vdd_opl_device(&opl);
    int m, c;

    printf("== sound epic: AdLib/OPL2 register + timer battery ==\n");

    vdd_bus_init(&bus, g_flat);
    vdd_bus_set_sinks(&bus, 0, 0, 0, 0);
    CHECK(vdd_bus_add(&bus, &dev) == 0, "add: opl device ok");
    CHECK(status() == 0, "reset: status register clear");

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
    CHECK(status() == 0x00, "detect: status reads 0x00 after reset");

    wr(0x02, 0xFF);                                  /* T1 preset: one 80us tick  */
    wr(0x04, 0x21);                                  /* start T1, T1 unmasked     */
    CHECK(status() == 0x00, "detect: status still 0x00 before the delay");
    vdd_opl_add_us(&opl, 80);                        /* the game's ~80us delay    */
    CHECK(status() == 0xC0, "detect: status reads 0xC0 (IRQ|T1) after 80us  <-- THE TEST");

    wr(0x04, 0x60);
    wr(0x04, 0x80);
    CHECK(status() == 0x00, "detect: flags reset again -> card confirmed present");

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
    CHECK(status() == 0x00, "masked timer1: overflow raises no flag");

    /* T8: a stopped timer does not advance ----------------------------------- */
    vdd_opl_reset(&opl);
    wr(0x02, 0xFF);
    vdd_opl_add_us(&opl, 4000);
    CHECK(status() == 0x00, "stopped timer: no flag no matter how much time passes");

    /* T9: the data port is write-only on an OPL2 ---------------------------- */
    { uint32_t v = 0; vdd_bus_io(&bus, 0x389, 1, 1, &v);
      CHECK(v == 0xFF, "0x389 reads 0xFF (write-only data port)"); }

    /* T10: the bus frame tick advances the timers --------------------------- */
    vdd_opl_reset(&opl);
    opl.frame_us = 16667;
    wr(0x02, 0x00);                                  /* 256 ticks = 20.48ms       */
    wr(0x04, 0x01);
    vdd_bus_frame(&bus);
    CHECK((status() & OPL_ST_T1) == 0, "frame tick: 16.7ms is short of 20.48ms");
    vdd_bus_frame(&bus);
    CHECK((status() & OPL_ST_T1) != 0, "frame tick: two frames pass 20.48ms -> flag");

    printf("-- %d checks, %d failures --\n", total, fails);
    return fails ? 1 : 0;
}
