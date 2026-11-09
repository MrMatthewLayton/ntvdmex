/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Video capture's fixed output size: every guest frame, whatever its mode, scaled into one
 * picture. (GH #357)
 *
 * WHY (Importance = 2):
 * An AVI stream has one frame size, so the recorder used to close the file and start a new
 * one each time the guest changed video mode -- a program that goes text -> 320x200 ->
 * 640x480 left three files. Now the size is chosen once, from the first frame, and every
 * frame after a mode change is scaled into it. One recording.
 *
 * THE RULES:
 * - The output size is the smallest of 640x480, 800x600, 1024x768, 1280x1024 and
 *   1600x1200 that holds the first frame at 1x; a first frame larger than all of them is
 *   kept at its own size (rounded up to even). It does not change for the recording.
 * - A frame is scaled by the largest WHOLE-PIXEL factor that fits both ways (320x200 ->
 *   640x400 at 2x), so pixels stay sharp and square. A frame larger than the output is
 *   scaled down, nearest-neighbour, keeping its proportions.
 * - It is centred; the bars around it are black. Pixel aspect is the guest's own (square):
 *   no 4:3 stretch, which would need a non-whole factor for 200-line modes.
 *
 * Pure C, no <windows.h>: tests/unit/capture_scale_test.c checks it off-VM.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_CAPTURE_SCALE_H
#define NTVDMEX_CAPTURE_SCALE_H

#include "../ntvdmex_types.h"

#define CAPTURE_SCALE_RGB_BYTES     3   /* 24-bit B, G, R */
#define CAPTURE_SCALE_ROW_ALIGN     3   /* DIB rows are padded to 4 bytes */

/* Where a source frame lands in the output picture, in output pixels from the top-left. */
typedef struct _CAPTURE_SCALE_PLACEMENT
{
    DWORD Width;            /* the scaled frame */
    DWORD Height;
    DWORD Left;             /* the bars: left and top */
    DWORD Top;
    DWORD Factor;           /* the whole-pixel factor; 0 = scaled down */
} CAPTURE_SCALE_PLACEMENT, *PCAPTURE_SCALE_PLACEMENT;

typedef const CAPTURE_SCALE_PLACEMENT *PCCAPTURE_SCALE_PLACEMENT;

/* The recording's output size for a first frame of `sourceWidth` x `sourceHeight`. */
VOID CaptureScaleChooseOutput(
    DWORD sourceWidth,
    DWORD sourceHeight,
    PDWORD outputWidth,
    PDWORD outputHeight);

/* Where a `sourceWidth` x `sourceHeight` frame goes in an `outputWidth` x `outputHeight` picture. */
VOID CaptureScalePlace(
    DWORD sourceWidth,
    DWORD sourceHeight,
    DWORD outputWidth,
    DWORD outputHeight,
    PCAPTURE_SCALE_PLACEMENT placement);

/* The bytes in one row of a 24-bit DIB `width` pixels wide. */
DWORD CaptureScaleStride(DWORD width);

/* `source` (24-bit, TOP-DOWN, `sourceStride` bytes a row) -> `target` (a 24-bit BOTTOM-UP
 * DIB, CaptureScaleStride(outputWidth) bytes a row), at `placement`, bars black.
 */
VOID CaptureScaleBlit(
    const BYTE *source,
    DWORD sourceWidth,
    DWORD sourceHeight,
    DWORD sourceStride,
    BYTE *target,
    DWORD outputWidth,
    DWORD outputHeight,
    PCCAPTURE_SCALE_PLACEMENT placement);

#endif
