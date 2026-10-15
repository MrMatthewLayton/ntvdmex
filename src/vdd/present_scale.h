/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The pure arithmetic behind the Display settings.
 *
 * WHY IT IS A HEADER OF ITS OWN. present_ddraw.c cannot be built off the VM: it
 * needs <ddraw.h>, an HWND and a desktop. The two decisions the Display page
 * actually makes -- WHERE the frame goes in the client area (aspect ratio) and
 * WHAT the pixels are before they get there (the scaler) -- are integer
 * arithmetic on a buffer, with no Windows in them at all. Kept here, they are
 * exercised by tests/unit/present_test.c on the build machine, which is the
 * difference between "the knob is wired" and "the knob is wired and right".
 *
 * [INFO]: #325: PIXEL-PERFECT BY DEFAULT. A frame's pixels are square unless the user
 * forces a ratio, and a window is a whole multiple of the frame. PresentLayout and
 * PresentWindowPicture below are the whole policy, for every output path.
 *
 * Pure C, no <windows.h>, no CRT.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_PRESENT_SCALE_H
#define NTVDMEX_PRESENT_SCALE_H

#include "../ntvdmex_types.h"

/* PresentLayout: laying out for a window or for the whole screen. */
#define PRESENT_LAYOUT_WINDOW           0
#define PRESENT_LAYOUT_SCREEN           1

/* A frame with no size yet is laid out as the 720x400 text screen (9:5). */
#define PRESENT_DEFAULT_FRAME_WIDTH     720
#define PRESENT_DEFAULT_FRAME_HEIGHT    400
#define PRESENT_DEFAULT_RATIO_WIDTH     9
#define PRESENT_DEFAULT_RATIO_HEIGHT    5

/* #158: the effect pass (PresentEffectRender). */
#define PRESENT_SCALE2X_FACTOR          2
#define PRESENT_SCALE3X_FACTOR          3
#define PRESENT_EFFECT_MAX_PIXELS       (1920u * 1440u)     /* The largest buffer: 640x480 at 3x */
#define PRESENT_SCANLINES_DARK_PERCENT  50                  /* Scanlines: what a dark row keeps */
#define PRESENT_CRT_SCANLINE_PERCENT    60                  /* CRT: softer scanlines... */
#define PRESENT_CRT_MASK_PERCENT        70                  /* ...a phosphor keeps the other two channels at this... */
#define PRESENT_CRT_LIFT_PERCENT        125                 /* ...and the whole is brightened back up */
#define PRESENT_CRT_MASK_PHASES         3                   /* Red, green, blue stripes, one column each */

/* ARGB channels, and the tints' arithmetic (see PresentTint). */
#define PRESENT_ALPHA_MASK              0xFF000000u
#define PRESENT_RED_SHIFT               16
#define PRESENT_GREEN_SHIFT             8
#define PRESENT_CHANNEL_MASK            0xFF
#define PRESENT_CHANNEL_MAX             255
#define PRESENT_CHANNEL_SCALE           255u
#define PRESENT_LUMA_RED                299u                /* Rec. 601, per mille */
#define PRESENT_LUMA_GREEN              587u
#define PRESENT_LUMA_BLUE               114u
#define PRESENT_LUMA_ROUND              500u
#define PRESENT_LUMA_SCALE              1000u
#define PRESENT_SEPIA_KEEP_NUMERATOR    2                   /* Keep 2/5 = 40% of the distance from luma */
#define PRESENT_SEPIA_KEEP_DENOMINATOR  5
#define PRESENT_SEPIA_WARM_RED          108                 /* The warm cast, percent */
#define PRESENT_SEPIA_WARM_GREEN        98
#define PRESENT_SEPIA_WARM_BLUE         80
#define PRESENT_SEPIA_BLACK_RED         30u                 /* Black fades to (30,22,12) */
#define PRESENT_SEPIA_BLACK_GREEN       22u
#define PRESENT_SEPIA_BLACK_BLUE        12u
#define PRESENT_SEPIA_SPAN_RED          225u                /* 255 - the black: full scale stays full */
#define PRESENT_SEPIA_SPAN_GREEN        233u
#define PRESENT_SEPIA_SPAN_BLUE         243u
#define PRESENT_ASPECT_ITEMS            "Native (square pixels)|4:3|16:9|16:10|Stretch"
#define PRESENT_FIT_ITEMS               "Whole pixels|Fill"
#define PRESENT_FILTER_ITEMS            "Nearest|Bilinear|Sharp"
#define PRESENT_SCALER_ITEMS            "None|Scale2x|Scale3x|Scanlines|CRT"
#define PRESENT_TINT_ITEMS              "Default|Sepia|Monochrome white|Monochrome green|Monochrome orange"

