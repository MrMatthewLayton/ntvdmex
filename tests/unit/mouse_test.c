/* mouse_test.c -- off-VM battery for the INT 33h driver's pure logic (src/host/i33_driver.h).
 *
 * GH #264 (09h: the guest graphics cursor) and #265 (2Bh-2Eh/33h profile and settings
 * blocks; 18h/19h alternate handlers and which handler an event goes to). The header is
 * the code the host runs; these checks pin the arithmetic so a later edit cannot quietly
 * change which pixel a mask bit lands on or which handler a Shift-click reaches.
 *
 * ⚠ What is checked here is OUR READING of RBIL (see the header). Where that reading is
 *   unmeasured the check says so in its name; tests/probes/dos/p_mouse3.asm is the probe
 *   that asks the oracles.
 *
 *   cc -std=c99 -I src/host -o mouse_test tests/unit/mouse_test.c   (or ./scripts/offvm.sh mouse)
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../src/host/i33_driver.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

static int all(const uint8_t *p, int n, uint8_t v)
{ int i; for (i = 0; i < n; ++i) if (p[i] != v) return 0; return 1; }

int main(void)
{
    uint8_t row[40];
    static const uint8_t cga[4] = { 0, 11, 13, 15 };        /* render_cga, palette 1 */

    printf("== INT 33h driver logic (i33_driver.h) ==\n");

    /* ---- 09h, one bit per pixel: AND then XOR, bit 15 leftmost ---- */
    memset(row, 0xA5, sizeof row);
    I33GraphicsCursorRow(row, 40, 8, 0xFFFF, 0x0000, 0x0F, NULL);
    CHECK(all(row, 40, 0xA5), "screen FFFF / cursor 0000: transparent, the row untouched");
    I33GraphicsCursorRow(row, 40, 8, 0x0000, 0x0000, 0x0F, NULL);
    CHECK(all(row + 8, 16, 0x00) && all(row, 8, 0xA5) && all(row + 24, 16, 0xA5),
          "screen 0000 / cursor 0000: exactly 16 pixels cleared, nothing either side");
    memset(row, 0xA5, sizeof row);
    I33GraphicsCursorRow(row, 40, 8, 0x0000, 0xFFFF, 0x0F, NULL);
    CHECK(all(row + 8, 16, 0x0F), "screen 0000 / cursor FFFF: 0 XOR 0Fh = 0Fh (DOSBox's 'ones'; UNMEASURED on MS)");
    memset(row, 0xA5, sizeof row);
    I33GraphicsCursorRow(row, 40, 8, 0xFFFF, 0xFFFF, 0x0F, NULL);
    CHECK(all(row + 8, 16, 0xAA), "screen FFFF / cursor FFFF: the pixel inverted (A5h ^ 0Fh = AAh)");
    memset(row, 0xA5, sizeof row);
    I33GraphicsCursorRow(row, 40, 8, 0x00FF, 0x0000, 0x0F, NULL);
    CHECK(all(row + 8, 8, 0x00) && all(row + 16, 8, 0xA5),
          "screen 00FF: the LEFT eight cleared -- bit 15 is the leftmost pixel (RBIL: 'low bit rightmost')");
    memset(row, 0x03, sizeof row);
    I33GraphicsCursorRow(row, 40, 8, 0xFFFF, 0x8001, 0x0F, NULL);
    CHECK(row[8] == 0x0C && row[23] == 0x0C && all(row + 9, 14, 0x03),
          "cursor 8001h: only the first and sixteenth pixels flip");

    /* ---- clipping: a bitmap hanging off either edge writes nothing outside ---- */
    memset(row, 0x11, sizeof row);
    I33GraphicsCursorRow(row + 4, 8, -4, 0x0000, 0x0000, 0x0F, NULL);   /* an 8-wide row at row+4 */
    CHECK(all(row, 4, 0x11) && all(row + 4, 8, 0x00) && all(row + 12, 28, 0x11),
          "x0 = -4 on an 8-pixel row: the visible part drawn, not a byte outside");

    /* ---- CGA 4-colour: 8 pixels, a bit PAIR each, through the renderer's palette ---- */
    memset(row, 0, sizeof row);
    for (int i = 0; i < 8; ++i) row[i] = cga[2];             /* colour 2 everywhere */
    I33GraphicsCursorRow(row, 40, 0, 0xFFFF, 0x0000, 0x0F, cga);
    CHECK(all(row, 8, cga[2]), "CGA: screen FFFF keeps the 2-bit colour (mapped back through the palette)");
    I33GraphicsCursorRow(row, 40, 0, 0x0000, 0xC000, 0x0F, cga);
    CHECK(row[0] == cga[3] && all(row + 1, 7, cga[0]),
          "CGA: cursor C000h = colour 3 in pixel 0 only, screen 0000 clears the other seven");
    for (int i = 0; i < 8; ++i) row[i] = cga[1];
    I33GraphicsCursorRow(row, 40, 0, 0xAAAA, 0x0000, 0x0F, cga);         /* AND 10b: 01 -> 00 */
    CHECK(all(row, 8, cga[0]) && row[8] == 0,
          "CGA: AND per bit pair (01 & 10 = 00), and only eight pixels wide (UNMEASURED: MS Programmer's Ref.)");

    /* ---- the whole bitmap: hot spot places it, the default arrow's tip ---- */
    {   static uint8_t fb[32 * 32];
        memset(fb, 7, sizeof fb);
        I33GraphicsCursorDraw(fb, 32, 32, 32, 10, 10, 0, 0, g_I33DefaultScreenMask, g_I33DefaultCursorMask, 0x0F, NULL);
        CHECK(fb[10 * 32 + 10] == 0 && fb[10 * 32 + 12] == 7,
              "default arrow, hot spot 0,0: row 0 = 3FFFh -> two black pixels at the pointer, then the screen");
        CHECK(fb[11 * 32 + 11] == 0x0F, "default arrow row 1: 4000h -> white (0Fh) one pixel in");
        memset(fb, 7, sizeof fb);
        I33GraphicsCursorDraw(fb, 32, 32, 32, 10, 10, 4, 2, g_I33DefaultScreenMask, g_I33DefaultCursorMask, 0x0F, NULL);
        /* origin (6,8): row 0 (3FFFh) clears x=6,7; row 2 (0FFFh / 6000h) at y=10 clears
           x=6..9, whitens x=7,8, keeps x=10. */
        CHECK(fb[8 * 32 + 6] == 0 && fb[8 * 32 + 7] == 0 && fb[8 * 32 + 8] == 7 && fb[8 * 32 + 5] == 7,
              "hot spot (4,2): the bitmap's origin is 4 left and 2 up of the pointer (screen pixels; UNMEASURED)");
        CHECK(fb[10 * 32 + 7] == 0x0F && fb[10 * 32 + 9] == 0 && fb[10 * 32 + 10] == 7,
              "hot spot (4,2): row 2 lands on the pointer's row");
        memset(fb, 7, sizeof fb);
        I33GraphicsCursorDraw(fb, 32, 32, 32, 0, 0, 8, 8, g_I33DefaultScreenMask, g_I33DefaultCursorMask, 0x0F, NULL);
        /* origin (-8,-8): bitmap row 8 (003Fh / 7F80h) is frame row 0, column 8 is x=0. */
        CHECK(fb[0] == 0x0F && fb[1] == 0 && fb[2] == 7,
              "hot spot pulling the bitmap off the top-left: the visible quarter drawn, clipped");
        CHECK(fb[31 * 32 + 31] == 7 && fb[8 * 32 + 8] == 7, "...and nothing written past it");
    }

    /* ---- 2Bh-2Eh / 33h: the block layouts (RBIL #03182, #03184) ---- */
    {   uint8_t acc[I33_ACC_LEN], out[I33_SET_LEN + 8];
        I33_SETTINGS st = { 4, 0, 50, 50, 50, 1, 3 };
        CHECK(I33_ACC_LEN == 324 && I33_SET_LEN == 340, "profile block 144h bytes, settings block 154h");
        CHECK(I33_ACC_THRESH == 4 && I33_ACC_FACTOR == 0x84 && I33_ACC_NAMES == 0x104,
              "offsets: thresholds 04h, factors 84h, names 104h");
        memset(acc, 0xEE, sizeof acc);
        I33AccelerationDefaults(acc);
        CHECK(acc[0] == 1 && acc[3] == 1, "defaults: each profile one entry long");
        CHECK(all(acc + I33_ACC_THRESH, 128, 0x7F), "defaults: thresholds 7Fh (RBIL's 'unused')");
        CHECK(all(acc + I33_ACC_FACTOR, 128, 0x10), "defaults: factors 10h = 1.0 -- what this driver applies");
        CHECK(memcmp(acc + I33_ACC_NAMES + 16, "Moderate        ", 16) == 0,
              "defaults: profile 2's name, blank-padded to 16 (names UNMEASURED)");
        memset(out, 0xEE, sizeof out);
        CHECK(I33SettingsBlock(out, sizeof out, &st, acc) == I33_SET_LEN && out[I33_SET_LEN] == 0xEE,
              "33h: a big buffer gets 154h bytes and not one more");
        CHECK(out[0] == 4 && out[2] == 50 && out[5] == 1 && out[6] == 3 && out[7] == 0 && out[0x0F] == 0,
              "33h header: type, sensitivities, curve, rate; the fields we have no notion of are 0");
        CHECK(memcmp(out + 0x10, acc, I33_ACC_LEN) == 0, "33h: the profile block follows at 10h");
        memset(out, 0xEE, sizeof out);
        CHECK(I33SettingsBlock(out, 5, &st, acc) == 5 && out[4] == 50 && out[5] == 0xEE,
              "33h: CX=5 gets the first five bytes, count 5 -- truncation, not an error");
    }

    /* ---- 18h/19h: install, replace, refuse ---- */
    {   I33_ALTERNATE a[I33_ALT_N]; unsigned ax; int who;
        memset(a, 0, sizeof a);
        CHECK(!I33AlternateSet(a, 0x001F, 0x1000, 0x10), "18h: no Shift/Ctrl/Alt bit -> refused (RBIL: 'at least one of bits 5-7')");
        CHECK(I33AlternateSet(a, 0x0022, 0x1000, 0x10), "18h: Shift + left press -> installed");
        CHECK(I33AlternateSet(a, 0x0044, 0x2000, 0x20), "18h: Ctrl + left release -> installed");
        CHECK(I33AlternateSet(a, 0x0062, 0x3000, 0x30), "18h: Shift+Ctrl -> a third slot");
        CHECK(!I33AlternateSet(a, 0x0082, 0x4000, 0x40), "18h: a fourth combination (Alt) -> refused");
        CHECK(I33AlternateSet(a, 0x0026, 0x1111, 0x11) && a[0].Segment == 0x1111 && a[0].Mask == 0x0026,
              "18h: the same combination again REPLACES its slot (UNMEASURED reading)");
        CHECK(I33AlternateFind(a, 0x0040) == 1 && I33AlternateFind(a, 0x0080) == -1 && I33AlternateFind(a, 0x0003) == -1,
              "19h: found by the shift bits; none for Alt; a mask with no shift bits finds nothing");

        /* who gets the event: kbflags 0040:0017 bit 0/1 Shift, 2 Ctrl, 3 Alt */
        who = I33PickHandler(a, 0x02, 0x00, 0x7F, &ax);
        CHECK(who == -1 && ax == 0x02, "no key held: the 0Ch handler, AX = the event");
        who = I33PickHandler(a, 0x02, 0x02, 0x7F, &ax);
        CHECK(who == 0 && ax == 0x22, "left Shift held, left press: the Shift handler, AX = event | 20h");
        who = I33PickHandler(a, 0x02, 0x01, 0x7F, &ax);
        CHECK(who == 0 && ax == 0x22, "RIGHT Shift is Shift too");
        who = I33PickHandler(a, 0x02, 0x06, 0x7F, &ax);
        CHECK(who == 2 && ax == 0x62, "Shift+Ctrl held: the Shift+Ctrl handler, not the Shift one (exact match; UNMEASURED)");
        who = I33PickHandler(a, 0x01, 0x02, 0x7F, &ax);
        CHECK(who == -1 && ax == 0x01, "Shift held but the Shift handler did not ask for motion: 0Ch's");
        who = I33PickHandler(a, 0x02, 0x08, 0x7F, &ax);
        CHECK(who == -1 && ax == 0x02, "Alt held, no Alt handler: 0Ch's");
        who = I33PickHandler(a, 0x20, 0x02, 0x7F, &ax);
        CHECK(who == -1 && ax == 0x20,
              "middle press (0Ch bit 5) with Shift held is NOT an alternate event -- bit 5 means Shift there");
        who = I33PickHandler(a, 0x02, 0x00, 0x00, &ax);
        CHECK(who == -2, "no key, no 0Ch handler: nobody");
        CHECK(I33ShiftBits(0x0F) == 0xE0 && I33ShiftBits(0x10) == 0,
              "shift bits: Shift/Ctrl/Alt fold to 20h/40h/80h; Scroll Lock is not one");
        CHECK(I33AlternateAny(a), "alt_any with handlers installed");
        memset(a, 0, sizeof a);
        CHECK(!I33AlternateAny(a), "alt_any after a reset");
    }

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
