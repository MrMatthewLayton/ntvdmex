/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for the 8259A VDD (src/vdd/vdd_pic.c).
 *
 * The in-service logic here is what stops an injected interrupt handler being
 * re-entered before it EOIs, which is the difference between Skyroads being
 * playable and Skyroads hanging the moment you press a key. Two heuristics were
 * tried in the host first and both were wrong in ways that only showed up on real
 * hardware, so this pins the real rules down where they can be checked in a second.
 *
 * Build+run via tests/probes/dos/run.sh.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "vdd_pic.h"

static INT g_Checks = 0, g_Failures = 0;
static VOID PicTestCheck(INT condition, PCSTR description)
{
    ++g_Checks;
    if (!condition)
    {
        ++g_Failures;
        printf("  FAIL  %s\n", description);
    }
    else
        printf("  PASS  %s\n", description);
}

/* The VDD talks to the bus only to claim ports, so a stub bus is enough.
 *
 * [CAUTION]: IT ALSO KEEPS THE HANDLERS IT WAS HANDED. The port side of this chip -- ICW1,
 * OCW2's EOI family, OCW3's read select and poll -- is half the device and none
 * of it was reachable from here, because the handlers are static in the VDD and
 * this stub used to throw them away. Everything the battery could ask was
 * therefore about the HOST-side API, which is the half that already worked.
 */
static BYTE g_LastFirstPort, g_LastLastPort;
static PVDD_PORT_IN_ROUTINE  g_InRoutine;
static PVDD_PORT_OUT_ROUTINE g_OutRoutine;
static PVOID g_Context;
INT VddClaimPorts(
    PVDD_BUS bus,
    WORD firstPort,
    WORD lastPort,
    PVDD_PORT_IN_ROUTINE inRoutine,
    PVDD_PORT_OUT_ROUTINE outRoutine,
    PVOID context)
{ (VOID)bus;
  g_InRoutine = inRoutine;
  g_OutRoutine = outRoutine;
  g_Context = context;
  g_LastFirstPort = (BYTE)firstPort;
  g_LastLastPort = (BYTE)lastPort;
  return 0; }

static VOID PicTestOut(WORD port, BYTE value)
{
    g_OutRoutine(g_Context, port, 1, value);
}

static BYTE PicTestIn(WORD port)
{
    UINT32 value = 0;

    g_InRoutine(g_Context, port, 1, &value);
    return (BYTE)value;
}

/* Reach the port handlers the way the bus would. They are static in the VDD, so
 * drive them through the device descriptor's init + the public host API instead.
 */
extern INT VddPicInitialize(PVDD_BUS bus, PVOID context);

