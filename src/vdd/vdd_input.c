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
    /* HOW FULL DID IT EVER GET. sc_dropped only fires once the ring has ALREADY
       overflowed, which makes it a pass/fail light with nothing before the failure.
       A held arrow auto-repeats at the OS rate and costs TWO bytes a repeat (E0 +
       code), while IRQ1 is delivered only when the exec loop gets a turn -- so the
       interesting question is how close a guest that stops trapping for a while
       comes to filling 32 bytes. This answers it without needing an overflow.
       ► It matters because of what overflow DOES here: dropping the oldest byte can
         strand an E0 prefix, and a bare 0x48 is keypad-8, not up-arrow. A held arrow
         would quietly stop being an arrow. */
    { int depth = st->sc_head - st->sc_tail;
      if (depth < 0) depth += (int)VDD_KBD_SIZE;
      if ((uint32_t)depth > st->sc_hiwater) st->sc_hiwater = (uint32_t)depth; }
    if (was_empty && st->bus) vdd_raise_irq(st->bus, 1);     /* empty -> full        */
}

/* --- scancode set 1 -> the BIOS keycode (US layout) ------------------------- */
/* ── ★ FOUR COLUMNS, BECAUSE THE BIOS HAS FOUR. ────────────────────────────────────
     Indexed by make code 0x00..0x58: what INT 09h stores for the key plain, with
     Shift, with Ctrl and with Alt. A 0 entry means the BIOS stores NOTHING for that
     combination (Ctrl+1 on a real keyboard is silent). This is the IBM table as every
     BIOS carries it (SeaBIOS's scan_to_scanascii is the same data).
   ⚠ The old code had only a plain and a shifted ASCII column and derived Ctrl by
     masking; it never looked at Alt at all. So Alt+F arrived as AH=21h AL='f' -- a
     letter -- where the BIOS says 2100h, and every DOS editor's menu accelerator
     (edit.com, QBasic, Turbo Pascal's IDE, Norton) typed a letter instead of opening
     its menu. The F-key rows matter just as much: Shift+F1 is 5400h, Ctrl+F1 5E00h,
     Alt+F1 6800h, and a program that binds them (every editor) needs those exact codes.
   The keypad rows hold the NAVIGATION codes in the plain column and the DIGITS in the
   Shift column, because that is how NumLock works: it swaps the two, and Shift undoes
   the swap. The Alt column of the keypad digits is the BIOS's Alt+numpad ASCII-entry
   accumulator (not modelled -- stores nothing). */
