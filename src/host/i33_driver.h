/* i33_driver.h -- the INT 33h driver's pure logic: the guest graphics cursor (09h),
 * the acceleration-profile / settings blocks (2Bh-2Eh, 33h) and the shift-qualified
 * alternate handlers (18h/19h). GH #264, #265.
 *
 * Header-only and dependency-free (stdint only: no windows.h, no string.h -- the host
 * is built -nostdlib), so the off-VM battery
 * (tests/unit/mouse_test.c) compiles exactly the code the host runs. The register
 * plumbing -- which register carries what, ES:DX vs a PM selector -- stays in
 * mouse_int33 (main.c); nothing here touches a guest.
 *
 * ── SPEC, AND HOW MUCH OF IT IS MEASURED ────────────────────────────────────────────
 *   RBIL INT 33h (MS Mouse 6.0+ for 18h/19h, 7.0+ for 2Bh-2Dh, 7.05+ for 33h, 8.0+
 *   for 34h, 8.10+ for 2Eh) and RBIL's tables #03168 (cursor bitmap), #03176 (the
 *   alternate call mask), #03182 (acceleration profile data), #03184 (settings).
 *   ⚠ UNMEASURED unless a row says otherwise. The 6.22 oracle runs MOUSE.COM 6.24,
 *   which predates 2Bh-34h; DOSBox-X is an emulator's opinion. tests/probes/dos/
 *   p_mouse3.asm asks every question below that a probe can ask, so the first oracle
 *   run turns these comments into measurements.
 */
#ifndef NTVDMEX_I33_DRIVER_H
#define NTVDMEX_I33_DRIVER_H

#include <stdint.h>

/* ══ 09h: THE GRAPHICS CURSOR ════════════════════════════════════════════════════════
     ES:DX -> 16 words of SCREEN mask then 16 words of CURSOR mask (RBIL #03168: "each
     word defines the sixteen pixels of a row, low bit rightmost" -- so bit 15 is the
     LEFTMOST pixel). The driver ANDs the screen under the pointer with the screen mask
     and then XORs the cursor mask into it; the hot spot (BX,CX, -16..16) is the bitmap
     point that sits on the pointer position.
   ► PER PIXEL, the AND keeps the pixel where the screen bit is 1 and clears it to 0
     where it is 0 -- a mask bit stands for EVERY bit of that pixel (all four planes in
     a 16-colour mode, both bits of a CGA pixel pair). The XOR bit likewise flips every
     bit the pixel has: `ones` below.
   ⚠ HOW MANY BITS A 256-COLOUR PIXEL HAS, AS FAR AS THE DRIVER IS CONCERNED, IS
     UNMEASURED. DOSBox's driver XORs 0Fh in every mode (mouse.cpp DrawCursor: `pixel ^
     0x0F`), which keeps the default arrow white (index 15) in mode 13h on the default
     palette; a driver that expanded the bit to FFh would draw it in index 255, black on
     that palette. We follow DOSBox (0Fh); p_mouse3's `i33.09.13h.row2` reads the answer
     out of the oracle's VRAM.
   ⚠ CGA 4-COLOUR (04h/05h) IS 2 BITS PER PIXEL, so a 16-bit mask word covers EIGHT
     screen pixels, each bit PAIR (high pair leftmost) one pixel's AND/XOR -- the shape a
     driver gets for free by applying the word straight to the packed video bytes, and
     what the MS Mouse Programmer's Reference describes for modes 4/5 (from memory, not
     held in the repo: UNMEASURED). Every other mode is one bit per pixel, 16 wide.
   ⚠ THE HOT SPOT IS IN SCREEN PIXELS (DOSBox: `POS_X/xratio - hotx`), i.e. NOT in the
     driver's doubled 640-wide virtual X of a 320-wide mode. In CGA 4-colour a hot spot
     counts mask BITS, so it is halved into pixels. Both UNMEASURED; p_mouse3's
     `i33.09.13h.hot.*` rows read where the oracle put the bitmap. */
#define I33_GC_ROWS 16

