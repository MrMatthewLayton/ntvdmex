/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Sharp's remainder scale (src/vdd/present_sharp.h). (#418)
 *
 *   cc -std=c99 -I src/vdd -o present_sharp_test tests/unit/present_sharp_test.c src/vdd/present_sharp.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <stdlib.h>
#include "present_sharp.h"

#define CHECK(condition, name) \
    do \
    { \
        if (!(condition)) \
        { \
            printf("FAIL: %s\n", name); \
            g_Failures++; \
        } \
    } while (0)

static INT g_Failures;

static VOID TestSameSizeIsACopy(VOID)
{
    static const DWORD source[4] = { 0x00112233, 0x00445566, 0x00778899, 0x00AABBCC };
    DWORD out[4];
    INT index;

    PresentSharpScale(source, 2, 2, out, 2, 2);

    for (index = 0; index < 4; index++)
        CHECK(out[index] == source[index], "same size: a copy");
}

static VOID TestBoundaryIsWeightedByArea(VOID)
{
    static const DWORD source[2] = { 0x00000000, 0x00FF0000 };
    DWORD out[3];

    /* 2 to 3: the middle pixel covers half of each source pixel. */
    PresentSharpScale(source, 2, 1, out, 3, 1);
    CHECK(out[0] == 0x00000000, "2 to 3: first pixel unblended");
    CHECK(out[1] == 0x007F0000, "2 to 3: the middle takes half of each");
    CHECK(out[2] == 0x00FF0000, "2 to 3: last pixel unblended");
}

static VOID TestChannelsDoNotBleed(VOID)
{
    static const DWORD source[2] = { 0x00FF00FF, 0x0000FF00 };
    DWORD out[3];

    PresentSharpScale(source, 2, 1, out, 3, 1);
    CHECK(out[1] == 0x007F7F7F, "blending keeps the channels apart");
}

/* The rig's picture: 200 guest rows at a whole 5x. Only a guest row's edge can show a blend. */
static VOID TestMostRowsAreCopies(VOID)
{
    enum
    {
        WIDTH = 4, SOURCE_ROWS = 1000, DESTINATION_ROWS = 1080
    };
    DWORD *source = (DWORD *)malloc(WIDTH * SOURCE_ROWS * sizeof(DWORD));
    DWORD *out = (DWORD *)malloc(WIDTH * DESTINATION_ROWS * sizeof(DWORD));
    INT row;
    INT column;
    INT blended = 0;

    for (row = 0; row < SOURCE_ROWS; row++)
        for (column = 0; column < WIDTH; column++)
            source[row * WIDTH + column] = (DWORD)((row / 5) & 1 ? 0x00FFFFFF : 0);

    PresentSharpScale(source, WIDTH, SOURCE_ROWS, out, WIDTH, DESTINATION_ROWS);

    for (row = 0; row < DESTINATION_ROWS; row++)
        if (out[row * WIDTH] != 0 && out[row * WIDTH] != 0x00FFFFFF)
            blended++;

    CHECK(blended > 0 && blended <= 199, "1000 to 1080: blended rows only at the 199 guest row edges");
    free(source);
    free(out);
}

static VOID TestLastPixelStaysInside(VOID)
{
    enum
    {
        SOURCE_SIZE = 1920, DESTINATION_SIZE = 8192
    };
    DWORD *source = (DWORD *)calloc(SOURCE_SIZE * 2, sizeof(DWORD));
    DWORD *out = (DWORD *)calloc(DESTINATION_SIZE * 3, sizeof(DWORD));

    source[SOURCE_SIZE - 1] = 0x00123456;
    source[SOURCE_SIZE * 2 - 1] = 0x00123456;
    PresentSharpScale(source, SOURCE_SIZE, 2, out, DESTINATION_SIZE, 3);
    CHECK(out[DESTINATION_SIZE - 1] == 0x00123456, "the last column reads the last source pixel");
    CHECK(out[DESTINATION_SIZE * 3 - 1] == 0x00123456, "the last row reads the last source row");
    free(source);
    free(out);
}

static VOID TestLimits(VOID)
{
    CHECK(PresentSharpCanScale(1920, 1000, 1920, 1080), "an enlargement scales");
    CHECK(!PresentSharpCanScale(640, 400, 320, 200), "a reduction does not");
    CHECK(!PresentSharpCanScale(0, 200, 320, 200), "an empty source does not");
    CHECK(!PresentSharpCanScale(320, 200, 8193, 200), "past the table does not");
}

INT main(VOID)
{
    TestSameSizeIsACopy();
    TestBoundaryIsWeightedByArea();
    TestChannelsDoNotBleed();
    TestMostRowsAreCopies();
    TestLastPixelStaysInside();
    TestLimits();

    if (g_Failures)
    {
        printf("present_sharp_test: %d FAILED\n", g_Failures);
        return 1;
    }

    printf("present_sharp_test: all passed\n");
    return 0;
}
