/*
 * present_scale.h -- the pure arithmetic behind the Display settings.
 *
 * WHY IT IS A HEADER OF ITS OWN. present_ddraw.c cannot be built off the VM: it
 * needs <ddraw.h>, an HWND and a desktop. The two decisions the Display page
 * actually makes -- WHERE the frame goes in the client area (aspect ratio) and
 * WHAT the pixels are before they get there (the scaler) -- are integer
 * arithmetic on a buffer, with no Windows in them at all. Kept here, they are
 * exercised by tests/unit/present_test.c on the build machine, which is the
 * difference between "the knob is wired" and "the knob is wired and right".
 *
 * ★ #325: PIXEL-PERFECT BY DEFAULT. A frame's pixels are square unless the user
 *   forces a ratio, and a window is a whole multiple of the frame. PresentLayout and
 *   PresentWindowPicture below are the whole policy, for every output path.
 *
 * Pure C, no <windows.h>, no CRT.
 */
#ifndef NTVDMEX_PRESENT_SCALE_H
#define NTVDMEX_PRESENT_SCALE_H

#include "../ntvdmex_types.h"

/* A frame with no size yet is laid out as the 720x400 text screen (9:5). */
#define PRESENT_DEFAULT_FRAME_WIDTH   720
#define PRESENT_DEFAULT_FRAME_HEIGHT  400
#define PRESENT_DEFAULT_RATIO_WIDTH   9
#define PRESENT_DEFAULT_RATIO_HEIGHT  5
#define PRESENT_SCALE2X_FACTOR        2
/* ARGB channels, and the tints' arithmetic (see PresentTint). */
#define PRESENT_ALPHA_MASK            0xFF000000u
#define PRESENT_RED_SHIFT             16
#define PRESENT_GREEN_SHIFT           8
#define PRESENT_CHANNEL_MASK          0xFF
#define PRESENT_CHANNEL_MAX           255
#define PRESENT_CHANNEL_SCALE         255u
#define PRESENT_LUMA_RED              299u    /* Rec. 601, per mille                      */
#define PRESENT_LUMA_GREEN            587u
#define PRESENT_LUMA_BLUE             114u
#define PRESENT_LUMA_ROUND            500u
#define PRESENT_LUMA_SCALE            1000u
#define PRESENT_SEPIA_KEEP_NUMERATOR  2       /* keep 2/5 = 40% of the distance from luma */
#define PRESENT_SEPIA_KEEP_DENOMINATOR 5
#define PRESENT_SEPIA_WARM_RED        108     /* the warm cast, percent                   */
#define PRESENT_SEPIA_WARM_GREEN      98
#define PRESENT_SEPIA_WARM_BLUE       80
#define PRESENT_SEPIA_BLACK_RED       30u     /* black fades to (30,22,12)                */
#define PRESENT_SEPIA_BLACK_GREEN     22u
#define PRESENT_SEPIA_BLACK_BLUE      12u
#define PRESENT_SEPIA_SPAN_RED        225u    /* 255 - the black: full scale stays full   */
#define PRESENT_SEPIA_SPAN_GREEN      233u
#define PRESENT_SEPIA_SPAN_BLUE       243u

/* Scaler ids -- the indices of the Scaler combo in settings.h, so the setting is
   the value and there is no translation table to disagree with the list. */
enum {
    PRESENT_SCALER_NONE = 0,
    PRESENT_SCALER_SCALE2X,
    PRESENT_SCALER_HQ2X,        /* NOT IMPLEMENTED -- presents as NONE, and says so */
    PRESENT_SCALER_SCANLINES,
    PRESENT_SCALER_CRT          /* = SCALE2X + SCANLINES                            */
};

/* Does this scaler double the source buffer before the stretch? */
static INT PresentScalerDoubles(INT scaler)
{ return scaler == PRESENT_SCALER_SCALE2X || scaler == PRESENT_SCALER_CRT; }