/* One row of the cursor over one row of the FRAME (palette indices, as the presenter's
   8-bpp snapshot holds them). `x0` = the frame column of the bitmap's left edge, which
   may be negative or run past `w`: clipped here, never trusted.
   map4 == NULL: 1 bit per pixel, 16 pixels; v' = (s ? v : 0) ^ (c ? ones : 0).
   map4 != NULL: CGA 4-colour, 2 bits per pixel, 8 pixels. The frame holds the RENDERED
     colour (render_cga maps the 2-bit value through its palette), so the 2-bit value is
     recovered through map4 first, masked, and mapped back -- the mask is applied to
     what the video memory holds, not to what the presenter shows. A frame value map4
     does not contain (nothing the CGA renderer writes) is taken as colour 0. */
static void i33_gc_row(uint8_t *row, int w, int x0, uint16_t scr, uint16_t cur,
                       uint8_t ones, const uint8_t *map4)
{
    int i;
    if (!map4) {
        for (i = 0; i < 16; ++i) {
            int x = x0 + i;
            uint16_t bit = (uint16_t)(0x8000u >> i);
            uint8_t v;
            if (x < 0 || x >= w) continue;
            v = (scr & bit) ? row[x] : 0;
            if (cur & bit) v = (uint8_t)(v ^ ones);
            row[x] = v;
        }
        return;
    }
    for (i = 0; i < 8; ++i) {
        int x = x0 + i, k;
        unsigned sh = (unsigned)(14 - 2 * i);
        unsigned s2 = (scr >> sh) & 3u, c2 = (cur >> sh) & 3u, v2 = 0;
        if (x < 0 || x >= w) continue;
        for (k = 0; k < 4; ++k) if (map4[k] == row[x]) { v2 = (unsigned)k; break; }
        v2 = (v2 & s2) ^ c2;
        row[x] = map4[v2 & 3u];
    }
}

/* The whole bitmap. (px,py) = the pointer in frame pixels, (hx,hy) = 09h's hot spot. */
static void i33_gc_draw(uint8_t *pix, int w, int h, int stride, int px, int py,
                        int hx, int hy, const uint16_t *scr, const uint16_t *cur,
                        uint8_t ones, const uint8_t *map4)
{
    int r;
    int x0 = px - (map4 ? hx / 2 : hx);
    int y0 = py - hy;
    if (!pix || w <= 0 || h <= 0) return;
    for (r = 0; r < I33_GC_ROWS; ++r) {
        int y = y0 + r;
        if (y < 0 || y >= h) continue;
        i33_gc_row(pix + (long)y * stride, w, x0, scr[r], cur[r], ones, map4);
    }
}

/* The MS driver's default arrow, as DOSBox carries it (mouse.cpp defaultScreenMask /
   defaultCursorMask -- transcribed from memory, not from a file in the repo). NOT what we draw after a
   reset -- the host draws its own artwork arrow until a guest defines a shape (main.c
   MS_CURSOR) -- but it is what the test proves the mask arithmetic against, and the
   shape a guest gets if it hands back what it never set. */
static const uint16_t I33_GC_DEF_SCR[16] = {
    0x3FFF, 0x1FFF, 0x0FFF, 0x07FF, 0x03FF, 0x01FF, 0x00FF, 0x007F,
    0x003F, 0x001F, 0x01FF, 0x00FF, 0x30FF, 0xF87F, 0xF87F, 0xFCFF };
static const uint16_t I33_GC_DEF_CUR[16] = {
    0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00,
    0x7F80, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000 };

/* ══ 2Bh-2Eh, 33h: ACCELERATION PROFILES AND THE SETTINGS BLOCK ═════════════════════
     RBIL #03182, the profile data -- 324 (144h) bytes, one block, four profiles:
       00h  4 BYTEs   length of profiles 1-4 (entries used of the 32)
       04h  4x32      threshold speeds, profile 1..4 (unused = 7Fh)
       84h  4x32      speedup factors,  profile 1..4 (10h = 1.0, 14h = 1.25, 20h = 2.0;
                      unused = 10h)
      104h  4x16      profile names, blank-padded
   ⛔ ACCELERATION IS NOT APPLIED, and that is the same decision as 0Fh/13h/1Ah (see
     docs/inventory/mouse.md): the pointer is the HOST's, with Windows' own ballistics,
     and 0Bh reports device counts scaled by msens. So the block is STORED and handed
     back exactly as loaded -- 2Bh in, 2Ch/33h out -- and selecting a profile changes
     which number 2Ch/2Dh report, not how the mouse moves.
   ⚠ THE DEFAULT CURVES ARE OURS, NOT MICROSOFT'S (UNMEASURED). Four profiles of one
     entry each, threshold 7Fh, factor 10h -- i.e. "1.0 at every speed", which is the
     truth about what this driver applies. The names are MS Mouse 8.x/9.x's
     control-panel names (from memory). p_mouse3 dumps the oracle's 2Ch block so the
     defaults can be replaced by measured ones the day an 8.x driver answers. */