/* Scaler ids -- the indices of the Scaler combo in settings.h, so the setting is
 * the value and there is no translation table to disagree with the list.
 *
 * [INFO]: #158: SCALE3X TOOK HQ2X'S INDEX. hq2x was never implemented (it presented as
 * None), so a stored 2 now means Scale3x, the nearest thing to what was asked for,
 * and Scanlines and CRT keep their stored values.
 */
enum
{
    PRESENT_SCALER_NONE = 0,
    PRESENT_SCALER_SCALE2X,
    PRESENT_SCALER_SCALE3X,
    PRESENT_SCALER_SCANLINES,
    PRESENT_SCALER_CRT,
    PRESENT_SCALER_COUNT
};

/* #325: THE ASPECT CHOICES, AND THEY ARE ALSO THE REGISTRY VALUE:
 * NATIVE is the default: square pixels, the frame's own width:height -- 320x200 is
 * 8:5, 720x400 is 9:5, 640x480 is 4:3. Nothing is assumed about the monitor a mode
 * was once shown on; a user who wants a period shape picks it. The forced ratios
 * shape the picture to that ratio whatever the mode. STRETCH is Native in a window
 * and fills an area the user did not size (maximised, fullscreen).
 *
 * [CAUTION]: THE REGISTRY NAME CHANGED WITH THE MEANING ("DisplayAspect", settings.h): index 0
 * was Auto (= 4:3 for every VGA mode), and a stored 0 must not silently become
 * something else.
 */
enum
{
    PRESENT_ASPECT_NATIVE = 0,
    PRESENT_ASPECT_4_3,
    PRESENT_ASPECT_16_9,
    PRESENT_ASPECT_16_10,
    PRESENT_ASPECT_STRETCH,
    PRESENT_ASPECT_COUNT
};

/* How a picture fills an area the user did not size (maximised or fullscreen). */
enum
{
    PRESENT_FIT_WHOLE = 0, PRESENT_FIT_FILL
};

/* Filtering -- only consulted when the picture is NOT a whole multiple of the frame
 * (every filter agrees on a whole multiple: point-sampled). Sharp enlarges by the
 * largest whole multiple point-sampled, then smooths only the remainder.
 */
enum
{
    PRESENT_FILTER_NEAREST = 0, PRESENT_FILTER_BILINEAR, PRESENT_FILTER_SHARP
};

/* #229: COLOUR FILTERS (docs/EMULATION.md). Default, Sepia, and the three
 * monochrome monitors of the period -- white (paper-white), green (P1 phosphor) and
 * orange (amber). Applied per COLOUR, never per pixel where a palette exists: the
 * presenter recolours the 256 palette entries (and the split-palette tables), so an
 * 8-bit frame costs 256 operations whatever its size. Direct-colour frames pay per
 * pixel, and only when a filter is chosen.
 * Monochrome is luminance (Rec. 601: 0.299 R + 0.587 G + 0.114 B) scaled into the
 * phosphor's colour.
 * Sepia is WASHED-OUT COLOUR, not a brown monochrome (user, s84: "I was hoping for
 * washed out color (sepia color), not black and off-white" -- the first cut was the
 * usual photo matrix, which throws the hue away). Three steps, in integers:
 * 1. keep 40% of each channel's distance from the luminance (the hue survives);
 * 2. a warm cast: red x1.08, green x0.98, blue x0.80;
 * 3. fade: lift black to a dark brown (30,22,12), as an old print's blacks fade.
 * So pure red stays the reddest thing on the screen, only muted and warm; white is
 * cream; black is brown.
 */
