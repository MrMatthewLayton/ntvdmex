/*
 * present_scale.h -- the pure arithmetic behind the Display settings.
 *
 * WHY IT IS A HEADER OF ITS OWN. present_ddraw.c cannot be built off the VM: it
 * needs <ddraw.h>, an HWND and a desktop. The two decisions the Display page
 * actually makes -- WHERE the frame goes in the client area (aspect ratio) and
 * WHAT the pixels are before they get there (the scaler) -- are integer
 * arithmetic on a buffer, with no Windows in them at all. Kept here, they are
 * exercised by tools/dostest/present_test.c on the build machine, which is the
 * difference between "the knob is wired" and "the knob is wired and right".
 *
 * ⚠ ASPECT RATIO IS NOT THE FRAMEBUFFER'S SHAPE. Mode 13h is 320x200 -- 8:5 --
 *   and it was NEVER meant to look like that: the CRT displayed it at 4:3 with
 *   non-square pixels. So "correct aspect" letterboxes to 4:3, which STRETCHES
 *   200 lines taller than a square-pixel scale would, and the circles in a game's
 *   title screen come out round. Switching it off fills the client area, which is
 *   what this host has always done.
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

/* ── WHERE THE FRAME GOES. ───────────────────────────────────────────────────────
     Fills (x,y,w,h) within a dst_w x dst_h area. With `aspect` off that is the
     whole area, unchanged. With it on the frame is scaled to the largest 4:3 box
     that fits and CENTRED; the caller paints what is left over.
   ⚠ The source size is deliberately NOT consulted: every mode this host presents
     was displayed on a 4:3 screen, and using the framebuffer's own ratio would
     reproduce the very distortion the setting exists to remove. */
/* ── THE ASPECT CHOICES, AND THEY ARE ALSO THE REGISTRY VALUE. ───────────────────
     Indices of the AspectRatio combo in settings.h.
   ★ 0 and 1 KEEP THE MEANINGS THEY HAD when this was a checkbox: 0 was off and 1
     was "correct aspect", which was 4:3 -- so an existing registry value migrates
     for free and nobody's window changes shape on upgrade. The new ratios are
     appended, which is exactly the discipline the CPU speed list did NOT manage. */
/* ── #228 (docs/EMULATION.md): index 0 is AUTO -- the window/output takes the shape
     the CURRENT MODE is displayed at -- where it used to be None (free, fill). Auto is
     a setting, not a ratio: present_aspect_auto() RESOLVES it against the mode into an
     encoded fixed ratio (PRESENT_ASPECT_FIXED | n<<8 | d), which every fit below reads
     like the named ones. Callers resolve once and pass the result through unchanged.
     Stored 0 (None) becomes Auto on upgrade, which is the replacement the spec asks for. */
enum {
    PRESENT_ASPECT_NONE = 0,          /* = AUTO as a setting; 0/0 ("fill") only if unresolved */
    PRESENT_ASPECT_4_3,
    PRESENT_ASPECT_16_9,
    PRESENT_ASPECT_16_10,
    PRESENT_ASPECT_STRETCH,           /* #228: Auto in a window; FILL fullscreen / maximised */
    PRESENT_ASPECT_COUNT
};
#define PRESENT_ASPECT_AUTO   PRESENT_ASPECT_NONE
#define PRESENT_ASPECT_FIXED  0x10000
#define PRESENT_ASPECT_ITEMS "Auto|4:3|16:9|16:10|Stretch"

/* Resolve the SETTING against the mode on screen. Auto: the VGA and text modes (up to
   720x480) are shown 4:3, as a CRT showed them -- 320x200 had tall pixels; a VESA mode
   has square pixels, so its own w:h (1280x1024 -> 5:4, 640x400 -> 16:10). Named ratios
   pass through. No frame yet -> 4:3. */
static int present_aspect_auto(int setting, int mode_w, int mode_h, int vesa)
{
    int a, b, t;
    if (setting == PRESENT_ASPECT_STRETCH) setting = PRESENT_ASPECT_AUTO;   /* the window's shape */
    if (setting != PRESENT_ASPECT_AUTO) return setting;
    if (mode_w < 1 || mode_h < 1 || !vesa || (mode_w <= 720 && mode_h <= 480))
        return PRESENT_ASPECT_FIXED | (4 << 8) | 3;
    a = mode_w; b = mode_h;                        /* reduce w:h */
    while (b) { t = a % b; a = b; b = t; }
    if ((mode_w / a) > 255 || (mode_h / a) > 255) return PRESENT_ASPECT_FIXED | (4 << 8) | 3;
    return PRESENT_ASPECT_FIXED | ((mode_w / a) << 8) | (mode_h / a);
}

/* ── #228 (user, s84): STRETCH. "In a window, this is actually the same as Auto. On a
     physical screen, in fullscreen, or when the window is maximized, we stretch the
     image to fit the available space." So the WINDOW's shape resolves as Auto (above),
     and only the fit into an area the user did not size -- fullscreen, or maximised --
     fills it: 0 = unresolved = 0/0 = fill, which present_fit reads as the whole area. */
static int present_stretch_fills(int setting, int area_is_screen)
{ return setting == PRESENT_ASPECT_STRETCH && area_is_screen; }
static int present_aspect_for_area(int setting, int mode_w, int mode_h, int vesa, int area_is_screen)
{
    if (present_stretch_fills(setting, area_is_screen)) return 0;
    return present_aspect_auto(setting, mode_w, mode_h, vesa);
}