#define I33_ACC_LEN      0x144
#define I33_ACC_LENS     0x000
#define I33_ACC_THRESH   0x004
#define I33_ACC_FACTOR   0x084
#define I33_ACC_NAMES    0x104
#define I33_ACC_NAMELEN  16
#define I33_ACC_N        4
#define I33_ACC_DEFAULT  1            /* active profile after a reset -- UNMEASURED */

static const char I33_ACC_DEFNAMES[I33_ACC_N][I33_ACC_NAMELEN + 1] = {
    "Slow            ", "Moderate        ", "Fast            ", "Unaccelerated   " };

static void i33_acc_default_names(uint8_t *names64)
{
    int p, i;
    for (p = 0; p < I33_ACC_N; ++p)
        for (i = 0; i < I33_ACC_NAMELEN; ++i)
            names64[p * I33_ACC_NAMELEN + i] = (uint8_t)I33_ACC_DEFNAMES[p][i];
}

static void i33_acc_defaults(uint8_t *acc)
{
    int p, i;
    for (p = 0; p < I33_ACC_N; ++p) {
        acc[I33_ACC_LENS + p] = 1;
        for (i = 0; i < 32; ++i) {
            acc[I33_ACC_THRESH + p * 32 + i] = 0x7F;
            acc[I33_ACC_FACTOR + p * 32 + i] = 0x10;
        }
    }
    i33_acc_default_names(acc + I33_ACC_NAMES);
}

/* RBIL #03184: 33h's buffer -- a 16-byte switch-settings header, then the profile data.
   The header bytes are filled from the state 1Ah/1Ch/24h already keep; the fields this
   driver has no notion of (laptop adjustment, memory type, SuperVGA, rotation, the
   button map, click lock) are 0. ⚠ The CODING of each byte (is 06h the 1Ch code or a
   rate in Hz; is 0Dh "left" 0 or 1) is UNMEASURED -- p_mouse3 dumps the oracle's. */
#define I33_SET_HDR      0x10
#define I33_SET_LEN      (I33_SET_HDR + I33_ACC_LEN)       /* 154h = 340 bytes */
typedef struct {
    uint8_t type;           /* 00h: as 24h's CH -- 4 = PS/2                         */
    uint8_t language;       /* 01h: as 23h -- 0 = English                           */
    uint8_t hsens, vsens;   /* 02h/03h: 1Ah's horizontal/vertical speed (0-100)     */
    uint8_t dblspd;         /* 04h: 1Ah's double-speed threshold (0-100)            */
    uint8_t curve;          /* 05h: the active acceleration profile (2Dh)           */
    uint8_t rate;           /* 06h: 1Ch's code                                      */
} i33_settings;

/* Fill `out` with at most `cap` bytes of the block; returns the count written (33h's
   CX on return). A short buffer gets the first `cap` bytes, not an error -- the call
   hands the size in and the count back, so truncation is the contract's own answer. */
static unsigned i33_settings_block(uint8_t *out, unsigned cap, const i33_settings *s,
                                   const uint8_t *acc)
{
    uint8_t b[I33_SET_LEN];
    unsigned n = cap < I33_SET_LEN ? cap : I33_SET_LEN, i;
    for (i = 0; i < I33_SET_HDR; ++i) b[i] = 0;
    b[0x00] = s->type;  b[0x01] = s->language;
    b[0x02] = s->hsens; b[0x03] = s->vsens; b[0x04] = s->dblspd;
    b[0x05] = s->curve; b[0x06] = s->rate;
    for (i = 0; i < I33_ACC_LEN; ++i) b[I33_SET_HDR + i] = acc[i];
    for (i = 0; i < n; ++i) out[i] = b[i];
    return n;
}

