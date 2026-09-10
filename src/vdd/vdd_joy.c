/* vdd_joy.c -- see vdd_joy.h.  Gameport ports 0x200-0x207 on the VDD bus; the
 * 558 one-shot model, timed by the host-injected microsecond clock.  Pure C,
 * no <windows.h>. */
#include "vdd_joy.h"

/* IN: buttons in bits 4-7 (0 = pressed), one-shot state in bits 0-3.
   An axis the adapter does not wire (JoystickType) or with nothing plugged in
   reads STUCK HIGH once triggered -- an open resistive input never reaches the
   558's threshold, so the pulse never ends. That is what a game's detection
   loop times out on to decide "no joystick", and it is also what an UNCLAIMED
   port looked like (0xFF), so a machine with the type set to None is
   indistinguishable from one without the card. */
static void joy_in(void *self, uint16_t port, uint8_t w, uint32_t *val)
{
    joy_state *st = (joy_state *)self; (void)port; (void)w;
    uint8_t v = 0xF0;                            /* no buttons pressed          */
    int i, wired = joy_axes(st);
    st->reads++;
    if (st->type == JOY_TYPE_NONE) { *val = 0xFF; return; }
    if (joy_live(st)) {
        uint8_t mask = (uint8_t)((1u << joy_buttons_wired(st)) - 1u);
        v = (uint8_t)((uint8_t)(~(st->buttons & mask)) << 4);
    }
    if (st->fired) {
        uint64_t el = st->now_us
                    ? st->now_us(st->clock_ctx) - st->trigger_us
                    : 0;                         /* no clock: pulses never end  */
        for (i = 0; i < 4; ++i) {
            int stuck = (i >= wired) || !st->present || !st->now_us;
            if (stuck || el < joy_axis_us(st->axis[i])) v |= (uint8_t)(1u << i);
        }
    }
    *val = v;
}

/* OUT (any value, any port in the range): fire the one-shots. */
static void joy_out(void *self, uint16_t port, uint8_t w, uint32_t val)
{
    joy_state *st = (joy_state *)self; (void)port; (void)w; (void)val;
    st->outs++;
    if (st->type == JOY_TYPE_NONE) return;       /* no card, nothing to fire   */
    st->fired = 1;
    st->trigger_us = st->now_us ? st->now_us(st->clock_ctx) : 0;
}

void vdd_joy_reset(void *self)
{
    joy_state *st = (joy_state *)self;
    st->fired = 0; st->trigger_us = 0; /* keep type + the host-fed sample      */
}

int vdd_joy_init(vdd_bus *b, void *self)
{
    joy_state *st = (joy_state *)self;
    st->bus = b;
    /* A real gameport card decodes the whole 0x200-0x207 block. */
    return vdd_claim_ports(b, 0x200, 0x207, joy_in, joy_out, st);
}