INT main(VOID)
{
    PIC_STATE pic;

    memset(&pic, 0, sizeof pic);
    VddPicInitialize((PVDD_BUS)0, &pic);

    printf("-- 8259A PIC VDD --\n");

    /* Reset state: a BIOS leaves IRQ0/IRQ1 live for a DOS program that never
     * touches the chip, and everything else masked.
     */
    PicTestCheck(pic.Master.VectorBase == 0x08, "master vector base is 0x08 (IRQ0 -> INT 08h)");
    PicTestCheck(pic.Slave.VectorBase == 0x70, "slave vector base is 0x70 (IRQ8 -> INT 70h)");
    PicTestCheck(VddPicVector(&pic, 0) == 0x08, "vector(IRQ0) = 0x08");
    PicTestCheck(VddPicVector(&pic, 5) == 0x0D, "vector(IRQ5) = 0x0D");
    PicTestCheck(VddPicVector(&pic, 8) == 0x70, "vector(IRQ8) = 0x70");
    PicTestCheck(VddPicCanDeliver(&pic, 0), "IRQ0 deliverable at reset");
    PicTestCheck(VddPicCanDeliver(&pic, 1), "IRQ1 deliverable at reset");
    PicTestCheck(!VddPicCanDeliver(&pic, 5), "IRQ5 masked at reset");

    /* THE CORE RULE: a line in service blocks itself until EOI. */
    VddPicAcknowledge(&pic, 0);
    PicTestCheck(!VddPicCanDeliver(&pic, 0), "IRQ0 in service blocks IRQ0 (no re-entry)");
    PicTestCheck(!VddPicCanDeliver(&pic, 1), "IRQ0 in service blocks lower-priority IRQ1");

    /* EOI to the master releases it. */
    PPIC_STATE picPointer = &pic;
    {
        extern VOID VddPicReset(PVOID self);
        (VOID)picPointer;
    }
    /* non-specific EOI via the command port */
    {
        NTVDD_DEVICE device = VddPicDevice(&pic);
        (VOID)device;
    }
    /* drive OCW2 through the same path the guest uses */
    {
        /* PicPortOut is static; emulate the guest's `out 20h,20h` by calling the
         * documented host API sequence it results in.
         */
        pic.Master.Isr &= (BYTE)~1u;      /* non-specific EOI clears highest in service */
    }
    PicTestCheck(VddPicCanDeliver(&pic, 0), "IRQ0 deliverable again after EOI");
    PicTestCheck(VddPicCanDeliver(&pic, 1), "IRQ1 deliverable again after EOI");

    /* Priority: a lower-priority line in service must NOT block a higher one --
     * that is what lets the timer pre-empt a keyboard handler, as on real iron.
     */
    VddPicReset(&pic);
    VddPicAcknowledge(&pic, 1);
    PicTestCheck(!VddPicCanDeliver(&pic, 1), "IRQ1 in service blocks IRQ1");
    PicTestCheck(VddPicCanDeliver(&pic, 0),  "IRQ1 in service does NOT block higher-priority IRQ0");

    /* Masking. */
    VddPicReset(&pic);
    pic.Master.Imr |= 0x01;
    PicTestCheck(!VddPicCanDeliver(&pic, 0), "masked IRQ0 is not deliverable");
    pic.Master.Imr &= (BYTE)~0x01;
    PicTestCheck(VddPicCanDeliver(&pic, 0), "unmasked IRQ0 is deliverable again");

    /* Auto-EOI never leaves a line in service. */
    VddPicReset(&pic);
    pic.Master.IsAutoEoi = 1;
    VddPicAcknowledge(&pic, 0);
    PicTestCheck(pic.Master.Isr == 0, "auto-EOI leaves nothing in service");
    PicTestCheck(VddPicCanDeliver(&pic, 0), "auto-EOI line stays deliverable");

    /* Slave lines are gated by the master's cascade (IRQ2). */
    VddPicReset(&pic);
    pic.Slave.Imr = 0x00;
    pic.Master.Imr = 0x00;
    PicTestCheck(VddPicCanDeliver(&pic, 9), "IRQ9 deliverable when cascade is open");
    pic.Master.Imr |= 0x04;
    PicTestCheck(!VddPicCanDeliver(&pic, 9), "IRQ9 blocked when the cascade line is masked");

    /* Raising sets the request bit, acknowledging clears it. */
    VddPicReset(&pic);
    VddPicRaise(&pic, 1);
    PicTestCheck((pic.Master.Irr & 0x02) != 0, "raise(IRQ1) sets the request bit");
    VddPicAcknowledge(&pic, 1);
    PicTestCheck((pic.Master.Irr & 0x02) == 0, "acknowledge(IRQ1) clears the request bit");
    PicTestCheck((pic.Master.Isr & 0x02) != 0, "acknowledge(IRQ1) sets the in-service bit");

    /* THE PORT SIDE. docs/ref/pic.md 4 and 5:
     * Marked from the code before any of this was written, and every one of
     * these was predicted to fail: OCW2's rotate forms and OCW3's Poll and
     * Special Mask Mode are simply not decoded. p_pic.asm asks the same two
     * questions of three real machines.
     */

    /* ROTATE ON SPECIFIC EOI (E0h) IS STILL AN EOI.
     *
     * [INFO]: ALL THREE ORACLES AGREE (p_pic pic.ocw2.rot.speoi = 0 on 6.22, dosbox-x
     * AND PCem): the in-service bit goes. We filed E0h under "other rotate
     * forms: nop", so the bit stayed set -- and an ISR bit that is never
     * cleared does not lose one interrupt, it kills that priority level and
     * everything below it for the rest of the run.
     */
    VddPicReset(&pic);
    VddPicAcknowledge(&pic, 0);
    PicTestCheck((pic.Master.Isr & 0x01) != 0, "ocw2: IRQ0 in service before the rotate EOI");
    PicTestOut(0x20, 0xE0);                       /* rotate on specific EOI, level 0 */
    PicTestCheck((pic.Master.Isr & 0x01) == 0, "ocw2: E0h (rotate on specific EOI) ENDS the interrupt");
    PicTestCheck(VddPicCanDeliver(&pic, 0), "ocw2: ...so IRQ0 is deliverable again");

    /* ...and the forms that are NOT an EOI must still not be one. C0h is "set
     * priority", 80h/00h are the auto-EOI rotate flags; none of them ends an
     * interrupt, and a model that treated the whole 0xE0 mask as "some kind of
     * EOI" would break the re-entrancy guard in the other direction.
     */
    VddPicReset(&pic);
    VddPicAcknowledge(&pic, 0);
    PicTestOut(0x20, 0xC0);
    PicTestCheck((pic.Master.Isr & 0x01) != 0, "ocw2: C0h (set priority) is NOT an EOI");
    PicTestOut(0x20, 0x80);
    PicTestCheck((pic.Master.Isr & 0x01) != 0, "ocw2: 80h (rotate in auto-EOI) is NOT an EOI");

    /* OCW3's POLL COMMAND.
     *
     * [CAUTION]: NO MAJORITY TO APPEAL TO: MS-DOS 6.22 under QEMU implements it and
     * answers 0x00; dosbox-x and PCem both drop the P bit and hand back the
     * register the last OCW3 selected. Two of three do not model the feature,
     * so their answer is the absence of a measurement -- the same shape as the
     * 8254's BCD bit. The datasheet is unambiguous, and here the one host that
     * does implement it AGREES with the datasheet, so this is better evidenced
     * than BCD was.
     * - The defining property: a poll read reports what is PENDING, and bit 7 is
     *   "there is one". A line already in service is not pending.
     */
    VddPicReset(&pic);
    VddPicAcknowledge(&pic, 0);             /* IRQ0 in service, nothing pending */
    PicTestOut(0x20, 0x0B);                       /* select ISR -- poll must override */
    PicTestOut(0x20, 0x0C);                       /* OCW3 with P=1, RR=0 */
    PicTestCheck(PicTestIn(0x20) == 0x00,
       "ocw3: poll with nothing PENDING reads 0x00, not the selected ISR");

    VddPicReset(&pic);
    VddPicRaise(&pic, 3);                   /* IRQ3 requested, not yet delivered */
    pic.Master.Imr = 0x00;
    PicTestOut(0x20, 0x0C);
    PicTestCheck(PicTestIn(0x20) == 0x83, "ocw3: poll reports pending IRQ3 as 0x83 (bit 7 + level)");
    PicTestCheck((pic.Master.Isr & 0x08) != 0, "ocw3: ...and the poll READ acknowledged it (ISR set)");
    PicTestCheck((pic.Master.Irr & 0x08) == 0, "ocw3: ...and cleared the request");

    /* A poll is a ONE-SHOT: it arms the next read only. The read after it goes
     * back to whatever the read select says, or a guest that polls once would
     * never see a status byte again.
     */
    PicTestOut(0x20, 0x0B);
    PicTestCheck(PicTestIn(0x20) == pic.Master.Isr, "ocw3: the read after a poll is a normal status read");

    /* #174: ROTATION, SPECIAL MASK MODE, ICW1's RESETS, SFNM:
     * All four from the Intel 8259A datasheet (docs/ref/pic.md 3-5). None has an
     * oracle answer yet except E0h's EOI half above; these pin the datasheet.
     */

    /* [INFO]: THE DEFAULT STATE IS UNCHANGED -- EXHAUSTIVELY. A guest that never programs
     * any of this must see exactly the lowest-bit-first resolver it had before, for
     * every ISR byte and every master line. The one deliberate change is the slave's
     * IR2 rule, pinned separately below.
     */
    {
        INT isr, line, imr, same = 1;
        static const BYTE imrs[] = { 0x00, 0xFC, 0x08, 0xA5 };
        for (imr = 0; imr < 4; ++imr)
            for (isr = 0; isr < 256; ++isr)
                for (line = 0; line < 8; ++line)
                {
                    BYTE bit = (BYTE)(1u << line);
                    INT old;
                    VddPicReset(&pic);
                    pic.Master.Imr = imrs[imr];
                    pic.Master.Isr = (BYTE)isr;
                    old = !(pic.Master.Imr & bit) && !(pic.Master.Isr & ((bit << 1) - 1));
                    if (old != VddPicCanDeliver(&pic, (BYTE)line))
                        same = 0;
                }
        PicTestCheck(same, "default: master resolver == the old lowest-bit-first rule, all ISR x 4 IMRs");
        VddPicReset(&pic);
        PicTestCheck(pic.Master.LowestPriority == 7 && pic.Slave.LowestPriority == 7, "default: IR7 lowest on both chips (no rotation)");
        PicTestCheck(!pic.Master.IsSpecialMaskMode && !pic.Slave.IsSpecialMaskMode && !pic.Master.IsSpecialFullyNested && !pic.Master.IsRotateInAutoEoi,
           "default: no SMM, no SFNM, no rotate-in-AEOI");
        /* non-specific EOI in the default state still clears the LOWEST bit number */
        pic.Master.Isr = 0x0A;                     /* IRQ1 + IRQ3 in service */
        PicTestOut(0x20, 0x20);
        PicTestCheck(pic.Master.Isr == 0x08, "default: non-specific EOI clears IRQ1 before IRQ3");
    }

    /* OCW2 C0h+L -- SET PRIORITY. L becomes the lowest, L+1 the highest; not an EOI. */
    VddPicReset(&pic);
    pic.Master.Imr = 0x00;
    PicTestOut(0x20, 0xC3);                       /* IR3 lowest => IR4 highest */
    PicTestCheck(pic.Master.LowestPriority == 3, "rotate: C3h makes IR3 the lowest priority");
    VddPicAcknowledge(&pic, 4);
    PicTestCheck(!VddPicCanDeliver(&pic, 0), "rotate: IRQ4 in service now blocks IRQ0 (outranked)");
    PicTestCheck(!VddPicCanDeliver(&pic, 3), "rotate: ...and IRQ3, now the lowest");
    VddPicAcknowledge(&pic, 0);             /* force both in service */
    PicTestOut(0x20, 0x20);                       /* non-specific EOI */
    PicTestCheck(pic.Master.Isr == 0x01, "rotate: non-specific EOI ends IRQ4 (highest), not IRQ0");
    PicTestCheck(VddPicCanDeliver(&pic, 6), "rotate: IRQ0 in service does not block IRQ6 (rank 2 vs 4)");
    PicTestCheck(!VddPicCanDeliver(&pic, 1), "rotate: ...but does block IRQ1 (rank 5)");

    /* OCW2 A0h -- ROTATE ON NON-SPECIFIC EOI: the line ended becomes the lowest. */
    VddPicReset(&pic);
    pic.Master.Imr = 0x00;
    VddPicAcknowledge(&pic, 1);
    PicTestOut(0x20, 0xA0);
    PicTestCheck(pic.Master.Isr == 0 && pic.Master.LowestPriority == 1, "rotate: A0h ends IRQ1 and makes it the lowest");
    VddPicAcknowledge(&pic, 3);
    PicTestCheck(!VddPicCanDeliver(&pic, 0), "rotate: ...so IRQ3 in service now blocks IRQ0");
    PicTestCheck(VddPicCanDeliver(&pic, 2), "rotate: ...and IRQ2, now the highest, gets through");
    VddPicReset(&pic);
    PicTestOut(0x20, 0xA0);                       /* nothing in service */
    PicTestCheck(pic.Master.LowestPriority == 7, "rotate: A0h with nothing in service rotates nothing");

    /* OCW2 E0h+L -- ROTATE ON SPECIFIC EOI: both halves now. */
    VddPicReset(&pic);
    VddPicAcknowledge(&pic, 0);
    VddPicAcknowledge(&pic, 5);
    PicTestOut(0x20, 0xE5);
    PicTestCheck(pic.Master.Isr == 0x01 && pic.Master.LowestPriority == 5,
       "rotate: E5h ends IRQ5 (not the highest) and makes it the lowest");

    /* OCW2 80h / 00h -- ROTATE IN AUTO-EOI. Needs AEOI from ICW4 to do anything. */
    VddPicReset(&pic);
    PicTestOut(0x20, 0x11);
    PicTestOut(0x21, 0x08);
    PicTestOut(0x21, 0x04);
    PicTestOut(0x21, 0x03);  /* AEOI */
    PicTestOut(0x21, 0x00);
    PicTestCheck(pic.Master.IsAutoEoi && pic.Master.LowestPriority == 7, "rotate-aeoi: ICW4=03h gives AEOI, fixed order");
    PicTestOut(0x20, 0x80);
    PicTestCheck(pic.Master.IsRotateInAutoEoi, "rotate-aeoi: 80h sets the mode");
    VddPicAcknowledge(&pic, 2);
    PicTestCheck(pic.Master.Isr == 0 && pic.Master.LowestPriority == 2, "rotate-aeoi: acknowledging IRQ2 makes it the lowest");
    VddPicAcknowledgeAutoEoi(&pic, 6);
    PicTestCheck(pic.Master.LowestPriority == 6, "rotate-aeoi: the host's auto-EOI acknowledge rotates too");
    PicTestOut(0x20, 0x00);
    PicTestCheck(!pic.Master.IsRotateInAutoEoi, "rotate-aeoi: 00h clears the mode");
    VddPicAcknowledge(&pic, 4);
    PicTestCheck(pic.Master.LowestPriority == 6, "rotate-aeoi: ...after which an acknowledge leaves priority alone");
    VddPicReset(&pic);
    PicTestOut(0x20, 0x80);
    VddPicAcknowledge(&pic, 0);
    PicTestCheck(pic.Master.LowestPriority == 7 && (pic.Master.Isr & 1),
       "rotate-aeoi: without AEOI the flag does nothing (ISR set, no rotation)");

    /* POLL follows the rotation: the highest pending line IN THE CURRENT ORDER. */
    VddPicReset(&pic);
    pic.Master.Imr = 0x00;
    PicTestOut(0x20, 0xC3);                       /* IR4 highest: order 4 5 6 7 0 1 2 3 */
    VddPicRaise(&pic, 0);
    VddPicRaise(&pic, 5);
    PicTestOut(0x20, 0x0C);
    PicTestCheck(PicTestIn(0x20) == 0x85, "rotate: poll picks IRQ5 over IRQ0 when IR4 is the highest");

    /* SPECIAL MASK MODE -- OCW3 bits 6:5. */
    VddPicReset(&pic);
    pic.Master.Imr = 0x00;
    VddPicAcknowledge(&pic, 3);
    pic.Master.Imr = 0x08;                         /* the handler masks its own line */
    PicTestCheck(!VddPicCanDeliver(&pic, 5), "smm: off -- masked IRQ3 in service still blocks IRQ5");
    PicTestOut(0x20, 0x28);                       /* SMM bit WITHOUT ESMM */
    PicTestCheck(!pic.Master.IsSpecialMaskMode, "smm: 28h (SMM without ESMM) changes nothing");
    PicTestOut(0x20, 0x68);                       /* ESMM + SMM: set */
    PicTestCheck(pic.Master.IsSpecialMaskMode, "smm: 68h sets Special Mask Mode");
    PicTestCheck(VddPicCanDeliver(&pic, 5), "smm: masked IRQ3 in service no longer blocks IRQ5");
    PicTestCheck(VddPicCanDeliver(&pic, 7), "smm: ...or IRQ7");
    PicTestCheck(!VddPicCanDeliver(&pic, 3), "smm: IRQ3 itself stays out (it is masked)");
    VddPicRaise(&pic, 6);
    PicTestOut(0x20, 0x0C);
    PicTestCheck(PicTestIn(0x20) == 0x86, "smm: a poll takes IRQ6 past the masked in-service IRQ3");
    PicTestCheck(!VddPicCanDeliver(&pic, 7),
       "smm: an UNMASKED line in service (IRQ6, just polled) still blocks below it");
    PicTestCheck(VddPicCanDeliver(&pic, 4), "smm: ...and not above it");
    PicTestOut(0x20, 0x48);                       /* ESMM, SMM=0: clear */
    PicTestCheck(!pic.Master.IsSpecialMaskMode, "smm: 48h clears Special Mask Mode");
    PicTestCheck(!VddPicCanDeliver(&pic, 4), "smm: cleared -- masked IRQ3 blocks IRQ4 again");

    /* ICW1's DATASHEET RESETS: read select -> IRR, IR7 lowest, SMM off, and with no
     * ICW4 coming, every ICW4 function off.
     *
     * [CAUTION]: The read-select reset is SPEC-DERIVED: no oracle discriminates it (PCem
     * disagrees; QEMU and dosbox-x cannot show it). docs/inventory/pic.md 3.
     */
    VddPicReset(&pic);
    PicTestOut(0x20, 0x0B);                       /* select ISR */
    PicTestOut(0x20, 0x68);                       /* SMM on */
    PicTestOut(0x20, 0xC3);                       /* rotate */
    PicTestOut(0x20, 0x11);
    PicTestOut(0x21, 0x08);
    PicTestOut(0x21, 0x04);
    PicTestOut(0x21, 0x01);
    PicTestCheck(!pic.Master.IsIsrSelected, "icw1: resets the status read to IRR");
    PicTestCheck(!pic.Master.IsSpecialMaskMode, "icw1: clears Special Mask Mode");
    PicTestCheck(pic.Master.LowestPriority == 7, "icw1: IR7 is the lowest priority again");
    pic.Master.Imr = 0x01;                         /* the probe's shape: IRQ0 masked */
    VddPicRaise(&pic, 0);
    PicTestCheck(PicTestIn(0x20) == 0x01, "icw1: ...so a read with no OCW3 returns the IRR, not the ISR");
    PicTestOut(0x20, 0x11);
    PicTestOut(0x21, 0x08);
    PicTestOut(0x21, 0x04);
    PicTestOut(0x21, 0x13);  /* AEOI+SFNM */
    PicTestCheck(pic.Master.IsAutoEoi && pic.Master.IsSpecialFullyNested, "icw4: 13h sets AEOI and SFNM");
    PicTestOut(0x20, 0x10);
    PicTestOut(0x21, 0x08);
    PicTestOut(0x21, 0x04);                    /* no ICW4 */
    PicTestCheck(!pic.Master.IsAutoEoi && !pic.Master.IsSpecialFullyNested && pic.Master.IcwStep == 0,
       "icw1: IC4=0 zeroes the ICW4 functions (AEOI, SFNM) and ends after ICW3");

    /* FULLY NESTED MODE ACROSS THE CASCADE -- THE ONE DEFAULT THAT CHANGED:
     * With SFNM off (what a BIOS programs), a slave line puts IR2 in service on the
     * master, and nothing more from the slave gets in -- not even a HIGHER slave
     * line -- until the master is EOI'd. We used to let it through.
     */
    VddPicReset(&pic);
    pic.Master.Imr = 0x00;
    pic.Slave.Imr = 0x00;
    VddPicAcknowledge(&pic, 12);            /* the PS/2 mouse, slave IR4 */
    PicTestCheck((pic.Master.Isr & 0x04) && (pic.Slave.Isr & 0x10), "cascade: IRQ12 puts slave IR4 and master IR2 in service");
    PicTestCheck(!VddPicCanDeliver(&pic, 8), "fnm: a HIGHER slave line (IRQ8) is held while IR2 is in service");
    PicTestCheck(!VddPicCanDeliver(&pic, 13), "fnm: ...and a lower one (IRQ13)");
    PicTestCheck(VddPicCanDeliver(&pic, 0) && VddPicCanDeliver(&pic, 1), "fnm: IRQ0/IRQ1 still outrank it");
    PicTestCheck(!VddPicCanDeliver(&pic, 3), "fnm: IRQ3-7 are blocked by IR2, as before");
    PicTestOut(0xA0, 0x20);                       /* the handler's slave EOI ... */
    PicTestCheck(!VddPicCanDeliver(&pic, 8), "fnm: a slave EOI alone does not reopen the slave");
    PicTestOut(0x20, 0x20);                       /* ... and its master EOI */
    PicTestCheck(VddPicCanDeliver(&pic, 8) && VddPicCanDeliver(&pic, 3),
       "fnm: the master EOI releases IR2 -- slave and IRQ3-7 open again");
    VddPicAcknowledge(&pic, 12);
    VddPicEndOfInterrupt(&pic, 12);                    /* the host's own EOI (our stubs) */
    PicTestCheck(pic.Master.Isr == 0 && VddPicCanDeliver(&pic, 8),
       "fnm: the host's slave EOI releases IR2 with the last slave bit");
    VddPicAcknowledgeAutoEoi(&pic, 10);
    PicTestCheck(pic.Master.Isr == 0 && pic.Slave.Isr == 0 && VddPicCanDeliver(&pic, 11),
       "fnm: a host auto-EOI'd slave line leaves nothing in service");

    /* SPECIAL FULLY NESTED MODE, asked for by ICW4 bit 4 on the master. */
    VddPicReset(&pic);
    PicTestOut(0x20, 0x11);
    PicTestOut(0x21, 0x08);
    PicTestOut(0x21, 0x04);
    PicTestOut(0x21, 0x11);  /* SFNM */
    PicTestOut(0xA0, 0x11);
    PicTestOut(0xA1, 0x70);
    PicTestOut(0xA1, 0x02);
    PicTestOut(0xA1, 0x01);
    PicTestOut(0x21, 0x00);
    PicTestOut(0xA1, 0x00);
    PicTestCheck(pic.Master.IsSpecialFullyNested && !pic.Slave.IsSpecialFullyNested, "sfnm: ICW4=11h sets SFNM on the master");
    VddPicAcknowledge(&pic, 12);
    PicTestCheck(VddPicCanDeliver(&pic, 8), "sfnm: a HIGHER slave line gets in while IR2 is in service");
    PicTestCheck(!VddPicCanDeliver(&pic, 12) && !VddPicCanDeliver(&pic, 13),
       "sfnm: the slave's own nesting still holds IRQ12 and below");
    PicTestCheck(!VddPicCanDeliver(&pic, 3), "sfnm: IR2 in service still blocks master IRQ3-7");
    PicTestCheck(VddPicCanDeliver(&pic, 1), "sfnm: ...and IRQ0/IRQ1 still outrank it");

    printf("-- %d checks, %d failures --\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
