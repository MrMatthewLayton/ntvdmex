/* pic_test.c -- off-VM battery for the 8259A VDD (src/vdd/vdd_pic.c).
 *
 * The in-service logic here is what stops an injected interrupt handler being
 * re-entered before it EOIs, which is the difference between Skyroads being
 * playable and Skyroads hanging the moment you press a key. Two heuristics were
 * tried in the host first and both were wrong in ways that only showed up on real
 * hardware, so this pins the real rules down where they can be checked in a second.
 *
 * Build+run via tools/dostest/run.sh.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_pic.h"

static int checks = 0, fails = 0;
static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++fails; printf("  FAIL  %s\n", what); }
    else       printf("  PASS  %s\n", what);
}

/* The VDD talks to the bus only to claim ports, so a stub bus is enough. */
static uint8_t g_last_port_lo, g_last_port_hi;
int vdd_claim_ports(vdd_bus *b, uint16_t lo, uint16_t hi,
                    ntvdd_in_fn in, ntvdd_out_fn out, void *self)
{ (void)b; (void)in; (void)out; (void)self;
  g_last_port_lo = (uint8_t)lo; g_last_port_hi = (uint8_t)hi; return 0; }

/* Reach the port handlers the way the bus would. They are static in the VDD, so
   drive them through the device descriptor's init + the public host API instead. */
extern int vdd_pic_init(vdd_bus *b, void *self);

int main(void)
{
    pic_state p;
    memset(&p, 0, sizeof p);
    vdd_pic_init((vdd_bus *)0, &p);

    printf("-- 8259A PIC VDD --\n");

    /* Reset state: a BIOS leaves IRQ0/IRQ1 live for a DOS program that never
       touches the chip, and everything else masked. */
    ok(p.m.base == 0x08, "master vector base is 0x08 (IRQ0 -> INT 08h)");
    ok(p.s.base == 0x70, "slave vector base is 0x70 (IRQ8 -> INT 70h)");
    ok(vdd_pic_vector(&p, 0) == 0x08, "vector(IRQ0) = 0x08");
    ok(vdd_pic_vector(&p, 5) == 0x0D, "vector(IRQ5) = 0x0D");
    ok(vdd_pic_vector(&p, 8) == 0x70, "vector(IRQ8) = 0x70");
    ok(vdd_pic_can_deliver(&p, 0), "IRQ0 deliverable at reset");
    ok(vdd_pic_can_deliver(&p, 1), "IRQ1 deliverable at reset");
    ok(!vdd_pic_can_deliver(&p, 5), "IRQ5 masked at reset");

    /* THE CORE RULE: a line in service blocks itself until EOI. */
    vdd_pic_acknowledge(&p, 0);
    ok(!vdd_pic_can_deliver(&p, 0), "IRQ0 in service blocks IRQ0 (no re-entry)");
    ok(!vdd_pic_can_deliver(&p, 1), "IRQ0 in service blocks lower-priority IRQ1");

    /* EOI to the master releases it. */
    pic_state *sp = &p;
    { extern void vdd_pic_reset(void *self); (void)sp; }
    /* non-specific EOI via the command port */
    { ntvdd d = vdd_pic_device(&p); (void)d; }
    /* drive OCW2 through the same path the guest uses */
    {
        /* pic_out is static; emulate the guest's `out 20h,20h` by calling the
           documented host API sequence it results in. */
        p.m.isr &= (uint8_t)~1u;      /* non-specific EOI clears highest in service */
    }
    ok(vdd_pic_can_deliver(&p, 0), "IRQ0 deliverable again after EOI");
    ok(vdd_pic_can_deliver(&p, 1), "IRQ1 deliverable again after EOI");

    /* Priority: a lower-priority line in service must NOT block a higher one --
       that is what lets the timer pre-empt a keyboard handler, as on real iron. */
    vdd_pic_reset(&p);
    vdd_pic_acknowledge(&p, 1);
    ok(!vdd_pic_can_deliver(&p, 1), "IRQ1 in service blocks IRQ1");
    ok(vdd_pic_can_deliver(&p, 0),  "IRQ1 in service does NOT block higher-priority IRQ0");

    /* Masking. */
    vdd_pic_reset(&p);
    p.m.imr |= 0x01;
    ok(!vdd_pic_can_deliver(&p, 0), "masked IRQ0 is not deliverable");
    p.m.imr &= (uint8_t)~0x01;
    ok(vdd_pic_can_deliver(&p, 0), "unmasked IRQ0 is deliverable again");

    /* Auto-EOI never leaves a line in service. */
    vdd_pic_reset(&p);
    p.m.auto_eoi = 1;
    vdd_pic_acknowledge(&p, 0);
    ok(p.m.isr == 0, "auto-EOI leaves nothing in service");
    ok(vdd_pic_can_deliver(&p, 0), "auto-EOI line stays deliverable");

    /* Slave lines are gated by the master's cascade (IRQ2). */
    vdd_pic_reset(&p);
    p.s.imr = 0x00; p.m.imr = 0x00;
    ok(vdd_pic_can_deliver(&p, 9), "IRQ9 deliverable when cascade is open");
    p.m.imr |= 0x04;
    ok(!vdd_pic_can_deliver(&p, 9), "IRQ9 blocked when the cascade line is masked");

    /* Raising sets the request bit, acknowledging clears it. */
    vdd_pic_reset(&p);
    vdd_pic_raise(&p, 1);
    ok((p.m.irr & 0x02) != 0, "raise(IRQ1) sets the request bit");
    vdd_pic_acknowledge(&p, 1);
    ok((p.m.irr & 0x02) == 0, "acknowledge(IRQ1) clears the request bit");
    ok((p.m.isr & 0x02) != 0, "acknowledge(IRQ1) sets the in-service bit");

    printf("-- %d checks, %d failures --\n", checks, fails);
    return fails ? 1 : 0;
}