#define SC_TABLE_MAX 0x58
static const uint16_t sc_key[SC_TABLE_MAX + 1][4] = {
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 00: none              */
    { 0x011B, 0x011B, 0x011B, 0x0100 },                 /* 01: Esc               */
    { 0x0231, 0x0221, 0x0000, 0x7800 },                 /* 02: 1 !               */
    { 0x0332, 0x0340, 0x0300, 0x7900 },                 /* 03: 2 @  (Ctrl+2=NUL) */
    { 0x0433, 0x0423, 0x0000, 0x7A00 },                 /* 04: 3 #               */
    { 0x0534, 0x0524, 0x0000, 0x7B00 },                 /* 05: 4 $               */
    { 0x0635, 0x0625, 0x0000, 0x7C00 },                 /* 06: 5 %               */
    { 0x0736, 0x075E, 0x071E, 0x7D00 },                 /* 07: 6 ^  (Ctrl+6=RS)  */
    { 0x0837, 0x0826, 0x0000, 0x7E00 },                 /* 08: 7 &               */
    { 0x0938, 0x092A, 0x0000, 0x7F00 },                 /* 09: 8 *               */
    { 0x0A39, 0x0A28, 0x0000, 0x8000 },                 /* 0A: 9 (               */
    { 0x0B30, 0x0B29, 0x0000, 0x8100 },                 /* 0B: 0 )               */
    { 0x0C2D, 0x0C5F, 0x0C1F, 0x8200 },                 /* 0C: - _  (Ctrl+-=US)  */
    { 0x0D3D, 0x0D2B, 0x0000, 0x8300 },                 /* 0D: = +               */
    { 0x0E08, 0x0E08, 0x0E7F, 0x0E00 },                 /* 0E: Backspace         */
    { 0x0F09, 0x0F00, 0x9400, 0xA500 },                 /* 0F: Tab               */
    { 0x1071, 0x1051, 0x1011, 0x1000 },                 /* 10: Q                 */
    { 0x1177, 0x1157, 0x1117, 0x1100 },                 /* 11: W                 */
    { 0x1265, 0x1245, 0x1205, 0x1200 },                 /* 12: E                 */
    { 0x1372, 0x1352, 0x1312, 0x1300 },                 /* 13: R                 */
    { 0x1474, 0x1454, 0x1414, 0x1400 },                 /* 14: T                 */
    { 0x1579, 0x1559, 0x1519, 0x1500 },                 /* 15: Y                 */
    { 0x1675, 0x1655, 0x1615, 0x1600 },                 /* 16: U                 */
    { 0x1769, 0x1749, 0x1709, 0x1700 },                 /* 17: I                 */
    { 0x186F, 0x184F, 0x180F, 0x1800 },                 /* 18: O                 */
    { 0x1970, 0x1950, 0x1910, 0x1900 },                 /* 19: P                 */
    { 0x1A5B, 0x1A7B, 0x1A1B, 0x1A00 },                 /* 1A: [ {               */
    { 0x1B5D, 0x1B7D, 0x1B1D, 0x1B00 },                 /* 1B: ] }               */
    { 0x1C0D, 0x1C0D, 0x1C0A, 0x1C00 },                 /* 1C: Enter             */
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 1D: Ctrl              */
    { 0x1E61, 0x1E41, 0x1E01, 0x1E00 },                 /* 1E: A                 */
    { 0x1F73, 0x1F53, 0x1F13, 0x1F00 },                 /* 1F: S                 */
    { 0x2064, 0x2044, 0x2004, 0x2000 },                 /* 20: D                 */
    { 0x2166, 0x2146, 0x2106, 0x2100 },                 /* 21: F                 */
    { 0x2267, 0x2247, 0x2207, 0x2200 },                 /* 22: G                 */
    { 0x2368, 0x2348, 0x2308, 0x2300 },                 /* 23: H                 */
    { 0x246A, 0x244A, 0x240A, 0x2400 },                 /* 24: J                 */
    { 0x256B, 0x254B, 0x250B, 0x2500 },                 /* 25: K                 */
    { 0x266C, 0x264C, 0x260C, 0x2600 },                 /* 26: L                 */
    { 0x273B, 0x273A, 0x0000, 0x2700 },                 /* 27: ; :               */
    { 0x2827, 0x2822, 0x0000, 0x2800 },                 /* 28: ' "               */
    { 0x2960, 0x297E, 0x0000, 0x2900 },                 /* 29: ` ~               */
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 2A: LShift            */
    { 0x2B5C, 0x2B7C, 0x2B1C, 0x2B00 },                 /* 2B: \ |               */
    { 0x2C7A, 0x2C5A, 0x2C1A, 0x2C00 },                 /* 2C: Z                 */
    { 0x2D78, 0x2D58, 0x2D18, 0x2D00 },                 /* 2D: X                 */
    { 0x2E63, 0x2E43, 0x2E03, 0x2E00 },                 /* 2E: C                 */
    { 0x2F76, 0x2F56, 0x2F16, 0x2F00 },                 /* 2F: V                 */
    { 0x3062, 0x3042, 0x3002, 0x3000 },                 /* 30: B                 */
    { 0x316E, 0x314E, 0x310E, 0x3100 },                 /* 31: N                 */
    { 0x326D, 0x324D, 0x320D, 0x3200 },                 /* 32: M                 */
    { 0x332C, 0x333C, 0x0000, 0x3300 },                 /* 33: , <               */
    { 0x342E, 0x343E, 0x0000, 0x3400 },                 /* 34: . >               */
    { 0x352F, 0x353F, 0x0000, 0x3500 },                 /* 35: / ?               */
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 36: RShift            */
    { 0x372A, 0x372A, 0x9600, 0x3700 },                 /* 37: keypad *          */
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 38: Alt               */
    { 0x3920, 0x3920, 0x3920, 0x3920 },                 /* 39: Space             */
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 3A: CapsLock          */
    { 0x3B00, 0x5400, 0x5E00, 0x6800 },                 /* 3B: F1                */
    { 0x3C00, 0x5500, 0x5F00, 0x6900 },                 /* 3C: F2                */
    { 0x3D00, 0x5600, 0x6000, 0x6A00 },                 /* 3D: F3                */
    { 0x3E00, 0x5700, 0x6100, 0x6B00 },                 /* 3E: F4                */
    { 0x3F00, 0x5800, 0x6200, 0x6C00 },                 /* 3F: F5                */
    { 0x4000, 0x5900, 0x6300, 0x6D00 },                 /* 40: F6                */
    { 0x4100, 0x5A00, 0x6400, 0x6E00 },                 /* 41: F7                */
    { 0x4200, 0x5B00, 0x6500, 0x6F00 },                 /* 42: F8                */
    { 0x4300, 0x5C00, 0x6600, 0x7000 },                 /* 43: F9                */
    { 0x4400, 0x5D00, 0x6700, 0x7100 },                 /* 44: F10               */
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 45: NumLock           */
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 46: ScrollLock        */
    { 0x4700, 0x4737, 0x7700, 0x0000 },                 /* 47: keypad 7 / Home   */
    { 0x4800, 0x4838, 0x8D00, 0x0000 },                 /* 48: keypad 8 / Up     */
    { 0x4900, 0x4939, 0x8400, 0x0000 },                 /* 49: keypad 9 / PgUp   */
    { 0x4A2D, 0x4A2D, 0x8E00, 0x4A00 },                 /* 4A: keypad -          */
    { 0x4B00, 0x4B34, 0x7300, 0x0000 },                 /* 4B: keypad 4 / Left   */
    { 0x4CF0, 0x4C35, 0x8F00, 0x0000 },                 /* 4C: keypad 5          */
    { 0x4D00, 0x4D36, 0x7400, 0x0000 },                 /* 4D: keypad 6 / Right  */
    { 0x4E2B, 0x4E2B, 0x9000, 0x4E00 },                 /* 4E: keypad +          */
    { 0x4F00, 0x4F31, 0x7500, 0x0000 },                 /* 4F: keypad 1 / End    */
    { 0x5000, 0x5032, 0x9100, 0x0000 },                 /* 50: keypad 2 / Down   */
    { 0x5100, 0x5133, 0x7600, 0x0000 },                 /* 51: keypad 3 / PgDn   */
    { 0x5200, 0x5230, 0x9200, 0x0000 },                 /* 52: keypad 0 / Ins    */
    { 0x5300, 0x532E, 0x9300, 0x0000 },                 /* 53: keypad . / Del    */
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 54: SysRq             */
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 55                    */
    { 0x565C, 0x567C, 0x0000, 0x0000 },                 /* 56: 102-key \ |       */
    { 0x8500, 0x8700, 0x8900, 0x8B00 },                 /* 57: F11               */
    { 0x8600, 0x8800, 0x8A00, 0x8C00 },                 /* 58: F12               */
};
/* The E0-prefixed (grey) keys: arrows, the nav cluster, keypad Enter and slash. Plain
   and Shift are the scancode with AL=0 -- the AL=0 is how a guest tells LEFT from '4'.
   Ctrl takes the keypad's Ctrl column above (Ctrl+Left = 7300h, the word-left of every
   editor). Alt has its OWN codes on the enhanced BIOS (Alt+Left = 9B00h ...), listed
   here for the scancodes that have one. */