/* Does it mask alternate DESTINATION rows? (a scanline is a property of the
   screen, not of the frame, so it is applied after the stretch, not before) */
static INT PresentScalerHasScanlines(INT scaler)
{ return scaler == PRESENT_SCALER_SCANLINES || scaler == PRESENT_SCALER_CRT; }

/* ── #325: THE ASPECT CHOICES, AND THEY ARE ALSO THE REGISTRY VALUE. ──────────────
     NATIVE is the default: square pixels, the frame's own width:height -- 320x200 is
     8:5, 720x400 is 9:5, 640x480 is 4:3. Nothing is assumed about the monitor a mode
     was once shown on; a user who wants a period shape picks it. The forced ratios
     shape the picture to that ratio whatever the mode. STRETCH is Native in a window
     and fills an area the user did not size (maximised, fullscreen).
   ⚠ THE REGISTRY NAME CHANGED WITH THE MEANING ("DisplayAspect", settings.h): index 0
     was Auto (= 4:3 for every VGA mode), and a stored 0 must not silently become
     something else. */
enum {
    PRESENT_ASPECT_NATIVE = 0,
    PRESENT_ASPECT_4_3,
    PRESENT_ASPECT_16_9,
    PRESENT_ASPECT_16_10,
    PRESENT_ASPECT_STRETCH,
    PRESENT_ASPECT_COUNT
};
#define PRESENT_ASPECT_ITEMS "Native (square pixels)|4:3|16:9|16:10|Stretch"

/* How a picture fills an area the user did not size (maximised or fullscreen). */
enum { PRESENT_FIT_WHOLE = 0, PRESENT_FIT_FILL };
#define PRESENT_FIT_ITEMS "Whole pixels|Fill"

/* Filtering -- only consulted when the picture is NOT a whole multiple of the frame
   (every filter agrees on a whole multiple: point-sampled). Sharp enlarges by the
   largest whole multiple point-sampled, then smooths only the remainder. */
enum { PRESENT_FILTER_NEAREST = 0, PRESENT_FILTER_BILINEAR, PRESENT_FILTER_SHARP };
#define PRESENT_FILTER_ITEMS "Nearest|Bilinear|Sharp"

/* The ratio a FORCED setting stands for; 0/0 for Native and Stretch (no ratio of its
   own). Used by PresentFit, where 0/0 means "fill". */
static VOID PresentAspectRatio(INT aspect, INT *ratioWidth, INT *ratioHeight)
{
    switch (aspect) {
    case PRESENT_ASPECT_4_3:   *ratioWidth = 4;  *ratioHeight = 3;  break;
    case PRESENT_ASPECT_16_9:  *ratioWidth = 16; *ratioHeight = 9;  break;
    case PRESENT_ASPECT_16_10: *ratioWidth = 16; *ratioHeight = 10; break;
    default:                   *ratioWidth = 0;  *ratioHeight = 0;  break;
    }
}

/* The ratio the picture is shaped to for a frame sw x sh: a forced one, or the
   frame's own (Native, and Stretch in a window). */
static VOID PresentTargetRatio(INT aspect, INT sourceWidth, INT sourceHeight, INT *ratioWidth, INT *ratioHeight)
{
    PresentAspectRatio(aspect, ratioWidth, ratioHeight);
    if (!*ratioWidth || !*ratioHeight) { *ratioWidth = sourceWidth > 0 ? sourceWidth : PRESENT_DEFAULT_RATIO_WIDTH; *ratioHeight = sourceHeight > 0 ? sourceHeight : PRESENT_DEFAULT_RATIO_HEIGHT; }
}
static INT PresentIsNative(INT aspect)
{ return aspect == PRESENT_ASPECT_NATIVE || aspect == PRESENT_ASPECT_STRETCH; }

