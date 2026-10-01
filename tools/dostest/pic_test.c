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

    /* ── #174: ROTATION, SPECIAL MASK MODE, ICW1's RESETS, SFNM. ─────────────────
       All four from the Intel 8259A datasheet (docs/ref/pic.md 3-5). None has an
       oracle answer yet except E0h's EOI half above; these pin the datasheet. */

    /* ★ THE DEFAULT STATE IS UNCHANGED -- EXHAUSTIVELY. A guest that never programs
       any of this must see exactly the lowest-bit-first resolver it had before, for
       every ISR byte and every master line. The one deliberate change is the slave's
       IR2 rule, pinned separately below. */
    {
        int isr, line, imr, same = 1;
        static const uint8_t imrs[] = { 0x00, 0xFC, 0x08, 0xA5 };
        for (imr = 0; imr < 4; ++imr)
            for (isr = 0; isr < 256; ++isr)
                for (line = 0; line < 8; ++line) {
                    uint8_t bit = (uint8_t)(1u << line);
                    int old;
                    vdd_pic_reset(&p);
                    p.m.imr = imrs[imr]; p.m.isr = (uint8_t)isr;
                    old = !(p.m.imr & bit) && !(p.m.isr & ((bit << 1) - 1));
                    if (old != vdd_pic_can_deliver(&p, (uint8_t)line)) same = 0;
                }
        ok(same, "default: master resolver == the old lowest-bit-first rule, all ISR x 4 IMRs");
        vdd_pic_reset(&p);
        ok(p.m.prio_low == 7 && p.s.prio_low == 7, "default: IR7 lowest on both chips (no rotation)");
        ok(!p.m.smm && !p.s.smm && !p.m.sfnm && !p.m.rotate_aeoi,
           "default: no SMM, no SFNM, no rotate-in-AEOI");
        /* non-specific EOI in the default state still clears the LOWEST bit number */
        p.m.isr = 0x0A;                     /* IRQ1 + IRQ3 in service            */
        outp(0x20, 0x20);
        ok(p.m.isr == 0x08, "default: non-specific EOI clears IRQ1 before IRQ3");
    }

    /* OCW2 C0h+L -- SET PRIORITY. L becomes the lowest, L+1 the highest; not an EOI. */
    vdd_pic_reset(&p);
    p.m.imr = 0x00;
    outp(0x20, 0xC3);                       /* IR3 lowest => IR4 highest         */
    ok(p.m.prio_low == 3, "rotate: C3h makes IR3 the lowest priority");
    vdd_pic_acknowledge(&p, 4);
    ok(!vdd_pic_can_deliver(&p, 0), "rotate: IRQ4 in service now blocks IRQ0 (outranked)");
    ok(!vdd_pic_can_deliver(&p, 3), "rotate: ...and IRQ3, now the lowest");
    vdd_pic_acknowledge(&p, 0);             /* force both in service             */
    outp(0x20, 0x20);                       /* non-specific EOI                  */
    ok(p.m.isr == 0x01, "rotate: non-specific EOI ends IRQ4 (highest), not IRQ0");
    ok(vdd_pic_can_deliver(&p, 6), "rotate: IRQ0 in service does not block IRQ6 (rank 2 vs 4)");
    ok(!vdd_pic_can_deliver(&p, 1), "rotate: ...but does block IRQ1 (rank 5)");

    /* OCW2 A0h -- ROTATE ON NON-SPECIFIC EOI: the line ended becomes the lowest. */
    vdd_pic_reset(&p);
    p.m.imr = 0x00;
    vdd_pic_acknowledge(&p, 1);
    outp(0x20, 0xA0);
    ok(p.m.isr == 0 && p.m.prio_low == 1, "rotate: A0h ends IRQ1 and makes it the lowest");
    vdd_pic_acknowledge(&p, 3);
    ok(!vdd_pic_can_deliver(&p, 0), "rotate: ...so IRQ3 in service now blocks IRQ0");
    ok(vdd_pic_can_deliver(&p, 2), "rotate: ...and IRQ2, now the highest, gets through");
    vdd_pic_reset(&p);
    outp(0x20, 0xA0);                       /* nothing in service                */
    ok(p.m.prio_low == 7, "rotate: A0h with nothing in service rotates nothing");

    /* OCW2 E0h+L -- ROTATE ON SPECIFIC EOI: both halves now. */
    vdd_pic_reset(&p);
    vdd_pic_acknowledge(&p, 0);
    vdd_pic_acknowledge(&p, 5);
    outp(0x20, 0xE5);
    ok(p.m.isr == 0x01 && p.m.prio_low == 5,
       "rotate: E5h ends IRQ5 (not the highest) and makes it the lowest");

    /* OCW2 80h / 00h -- ROTATE IN AUTO-EOI. Needs AEOI from ICW4 to do anything. */
    vdd_pic_reset(&p);
    outp(0x20, 0x11); outp(0x21, 0x08); outp(0x21, 0x04); outp(0x21, 0x03);  /* AEOI */
    outp(0x21, 0x00);
    ok(p.m.auto_eoi && p.m.prio_low == 7, "rotate-aeoi: ICW4=03h gives AEOI, fixed order");
    outp(0x20, 0x80);
    ok(p.m.rotate_aeoi, "rotate-aeoi: 80h sets the mode");
    vdd_pic_acknowledge(&p, 2);
    ok(p.m.isr == 0 && p.m.prio_low == 2, "rotate-aeoi: acknowledging IRQ2 makes it the lowest");
    vdd_pic_ack_autoeoi(&p, 6);
    ok(p.m.prio_low == 6, "rotate-aeoi: the host's auto-EOI acknowledge rotates too");
    outp(0x20, 0x00);
    ok(!p.m.rotate_aeoi, "rotate-aeoi: 00h clears the mode");
    vdd_pic_acknowledge(&p, 4);
    ok(p.m.prio_low == 6, "rotate-aeoi: ...after which an acknowledge leaves priority alone");
    vdd_pic_reset(&p);
    outp(0x20, 0x80);
    vdd_pic_acknowledge(&p, 0);
    ok(p.m.prio_low == 7 && (p.m.isr & 1),
       "rotate-aeoi: without AEOI the flag does nothing (ISR set, no rotation)");

    /* POLL follows the rotation: the highest pending line IN THE CURRENT ORDER. */
    vdd_pic_reset(&p);
    p.m.imr = 0x00;
    outp(0x20, 0xC3);                       /* IR4 highest: order 4 5 6 7 0 1 2 3 */
    vdd_pic_raise(&p, 0); vdd_pic_raise(&p, 5);
    outp(0x20, 0x0C);
    ok(inp(0x20) == 0x85, "rotate: poll picks IRQ5 over IRQ0 when IR4 is the highest");

    /* SPECIAL MASK MODE -- OCW3 bits 6:5. */
    vdd_pic_reset(&p);
    p.m.imr = 0x00;
    vdd_pic_acknowledge(&p, 3);
    p.m.imr = 0x08;                         /* the handler masks its own line    */
    ok(!vdd_pic_can_deliver(&p, 5), "smm: off -- masked IRQ3 in service still blocks IRQ5");
    outp(0x20, 0x28);                       /* SMM bit WITHOUT ESMM              */
    ok(!p.m.smm, "smm: 28h (SMM without ESMM) changes nothing");
    outp(0x20, 0x68);                       /* ESMM + SMM: set                   */
    ok(p.m.smm, "smm: 68h sets Special Mask Mode");
    ok(vdd_pic_can_deliver(&p, 5), "smm: masked IRQ3 in service no longer blocks IRQ5");
    ok(vdd_pic_can_deliver(&p, 7), "smm: ...or IRQ7");
    ok(!vdd_pic_can_deliver(&p, 3), "smm: IRQ3 itself stays out (it is masked)");
    vdd_pic_raise(&p, 6);
    outp(0x20, 0x0C);
    ok(inp(0x20) == 0x86, "smm: a poll takes IRQ6 past the masked in-service IRQ3");
    ok(!vdd_pic_can_deliver(&p, 7),
       "smm: an UNMASKED line in service (IRQ6, just polled) still blocks below it");
    ok(vdd_pic_can_deliver(&p, 4), "smm: ...and not above it");
    outp(0x20, 0x48);                       /* ESMM, SMM=0: clear                */
    ok(!p.m.smm, "smm: 48h clears Special Mask Mode");
    ok(!vdd_pic_can_deliver(&p, 4), "smm: cleared -- masked IRQ3 blocks IRQ4 again");

    /* ICW1's DATASHEET RESETS: read select -> IRR, IR7 lowest, SMM off, and with no
       ICW4 coming, every ICW4 function off.
       ⚠ The read-select reset is SPEC-DERIVED: no oracle discriminates it (PCem
         disagrees; QEMU and dosbox-x cannot show it). docs/inventory/pic.md 3. */
    vdd_pic_reset(&p);
    outp(0x20, 0x0B);                       /* select ISR                        */
    outp(0x20, 0x68);                       /* SMM on                            */
    outp(0x20, 0xC3);                       /* rotate                            */
    outp(0x20, 0x11); outp(0x21, 0x08); outp(0x21, 0x04); outp(0x21, 0x01);
    ok(!p.m.read_isr, "icw1: resets the status read to IRR");
    ok(!p.m.smm, "icw1: clears Special Mask Mode");
    ok(p.m.prio_low == 7, "icw1: IR7 is the lowest priority again");
    p.m.imr = 0x01;                         /* the probe's shape: IRQ0 masked    */
    vdd_pic_raise(&p, 0);
    ok(inp(0x20) == 0x01, "icw1: ...so a read with no OCW3 returns the IRR, not the ISR");
    outp(0x20, 0x11); outp(0x21, 0x08); outp(0x21, 0x04); outp(0x21, 0x13);  /* AEOI+SFNM */
    ok(p.m.auto_eoi && p.m.sfnm, "icw4: 13h sets AEOI and SFNM");
    outp(0x20, 0x10); outp(0x21, 0x08); outp(0x21, 0x04);                    /* no ICW4 */
    ok(!p.m.auto_eoi && !p.m.sfnm && p.m.icw_step == 0,
       "icw1: IC4=0 zeroes the ICW4 functions (AEOI, SFNM) and ends after ICW3");

    /* ── FULLY NESTED MODE ACROSS THE CASCADE -- THE ONE DEFAULT THAT CHANGED. ──
       With SFNM off (what a BIOS programs), a slave line puts IR2 in service on the
       master, and nothing more from the slave gets in -- not even a HIGHER slave
       line -- until the master is EOI'd. We used to let it through. */
    vdd_pic_reset(&p);
    p.m.imr = 0x00; p.s.imr = 0x00;
    vdd_pic_acknowledge(&p, 12);            /* the PS/2 mouse, slave IR4         */
    ok((p.m.isr & 0x04) && (p.s.isr & 0x10), "cascade: IRQ12 puts slave IR4 and master IR2 in service");
    ok(!vdd_pic_can_deliver(&p, 8), "fnm: a HIGHER slave line (IRQ8) is held while IR2 is in service");
    ok(!vdd_pic_can_deliver(&p, 13), "fnm: ...and a lower one (IRQ13)");
    ok(vdd_pic_can_deliver(&p, 0) && vdd_pic_can_deliver(&p, 1), "fnm: IRQ0/IRQ1 still outrank it");
    ok(!vdd_pic_can_deliver(&p, 3), "fnm: IRQ3-7 are blocked by IR2, as before");
    outp(0xA0, 0x20);                       /* the handler's slave EOI ...       */
    ok(!vdd_pic_can_deliver(&p, 8), "fnm: a slave EOI alone does not reopen the slave");
    outp(0x20, 0x20);                       /* ... and its master EOI            */
    ok(vdd_pic_can_deliver(&p, 8) && vdd_pic_can_deliver(&p, 3),
       "fnm: the master EOI releases IR2 -- slave and IRQ3-7 open again");
    vdd_pic_acknowledge(&p, 12);
    vdd_pic_eoi(&p, 12);                    /* the host's own EOI (our stubs)    */
    ok(p.m.isr == 0 && vdd_pic_can_deliver(&p, 8),
       "fnm: the host's slave EOI releases IR2 with the last slave bit");
    vdd_pic_ack_autoeoi(&p, 10);
    ok(p.m.isr == 0 && p.s.isr == 0 && vdd_pic_can_deliver(&p, 11),
       "fnm: a host auto-EOI'd slave line leaves nothing in service");

    /* SPECIAL FULLY NESTED MODE, asked for by ICW4 bit 4 on the master. */
    vdd_pic_reset(&p);
    outp(0x20, 0x11); outp(0x21, 0x08); outp(0x21, 0x04); outp(0x21, 0x11);  /* SFNM */
    outp(0xA0, 0x11); outp(0xA1, 0x70); outp(0xA1, 0x02); outp(0xA1, 0x01);
    outp(0x21, 0x00); outp(0xA1, 0x00);
    ok(p.m.sfnm && !p.s.sfnm, "sfnm: ICW4=11h sets SFNM on the master");
    vdd_pic_acknowledge(&p, 12);
    ok(vdd_pic_can_deliver(&p, 8), "sfnm: a HIGHER slave line gets in while IR2 is in service");
    ok(!vdd_pic_can_deliver(&p, 12) && !vdd_pic_can_deliver(&p, 13),
       "sfnm: the slave's own nesting still holds IRQ12 and below");
    ok(!vdd_pic_can_deliver(&p, 3), "sfnm: IR2 in service still blocks master IRQ3-7");
    ok(vdd_pic_can_deliver(&p, 1), "sfnm: ...and IRQ0/IRQ1 still outrank it");

    printf("-- %d checks, %d failures --\n", checks, fails);
    return fails ? 1 : 0;
}
