/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Tools > Capture > Record Video: the guest's picture and our whole mix, to AVI. (GH #155)
 *
 * HOW (Importance = 2):
 * - THE SOUND IS THE CLOCK. The audio pump hands every buffer it fills to a tap
 *   (AudioWaveSetTap), which copies it into a ring; a writer thread drains the ring and,
 *   for each 1/fps of sound, writes that sound and the picture current at that moment. So
 *   the picture can never drift from the sound, whatever the guest's frame rate is.
 * - The picture is the guest's own pixels at its own resolution, converted to 24-bit RGB
 *   (the per-row palette included), then compressed by the codec the user chose in
 *   Windows' Video Compression dialog (remembered in the registry), or stored uncompressed.
 *   An unchanged picture is written as a repeat: no bytes.
 * - A new file ("segment") starts when the guest changes video mode (an AVI stream has one
 *   size) or when the file nears CAPTURE_AVI_SPLIT_BYTES.
 * - The writer is behind by at most a ring's worth; if the codec cannot keep up, frames
 *   become repeats and, past that, sound is dropped and counted.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_CAPTURE_VIDEO_H
#define NTVDMEX_CAPTURE_VIDEO_H

#include "../ntvdmex_types.h"

/* Start recording into the capture folder. FALSE = already recording, or no file. */
BOOL CaptureVideoStart(VOID);

/* Finish the file (index, sizes) and say what was recorded. Idempotent; every exit path calls it. */
VOID CaptureVideoStop(VOID);

BOOL CaptureVideoIsRecording(VOID);

/* Windows' Video Compression dialog; the choice is remembered. FALSE = cancelled. */
BOOL CaptureVideoChooseCodec(_In_opt_ HWND owner);

#endif /* NTVDMEX_CAPTURE_VIDEO_H */
