/* vdd_pic.c -- see vdd_pic.h.  8259A pair on the VDD bus.  Pure C. */
#include "vdd_pic.h"

/* ── PRIORITY IS A RING, NOT A BIT NUMBER. (#174) ─────────────────────────────────────
     The 8259A's priorities rotate: OCW2 can name any IR line the LOWEST, and the one
     after it (mod 8) becomes the highest. `prio_low` = 7 is the fixed order every PC
     BIOS leaves behind (IR0 highest, IR7 lowest), and in that state pic_rank(c, n) == n,
     so every rule below reduces EXACTLY to the lowest-bit-first test this file used
     before rotation existed. A guest that never writes C0h/A0h/E0h/80h sees no change.
   ► rank 0 = highest priority. */
static int pic_rank(const pic_chip *c, int line)
{
    return (line - (int)c->prio_low - 1) & 7;
}

/* The highest-priority line set in `mask`, in the chip's CURRENT rotation; -1 if none. */
static int pic_top(const pic_chip *c, uint8_t mask)
{
    int r;
    for (r = 0; r < 8; ++r) {
        int line = ((int)c->prio_low + 1 + r) & 7;
        if (mask & (1u << line)) return line;
    }
    return -1;
}

/* The in-service bits that stop `line` being delivered on this chip.
   ► FULLY NESTED: every in-service line at the same or higher priority.
   ► SPECIAL MASK MODE (OCW3 ESMM/SMM): a MASKED line's in-service bit stops counting.
     The datasheet: "when a mask bit is set in OCW1, it inhibits further interrupts at
     that level and enables interrupts from all other levels (lower as well as higher)
     that are not masked." It is how a handler masks its own line and lets everything
     else -- including LOWER priorities -- in before it EOIs. An UNMASKED in-service
     line still blocks as usual (QEMU, MAME and Bochs all model it this way: the
     resolver sees ISR & ~IMR).
   ► SPECIAL FULLY NESTED MODE (ICW4 SFNM, master only): IR2 in service does not block
     IR2 itself, so a HIGHER-priority slave line can interrupt a lower one's handler --
     the slave's own resolver decides that. IR2 in service still blocks IR3-7: SFNM
     relaxes the equal-priority rule for the cascade input and nothing else. */
static uint8_t pic_blockers(const pic_chip *c, int line, int is_master)
{
    uint8_t eff = c->isr, out = 0;
    int i, rl = pic_rank(c, line);
    if (c->smm) eff = (uint8_t)(eff & ~c->imr);
    for (i = 0; i < 8; ++i) {
        if (!(eff & (1u << i))) continue;
        if (is_master && c->sfnm && line == 2 && i == 2) continue;
        if (pic_rank(c, i) <= rl) out = (uint8_t)(out | (1u << i));
    }
    return out;
}

/* May this chip pass `line` on right now? Masked, or outranked by something in service,
   is a no. The one resolver every path uses -- delivery, the cascade, and the poll. */
static int pic_line_open(const pic_chip *c, int line, int is_master)
{
    if (c->imr & (1u << line)) return 0;
    return pic_blockers(c, line, is_master) == 0;
}

static void pic_chip_reset(pic_chip *c, uint8_t base)
{
    c->imr = 0xFF;              /* everything masked until the guest unmasks     */
    c->irr = c->isr = 0;
    c->base = base;
    c->icw_step = 0; c->icw4_needed = 0; c->read_isr = 0; c->auto_eoi = 0;
    c->poll_armed = 0;
    c->prio_low = 7; c->rotate_aeoi = 0; c->smm = 0; c->sfnm = 0;   /* a BIOS's chip */
}

/* ── IN-SERVICE UPDATES ARE ATOMIC. ───────────────────────────────────────────────
     The host acknowledges IRQ0 from its tick courier, which by design runs with the
     guest thread frozen and NO device lock held, while the guest's own EOIs (and every
     other line's acknowledge) arrive under that lock. A plain `isr |= bit` / `isr &=
     ~bit` is a byte read-modify-write, and two of them racing lose one bit -- IRQ1's
     in-service bit IS the keyboard re-entrancy guard, so losing it is "press a key and
     everything hangs" all over again. The host already OR's IRR atomically for the
     same reason (host_irq_sink); ISR gets the same treatment here so that IRQ0 can be
     held in service across threads. GCC/Clang builtins, so this file stays pure C. */