/* The largest n:d rectangle in dst, centred. */
static VOID PresentFitRatio(INT destinationWidth, INT destinationHeight, INT ratioWidth, INT ratioHeight, INT *left, INT *top, INT *width, INT *height)
{
    long fitWidth, fitHeight;
    if (destinationWidth < 1) destinationWidth = 1;
    if (destinationHeight < 1) destinationHeight = 1;
    if (ratioWidth < 1 || ratioHeight < 1) { *left = 0; *top = 0; *width = destinationWidth; *height = destinationHeight; return; }
    fitWidth = destinationWidth; fitHeight = (long)destinationWidth * ratioHeight / ratioWidth;
    if (fitHeight > destinationHeight) { fitHeight = destinationHeight; fitWidth = (long)destinationHeight * ratioWidth / ratioHeight; }
    if (fitWidth < 1) fitWidth = 1;
    if (fitHeight < 1) fitHeight = 1;
    *width = (INT)fitWidth; *height = (INT)fitHeight;
    *left = (destinationWidth - *width) / 2;
    *top = (destinationHeight - *height) / 2;
}

/* ── #325: WHERE THE PICTURE GOES, for every path (window, maximised, fullscreen, both
     renderers). `screen` = an area the user did not size (maximised / fullscreen).
       Stretch on a screen      -> the whole area.
       Native, Whole pixels     -> the largest whole multiple k that fits (one k, so the
                                   pixels stay square); if even 1x does not fit, scaled
                                   down on-ratio.
       Forced, Whole pixels     -> the largest pair of whole multiples that gives the
                                   ratio EXACTLY (320x200 at 4:3 is 5x6 = 1600x1200), if
                                   one fits; otherwise the largest on-ratio rectangle.
       Fill (or a window)       -> the largest on-ratio rectangle.
     A window is sized to the picture (PresentWindowPicture), so in a window this
     returns the whole client. Centred; the caller paints the borders. */
static VOID PresentLayout(INT aspect, INT fit, INT isScreen, INT destinationWidth, INT destinationHeight,
                           INT sourceWidth, INT sourceHeight, INT *left, INT *top, INT *width, INT *height)
{
    INT ratioWidth, ratioHeight;
    if (destinationWidth < 1) destinationWidth = 1;
    if (destinationHeight < 1) destinationHeight = 1;
    if (aspect == PRESENT_ASPECT_STRETCH && isScreen) { *left = 0; *top = 0; *width = destinationWidth; *height = destinationHeight; return; }
    PresentTargetRatio(aspect, sourceWidth, sourceHeight, &ratioWidth, &ratioHeight);
    if (sourceWidth > 0 && sourceHeight > 0 && fit == PRESENT_FIT_WHOLE) {
        if (PresentIsNative(aspect)) {
            INT scaleX = destinationWidth / sourceWidth, scaleY = destinationHeight / sourceHeight, scale = scaleX < scaleY ? scaleX : scaleY;
            if (scale >= 1) {
                *width = sourceWidth * scale; *height = sourceHeight * scale;
                *left = (destinationWidth - *width) / 2; *top = (destinationHeight - *height) / 2;
                return;
            }
        } else {
            long bestArea = 0; INT bestScaleX = 0, bestScaleY = 0, scaleX;
            for (scaleX = 1; (long)sourceWidth * scaleX <= destinationWidth; ++scaleX) {
                long numerator = (long)sourceWidth * scaleX * ratioHeight, denominator = (long)sourceHeight * ratioWidth;   /* ny = num/den */
                if (numerator % denominator) continue;
                if ((long)sourceHeight * (numerator / denominator) > destinationHeight || numerator / denominator < 1) continue;
                if ((long)sourceWidth * scaleX * sourceHeight * (numerator / denominator) > bestArea) {
                    bestArea = (long)sourceWidth * scaleX * sourceHeight * (numerator / denominator); bestScaleX = scaleX; bestScaleY = (INT)(numerator / denominator);
                }
            }
            if (bestArea) {
                *width = sourceWidth * bestScaleX; *height = sourceHeight * bestScaleY;
                *left = (destinationWidth - *width) / 2; *top = (destinationHeight - *height) / 2;
                return;
            }
        }
    }
    PresentFitRatio(destinationWidth, destinationHeight, ratioWidth, ratioHeight, left, top, width, height);
}

