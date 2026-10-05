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
#include "present_scale.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

int main(void)
{
    int x, y, w, h;

    printf("== Display settings: aspect fit + Scale2x (present_scale.h) ==\n");

    /* ── ASPECT OFF IS THE HISTORICAL BEHAVIOUR AND MUST STAY EXACT. ───────────
         Every present this host has ever done filled the client area. A setting
         that is off has to leave that byte-for-byte alone, or turning the
         feature on becomes the safe option and nobody trusts the default. */
    present_fit(1024, 600, 0, &x, &y, &w, &h);
    CHECK(x == 0 && y == 0 && w == 1024 && h == 600,
          "aspect off: the frame fills the client area, exactly as before");

    /* A window WIDER than 4:3 -> bars at the sides, frame as tall as the client. */
    present_fit(1000, 600, 1, &x, &y, &w, &h);
    CHECK(w == 800 && h == 600, "aspect on, wide window: 4:3 box is 800x600");
    CHECK(x == 100 && y == 0,   "...centred, so the bars are equal at left and right");

    /* A window TALLER than 4:3 -> bars top and bottom. */
    present_fit(640, 600, 1, &x, &y, &w, &h);
    CHECK(w == 640 && h == 480, "aspect on, tall window: 4:3 box is 640x480");
    CHECK(x == 0 && y == 60,    "...centred, so the bars are equal top and bottom");

    /* Exactly 4:3 -> no bars at all, and no off-by-one that would leave a
       one-pixel line of stale frame down one edge. */
    present_fit(640, 480, 1, &x, &y, &w, &h);
    CHECK(x == 0 && y == 0 && w == 640 && h == 480,
          "aspect on, a 4:3 client: no bars and no off-by-one");
    present_fit(1280, 960, 1, &x, &y, &w, &h);
    CHECK(x == 0 && y == 0 && w == 1280 && h == 960, "...at any 4:3 size");

    /* A client area can genuinely be zero-sized -- a minimised window reports it
       -- and the result still has to be something StretchDIBits will accept. */
    present_fit(0, 0, 1, &x, &y, &w, &h);
    CHECK(w >= 1 && h >= 1, "a zero-sized client still yields a blittable rectangle");
    present_fit(0, 0, 0, &x, &y, &w, &h);
    CHECK(w >= 1 && h >= 1, "...with aspect off too");

    /* ── WHICH SCALER DOES WHAT. ──────────────────────────────────────────────
         The ids ARE the Scaler combo's indices (settings.h), so a wrong answer
         here is a knob that silently selects the neighbouring effect. */
    CHECK(!present_scaler_doubles(PRESENT_SCALER_NONE)
       && !present_scaler_scanlines(PRESENT_SCALER_NONE), "scaler None does nothing");
    CHECK(present_scaler_doubles(PRESENT_SCALER_SCALE2X)
       && !present_scaler_scanlines(PRESENT_SCALER_SCALE2X), "Scale2x doubles, no scanlines");
    CHECK(!present_scaler_doubles(PRESENT_SCALER_SCANLINES)
       && present_scaler_scanlines(PRESENT_SCALER_SCANLINES), "Scanlines masks, no doubling");
    CHECK(present_scaler_doubles(PRESENT_SCALER_CRT)
       && present_scaler_scanlines(PRESENT_SCALER_CRT), "CRT is both");
    /* ⚠ hq2x is NOT implemented. It must behave as None -- visibly nothing --
         rather than half-selecting one of the effects it is not. */
    CHECK(!present_scaler_doubles(PRESENT_SCALER_HQ2X)
       && !present_scaler_scanlines(PRESENT_SCALER_HQ2X),
          "hq2x is unimplemented and presents as None, not as something else");

    /* ── SCALE2X. ─────────────────────────────────────────────────────────────
         The rule is an EQUALITY test between neighbours, so running it on
         palette indices rather than colours is exact, not an approximation. */
    {
        static const uint8_t flat3[9] = { 7,7,7, 7,7,7, 7,7,7 };
        uint8_t dst[6 * 6 + 8];
        int i, ok = 1;
        memset(dst, 0xEE, sizeof dst);
        present_scale2x_8(flat3, 3, 3, 3, dst);
        for (i = 0; i < 36; ++i) if (dst[i] != 7) ok = 0;
        CHECK(ok, "Scale2x: a flat field doubles to the same flat field");
        ok = 1;
        for (i = 36; i < (int)sizeof dst; ++i) if (dst[i] != 0xEE) ok = 0;
        CHECK(ok, "Scale2x: writes exactly (2w x 2h) bytes and not one more");
    }
    {
        /* An isolated pixel has no neighbour it agrees with, so it must survive
           as a clean 2x2 block. Blurring it would be the failure mode that turns
           a mouse cursor or a 1-pixel font stem into mush. */
        static const uint8_t dot[9] = { 1,1,1, 1,2,1, 1,1,1 };
        uint8_t dst[36];
        present_scale2x_8(dot, 3, 3, 3, dst);
        CHECK(dst[2*6+2] == 2 && dst[2*6+3] == 2 && dst[3*6+2] == 2 && dst[3*6+3] == 2,
              "Scale2x: an isolated pixel stays a solid 2x2 block");
    }
    {
        /* The whole point: a staircase edge gains its corner. Centre pixel has
           B=D=2 and F=H=1, so the top-left quarter becomes 2 and the other three
           stay 1 -- the diagonal, not the step. */
        static const uint8_t diag[9] = { 2,2,1,
                                         2,1,1,
                                         1,1,1 };
        uint8_t dst[36];
        present_scale2x_8(diag, 3, 3, 3, dst);
        CHECK(dst[2*6+2] == 2, "Scale2x: the staircase corner is filled from the diagonal");
        CHECK(dst[2*6+3] == 1 && dst[3*6+2] == 1 && dst[3*6+3] == 1,
              "...and the other three quarters keep the centre pixel");
    }
    {
        /* Edges clamp their neighbours, so the border doubles plainly. This is
           also the read-past-the-buffer check: the source here is exactly 9
           bytes with a guard after it. */
        static const uint8_t guarded[9 + 4] = { 2,2,1, 2,1,1, 1,1,1, 0,0,0,0 };
        uint8_t dst[36];
        present_scale2x_8(guarded, 3, 3, 3, dst);
        CHECK(dst[0] == 2 && dst[1] == 2 && dst[6] == 2 && dst[7] == 2,
              "Scale2x: the top-left corner doubles plainly (neighbours clamp)");
        CHECK(dst[5*6+5] == 1, "Scale2x: ...and so does the bottom-right");
    }
    {
        /* A source with a STRIDE wider than its width -- which is what a real
           framebuffer row looks like -- must be walked by the stride, not the
           width, or every row after the first is shifted. */
        static const uint8_t padded[3 * 5] = { 3,3,3, 9,9,
                                               3,3,3, 9,9,
                                               3,3,3, 9,9 };
        uint8_t dst[36];
        int i, ok = 1;
        present_scale2x_8(padded, 3, 3, 5, dst);
        for (i = 0; i < 36; ++i) if (dst[i] != 3) ok = 0;
        CHECK(ok, "Scale2x: a stride wider than the width is respected");
    }


    printf("== Display: the aspect lock and the minimum window ==\n");

    /* ── THE ASPECT LIST IS NOW A FOUR-WAY, AND 0/1 MUST STILL MEAN WHAT THEY DID.
         It used to be a checkbox: 0 = fill, 1 = "correct aspect" = 4:3. Anything
         already in a registry has to survive that becoming a combo, or every
         existing install's window quietly changes shape on upgrade. */
    CHECK(PRESENT_ASPECT_NONE == 0, "aspect 0 is still None, so an old unchecked box still fills");
    CHECK(PRESENT_ASPECT_4_3 == 1,  "aspect 1 is still 4:3, so an old checked box still means 4:3");

    {   int n = 0, d = 0;
        present_aspect_ratio(PRESENT_ASPECT_16_9, &n, &d);
        CHECK(n == 16 && d == 9, "16:9 is 16/9");
        present_aspect_ratio(PRESENT_ASPECT_NONE, &n, &d);
        CHECK(n == 0 && d == 0, "None has no ratio at all -- callers read that as 'fill'"); }

    /* A wide window under 16:9 pillarboxes to 16:9, not to the old hard-coded 4:3. */
    present_fit(1000, 400, PRESENT_ASPECT_16_9, &x, &y, &w, &h);
    CHECK(w == 711 && h == 400, "16:9 in a 1000x400 client is 711x400, height-bound");

    /* ── ★ THE MINIMUM WINDOW, WHERE THE ASPECT LOCK AND THE 640x480 FLOOR MEET.
         On-aspect, at least 640 wide AND at least 480 tall -- so for a ratio WIDER
         than 4:3 the height binds first and drags the width up past 640. Getting it
         backwards gives a 16:9 minimum of 640x360, which is under the very floor
         the rule exists to enforce. */
    {   int mw = 0, mh = 0;
        present_min_client(PRESENT_ASPECT_NONE, &mw, &mh);
        CHECK(mw == 640 && mh == 480, "no lock: the minimum is just the 640x480 floor");
        present_min_client(PRESENT_ASPECT_4_3, &mw, &mh);
        CHECK(mw == 640 && mh == 480, "4:3: both constraints bind at once -- exactly 640x480");
        present_min_client(PRESENT_ASPECT_16_10, &mw, &mh);
        CHECK(mw == 768 && mh == 480, "16:10: the 480 height binds first, so 768x480");
        present_min_client(PRESENT_ASPECT_16_9, &mw, &mh);
        CHECK(mw == 853 && mh == 480, "16:9: wider still, so 853x480"); }

    {   int i, mw = 0, mh = 0, ok2 = 1;
        for (i = 0; i < PRESENT_ASPECT_COUNT; ++i) {
            present_min_client(i, &mw, &mh);
            if (mw < 640 || mh < 480) ok2 = 0;
        }
        CHECK(ok2, "no aspect can produce a minimum below 640x480 -- that is the floor"); }

    /* #228: Auto resolves against the mode: VGA/text 4:3 (a CRT's shape), VESA square. */
    {   int n, d;
        present_aspect_ratio(present_aspect_auto(PRESENT_ASPECT_AUTO, 320, 200, 0), &n, &d);
        CHECK(n == 4 && d == 3, "auto: 320x200 (mode 13h) shows 4:3, tall pixels as on a CRT");
        present_aspect_ratio(present_aspect_auto(PRESENT_ASPECT_AUTO, 720, 400, 0), &n, &d);
        CHECK(n == 4 && d == 3, "auto: 720x400 text shows 4:3");
        present_aspect_ratio(present_aspect_auto(PRESENT_ASPECT_AUTO, 1280, 1024, 1), &n, &d);
        CHECK(n == 5 && d == 4, "auto: VESA 1280x1024 is square-pixel 5:4");
        present_aspect_ratio(present_aspect_auto(PRESENT_ASPECT_AUTO, 1024, 768, 1), &n, &d);
        CHECK(n == 4 && d == 3, "auto: VESA 1024x768 is 4:3");
        CHECK(present_aspect_auto(PRESENT_ASPECT_16_9, 320, 200, 0) == PRESENT_ASPECT_16_9,
              "auto: a named ratio passes straight through");
        {   int x, y, w, h;
            present_fit(1000, 1000, present_aspect_auto(PRESENT_ASPECT_AUTO, 320, 200, 0), &x, &y, &w, &h);
            CHECK(w == 1000 && h == 750 && y == 125, "auto: the fit letterboxes a 4:3 mode in a square client"); } }

    /* #228 (user, s84): Stretch is Auto for the window's shape, and fills an area the user
       did not size -- fullscreen or maximised -- with no bars. */
    {   int x, y, w, h, n, d;
        CHECK(PRESENT_ASPECT_STRETCH == 4, "stretch is APPENDED, so every stored aspect keeps its meaning");
        present_aspect_ratio(present_aspect_auto(PRESENT_ASPECT_STRETCH, 320, 200, 0), &n, &d);
        CHECK(n == 4 && d == 3, "stretch: the window's shape is Auto's (320x200 -> 4:3)");
        present_fit(1000, 400, present_aspect_for_area(PRESENT_ASPECT_STRETCH, 320, 200, 0, 0), &x, &y, &w, &h);
        CHECK(w == 533 && h == 400, "stretch: in a window it letterboxes like Auto");
        present_fit(1680, 1050, present_aspect_for_area(PRESENT_ASPECT_STRETCH, 320, 200, 0, 1), &x, &y, &w, &h);
        CHECK(x == 0 && y == 0 && w == 1680 && h == 1050, "stretch: fullscreen/maximised fills the whole area");
        present_fit(1680, 1050, present_aspect_for_area(PRESENT_ASPECT_AUTO, 320, 200, 0, 1), &x, &y, &w, &h);
        CHECK(w == 1400 && h == 1050, "auto: fullscreen still keeps 4:3 -- only Stretch fills"); }

    /* #229: the colour filters recolour a COLOUR; Default must be the identity. */
    CHECK(present_tint(0xFF123456u, PRESENT_TINT_DEFAULT) == 0xFF123456u, "tint: Default leaves a colour alone");
    CHECK(present_tint(0xFFFFFFFFu, PRESENT_TINT_MONO_WHITE) == 0xFFFFFFFFu, "tint: white stays white on a paper-white screen");
    CHECK(present_tint(0xFFFFFFFFu, PRESENT_TINT_MONO_GREEN) == 0xFF33FF33u, "tint: full brightness is the P1 green");
    CHECK(present_tint(0xFFFFFFFFu, PRESENT_TINT_MONO_ORANGE) == 0xFFFFB000u, "tint: full brightness is amber");
    CHECK(present_tint(0xFF000000u, PRESENT_TINT_MONO_ORANGE) == 0xFF000000u, "tint: black stays black");
    CHECK(present_tint(0xFFFF0000u, PRESENT_TINT_MONO_WHITE) == 0xFF4C4C4Cu, "tint: pure red is 29.9% grey (Rec. 601)");
    /* #229 (user, s84): sepia is washed-out COLOUR -- the hue survives, muted and warm. */
    {   uint32_t w = present_tint(0xFFFFFFFFu, PRESENT_TINT_SEPIA);
        uint32_t k = present_tint(0xFF000000u, PRESENT_TINT_SEPIA);
        uint32_t rd = present_tint(0xFFFF0000u, PRESENT_TINT_SEPIA);
        uint32_t bl = present_tint(0xFF0000FFu, PRESENT_TINT_SEPIA);
        #define CH(c, sh) (((c) >> (sh)) & 0xFFu)
        CHECK(CH(w,16) >= CH(w,8) && CH(w,8) > CH(w,0) && CH(w,0) >= 0xC0u,
              "tint: sepia white is cream -- warm, and still bright");
        CHECK(CH(k,16) > CH(k,8) && CH(k,8) > CH(k,0) && CH(k,16) < 0x40u,
              "tint: sepia black is a dark brown, not black");
        CHECK(CH(rd,16) > CH(rd,8) + 60u && CH(rd,16) > CH(rd,0) + 60u,
              "tint: sepia keeps the hue -- pure red is still clearly red");
        CHECK(CH(rd,16) < 0xE0u && CH(rd,8) > 0x30u,
              "tint: ...but washed out: less saturated than the input");
        CHECK(CH(bl,0) > CH(bl,16) && CH(bl,0) > CH(bl,8),
              "tint: sepia pure blue is still bluest (hue survives the warm cast)");
        #undef CH
    }
    printf("-- %d checks, %d failures --\n", total, fails);
    return fails ? 1 : 0;
}
