/* vdd_input.c -- see vdd_input.h.  Keyboard ring buffer + INT 16h servicer, on
 * the VDD bus.  Pure C, no <windows.h>; non-blocking (reports empty via ZF). */
#include "vdd_input.h"

static int next(int i) { return (i + 1) % VDD_KBD_SIZE; }

/* --- the BIOS keyboard ring, in guest memory at 0040:001E ------------------ */

static uint16_t bda_r16(const input_state *st, int off)
{ return (uint16_t)(st->bda[off] | (st->bda[off + 1] << 8)); }

static void bda_w16(input_state *st, int off, uint16_t v)
{ st->bda[off] = (uint8_t)v; st->bda[off + 1] = (uint8_t)(v >> 8); }

/* Advance a ring pointer, wrapping at the end of the 16-entry buffer. */
static uint16_t bda_next(uint16_t p)
{ p += 2; return (p >= BDA_KB_END) ? (uint16_t)BDA_KB_START : p; }

void vdd_input_push(input_state *st, uint16_t key)
{
    uint16_t head, tail, n;
    if (!st->bda) return;                 /* no guest memory yet: nowhere to put it */
    head = bda_r16(st, BDA_KB_HEAD);
    tail = bda_r16(st, BDA_KB_TAIL);
    /* A pointer pair the guest has not initialised (or has scribbled on) would send the
       writes anywhere in the BDA, so validate before trusting them. */
    if (head < BDA_KB_START || head >= BDA_KB_END || (head & 1) ||
        tail < BDA_KB_START || tail >= BDA_KB_END || (tail & 1)) {
        head = tail = BDA_KB_START;
        bda_w16(st, BDA_KB_HEAD, head);
    }
    n = bda_next(tail);
    if (n == head) return;                /* full -> discard the NEW key: the real BIOS
                                             beeps and throws it away. Dropping the OLDEST
                                             instead would split a keystroke stream. */
    bda_w16(st, tail, key);
    bda_w16(st, BDA_KB_TAIL, n);
}

int vdd_input_pop(input_state *st, uint16_t *key)
{
    uint16_t head, tail;
    if (!st->bda) return 0;
    head = bda_r16(st, BDA_KB_HEAD);
    tail = bda_r16(st, BDA_KB_TAIL);
    if (head == tail) return 0;
    if (head < BDA_KB_START || head >= BDA_KB_END || (head & 1)) return 0;
    *key = bda_r16(st, head);
    bda_w16(st, BDA_KB_HEAD, bda_next(head));
    return 1;
}

int vdd_input_peek(input_state *st, uint16_t *key)
{
    uint16_t head, tail;
    if (!st->bda) return 0;
    head = bda_r16(st, BDA_KB_HEAD);
    tail = bda_r16(st, BDA_KB_TAIL);
    if (head == tail) return 0;
    if (head < BDA_KB_START || head >= BDA_KB_END || (head & 1)) return 0;
    *key = bda_r16(st, head);
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
    st->sc_pushed++;
    if (n == st->sc_tail) { st->sc_dropped++; st->sc_tail = next(st->sc_tail); }  /* full -> drop oldest */
    st->sc_buf[st->sc_head] = sc;
    st->sc_head = n;
    if (was_empty && st->bus) vdd_raise_irq(st->bus, 1);     /* empty -> full        */
}

/* --- scancode set 1 -> ASCII (US layout) ----------------------------------- */
/* Indexed by make code, 0x00..0x53. 0 means "no ASCII": either a modifier, or one of the
   keys whose whole identity is its scancode (F-keys, arrows), which the BIOS reports as
   AH=scancode with AL=0. That AL=0 is the convention a DOS program tests to tell an arrow
   from a character, so it matters that these are 0 and not something plausible-looking. */
static const uint8_t sc_ascii[] = {
    0,    0x1B, '1',  '2',  '3',  '4',  '5',  '6',   /* 00-07 */
    '7',  '8',  '9',  '0',  '-',  '=',  0x08, 0x09,  /* 08-0F */
    'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',   /* 10-17 */
    'o',  'p',  '[',  ']',  0x0D, 0,    'a',  's',   /* 18-1F */
    'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',   /* 20-27 */
    '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',   /* 28-2F */
    'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',   /* 30-37 */
    0,    ' ',  0,    0,    0,    0,    0,    0,     /* 38-3F */
    0,    0,    0,    0,    0,    0,    0,    '7',   /* 40-47 */
    '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',   /* 48-4F */
    '2',  '3',  '0',  '.'                            /* 50-53 */
};
static const uint8_t sc_ascii_shift[] = {
    0,    0x1B, '!',  '@',  '#',  '$',  '%',  '^',
    '&',  '*',  '(',  ')',  '_',  '+',  0x08, 0,     /* shift-TAB: AL=0, AH=0F */
    'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',
    'O',  'P',  '{',  '}',  0x0D, 0,    'A',  'S',
    'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',
    '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',
    'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',
    0,    ' ',  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    '7',
    '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',
    '2',  '3',  '0',  '.'
};
#define SC_TABLE_MAX ((uint8_t)(sizeof sc_ascii - 1))