enum
{
    PRESENT_TINT_DEFAULT = 0,
    PRESENT_TINT_SEPIA,
    PRESENT_TINT_MONO_WHITE,
    PRESENT_TINT_MONO_GREEN,
    PRESENT_TINT_MONO_ORANGE,
    PRESENT_TINT_COUNT
};

/* The pixel-art factor a scaler enlarges by (Scale2x 2, Scale3x 3); 1 for the rest. */
INT PresentScalerFactor(INT scaler);

/* Does it darken the bottom of each frame row? (Scanlines and CRT) */
INT PresentScalerHasScanlines(INT scaler);

/* Does it change the picture at all? None is the one that does not. */
INT PresentScalerHasEffect(INT scaler);

/* The ratio a FORCED setting stands for; 0/0 for Native and Stretch (no ratio of its
 * own). Used by PresentFit, where 0/0 means "fill".
 */
VOID PresentAspectRatio(INT aspect, INT *ratioWidth, INT *ratioHeight);

/* The ratio the picture is shaped to for a frame sw x sh: a forced one, or the
 * frame's own (Native, and Stretch in a window).
 */
VOID PresentTargetRatio(
    INT aspect,
    INT sourceWidth,
    INT sourceHeight,
    INT *ratioWidth,
    INT *ratioHeight);
INT PresentIsNative(INT aspect);

/* The largest n:d rectangle in dst, centred. */
VOID PresentFitRatio(
    INT destinationWidth,
    INT destinationHeight,
    INT ratioWidth,
    INT ratioHeight,
    INT *left,
    INT *top,
    INT *width,
    INT *height);

/* #325: WHERE THE PICTURE GOES, for every path (window, maximised, fullscreen, both
 * renderers). `screen` = an area the user did not size (maximised / fullscreen).
 * Stretch on a screen      -> the whole area.
 * Native, Whole pixels     -> the largest whole multiple k that fits (one k, so the
 *                             pixels stay square); if even 1x does not fit, scaled
 *                             down on-ratio.
 * Forced, Whole pixels     -> the largest pair of whole multiples that gives the
 *                             ratio EXACTLY (320x200 at 4:3 is 5x6 = 1600x1200), if
 *                             one fits; otherwise the largest on-ratio rectangle.
 * Fill (or a window)       -> the largest on-ratio rectangle.
 * A window is sized to the picture (PresentWindowPicture), so in a window this
 * returns the whole client. Centred; the caller paints the borders.
 */
VOID PresentLayout(
    INT aspect,
    INT fit,
    INT isScreen,
    INT destinationWidth,
    INT destinationHeight,
    INT sourceWidth,
    INT sourceHeight,
    INT *left,
    INT *top,
    INT *width,
    INT *height);

/* #325: THE PICTURE A WINDOW IS SIZED TO, at whole scale k (1x = one desktop pixel
 * per frame pixel). Native: the frame times k. Forced: k times the frame's WIDTH, and
 * the height that gives the ratio -- mode 13h at 2x and 4:3 is 640x480.
 */
VOID PresentWindowPicture(
    INT aspect,
    INT sourceWidth,
    INT sourceHeight,
    INT scale,
    INT *width,
    INT *height);