/* ── #325: THE PICTURE A WINDOW IS SIZED TO, at whole scale k (1x = one desktop pixel
     per frame pixel). Native: the frame times k. Forced: k times the frame's WIDTH, and
     the height that gives the ratio -- mode 13h at 2x and 4:3 is 640x480. */
static VOID PresentWindowPicture(INT aspect, INT sourceWidth, INT sourceHeight, INT scale, INT *width, INT *height)
{
    INT ratioWidth, ratioHeight;
    if (scale < 1) scale = 1;
    if (sourceWidth < 1 || sourceHeight < 1) { sourceWidth = PRESENT_DEFAULT_FRAME_WIDTH; sourceHeight = PRESENT_DEFAULT_FRAME_HEIGHT; }
    *width = sourceWidth * scale;
    if (PresentIsNative(aspect)) { *height = sourceHeight * scale; return; }
    PresentTargetRatio(aspect, sourceWidth, sourceHeight, &ratioWidth, &ratioHeight);
    *height = (INT)(((long)*width * ratioHeight + ratioWidth / 2) / ratioWidth);
}

/* Where the frame goes inside the client area.
 ⚠ WITH A LOCK ON, THE CLIENT IS ALREADY THAT SHAPE, so this normally returns the
   whole client and there are no bars -- which is the point of locking the window
   rather than letterboxing inside a free-shaped one. It still letterboxes when the
   two disagree (a maximised window, a drag Windows would not let us constrain),
   because distorting the picture is the worse of the two answers. */
static VOID PresentFit(INT destinationWidth, INT destinationHeight, INT aspect,
                        INT *left, INT *top, INT *width, INT *height)
{
    INT fitWidth, fitHeight, ratioWidth, ratioHeight;
    if (destinationWidth < 1) destinationWidth = 1;
    if (destinationHeight < 1) destinationHeight = 1;
    PresentAspectRatio(aspect, &ratioWidth, &ratioHeight);
    if (!ratioWidth || !ratioHeight) { *left = 0; *top = 0; *width = destinationWidth; *height = destinationHeight; return; }
    fitWidth = destinationWidth; fitHeight = destinationWidth * ratioHeight / ratioWidth;              /* as wide as possible...          */
    if (fitHeight > destinationHeight) { fitHeight = destinationHeight; fitWidth = destinationHeight * ratioWidth / ratioHeight; }  /* ...unless too tall      */
    if (fitWidth < 1) fitWidth = 1;
    if (fitHeight < 1) fitHeight = 1;
    *width = fitWidth; *height = fitHeight;
    *left = (destinationWidth - fitWidth) / 2;
    *top = (destinationHeight - fitHeight) / 2;
}

/* (#325: present_fit_int -- whole multiples behind the fsinteger.flag file knob -- is
   gone. Whole pixels are the default for every path now; see PresentLayout.) */

/* ── SCALE2X (EPX). ──────────────────────────────────────────────────────────────
     Each source pixel becomes four. A corner is interpolated only where the two
     neighbours it lies between AGREE and the opposite pair does not -- which is
     what turns a staircase into a diagonal without touching the interior of a
     flat region. Working on PALETTE INDICES, not colours, is not an approximation
     here: the rule is an equality test, and two indices are the same colour iff
     they are the same index in the same palette.

     Edges use clamped neighbours, so the border of the image is a plain doubling
     -- there is no off-buffer read and no seam.

     dst must have room for (sw*2) x (sh*2) bytes with a stride of sw*2. */
/* `int sourceWidth`, not INT: spelt INT it moved the register allocation of
   present_ddraw.c, which inlines this -- bisected to that one parameter (s93). */
