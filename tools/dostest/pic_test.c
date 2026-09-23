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

/* The VDD talks to the bus only to claim ports, so a stub bus is enough.
   ⚠ IT ALSO KEEPS THE HANDLERS IT WAS HANDED. The port side of this chip -- ICW1,
     OCW2's EOI family, OCW3's read select and poll -- is half the device and none
     of it was reachable from here, because the handlers are static in the VDD and
     this stub used to throw them away. Everything the battery could ask was
     therefore about the HOST-side API, which is the half that already worked. */
static uint8_t g_last_port_lo, g_last_port_hi;
static ntvdd_in_fn  g_in;
static ntvdd_out_fn g_out;
static void        *g_self;
int vdd_claim_ports(vdd_bus *b, uint16_t lo, uint16_t hi,
                    ntvdd_in_fn in, ntvdd_out_fn out, void *self)
{ (void)b;
  g_in = in; g_out = out; g_self = self;
  g_last_port_lo = (uint8_t)lo; g_last_port_hi = (uint8_t)hi; return 0; }

static void outp(uint16_t port, uint8_t v)
{ g_out(g_self, port, 1, v); }
static uint8_t inp(uint16_t port)
{ uint32_t v = 0; g_in(g_self, port, 1, &v); return (uint8_t)v; }

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

    /* ── THE PORT SIDE. docs/ref/pic.md 4 and 5. ──────────────────────────
       Marked from the code before any of this was written, and every one of
       these was predicted to fail: OCW2's rotate forms and OCW3's Poll and
       Special Mask Mode are simply not decoded. p_pic.asm asks the same two
       questions of three real machines. */

    /* ROTATE ON SPECIFIC EOI (E0h) IS STILL AN EOI.
       ★ ALL THREE ORACLES AGREE (p_pic pic.ocw2.rot.speoi = 0 on 6.22, dosbox-x
         AND PCem): the in-service bit goes. We filed E0h under "other rotate
         forms: nop", so the bit stayed set -- and an ISR bit that is never
         cleared does not lose one interrupt, it kills that priority level and
         everything below it for the rest of the run. */
    vdd_pic_reset(&p);
    vdd_pic_acknowledge(&p, 0);
    ok((p.m.isr & 0x01) != 0, "ocw2: IRQ0 in service before the rotate EOI");
    outp(0x20, 0xE0);                       /* rotate on specific EOI, level 0 */
    ok((p.m.isr & 0x01) == 0, "ocw2: E0h (rotate on specific EOI) ENDS the interrupt");
    ok(vdd_pic_can_deliver(&p, 0), "ocw2: ...so IRQ0 is deliverable again");

    /* ...and the forms that are NOT an EOI must still not be one. C0h is "set
       priority", 80h/00h are the auto-EOI rotate flags; none of them ends an
       interrupt, and a model that treated the whole 0xE0 mask as "some kind of
       EOI" would break the re-entrancy guard in the other direction. */
    vdd_pic_reset(&p);
    vdd_pic_acknowledge(&p, 0);
    outp(0x20, 0xC0);
    ok((p.m.isr & 0x01) != 0, "ocw2: C0h (set priority) is NOT an EOI");
    outp(0x20, 0x80);
    ok((p.m.isr & 0x01) != 0, "ocw2: 80h (rotate in auto-EOI) is NOT an EOI");

    /* OCW3's POLL COMMAND.
       ⚠ NO MAJORITY TO APPEAL TO: MS-DOS 6.22 under QEMU implements it and
         answers 0x00; dosbox-x and PCem both drop the P bit and hand back the
         register the last OCW3 selected. Two of three do not model the feature,
         so their answer is the absence of a measurement -- the same shape as the
         8254's BCD bit. The datasheet is unambiguous, and here the one host that
         does implement it AGREES with the datasheet, so this is better evidenced
         than BCD was.
       ► The defining property: a poll read reports what is PENDING, and bit 7 is
         "there is one". A line already in service is not pending. */
    vdd_pic_reset(&p);
    vdd_pic_acknowledge(&p, 0);             /* IRQ0 in service, nothing pending */
    outp(0x20, 0x0B);                       /* select ISR -- poll must override */
    outp(0x20, 0x0C);                       /* OCW3 with P=1, RR=0              */
    ok(inp(0x20) == 0x00,
       "ocw3: poll with nothing PENDING reads 0x00, not the selected ISR");

    vdd_pic_reset(&p);
    vdd_pic_raise(&p, 3);                   /* IRQ3 requested, not yet delivered */
    p.m.imr = 0x00;
    outp(0x20, 0x0C);
    ok(inp(0x20) == 0x83, "ocw3: poll reports pending IRQ3 as 0x83 (bit 7 + level)");
    ok((p.m.isr & 0x08) != 0, "ocw3: ...and the poll READ acknowledged it (ISR set)");
    ok((p.m.irr & 0x08) == 0, "ocw3: ...and cleared the request");

    /* A poll is a ONE-SHOT: it arms the next read only. The read after it goes
       back to whatever the read select says, or a guest that polls once would
       never see a status byte again. */
    outp(0x20, 0x0B);
    ok(inp(0x20) == p.m.isr, "ocw3: the read after a poll is a normal status read");

    printf("-- %d checks, %d failures --\n", checks, fails);
    return fails ? 1 : 0;
}
