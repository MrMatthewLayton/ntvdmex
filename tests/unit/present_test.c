/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for the Display settings' arithmetic
 * (src/vdd/present_scale.h).
 *
 * present_ddraw.c cannot be built here: it wants <ddraw.h>, an HWND and a
 * desktop. The two things the Display page actually decides -- where the frame
 * goes in the client area, and what the pixels are before they get there -- are
 * integer arithmetic on a buffer, and they are checked here, on the build
 * machine, so "the knob is wired" and "the knob is wired and right" are not the
 * same claim.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "present_scale.h"
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static INT g_Total = 0;
static INT g_Failures = 0;

INT main(VOID)
{
    INT left;
    INT top;
    INT width;
    INT height;

    printf("== Display settings: aspect fit + scalers (present_scale.h) ==\n");

    /* ASPECT OFF IS THE HISTORICAL BEHAVIOUR AND MUST STAY EXACT:
     * Every present this host has ever done filled the client area. A setting
     * that is off has to leave that byte-for-byte alone, or turning the
     * feature on becomes the safe option and nobody trusts the default.
     */
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
     * one-pixel line of stale frame down one edge.
     */
    PresentFit(640, 480, 1, &left, &top, &width, &height);
    CHECK(left == 0 && top == 0 && width == 640 && height == 480,
          "aspect on, a 4:3 client: no bars and no off-by-one");
    PresentFit(1280, 960, 1, &left, &top, &width, &height);
    CHECK(left == 0 && top == 0 && width == 1280 && height == 960, "...at any 4:3 size");

    /* A client area can genuinely be zero-sized -- a minimised window reports it
     * -- and the result still has to be something StretchDIBits will accept.
     */
    PresentFit(0, 0, 1, &left, &top, &width, &height);
    CHECK(width >= 1 && height >= 1, "a zero-sized client still yields a blittable rectangle");
    PresentFit(0, 0, 0, &left, &top, &width, &height);
    CHECK(width >= 1 && height >= 1, "...with aspect off too");

    /* WHICH SCALER DOES WHAT:
     * The ids ARE the Scaler combo's indices (settings.h) and the stored values, so a
     * wrong answer here is a knob that silently selects the neighbouring effect.
     * #158: Scale3x took hq2x's index; Scanlines and CRT kept theirs.
     */
    CHECK(PRESENT_SCALER_NONE == 0 && PRESENT_SCALER_SCALE2X == 1 && PRESENT_SCALER_SCALE3X == 2
       && PRESENT_SCALER_SCANLINES == 3 && PRESENT_SCALER_CRT == 4,
          "scaler indices: None 0, Scale2x 1, Scale3x 2, Scanlines 3, CRT 4");
    CHECK(strcmp(PRESENT_SCALER_ITEMS, "None|Scale2x|Scale3x|Scanlines|CRT") == 0,
          "...and the list the dialog and the menu show is in that order");
    CHECK(!PresentScalerHasEffect(PRESENT_SCALER_NONE) && PresentScalerFactor(PRESENT_SCALER_NONE) == 1
       && !PresentScalerHasScanlines(PRESENT_SCALER_NONE), "scaler None does nothing");
    CHECK(PresentScalerFactor(PRESENT_SCALER_SCALE2X) == 2
       && !PresentScalerHasScanlines(PRESENT_SCALER_SCALE2X), "Scale2x enlarges by 2, no scanlines");
    CHECK(PresentScalerFactor(PRESENT_SCALER_SCALE3X) == 3
       && !PresentScalerHasScanlines(PRESENT_SCALER_SCALE3X), "Scale3x enlarges by 3, no scanlines");
    CHECK(PresentScalerFactor(PRESENT_SCALER_SCANLINES) == 1
       && PresentScalerHasScanlines(PRESENT_SCALER_SCANLINES), "Scanlines shades, no pixel-art scaling");
    CHECK(PresentScalerFactor(PRESENT_SCALER_CRT) == 1
       && PresentScalerHasScanlines(PRESENT_SCALER_CRT), "CRT shades, no pixel-art scaling");
    CHECK(PresentScalerHasEffect(1) && PresentScalerHasEffect(4) && !PresentScalerHasEffect(5) && !PresentScalerHasEffect(-1),
          "a stored value outside the list draws no effect");

    /* SCALE2X:
     * The rule is an EQUALITY test between neighbours, so it needs no colour distance.
     */
    {
        static const UINT32 flat3[9] = { 7,7,7, 7,7,7, 7,7,7 };
        UINT32 destination[6 * 6 + 8];
        INT index;
        INT isOk = 1;

        for (index = 0; index < (INT)(sizeof destination / sizeof destination[0]); ++index)
            destination[index] = 0xEEEEEEEEu;

        PresentScaleEpx(flat3, 3, 3, 3, 2, destination);

        for (index = 0; index < 36; ++index)
            if (destination[index] != 7)
                isOk = 0;

        CHECK(isOk, "Scale2x: a flat field doubles to the same flat field");
        isOk = 1;

        for (index = 36; index < (INT)(sizeof destination / sizeof destination[0]); ++index)
            if (destination[index] != 0xEEEEEEEEu)
                isOk = 0;

        CHECK(isOk, "Scale2x: writes exactly (2w x 2h) pixels and not one more");
    }
    {
        /* An isolated pixel has no neighbour it agrees with, so it must survive
         * as a clean 2x2 block. Blurring it would be the failure mode that turns
         * a mouse cursor or a 1-pixel font stem into mush.
         */
        static const UINT32 dot[9] = { 1,1,1, 1,2,1, 1,1,1 };
        UINT32 destination[36];
        PresentScaleEpx(dot, 3, 3, 3, 2, destination);
        CHECK(destination[2*6+2] == 2 && destination[2*6+3] == 2 && destination[3*6+2] == 2 && destination[3*6+3] == 2,
              "Scale2x: an isolated pixel stays a solid 2x2 block");
    }
    {
        /* The whole point: a staircase edge gains its corner. Centre pixel has
         * B=D=2 and F=H=1, so the top-left quarter becomes 2 and the other three
         * stay 1 -- the diagonal, not the step.
         */
        static const UINT32 diag[9] = { 2,2,1,
                                        2,1,1,
                                        1,1,1 };
        UINT32 destination[36];
        PresentScaleEpx(diag, 3, 3, 3, 2, destination);
        CHECK(destination[2*6+2] == 2, "Scale2x: the staircase corner is filled from the diagonal");
        CHECK(destination[2*6+3] == 1 && destination[3*6+2] == 1 && destination[3*6+3] == 1,
              "...and the other three quarters keep the centre pixel");
        CHECK(destination[0] == 2 && destination[1] == 2 && destination[6] == 2 && destination[7] == 2,
              "Scale2x: the top-left corner doubles plainly (neighbours clamp)");
        CHECK(destination[5*6+5] == 1, "Scale2x: ...and so does the bottom-right");
    }
    {
        /* A source with a STRIDE wider than its width -- which is what a real
         * framebuffer row looks like -- must be walked by the stride, not the
         * width, or every row after the first is shifted.
         */
        static const UINT32 padded[3 * 5] = { 3,3,3, 9,9,
                                              3,3,3, 9,9,
                                              3,3,3, 9,9 };
        UINT32 destination[36];
        INT index;
        INT isOk = 1;
        PresentScaleEpx(padded, 3, 3, 5, 2, destination);

        for (index = 0; index < 36; ++index)
            if (destination[index] != 3)
                isOk = 0;

        CHECK(isOk, "Scale2x: a stride wider than the width is respected");
    }
    {
        /* Direct colour: the values are whole ARGB words, not indices. */
        static const UINT32 colours[4] = { 0x00FF0000u, 0x0000FF00u,
                                           0x0000FF00u, 0x0000FF00u };
        UINT32 destination[16];
        PresentScaleEpx(colours, 2, 2, 2, 2, destination);
        CHECK(destination[0] == 0x00FF0000u && destination[5] == 0x0000FF00u && destination[15] == 0x0000FF00u,
              "Scale2x: works on ARGB words (direct-colour frames): the red corner pixel loses its inner corner to green");
    }

    /* SCALE3X:
     * The same equality rule at 3x3. The edge middles take a neighbour only where the
     * corner beyond disagrees with the centre, so a one-pixel line stays one pixel
     * (three output pixels) wide.
     */
    {
        static const UINT32 dot[9] = { 1,1,1, 1,2,1, 1,1,1 };
        UINT32 destination[81];
        INT row;
        INT column;
        INT isOk = 1;
        PresentScaleEpx(dot, 3, 3, 3, 3, destination);

        for (row = 3; row < 6; ++row)
            for (column = 3; column < 6; ++column)
                if (destination[row * 9 + column] != 2)
                    isOk = 0;

        CHECK(isOk && destination[2*9+3] == 1 && destination[3*9+2] == 1,
              "Scale3x: an isolated pixel stays a solid 3x3 block");
    }
    {
        static const UINT32 diag[9] = { 2,2,1,
                                        2,1,1,
                                        1,1,1 };
        UINT32 destination[81];
        INT index;
        INT others = 0;
        PresentScaleEpx(diag, 3, 3, 3, 3, destination);

        for (index = 1; index < 9; ++index)
            if (destination[(3 + index / 3) * 9 + 3 + index % 3] != 1)
                ++others;

        CHECK(destination[3*9+3] == 2 && others == 0,
              "Scale3x: the staircase gains only its corner sub-pixel; the other eight keep the centre");
    }
    {
        static const UINT32 line[9] = { 1,2,1,
                                        1,2,1,
                                        1,2,1 };
        static const UINT32 expected[9] = { 1,1,1, 2,2,2, 1,1,1 };
        UINT32 destination[81];
        INT column;
        INT isOk = 1;
        PresentScaleEpx(line, 3, 3, 3, 3, destination);

        for (column = 0; column < 9; ++column)
            if (destination[4 * 9 + column] != expected[column])
                isOk = 0;

        CHECK(isOk, "Scale3x: a one-pixel line stays exactly three output pixels wide");
    }

    printf("== Display: the effect pass (#158) ==\n");

    /* THE BUFFER SCALE: never more than the picture's whole scale, a divisor of it when
     * one fits, and 1 (no effect) under 2x.
     */
    CHECK(PresentEffectScale(320, 200, 640, 400, 1920u * 1440u) == 2, "effect scale: 320x200 in a 2x picture draws at 2");
    CHECK(PresentEffectScale(320, 200, 960, 600, 1920u * 1440u) == 3, "...in a 3x picture at 3");
    CHECK(PresentEffectScale(320, 200, 800, 500, 1920u * 1440u) == 2, "...in a 2.5x picture (Fill) at 2, the stretch does the rest");
    CHECK(PresentEffectScale(320, 200, 2560, 1600, 1920u * 1440u) == 4,
          "...in an 8x picture too big for the buffer, at 4: a divisor, so the 2x stretch after is exact");
    CHECK(PresentEffectScale(320, 200, 2240, 1400, 1920u * 1440u) == 6,
          "...at 7x (no divisor fits) the largest that fits, 6");
    CHECK(PresentEffectScale(320, 200, 500, 300, 1920u * 1440u) == 1, "...under 2x: no effect (1)");
    CHECK(PresentEffectScale(1280, 1024, 2560, 2048, 1920u * 1440u) == 1, "...a frame too big for even a 2x buffer: no effect");
    CHECK(PresentEffectScale(0, 0, 640, 400, 1920u * 1440u) == 1, "...a frame with no size: no effect");

    /* SCANLINES BELONG TO THE GAME'S ROWS: the bottom half of every frame row's
     * screen rows, at every scale -- not every other screen row.
     */
    {
        static const UINT32 white[2] = { 0x00FFFFFFu, 0x00FFFFFFu };
        UINT32 destination[4 * 4 * 2];
        UINT32 scratch[4 * 4 * 2];

        PresentEffectRender(white, 2, 1, 2, PRESENT_SCALER_SCANLINES, 2, scratch, destination);
        CHECK(destination[0] == 0x00FFFFFFu && destination[3] == 0x00FFFFFFu && destination[4] == 0x007F7F7Fu && destination[7] == 0x007F7F7Fu,
              "Scanlines at 2x: row 0 full, row 1 at half");

        PresentEffectRender(white, 2, 1, 2, PRESENT_SCALER_SCANLINES, 3, scratch, destination);
        CHECK(destination[0] == 0x00FFFFFFu && destination[6] == 0x00FFFFFFu && destination[12] == 0x007F7F7Fu,
              "Scanlines at 3x: two rows full, the third dark (not 2-then-1 alternating with 1-then-2)");

        PresentEffectRender(white, 2, 1, 2, PRESENT_SCALER_SCANLINES, 4, scratch, destination);
        CHECK(destination[8] == 0x00FFFFFFu && destination[16] == 0x007F7F7Fu && destination[24] == 0x007F7F7Fu,
              "Scanlines at 4x: rows 2 and 3 dark");
    }
    {
        /* Two frame rows at 3x: the dark row repeats with the frame row, rows 2 and 5. */
        static const UINT32 white[2] = { 0x00FFFFFFu, 0x00FFFFFFu };
        UINT32 destination[3 * 6];
        UINT32 scratch[3 * 6];
        PresentEffectRender(white, 1, 2, 1, PRESENT_SCALER_SCANLINES, 3, scratch, destination);
        CHECK(destination[1*3] == 0x00FFFFFFu && destination[2*3] == 0x007F7F7Fu && destination[3*3] == 0x00FFFFFFu
           && destination[5*3] == 0x007F7F7Fu, "Scanlines: one dark band per frame row (rows 2 and 5 at 3x)");
    }
    {
        /* Scale2x at a larger buffer scale: EPX at 2, then each sub-pixel repeated. */
        static const UINT32 dot[9] = { 1,1,1, 1,2,1, 1,1,1 };
        UINT32 destination[12 * 12 + 4];
        UINT32 scratch[6 * 6];
        INT index;
        INT isOk = 1;

        destination[144] = 0xEEEEEEEEu;
        PresentEffectRender(dot, 3, 3, 3, PRESENT_SCALER_SCALE2X, 4, scratch, destination);

        for (index = 0; index < 144; ++index)
        {
            INT row = index / 12;
            INT column = index % 12;
            UINT32 expected = (row >= 4 && row < 8 && column >= 4 && column < 8) ? 2u : 1u;

            if (destination[index] != expected)
                isOk = 0;
        }

        CHECK(isOk && destination[144] == 0xEEEEEEEEu, "Scale2x drawn at 4x: a 4x4 block, nothing written past the buffer");

        PresentEffectRender(dot, 3, 3, 3, PRESENT_SCALER_SCALE3X, 2, scratch, destination);
        CHECK(destination[2*6+2] == 2 && destination[1*6+1] == 1, "Scale3x into a 2x buffer runs as Scale2x (it never draws past the buffer)");
    }
    {
        /* CRT on white at 3x: columns red, green, blue; each keeps its channel and 70% of
         * the other two, lifted 25% (clamped); the dark row keeps 60%.
         */
        static const UINT32 white[1] = { 0x00FFFFFFu };
        static const UINT32 black[1] = { 0x00000000u };
        UINT32 destination[9];
        UINT32 scratch[9];

        PresentEffectRender(white, 1, 1, 1, PRESENT_SCALER_CRT, 3, scratch, destination);
        CHECK(destination[0] == 0x00FFDEDEu && destination[1] == 0x00DEFFDEu && destination[2] == 0x00DEDEFFu,
              "CRT: the mask columns run red, green, blue");
        CHECK(destination[6] == 0x00BE8585u, "CRT: the dark row of a white pixel is (190,133,133) in a red column");

        PresentEffectRender(black, 1, 1, 1, PRESENT_SCALER_CRT, 3, scratch, destination);
        CHECK(destination[0] == 0 && destination[8] == 0, "CRT: black stays black");
    }

    printf("== Display: the aspect lock and the minimum window ==\n");

    /* #325: THE ASPECT LIST. Native (square pixels) is index 0 and the default; the
     * forced ratios keep their indices; Stretch is last. (The registry NAME changed
     * with the meaning of 0, in settings.h, so no stored value is reinterpreted.)
     */
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

    /* #325: THE PICTURE A WINDOW IS SIZED TO. 1x = one desktop pixel per frame pixel. */
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

    /* #325: WHERE THE PICTURE GOES. */
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
