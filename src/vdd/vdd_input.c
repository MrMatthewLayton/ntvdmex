/* vdd_input.c -- see vdd_input.h.  Keyboard ring buffer + INT 16h servicer, on
 * the VDD bus.  Pure C, no <windows.h>; non-blocking (reports empty via ZF). */
#include "vdd_input.h"

static int next(int i) { return (i + 1) % VDD_KBD_SIZE; }

void vdd_input_push(input_state *st, uint16_t key)
{
    int n = next(st->head);
    if (n == st->tail) st->tail = next(st->tail);  /* full -> drop oldest        */
    st->buf[st->head] = key;
    st->head = n;
}

int vdd_input_pop(input_state *st, uint16_t *key)
{
    if (st->head == st->tail) return 0;
    *key = st->buf[st->tail];
    st->tail = next(st->tail);
    return 1;
}

int vdd_input_peek(input_state *st, uint16_t *key)
{
    if (st->head == st->tail) return 0;
    *key = st->buf[st->tail];
    return 1;
}

/* --- raw AT keyboard: scancode FIFO + ports 0x60/0x64 --------------------- */
/* Push a scancode and assert IRQ1 the way an 8042 does: the controller holds ONE byte in
   its output buffer and raises the line on the empty->full transition; the next byte is not
   presented (and no further interrupt occurs) until the guest reads port 0x60. Pacing the
   interrupt off the guest's own reads is what keeps scancodes and interrupts in step.
   Getting this wrong is very visible in a game: with one latched interrupt per byte and no
   pacing the backlog outran delivery, and with a cap on that latch the surplus bytes lost
   their interrupts altogether -- so E0-prefixed keys (every arrow) never arrived, and a
   key's BREAK code could be stranded in the FIFO, leaving the game convinced it was still
   held. That is exactly "arrows do nothing, and space sticks on after you let go". */
void vdd_input_push_scancode(input_state *st, uint8_t sc)
{
    int was_empty = (st->sc_head == st->sc_tail);
    int n = next(st->sc_head);
    if (n == st->sc_tail) st->sc_tail = next(st->sc_tail);   /* full -> drop oldest */
    st->sc_buf[st->sc_head] = sc;
    st->sc_head = n;
    if (was_empty && st->bus) vdd_raise_irq(st->bus, 1);     /* empty -> full        */
}

void vdd_input_bios_consume(input_state *st)
{
    if (st->sc_head == st->sc_tail) return;
    st->sc_last = st->sc_buf[st->sc_tail];
    st->sc_tail = next(st->sc_tail);
    if (st->sc_head != st->sc_tail && st->bus) vdd_raise_irq(st->bus, 1);
}

int vdd_input_sc_pending(const input_state *st)
{
    return st->sc_head != st->sc_tail;
}

/* IN 0x60 = keyboard data (pop one scancode; re-reads see the last byte).
   IN 0x64 = 8042 status: bit0 (OBF) set while a scancode waits. Writes to
   either (LED/8042 commands) are accepted and ignored. */
static void kbd_hw_in(void *self, uint16_t port, uint8_t w, uint32_t *val)
{
    input_state *st = (input_state *)self;
    (void)w;
    if (port == 0x64) {                       /* status register                    */
        *val = vdd_input_sc_pending(st) ? 0x01 : 0x00;
        return;
    }
    /* port 0x60: data register */
    st->p60_reads++;
    if (st->sc_head != st->sc_tail) {
        st->sc_last = st->sc_buf[st->sc_tail];
        st->sc_tail = next(st->sc_tail);
        /* Still more queued? The controller presents the next byte and re-asserts the
           line, so the guest gets exactly one interrupt per scancode, at its own pace. */
        if (st->sc_head != st->sc_tail && st->bus) vdd_raise_irq(st->bus, 1);
    }
    *val = st->sc_last;
}

static void kbd_hw_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    (void)self; (void)port; (void)w; (void)v;  /* accept 8042/LED commands, ignore */
}

/* INT 16h -- BIOS keyboard. ZF semantics: AH=01 sets ZF=1 when no key is ready.
   AH=00 here is non-blocking (the host loops + waits on a key event, re-issuing
   until ZF=0); it sets ZF=1 + leaves AX when the buffer is empty. */
static void int16(void *self, ntvdd_regs *r)
{
    input_state *st = (input_state *)self;
    uint16_t key;
    switch (r_ah(r)) {
    case 0x00: case 0x10: st->int16_calls[0]++; break;
    case 0x01: case 0x11: st->int16_calls[1]++; break;
    case 0x02:            st->int16_calls[2]++; break;
    default:              st->int16_calls[3]++; break;
    }
    switch (r_ah(r)) {
    case 0x00:                              /* read key (host blocks on empty)    */
    case 0x10:                              /* read key, enhanced (101-key)        */
        if (vdd_input_pop(st, &key)) { s_ax(r, key); r->zf = 0; }
        else r->zf = 1;
        break;
    case 0x01:                              /* check key (non-blocking)           */
    case 0x11:                              /* check key, enhanced (101-key)       */
        if (vdd_input_peek(st, &key)) { s_ax(r, key); r->zf = 0; }
        else r->zf = 1;                     /* ZF=1 => no key (QB's INKEY$ -> "") */
        break;
    case 0x02:                              /* shift status -> none for now        */
    case 0x12:                              /* extended shift status               */
        s_al(r, 0); r->zf = 0;
        break;
    default:                                /* unknown fn: report "no key", never  */
        r->zf = 1;                          /* a phantom keystroke (was a bug)     */
        break;
    }
}

void vdd_input_reset(void *self)
{
    input_state *st = (input_state *)self;
    st->head = st->tail = 0;
    st->sc_head = st->sc_tail = 0;
    st->sc_last = 0;
}

int vdd_input_init(vdd_bus *b, void *self)
{
    input_state *st = (input_state *)self;
    st->bus = b;
    vdd_input_reset(st);
    if (vdd_claim_int(b, 0x16, int16, st)) return -1;
    if (vdd_claim_ports(b, 0x60, 0x60, kbd_hw_in, kbd_hw_out, st)) return -1;  /* data   */
    if (vdd_claim_ports(b, 0x64, 0x64, kbd_hw_in, kbd_hw_out, st)) return -1;  /* status */
    return 0;
}
