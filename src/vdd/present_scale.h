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
 *   forces a ratio, and a window is a whole multiple of the frame. present_layout and
 *   present_window_picture below are the whole policy, for every output path.
 *
 * Pure C, no <windows.h>, no CRT.
 */
#ifndef NTVDMEX_PRESENT_SCALE_H
#define NTVDMEX_PRESENT_SCALE_H

#include <stdint.h>

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
static int present_scaler_doubles(int scaler)
{ return scaler == PRESENT_SCALER_SCALE2X || scaler == PRESENT_SCALER_CRT; }

/* Does it mask alternate DESTINATION rows? (a scanline is a property of the
   screen, not of the frame, so it is applied after the stretch, not before) */
static int present_scaler_scanlines(int scaler)
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
   own). Used by present_fit, where 0/0 means "fill". */
static void present_aspect_ratio(int aspect, int *n, int *d)
{
    switch (aspect) {
    case PRESENT_ASPECT_4_3:   *n = 4;  *d = 3;  break;
    case PRESENT_ASPECT_16_9:  *n = 16; *d = 9;  break;
    case PRESENT_ASPECT_16_10: *n = 16; *d = 10; break;
    default:                   *n = 0;  *d = 0;  break;
    }
}

/* The ratio the picture is shaped to for a frame sw x sh: a forced one, or the
   frame's own (Native, and Stretch in a window). */
static void present_target_ratio(int aspect, int sw, int sh, int *n, int *d)
{
    present_aspect_ratio(aspect, n, d);
    if (!*n || !*d) { *n = sw > 0 ? sw : 9; *d = sh > 0 ? sh : 5; }
}
static int present_is_native(int aspect)
{ return aspect == PRESENT_ASPECT_NATIVE || aspect == PRESENT_ASPECT_STRETCH; }