static uint16_t sc_ext_alt(uint8_t code)
{
    switch (code) {
    case 0x47: return 0x9700; case 0x48: return 0x9800; case 0x49: return 0x9900;
    case 0x4B: return 0x9B00; case 0x4D: return 0x9D00; case 0x4F: return 0x9F00;
    case 0x50: return 0xA000; case 0x51: return 0xA100; case 0x52: return 0xA200;
    case 0x53: return 0xA300; case 0x1C: return 0xA600; case 0x35: return 0xA400;
    default:   return 0;
    }
}
/* A plain E0 key: the two the enhanced BIOS gives an ASCII to (keypad Enter and /),
   the rest AL=0. */
static uint16_t sc_ext_plain(uint8_t code)
{
    return (uint16_t)((code << 8) | (code == 0x1C ? 0x0D : code == 0x35 ? '/' : 0));
}
static int sc_is_letter(uint8_t code)
{
    uint16_t k = sc_key[code][0] & 0xFF;
    return k >= 'a' && k <= 'z';
}

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
    uint8_t fl, ascii = 0;
    uint16_t key;

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
    /* ── THE COLUMN IS DECIDED BY PRECEDENCE: Alt beats Ctrl beats Shift. ────────────
         That is the BIOS's order (Ctrl+Alt+Del is the Alt column of Del), and it is
         what makes Alt+Shift+F still 2100h. */
    if (ext) {
        if      (fl & KF_ALT)  key = sc_ext_alt(code);
        else if (fl & KF_CTRL) key = sc_key[code][2];
        else                   key = sc_ext_plain(code);   /* Shift changes nothing */
    } else if (fl & KF_ALT) {
        key = sc_key[code][3];
    } else if (fl & KF_CTRL) {
        key = sc_key[code][2];
    } else {
        int shifted = (fl & (KF_LSHIFT | KF_RSHIFT)) != 0;
        /* CapsLock inverts Shift for LETTERS only; NumLock inverts it for the KEYPAD
           only. Neither touches anything else, so '1' stays '1' with Caps on. */
        if ((fl & KF_CAPS) && sc_is_letter(code)) shifted = !shifted;
        if ((fl & KF_NUM) && code >= 0x47 && code <= 0x53) shifted = !shifted;
        key = sc_key[code][shifted ? 1 : 0];
    }
    if (key) vdd_input_push(st, key);                  /* 0 = the BIOS stores nothing */
    (void)ascii;
}

