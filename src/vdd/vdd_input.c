/* vdd_input.c -- see vdd_input.h.  Keyboard ring buffer + INT 16h servicer, on
 * the VDD bus.  Pure C, no <windows.h>; non-blocking (reports empty via ZF). */
#include "vdd_input.h"

static int next(int i) { return (i + 1) % VDD_KBD_SIZE; }

/* --- the BIOS keyboard ring, in guest memory at 0040:001E ------------------ */

static uint16_t bda_r16(const input_state *st, int off)
{ return (uint16_t)(st->bda[off] | (st->bda[off + 1] << 8)); }

static void bda_w16(input_state *st, int off, uint16_t v)
{ st->bda[off] = (uint8_t)v; st->bda[off + 1] = (uint8_t)(v >> 8); }

/* #274: the ring's bounds, from 0040:0080/0082 (see vdd_input.h). A pair that cannot
   describe a ring -- odd, empty, inverted, or too small to hold one key -- is POST's
   001E/003E instead: a BIOS that trusted it would write keys through a wild pointer. */
static void kb_bounds(const input_state *st, uint16_t *s, uint16_t *e)
{
    uint16_t a = bda_r16(st, BDA_KB_BUFSTART), b = bda_r16(st, BDA_KB_BUFEND);
    if ((a & 1) || (b & 1) || a >= b || (uint16_t)(b - a) < 4) { a = BDA_KB_START; b = BDA_KB_END; }
    *s = a; *e = b;
}

/* Advance a ring pointer, wrapping at the end of the buffer. */
static uint16_t bda_next(uint16_t p, uint16_t s, uint16_t e)
{ p += 2; return (p >= e) ? s : p; }

int vdd_input_push(input_state *st, uint16_t key)
{
    uint16_t head, tail, n, s, e;
    if (!st->bda) return 0;               /* no guest memory yet: nowhere to put it */
    kb_bounds(st, &s, &e);
    head = bda_r16(st, BDA_KB_HEAD);
    tail = bda_r16(st, BDA_KB_TAIL);
    /* A pointer pair the guest has not initialised (or has scribbled on) would send the
       writes anywhere in the BDA, so validate before trusting them. */
    if (head < s || head >= e || ((head - s) & 1) ||
        tail < s || tail >= e || ((tail - s) & 1)) {
        head = tail = s;
        bda_w16(st, BDA_KB_HEAD, head);
    }
    n = bda_next(tail, s, e);
    if (n == head) return 0;              /* full -> discard the NEW key: the real BIOS
                                             beeps and throws it away. Dropping the OLDEST
                                             instead would split a keystroke stream.
                                             The RESULT is the answer INT 16h AH=05h owes
                                             its caller (AL=1 = full), so it is returned
                                             rather than swallowed. */
    bda_w16(st, tail, key);
    bda_w16(st, BDA_KB_TAIL, n);
    return 1;
}

int vdd_input_pop(input_state *st, uint16_t *key)
{
    uint16_t head, tail, s, e;
    if (!st->bda) return 0;
    kb_bounds(st, &s, &e);
    head = bda_r16(st, BDA_KB_HEAD);
    tail = bda_r16(st, BDA_KB_TAIL);
    if (head == tail) return 0;
    if (head < s || head >= e || ((head - s) & 1)) return 0;
    *key = bda_r16(st, head);
    bda_w16(st, BDA_KB_HEAD, bda_next(head, s, e));
    return 1;
}

