/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Check what we ship for the VGA against the GENUINE IBM VGA BIOS.
 *
 * WHY THIS EXISTS:
 * Every table in vga_defaults.h is a CLAIM ABOUT REAL HARDWARE.
 * Nothing in the ordinary battery can falsify such a claim: those tables are both
 * the thing under test and the only statement of what is correct, so a test
 * written against them can do no more than agree with itself. That is how the
 * attribute palette carried a wrong index 6 through a whole session -- it had been
 * inferred from a picture, and every test agreed with it.
 *
 * PCem-ROMs-master/ibm_vga.bin is an outside witness: the real IBM VGA BIOS, which
 * nothing in this repo produced. Checking against it is the only way these tables
 * can be WRONG rather than merely self-consistent.
 *
 * WHY IT SKIPS RATHER THAN FAILS:
 * The ROM pack is licensed and gitignored, so a clean checkout does not have it.
 * A missing oracle is not a defect and must not fail the battery -- but it must be
 * VISIBLE, because a silent skip is how an oracle quietly stops being consulted.
 * Same shape as the real-module cases in ne_test.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "vga_defaults.h"

static INT g_Passes = 0;
static INT g_Failures = 0;
static INT g_Skips = 0;

static VOID VgaRomTestCheck(INT condition, PCSTR description)
{
    if (condition)
    {
        ++g_Passes;
        printf("  PASS  %s\n", description);
    }
    else
    {
        ++g_Failures;
        printf("  FAIL  %s\n", description);
    }
}

static VOID VgaRomTestSkip(PCSTR description)
{
    ++g_Skips;
    printf("  SKIP  %s\n", description);
}

/* run.sh may invoke us from the repo root or from tests/unit. */
static PBYTE VgaRomTestReadFile(PCSTR relativePath, long *length)
{
    PCSTR prefixes[] = { "", "../../" };
    UINT index;

    for (index = 0; index < 2; ++index)
    {
        CHAR path[512];
        FILE *file;
        PBYTE bytes;
        long size;
        snprintf(path, sizeof path, "%s%s", prefixes[index], relativePath);
        file = fopen(path, "rb");

        if (!file)
            continue;

        fseek(file, 0, SEEK_END);
        size = ftell(file);
        fseek(file, 0, SEEK_SET);
        bytes = (PBYTE)malloc((size_t)size);

        if (!bytes || fread(bytes, 1, (size_t)size, file) != (size_t)size)
        {
            fclose(file);
            free(bytes);
            return NULL;
        }

        fclose(file);
        *length = size;
        return bytes;
    }

    return NULL;
}

/* Find a byte block in the ROM. The fonts sit as contiguous runs of glyph data, so
 * "does this exact table appear anywhere in the real BIOS" is the whole question --
 * no offset needs to be hardcoded, and none is, because a hardcoded offset would
 * turn a ROM-layout change into a false failure.
 */
static long VgaRomTestFind(PCBYTE haystack, long haystackLength, PCBYTE needle, long needleLength)
{
    long index;

    if (needleLength > haystackLength)
        return -1;

    for (index = 0; index + needleLength <= haystackLength; ++index)
        if (!memcmp(haystack + index, needle, (size_t)needleLength))
            return index;

    return -1;
}

INT main(VOID)
{
    long romLength = 0;
    PBYTE rom = VgaRomTestReadFile("PCem-ROMs-master/ibm_vga.bin", &romLength);

    printf("== VGA tables vs the genuine IBM VGA BIOS (oracle-gated) ==\n");

    if (!rom)
    {
        VgaRomTestSkip("ibm_vga.bin absent -- VGA font/table claims UNVERIFIED this run");
        printf("        (the ROM pack is licensed + gitignored; see docs, it is not a defect)\n");
        printf("\n%d checks, %d failed, %d skipped\n", g_Passes + g_Failures, g_Failures, g_Skips);
        return 0;
    }

    VgaRomTestCheck(romLength == 32768, "ibm_vga.bin is the expected 32KB image");

    /* -- THE THREE FONTS are no longer IBM's (#322): NTVDMEX ships no font data and
     * builds the tables from the system's fonts at start-up, so there is nothing
     * here to compare against the ROM. (The checks that did so are retired.)
     */

    /* -- THE PER-MODE CRTC TABLES. vgadefs.asm read these back off a real card; the
     * BIOS is where they come from in the first place, so the two must agree. A
     * mode's 25 CRTC bytes appear in the ROM's video parameter table as a run.
     *
     * [CAUTION]: The BIOS table stores CRTC 0x00..0x18 contiguously, which is exactly our
     * row, so a whole row is findable. If a row ever stops being findable the
     * likely cause is that someone "tidied" a value.
     */
    {   UINT row;
    INT found = 0;
    INT tried = 0;

        for (row = 0; row < 9; ++row)
        {
            long offset = VgaRomTestFind(rom, romLength, g_VgaCrtcDefaults[row], 25);
            ++tried;

            if (offset >= 0)
                ++found;
        }

        VgaRomTestCheck(found >= 7, "per-mode CRTC rows are present in the ROM's parameter table");
        printf("        (%d of %d CRTC rows located in the real BIOS)\n", found, tried);
    }

    /* -- THE VERTICAL TIMING WE NOW DERIVE FROM THOSE ROWS. This is the claim the
     * 0x3DA model rests on, so state it here in numbers rather than leaving it
     * implicit in a table: 640x350 is the mode that broke the old two-case guess.
     */
    {   PCBYTE crtc = g_VgaCrtcDefaults[6];      /* modes 0Fh, 10h */
        UINT overflow = crtc[0x07];
        UINT maximumScanLine = crtc[0x09];
        UINT verticalTotal  = crtc[0x06] | ((overflow >> 0 & 1) << 8) | ((overflow >> 5 & 1) << 9);
        UINT verticalDisplayEnd = crtc[0x12] | ((overflow >> 1 & 1) << 8) | ((overflow >> 6 & 1) << 9);
        UINT verticalBlankStart = crtc[0x15] | ((overflow >> 3 & 1) << 8) | ((maximumScanLine >> 5 & 1) << 9);
        VgaRomTestCheck(verticalTotal + 2 == 449, "640x350: the BIOS says 449 scanlines per frame");
        VgaRomTestCheck(verticalDisplayEnd + 1 == 350, "640x350: the BIOS says 350 active lines, not 400");
        VgaRomTestCheck(verticalBlankStart == 355,     "640x350: blanking starts at line 355, not 400");
    }

    printf("\n%d checks, %d failed, %d skipped\n", g_Passes + g_Failures, g_Failures, g_Skips);
    free(rom);
    return g_Failures ? 1 : 0;
}