/* The BIOS INT 09h arm. Normally the byte is still in the FIFO; but a guest that hooked
   INT 09h ahead of us and read port 0x60 itself has already popped it (see sc_bios_owed),
   and the real BIOS would simply read the same byte again from the 8042. So: FIFO first,
   the owed byte second, and nothing if neither -- a spurious INT 09h with no key must
   not re-translate a stale byte. */
void vdd_input_bios_consume(input_state *st)
{
    uint8_t sc;
    if (st->sc_head == st->sc_tail) {
        if (st->sc_bios_owed) {
            st->sc_bios_owed = 0;
            st->sc_owed_served++;
            bios_translate(st, st->sc_last);
        }
        return;
    }
    sc = st->sc_buf[st->sc_tail];
    st->sc_last = sc;
    st->sc_bios_owed = 0;                   /* a newer byte supersedes any owed one    */
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
        st->sc_bios_owed = 1;               /* the BIOS arm may still chain and want it */
        /* Still more queued? The controller presents the next byte and re-asserts the
           line, so the guest gets exactly one interrupt per scancode, at its own pace. */
        if (st->sc_head != st->sc_tail && st->bus) vdd_raise_irq(st->bus, 1);
    }
    *val = st->sc_last;
}

/* 8042 command/data writes. Still ACCEPTED-AND-IGNORED behaviourally -- changing
   what the keyboard does on the strength of an untested guess is how the last two
   attempts at this went -- but no longer SILENTLY. See the fields in vdd_input.h:
   what we want out of a run is whether the guest ever sets its own typematic rate,
   and to what. */
static void kbd_hw_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    input_state *st = (input_state *)self;
    uint8_t b = (uint8_t)v;
    (void)w;
    if (st) {
        st->kbd_out_writes++;
        if (st->kbd_out_n < 16) {
            st->kbd_out_log[st->kbd_out_n][0] = (uint8_t)(port & 0xFF);
            st->kbd_out_log[st->kbd_out_n][1] = b;
            st->kbd_out_n++;
        }
        if (st->kbd_expect_rate) {          /* the byte after 0xF3 is the rate     */
            st->kbd_typematic_byte = b;
            st->kbd_typematic_set  = 1;
            st->kbd_expect_rate    = 0;
        } else if (port == 0x60 && b == 0xF3) {
            st->kbd_expect_rate = 1;
        }
    }
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
    st->sc_bios_owed = 0;
    st->ext_pending = 0;
    if (st->bda) {                          /* an empty ring is head==tail at its start */
        bda_w16(st, BDA_KB_HEAD, BDA_KB_START);
        bda_w16(st, BDA_KB_TAIL, BDA_KB_START);
        st->bda[BDA_KB_FLAGS]  = 0;
        st->bda[BDA_KB_FLAGS2] = 0;
        /* 0040:0096 bit 4 = "enhanced (101/102-key) keyboard present". It is what a
           program checks before it uses INT 16h AH=10h/11h and the F11/F12 and grey
           key codes -- edit.com and QBasic among them. We serve those functions, so
           say so; a zero here makes them fall back to the 83-key subset. */
        st->bda[0x96] = (uint8_t)(st->bda[0x96] | 0x10);
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
