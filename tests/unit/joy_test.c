/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM unit battery for the gameport VDD (vdd_joy.c).
 *
 * The device is a 558 quad one-shot: OUT fires it, IN reports which axes are
 * still timing plus the buttons (active low). The clock is injected, so every
 * duration below is exact -- the test IS the datasheet the port model claims
 * to implement. What cannot be tested here is the winmm poll thread in
 * main.c; this pins the part the guest actually talks to.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "vdd_joy.h"

static INT g_Total = 0, g_Failures = 0;
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static BYTE g_GuestMemory[0x100000];

static UINT64 g_NowMicroseconds;
static UINT64 JoyTestFakeClock(PVOID context)
{
    (VOID)context;
    return g_NowMicroseconds;
}

static UINT32 JoyTestReadGameport(PVDD_BUS bus)
{
    UINT32 value = 0xEE;

    VddBusIo(bus, 0x201, 1, 1, &value);
    return value;
}

static VOID JoyTestTriggerGameport(PVDD_BUS bus)
{
    VddBusIo(bus, 0x201, 1, 0, &(UINT32){0});
}

INT main(VOID)
{
    VDD_BUS bus;
    JOYSTICK_STATE joystick;

    memset(&joystick, 0, sizeof joystick);
    NTVDD_DEVICE joystickDevice = VddJoystickDevice(&joystick);

    printf("== gameport VDD battery ==\n");

    VddBusInitialize(&bus, g_GuestMemory);
    joystick.NowMicroseconds = JoyTestFakeClock;
    CHECK(VddBusAdd(&bus, &joystickDevice) == 0, "add: joystick ok");
    CHECK(bus.Ports[bus.PortCount - 1].First == 0x200
          && bus.Ports[bus.PortCount - 1].Last == 0x207,
          "add: claimed the 0x200-0x207 block");

    /* T1: type None reads exactly like the unclaimed port used to ---------- */
    CHECK(JoyTestReadGameport(&bus) == 0xFF, "type None: reads 0xFF (empty ISA slot)");
    JoyTestTriggerGameport(&bus);
    CHECK(JoyTestReadGameport(&bus) == 0xFF, "type None: still 0xFF after a trigger");

    /* T2: adapter fitted, nothing plugged in ------------------------------- */
    joystick.Type = JOYSTICK_TYPE_2AXIS;
    joystick.IsPresent = 0;
    CHECK(JoyTestReadGameport(&bus) == 0xF0, "unplugged: buttons up, one-shots idle");
    JoyTestTriggerGameport(&bus);
    g_NowMicroseconds += 2000;                       /* 2 ms: any real axis long done */
    CHECK((JoyTestReadGameport(&bus) & 0x0F) == 0x0F, "unplugged: axes STUCK high (timeout)");

    /* T3: plugged in, centred, 2-axis/2-button ----------------------------- */
    joystick.IsPresent = 1;
    joystick.Axis[0] = 0x80;
    joystick.Axis[1] = 0x80;
    joystick.Axis[2] = 0x80;
    joystick.Axis[3] = 0x80;
    joystick.TriggerMicroseconds = 0;                  /* fresh */
    g_NowMicroseconds = 100000;
    JoyTestTriggerGameport(&bus);
    CHECK((JoyTestReadGameport(&bus) & 0x0F) == 0x0F, "t=0: all pulses high");
    CHECK((JoyTestReadGameport(&bus) & 0xF0) == 0xF0, "no buttons pressed: bits 4-7 high");
    g_NowMicroseconds += 400;                        /* centre = 25+128*4 = 537 us */
    CHECK((JoyTestReadGameport(&bus) & 0x03) == 0x03, "t=400us < 537: A axes still timing");
    g_NowMicroseconds += 200;                        /* t=600us */
    CHECK((JoyTestReadGameport(&bus) & 0x03) == 0x00, "t=600us > 537: A axes done");
    CHECK((JoyTestReadGameport(&bus) & 0x0C) == 0x0C, "2-axis type: B axes stuck (absent)");

    /* T4: the duration tracks the position --------------------------------- */
    joystick.Axis[0] = 0x00;
    joystick.Axis[1] = 0xFF;
    g_NowMicroseconds = 200000;
    JoyTestTriggerGameport(&bus);
    g_NowMicroseconds += 30;                         /* x: 25us, y: 1045us */
    CHECK((JoyTestReadGameport(&bus) & 0x01) == 0x00, "x=0: 25us pulse already over at 30us");
    CHECK((JoyTestReadGameport(&bus) & 0x02) == 0x02, "y=255: 1045us pulse still high");
    g_NowMicroseconds += 1100;
    CHECK((JoyTestReadGameport(&bus) & 0x02) == 0x00, "y=255: over by 1130us");

    /* T5: buttons are active low, masked to the wired count ----------------- */
    joystick.Buttons = 0x05;                  /* 1 + 3 pressed */
    CHECK((JoyTestReadGameport(&bus) & 0xF0) == 0xE0, "2-button type: button 1 low, 3 masked");
    joystick.Type = JOYSTICK_TYPE_4AXIS;
    CHECK((JoyTestReadGameport(&bus) & 0xF0) == 0xA0, "4-button type: buttons 1+3 low");

    /* T6: 4-axis type times the B pair too ---------------------------------- */
    joystick.Buttons = 0;
    joystick.Axis[2] = 0x00;
    joystick.Axis[3] = 0x00;
    g_NowMicroseconds = 300000;
    JoyTestTriggerGameport(&bus);
    g_NowMicroseconds += 100;                        /* B axes: 25us, long over */
    CHECK((JoyTestReadGameport(&bus) & 0x0C) == 0x00, "4-axis: B axes measured, not stuck");

    /* T7: reset drops the pulse but keeps the configuration ----------------- */
    VddJoystickReset(&joystick);
    CHECK(!joystick.HasFired && joystick.Type == JOYSTICK_TYPE_4AXIS,
          "reset: one-shots idle, type kept");
    CHECK((JoyTestReadGameport(&bus) & 0x0F) == 0x00, "reset: axis bits low until re-fired");

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
