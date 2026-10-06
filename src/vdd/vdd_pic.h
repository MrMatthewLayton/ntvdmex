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
 * whole thing is exercised off-VM by tests/unit/pic_test.c.
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
    uint8_t poll_armed;     /* OCW3 bit 2: the NEXT base-port read is a POLL, and
                               a poll read is an ACKNOWLEDGE -- it sets ISR and
                               clears IRR exactly as a delivery would. One-shot:
                               the read consumes it. See docs/ref/pic.md 5.      */
    /* ── THE PROGRAMMING INTERFACE A PC BIOS NEVER TOUCHES (#174). All four reset to
         the fixed-priority, fully nested chip every DOS program assumes, so a guest
         that does not program them sees nothing new. docs/ref/pic.md 4-5. */
    uint8_t prio_low;       /* the IR line with the LOWEST priority; 7 = fixed order
                               (IR0 highest). OCW2 C0h/A0h/E0h and rotate-in-AEOI
                               move it; ICW1 puts it back to 7.                   */
    uint8_t rotate_aeoi;    /* OCW2 80h sets / 00h clears: in AEOI mode, the line
                               just acknowledged becomes the lowest priority.     */
    uint8_t smm;            /* OCW3 bits 6:5 = 11 sets / 10 clears Special Mask
                               Mode: a MASKED in-service line stops blocking.     */
    uint8_t sfnm;           /* ICW4 bit 4, Special Fully Nested Mode. Meaningful on
                               the master only: IR2 in service no longer blocks a
                               further slave request.                             */
} pic_chip;

typedef struct pic_state {
    VDD_BUS *bus;
    pic_chip m, s;          /* master, slave                                      */
} pic_state;

int vdd_pic_init(VDD_BUS *b, void *self);
void vdd_pic_reset(void *self);

/* --- what the host asks the PIC -------------------------------------------- */

/* May line `irq` (0-15) be delivered right now? False if it is masked, or if it or
   a higher-priority line is still in service. This is the whole point: it is what
   stops an injected handler being re-entered before it has EOI'd. "Higher priority"
   follows the chip's rotation and Special Mask Mode; a slave line also needs the
   master to accept IR2, which in fully nested mode (no SFNM) means IR2 itself must not
   be in service.
   ⚠ It answers for ONE line. Which of several pending lines goes first is the host's
     walk (g_irq_order in main.c), which is the FIXED order -- right unless a guest has
     rotated the priorities. */
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
/* acknowledge()+eoi() as ONE operation, for lines the host auto-EOIs. Touches only IRR,
   so it is safe from a thread that does not hold the device lock. See the .c file. */
void vdd_pic_ack_autoeoi(pic_state *st, uint8_t irq);

static inline NTVDD_DEVICE vdd_pic_device(pic_state *st)
{
    NTVDD_DEVICE d;
    d.Name = "pic"; d.Initialize = vdd_pic_init; d.Reset = vdd_pic_reset;
    d.Shutdown = 0; d.Context = st;
    return d;
}

#endif /* NTVDMEX_VDD_PIC_H */
