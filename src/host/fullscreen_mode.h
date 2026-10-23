/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * #331: which display mode fullscreen asks for -- the desktop's, the panel's own, or one
 * the user picked from the list.
 *
 * WHY THIS EXISTS. Whole-pixel fullscreen (#325) fits the largest whole multiple of the
 * frame into the screen. On a 2560x1600 panel run at a 1680x1050 desktop, 320x200 gets
 * 5x with borders on every side; at the panel's own 2560x1600 it is exactly 8x and fills
 * it. So fullscreen may now switch the display to another mode for as long as it lasts.
 *
 * [CAUTION]: ONLY THE SIZE CHANGES. A mode is offered only at the desktop's colour depth
 * AND refresh rate: a refresh change is what blanked the test machine's monitor into
 * standby (s83), and nothing here is worth that. A size with no mode at the desktop's
 * refresh is not offered at all.
 *
 * [INFO]: "NATIVE" IS THE LARGEST SIZE AT THAT DEPTH AND REFRESH. XP does not report a
 * panel's native resolution (that arrived with Vista's EDID plumbing); the largest mode the
 * driver lists at the desktop's refresh is the panel's own on every LCD driver we know of,
 * and on a CRT it is the largest the monitor accepts -- which the driver checked.
 *
 * Pure: the host enumerates the modes (EnumDisplaySettings) and switches
 * (ChangeDisplaySettings); everything here is the choice, tested off-VM in
 * tests/unit/fullscreen_mode_test.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_FULLSCREEN_MODE_H
#define NTVDMEX_FULLSCREEN_MODE_H

#include "../ntvdmex_types.h"

/* The FullscreenResolution setting: 0 = the desktop's mode (no change, the default),
 * 1 = the largest (native), anything else a size packed as width << 16 | height --
 * a SIZE rather than a list index, so it means the same thing when the list changes.
 */
#define FULLSCREEN_RESOLUTION_DESKTOP   0u
#define FULLSCREEN_RESOLUTION_NATIVE    1u
#define FULLSCREEN_RESOLUTION_SHIFT     16
#define FULLSCREEN_RESOLUTION_MASK      0xFFFFu
#define FULLSCREEN_RESOLUTION_PACK(width, height) \
    ((((DWORD)(width) & FULLSCREEN_RESOLUTION_MASK) << FULLSCREEN_RESOLUTION_SHIFT) | ((DWORD)(height) & FULLSCREEN_RESOLUTION_MASK))
#define FULLSCREEN_RESOLUTION_WIDTH(setting)    (((DWORD)(setting) >> FULLSCREEN_RESOLUTION_SHIFT) & FULLSCREEN_RESOLUTION_MASK)
#define FULLSCREEN_RESOLUTION_HEIGHT(setting)   ((DWORD)(setting) & FULLSCREEN_RESOLUTION_MASK)
#define FULLSCREEN_MODES_MAX                    64  /* Distinct sizes offered in the list */

typedef struct FULLSCREEN_MODE
{
    DWORD Width;
    DWORD Height;
    DWORD BitsPerPixel;
    DWORD Frequency;        /* Hz; 0 or 1 = the driver's default, compared as given */
} FULLSCREEN_MODE, *PFULLSCREEN_MODE;
typedef const FULLSCREEN_MODE *PCFULLSCREEN_MODE;

/* The sizes `modes` offers at the desktop's depth and refresh, one entry per size, largest
 * area first (wider first on a tie). Returns how many went into `list`.
 */
static INT FullscreenModeList(
    PCFULLSCREEN_MODE modes,
    INT count,
    PCFULLSCREEN_MODE desktop,
    PFULLSCREEN_MODE list,
    INT capacity)
{
    INT found = 0;
    INT index;

    for (index = 0; index < count; ++index)
    {
        PCFULLSCREEN_MODE mode = &modes[index];
        INT seen = 0;
        INT slot;

        if (mode->BitsPerPixel != desktop->BitsPerPixel || mode->Frequency != desktop->Frequency
            || !mode->Width || !mode->Height)
            continue;

        for (slot = 0; slot < found; ++slot)
            if (list[slot].Width == mode->Width && list[slot].Height == mode->Height)
                seen = 1;

        if (seen || found >= capacity)
            continue;

        /* insertion: largest area first, wider first on a tie */
        slot = found;

        while (slot > 0)
        {
            UINT64 previousArea = (UINT64)list[slot - 1].Width * list[slot - 1].Height;
            UINT64 area = (UINT64)mode->Width * mode->Height;

            if (previousArea > area || (previousArea == area && list[slot - 1].Width >= mode->Width))
                break;

            list[slot] = list[slot - 1];
            --slot;
        }

        list[slot] = *mode;
        ++found;
    }

    return found;
}

/* The mode the setting asks for. Returns 1 with `chosen` filled when fullscreen should
 * switch to it, 0 to stay on the desktop's mode: the setting says so, the size is not
 * offered at the desktop's depth and refresh, or it IS the desktop's size already.
 */
static INT FullscreenModeChoose(
    PCFULLSCREEN_MODE modes,
    INT count,
    PCFULLSCREEN_MODE desktop,
    DWORD setting,
    PFULLSCREEN_MODE chosen)
{
    FULLSCREEN_MODE list[FULLSCREEN_MODES_MAX];
    INT listed;
    INT pick = -1;
    INT index;

    if (setting == FULLSCREEN_RESOLUTION_DESKTOP)
        return 0;

    listed = FullscreenModeList(modes, count, desktop, list, FULLSCREEN_MODES_MAX);

    if (setting == FULLSCREEN_RESOLUTION_NATIVE)
        pick = listed ? 0 : -1;
    else
        for (index = 0; index < listed; ++index)
            if (list[index].Width == FULLSCREEN_RESOLUTION_WIDTH(setting)
                && list[index].Height == FULLSCREEN_RESOLUTION_HEIGHT(setting))
                pick = index;

    if (pick < 0 || (list[pick].Width == desktop->Width && list[pick].Height == desktop->Height))
        return 0;

    *chosen = list[pick];
    return 1;
}

/* The whole-pixel factor a `frameWidth` x `frameHeight` picture gets on the screen: the
 * largest N with N x the frame inside it, 0 if it does not fit even once. For the log line,
 * so a choice can be judged by the number it was made for.
 */
static DWORD FullscreenWholeScale(
    DWORD screenWidth,
    DWORD screenHeight,
    DWORD frameWidth,
    DWORD frameHeight)
{
    DWORD across;
    DWORD down;

    if (!frameWidth || !frameHeight)
        return 0;

    across = screenWidth / frameWidth;
    down = screenHeight / frameHeight;
    return across < down ? across : down;
}

#endif