/* The ratio as a fraction. An unresolved Auto gives 0/0, which callers read as "fill". */
static void present_aspect_ratio(int aspect, int *n, int *d)
{
    if (aspect & PRESENT_ASPECT_FIXED) { *n = (aspect >> 8) & 0xFF; *d = aspect & 0xFF; return; }
    switch (aspect) {
    case PRESENT_ASPECT_4_3:   *n = 4;  *d = 3;  break;
    case PRESENT_ASPECT_16_9:  *n = 16; *d = 9;  break;
    case PRESENT_ASPECT_16_10: *n = 16; *d = 10; break;
    default:                   *n = 0;  *d = 0;  break;
    }
}

/* ── THE SMALLEST WINDOW THIS ASPECT ALLOWS. ─────────────────────────────────────
     At least PRESENT_MIN_W wide AND at least PRESENT_MIN_H tall, and on-aspect --
     so for a WIDE ratio the height binds first and forces extra width. 4:3 lands
     exactly on 640x480; 16:10 needs 768x480; 16:9 needs 853x480.
   With no lock there is nothing to satisfy but the floor itself. */
#define PRESENT_MIN_W 640
#define PRESENT_MIN_H 480
static void present_min_client(int aspect, int *w, int *h)
{
    int n, d;
    present_aspect_ratio(aspect, &n, &d);
    if (!n || !d) { *w = PRESENT_MIN_W; *h = PRESENT_MIN_H; return; }
    /* ⚠ ONE constraint binds and the other is then satisfied for free -- work out
         WHICH, and derive the other side from it. Ceiling-rounding both independently
         (the first cut) overshoots: 16:9 came out 854x481 instead of 853x480, i.e.
         a pixel proud of the floor on both axes for no reason. */
    *w = PRESENT_MIN_W;
    *h = (PRESENT_MIN_W * d + n / 2) / n;
    if (*h < PRESENT_MIN_H) {                    /* too short -> the HEIGHT binds */
        *h = PRESENT_MIN_H;
        *w = (PRESENT_MIN_H * n + d / 2) / d;
    }
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

/* ── ★★ SHARP PIXELS: WHOLE MULTIPLES, ONE FACTOR PER AXIS. ──────────────────────────
     present_fit above gives the biggest on-aspect rectangle that fits, and at a typical
     desktop size that is a FRACTIONAL multiple of the guest's frame: 1680/320 = 5.25.
     A stretch to 5.25x cannot put the same number of physical pixels under each guest
     pixel, so columns come out alternately 5 and 6 wide. DirectDraw's stretch blt is
     point-sampled on the drivers of this era, so this is not a soft interpolation --
     it is a visibly UNEVEN grid, which is what "the pixels look blurry" actually is.
     (User, 2026-09-10, on a 2560x1600 panel driven at 1680x1050.)

   ► SO SNAP EACH AXIS TO A WHOLE MULTIPLE -- AND ALLOW THE TWO TO DIFFER. Independent
     factors are what lets this serve the aspect setting at the same time as sharpness,
     because a DOS frame does not have square pixels: 320x200 shown at 4:3 needs a pixel
     that is 1.2 times taller than it is wide, and 5x6 delivers exactly that (1600x1200
     is precisely 4:3). One shared factor could only ever produce 8:5.

   ⚠ THE TWO GOALS GENUINELY CONFLICT AND THE CALLER SHOULD KNOW IT. At most sizes there
     is no integer pair that hits the aspect exactly, so we take the closest and then the
     largest -- sharp always, aspect as near as whole numbers allow. With PRESENT_ASPECT_
     NONE there is nothing to approximate and the rule collapses to the obvious one:
     square pixels, so nx == ny.

   Search cost is (dst_w/src_w) * (dst_h/src_h) iterations -- at most a few hundred on any
   real display, and only on the present path, which already touches every pixel. */
static void present_fit_int(int dst_w, int dst_h, int src_w, int src_h, int aspect,
                            int *x, int *y, int *w, int *h)
{
    int n, d, nx, ny, mx, my, bx = 1, by = 1;
    long best_err = -1, best_area = -1;
    if (dst_w < 1) dst_w = 1;
    if (dst_h < 1) dst_h = 1;
    /* No frame yet, or one too big to multiply at all: fall back to the smooth fit
       rather than inventing a factor of zero. */
    if (src_w < 1 || src_h < 1 || src_w > dst_w || src_h > dst_h) {
        present_fit(dst_w, dst_h, aspect, x, y, w, h);
        return;
    }
    mx = dst_w / src_w; my = dst_h / src_h;
    if (mx < 1) mx = 1;
    if (my < 1) my = 1;
    present_aspect_ratio(aspect, &n, &d);
    if (!n || !d) {                          /* square pixels: one factor, both axes */
        bx = by = (mx < my) ? mx : my;
    } else {
        for (nx = 1; nx <= mx; ++nx) {
            for (ny = 1; ny <= my; ++ny) {
                /* Aspect error, cross-multiplied so it stays integer: we want
                   (src_w*nx) / (src_h*ny) == n/d, i.e. src_w*nx*d == src_h*ny*n. */
                long ww = (long)src_w * nx, hh = (long)src_h * ny;
                long err = ww * d - hh * n;
                long area = ww * hh;
                if (err < 0) err = -err;
                /* Scale the error against the rectangle so a big near-miss is not
                   ranked worse than a tiny one; then prefer the LARGER picture among
                   equally-accurate pairs, or every aspect would pick 1x1. */
                err = (err * 1000) / (hh * n);
                if (best_err < 0 || err < best_err
                    || (err == best_err && area > best_area)) {
                    best_err = err; best_area = area; bx = nx; by = ny;
                }
            }
        }
    }
    *w = src_w * bx; *h = src_h * by;
    *x = (dst_w - *w) / 2;
    *y = (dst_h - *h) / 2;
}

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
