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
enum {
    PRESENT_ASPECT_NONE = 0,
    PRESENT_ASPECT_4_3,
    PRESENT_ASPECT_16_9,
    PRESENT_ASPECT_16_10,
    PRESENT_ASPECT_COUNT
};
#define PRESENT_ASPECT_ITEMS "None|4:3|16:9|16:10"

/* The ratio as a fraction. NONE gives 0/0, which every caller reads as "fill". */
static void present_aspect_ratio(int aspect, int *n, int *d)
{
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

#endif /* NTVDMEX_PRESENT_SCALE_H */