/* Where the frame goes inside the client area.
 *
 * [CAUTION]: WITH A LOCK ON, THE CLIENT IS ALREADY THAT SHAPE, so this normally returns the
 * whole client and there are no bars -- which is the point of locking the window
 * rather than letterboxing inside a free-shaped one. It still letterboxes when the
 * two disagree (a maximised window, a drag Windows would not let us constrain),
 * because distorting the picture is the worse of the two answers.
 */
VOID PresentFit(
    INT destinationWidth,
    INT destinationHeight,
    INT aspect,
    INT *left,
    INT *top,
    INT *width,
    INT *height);

/* (#325: present_fit_int -- whole multiples behind the fsinteger.flag file knob -- is
 * gone. Whole pixels are the default for every path now; see PresentLayout.)
 */

/* SCALE2X AND SCALE3X (EPX), from the published description of the algorithm:
 * Each source pixel becomes 2x2 (or 3x3). A sub-pixel takes a neighbour's colour
 * only where the neighbours it lies between AGREE and the opposite pair does not --
 * which turns a staircase into a diagonal without touching the interior of a flat
 * region. The rule is an equality test, so it works on ARGB without any notion of
 * distance between colours.
 *
 * Edges use clamped neighbours, so the border of the image is a plain enlargement
 * -- there is no off-buffer read and no seam.
 *
 * `factor` is 2 or 3. destination must have room for (sourceWidth*factor) x
 * (sourceHeight*factor) pixels with a stride of sourceWidth*factor.
 */
VOID PresentScaleEpx(
    const UINT32 *source,
    INT sourceWidth,
    INT sourceHeight,
    INT sourceStride,
    INT factor,
    UINT32 *destination);

/* #158: THE BUFFER SCALE OF THE EFFECT PASS (Importance = 2):
 * The effects are drawn at a WHOLE multiple m of the frame and then stretched to the
 * picture, so m is never more than the picture's own whole scale k (min of
 * pictureWidth/sourceWidth and pictureHeight/sourceHeight): Scale3x into a 2x picture
 * would only be thrown away again by the stretch, and a scanline needs at least two
 * screen rows per frame row to exist. Among the m that fit maxPixels, a DIVISOR of k
 * is preferred, so the stretch that follows is exact (an 8x picture of 320x200 gets a
 * 4x buffer and a plain 2x stretch, not a 5x buffer and uneven rows). Returns 1 when
 * no effect can be drawn: a picture under 2x, or a frame too big for even a 2x buffer.
 */
INT PresentEffectScale(
    INT sourceWidth,
    INT sourceHeight,
    INT pictureWidth,
    INT pictureHeight,
    UINT maxPixels);

/* #158: THE EFFECT PASS. Draws the frame (ARGB) at `scale` (from PresentEffectScale)
 * into destination, (sourceWidth*scale) x (sourceHeight*scale) with that width as its
 * stride:
 * 1. Scale2x / Scale3x into `scratch` first (at no more than `scale`), which then
 *    stands in for the frame. scratch needs (sourceWidth*f) x (sourceHeight*f) pixels.
 * 2. Every frame row becomes `scale` rows; with Scanlines or CRT the bottom half of
 *    them (scale/2, rounded down) is darkened, so the line belongs to the GAME's row
 *    at every scale, never to whichever screen rows happen to be odd.
 * 3. CRT adds an aperture mask: buffer columns cycle red, green, blue, each keeping
 *    its own channel and PRESENT_CRT_MASK_PERCENT of the other two; then the whole is
 *    lifted by PRESENT_CRT_LIFT_PERCENT, because a mask and scanlines darken the
 *    picture the user chose the brightness of.
 */
VOID PresentEffectRender(
    const UINT32 *source,
    INT sourceWidth,
    INT sourceHeight,
    INT sourceStride,
    INT scaler,
    INT scale,
    UINT32 *scratch,
    UINT32 *destination);

UINT32 PresentTint(UINT32 argb, INT tint);

#endif /* NTVDMEX_PRESENT_SCALE_H */