/* ══ 18h/19h: THE ALTERNATE (SHIFT-QUALIFIED) HANDLERS ═══════════════════════════════
     RBIL #03176, the call mask: bits 0-4 the events (motion, L press/release, R
     press/release -- NO middle button, its 0Ch bits are reused), bits 5/6/7 "call if
     Shift / Ctrl / Alt is pressed during the event". "Up to three handlers can be
     defined by separate calls to this function, each with at least one of bits 5-7
     set." 18h answers AX=0018h on success, FFFFh on error; 19h finds the handler whose
     call mask matches CX and answers BX:DX = its address, CX = its mask (0 = none).
   ► THE SHIFT BITS ARE THE KEY. A handler is identified by its shift combination: a
     second 18h with the same bits 5-7 REPLACES the first (it is the same slot), a new
     combination takes a free slot, and a fourth combination is the error. 19h matches
     on bits 5-7 for the same reason.
   ⚠ UNMEASURED, and both choices are ours: (a) "replace on the same combination" -- RBIL
     only says three may be defined; (b) 19h matching on the shift bits rather than the
     whole mask.
   ► DELIVERY: the shift state is BDA 0040:0017 (bit 0 right Shift, 1 left Shift, 2 Ctrl,
     3 Alt), folded into the mask's bit positions. The handler whose combination is
     EXACTLY the keys held, and whose event bits include the event, is called with AX =
     the event bits | the shift bits ("same bit assignments as call mask"). Otherwise
     the event goes to the 0Ch handler as before.
   ⚠ UNMEASURED: exact-match (Shift+Ctrl held does not call a Shift-only handler), and
     ONE call per event (a matching alternate handler REPLACES the 0Ch call rather than
     preceding it). Both are the readings under which a program's own event queue sees
     each event once. */
#define I33_ALT_N        3
#define I33_ALT_SHIFTS   0x00E0u
#define I33_ALT_EVENTS   0x001Fu
typedef struct { uint16_t mask; uint16_t seg; uint32_t off; } i33_alt;

static unsigned i33_shift_bits(uint8_t kbflags)
{
    return ((kbflags & 0x03) ? 0x20u : 0u) | ((kbflags & 0x04) ? 0x40u : 0u)
         | ((kbflags & 0x08) ? 0x80u : 0u);
}

/* 18h. Returns 1 = installed (AX=0018h), 0 = refused (AX=FFFFh). */
static int i33_alt_set(i33_alt *alt, uint16_t mask, uint16_t seg, uint32_t off)
{
    int i, freei = -1;
    unsigned sh = mask & I33_ALT_SHIFTS;
    if (!sh) return 0;                                  /* needs one of Shift/Ctrl/Alt */
    for (i = 0; i < I33_ALT_N; ++i) {
        if (alt[i].mask && (alt[i].mask & I33_ALT_SHIFTS) == sh) break;
        if (!alt[i].mask && freei < 0) freei = i;
    }
    if (i == I33_ALT_N) { if (freei < 0) return 0; i = freei; }
    alt[i].mask = mask; alt[i].seg = seg; alt[i].off = off;
    return 1;
}

/* 19h. Returns the slot, or -1 (CX=0). */
static int i33_alt_find(const i33_alt *alt, uint16_t mask)
{
    int i;
    unsigned sh = mask & I33_ALT_SHIFTS;
    if (!sh) return -1;
    for (i = 0; i < I33_ALT_N; ++i)
        if (alt[i].mask && (alt[i].mask & I33_ALT_SHIFTS) == sh) return i;
    return -1;
}

static int i33_alt_any(const i33_alt *alt)
{
    int i;
    for (i = 0; i < I33_ALT_N; ++i) if (alt[i].mask & I33_ALT_EVENTS) return 1;
    return 0;
}

/* WHO GETS THIS EVENT. ev = the event bits (0Ch layout: bit 5/6 are the MIDDLE button
   there), main_mask = 0Ch's call mask or 0 when no 0Ch handler is installed. Returns
   0..2 = that alternate slot, -1 = the 0Ch handler, -2 = nobody asked for it. *ax = the
   condition word the chosen handler is called with. */
static int i33_pick(const i33_alt *alt, unsigned ev, uint8_t kbflags, unsigned main_mask,
                    unsigned *ax)
{
    unsigned sh = i33_shift_bits(kbflags);
    int i;
    if (sh)
        for (i = 0; i < I33_ALT_N; ++i) {
            unsigned m = alt[i].mask;
            if (m && (m & I33_ALT_SHIFTS) == sh && (ev & m & I33_ALT_EVENTS)) {
                *ax = (ev & m & I33_ALT_EVENTS) | sh;
                return i;
            }
        }
    if (ev & main_mask) { *ax = ev & main_mask; return -1; }
    *ax = 0;
    return -2;
}

#endif /* NTVDMEX_I33_DRIVER_H */
