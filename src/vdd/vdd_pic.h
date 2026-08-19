/*
 * vdd_pic.h -- the 8259A interrupt controller pair (ports 0x20/0x21, 0xA0/0xA1).
 *
 * WHY THIS EXISTS, and why it is not optional any more. Once the host could inject
 * interrupts asynchronously (SuspendThread + SetThreadContext, session 11) there was
 * nothing left to stop it injecting a line into a handler that was already running:
 * a keyboard ISR re-enables interrupts early, as they do, so the next IRQ1 landed
 * inside it, and the next inside that. Skyroads became playable and then hung the
 * instant a key was pressed. Two heuristics were tried and both were wrong -- a
 * "busy" flag with a timeout throttled the timer to 4 ticks/sec, and comparing the
 * guest's SP against the injection point misfires constantly because normal guest
 * code runs deeper on the stack than the point we interrupted.
 *
 * The hardware has always had the right answer: an IN-SERVICE register. The PIC sets
 * ISR bit n when it delivers line n, and refuses to deliver n (or anything of lower
 * priority) until the handler acknowledges with an EOI. Guests already send that EOI
 * -- Skyroads writes 0x20 to port 0x20 several hundred times a run, and until now
 * nothing claimed the port, so it went nowhere. Claiming it turns the guess into a
 * fact, and brings IRQ MASKING with it (the IMR at 0x21), which games use to silence
 * lines they are not servicing.
 *
 * Pure C, no <windows.h>: state is explicit and effects go through the bus, so the
 * whole thing is exercised off-VM by tools/dostest/pic_test.c.
 */
#ifndef NTVDMEX_VDD_PIC_H
#define NTVDMEX_VDD_PIC_H

#include "ntvdd.h"

typedef struct pic_chip {
    uint8_t imr;            /* OCW1: 1 = line masked                              */
    uint8_t irr;            /* requests raised but not yet delivered              */
    uint8_t isr;            /* delivered and not yet EOI'd                        */
    uint8_t base;           /* ICW2: vector base (master 0x08, slave 0x70)        */
    uint8_t icw_step;       /* 0 = running; 1..3 = expecting ICW2/3/4             */
    uint8_t icw4_needed;    /* from ICW1 bit 0                                    */
    uint8_t read_isr;       /* OCW3: next read of the base port returns ISR       */
    uint8_t auto_eoi;       /* ICW4 bit 1: clear ISR at delivery time             */
} pic_chip;

typedef struct pic_state {
    vdd_bus *bus;
    pic_chip m, s;          /* master, slave                                      */
} pic_state;

int vdd_pic_init(vdd_bus *b, void *self);
void vdd_pic_reset(void *self);

/* --- what the host asks the PIC -------------------------------------------- */

/* May line `irq` (0-15) be delivered right now? False if it is masked, or if it or
   a higher-priority line is still in service. This is the whole point: it is what
   stops an injected handler being re-entered before it has EOI'd. */
int  vdd_pic_can_deliver(pic_state *st, uint8_t irq);

/* Record that the host has just vectored `irq` into the guest: sets the in-service
   bit (unless the chip is in auto-EOI mode) and clears the pending request. */
void vdd_pic_acknowledge(pic_state *st, uint8_t irq);

/* The guest's vector for a line, from the programmed base (master 8 -> INT 08h). */
uint8_t vdd_pic_vector(pic_state *st, uint8_t irq);

/* Note a line as requested; used for IRR bookkeeping/reporting. */
void vdd_pic_raise(pic_state *st, uint8_t irq);

/* End-of-interrupt for one line. Guests normally do this themselves by writing OCW2 to
   port 0x20, and now that we claim the port they reach the same state. The host needs it
   directly for two cases the guest cannot cover: our own BIOS stand-in INT 08h handler
   (the real BIOS timer ISR ends with an EOI, and ours is a BOP with nowhere to put one),
   and lines vectored at our default do-nothing stubs, which by definition never EOI. */
void vdd_pic_eoi(pic_state *st, uint8_t irq);

static inline ntvdd vdd_pic_device(pic_state *st)
{
    ntvdd d;
    d.name = "pic"; d.init = vdd_pic_init; d.reset = vdd_pic_reset;
    d.shutdown = 0; d.self = st;
    return d;
}

#endif /* NTVDMEX_VDD_PIC_H */