static VOID PresentScale2x8(const BYTE *source, int sourceWidth, INT sourceHeight, INT sourceStride,
                              BYTE *destination)
{
    INT column, row, destinationStride = sourceWidth * PRESENT_SCALE2X_FACTOR;
    for (row = 0; row < sourceHeight; ++row) {
        const BYTE *sourceRow = source + (SIZE_T)row * sourceStride;
        const BYTE *rowAbove  = source + (SIZE_T)(row > 0        ? row - 1 : 0) * sourceStride;
        const BYTE *rowBelow  = source + (SIZE_T)(row < sourceHeight - 1   ? row + 1 : sourceHeight - 1) * sourceStride;
        BYTE *outputRow0 = destination + (SIZE_T)(row * PRESENT_SCALE2X_FACTOR)     * destinationStride;
        BYTE *outputRow1 = destination + (SIZE_T)(row * PRESENT_SCALE2X_FACTOR + 1) * destinationStride;
        for (column = 0; column < sourceWidth; ++column) {
            BYTE centre = sourceRow[column];
            BYTE above = rowAbove[column],  below = rowBelow[column];
            BYTE left = sourceRow[column > 0      ? column - 1 : 0];
            BYTE right = sourceRow[column < sourceWidth - 1 ? column + 1 : sourceWidth - 1];
            BYTE topLeft = centre, topRight = centre, bottomLeft = centre, bottomRight = centre;
            if (above != below && left != right) {
                if (left == above) topLeft = left;
                if (above == right) topRight = right;
                if (left == below) bottomLeft = left;
                if (below == right) bottomRight = right;
            }
            outputRow0[column * PRESENT_SCALE2X_FACTOR] = topLeft; outputRow0[column * PRESENT_SCALE2X_FACTOR + 1] = topRight;
            outputRow1[column * PRESENT_SCALE2X_FACTOR] = bottomLeft; outputRow1[column * PRESENT_SCALE2X_FACTOR + 1] = bottomRight;
        }
    }
}

/* ── #229: COLOUR FILTERS (docs/EMULATION.md). Default, Sepia, and the three
     monochrome monitors of the period -- white (paper-white), green (P1 phosphor) and
     orange (amber). Applied per COLOUR, never per pixel where a palette exists: the
     presenter recolours the 256 palette entries (and the split-palette tables), so an
     8-bit frame costs 256 operations whatever its size. Direct-colour frames pay per
     pixel, and only when a filter is chosen.
   Monochrome is luminance (Rec. 601: 0.299 R + 0.587 G + 0.114 B) scaled into the
   phosphor's colour.
   Sepia is WASHED-OUT COLOUR, not a brown monochrome (user, s84: "I was hoping for
   washed out color (sepia color), not black and off-white" -- the first cut was the
   usual photo matrix, which throws the hue away). Three steps, in integers:
     1. keep 40% of each channel's distance from the luminance (the hue survives);
     2. a warm cast: red x1.08, green x0.98, blue x0.80;
     3. fade: lift black to a dark brown (30,22,12), as an old print's blacks fade.
   So pure red stays the reddest thing on the screen, only muted and warm; white is
   cream; black is brown. */
enum {
    PRESENT_TINT_DEFAULT = 0,
    PRESENT_TINT_SEPIA,
    PRESENT_TINT_MONO_WHITE,
    PRESENT_TINT_MONO_GREEN,
    PRESENT_TINT_MONO_ORANGE,
    PRESENT_TINT_COUNT
};
#define PRESENT_TINT_ITEMS "Default|Sepia|Monochrome white|Monochrome green|Monochrome orange"

