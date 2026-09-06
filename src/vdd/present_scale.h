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
static void present_fit(int dst_w, int dst_h, int aspect,
                        int *x, int *y, int *w, int *h)
{
    int fw, fh;
    if (dst_w < 1) dst_w = 1;
    if (dst_h < 1) dst_h = 1;
    if (!aspect) { *x = 0; *y = 0; *w = dst_w; *h = dst_h; return; }
    fw = dst_w; fh = dst_w * 3 / 4;             /* as wide as possible...          */
    if (fh > dst_h) { fh = dst_h; fw = dst_h * 4 / 3; }  /* ...unless too tall     */
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
