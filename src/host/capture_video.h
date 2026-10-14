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

#define CAPTURE_VIDEO_CODECS_MAX        64
#define CAPTURE_VIDEO_CODEC_NAME_MAX    64

/* One installed codec that takes our 24-bit frames, for Settings > Capture's list. */
typedef struct _CAPTURE_VIDEO_CODEC
{
    DWORD Handler;                      /* its FOURCC: the VideoCodec setting's value */
    BOOL  HasQuality;                   /* honours a quality setting */
    BOOL  HasKeyFrames;                 /* makes delta frames, so key-frame spacing applies */
    BOOL  CanConfigure;                 /* has its own Configure window */
    CHAR  Name[CAPTURE_VIDEO_CODEC_NAME_MAX];
} CAPTURE_VIDEO_CODEC, *PCAPTURE_VIDEO_CODEC;

/* Start recording into the capture folder. FALSE = already recording, or no file. */
BOOL CaptureVideoStart(VOID);

/* Finish the file (index, sizes) and say what was recorded. Idempotent; every exit path calls it. */
VOID CaptureVideoStop(VOID);

BOOL CaptureVideoIsRecording(VOID);

/* The installed codecs that can compress our frames. Returns how many were written. */
INT CaptureVideoCodecs(_Out_writes_(capacity) PCAPTURE_VIDEO_CODEC list, _In_ INT capacity);

/* The codec's own Configure window; what it sets is kept for that codec. FALSE = it has none. */
BOOL CaptureVideoConfigure(_In_opt_ HWND owner, _In_ DWORD handler);

/* Where captures go, with a trailing backslash: Settings > Capture's folder (created if
 * missing), or debug\out when that is blank or cannot be made -- every instance's, not
 * its own out\N. Screenshots use it too.
 */
PCSTR CaptureVideoFolder(VOID);

#endif /* NTVDMEX_CAPTURE_VIDEO_H */
