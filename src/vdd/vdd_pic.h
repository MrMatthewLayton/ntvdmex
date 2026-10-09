/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The 8259A interrupt controller pair (ports 0x20/0x21, 0xA0/0xA1).
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
 * No Windows calls, only Windows types: state is explicit and effects go through the bus, so the
 * whole thing is exercised off-VM by tests/unit/pic_test.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDD_PIC_H
#define NTVDMEX_VDD_PIC_H

#include "ntvdd.h"

/* PicIsLineOpen: which 8259. */
#define PIC_CHIP_SLAVE      0
#define PIC_CHIP_MASTER     1

#define PIC_DEVICE_NAME     "pic"

typedef struct _PIC_CHIP
{
    BYTE Imr;               /* OCW1: 1 = line masked */
    BYTE Irr;               /* requests raised but not yet delivered */
    BYTE Isr;               /* delivered and not yet EOI'd */
    BYTE VectorBase;        /* ICW2: vector base (master 0x08, slave 0x70) */
    BYTE IcwStep;           /* 0 = running; 1..3 = expecting ICW2/3/4 */
    BYTE IsIcw4Needed;      /* from ICW1 bit 0 */
    BYTE IsIsrSelected;     /* OCW3: next read of the base port returns ISR */
    BYTE IsAutoEoi;         /* ICW4 bit 1: clear ISR at delivery time */
    BYTE IsPollArmed;       /* OCW3 bit 2: the NEXT base-port read is a POLL, and
                               a poll read is an ACKNOWLEDGE -- it sets ISR and
                               clears IRR exactly as a delivery would. One-shot:
                               the read consumes it. See docs/ref/pic.md 5.      */
    /* THE PROGRAMMING INTERFACE A PC BIOS NEVER TOUCHES (#174). All four reset to
     * the fixed-priority, fully nested chip every DOS program assumes, so a guest
     * that does not program them sees nothing new. docs/ref/pic.md 4-5.
     */
    BYTE LowestPriority;    /* the IR line with the LOWEST priority; 7 = fixed order
                               (IR0 highest). OCW2 C0h/A0h/E0h and rotate-in-AEOI
                               move it; ICW1 puts it back to 7.                   */
    BYTE IsRotateInAutoEoi; /* OCW2 80h sets / 00h clears: in AEOI mode, the line
                               just acknowledged becomes the lowest priority.     */
    BYTE IsSpecialMaskMode; /* OCW3 bits 6:5 = 11 sets / 10 clears Special Mask
                               Mode: a MASKED in-service line stops blocking.     */
    BYTE IsSpecialFullyNested; /* ICW4 bit 4, Special Fully Nested Mode. Meaningful on
                               the master only: IR2 in service no longer blocks a
                               further slave request.                             */
} PIC_CHIP, *PPIC_CHIP;

typedef const PIC_CHIP *PCPIC_CHIP;

typedef struct _PIC_STATE
{
    PVDD_BUS Bus;
    PIC_CHIP Master;
    PIC_CHIP Slave;
} PIC_STATE, *PPIC_STATE;

INT  VddPicInitialize(_In_ PVDD_BUS bus, _In_ PVOID context);
VOID VddPicReset(_In_ PVOID context);

/* what the host asks the PIC: */

/* May line `irq` (0-15) be delivered right now? False if it is masked, or if it or
 * a higher-priority line is still in service. This is the whole point: it is what
 * stops an injected handler being re-entered before it has EOI'd. "Higher priority"
 * follows the chip's rotation and Special Mask Mode; a slave line also needs the
 * master to accept IR2, which in fully nested mode (no SFNM) means IR2 itself must not
 * be in service.
 *
 * [CAUTION]: It answers for ONE line. Which of several pending lines goes first is the host's
 * walk (g_irq_order in main.c), which is the FIXED order -- right unless a guest has
 * rotated the priorities.
 */
INT  VddPicCanDeliver(_In_ PPIC_STATE state, _In_ BYTE irq);

/* Record that the host has just vectored `irq` into the guest: sets the in-service
 * bit (unless the chip is in auto-EOI mode) and clears the pending request.
 */
VOID VddPicAcknowledge(_Inout_ PPIC_STATE state, _In_ BYTE irq);

/* The guest's vector for a line, from the programmed base (master 8 -> INT 08h). */
BYTE VddPicVector(_In_ PPIC_STATE state, _In_ BYTE irq);

/* Note a line as requested; used for IRR bookkeeping/reporting. */
VOID VddPicRaise(_Inout_ PPIC_STATE state, _In_ BYTE irq);

/* End-of-interrupt for one line. Guests normally do this themselves by writing OCW2 to
 * port 0x20, and now that we claim the port they reach the same state. The host needs it
 * directly for two cases the guest cannot cover: our own BIOS stand-in INT 08h handler
 * (the real BIOS timer ISR ends with an EOI, and ours is a BOP with nowhere to put one),
 * and lines vectored at our default do-nothing stubs, which by definition never EOI.
 */
VOID VddPicEndOfInterrupt(_Inout_ PPIC_STATE state, _In_ BYTE irq);
/* acknowledge()+eoi() as ONE operation, for lines the host auto-EOIs. Touches only IRR,
 * so it is safe from a thread that does not hold the device lock. See the .c file.
 */
VOID VddPicAcknowledgeAutoEoi(_Inout_ PPIC_STATE state, _In_ BYTE irq);

static inline NTVDD_DEVICE VddPicDevice(_In_ PPIC_STATE state)
{
    NTVDD_DEVICE device;

    device.Name = PIC_DEVICE_NAME;
    device.Initialize = VddPicInitialize;
    device.Reset = VddPicReset;
    device.Shutdown = 0;
    device.Context = state;
    return device;
}

#endif /* NTVDMEX_VDD_PIC_H */