#define ISR_SET(c, bit)  ((void)__sync_fetch_and_or (&(c)->isr, (uint8_t)(bit)))
#define ISR_CLR(c, bit)  ((void)__sync_fetch_and_and(&(c)->isr, (uint8_t)~(uint8_t)(bit)))
/* IRR likewise: the host raises with an atomic OR from the PIT's lock, so the clear
   at acknowledge must be atomic too or a raise can be lost under it. */
#define IRR_CLR(c, bit)  ((void)__sync_fetch_and_and(&(c)->irr, (uint8_t)~(uint8_t)(bit)))

/* The in-service half of an acknowledge cycle, on one chip: ISR set -- or, in AEOI,
   the automatic EOI, which in ROTATE-IN-AEOI mode also makes this line the lowest
   priority (it is a "rotate on non-specific EOI" with the EOI done for you).
   ⚠ prio_low is a plain byte store. It only moves when a guest has programmed AEOI +
     rotate, which no PC BIOS does; a byte store is not a read-modify-write, so it
     cannot lose a concurrent ISR/IRR update the way `isr |= bit` could. */
static void pic_intack(pic_chip *c, int line)
{
    if (!c->auto_eoi)          ISR_SET(c, 1u << line);
    else if (c->rotate_aeoi)   c->prio_low = (uint8_t)line;
}

/* --- port side ------------------------------------------------------------- */

