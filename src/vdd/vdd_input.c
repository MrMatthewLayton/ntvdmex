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
void vdd_input_push_scancode(input_state *st, uint8_t sc)
{
    int n = next(st->sc_head);
    if (n == st->sc_tail) st->sc_tail = next(st->sc_tail);   /* full -> drop oldest */
    st->sc_buf[st->sc_head] = sc;
    st->sc_head = n;
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
    if (st->sc_head != st->sc_tail) {
        st->sc_last = st->sc_buf[st->sc_tail];
        st->sc_tail = next(st->sc_tail);
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
