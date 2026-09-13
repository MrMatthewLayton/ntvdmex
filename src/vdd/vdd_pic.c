/* vdd_pic.c -- see vdd_pic.h.  8259A pair on the VDD bus.  Pure C. */
#include "vdd_pic.h"

/* Lowest set bit = highest priority, which is the fixed-priority order every DOS-era
   PC uses (IRQ0 highest). Returns -1 if none. */
static int pic_top(uint8_t mask)
{
    int i;
    for (i = 0; i < 8; ++i) if (mask & (1u << i)) return i;
    return -1;
}

static void pic_chip_reset(pic_chip *c, uint8_t base)
{
    c->imr = 0xFF;              /* everything masked until the guest unmasks     */
    c->irr = c->isr = 0;
    c->base = base;
    c->icw_step = 0; c->icw4_needed = 0; c->read_isr = 0; c->auto_eoi = 0;
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

/* --- port side ------------------------------------------------------------- */

/* base port (0x20 / 0xA0): ICW1, OCW2 (EOI), OCW3 (read select) */
static void pic_cmd_write(pic_chip *c, uint8_t v)
{
    if (v & 0x10) {                         /* ICW1: begin initialisation        */
        c->icw_step = 1;
        c->icw4_needed = (uint8_t)(v & 0x01);
        c->isr = c->irr = 0;
        c->imr = 0;                         /* ICW1 clears the mask register     */
        return;
    }
    if (v & 0x08) {                         /* OCW3                              */
        if (v & 0x02) c->read_isr = (uint8_t)(v & 0x01);   /* 0=IRR, 1=ISR       */
        return;
    }
    /* OCW2: the EOI family. */
    switch (v & 0xE0) {
    case 0x20: {                            /* non-specific EOI                  */
        int top = pic_top(c->isr);
        if (top >= 0) ISR_CLR(c, 1u << top);
        break; }
    case 0x60:                              /* specific EOI                      */
        ISR_CLR(c, 1u << (v & 7));
        break;
    case 0xA0: {                            /* rotate on non-specific EOI        */
        int top = pic_top(c->isr);
        if (top >= 0) ISR_CLR(c, 1u << top);
        break; }
    default:
        break;                              /* other rotate/priority forms: nop  */
    }
}

/* data port (0x21 / 0xA1): ICW2/3/4 during init, otherwise OCW1 = the mask */
static void pic_data_write(pic_chip *c, uint8_t v)
{
    switch (c->icw_step) {
    case 1: c->base = v; c->icw_step = 2; return;          /* ICW2: vector base  */
    case 2: c->icw_step = c->icw4_needed ? 3 : 0; return;  /* ICW3: cascade map  */
    case 3: c->auto_eoi = (uint8_t)((v & 0x02) ? 1 : 0);   /* ICW4               */
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

static void pic_in(void *self, uint16_t port, uint8_t w, uint32_t *val)
{
    pic_state *st = (pic_state *)self;
    pic_chip *c = (port < 0xA0) ? &st->m : &st->s;
    (void)w;
    if (port == 0x21 || port == 0xA1) *val = c->imr;
    else                              *val = c->read_isr ? c->isr : c->irr;
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
    uint8_t bit;
    if (irq >= 16) return 0;
    c   = (irq < 8) ? &st->m : &st->s;
    bit = (uint8_t)(1u << (irq & 7));
    if (c->imr & bit) return 0;                 /* masked                        */
    /* In service at this or higher priority (lower bit number) blocks delivery --
       this is what stops a handler being re-entered before it EOIs. */
    if (c->isr & ((bit << 1) - 1)) return 0;
    /* A slave line is also gated by the master's cascade line (IRQ2). */
    if (irq >= 8) {
        if (st->m.imr & 0x04) return 0;
        if (st->m.isr & 0x03) return 0;         /* IRQ0/1 outrank the whole slave */
    }
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
    if (!c->auto_eoi) ISR_SET(c, bit);
    if (irq >= 8 && !st->m.auto_eoi) ISR_SET(&st->m, 0x04);   /* cascade in service */
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
