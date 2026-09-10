/*
 * vdd_joy.h -- the gameport (joystick) VDD.  (session 62)
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
 * present/axes/buttons; this file never touches <windows.h> so the off-VM
 * battery can drive it like every other VDD.
 */
#ifndef NTVDMEX_VDD_JOY_H
#define NTVDMEX_VDD_JOY_H

#include "vdd_bus.h"

/* Host-injected microsecond clock. NULL is legal (off-VM default): the
   one-shots then never time out, which reads as "no joystick" to a guest. */
typedef uint64_t (*joy_clock_fn)(void *ctx);

/* Mirrors the JoystickType setting: what the emulated ADAPTER is fitted with.
   The HOST stick (present) is a separate fact -- an adapter with nothing
   plugged in is exactly how an absent axis reads on real hardware. */
enum { JOY_TYPE_NONE = 0, JOY_TYPE_2AXIS = 1, JOY_TYPE_4AXIS = 2 };

typedef struct joy_state {
    vdd_bus     *bus;
    joy_clock_fn now_us;   void *clock_ctx;

    uint8_t      type;         /* JOY_TYPE_*: how many axes/buttons are wired  */

    /* the host-fed sample (poll thread on the real host; a test off-VM) */
    volatile uint8_t present;  /* 0 = nothing plugged into the adapter         */
    volatile uint8_t buttons;  /* bits 0-3 = buttons 1-4, 1 = PRESSED          */
    volatile uint8_t axis[4];  /* 0..255, 0x80 centred; A(x,y) then B(x,y)     */

    uint8_t      fired;        /* the one-shots have been triggered at all --
                                  NOT trigger_us==0, because an injected clock
                                  may legitimately read 0 at fire time         */
    uint64_t     trigger_us;   /* when the one-shots last fired                */
    uint32_t     reads, outs;  /* CLOSE diagnostics: did the guest ever poll?  */
} joy_state;

/* One-shot duration for an axis position, in microseconds: 25..1045, centre
   ~537 -- inside the ~1.12 ms a real gameport spans, and wide enough that a
   guest polling through the port trap still gets ~200 samples across it. */
static inline uint32_t joy_axis_us(uint8_t v) { return 25u + (uint32_t)v * 4u; }

/* How many axes/buttons the configured adapter wires up. */
static inline int joy_axes(const joy_state *st)
{ return st->type == JOY_TYPE_4AXIS ? 4 : (st->type == JOY_TYPE_2AXIS ? 2 : 0); }
static inline int joy_buttons_wired(const joy_state *st)
{ return st->type == JOY_TYPE_4AXIS ? 4 : (st->type == JOY_TYPE_2AXIS ? 2 : 0); }

/* Is the stick usable right now (adapter fitted AND something plugged in)? */
static inline int joy_live(const joy_state *st)
{ return st->type != JOY_TYPE_NONE && st->present; }

int  vdd_joy_init(vdd_bus *b, void *self);
void vdd_joy_reset(void *self);
static inline ntvdd vdd_joy_device(joy_state *st)
{ ntvdd d; d.name = "joystick"; d.init = vdd_joy_init; d.reset = vdd_joy_reset;
  d.shutdown = 0; d.self = st; return d; }

#endif /* NTVDMEX_VDD_JOY_H */