/* The largest n:d rectangle in dst, centred. */
static void present_fit_nd(int dst_w, int dst_h, int n, int d, int *x, int *y, int *w, int *h)
{
    long fw, fh;
    if (dst_w < 1) dst_w = 1;
    if (dst_h < 1) dst_h = 1;
    if (n < 1 || d < 1) { *x = 0; *y = 0; *w = dst_w; *h = dst_h; return; }
    fw = dst_w; fh = (long)dst_w * d / n;
    if (fh > dst_h) { fh = dst_h; fw = (long)dst_h * n / d; }
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;
    *w = (int)fw; *h = (int)fh;
    *x = (dst_w - *w) / 2;
    *y = (dst_h - *h) / 2;
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
     A window is sized to the picture (present_window_picture), so in a window this
     returns the whole client. Centred; the caller paints the borders. */
static void present_layout(int aspect, int fit, int screen, int dst_w, int dst_h,
                           int sw, int sh, int *x, int *y, int *w, int *h)
{
    int n, d;
    if (dst_w < 1) dst_w = 1;
    if (dst_h < 1) dst_h = 1;
    if (aspect == PRESENT_ASPECT_STRETCH && screen) { *x = 0; *y = 0; *w = dst_w; *h = dst_h; return; }
    present_target_ratio(aspect, sw, sh, &n, &d);
    if (sw > 0 && sh > 0 && fit == PRESENT_FIT_WHOLE) {
        if (present_is_native(aspect)) {
            int kx = dst_w / sw, ky = dst_h / sh, k = kx < ky ? kx : ky;
            if (k >= 1) {
                *w = sw * k; *h = sh * k;
                *x = (dst_w - *w) / 2; *y = (dst_h - *h) / 2;
                return;
            }
        } else {
            long best = 0; int bx = 0, by = 0, nx;
            for (nx = 1; (long)sw * nx <= dst_w; ++nx) {
                long num = (long)sw * nx * d, den = (long)sh * n;   /* ny = num/den */
                if (num % den) continue;
                if ((long)sh * (num / den) > dst_h || num / den < 1) continue;
                if ((long)sw * nx * sh * (num / den) > best) {
                    best = (long)sw * nx * sh * (num / den); bx = nx; by = (int)(num / den);
                }
            }
            if (best) {
                *w = sw * bx; *h = sh * by;
                *x = (dst_w - *w) / 2; *y = (dst_h - *h) / 2;
                return;
            }
        }
    }
    present_fit_nd(dst_w, dst_h, n, d, x, y, w, h);
}

/* ── #325: THE PICTURE A WINDOW IS SIZED TO, at whole scale k (1x = one desktop pixel
     per frame pixel). Native: the frame times k. Forced: k times the frame's WIDTH, and
     the height that gives the ratio -- mode 13h at 2x and 4:3 is 640x480. */
static void present_window_picture(int aspect, int sw, int sh, int k, int *w, int *h)
{
    int n, d;
    if (k < 1) k = 1;
    if (sw < 1 || sh < 1) { sw = 720; sh = 400; }
    *w = sw * k;
    if (present_is_native(aspect)) { *h = sh * k; return; }
    present_target_ratio(aspect, sw, sh, &n, &d);
    *h = (int)(((long)*w * d + n / 2) / n);
}

/* Where the frame goes inside the client area.
 ⚠ WITH A LOCK ON, THE CLIENT IS ALREADY THAT SHAPE, so this normally returns the
   whole client and there are no bars -- which is the point of locking the window
   rather than letterboxing inside a free-shaped one. It still letterboxes when the
   two disagree (a maximised window, a drag Windows would not let us constrain),
   because distorting the picture is the worse of the two answers. */
static void present_fit(int dst_w, int dst_h, int aspect,
                        int *x, int *y, int *w, int *h)
{
    int fw, fh, n, d;
    if (dst_w < 1) dst_w = 1;
    if (dst_h < 1) dst_h = 1;
    present_aspect_ratio(aspect, &n, &d);
    if (!n || !d) { *x = 0; *y = 0; *w = dst_w; *h = dst_h; return; }
    fw = dst_w; fh = dst_w * d / n;              /* as wide as possible...          */
    if (fh > dst_h) { fh = dst_h; fw = dst_h * n / d; }  /* ...unless too tall      */
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;
    *w = fw; *h = fh;
    *x = (dst_w - fw) / 2;
    *y = (dst_h - fh) / 2;
}

/* (#325: present_fit_int -- whole multiples behind the fsinteger.flag file knob -- is
   gone. Whole pixels are the default for every path now; see present_layout.) */

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
static void present_scale2x_8(const uint8_t *src, int sw, int sh, int sstride,
                              uint8_t *dst)
{
    int x, y, dstride = sw * 2;
    for (y = 0; y < sh; ++y) {
        const uint8_t *row = src + (size_t)y * sstride;
        const uint8_t *up  = src + (size_t)(y > 0        ? y - 1 : 0) * sstride;
        const uint8_t *dn  = src + (size_t)(y < sh - 1   ? y + 1 : sh - 1) * sstride;
        uint8_t *o0 = dst + (size_t)(y * 2)     * dstride;
        uint8_t *o1 = dst + (size_t)(y * 2 + 1) * dstride;
        for (x = 0; x < sw; ++x) {
            uint8_t e = row[x];
            uint8_t b = up[x],  h = dn[x];
            uint8_t d = row[x > 0      ? x - 1 : 0];
            uint8_t f = row[x < sw - 1 ? x + 1 : sw - 1];
            uint8_t e0 = e, e1 = e, e2 = e, e3 = e;
            if (b != h && d != f) {
                if (d == b) e0 = d;
                if (b == f) e1 = f;
                if (d == h) e2 = d;
                if (h == f) e3 = f;
            }
            o0[x * 2] = e0; o0[x * 2 + 1] = e1;
            o1[x * 2] = e2; o1[x * 2 + 1] = e3;
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

static uint32_t present_tint(uint32_t argb, int tint)
{
    uint32_t a = argb & 0xFF000000u;
    uint32_t r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    uint32_t y = (r * 299u + g * 587u + b * 114u + 500u) / 1000u;   /* 0..255 */
    uint32_t pr, pg, pb;
    switch (tint) {
    case PRESENT_TINT_SEPIA: {
        /* 1. desaturate to 40%: c' = y + 0.4 (c - y), signed */
        int dr = (int)y + ((int)r - (int)y) * 2 / 5;
        int dg = (int)y + ((int)g - (int)y) * 2 / 5;
        int db = (int)y + ((int)b - (int)y) * 2 / 5;
        /* 2. warm cast */
        dr = dr * 108 / 100; dg = dg * 98 / 100; db = db * 80 / 100;
        if (dr > 255) dr = 255;
        if (dg > 255) dg = 255;
        if (db > 255) db = 255;
        if (dr < 0) dr = 0;
        if (dg < 0) dg = 0;
        if (db < 0) db = 0;
        /* 3. fade: black -> (30,22,12), full scale stays full scale */
        pr = 30u + (uint32_t)dr * 225u / 255u;
        pg = 22u + (uint32_t)dg * 233u / 255u;
        pb = 12u + (uint32_t)db * 243u / 255u;
        return a | (pr << 16) | (pg << 8) | pb; }
    case PRESENT_TINT_MONO_WHITE:  pr = 255u; pg = 255u; pb = 255u; break;
    case PRESENT_TINT_MONO_GREEN:  pr = 51u;  pg = 255u; pb = 51u;  break;   /* P1 */
    case PRESENT_TINT_MONO_ORANGE: pr = 255u; pg = 176u; pb = 0u;   break;   /* amber */
    default: return argb;
    }
    return a | (((y * pr) / 255u) << 16) | (((y * pg) / 255u) << 8) | ((y * pb) / 255u);
}

#endif /* NTVDMEX_PRESENT_SCALE_H */
