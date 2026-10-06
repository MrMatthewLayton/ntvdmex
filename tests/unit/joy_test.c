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

static uint32_t rd201(VDD_BUS *b)
{ uint32_t v = 0xEE; VddBusIo(b, 0x201, 1, 1, &v); return v; }
static void wr201(VDD_BUS *b)
{ VddBusIo(b, 0x201, 1, 0, &(uint32_t){0}); }

int main(void)
{
    VDD_BUS bus;
    JOYSTICK_STATE joy; memset(&joy, 0, sizeof joy);
    NTVDD_DEVICE jdev = VddJoystickDevice(&joy);

    printf("== gameport VDD battery ==\n");

    VddBusInitialize(&bus, g_flat);
    joy.NowMicroseconds = fake_clock;
    CHECK(VddBusAdd(&bus, &jdev) == 0, "add: joystick ok");
    CHECK(bus.Ports[bus.PortCount - 1].First == 0x200
          && bus.Ports[bus.PortCount - 1].Last == 0x207,
          "add: claimed the 0x200-0x207 block");

    /* T1: type None reads exactly like the unclaimed port used to ---------- */
    CHECK(rd201(&bus) == 0xFF, "type None: reads 0xFF (empty ISA slot)");
    wr201(&bus);
    CHECK(rd201(&bus) == 0xFF, "type None: still 0xFF after a trigger");

    /* T2: adapter fitted, nothing plugged in ------------------------------- */
    joy.Type = JOYSTICK_TYPE_2AXIS; joy.IsPresent = 0;
    CHECK(rd201(&bus) == 0xF0, "unplugged: buttons up, one-shots idle");
    wr201(&bus);
    g_now += 2000;                       /* 2 ms: any real axis long done      */
    CHECK((rd201(&bus) & 0x0F) == 0x0F, "unplugged: axes STUCK high (timeout)");

    /* T3: plugged in, centred, 2-axis/2-button ----------------------------- */
    joy.IsPresent = 1;
    joy.Axis[0] = 0x80; joy.Axis[1] = 0x80; joy.Axis[2] = 0x80; joy.Axis[3] = 0x80;
    joy.TriggerMicroseconds = 0;                  /* fresh */
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
    joy.Axis[0] = 0x00; joy.Axis[1] = 0xFF;
    g_now = 200000; wr201(&bus);
    g_now += 30;                         /* x: 25us, y: 1045us                 */
    CHECK((rd201(&bus) & 0x01) == 0x00, "x=0: 25us pulse already over at 30us");
    CHECK((rd201(&bus) & 0x02) == 0x02, "y=255: 1045us pulse still high");
    g_now += 1100;
    CHECK((rd201(&bus) & 0x02) == 0x00, "y=255: over by 1130us");

    /* T5: buttons are active low, masked to the wired count ----------------- */
    joy.Buttons = 0x05;                  /* 1 + 3 pressed                      */
    CHECK((rd201(&bus) & 0xF0) == 0xE0, "2-button type: button 1 low, 3 masked");
    joy.Type = JOYSTICK_TYPE_4AXIS;
    CHECK((rd201(&bus) & 0xF0) == 0xA0, "4-button type: buttons 1+3 low");

    /* T6: 4-axis type times the B pair too ---------------------------------- */
    joy.Buttons = 0; joy.Axis[2] = 0x00; joy.Axis[3] = 0x00;
    g_now = 300000; wr201(&bus);
    g_now += 100;                        /* B axes: 25us, long over            */
    CHECK((rd201(&bus) & 0x0C) == 0x00, "4-axis: B axes measured, not stuck");

    /* T7: reset drops the pulse but keeps the configuration ----------------- */
    VddJoystickReset(&joy);
    CHECK(!joy.HasFired && joy.Type == JOYSTICK_TYPE_4AXIS,
          "reset: one-shots idle, type kept");
    CHECK((rd201(&bus) & 0x0F) == 0x00, "reset: axis bits low until re-fired");

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