/* base port (0x20 / 0xA0): ICW1, OCW2 (EOI), OCW3 (read select) */
static void pic_cmd_write(pic_chip *c, uint8_t v)
{
    if (v & 0x10) {                         /* ICW1: begin initialisation        */
        c->icw_step = 1;
        c->icw4_needed = (uint8_t)(v & 0x01);
        c->isr = c->irr = 0;
        c->imr = 0;                         /* ICW1 clears the mask register     */
        /* ── THE REST OF ICW1's SIDE EFFECTS (#174), from the datasheet's list:
             IR7 is assigned the lowest priority, Special Mask Mode is cleared, and
             the status read is set to the IRR. The rotate-in-AEOI flag is not on
             Intel's list; it is cleared here too, as QEMU's init reset does, on the
             grounds that a re-initialised chip is a fresh one.
           ⚠ THE READ-SELECT RESET IS SPEC-DERIVED AND UNVERIFIED BY ORACLE. p_pic's
             pic.icw1.readsel only discriminates on PCem, and PCem does NOT reset it
             (AH=00, AL=01). QEMU and dosbox-x clear the IRR at ICW1, so their reads
             cannot say which register came back. One emulator that disagrees with the
             datasheet is not a second oracle; the datasheet wins until real silicon
             says otherwise. docs/inventory/pic.md 3. */
        c->read_isr = 0;
        c->prio_low = 7; c->smm = 0; c->rotate_aeoi = 0;
        /* "If IC4 = 0, then all functions selected in ICW4 are set to zero" -- with
           no ICW4 coming, AEOI and SFNM must not survive from the last init. */
        if (!c->icw4_needed) { c->auto_eoi = 0; c->sfnm = 0; }
        return;
    }
    if (v & 0x08) {                         /* OCW3                              */
        if (v & 0x02) c->read_isr = (uint8_t)(v & 0x01);   /* 0=IRR, 1=ISR       */
        /* Special Mask Mode: ESMM (bit 6) enables the SMM bit (bit 5) to mean
           anything; ESMM clear leaves the mode alone. 11 = set, 10 = clear. */
        if (v & 0x40) c->smm = (uint8_t)((v & 0x20) ? 1 : 0);
        /* ── ★★★ THE POLL COMMAND, AND WHY DROPPING IT WAS THE "RUNS BUT LIES"
             SHAPE. A poll read and a status read are THE SAME `IN` ON THE SAME
             PORT; the only thing that tells them apart is which OCW3 was written
             last. So a guest that polls never got an error -- it got whatever
             register the read select happened to name, and read it as
             "bit 7 = an interrupt is pending, bits 2:0 = its level". An IRR of
             0x01 (IRQ0 requested) reads as NO INTERRUPT because bit 7 is clear;
             an IRR of 0x80 (IRQ7) reads as "pending, level 0". Plausible, wrong,
             silent.
           ⚠ ORACLES: MS-DOS 6.22 under QEMU implements it (p_pic pic.ocw3.poll =
             0x00); dosbox-x and PCem both drop the P bit and answer 0x01, the
             selected ISR. Two of three do not model the feature, so their answer
             is the absence of a measurement rather than a measurement of absence
             -- the same footing as the 8254's BCD bit, except that here the one
             host which DOES implement it agrees with the datasheet. */
        if (v & 0x04) c->poll_armed = 1;
        return;
    }
    /* OCW2: the EOI family. */
    /* ⚠ NON-SPECIFIC EOI CLEARS THE HIGHEST-PRIORITY IN-SERVICE BIT IN THE CURRENT
         ROTATION, against the full ISR -- Special Mask Mode does not hide masked
         bits from it. The datasheet says a guest that disturbs the nesting (SMM,
         rotation mid-handler) must use a SPECIFIC EOI; this is what the chip does
         when it does not. */
    switch (v & 0xE0) {
    case 0x20: {                            /* non-specific EOI                  */
        int top = pic_top(c, c->isr);
        if (top >= 0) ISR_CLR(c, 1u << top);
        break; }
    case 0x60:                              /* specific EOI                      */
        ISR_CLR(c, 1u << (v & 7));
        break;
    /* Rotate on non-specific EOI: the line it ends becomes the LOWEST priority. With
       nothing in service there is no line to name, so nothing rotates (QEMU agrees). */
    case 0xA0: {
        int top = pic_top(c, c->isr);
        if (top >= 0) { ISR_CLR(c, 1u << top); c->prio_low = (uint8_t)top; }
        break; }
    /* ── ROTATE ON SPECIFIC EOI. IT IS STILL AN EOI, AND THIS USED TO BE A NOP. ──
         E0h+L2:L0 ends the interrupt on L AND makes L the lowest priority. Until #174
         only the EOI half was modelled; THE EOI HALF IS THE ONE THAT IS NOT OPTIONAL:
         an in-service bit that is never cleared does not cost one interrupt, it kills
         that priority level and everything below it for the rest of the run.
       ★ ALL THREE ORACLES AGREE the bit goes (p_pic pic.ocw2.rot.speoi = 0 on
         6.22, dosbox-x and PCem alike). Unanimous, so no judgement was needed.
         The rotation half is datasheet only -- no probe asks it yet. */
    case 0xE0:
        ISR_CLR(c, 1u << (v & 7));
        c->prio_low = (uint8_t)(v & 7);
        break;
    /* Set priority: L becomes the lowest, L+1 the highest. NOT an EOI. */
    case 0xC0:
        c->prio_low = (uint8_t)(v & 7);
        break;
    /* Rotate in auto-EOI mode, set (80h) / clear (00h). Takes effect at the next
       acknowledge, and only while ICW4 selected AEOI (pic_intack). NOT an EOI. */
    case 0x80: c->rotate_aeoi = 1; break;
    case 0x00: c->rotate_aeoi = 0; break;
    default:
        /* 40h is the datasheet's "no operation". None of C0h/80h/00h/40h ends an
           interrupt -- treating the whole 0xE0 field as "some kind of EOI" would
           break the re-entrancy guard in the other direction. */
        break;
    }
}