/* Shift-state bits in 0040:0017, as every DOS program expects to find them. */
#define KF_RSHIFT 0x01
#define KF_LSHIFT 0x02
#define KF_CTRL   0x04
#define KF_ALT    0x08
#define KF_SCROLL 0x10
#define KF_NUM    0x20
#define KF_CAPS   0x40

static uint8_t kb_flags(const input_state *st)
{ return st->bda ? st->bda[BDA_KB_FLAGS] : 0; }

static void kb_flags_set(input_state *st, uint8_t bit, int on)
{
    if (!st->bda) return;
    if (on) st->bda[BDA_KB_FLAGS] = (uint8_t)(st->bda[BDA_KB_FLAGS] | bit);
    else    st->bda[BDA_KB_FLAGS] = (uint8_t)(st->bda[BDA_KB_FLAGS] & ~bit);
}

/* One scancode -> the BIOS's view of it: update the shift state, and for a make code
   that denotes a character or a named key, store AH=scancode AL=ascii in the ring. */
static void bios_translate(input_state *st, uint8_t sc)
{
    int is_break = (sc & 0x80) != 0;
    uint8_t code = (uint8_t)(sc & 0x7F);
    int ext = st->ext_pending;
    uint8_t fl, ascii;

    if (sc == 0xE0) { st->ext_pending = 1; return; }   /* prefix: the next code is extended */
    if (sc == 0xE1) { st->ext_pending = 0; return; }   /* Pause: ignored, not a key event   */
    st->ext_pending = 0;

    switch (code) {                                    /* modifiers: state, never a keystroke */
    case 0x2A: if (!ext) kb_flags_set(st, KF_LSHIFT, !is_break); return;  /* E0 2A is the
                  fake shift the controller brackets some extended keys with -- not a shift */
    case 0x36: kb_flags_set(st, KF_RSHIFT, !is_break); return;
    case 0x1D: kb_flags_set(st, KF_CTRL,   !is_break); return;
    case 0x38: kb_flags_set(st, KF_ALT,    !is_break); return;
    case 0x3A: if (!is_break) kb_flags_set(st, KF_CAPS,   !(kb_flags(st) & KF_CAPS));   return;
    case 0x45: if (!is_break) kb_flags_set(st, KF_NUM,    !(kb_flags(st) & KF_NUM));    return;
    case 0x46: if (!is_break) kb_flags_set(st, KF_SCROLL, !(kb_flags(st) & KF_SCROLL)); return;
    default: break;
    }
    if (is_break) return;                              /* releases change no buffer content */
    if (code > SC_TABLE_MAX || code == 0) return;

    fl = kb_flags(st);
    if (ext) {
        /* Extended keys are their scancode: arrows, nav cluster, keypad Enter and slash.
           AL stays 0 so the guest can tell E0 4B (LEFT) from '4'. */
        ascii = (code == 0x1C) ? 0x0D : (code == 0x35) ? '/' : 0;
        vdd_input_push(st, (uint16_t)((code << 8) | ascii));
        return;
    }
    ascii = (fl & (KF_LSHIFT | KF_RSHIFT)) ? sc_ascii_shift[code] : sc_ascii[code];
    if ((fl & KF_CAPS) && ascii >= 'a' && ascii <= 'z') ascii = (uint8_t)(ascii - 'a' + 'A');
    else if ((fl & KF_CAPS) && ascii >= 'A' && ascii <= 'Z') ascii = (uint8_t)(ascii - 'A' + 'a');
    if ((fl & KF_CTRL) && ((ascii >= 'a' && ascii <= 'z') || (ascii >= 'A' && ascii <= 'Z')))
        ascii = (uint8_t)(ascii & 0x1F);               /* Ctrl+letter -> 1..26 */
    if (code >= 0x47 && code <= 0x53 && !(fl & KF_NUM))
        ascii = 0;                                     /* NumLock off: the keypad navigates */
    vdd_input_push(st, (uint16_t)((code << 8) | ascii));
}

void vdd_input_bios_consume(input_state *st)
{
    uint8_t sc;
    if (st->sc_head == st->sc_tail) return;
    sc = st->sc_buf[st->sc_tail];
    st->sc_last = sc;
    st->sc_tail = next(st->sc_tail);
    bios_translate(st, sc);                 /* <- what the stub never did: make it a KEY */
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
    case 0x02:                              /* shift status, from 0040:0017        */
        s_al(r, kb_flags(st)); r->zf = 0;
        break;
    case 0x12:                              /* extended shift status: AL=0017 AH=0018 */
        s_al(r, kb_flags(st));
        s_ah(r, st->bda ? st->bda[BDA_KB_FLAGS2] : 0);
        r->zf = 0;
        break;
    default:                                /* unknown fn: report "no key", never  */
        r->zf = 1;                          /* a phantom keystroke (was a bug)     */
        break;
    }
}

void vdd_input_reset(void *self)
{
    input_state *st = (input_state *)self;
    st->sc_head = st->sc_tail = 0;
    st->sc_last = 0;
    st->ext_pending = 0;
    if (st->bda) {                          /* an empty ring is head==tail at its start */
        bda_w16(st, BDA_KB_HEAD, BDA_KB_START);
        bda_w16(st, BDA_KB_TAIL, BDA_KB_START);
        st->bda[BDA_KB_FLAGS]  = 0;
        st->bda[BDA_KB_FLAGS2] = 0;
    }
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
