/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Video capture's fixed output size (GH #357): the size a recording is given, where each
 * guest mode lands in it, and the pixels -- scaled, centred, with black bars.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "../../src/host/capture_scale.h"
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
                                    else{g_Failed++; printf("  FAIL  %s\n",(message));} }while(0)

static INT g_Total;
static INT g_Failed;
static BYTE g_Source[16 * 16 * 3];
static BYTE g_Target[640 * 3 * 480];

/* Pixel (column, row) of the output picture, counted from the TOP, as B, G, R packed in a DWORD. */
static DWORD CaptureScaleTestAt(DWORD outputWidth, DWORD outputHeight, DWORD column, DWORD row)
{
    const BYTE *pixel = g_Target + (SIZE_T)(outputHeight - 1 - row) * CaptureScaleStride(outputWidth) + column * 3;

    return pixel[0] | ((DWORD)pixel[1] << 8) | ((DWORD)pixel[2] << 16);
}

/* A source frame whose every pixel names itself: B = x, G = y, R = 0x80. */
static VOID CaptureScaleTestFill(DWORD width, DWORD height)
{
    DWORD row;

    for (row = 0; row < height; ++row)
    {
        DWORD column;

        for (column = 0; column < width; ++column)
        {
            BYTE *pixel = g_Source + (row * width + column) * 3;
            pixel[0] = (BYTE)column;
            pixel[1] = (BYTE)row;
            pixel[2] = 0x80;
        }
    }
}

INT main(VOID)
{
    DWORD width;
    DWORD height;
    CAPTURE_SCALE_PLACEMENT placement;

    printf("== #357: one recording, one size ==\n");

    CaptureScaleChooseOutput(320, 200, &width, &height);
    CHECK(width == 640 && height == 480, "output: 320x200 starts a 640x480 recording");
    CaptureScaleChooseOutput(720, 400, &width, &height);
    CHECK(width == 800 && height == 600, "output: 720x400 text needs 800x600 to fit at 1x");
    CaptureScaleChooseOutput(640, 480, &width, &height);
    CHECK(width == 640 && height == 480, "output: 640x480 is its own size");
    CaptureScaleChooseOutput(1024, 768, &width, &height);
    CHECK(width == 1024 && height == 768, "output: 1024x768 is its own size");
    CaptureScaleChooseOutput(2049, 1537, &width, &height);
    CHECK(width == 2050 && height == 1538, "output: past the ladder, the frame's size rounded up to even");

    CaptureScalePlace(320, 200, 640, 480, &placement);
    CHECK(placement.Factor == 2 && placement.Width == 640 && placement.Height == 400, "place: 320x200 in 640x480 is a whole 2x");
    CHECK(placement.Left == 0 && placement.Top == 40, "place: ...letterboxed, 40 lines top and bottom");
    CaptureScalePlace(640, 200, 640, 480, &placement);
    CHECK(placement.Factor == 1 && placement.Width == 640 && placement.Height == 200 && placement.Top == 140, "place: 640x200 at 1x, centred");
    CaptureScalePlace(320, 240, 800, 600, &placement);
    CHECK(placement.Factor == 2 && placement.Left == 80 && placement.Top == 60, "place: 320x240 in 800x600: 2x, bars on all four sides");
    CaptureScalePlace(80, 25, 640, 480, &placement);
    CHECK(placement.Factor == 8 && placement.Width == 640 && placement.Height == 200, "place: the largest whole factor that fits both ways");
    CaptureScalePlace(800, 600, 640, 480, &placement);
    CHECK(placement.Factor == 0 && placement.Width == 640 && placement.Height == 480 && placement.Left == 0 && placement.Top == 0,
          "place: a bigger mode later is scaled DOWN to fit, proportions kept");
    CaptureScalePlace(720, 400, 640, 480, &placement);
    CHECK(placement.Factor == 0 && placement.Width == 640 && placement.Height == 355 && placement.Top == 62,
          "place: 720x400 into 640x480: the width fills, letterboxed");
    CaptureScalePlace(0, 0, 640, 480, &placement);
    CHECK(placement.Width == 0 && placement.Height == 0, "place: no frame, nothing placed");

    CHECK(CaptureScaleStride(640) == 1920 && CaptureScaleStride(321) == 964, "stride: 24-bit DIB rows padded to 4 bytes");

    /* The pixels: a 4x2 frame into 16x8 is 4x, edge to edge. */
    CaptureScaleTestFill(4, 2);
    memset(g_Target, 0xEE, sizeof g_Target);
    CaptureScalePlace(4, 2, 16, 8, &placement);
    CHECK(placement.Factor == 4 && placement.Width == 16 && placement.Height == 8, "blit: 4x2 into 16x8 is 4x, edge to edge");
    CaptureScaleBlit(g_Source, 4, 2, 4 * 3, g_Target, 16, 8, &placement);
    CHECK(CaptureScaleTestAt(16, 8, 0, 0) == 0x800000 && CaptureScaleTestAt(16, 8, 3, 3) == 0x800000, "blit: source (0,0) fills a 4x4 block");
    CHECK(CaptureScaleTestAt(16, 8, 4, 0) == 0x800001 && CaptureScaleTestAt(16, 8, 15, 7) == 0x800103, "blit: and every block is its own source pixel");

    CaptureScaleTestFill(4, 2);
    memset(g_Target, 0xEE, sizeof g_Target);
    CaptureScalePlace(4, 2, 16, 12, &placement);
    CaptureScaleBlit(g_Source, 4, 2, 4 * 3, g_Target, 16, 12, &placement);
    CHECK(placement.Factor == 4 && placement.Top == 2, "blit: 4x2 into 16x12 -- 4x, 2 lines of bar each side");
    CHECK(CaptureScaleTestAt(16, 12, 0, 0) == 0 && CaptureScaleTestAt(16, 12, 15, 1) == 0 && CaptureScaleTestAt(16, 12, 7, 11) == 0,
          "blit: the bars are black");
    CHECK(CaptureScaleTestAt(16, 12, 0, 2) == 0x800000 && CaptureScaleTestAt(16, 12, 15, 9) == 0x800103, "blit: the picture between them");
    CHECK(g_Target[CaptureScaleStride(16) * 12] == 0xEE, "blit: nothing written past the picture");

    /* Scaled down: 8x8 into 4x4, nearest -- every other pixel. */
    CaptureScaleTestFill(8, 8);
    memset(g_Target, 0xEE, sizeof g_Target);
    CaptureScalePlace(8, 8, 4, 4, &placement);
    CaptureScaleBlit(g_Source, 8, 8, 8 * 3, g_Target, 4, 4, &placement);
    CHECK(placement.Factor == 0 && CaptureScaleTestAt(4, 4, 1, 1) == 0x800202 && CaptureScaleTestAt(4, 4, 3, 3) == 0x800606,
          "blit: scaled down, nearest neighbour");

    printf("\n== %d checks, %d failed\n", g_Total, g_Failed);
    return g_Failed ? 1 : 0;
}
