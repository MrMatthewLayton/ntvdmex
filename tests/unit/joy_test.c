/* joy_test.c -- off-VM unit battery for the gameport VDD (vdd_joy.c).
 *
 * The device is a 558 quad one-shot: OUT fires it, IN reports which axes are
 * still timing plus the buttons (active low). The clock is injected, so every
 * duration below is exact -- the test IS the datasheet the port model claims
 * to implement. What cannot be tested here is the winmm poll thread in
 * main.c; this pins the part the guest actually talks to.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_joy.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

static uint8_t g_flat[0x100000];

static uint64_t g_now;
static uint64_t fake_clock(void *ctx) { (void)ctx; return g_now; }

static uint32_t rd201(vdd_bus *b)
{ uint32_t v = 0xEE; vdd_bus_io(b, 0x201, 1, 1, &v); return v; }
static void wr201(vdd_bus *b)
{ vdd_bus_io(b, 0x201, 1, 0, &(uint32_t){0}); }

int main(void)
{
    vdd_bus bus;
    joy_state joy; memset(&joy, 0, sizeof joy);
    ntvdd jdev = vdd_joy_device(&joy);

    printf("== gameport VDD battery ==\n");

    vdd_bus_init(&bus, g_flat);
    joy.now_us = fake_clock;
    CHECK(vdd_bus_add(&bus, &jdev) == 0, "add: joystick ok");
    CHECK(bus.ports[bus.n_ports - 1].lo == 0x200
          && bus.ports[bus.n_ports - 1].hi == 0x207,
          "add: claimed the 0x200-0x207 block");

    /* T1: type None reads exactly like the unclaimed port used to ---------- */
    CHECK(rd201(&bus) == 0xFF, "type None: reads 0xFF (empty ISA slot)");
    wr201(&bus);
    CHECK(rd201(&bus) == 0xFF, "type None: still 0xFF after a trigger");

    /* T2: adapter fitted, nothing plugged in ------------------------------- */
    joy.type = JOY_TYPE_2AXIS; joy.present = 0;
    CHECK(rd201(&bus) == 0xF0, "unplugged: buttons up, one-shots idle");
    wr201(&bus);
    g_now += 2000;                       /* 2 ms: any real axis long done      */
    CHECK((rd201(&bus) & 0x0F) == 0x0F, "unplugged: axes STUCK high (timeout)");

    /* T3: plugged in, centred, 2-axis/2-button ----------------------------- */
    joy.present = 1;
    joy.axis[0] = 0x80; joy.axis[1] = 0x80; joy.axis[2] = 0x80; joy.axis[3] = 0x80;
    joy.trigger_us = 0;                  /* fresh */
    g_now = 100000;
    wr201(&bus);
    CHECK((rd201(&bus) & 0x0F) == 0x0F, "t=0: all pulses high");
    CHECK((rd201(&bus) & 0xF0) == 0xF0, "no buttons pressed: bits 4-7 high");
    g_now += 400;                        /* centre = 25+128*4 = 537 us         */
    CHECK((rd201(&bus) & 0x03) == 0x03, "t=400us < 537: A axes still timing");
    g_now += 200;                        /* t=600us */
    CHECK((rd201(&bus) & 0x03) == 0x00, "t=600us > 537: A axes done");
    CHECK((rd201(&bus) & 0x0C) == 0x0C, "2-axis type: B axes stuck (absent)");

    /* T4: the duration tracks the position --------------------------------- */
    joy.axis[0] = 0x00; joy.axis[1] = 0xFF;
    g_now = 200000; wr201(&bus);
    g_now += 30;                         /* x: 25us, y: 1045us                 */
    CHECK((rd201(&bus) & 0x01) == 0x00, "x=0: 25us pulse already over at 30us");
    CHECK((rd201(&bus) & 0x02) == 0x02, "y=255: 1045us pulse still high");
    g_now += 1100;
    CHECK((rd201(&bus) & 0x02) == 0x00, "y=255: over by 1130us");

    /* T5: buttons are active low, masked to the wired count ----------------- */
    joy.buttons = 0x05;                  /* 1 + 3 pressed                      */
    CHECK((rd201(&bus) & 0xF0) == 0xE0, "2-button type: button 1 low, 3 masked");
    joy.type = JOY_TYPE_4AXIS;
    CHECK((rd201(&bus) & 0xF0) == 0xA0, "4-button type: buttons 1+3 low");

    /* T6: 4-axis type times the B pair too ---------------------------------- */
    joy.buttons = 0; joy.axis[2] = 0x00; joy.axis[3] = 0x00;
    g_now = 300000; wr201(&bus);
    g_now += 100;                        /* B axes: 25us, long over            */
    CHECK((rd201(&bus) & 0x0C) == 0x00, "4-axis: B axes measured, not stuck");

    /* T7: reset drops the pulse but keeps the configuration ----------------- */
    vdd_joy_reset(&joy);
    CHECK(!joy.fired && joy.type == JOY_TYPE_4AXIS,
          "reset: one-shots idle, type kept");
    CHECK((rd201(&bus) & 0x0F) == 0x00, "reset: axis bits low until re-fired");

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
