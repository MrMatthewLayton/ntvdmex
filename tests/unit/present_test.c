/* present_test.c -- off-VM battery for the Display settings' arithmetic
 * (src/vdd/present_scale.h).
 *
 * present_ddraw.c cannot be built here: it wants <ddraw.h>, an HWND and a
 * desktop. The two things the Display page actually decides -- where the frame
 * goes in the client area, and what the pixels are before they get there -- are
 * integer arithmetic on a buffer, and they are checked here, on the build
 * machine, so "the knob is wired" and "the knob is wired and right" are not the
 * same claim.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "present_scale.h"

static INT g_Total = 0, g_Failures = 0;
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

INT main(VOID)
{
    INT left, top, width, height;

    printf("== Display settings: aspect fit + Scale2x (present_scale.h) ==\n");

    /* ── ASPECT OFF IS THE HISTORICAL BEHAVIOUR AND MUST STAY EXACT. ───────────
         Every present this host has ever done filled the client area. A setting
         that is off has to leave that byte-for-byte alone, or turning the
         feature on becomes the safe option and nobody trusts the default. */
    PresentFit(1024, 600, 0, &left, &top, &width, &height);
    CHECK(left == 0 && top == 0 && width == 1024 && height == 600,
          "aspect off: the frame fills the client area, exactly as before");

    /* A window WIDER than 4:3 -> bars at the sides, frame as tall as the client. */
    PresentFit(1000, 600, 1, &left, &top, &width, &height);
    CHECK(width == 800 && height == 600, "aspect on, wide window: 4:3 box is 800x600");
    CHECK(left == 100 && top == 0,   "...centred, so the bars are equal at left and right");

    /* A window TALLER than 4:3 -> bars top and bottom. */
    PresentFit(640, 600, 1, &left, &top, &width, &height);
    CHECK(width == 640 && height == 480, "aspect on, tall window: 4:3 box is 640x480");
    CHECK(left == 0 && top == 60,    "...centred, so the bars are equal top and bottom");

    /* Exactly 4:3 -> no bars at all, and no off-by-one that would leave a
       one-pixel line of stale frame down one edge. */
    PresentFit(640, 480, 1, &left, &top, &width, &height);
    CHECK(left == 0 && top == 0 && width == 640 && height == 480,
          "aspect on, a 4:3 client: no bars and no off-by-one");
    PresentFit(1280, 960, 1, &left, &top, &width, &height);
    CHECK(left == 0 && top == 0 && width == 1280 && height == 960, "...at any 4:3 size");

    /* A client area can genuinely be zero-sized -- a minimised window reports it
       -- and the result still has to be something StretchDIBits will accept. */
    PresentFit(0, 0, 1, &left, &top, &width, &height);
    CHECK(width >= 1 && height >= 1, "a zero-sized client still yields a blittable rectangle");
    PresentFit(0, 0, 0, &left, &top, &width, &height);
    CHECK(width >= 1 && height >= 1, "...with aspect off too");

    /* ── WHICH SCALER DOES WHAT. ──────────────────────────────────────────────
         The ids ARE the Scaler combo's indices (settings.h), so a wrong answer
         here is a knob that silently selects the neighbouring effect. */
    CHECK(!PresentScalerDoubles(PRESENT_SCALER_NONE)
       && !PresentScalerHasScanlines(PRESENT_SCALER_NONE), "scaler None does nothing");
    CHECK(PresentScalerDoubles(PRESENT_SCALER_SCALE2X)
       && !PresentScalerHasScanlines(PRESENT_SCALER_SCALE2X), "Scale2x doubles, no scanlines");
    CHECK(!PresentScalerDoubles(PRESENT_SCALER_SCANLINES)
       && PresentScalerHasScanlines(PRESENT_SCALER_SCANLINES), "Scanlines masks, no doubling");
    CHECK(PresentScalerDoubles(PRESENT_SCALER_CRT)
       && PresentScalerHasScanlines(PRESENT_SCALER_CRT), "CRT is both");
    /* ⚠ hq2x is NOT implemented. It must behave as None -- visibly nothing --
         rather than half-selecting one of the effects it is not. */
    CHECK(!PresentScalerDoubles(PRESENT_SCALER_HQ2X)
       && !PresentScalerHasScanlines(PRESENT_SCALER_HQ2X),
          "hq2x is unimplemented and presents as None, not as something else");

    /* ── SCALE2X. ─────────────────────────────────────────────────────────────
         The rule is an EQUALITY test between neighbours, so running it on
         palette indices rather than colours is exact, not an approximation. */
    {
        static const BYTE flat3[9] = { 7,7,7, 7,7,7, 7,7,7 };
        BYTE destination[6 * 6 + 8];
        INT index, isOk = 1;
        memset(destination, 0xEE, sizeof destination);
        PresentScale2x8(flat3, 3, 3, 3, destination);
        for (index = 0; index < 36; ++index) if (destination[index] != 7) isOk = 0;
        CHECK(isOk, "Scale2x: a flat field doubles to the same flat field");
        isOk = 1;
        for (index = 36; index < (INT)sizeof destination; ++index) if (destination[index] != 0xEE) isOk = 0;
        CHECK(isOk, "Scale2x: writes exactly (2w x 2h) bytes and not one more");
    }
    {
        /* An isolated pixel has no neighbour it agrees with, so it must survive
           as a clean 2x2 block. Blurring it would be the failure mode that turns
           a mouse cursor or a 1-pixel font stem into mush. */
        static const BYTE dot[9] = { 1,1,1, 1,2,1, 1,1,1 };
        BYTE destination[36];
        PresentScale2x8(dot, 3, 3, 3, destination);
        CHECK(destination[2*6+2] == 2 && destination[2*6+3] == 2 && destination[3*6+2] == 2 && destination[3*6+3] == 2,
              "Scale2x: an isolated pixel stays a solid 2x2 block");
    }
    {
        /* The whole point: a staircase edge gains its corner. Centre pixel has
           B=D=2 and F=H=1, so the top-left quarter becomes 2 and the other three
           stay 1 -- the diagonal, not the step. */
        static const BYTE diag[9] = { 2,2,1,
                                         2,1,1,
                                         1,1,1 };
        BYTE destination[36];
        PresentScale2x8(diag, 3, 3, 3, destination);
        CHECK(destination[2*6+2] == 2, "Scale2x: the staircase corner is filled from the diagonal");
        CHECK(destination[2*6+3] == 1 && destination[3*6+2] == 1 && destination[3*6+3] == 1,
              "...and the other three quarters keep the centre pixel");
    }
    {
        /* Edges clamp their neighbours, so the border doubles plainly. This is
           also the read-past-the-buffer check: the source here is exactly 9
           bytes with a guard after it. */
        static const BYTE guarded[9 + 4] = { 2,2,1, 2,1,1, 1,1,1, 0,0,0,0 };
        BYTE destination[36];
        PresentScale2x8(guarded, 3, 3, 3, destination);
        CHECK(destination[0] == 2 && destination[1] == 2 && destination[6] == 2 && destination[7] == 2,
              "Scale2x: the top-left corner doubles plainly (neighbours clamp)");
        CHECK(destination[5*6+5] == 1, "Scale2x: ...and so does the bottom-right");
    }
    {
        /* A source with a STRIDE wider than its width -- which is what a real
           framebuffer row looks like -- must be walked by the stride, not the
           width, or every row after the first is shifted. */
        static const BYTE padded[3 * 5] = { 3,3,3, 9,9,
                                               3,3,3, 9,9,
                                               3,3,3, 9,9 };
        BYTE destination[36];
        INT index, isOk = 1;
        PresentScale2x8(padded, 3, 3, 5, destination);
        for (index = 0; index < 36; ++index) if (destination[index] != 3) isOk = 0;
        CHECK(isOk, "Scale2x: a stride wider than the width is respected");
    }


    printf("== Display: the aspect lock and the minimum window ==\n");

    /* ── #325: THE ASPECT LIST. Native (square pixels) is index 0 and the default; the
         forced ratios keep their indices; Stretch is last. (The registry NAME changed
         with the meaning of 0, in settings.h, so no stored value is reinterpreted.) */
    CHECK(PRESENT_ASPECT_NATIVE == 0 && PRESENT_ASPECT_4_3 == 1 && PRESENT_ASPECT_STRETCH == 4,
          "aspect indices: Native 0, 4:3 1, Stretch 4");
    {   INT numerator = 0, denominator = 0;
        PresentAspectRatio(PRESENT_ASPECT_16_9, &numerator, &denominator);
        CHECK(numerator == 16 && denominator == 9, "16:9 is 16/9");
        PresentAspectRatio(PRESENT_ASPECT_NATIVE, &numerator, &denominator);
        CHECK(numerator == 0 && denominator == 0, "Native has no ratio of its own (present_fit reads 0/0 as fill)");
        PresentTargetRatio(PRESENT_ASPECT_NATIVE, 320, 200, &numerator, &denominator);
        CHECK(numerator == 320 && denominator == 200, "Native's target ratio is the frame's own: 320x200 is 8:5"); }

    /* A wide window under 16:9 pillarboxes to 16:9. */
    PresentFit(1000, 400, PRESENT_ASPECT_16_9, &left, &top, &width, &height);
    CHECK(width == 711 && height == 400, "16:9 in a 1000x400 client is 711x400, height-bound");

    /* ── #325: THE PICTURE A WINDOW IS SIZED TO. 1x = one desktop pixel per frame pixel. */
    {   INT pictureWidth, pictureHeight;
        PresentWindowPicture(PRESENT_ASPECT_NATIVE, 320, 200, 1, &pictureWidth, &pictureHeight);
        CHECK(pictureWidth == 320 && pictureHeight == 200, "window: 320x200 at 1x is 320x200 -- no 640x480 floor");
        PresentWindowPicture(PRESENT_ASPECT_NATIVE, 320, 200, 3, &pictureWidth, &pictureHeight);
        CHECK(pictureWidth == 960 && pictureHeight == 600, "window: 320x200 at 3x is 960x600");
        PresentWindowPicture(PRESENT_ASPECT_NATIVE, 720, 400, 2, &pictureWidth, &pictureHeight);
        CHECK(pictureWidth == 1440 && pictureHeight == 800, "window: 720x400 text at 2x is 1440x800");
        PresentWindowPicture(PRESENT_ASPECT_STRETCH, 640, 200, 1, &pictureWidth, &pictureHeight);
        CHECK(pictureWidth == 640 && pictureHeight == 200, "window: Stretch in a window is Native (640x200 stays 16:5)");
        PresentWindowPicture(PRESENT_ASPECT_4_3, 320, 200, 2, &pictureWidth, &pictureHeight);
        CHECK(pictureWidth == 640 && pictureHeight == 480, "window: forced 4:3 keeps the width -- 320x200 at 2x is 640x480");
        PresentWindowPicture(PRESENT_ASPECT_16_9, 640, 200, 1, &pictureWidth, &pictureHeight);
        CHECK(pictureWidth == 640 && pictureHeight == 360, "window: forced 16:9 -- 640x200 at 1x is 640x360"); }

    /* ── #325: WHERE THE PICTURE GOES. */
    PresentLayout(PRESENT_ASPECT_NATIVE, PRESENT_FIT_WHOLE, 1, 1680, 1050, 320, 200, &left, &top, &width, &height);
    CHECK(width == 1600 && height == 1000 && left == 40 && top == 25,
          "layout: 320x200 on 1680x1050, whole pixels -> 5x = 1600x1000, centred");
    PresentLayout(PRESENT_ASPECT_NATIVE, PRESENT_FIT_WHOLE, 1, 1680, 1050, 720, 400, &left, &top, &width, &height);
    CHECK(width == 1440 && height == 800 && left == 120 && top == 125, "layout: 720x400 text -> 2x = 1440x800");
    PresentLayout(PRESENT_ASPECT_NATIVE, PRESENT_FIT_WHOLE, 1, 1680, 1050, 800, 600, &left, &top, &width, &height);
    CHECK(width == 800 && height == 600 && left == 440 && top == 225, "layout: 800x600 -> 1x, with the borders a physical limit");
    PresentLayout(PRESENT_ASPECT_NATIVE, PRESENT_FIT_WHOLE, 1, 1680, 1000, 1280, 1024, &left, &top, &width, &height);
    CHECK(width == 1250 && height == 1000, "layout: 1280x1024 that does not fit at 1x is scaled down on-ratio");
    PresentLayout(PRESENT_ASPECT_NATIVE, PRESENT_FIT_FILL, 1, 1680, 1050, 320, 200, &left, &top, &width, &height);
    CHECK(width == 1680 && height == 1050, "layout: Fill -> the largest 8:5 picture (the whole 1680x1050)");
    PresentLayout(PRESENT_ASPECT_4_3, PRESENT_FIT_WHOLE, 1, 1600, 1200, 320, 200, &left, &top, &width, &height);
    CHECK(width == 1600 && height == 1200 && left == 0, "layout: forced 4:3, whole pixels -> 5x6 = 1600x1200 EXACTLY 4:3");
    PresentLayout(PRESENT_ASPECT_4_3, PRESENT_FIT_WHOLE, 1, 1680, 1050, 320, 200, &left, &top, &width, &height);
    CHECK(width == 1400 && height == 1050, "layout: forced 4:3 with no exact whole pair that fits -> 1400x1050");
    PresentLayout(PRESENT_ASPECT_STRETCH, PRESENT_FIT_WHOLE, 1, 1680, 1050, 320, 200, &left, &top, &width, &height);
    CHECK(left == 0 && top == 0 && width == 1680 && height == 1050, "layout: Stretch on a screen fills it");
    PresentLayout(PRESENT_ASPECT_STRETCH, PRESENT_FIT_WHOLE, 0, 640, 400, 320, 200, &left, &top, &width, &height);
    CHECK(left == 0 && top == 0 && width == 640 && height == 400, "layout: in a window sized to the picture, the whole client");
    PresentLayout(PRESENT_ASPECT_4_3, PRESENT_FIT_WHOLE, 0, 640, 480, 320, 200, &left, &top, &width, &height);
    CHECK(left == 0 && top == 0 && width == 640 && height == 480, "layout: a forced-4:3 window fills its client exactly");

    /* #229: the colour filters recolour a COLOUR; Default must be the identity. */
    CHECK(PresentTint(0xFF123456u, PRESENT_TINT_DEFAULT) == 0xFF123456u, "tint: Default leaves a colour alone");
    CHECK(PresentTint(0xFFFFFFFFu, PRESENT_TINT_MONO_WHITE) == 0xFFFFFFFFu, "tint: white stays white on a paper-white screen");
    CHECK(PresentTint(0xFFFFFFFFu, PRESENT_TINT_MONO_GREEN) == 0xFF33FF33u, "tint: full brightness is the P1 green");
    CHECK(PresentTint(0xFFFFFFFFu, PRESENT_TINT_MONO_ORANGE) == 0xFFFFB000u, "tint: full brightness is amber");
    CHECK(PresentTint(0xFF000000u, PRESENT_TINT_MONO_ORANGE) == 0xFF000000u, "tint: black stays black");
    CHECK(PresentTint(0xFFFF0000u, PRESENT_TINT_MONO_WHITE) == 0xFF4C4C4Cu, "tint: pure red is 29.9% grey (Rec. 601)");
    /* #229 (user, s84): sepia is washed-out COLOUR -- the hue survives, muted and warm. */
    {   UINT32 white = PresentTint(0xFFFFFFFFu, PRESENT_TINT_SEPIA);
        UINT32 black = PresentTint(0xFF000000u, PRESENT_TINT_SEPIA);
        UINT32 red = PresentTint(0xFFFF0000u, PRESENT_TINT_SEPIA);
        UINT32 blue = PresentTint(0xFF0000FFu, PRESENT_TINT_SEPIA);
        #define CHANNEL(colour, shift) (((colour) >> (shift)) & 0xFFu)
        CHECK(CHANNEL(white,16) >= CHANNEL(white,8) && CHANNEL(white,8) > CHANNEL(white,0) && CHANNEL(white,0) >= 0xC0u,
              "tint: sepia white is cream -- warm, and still bright");
        CHECK(CHANNEL(black,16) > CHANNEL(black,8) && CHANNEL(black,8) > CHANNEL(black,0) && CHANNEL(black,16) < 0x40u,
              "tint: sepia black is a dark brown, not black");
        CHECK(CHANNEL(red,16) > CHANNEL(red,8) + 60u && CHANNEL(red,16) > CHANNEL(red,0) + 60u,
              "tint: sepia keeps the hue -- pure red is still clearly red");
        CHECK(CHANNEL(red,16) < 0xE0u && CHANNEL(red,8) > 0x30u,
              "tint: ...but washed out: less saturated than the input");
        CHECK(CHANNEL(blue,0) > CHANNEL(blue,16) && CHANNEL(blue,0) > CHANNEL(blue,8),
              "tint: sepia pure blue is still bluest (hue survives the warm cast)");
        #undef CHANNEL
    }
    printf("-- %d checks, %d failures --\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