int vdd_input_peek(input_state *st, uint16_t *key)
{
    uint16_t head, tail, s, e;
    if (!st->bda) return 0;
    kb_bounds(st, &s, &e);
    head = bda_r16(st, BDA_KB_HEAD);
    tail = bda_r16(st, BDA_KB_TAIL);
    if (head == tail) return 0;
    if (head < s || head >= e || ((head - s) & 1)) return 0;
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
/* Is the byte at the head of the FIFO presented in the output buffer yet? */
static int sc_avail(const input_state *st)
{
    if (st->sc_head == st->sc_tail) return 0;
    if (st->time_us && st->time_us() < st->sc_hold_until) return 0;
    return 1;
}
/* Take the head byte out of the output buffer: it becomes the re-read value, the
   BIOS arm is owed it, and the keyboard starts sending the next one (the hold). */
static uint8_t sc_pop(input_state *st)
{
    uint8_t sc = st->sc_buf[st->sc_tail];
    st->sc_tail = next(st->sc_tail);
    st->sc_last = sc;
    st->sc_irq_up = 0;
    if (st->time_us) st->sc_hold_until = st->time_us() + KBD_XFER_US;
    return sc;
}
/* Raise IRQ1 for the head byte if it is presented and nobody has announced it. */
void vdd_input_poll(input_state *st)
{
    if (st->sc_irq_up || !sc_avail(st)) return;
    st->sc_irq_up = 1;
    st->sc_bios_owed = 0;       /* the buffer now holds THIS byte; the old one is gone */
    if (st->bus) vdd_raise_irq(st->bus, 1);
}
int vdd_input_sc_queued(const input_state *st)
{
    int depth = st->sc_head - st->sc_tail;
    if (depth < 0) depth += (int)VDD_KBD_SIZE;
    return depth;
}

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
    /* empty -> full: announce it now if the keyboard may send yet, else the poll will
       when the transfer delay from the last pop has passed. */
    if (was_empty) vdd_input_poll(st);
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
   the swap. The Alt column of the keypad digits is 0 because those keys feed the BIOS's
   Alt+numpad accumulator at 0040:0019 instead of storing anything (#274, kb_altnum). */
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
/* ── #254: A PLAIN E0 KEY, IN THE ENHANCED BIOS's OWN FORM. ─────────────────────────
     The grey cluster is what AH=10h/11h exist to tell apart from the keypad, and the
     enhanced BIOS marks it in the code itself: AL=E0h for the grey arrows / nav keys
     (grey Left = 4BE0h, keypad Left = 4B00h), and AH=E0h for keypad Enter (E00Dh) and
     keypad slash (E02Fh). This stored the 83-key forms (4B00h, 1C0Dh, 352Fh), so the
     two clusters were indistinguishable. kb_compat() folds these back for AH=00h/01h,
     as IBM's K1S translation does; DOS's CON reads through vdd_input_dos_key(). */
static uint16_t sc_ext_plain(uint8_t code)
{
    if (code == 0x1C) return 0xE00D;                 /* keypad Enter */
    if (code == 0x35) return 0xE02F;                 /* keypad /     */
    return (uint16_t)((code << 8) | 0xE0);
}
/* ...and with Ctrl: the keypad's Ctrl code with AL=E0h for the grey nav keys (Ctrl+
   grey Left = 73E0h), keypad Enter E00Ah, keypad slash 9500h (RBIL's INT 16h table). */
static uint16_t sc_ext_ctrl(uint8_t code)
{
    if (code == 0x1C) return 0xE00A;
    if (code == 0x35) return 0x9500;
    if (code >= 0x47 && code <= 0x53 && code != 0x4A && code != 0x4C && code != 0x4E)
        return (uint16_t)((sc_key[code][2] & 0xFF00) | 0xE0);
    return 0;
}
/* ── #136: KEYBOARD LAYOUTS, TAKEN FROM WINDOWS XP'S OWN TABLES. ─────────────────────
     The table above is the US BIOS. A layout only changes what the PLAIN and SHIFT
     columns produce for some keys, so each layout is an overlay of (scan code, plain,
     shift) in the DOS code page. The rows were NOT typed from memory: `rigshot kbdmap
     <KLID>` asked XP (LoadKeyboardLayout -> ToAsciiEx -> CharToOem) what every scan
     code produces on the rig, and the tables are the differences from its US answer
     (runs/s82/kbdmap.txt; the US dump matches this BIOS table except Shift+Tab and
     keypad 5, where the BIOS deliberately differs). Dead keys (German ^ and the accent
     key, French ^) keep the US character: composition is not modelled. Ctrl and Alt
     columns are unchanged, except that a letter which MOVED (German Y/Z, French A/Q/
     W/Z/M) gets the Ctrl code of the letter it now types. Index = SET_KBLAYOUT's item:
     US | United Kingdom | German | French. */
typedef struct { uint8_t sc, plain, shift; } kbd_over;
/* United Kingdom (XP layout 00000809): 5 keys differ from US. */
static const kbd_over KBD_UK[] = {
    { 0x03, 0x32, 0x22 },
    { 0x04, 0x33, 0x9C },
    { 0x28, 0x27, 0x40 },
    { 0x29, 0x60, 0xAA },
    { 0x2B, 0x23, 0x7E },
    { 0, 0, 0 }
};
/* German (XP layout 00000407): 19 keys differ from US. */
static const kbd_over KBD_DE[] = {
    { 0x03, 0x32, 0x22 },
    { 0x04, 0x33, 0xF5 },
    { 0x07, 0x36, 0x26 },
    { 0x08, 0x37, 0x2F },
    { 0x09, 0x38, 0x28 },
    { 0x0A, 0x39, 0x29 },
    { 0x0B, 0x30, 0x3D },
    { 0x0C, 0xE1, 0x3F },
    { 0x15, 0x7A, 0x5A },
    { 0x1A, 0x81, 0x9A },
    { 0x1B, 0x2B, 0x2A },
    { 0x27, 0x94, 0x99 },
    { 0x28, 0x84, 0x8E },
    { 0x2B, 0x23, 0x27 },
    { 0x2C, 0x79, 0x59 },
    { 0x33, 0x2C, 0x3B },
    { 0x34, 0x2E, 0x3A },
    { 0x35, 0x2D, 0x5F },
    { 0x56, 0x3C, 0x3E },
    { 0, 0, 0 }
};
/* French (XP layout 0000040C): 25 keys differ from US. */
static const kbd_over KBD_FR[] = {
    { 0x02, 0x26, 0x31 },
    { 0x03, 0x82, 0x32 },
    { 0x04, 0x22, 0x33 },
    { 0x05, 0x27, 0x34 },
    { 0x06, 0x28, 0x35 },
    { 0x07, 0x2D, 0x36 },
    { 0x08, 0x8A, 0x37 },
    { 0x09, 0x5F, 0x38 },
    { 0x0A, 0x87, 0x39 },
    { 0x0B, 0x85, 0x30 },
    { 0x0C, 0x29, 0xF8 },
    { 0x10, 0x61, 0x41 },
    { 0x11, 0x7A, 0x5A },
    { 0x1B, 0x24, 0x9C },
    { 0x1E, 0x71, 0x51 },
    { 0x27, 0x6D, 0x4D },
    { 0x28, 0x97, 0x25 },
    { 0x29, 0xFD, 0x00 },
    { 0x2B, 0x2A, 0xE6 },
    { 0x2C, 0x77, 0x57 },
    { 0x32, 0x2C, 0x3F },
    { 0x33, 0x3B, 0x2E },
    { 0x34, 0x3A, 0x2F },
    { 0x35, 0x21, 0xF5 },
    { 0x56, 0x3C, 0x3E },
    { 0, 0, 0 }
};

static const kbd_over *const KBD_LAYOUT[4] = { 0, KBD_UK, KBD_DE, KBD_FR };

/* The plain (shift=0) or shifted character of a key on the active layout; 0 = none. */
static uint8_t kbd_char(const input_state *st, uint8_t code, int shift)
{
    const kbd_over *o = (st->layout < 4) ? KBD_LAYOUT[st->layout] : 0;
    for (; o && o->sc; ++o) if (o->sc == code) return shift ? o->shift : o->plain;
    if (code > SC_TABLE_MAX) return 0;
    return (uint8_t)(sc_key[code][shift ? 1 : 0] & 0xFF);
}

static int sc_is_letter_on(const input_state *st, uint8_t code)
{
    uint8_t k = kbd_char(st, code, 0);
    return k >= 'a' && k <= 'z';
}

int vdd_input_char_to_key(const input_state *st, uint8_t ch, uint8_t *sc, int *shift)
{
    uint8_t c;
    int sh;
    for (sh = 0; sh < 2; ++sh)
        for (c = 0x02; c <= SC_TABLE_MAX; ++c) {
            if (c == 0x0F && sh) continue;          /* Shift+Tab is back-tab, not a char */
            if (c >= 0x47 && c <= 0x53) continue;   /* the keypad: NumLock decides it   */
            if (kbd_char(st, c, sh) == ch) { *sc = c; *shift = sh; return 1; }
        }
    return 0;
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

/* ── #254: THE REST OF THE BIOS's KEYBOARD STATE. ─────────────────────────────────────
     0040:0018 (KB_FLAG_1): bit 0 LEFT Ctrl held, 1 LEFT Alt held, 2 SysReq held,
       3 PAUSE active, 4 Scroll held, 5 NumLock held, 6 Caps held, 7 Insert held.
     0040:0096 (KB_FLAG_3): bit 0 last code was E1h, 1 last code was E0h, 2 RIGHT Ctrl
       held, 3 RIGHT Alt held, 4 enhanced keyboard (set at reset).
     0040:0017 bit 2/3 (Ctrl/Alt) are "either side", so they follow the two held bits
     rather than the last make/break -- releasing left Ctrl while right is held used to
     clear Ctrl. The lock keys toggle once per PRESS: a held key's typematic repeats
     arrive as more makes, and the "held" bit is what stops them re-toggling. */
#define KF1_LCTRL  0x01
#define KF1_LALT   0x02
#define KF1_SYSRQ  0x04
#define KF1_PAUSE  0x08
#define KF1_SCROLL 0x10
#define KF1_NUM    0x20
#define KF1_CAPS   0x40
#define KF1_INS    0x80
#define KF3_E1     0x01
#define KF3_E0     0x02
#define KF3_RCTRL  0x04
#define KF3_RALT   0x08
#define KF_INS     0x80
#define BDA_KB_FLAGS3 0x96
#define BDA_BREAK     0x71

static uint8_t kbf(const input_state *st, int off) { return st->bda ? st->bda[off] : 0; }
static void kbf_set(input_state *st, int off, uint8_t bit, int on)
{
    if (!st->bda) return;
    if (on) st->bda[off] = (uint8_t)(st->bda[off] | bit);
    else    st->bda[off] = (uint8_t)(st->bda[off] & ~bit);
}
/* A lock key: toggle `lock` in 0017 on the first make only; track `held` in 0018. */
static void kb_lock_key(input_state *st, uint8_t lock, uint8_t held, int is_break)
{
    if (is_break) { kbf_set(st, BDA_KB_FLAGS2, held, 0); return; }
    if (kbf(st, BDA_KB_FLAGS2) & held) return;           /* typematic repeat */
    kbf_set(st, BDA_KB_FLAGS2, held, 1);
    kb_flags_set(st, lock, !(kb_flags(st) & lock));
}

/* ── #274: ALT + KEYPAD DIGITS = THE CHARACTER WITH THAT DECIMAL CODE. ─────────────────
     While Alt is held, each keypad digit (the non-E0 keypad, whatever NumLock says)
     does `0040:0019 = 0040:0019 * 10 + digit` -- a BYTE, so it wraps mod 256 as the
     BIOS's does -- and stores nothing. When Alt is released, a non-zero accumulator is
     stored as AH=00h AL=value (Alt+2+4+0 -> 00F0h, the code kb_compat already lets
     through) and the accumulator is cleared; zero stores nothing. Any OTHER key pressed
     with Alt held throws the accumulated value away and is translated as usual.
     (IBM PC/AT TR, KB_INT "ALT-INPUT-TABLE" / K32 "zero anything that's been
     accumulated"; RBIL MEMORY.LST 0040:0019.) Unmeasured: no oracle can hold Alt.
   ⚠ With BOTH Alts down the value is stored when the last one comes up -- 0017 bit 3
     is "either Alt", and that is the bit this follows. Ctrl+Alt still accumulates: the
     AT BIOS checks Ctrl+Alt only for Del, which a VDD cannot honour anyway. */
static int kp_digit(uint8_t code)
{
    switch (code) {
    case 0x52: return 0; case 0x4F: return 1; case 0x50: return 2; case 0x51: return 3;
    case 0x4B: return 4; case 0x4C: return 5; case 0x4D: return 6;
    case 0x47: return 7; case 0x48: return 8; case 0x49: return 9;
    default:   return -1;
    }
}

/* One scancode -> the BIOS's view of it: update the shift state, and for a make code
   that denotes a character or a named key, store AH=scancode AL=ascii in the ring.
   Returns a KB_ACT_* for the caller to run (#254). */
static int bios_translate(input_state *st, uint8_t sc)
{
    int is_break = (sc & 0x80) != 0;
    uint8_t code = (uint8_t)(sc & 0x7F);
    int ext = st->ext_pending;
    uint8_t fl, ascii = 0;
    uint16_t key;

    if (sc == 0xE0) {                                  /* prefix: the next code is extended */
        st->ext_pending = 1;
        kbf_set(st, BDA_KB_FLAGS3, KF3_E0, 1);
        return KB_ACT_NONE;
    }
    if (sc == 0xE1) {                                  /* Pause: E1 1D 45 / E1 9D C5        */
        st->ext_pending = 0; st->e1_pending = 2;
        kbf_set(st, BDA_KB_FLAGS3, KF3_E1, 1);
        return KB_ACT_NONE;
    }
    st->ext_pending = 0;
    kbf_set(st, BDA_KB_FLAGS3, KF3_E0 | KF3_E1, 0);
    if (st->e1_pending) {
        /* ── PAUSE. The make sequence is E1 1D 45, the break E1 9D C5, and neither is
             a Ctrl or a NumLock (the E1 is there so an old BIOS reads it as
             Ctrl+NumLock, the 83-key pause). The BIOS sets 0018 bit 3 and spins in
             its handler, interrupts on, until the next keystroke. */
        if (--st->e1_pending) return KB_ACT_NONE;      /* the 1D / 9D                       */
        if (is_break || (kbf(st, BDA_KB_FLAGS2) & KF1_PAUSE)) return KB_ACT_NONE;
        kbf_set(st, BDA_KB_FLAGS2, KF1_PAUSE, 1);
        st->bios_actions[KB_ACT_PAUSE]++;
        return KB_ACT_PAUSE;
    }

    switch (code) {                                    /* modifiers: state, never a keystroke */
    case 0x2A: if (!ext) kb_flags_set(st, KF_LSHIFT, !is_break); return KB_ACT_NONE;
               /* E0 2A is the fake shift the controller brackets some extended keys
                  with -- not a shift. Likewise E0 36. */
    case 0x36: if (!ext) kb_flags_set(st, KF_RSHIFT, !is_break); return KB_ACT_NONE;
    case 0x1D:
        if (ext) kbf_set(st, BDA_KB_FLAGS3, KF3_RCTRL, !is_break);
        else     kbf_set(st, BDA_KB_FLAGS2, KF1_LCTRL, !is_break);
        kb_flags_set(st, KF_CTRL, (kbf(st, BDA_KB_FLAGS2) & KF1_LCTRL) || (kbf(st, BDA_KB_FLAGS3) & KF3_RCTRL));
        return KB_ACT_NONE;
    case 0x38:
        if (ext) kbf_set(st, BDA_KB_FLAGS3, KF3_RALT, !is_break);
        else     kbf_set(st, BDA_KB_FLAGS2, KF1_LALT, !is_break);
        kb_flags_set(st, KF_ALT, (kbf(st, BDA_KB_FLAGS2) & KF1_LALT) || (kbf(st, BDA_KB_FLAGS3) & KF3_RALT));
        if (is_break && !(kb_flags(st) & KF_ALT) && st->bda) {   /* #274: Alt+keypad ends */
            uint8_t v = st->bda[BDA_KB_ALTNUM];
            st->bda[BDA_KB_ALTNUM] = 0;
            if (v) vdd_input_push(st, v);              /* AH=00h AL=the code         */
        }
        return KB_ACT_NONE;
    case 0x3A: kb_lock_key(st, KF_CAPS, KF1_CAPS, is_break); return KB_ACT_NONE;
    case 0x45:
        /* ⚠ Plain 45 is NumLock on the keyboard; our host sends NumLock as E0 45 (the
             Win32 extended bit), so both forms are NumLock here. Ctrl + a plain 45 is
             the 83-key PAUSE. */
        if (!ext && !is_break && (kb_flags(st) & KF_CTRL)) {
            if (kbf(st, BDA_KB_FLAGS2) & KF1_PAUSE) return KB_ACT_NONE;
            kbf_set(st, BDA_KB_FLAGS2, KF1_PAUSE, 1);
            st->bios_actions[KB_ACT_PAUSE]++;
            return KB_ACT_PAUSE;
        }
        kb_lock_key(st, KF_NUM, KF1_NUM, is_break); return KB_ACT_NONE;
    case 0x46:
        /* ── CTRL-BREAK. The Break key sends E0 46 with Ctrl held (and Ctrl+Scroll Lock
             is Break on the 83-key board). The BIOS empties the ring, sets 0040:0071
             bit 7, calls INT 1Bh, and stores 0000h. Not a Scroll Lock toggle. */
        if (!is_break && (kb_flags(st) & KF_CTRL)) {
            if (st->bda) {
                st->bda[BDA_KB_HEAD] = st->bda[BDA_KB_TAIL];
                st->bda[BDA_KB_HEAD + 1] = st->bda[BDA_KB_TAIL + 1];
                st->bda[BDA_BREAK] = (uint8_t)(st->bda[BDA_BREAK] | 0x80);
            }
            vdd_input_push(st, 0x0000);
            st->bios_actions[KB_ACT_BREAK]++;
            return KB_ACT_BREAK;
        }
        if (ext) return KB_ACT_NONE;                   /* E0 46 without Ctrl: nothing    */
        kb_lock_key(st, KF_SCROLL, KF1_SCROLL, is_break); return KB_ACT_NONE;
    case 0x54:
        /* ── SYSREQ (Alt+Print Screen). Held bit 0018 bit 2; INT 15h AX=8500h on the
             press, 8501h on the release. Stores nothing. */
        if (is_break) {
            if (!(kbf(st, BDA_KB_FLAGS2) & KF1_SYSRQ)) return KB_ACT_NONE;
            kbf_set(st, BDA_KB_FLAGS2, KF1_SYSRQ, 0);
            st->bios_actions[KB_ACT_SYSRQ_U]++;
            return KB_ACT_SYSRQ_U;
        }
        if (kbf(st, BDA_KB_FLAGS2) & KF1_SYSRQ) return KB_ACT_NONE;   /* repeat */
        kbf_set(st, BDA_KB_FLAGS2, KF1_SYSRQ, 1);
        st->bios_actions[KB_ACT_SYSRQ_D]++;
        return KB_ACT_SYSRQ_D;
    case 0x52:
        /* ── INSERT. 0018 bit 7 while held; 0017 bit 7 toggles on the press when the
             key is acting as Insert (grey, or keypad with NumLock and Shift agreeing)
             and Alt/Ctrl are up. The keystroke is stored as well. */
        if (is_break) { kbf_set(st, BDA_KB_FLAGS2, KF1_INS, 0); return KB_ACT_NONE; }
        fl = kb_flags(st);
        if (!(kbf(st, BDA_KB_FLAGS2) & KF1_INS) && !(fl & (KF_ALT | KF_CTRL))
            && (ext || !(fl & KF_NUM) == !(fl & (KF_LSHIFT | KF_RSHIFT))))
            kb_flags_set(st, KF_INS, !(fl & KF_INS));
        kbf_set(st, BDA_KB_FLAGS2, KF1_INS, 1);
        break;
    default: break;
    }
    if (is_break) return KB_ACT_NONE;                  /* releases change no buffer content */
    /* ── WHILE PAUSED, the next keystroke ends the pause and is thrown away. */
    if (kbf(st, BDA_KB_FLAGS2) & KF1_PAUSE) {
        kbf_set(st, BDA_KB_FLAGS2, KF1_PAUSE, 0);
        return KB_ACT_NONE;
    }
    /* ── #274: ALT + A KEYPAD DIGIT ACCUMULATES; ANY OTHER KEY UNDER ALT CLEARS IT. */
    if ((kb_flags(st) & KF_ALT) && st->bda) {
        int d = ext ? -1 : kp_digit(code);
        if (d >= 0) {
            st->bda[BDA_KB_ALTNUM] = (uint8_t)(st->bda[BDA_KB_ALTNUM] * 10 + d);
            return KB_ACT_NONE;
        }
        st->bda[BDA_KB_ALTNUM] = 0;
    }
    /* ── PRINT SCREEN: E0 37 (the grey key). Ctrl+PrtSc is the 7200h keystroke; on
         its own it calls INT 05h and stores nothing (it used to store 3700h). */
    if (ext && code == 0x37) {
        if (kb_flags(st) & KF_CTRL) { vdd_input_push(st, 0x7200); return KB_ACT_NONE; }
        if (kb_flags(st) & KF_ALT)  return KB_ACT_NONE;
        st->bios_actions[KB_ACT_PRTSC]++;
        return KB_ACT_PRTSC;
    }
    if (code > SC_TABLE_MAX || code == 0) return KB_ACT_NONE;

    fl = kb_flags(st);
    /* ── THE COLUMN IS DECIDED BY PRECEDENCE: Alt beats Ctrl beats Shift. ────────────
         That is the BIOS's order (Ctrl+Alt+Del is the Alt column of Del), and it is
         what makes Alt+Shift+F still 2100h. */
    if (ext) {
        if      (fl & KF_ALT)  key = sc_ext_alt(code);
        else if (fl & KF_CTRL) key = sc_ext_ctrl(code);
        else                   key = sc_ext_plain(code);   /* Shift changes nothing */
    } else if (fl & KF_ALT) {
        key = sc_key[code][3];
    } else if (fl & KF_CTRL) {
        key = sc_key[code][2];
        if (st->layout && sc_is_letter_on(st, code))           /* #136: a moved letter */
            key = (uint16_t)((code << 8) | (kbd_char(st, code, 0) & 0x1F));
    } else {
        int shifted = (fl & (KF_LSHIFT | KF_RSHIFT)) != 0;
        /* CapsLock inverts Shift for LETTERS only; NumLock inverts it for the KEYPAD
           only. Neither touches anything else, so '1' stays '1' with Caps on. */
        if ((fl & KF_CAPS) && sc_is_letter_on(st, code)) shifted = !shifted;
        if ((fl & KF_NUM) && code >= 0x47 && code <= 0x53) shifted = !shifted;
        key = sc_key[code][shifted ? 1 : 0];
        if (st->layout && !(code >= 0x47 && code <= 0x53) && !(code == 0x0F && shifted)) {
            uint8_t ch = kbd_char(st, code, shifted);        /* #136: the layout's char */
            if ((key & 0xFF) != ch) key = ch ? (uint16_t)((code << 8) | ch) : 0;
        }
    }
    if (key) vdd_input_push(st, key);                  /* 0 = the BIOS stores nothing */
    (void)ascii;
    return KB_ACT_NONE;
}

void vdd_input_pause_cancel(input_state *st)
{ kbf_set(st, BDA_KB_FLAGS2, KF1_PAUSE, 0); }

uint16_t vdd_input_dos_key(uint16_t key)
{
    uint8_t sc = (uint8_t)(key >> 8), ch = (uint8_t)key;
    if (sc == 0xE0) return (uint16_t)(((ch == 0x0D || ch == 0x0A) ? 0x1C00 : 0x3500) | ch);
    if (ch == 0xE0 && sc != 0) return (uint16_t)(sc << 8);
    return key;
}

/* The BIOS INT 09h arm. Normally the byte is still in the FIFO; but a guest that hooked
   INT 09h ahead of us and read port 0x60 itself has already popped it (see sc_bios_owed),
   and the real BIOS would simply read the same byte again from the 8042. So: FIFO first,
   the owed byte second, and nothing if neither -- a spurious INT 09h with no key must
   not re-translate a stale byte. */
int vdd_input_bios_fetch(input_state *st)
{
    uint8_t sc;
    /* Chained after a hook that read the byte: the output buffer still shows that
       byte (the keyboard has not sent the next one yet), so that is what the BIOS
       reads -- NOT the next queued byte, which belongs to the next interrupt. */
    if (st->sc_bios_owed) {
        st->sc_bios_owed = 0;
        st->sc_owed_served++;
        return st->sc_last;
    }
    if (!sc_avail(st)) return -1;           /* spurious: nothing presented           */
    sc = sc_pop(st);
    vdd_input_poll(st);                     /* next byte: now (no clock) or after the hold */
    return sc;
}

int vdd_input_bios_translate(input_state *st, uint8_t sc)
{
    return bios_translate(st, sc);          /* <- what the stub never did: make it a KEY */
}

int vdd_input_bios_consume(input_state *st)
{
    int sc = vdd_input_bios_fetch(st);
    return sc < 0 ? KB_ACT_NONE : bios_translate(st, (uint8_t)sc);
}

int vdd_input_host_key_bytes(uint8_t rawsc, int ext, int is_break,
                             uint8_t out[6], int *no_repeat)
{
    int n = 0;
    *no_repeat = 0;
    if (rawsc == 0x45 && !ext) {            /* Pause: the whole sequence on the press */
        static const uint8_t seq[6] = { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 };
        *no_repeat = 1;
        if (is_break) return 0;
        for (n = 0; n < 6; ++n) out[n] = seq[n];
        return 6;
    }
    if (rawsc == 0x46 && ext) {             /* Ctrl+Break: make AND break on the press */
        *no_repeat = 1;
        if (is_break) return 0;
        out[0] = 0xE0; out[1] = 0x46; out[2] = 0xE0; out[3] = 0xC6;
        return 4;
    }
    if (ext) out[n++] = 0xE0;
    out[n++] = is_break ? (uint8_t)(rawsc | 0x80) : rawsc;
    return n;
}

/* "Is a byte presented?" -- OBF, as the host's delivery gates ask it. */
int vdd_input_sc_pending(const input_state *st)
{
    return sc_avail(st);
}

/* Present a controller reply at port 60h. One byte deep, which is what the part
   is: a second command before the first is read simply overwrites it. */
static void kbc_reply(input_state *st, uint8_t v)
{ st->kbc_reply = v; st->kbc_reply_rdy = 1; }

/* ── A20: ONE WIRE, AND EVERY DOOR READS THE SAME BIT. ───────────────────────
     The 8042's output port (bit 1), System Control Port A (port 92h bit 1) and
     the XMS driver's AH=03h..07h are three interfaces to ONE line, and software
     picks whichever it likes. We used to keep a flag per door and only XMS had
     one, so a guest that opened the gate the hardware way and then asked XMS was
     told it was shut -- and concluded the machine could not do XMS.
   ⚠ THIS IS THE FLAG, NOT THE ADDRESS WRAP. Not modelling the wrap is a separate
     decision recorded in dos_xms.h and in main.c, and it still stands; what was
     wrong was that the three ways of ASKING disagreed with each other. */
void vdd_input_a20_set(input_state *st, int on)
{ if (on) st->kbc_outport |= 0x02; else st->kbc_outport &= (uint8_t)~0x02; }
int  vdd_input_a20_get(const input_state *st)
{ return (st->kbc_outport & 0x02) ? 1 : 0; }

/* ── SYSTEM CONTROL PORT A (92h) -- THE FAST A20 GATE. ───────────────────────
     Bit 1 is A20, the same wire as the output port's bit 1, and bit 0 is the
     PS/2 FAST RESET. Nothing claimed this port at all, so a write vanished and a
     read returned the bus's absent-device 0xFF -- whose bit 1 is SET, so a guest
     checking the gate here was told "A20 is on" by an empty bus. Right answer,
     no mechanism, and wrong the moment anything turned it off.
   ⚠ 6.22-under-QEMU and dosbox-x both decode 92h (p_kbc kbc.port92.read = 0x02);
     PCem's AT-class config answers 0xFF, which is period-correct for a machine
     that predates it. We target XP-era hardware, where the port exists.
   ⛔ BIT 0 IS A RESET AND IS COUNTED, NOT OBEYED -- same reasoning as the 8042's
     FEh: a VDD cannot reboot the machine it is a guest on. */
static void syscon_in(void *self, uint16_t port, uint8_t w, uint32_t *val)
{
    input_state *st = (input_state *)self; (void)port; (void)w;
    *val = (uint8_t)(vdd_input_a20_get(st) ? 0x02 : 0x00);
}
static void syscon_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    input_state *st = (input_state *)self; (void)port; (void)w;
    if (v & 0x01) st->kbc_reset_asked++;
    vdd_input_a20_set(st, (v & 0x02) ? 1 : 0);
}

/* IN 0x60 = keyboard data (pop one scancode; re-reads see the last byte).
   IN 0x64 = 8042 status: bit0 (OBF) set while a scancode waits. Writes to
   either (LED/8042 commands) are accepted and ignored. */
static void kbd_hw_in(void *self, uint16_t port, uint8_t w, uint32_t *val)
{
    input_state *st = (input_state *)self;
    (void)w;
    if (port == 0x64) {                       /* status register                    */
        /* ── ★★★ SEVEN OF THE EIGHT STATUS BITS USED TO BE ZERO. ──────────────
             We answered OBF and nothing else. The one that matters is bit 2, SYS,
             which POST sets on any machine DOS is running on; bit 3 (A2) says the
             last write went to 64h rather than 60h, and bit 4 (INH) says the
             keyboard is not inhibited.
           ★ ALL THREE ORACLES ANSWER 0x1C for the idle status with OBF and AUXB
             masked out (p_kbc kbc.status.idle) -- 6.22 under QEMU, dosbox-x AND
             PCem. Unanimous, so no judgement was required; we answered 0x00.
           ⚠ IBF (bit 1) STAYS 0 ON PURPOSE. The canonical driver loop is "wait
             until IBF is clear, then write", so a constant 0 means every write is
             accepted at once. We have no transfer delay to model and inventing
             one would only make drivers wait. Recorded in inventory/kbc.md as
             N/A-by-luck rather than left to be rediscovered. */
        uint8_t status = 0x14;                /* SYS (POST done) + INH (not held) */
        if (st->kbc_reply_rdy || sc_avail(st)) status |= 0x01;   /* OBF           */
        if (st->kbc_last_was_cmd)              status |= 0x08;   /* A2            */
        *val = status;
        return;
    }
    /* port 0x60: data register.
       ⛔ A CONTROLLER REPLY COMES OUT BEFORE ANY SCANCODE, and it is kept in its
          own byte rather than in sc_last. On the real part they share one output
          buffer; keeping them apart here is what stops a discarded command handing
          the guest a KEYSTROKE where it asked for the output port -- which is
          exactly what we used to do, and what a driver reads bit 1 of as A20. */
    if (st->kbc_reply_rdy) {
        st->kbc_reply_rdy = 0;
        *val = st->kbc_reply;
        return;
    }
    st->p60_reads++;
    if (sc_avail(st)) {
        (void)sc_pop(st);
        st->sc_bios_owed = 1;               /* the BIOS arm may still chain and want it */
        /* Still more queued? The controller presents the next byte and re-asserts the
           line -- after the keyboard's transfer time (poll), or at once with no clock --
           so the guest gets exactly one interrupt per scancode, at its own pace. */
        vdd_input_poll(st);
    } else if (st->sc_head != st->sc_tail) {
        st->sc_held_reads++;                /* re-read inside the hold: same byte    */
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
    if (!st) return;
    st->kbd_out_writes++;
    if (st->kbd_out_n < 16) {
        st->kbd_out_log[st->kbd_out_n][0] = (uint8_t)(port & 0xFF);
        st->kbd_out_log[st->kbd_out_n][1] = b;
        st->kbd_out_n++;
    }
    st->kbc_last_was_cmd = (uint8_t)(port == 0x64);      /* status bit 3 (A2)      */

    if (port == 0x64) {
        /* ── 8042 COMMANDS. They used to be counted and dropped. ───────────────
             ★ PCem, on a real AMI BIOS, answers all of these; 6.22-under-QEMU and
               dosbox-x answer none of them (p_kbc kbc.selftest.55, kbc.outport.d0).
               The period-correct machine is the one with the feature here. */
        st->kbc_cmd = 0;
        switch (b) {
        case 0xAA: kbc_reply(st, 0x55); break;      /* self test passed           */
        case 0xAB: kbc_reply(st, 0x00); break;      /* interface test: no error   */
        case 0x20: kbc_reply(st, st->kbc_cmdbyte); break;
        case 0xD0: kbc_reply(st, st->kbc_outport); break;
        case 0x60: case 0xD1:                        /* a parameter byte follows   */
        case 0xD2:                                   /* #244: write kbd output buf */
            st->kbc_cmd = b; break;
        case 0xAD: st->kbc_cmdbyte |= 0x10; break;   /* disable keyboard clock     */
        case 0xAE: st->kbc_cmdbyte &= (uint8_t)~0x10; break;
        case 0xA7: st->kbc_cmdbyte |= 0x20; break;   /* disable aux clock          */
        case 0xA8: st->kbc_cmdbyte &= (uint8_t)~0x20; break;
        /* ⛔ FEh PULSES THE CPU RESET LINE, and we do not have one to pulse. It is
             COUNTED rather than obeyed: a VDD cannot reboot the machine it is a
             guest on, and pretending otherwise would be worse than the count. The
             guest sees no reset and the run says it was asked for. */
        case 0xFE: st->kbc_reset_asked++; break;
        default: break;
        }
        return;
    }

    /* port 0x60: either the parameter of an 8042 command, or a KEYBOARD command. */
    if (st->kbc_cmd == 0x60) { st->kbc_cmdbyte = b; st->kbc_cmd = 0; return; }
    if (st->kbc_cmd == 0xD2) {
        /* ── #244: D2h, WRITE KEYBOARD OUTPUT BUFFER. The byte comes out at port 60h
             exactly as if the keyboard had sent it, IRQ1 included -- which is what
             makes the BIOS's INT 09h path testable without a finger on a key (p_kbd3
             injects through it). PS/2-class controllers and AMI's KBC have it; the
             original AT 8042 did not (IBM PS/2 TR "Keyboard/Auxiliary Device
             Controller"; RBIL PORTS.LST 64h D2h). ⚠ The byte bypasses set-2 -> set-1
             translation on real parts; ours is set 1 throughout, so it goes straight
             into the scancode FIFO. Which oracles answer it is the probe's question. */
        st->kbc_cmd = 0;
        vdd_input_push_scancode(st, b);
        return;
    }
    if (st->kbc_cmd == 0xD1) {
        /* ── ★★★ THE OUTPUT PORT, WHICH IS WHERE A20 LIVES. ───────────────────
             Bit 1 is the A20 gate and bit 0 is CPU reset, active low. A20 is
             written straight through to the one flag the whole host shares --
             see vdd_input_a20 -- because THE THREE DOORS ARE ONE WIRE: a guest
             that opens the gate here and then asks XMS "is A20 on" has to be
             told yes, or it concludes the machine cannot do XMS at all. */
        st->kbc_cmd = 0;
        if (!(b & 0x01)) st->kbc_reset_asked++;      /* bit 0 low = reset request  */
        st->kbc_outport = (uint8_t)(b | 0x01);       /* we never actually reset    */
        return;
    }
    if (st->kbd_expect_rate) {              /* the byte after 0xF3 is the rate     */
        st->kbd_typematic_byte = b;
        st->kbd_typematic_set  = 1;
        st->kbd_expect_rate    = 0;
    } else if (b == 0xF3) {
        st->kbd_expect_rate = 1;
    }
}

/* ── #188: WHAT AN 83-KEY PROGRAM IS ALLOWED TO SEE. ───────────────────────────────
     AH=00h/01h are the pre-101-key calls, and IBM's BIOS filters the ring for them
     (the K1S translation in the PC/AT BIOS listing): a code only an enhanced keyboard
     can make is DISCARDED -- consumed, head moved on -- and the gray-key E0 forms are
     rewritten to their 83-key equivalents. AH=10h/11h see everything unaltered.
     Measured (p_kbd 16.01.enh, F11 = 8500h): PCem's genuine AMI BIOS answers "empty"
     with the head advanced past it; QEMU's SeaBIOS hands 8500h back. The genuine ROM
     is the reference. Returns 0 = discard, 1 = deliver *key (possibly rewritten). */
static int kb_compat(uint16_t *key)
{
    uint8_t sc = (uint8_t)(*key >> 8), ch = (uint8_t)*key;
    if (sc == 0xE0) {                       /* keypad Enter / keypad '/'            */
        *key = (uint16_t)(((ch == 0x0D || ch == 0x0A) ? 0x1C00 : 0x3500) | ch);
        return 1;
    }
    if (sc > 0x84) return 0;                /* F11/F12, Ctrl+arrows, Alt+Enter ...  */
    if (ch == 0xF0) return sc == 0 ? 1 : 0; /* fill-ins; 00F0 is Alt+keypad 240     */
    if (ch == 0xE0 && sc != 0) *key = (uint16_t)(sc << 8);   /* gray arrows etc.   */
    return 1;
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
    case 0x02: case 0x12: st->int16_calls[2]++; break;
    default:              st->int16_calls[3]++; break;
    }
    switch (r_ah(r)) {
    case 0x00:                              /* read key (host blocks on empty)    */
        r->zf = 1;                          /* #188: enhanced-only codes discarded */
        while (vdd_input_pop(st, &key))
            if (kb_compat(&key)) { s_ax(r, key); r->zf = 0; break; }
        break;
    case 0x10:                              /* read key, enhanced (101-key)        */
        if (vdd_input_pop(st, &key)) { s_ax(r, key); r->zf = 0; }
        else r->zf = 1;
        break;
    case 0x01:                              /* check key (non-blocking)           */
        r->zf = 1;                          /* ZF=1 => no key (QB's INKEY$ -> "") */
        while (vdd_input_peek(st, &key)) {  /* #188: a discard CONSUMES the entry  */
            if (kb_compat(&key)) { s_ax(r, key); r->zf = 0; break; }
            (void)vdd_input_pop(st, &key);
        }
        break;
    case 0x11:                              /* check key, enhanced (101-key)       */
        if (vdd_input_peek(st, &key)) { s_ax(r, key); r->zf = 0; }
        else r->zf = 1;
        break;
    case 0x02:                              /* shift status, from 0040:0017        */
        s_al(r, kb_flags(st)); r->zf = 0;
        break;
    case 0x12: {                            /* extended shift status                  */
        /* ── #254: AH IS ITS OWN LAYOUT, NOT A COPY OF 0040:0018. ─────────────────────
             AH: 0 LCtrl 1 LAlt 2 RCtrl 3 RAlt 4 Scroll 5 Num 6 Caps 7 SysReq (held).
             0018 has SysReq at bit 2, Pause at 3 and Insert at 7; the right-hand keys
             live in 0096 bits 2/3. This copied 0018 whole, which nothing wrote. */
        uint8_t f1 = kbf(st, BDA_KB_FLAGS2), f3 = kbf(st, BDA_KB_FLAGS3);
        s_al(r, kb_flags(st));
        s_ah(r, (uint8_t)((f1 & 0x73) | (f3 & 0x0C) | ((f1 & KF1_SYSRQ) ? 0x80 : 0)));
        r->zf = 0;
        break; }
    case 0x03:                              /* set typematic rate/delay (AL=05)    */
        /* There is nothing to store: the repeat rate is the host OS's, and the BIOS
           keeps no readable copy of it. What matters is that the call is ANSWERED --
           measured on 6.22 (p_kbd.asm 16.03.typematic): AX unchanged, CF=0. A guest
           that sets the rate and gets an error back can conclude the BIOS is not
           there at all. */
        r->cf = 0; r->zf = 0;
        break;
    case 0x05:                              /* push a keystroke: CH=scan CL=ascii   */
        /* ★ THE WRITE SIDE OF THE RING, and it was missing entirely -- the `default`
             arm below left AX exactly as the caller passed it, so a program read its
             own byte back and called it success (p_kbd.asm caught it only once the
             probe POISONED AL; without the poison the row was a false match).
             This is how DOSKEY, installers that pre-answer their own prompts, and
             every key-stuffing TSR put keys in. Oracle: AL=0 stored, AL=1 full. */
        s_al(r, (uint8_t)(vdd_input_push(st, r_cx(r)) ? 0x00 : 0x01));
        r->cf = 0; r->zf = 0;
        break;
    case 0x09:                              /* which INT 16h functions exist -> AL  */
        /* #188: 0xB1, MEASURED on PCem's genuine AMI BIOS (DOSBox-X agrees); 0x30 was
           QEMU's SeaBIOS. Bits: 0 = 0300h default rate, 4 = 0Ah keyboard ID (below),
           5 = 10h-12h enhanced, 7 = as the AMI ROM sets it. */
        s_al(r, 0xB1);
        r->cf = 0; r->zf = 0;
        break;
    case 0x0A:                              /* #188: get keyboard ID -> BX          */
        /* 41ABh = an MF2 (101/102-key) keyboard behind a translating 8042, the ID
           bit 4 of AH=09h promises. */
        s_bx(r, 0x41AB);
        r->cf = 0; r->zf = 0;
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
    st->sc_hold_until = 0; st->sc_irq_up = 0;
    st->ext_pending = 0;
    st->e1_pending = 0;
    /* ── THE CONTROLLER'S POST STATE. ────────────────────────────────────────
         A machine DOS is running on has been through POST, so: the keyboard
         interrupt and translation are enabled in the command byte, and the
         output port has the reset line HIGH (bit 0 -- it would be resetting
         otherwise) and **A20 ALREADY OPEN**, which is what every BIOS since the
         AT leaves behind. Coming up with A20 shut would make our own reset a
         guest-visible event that no real machine has. */
    st->kbc_cmd = 0; st->kbc_reply = 0; st->kbc_reply_rdy = 0;
    /* ⚠ A2 (status bit 3) STARTS SET, and it took the probe to notice. It says
         "the last write went to 64h rather than 60h", and on a machine DOS is
         running on that write was POST's own last command to the controller --
         all three oracles read 0x1C at probe start, we read 0x14, and the single
         missing bit was this one. A reset that leaves it clear is claiming the
         last thing anyone wrote was keyboard data, which has never been true. */
    st->kbc_last_was_cmd = 1;
    st->kbc_cmdbyte = 0x45;                 /* IRQ1 on, translation on, SYS set   */
    st->kbc_outport = 0x03;                 /* reset line high, A20 enabled       */
    if (st->bda) {                          /* an empty ring is head==tail at its start */
        bda_w16(st, BDA_KB_HEAD, BDA_KB_START);
        bda_w16(st, BDA_KB_TAIL, BDA_KB_START);
        st->bda[BDA_KB_FLAGS]  = 0;
        st->bda[BDA_KB_FLAGS2] = 0;
        st->bda[BDA_KB_ALTNUM] = 0;         /* #274 */
        /* #274: POST's ring bounds, which push/pop/peek now read (vdd_input.h). */
        bda_w16(st, BDA_KB_BUFSTART, BDA_KB_START);
        bda_w16(st, BDA_KB_BUFEND,   BDA_KB_END);
        /* 0040:0096 bit 4 = "enhanced (101/102-key) keyboard present". It is what a
           program checks before it uses INT 16h AH=10h/11h and the F11/F12 and grey
           key codes -- edit.com and QBasic among them. We serve those functions, so
           say so; a zero here makes them fall back to the 83-key subset. */
        st->bda[0x96] = (uint8_t)((st->bda[0x96] & 0xF0) | 0x10);   /* #254: E0/E1/RCtrl/RAlt clear */
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
    if (vdd_claim_ports(b, 0x92, 0x92, syscon_in, syscon_out, st)) return -1;  /* A20    */
    return 0;
}