/* data port (0x21 / 0xA1): ICW2/3/4 during init, otherwise OCW1 = the mask */
static void pic_data_write(pic_chip *c, uint8_t v)
{
    switch (c->icw_step) {
    case 1: c->base = v; c->icw_step = 2; return;          /* ICW2: vector base  */
    case 2: c->icw_step = c->icw4_needed ? 3 : 0; return;  /* ICW3: cascade map  */
    case 3: c->auto_eoi = (uint8_t)((v & 0x02) ? 1 : 0);   /* ICW4               */
            /* SFNM, bit 4. A PC BIOS writes 01h -- off -- so the default stays the
               restrictive fully nested mode (pic_blockers). µPM and BUF have no
               effect a host without a bus cycle can show. */
            c->sfnm = (uint8_t)((v & 0x10) ? 1 : 0);
            c->icw_step = 0; return;
    default: c->imr = v; return;                           /* OCW1: mask         */
    }
}

static void pic_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    pic_state *st = (pic_state *)self;
    uint8_t val = (uint8_t)v; (void)w;
    switch (port) {
    case 0x20: pic_cmd_write(&st->m, val);  break;
    case 0x21: pic_data_write(&st->m, val); break;
    case 0xA0: pic_cmd_write(&st->s, val);  break;
    case 0xA1: pic_data_write(&st->s, val); break;
    default: break;
    }
}

/* A poll read: report the highest-priority PENDING line and acknowledge it.
   ⚠ PENDING, NOT IN SERVICE. Bit 7 answers "is there an interrupt to take", and a
     line already in service is not one -- which is what makes the idle-with-IRQ0-
     in-service case (p_pic) discriminating at all. Requests that are masked, or
     outranked by something already in service, are not available either: the poll
     goes through the same resolver a delivery would -- rotation and SMM included.
   ► Only the highest-priority pending line is a candidate: the chip offers that one
     or nothing. Nothing is lost by not looking further -- whatever in-service bit
     blocks it outranks every pending line below it too, in SMM as well. */
static uint8_t pic_poll_read(pic_chip *c, int is_master)
{
    uint8_t ready = (uint8_t)(c->irr & ~c->imr);
    int top = pic_top(c, ready);
    if (top < 0 || !pic_line_open(c, top, is_master))
        return 0x00;                             /* bit 7 clear: none takeable  */
    IRR_CLR(c, 1u << top);
    pic_intack(c, top);
    return (uint8_t)(0x80 | top);
}

static void pic_in(void *self, uint16_t port, uint8_t w, uint32_t *val)
{
    pic_state *st = (pic_state *)self;
    pic_chip *c = (port < 0xA0) ? &st->m : &st->s;
    (void)w;
    if (port == 0x21 || port == 0xA1) { *val = c->imr; return; }
    /* A POLL IS A ONE-SHOT: OCW3's P bit arms the NEXT read only, and this read
       consumes it. Leaving it armed would mean a guest that polls once never sees
       a status byte again. */
    if (c->poll_armed) { c->poll_armed = 0; *val = pic_poll_read(c, c == &st->m); return; }
    *val = c->read_isr ? c->isr : c->irr;
}

/* --- host side ------------------------------------------------------------- */

void vdd_pic_raise(pic_state *st, uint8_t irq)
{
    if (irq < 8)       st->m.irr |= (uint8_t)(1u << irq);
    else if (irq < 16) st->s.irr |= (uint8_t)(1u << (irq - 8));
}

int vdd_pic_can_deliver(pic_state *st, uint8_t irq)
{
    pic_chip *c;
    if (irq >= 16) return 0;
    c   = (irq < 8) ? &st->m : &st->s;
    /* Masked, or in service at this or higher priority, blocks delivery -- this is
       what stops a handler being re-entered before it EOIs. */
    if (!pic_line_open(c, irq & 7, irq < 8)) return 0;
    /* ── A SLAVE LINE MUST ALSO GET THROUGH THE MASTER'S IR2, AND IN FULLY NESTED
         MODE IR2 IN SERVICE IS A "NO". (#174) ──────────────────────────────────────
         This used to test the master's mask and its bits 0:1 only, so a second slave
         interrupt got in while IR2 was still in service -- Special Fully Nested Mode,
         implemented without anyone asking for it. A PC BIOS programs SFNM OFF, so on
         an AT the slave is locked out from the moment one of its lines is delivered
         until the handler's master EOI (docs/ref/pic.md 5). ICW4 bit 4 now asks for
         the permissive behaviour explicitly.
       ► WHY THE HOST CAN AFFORD IT: IR2 in service already blocked IRQ3-7, so any path
         that failed to release it was already killing five master lines; every host
         path that sets it releases it (vdd_pic_eoi clears IR2 once the slave's ISR is
         empty -- our stubs, the PM reflection, a failed PM inject -- and a guest
         handler's own `out 20h` does the rest). */
    if (irq >= 8 && !pic_line_open(&st->m, 2, 1)) return 0;
    return 1;
}

