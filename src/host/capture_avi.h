/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The AVI writer for video capture. (GH #155)
 *
 * WHAT IT WRITES (Importance = 2):
 * An AVI 1.0 file, from Microsoft's published AVI RIFF reference: one video stream ('00dc')
 * and one 16-bit PCM audio stream ('01wb'), interleaved in 'movi', with an 'idx1' index.
 * - A video chunk of length 0 is a REPEATED frame: the previous picture stays up. That is
 *   how an unchanged screen costs nothing, and how the video keeps a fixed frame rate
 *   against a guest that draws when it likes.
 * - The caller splits recordings: CaptureAviBytes says how big the file is, and AVI 1.0
 *   readers are only dependable below CAPTURE_AVI_SPLIT_BYTES.
 *
 * Pure C, no <windows.h>: the file and the memory come in through CAPTURE_AVI_IO, so the
 * layout is checked off-VM by tests/unit/capture_avi_test.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_CAPTURE_AVI_H
#define NTVDMEX_CAPTURE_AVI_H

#include "../ntvdmex_types.h"

#define CAPTURE_AVI_SPLIT_BYTES         0x40000000u     /* 1 GB: where AVI 1.0 stays readable everywhere */
#define CAPTURE_AVI_FORMAT_EXTRA_MAX    1024            /* Codec bytes after the BITMAPINFOHEADER */
#define CAPTURE_AVI_INDEX_ENTRY_BYTES   16

/* Where the bytes go. Write appends; WriteAt patches an earlier position (the sizes, at
 * close); Resize grows the index block (NULL in = a new block; 0 bytes = free it, result
 * ignored). Write and WriteAt return FALSE on failure, Resize NULL.
 */
typedef struct _CAPTURE_AVI_IO
{
    BOOL (*Write)(PVOID context, const VOID *data, DWORD length);
    BOOL (*WriteAt)(PVOID context, DWORD offset, const VOID *data, DWORD length);
    PVOID (*Resize)(PVOID context, PVOID block, DWORD bytes);
    PVOID Context;
} CAPTURE_AVI_IO, *PCAPTURE_AVI_IO;

typedef struct _CAPTURE_AVI_FORMAT
{
    DWORD Width;
    DWORD Height;
    DWORD FrameRate;                /* frames per second = FrameRate / FrameScale */
    DWORD FrameScale;
    DWORD Handler;                  /* the codec's FOURCC, 0 = uncompressed */
    /* The video stream's format, as the codec states it: a BITMAPINFOHEADER (40 bytes)
     * plus whatever the codec appends. Written into 'strf' as it is.
     */
    const BYTE *VideoFormat;
    DWORD VideoFormatBytes;
    DWORD AudioHz;                  /* 16-bit PCM */
    DWORD AudioChannels;
} CAPTURE_AVI_FORMAT, *PCAPTURE_AVI_FORMAT;
typedef const CAPTURE_AVI_FORMAT *PCCAPTURE_AVI_FORMAT;

typedef struct _CAPTURE_AVI
{
    CAPTURE_AVI_IO Io;
    CAPTURE_AVI_FORMAT Format;
    DWORD Bytes;                    /* the file so far */
    DWORD VideoStrhOffset;          /* where each header patched at close begins */
    DWORD AudioStrhOffset;
    DWORD MoviListOffset;
    DWORD MoviOffset;               /* the 'movi' FOURCC: index offsets count from here */
    DWORD VideoFrames;              /* including repeats */
    DWORD AudioBytes;
    DWORD LargestChunk;
    BYTE *Index;
    DWORD IndexBytes;
    DWORD IndexCapacity;
    BOOL  IsBroken;                 /* a write failed: everything after is refused */
} CAPTURE_AVI, *PCAPTURE_AVI;

/* Write the headers (sizes patched at close). FALSE = could not write. */
BOOL CaptureAviOpen(
    _Out_ PCAPTURE_AVI avi,
    _In_ PCAPTURE_AVI_IO io,
    _In_ PCCAPTURE_AVI_FORMAT format);

/* One video frame. length 0 = repeat the previous frame. */
BOOL CaptureAviVideo(
    _Inout_ PCAPTURE_AVI avi,
    _In_reads_opt_(length) const VOID *data,
    _In_ DWORD length,
    _In_ BOOL isKeyFrame);

/* Interleaved 16-bit PCM, whole sample frames. */
BOOL CaptureAviAudio(
    _Inout_ PCAPTURE_AVI avi,
    _In_reads_(length) const VOID *data,
    _In_ DWORD length);

/* The file's size so far, with the index still to come: the caller splits on it. */
DWORD CaptureAviBytes(_In_ const CAPTURE_AVI *avi);

/* Write the index and patch every size. FALSE = the file is incomplete. Frees the index. */
BOOL CaptureAviClose(_Inout_ PCAPTURE_AVI avi);

#endif /* NTVDMEX_CAPTURE_AVI_H */