static UINT32 PresentTint(UINT32 argb, INT tint)
{
    UINT32 alpha = argb & PRESENT_ALPHA_MASK;
    UINT32 red = (argb >> PRESENT_RED_SHIFT) & PRESENT_CHANNEL_MASK, green = (argb >> PRESENT_GREEN_SHIFT) & PRESENT_CHANNEL_MASK, blue = argb & PRESENT_CHANNEL_MASK;
    UINT32 luminance = (red * PRESENT_LUMA_RED + green * PRESENT_LUMA_GREEN + blue * PRESENT_LUMA_BLUE + PRESENT_LUMA_ROUND) / PRESENT_LUMA_SCALE;   /* 0..255 */
    UINT32 outputRed, outputGreen, outputBlue;
    switch (tint) {
    case PRESENT_TINT_SEPIA: {
        /* 1. desaturate to 40%: c' = y + 0.4 (c - y), signed */
        INT sepiaRed = (INT)luminance + ((INT)red - (INT)luminance) * PRESENT_SEPIA_KEEP_NUMERATOR / PRESENT_SEPIA_KEEP_DENOMINATOR;
        INT sepiaGreen = (INT)luminance + ((INT)green - (INT)luminance) * PRESENT_SEPIA_KEEP_NUMERATOR / PRESENT_SEPIA_KEEP_DENOMINATOR;
        INT sepiaBlue = (INT)luminance + ((INT)blue - (INT)luminance) * PRESENT_SEPIA_KEEP_NUMERATOR / PRESENT_SEPIA_KEEP_DENOMINATOR;
        /* 2. warm cast */
        sepiaRed = sepiaRed * PRESENT_SEPIA_WARM_RED / PERCENT; sepiaGreen = sepiaGreen * PRESENT_SEPIA_WARM_GREEN / PERCENT; sepiaBlue = sepiaBlue * PRESENT_SEPIA_WARM_BLUE / PERCENT;
        if (sepiaRed > PRESENT_CHANNEL_MAX) sepiaRed = PRESENT_CHANNEL_MAX;
        if (sepiaGreen > PRESENT_CHANNEL_MAX) sepiaGreen = PRESENT_CHANNEL_MAX;
        if (sepiaBlue > PRESENT_CHANNEL_MAX) sepiaBlue = PRESENT_CHANNEL_MAX;
        if (sepiaRed < 0) sepiaRed = 0;
        if (sepiaGreen < 0) sepiaGreen = 0;
        if (sepiaBlue < 0) sepiaBlue = 0;
        /* 3. fade: black -> (30,22,12), full scale stays full scale */
        outputRed = PRESENT_SEPIA_BLACK_RED + (UINT32)sepiaRed * PRESENT_SEPIA_SPAN_RED / PRESENT_CHANNEL_SCALE;
        outputGreen = PRESENT_SEPIA_BLACK_GREEN + (UINT32)sepiaGreen * PRESENT_SEPIA_SPAN_GREEN / PRESENT_CHANNEL_SCALE;
        outputBlue = PRESENT_SEPIA_BLACK_BLUE + (UINT32)sepiaBlue * PRESENT_SEPIA_SPAN_BLUE / PRESENT_CHANNEL_SCALE;
        return alpha | (outputRed << PRESENT_RED_SHIFT) | (outputGreen << PRESENT_GREEN_SHIFT) | outputBlue; }
    case PRESENT_TINT_MONO_WHITE:  outputRed = 255u; outputGreen = 255u; outputBlue = 255u; break;
    case PRESENT_TINT_MONO_GREEN:  outputRed = 51u;  outputGreen = 255u; outputBlue = 51u;  break;   /* P1 */
    case PRESENT_TINT_MONO_ORANGE: outputRed = 255u; outputGreen = 176u; outputBlue = 0u;   break;   /* amber */
    default: return argb;
    }
    return alpha | (((luminance * outputRed) / PRESENT_CHANNEL_SCALE) << PRESENT_RED_SHIFT) | (((luminance * outputGreen) / PRESENT_CHANNEL_SCALE) << PRESENT_GREEN_SHIFT) | ((luminance * outputBlue) / PRESENT_CHANNEL_SCALE);
}

#endif /* NTVDMEX_PRESENT_SCALE_H */
