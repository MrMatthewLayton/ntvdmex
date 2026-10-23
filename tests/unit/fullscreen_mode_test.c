/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for #331's fullscreen mode choice (src/host/fullscreen_mode.h).
 *
 * The host's half -- EnumDisplaySettings, ChangeDisplaySettings, putting the desktop
 * back -- needs a display. What the setting MEANS does not: which modes are offered,
 * which one "Native" is, and that nothing ever changes the refresh rate.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "fullscreen_mode.h"
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static INT g_Total = 0;
static INT g_Failures = 0;

INT main(VOID)
{
    /* The issue's machine: a 2560x1600 panel, the desktop at 1680x1050, 32 bpp, 60 Hz;
     * the driver also lists 75 Hz modes and 16 bpp modes, which must never be chosen.
     */
    static const FULLSCREEN_MODE modes[] =
    {
        { 640, 480, 32, 60 }, { 640, 480, 32, 75 }, { 800, 600, 32, 60 }, { 1024, 768, 32, 60 },
        { 1280, 1024, 32, 60 }, { 1680, 1050, 32, 60 }, { 1920, 1200, 32, 60 }, { 2560, 1600, 32, 60 },
        { 2560, 1600, 16, 60 }, { 3000, 2000, 32, 75 }, { 1680, 1050, 32, 60 }, { 0, 0, 32, 60 },
    };
    const INT count = (INT)(sizeof modes / sizeof modes[0]);
    const FULLSCREEN_MODE desktop = { 1680, 1050, 32, 60 };
    FULLSCREEN_MODE list[FULLSCREEN_MODES_MAX];
    FULLSCREEN_MODE chosen;
    INT listed;

    printf("== #331: fullscreen resolution (fullscreen_mode.h) ==\n");

    listed = FullscreenModeList(modes, count, &desktop, list, FULLSCREEN_MODES_MAX);
    CHECK(listed == 7, "list: one entry per size at the desktop's depth and refresh (7)");
    CHECK(list[0].Width == 2560 && list[0].Height == 1600, "list: the largest first");
    CHECK(list[listed - 1].Width == 640 && list[listed - 1].Height == 480, "list: the smallest last");
    {   INT index;
        INT isOnlyDesktopRate = 1;

        for (index = 0; index < listed; ++index)
            if (list[index].Frequency != 60 || list[index].BitsPerPixel != 32)
                isOnlyDesktopRate = 0;

        CHECK(isOnlyDesktopRate, "list: never another refresh rate or depth (the s83 standby trap)"); }
    CHECK(FullscreenModeList(modes, count, &desktop, list, 3) == 3, "list: stops at its capacity");

    CHECK(FullscreenModeChoose(modes, count, &desktop, FULLSCREEN_RESOLUTION_DESKTOP, &chosen) == 0,
          "Desktop: no change -- today's behaviour, the default");
    CHECK(FullscreenModeChoose(modes, count, &desktop, FULLSCREEN_RESOLUTION_NATIVE, &chosen) == 1
          && chosen.Width == 2560 && chosen.Height == 1600 && chosen.Frequency == 60 && chosen.BitsPerPixel == 32,
          "Native: the largest size at the desktop's refresh and depth (2560x1600, not the 75 Hz 3000x2000)");
    CHECK(FullscreenModeChoose(modes, count, &desktop, FULLSCREEN_RESOLUTION_PACK(1920, 1200), &chosen) == 1
          && chosen.Width == 1920 && chosen.Height == 1200, "a picked size: that size");
    CHECK(FullscreenModeChoose(modes, count, &desktop, FULLSCREEN_RESOLUTION_PACK(1680, 1050), &chosen) == 0,
          "a picked size that IS the desktop: no change");
    CHECK(FullscreenModeChoose(modes, count, &desktop, FULLSCREEN_RESOLUTION_PACK(1600, 900), &chosen) == 0,
          "a picked size this display no longer offers: stay on the desktop, never guess");
    CHECK(FullscreenModeChoose(modes, count, &desktop, FULLSCREEN_RESOLUTION_PACK(3000, 2000), &chosen) == 0,
          "a size offered only at another refresh: refused");
    {   const FULLSCREEN_MODE native = { 2560, 1600, 32, 60 };
        CHECK(FullscreenModeChoose(modes, count, &native, FULLSCREEN_RESOLUTION_NATIVE, &chosen) == 0,
              "Native when the desktop already is native: no change"); }

    CHECK(FULLSCREEN_RESOLUTION_WIDTH(FULLSCREEN_RESOLUTION_PACK(2560, 1600)) == 2560
          && FULLSCREEN_RESOLUTION_HEIGHT(FULLSCREEN_RESOLUTION_PACK(2560, 1600)) == 1600,
          "setting: a size round-trips through the packed value");
    CHECK(FULLSCREEN_RESOLUTION_PACK(640, 480) > FULLSCREEN_RESOLUTION_NATIVE,
          "setting: no real size collides with Desktop (0) or Native (1)");

    CHECK(FullscreenWholeScale(1680, 1050, 320, 200) == 5, "scale: 320x200 on 1680x1050 is 5x (the issue's borders)");
    CHECK(FullscreenWholeScale(2560, 1600, 320, 200) == 8, "scale: ...and 8x on 2560x1600, edge to edge");
    CHECK(FullscreenWholeScale(640, 480, 720, 400) == 0, "scale: a frame bigger than the screen: 0");
    CHECK(FullscreenWholeScale(640, 480, 0, 0) == 0, "scale: no frame: 0");

    printf("\n== %d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