void vdd_pic_acknowledge(pic_state *st, uint8_t irq)
{
    pic_chip *c;
    uint8_t bit;
    if (irq >= 16) return;
    c   = (irq < 8) ? &st->m : &st->s;
    bit = (uint8_t)(1u << (irq & 7));
    IRR_CLR(c, bit);
    pic_intack(c, irq & 7);
    if (irq >= 8) pic_intack(&st->m, 2);        /* the cascade: IR2 in service   */
}

/* Acknowledge a line the HOST auto-EOIs -- IRQ0, and any line still vectored at one of
   our own do-nothing stubs. Net effect is identical to acknowledge() followed by eoi():
   the request bit is cleared and ISR ends exactly where it started.
   ► WHY IT IS A FUNCTION RATHER THAN THE PAIR. The pair SETS the in-service bit and
     then clears it, and that transient is a read-modify-write on a byte shared with
     every other line. A caller that does not hold the device lock -- the tick courier
     in the host, which by construction runs while the guest thread is frozen and no
     lock is held -- could interleave with a concurrent acknowledge() for a DIFFERENT
     line and lose ITS in-service bit. IRQ1's in-service bit is the keyboard
     re-entrancy guard, so losing it is the "press a key and everything hangs" fault.
     Doing the net operation directly touches only IRR, which no delivery decision
     reads (can_deliver consults IMR and ISR), so it is safe from any thread. */
void vdd_pic_ack_autoeoi(pic_state *st, uint8_t irq)
{
    if (irq >= 8) { vdd_pic_acknowledge(st, irq); vdd_pic_eoi(st, irq); return; }
    IRR_CLR(&st->m, 1u << irq);
    /* The acknowledge half of a chip in rotate-in-AEOI mode moves the priority; the
       host's specific EOI that follows does not. A single byte store, and never taken
       unless a guest programmed AEOI + rotate. */
    if (st->m.auto_eoi && st->m.rotate_aeoi) st->m.prio_low = irq;
}

void vdd_pic_eoi(pic_state *st, uint8_t irq)
{
    if (irq >= 16) return;
    if (irq < 8) ISR_CLR(&st->m, 1u << irq);
    else       { ISR_CLR(&st->s, 1u << (irq - 8));
                 if (!st->s.isr) ISR_CLR(&st->m, 0x04); }   /* release the cascade */
}

uint8_t vdd_pic_vector(pic_state *st, uint8_t irq)
{
    if (irq < 8)  return (uint8_t)(st->m.base + irq);
    if (irq < 16) return (uint8_t)(st->s.base + (irq - 8));
    return 0;
}

void vdd_pic_reset(void *self)
{
    pic_state *st = (pic_state *)self;
    vdd_bus *b = st->bus;
    pic_chip_reset(&st->m, 0x08);
    pic_chip_reset(&st->s, 0x70);
    /* A PC's BIOS leaves the timer and keyboard unmasked before handing control to
       the program, and a DOS game inherits that; masking everything here would mean a
       game that never touches the PIC (many do not) got no interrupts at all. */
    st->m.imr = 0xFC;                            /* IRQ0 + IRQ1 enabled          */
    st->s.imr = 0xFF;
    st->bus = b;
}

int vdd_pic_init(vdd_bus *b, void *self)
{
    pic_state *st = (pic_state *)self;
    st->bus = b;
    vdd_pic_reset(st);
    st->bus = b;
    if (vdd_claim_ports(b, 0x20, 0x21, pic_in, pic_out, st)) return -1;
    if (vdd_claim_ports(b, 0xA0, 0xA1, pic_in, pic_out, st)) return -1;
    return 0;
}
