/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The gameport (joystick) VDD.  (session 62)
 *
 * Port 0x201 was UNCLAIMED until now: reads returned 0xFF like an empty ISA
 * slot, and Mario Bros died right after a CLI poll of it (suspect at the time,
 * unproven for want of a crash dump). This VDD claims the gameport range and
 * models the one piece of real hardware behind it: the 558 quad one-shot.
 *
 *   OUT 0x201, anything   -> fires the four one-shots
 *   IN  0x201             -> bits 0-3: 1 while each axis' one-shot is still
 *                            timing (duration proportional to stick position),
 *                            bits 4-7: buttons, 0 = PRESSED (active low)
 *
 * A game measures an axis by writing the port and counting loops until the
 * axis bit drops -- absolute scale does not matter because every game
 * calibrates, but the RELATIVE duration must track the stick. Time comes from
 * a host-injected microsecond clock (QPC on the real host, a settable fake in
 * joy_test.c), NOT from counting port reads: the port-trap cost (~2.33 us per
 * IN) would otherwise be the timebase.
 *
 * The stick itself is host-fed: a winmm poll thread in main.c writes
 * present/axes/buttons; this file makes no Windows calls (only Windows types) so
 * the off-VM battery can drive it like every other VDD.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDD_JOY_H
#define NTVDMEX_VDD_JOY_H

#include "vdd_bus.h"

/* Host-injected microsecond clock. NULL is legal (off-VM default): the
 * one-shots then never time out, which reads as "no joystick" to a guest.
 */
typedef UINT64 (*PJOYSTICK_CLOCK_ROUTINE)(PVOID context);

/* Mirrors the JoystickType setting: what the emulated ADAPTER is fitted with.
 * The HOST stick (IsPresent) is a separate fact -- an adapter with nothing
 * plugged in is exactly how an absent axis reads on real hardware.
 * (JOYSTICK_, not JOY_: mmsystem.h owns the JOY_ names.)
 */
enum
{
    JOYSTICK_TYPE_NONE = 0, JOYSTICK_TYPE_2AXIS = 1, JOYSTICK_TYPE_4AXIS = 2
};

#define JOYSTICK_AXES               4       /* A(x,y) then B(x,y) */
#define JOYSTICK_2AXIS_WIRED        2       /* Axes and buttons a 2-axis adapter wires */
#define JOYSTICK_4AXIS_WIRED        4
#define JOYSTICK_NONE_WIRED         0
#define JOYSTICK_DEVICE_NAME        "joystick"
#define JOYSTICK_AXIS_MIN           0
#define JOYSTICK_AXIS_MAX           255
#define JOYSTICK_BUTTON_MASK        0x0F    /* Four buttons */

/* A POV hat, in hundredths of a degree: below 36000 it points, in eight octants from north. */
#define JOYSTICK_POV_FULL_CIRCLE    36000
#define JOYSTICK_POV_OCTANT_U       4500u
#define JOYSTICK_POV_OCTANT_MASK    7u
#define JOYSTICK_POV_NORTH          0
#define JOYSTICK_POV_NORTH_EAST     1
#define JOYSTICK_POV_SOUTH_EAST     3
#define JOYSTICK_POV_SOUTH          4
#define JOYSTICK_POV_SOUTH_WEST     5
#define JOYSTICK_POV_NORTH_WEST     7

typedef struct _JOYSTICK_STATE
{
    PVDD_BUS     Bus;
    PJOYSTICK_CLOCK_ROUTINE NowMicroseconds;   PVOID ClockContext;

    BYTE         Type;         /* JOYSTICK_TYPE_*: how many axes/buttons are wired */

    /* the host-fed sample (poll thread on the real host; a test off-VM) */
    volatile BYTE IsPresent;   /* 0 = nothing plugged into the adapter */
    volatile BYTE Buttons;     /* bits 0-3 = buttons 1-4, 1 = PRESSED */
    volatile BYTE Axis[JOYSTICK_AXES];  /* 0..255, 0x80 centred; A(x,y) then B(x,y) */

    BYTE         HasFired;     /* the one-shots have been triggered at all --
                                  NOT TriggerMicroseconds==0, because an injected
                                  clock may legitimately read 0 at fire time  */
    UINT64       TriggerMicroseconds;   /* when the one-shots last fired */
    UINT32       PortReads, PortWrites; /* CLOSE diagnostics: did the guest ever poll? */
} JOYSTICK_STATE, *PJOYSTICK_STATE;

typedef const JOYSTICK_STATE *PCJOYSTICK_STATE;

/* One-shot duration for an axis position, in microseconds: 25..1045, centre
 * ~537 -- inside the ~1.12 ms a real gameport spans, and wide enough that a
 * guest polling through the port trap still gets ~200 samples across it.
 */
#define JOYSTICK_PULSE_BASE_US      25u
#define JOYSTICK_PULSE_US_PER_STEP  4u
static inline UINT32 VddJoystickAxisMicroseconds(BYTE position)
{
    return JOYSTICK_PULSE_BASE_US + (UINT32)position * JOYSTICK_PULSE_US_PER_STEP;
}

/* How many axes/buttons the configured adapter wires up. */
static inline INT VddJoystickAxes(_In_ PCJOYSTICK_STATE state)
{
    return state->Type == JOYSTICK_TYPE_4AXIS ? JOYSTICK_4AXIS_WIRED : (state->Type == JOYSTICK_TYPE_2AXIS ? JOYSTICK_2AXIS_WIRED : JOYSTICK_NONE_WIRED);
}

static inline INT VddJoystickButtonsWired(_In_ PCJOYSTICK_STATE state)
{
    return state->Type == JOYSTICK_TYPE_4AXIS ? JOYSTICK_4AXIS_WIRED : (state->Type == JOYSTICK_TYPE_2AXIS ? JOYSTICK_2AXIS_WIRED : JOYSTICK_NONE_WIRED);
}

/* Is the stick usable right now (adapter fitted AND something plugged in)? */
static inline INT VddJoystickIsLive(_In_ PCJOYSTICK_STATE state)
{
    return state->Type != JOYSTICK_TYPE_NONE && state->IsPresent;
}

INT  VddJoystickInitialize(_In_ PVDD_BUS bus, _In_ PVOID context);
VOID VddJoystickReset(_In_ PVOID context);
static inline NTVDD_DEVICE VddJoystickDevice(_In_ PJOYSTICK_STATE state)
{ NTVDD_DEVICE device; device.Name = JOYSTICK_DEVICE_NAME; device.Initialize = VddJoystickInitialize; device.Reset = VddJoystickReset;
  device.Shutdown = 0; device.Context = state; return device; }

#endif /* NTVDMEX_VDD_JOY_H */
