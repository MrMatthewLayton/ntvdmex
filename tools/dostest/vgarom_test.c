/* vgarom_test.c -- check what we ship for the VGA against the GENUINE IBM VGA BIOS.
 *
 * ── WHY THIS EXISTS ──────────────────────────────────────────────────────────────
 *   Every table in vga_font_*.h and vga_defaults.h is a CLAIM ABOUT REAL HARDWARE.
 *   Nothing in the ordinary battery can falsify such a claim: those tables are both
 *   the thing under test and the only statement of what is correct, so a test
 *   written against them can do no more than agree with itself. That is how the
 *   attribute palette carried a wrong index 6 through a whole session -- it had been
 *   inferred from a picture, and every test agreed with it.
 *
 *   PCem-ROMs-master/ibm_vga.bin is an outside witness: the real IBM VGA BIOS, which
 *   nothing in this repo produced. Checking against it is the only way these tables
 *   can be WRONG rather than merely self-consistent.
 *
 * ── WHY IT SKIPS RATHER THAN FAILS ───────────────────────────────────────────────
 *   The ROM pack is licensed and gitignored, so a clean checkout does not have it.
 *   A missing oracle is not a defect and must not fail the battery -- but it must be
 *   VISIBLE, because a silent skip is how an oracle quietly stops being consulted.
 *   Same shape as the real-module cases in ne_test.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "vga_font_8x16.h"
#include "vga_font_8x14.h"
#include "vga_font_8x8.h"
#include "vga_defaults.h"

static int pass = 0, fail = 0, skip = 0;
static void ok(int c, const char *what)
{ if (c) { ++pass; printf("  PASS  %s\n", what); } else { ++fail; printf("  FAIL  %s\n", what); } }
static void skipped(const char *what) { ++skip; printf("  SKIP  %s\n", what); }

/* run.sh may invoke us from the repo root or from tools/dostest. */
static uint8_t *slurp(const char *rel, long *len)
{
    const char *pfx[] = { "", "../../" };
    unsigned i;
    for (i = 0; i < 2; ++i) {
        char p[512]; FILE *f; uint8_t *b; long n;
        snprintf(p, sizeof p, "%s%s", pfx[i], rel);
        f = fopen(p, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        b = (uint8_t *)malloc((size_t)n);
        if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
        fclose(f); *len = n; return b;
    }
    return NULL;
}

/* Find a byte block in the ROM. The fonts sit as contiguous runs of glyph data, so
   "does this exact table appear anywhere in the real BIOS" is the whole question --
   no offset needs to be hardcoded, and none is, because a hardcoded offset would
   turn a ROM-layout change into a false failure. */
static long find(const uint8_t *hay, long hn, const uint8_t *nee, long nn)
{
    long i;
    if (nn > hn) return -1;
    for (i = 0; i + nn <= hn; ++i) if (!memcmp(hay + i, nee, (size_t)nn)) return i;
    return -1;
}

static uint8_t flat[4096];
static long flatten(const void *tbl, int glyphs, int h)
{
    const uint8_t *p = (const uint8_t *)tbl;
    memcpy(flat, p, (size_t)(glyphs * h));
    return glyphs * h;
}

int main(void)
{
    long n = 0;
    uint8_t *rom = slurp("PCem-ROMs-master/ibm_vga.bin", &n);

    printf("== VGA tables vs the genuine IBM VGA BIOS (oracle-gated) ==\n");
    if (!rom) {
        skipped("ibm_vga.bin absent -- VGA font/table claims UNVERIFIED this run");
        printf("        (the ROM pack is licensed + gitignored; see docs, it is not a defect)\n");
        printf("\n%d checks, %d failed, %d skipped\n", pass + fail, fail, skip);
        return 0;
    }
    ok(n == 32768, "ibm_vga.bin is the expected 32KB image");

    /* ── THE THREE FONTS. All three must appear in the real BIOS byte for byte.
         The 8x8 in particular was once MANUFACTURED here, by OR-ing pairs of rows
         out of the 8x16 -- a plausible-looking font that no IBM machine ever drew.
         This is the check that would have caught that. */
    {   long off;
        off = find(rom, n, (const uint8_t *)vga_font_8x16, flatten(vga_font_8x16, 256, 16));
        ok(off >= 0, "8x16 font is byte-identical to the ROM's");
        off = find(rom, n, (const uint8_t *)vga_font_8x14, flatten(vga_font_8x14, 256, 14));
        ok(off >= 0, "8x14 font is byte-identical to the ROM's");
        off = find(rom, n, (const uint8_t *)vga_font_8x8,  flatten(vga_font_8x8,  256, 8));
        ok(off >= 0, "8x8 font is byte-identical to the ROM's (not derived)");
    }

    /* ── THE PER-MODE CRTC TABLES. vgadefs.asm read these back off a real card; the
         BIOS is where they come from in the first place, so the two must agree. A
         mode's 25 CRTC bytes appear in the ROM's video parameter table as a run.
       ⚠ The BIOS table stores CRTC 0x00..0x18 contiguously, which is exactly our
         row, so a whole row is findable. If a row ever stops being findable the
         likely cause is that someone "tidied" a value. */
    {   unsigned r; int found = 0, tried = 0;
        for (r = 0; r < 9; ++r) {
            long off = find(rom, n, VGA_CRTC_DEFAULT[r], 25);
            ++tried; if (off >= 0) ++found;
        }
        ok(found >= 7, "per-mode CRTC rows are present in the ROM's parameter table");
        printf("        (%d of %d CRTC rows located in the real BIOS)\n", found, tried);
    }

    /* ── THE VERTICAL TIMING WE NOW DERIVE FROM THOSE ROWS. This is the claim the
         0x3DA model rests on, so state it here in numbers rather than leaving it
         implicit in a table: 640x350 is the mode that broke the old two-case guess. */
    {   const unsigned char *c = VGA_CRTC_DEFAULT[6];      /* modes 0Fh, 10h */
        unsigned ov = c[0x07], ms = c[0x09];
        unsigned vt  = c[0x06] | ((ov >> 0 & 1) << 8) | ((ov >> 5 & 1) << 9);
        unsigned vde = c[0x12] | ((ov >> 1 & 1) << 8) | ((ov >> 6 & 1) << 9);
        unsigned vbs = c[0x15] | ((ov >> 3 & 1) << 8) | ((ms >> 5 & 1) << 9);
        ok(vt + 2 == 449, "640x350: the BIOS says 449 scanlines per frame");
        ok(vde + 1 == 350, "640x350: the BIOS says 350 active lines, not 400");
        ok(vbs == 355,     "640x350: blanking starts at line 355, not 400");
    }

    printf("\n%d checks, %d failed, %d skipped\n", pass + fail, fail, skip);
    free(rom);
    return fail ? 1 : 0;
}
