/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The AVI writer for video capture (GH #155): see capture_avi.h.
 *
 * The layout is AVI 1.0 as Microsoft's AVI RIFF reference gives it:
 *
 *     RIFF 'AVI '
 *       LIST 'hdrl'
 *         'avih'  the main header (56 bytes)
 *         LIST 'strl'  'strh' (56) + 'strf' (BITMAPINFOHEADER + codec bytes)   video
 *         LIST 'strl'  'strh' (56) + 'strf' (WAVEFORMATEX, 18)                 audio
 *       LIST 'movi'   '00dc' / '01wb' chunks, each padded to an even length
 *       'idx1'        16 bytes per chunk: id, flags, offset from 'movi', length
 *
 * Every size the file cannot know until it ends is written as 0 and patched at close.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "capture_avi.h"

#define CAPTURE_AVI_CHUNK_HEADER            8               /* FOURCC + 32-bit size */
#define CAPTURE_AVI_LIST_HEADER             12              /* 'LIST' + size + list type */
#define CAPTURE_AVI_MAIN_HEADER_BYTES       56
#define CAPTURE_AVI_STREAM_HEADER_BYTES     56
#define CAPTURE_AVI_WAVE_FORMAT_BYTES       18              /* WAVEFORMATEX with cbSize = 0 */
#define CAPTURE_AVI_BITMAP_HEADER_BYTES     40              /* BITMAPINFOHEADER */
#define CAPTURE_AVI_WAVE_FORMAT_PCM         1
#define CAPTURE_AVI_BITS_PER_SAMPLE         16
#define CAPTURE_AVI_SAMPLE_BYTES            2
#define CAPTURE_AVI_FLAG_HAS_INDEX          0x00000010u     /* AVIF_HASINDEX */
#define CAPTURE_AVI_FLAG_INTERLEAVED        0x00000100u     /* AVIF_ISINTERLEAVED */
#define CAPTURE_AVI_INDEX_KEY_FRAME         0x00000010u     /* AVIIF_KEYFRAME */
#define CAPTURE_AVI_INDEX_GROW_BYTES        65536
#define CAPTURE_AVI_MICROSECONDS            1000000u
#define CAPTURE_AVI_STREAM_COUNT            2

/* The main header's fields, by offset from the start of its data. */
#define CAPTURE_AVI_AVIH_MICROSECONDS       0
#define CAPTURE_AVI_AVIH_MAX_RATE           4
#define CAPTURE_AVI_AVIH_FLAGS              12
#define CAPTURE_AVI_AVIH_FRAMES             16
#define CAPTURE_AVI_AVIH_STREAMS            24
#define CAPTURE_AVI_AVIH_BUFFER             28
#define CAPTURE_AVI_AVIH_WIDTH              32
#define CAPTURE_AVI_AVIH_HEIGHT             36

/* A stream header's fields, by offset from the start of its data. */
#define CAPTURE_AVI_STRH_TYPE               0
#define CAPTURE_AVI_STRH_HANDLER            4
#define CAPTURE_AVI_STRH_SCALE              20
#define CAPTURE_AVI_STRH_RATE               24
#define CAPTURE_AVI_STRH_LENGTH             32
#define CAPTURE_AVI_STRH_BUFFER             36
#define CAPTURE_AVI_STRH_QUALITY            40
#define CAPTURE_AVI_STRH_SAMPLE_SIZE        44
#define CAPTURE_AVI_STRH_FRAME_RIGHT        52
#define CAPTURE_AVI_STRH_FRAME_BOTTOM       54
#define CAPTURE_AVI_QUALITY_DEFAULT         0xFFFFFFFFu

#define CAPTURE_AVI_FOURCC(a, b, c, d) \
    ((DWORD)(BYTE)(a) | ((DWORD)(BYTE)(b) << 8) | ((DWORD)(BYTE)(c) << 16) | ((DWORD)(BYTE)(d) << 24))

static VOID CaptureAviPut32(BYTE *bytes, DWORD value)
{
    bytes[0] = (BYTE)value;
    bytes[1] = (BYTE)(value >> 8);
    bytes[2] = (BYTE)(value >> 16);
    bytes[3] = (BYTE)(value >> 24);
}

static VOID CaptureAviPut16(BYTE *bytes, WORD value)
{
    bytes[0] = (BYTE)value;
    bytes[1] = (BYTE)(value >> 8);
}

static VOID CaptureAviZero(BYTE *bytes, DWORD length)
{
    while (length--)
        *bytes++ = 0;
}

static BOOL CaptureAviWrite(PCAPTURE_AVI avi, const VOID *data, DWORD length)
{
    if (avi->IsBroken || !avi->Io.Write(avi->Io.Context, data, length))
    {
        avi->IsBroken = TRUE;
        return FALSE;
    }

    avi->Bytes += length;
    return TRUE;
}

static BOOL CaptureAviPatch(PCAPTURE_AVI avi, DWORD offset, DWORD value)
{
    BYTE bytes[4];

    CaptureAviPut32(bytes, value);

    if (avi->IsBroken || !avi->Io.WriteAt(avi->Io.Context, offset, bytes, sizeof(bytes)))
    {
        avi->IsBroken = TRUE;
        return FALSE;
    }

    return TRUE;
}

/* A chunk or list header: id, size, and (for a list) its type. */
static BOOL CaptureAviHeader(PCAPTURE_AVI avi, DWORD id, DWORD size, DWORD listType)
{
    BYTE bytes[CAPTURE_AVI_LIST_HEADER];

    CaptureAviPut32(bytes, id);
    CaptureAviPut32(bytes + 4, size);
    CaptureAviPut32(bytes + 8, listType);
    return CaptureAviWrite(avi, bytes, listType ? CAPTURE_AVI_LIST_HEADER : CAPTURE_AVI_CHUNK_HEADER);
}

static BOOL CaptureAviIndexAdd(
    PCAPTURE_AVI avi,
    DWORD id,
    DWORD flags,
    DWORD chunkOffset,
    DWORD length)
{
    BYTE *entry;

    if (avi->IndexBytes + CAPTURE_AVI_INDEX_ENTRY_BYTES > avi->IndexCapacity)
    {
        DWORD capacity = avi->IndexCapacity + CAPTURE_AVI_INDEX_GROW_BYTES;
        BYTE *grown = (BYTE *)avi->Io.Resize(avi->Io.Context, avi->Index, capacity);

        if (!grown)
        {
            avi->IsBroken = TRUE;
            return FALSE;
        }

        avi->Index = grown;
        avi->IndexCapacity = capacity;
    }

    entry = avi->Index + avi->IndexBytes;
    CaptureAviPut32(entry, id);
    CaptureAviPut32(entry + 4, flags);
    CaptureAviPut32(entry + 8, chunkOffset - avi->MoviOffset);
    CaptureAviPut32(entry + 12, length);
    avi->IndexBytes += CAPTURE_AVI_INDEX_ENTRY_BYTES;
    return TRUE;
}

/* One 'movi' chunk, padded to even, and its index entry. */
static BOOL CaptureAviChunk(PCAPTURE_AVI avi, DWORD id, const VOID *data, DWORD length, DWORD flags)
{
    static const BYTE padding = 0;
    DWORD chunkOffset = avi->Bytes;

    if (!CaptureAviHeader(avi, id, length, 0)
        || (length && !CaptureAviWrite(avi, data, length))
        || ((length & 1) && !CaptureAviWrite(avi, &padding, 1)))
        return FALSE;

    if (length > avi->LargestChunk)
        avi->LargestChunk = length;

    return CaptureAviIndexAdd(avi, id, flags, chunkOffset, length);
}

BOOL CaptureAviOpen(PCAPTURE_AVI avi, PCAPTURE_AVI_IO io, PCCAPTURE_AVI_FORMAT format)
{
    BYTE header[CAPTURE_AVI_MAIN_HEADER_BYTES];
    BYTE wave[CAPTURE_AVI_WAVE_FORMAT_BYTES];
    DWORD videoFormatPadded = (format->VideoFormatBytes + 1) & ~1u;
    DWORD videoStrlBytes = 4 + CAPTURE_AVI_CHUNK_HEADER + CAPTURE_AVI_STREAM_HEADER_BYTES + CAPTURE_AVI_CHUNK_HEADER + videoFormatPadded;
    DWORD audioStrlBytes = 4 + CAPTURE_AVI_CHUNK_HEADER + CAPTURE_AVI_STREAM_HEADER_BYTES + CAPTURE_AVI_CHUNK_HEADER + CAPTURE_AVI_WAVE_FORMAT_BYTES;
    DWORD hdrlBytes = 4
        + CAPTURE_AVI_CHUNK_HEADER + CAPTURE_AVI_MAIN_HEADER_BYTES
        + CAPTURE_AVI_CHUNK_HEADER + videoStrlBytes
        + CAPTURE_AVI_CHUNK_HEADER + audioStrlBytes;
    DWORD blockAlign = format->AudioChannels * CAPTURE_AVI_SAMPLE_BYTES;
    static const BYTE padding = 0;

    CaptureAviZero((BYTE *)avi, sizeof(*avi));
    avi->Io = *io;
    avi->Format = *format;

    if (!format->FrameRate || !format->FrameScale || !blockAlign || format->VideoFormatBytes < CAPTURE_AVI_BITMAP_HEADER_BYTES
        || format->VideoFormatBytes > CAPTURE_AVI_FORMAT_EXTRA_MAX + CAPTURE_AVI_BITMAP_HEADER_BYTES)
        return FALSE;

    CaptureAviHeader(avi, CAPTURE_AVI_FOURCC('R', 'I', 'F', 'F'), 0, CAPTURE_AVI_FOURCC('A', 'V', 'I', ' '));
    CaptureAviHeader(avi, CAPTURE_AVI_FOURCC('L', 'I', 'S', 'T'), hdrlBytes, CAPTURE_AVI_FOURCC('h', 'd', 'r', 'l'));

    /* 'avih': frame count, buffer size and data rate are patched at close. */
    CaptureAviZero(header, sizeof(header));
    CaptureAviPut32(header + CAPTURE_AVI_AVIH_MICROSECONDS, (DWORD)((ULONGLONG)CAPTURE_AVI_MICROSECONDS * format->FrameScale / format->FrameRate));
    CaptureAviPut32(header + CAPTURE_AVI_AVIH_FLAGS, CAPTURE_AVI_FLAG_HAS_INDEX | CAPTURE_AVI_FLAG_INTERLEAVED);
    CaptureAviPut32(header + CAPTURE_AVI_AVIH_STREAMS, CAPTURE_AVI_STREAM_COUNT);
    CaptureAviPut32(header + CAPTURE_AVI_AVIH_WIDTH, format->Width);
    CaptureAviPut32(header + CAPTURE_AVI_AVIH_HEIGHT, format->Height);
    CaptureAviHeader(avi, CAPTURE_AVI_FOURCC('a', 'v', 'i', 'h'), CAPTURE_AVI_MAIN_HEADER_BYTES, 0);
    CaptureAviWrite(avi, header, sizeof(header));

    /* The video stream. */
    CaptureAviHeader(avi, CAPTURE_AVI_FOURCC('L', 'I', 'S', 'T'), videoStrlBytes, CAPTURE_AVI_FOURCC('s', 't', 'r', 'l'));
    CaptureAviZero(header, sizeof(header));
    CaptureAviPut32(header + CAPTURE_AVI_STRH_TYPE, CAPTURE_AVI_FOURCC('v', 'i', 'd', 's'));
    CaptureAviPut32(header + CAPTURE_AVI_STRH_HANDLER, format->Handler);
    CaptureAviPut32(header + CAPTURE_AVI_STRH_SCALE, format->FrameScale);
    CaptureAviPut32(header + CAPTURE_AVI_STRH_RATE, format->FrameRate);
    CaptureAviPut32(header + CAPTURE_AVI_STRH_QUALITY, CAPTURE_AVI_QUALITY_DEFAULT);
    CaptureAviPut16(header + CAPTURE_AVI_STRH_FRAME_RIGHT, (WORD)format->Width);
    CaptureAviPut16(header + CAPTURE_AVI_STRH_FRAME_BOTTOM, (WORD)format->Height);
    CaptureAviHeader(avi, CAPTURE_AVI_FOURCC('s', 't', 'r', 'h'), CAPTURE_AVI_STREAM_HEADER_BYTES, 0);
    avi->VideoStrhOffset = avi->Bytes;
    CaptureAviWrite(avi, header, sizeof(header));
    CaptureAviHeader(avi, CAPTURE_AVI_FOURCC('s', 't', 'r', 'f'), format->VideoFormatBytes, 0);
    CaptureAviWrite(avi, format->VideoFormat, format->VideoFormatBytes);

    if (format->VideoFormatBytes & 1)
        CaptureAviWrite(avi, &padding, 1);

    /* The audio stream: one "sample" is one interleaved frame of 16-bit PCM. */
    CaptureAviHeader(avi, CAPTURE_AVI_FOURCC('L', 'I', 'S', 'T'), audioStrlBytes, CAPTURE_AVI_FOURCC('s', 't', 'r', 'l'));
    CaptureAviZero(header, sizeof(header));
    CaptureAviPut32(header + CAPTURE_AVI_STRH_TYPE, CAPTURE_AVI_FOURCC('a', 'u', 'd', 's'));
    CaptureAviPut32(header + CAPTURE_AVI_STRH_SCALE, blockAlign);
    CaptureAviPut32(header + CAPTURE_AVI_STRH_RATE, format->AudioHz * blockAlign);
    CaptureAviPut32(header + CAPTURE_AVI_STRH_QUALITY, CAPTURE_AVI_QUALITY_DEFAULT);
    CaptureAviPut32(header + CAPTURE_AVI_STRH_SAMPLE_SIZE, blockAlign);
    CaptureAviHeader(avi, CAPTURE_AVI_FOURCC('s', 't', 'r', 'h'), CAPTURE_AVI_STREAM_HEADER_BYTES, 0);
    avi->AudioStrhOffset = avi->Bytes;
    CaptureAviWrite(avi, header, sizeof(header));
    CaptureAviZero(wave, sizeof(wave));
    CaptureAviPut16(wave, CAPTURE_AVI_WAVE_FORMAT_PCM);
    CaptureAviPut16(wave + 2, (WORD)format->AudioChannels);
    CaptureAviPut32(wave + 4, format->AudioHz);
    CaptureAviPut32(wave + 8, format->AudioHz * blockAlign);
    CaptureAviPut16(wave + 12, (WORD)blockAlign);
    CaptureAviPut16(wave + 14, CAPTURE_AVI_BITS_PER_SAMPLE);
    CaptureAviHeader(avi, CAPTURE_AVI_FOURCC('s', 't', 'r', 'f'), CAPTURE_AVI_WAVE_FORMAT_BYTES, 0);
    CaptureAviWrite(avi, wave, sizeof(wave));

    /* 'movi': its size is patched at close; index offsets count from its type FOURCC. */
    avi->MoviListOffset = avi->Bytes;
    CaptureAviHeader(avi, CAPTURE_AVI_FOURCC('L', 'I', 'S', 'T'), 0, CAPTURE_AVI_FOURCC('m', 'o', 'v', 'i'));
    avi->MoviOffset = avi->MoviListOffset + CAPTURE_AVI_CHUNK_HEADER;
    return !avi->IsBroken;
}

BOOL CaptureAviVideo(PCAPTURE_AVI avi, const VOID *data, DWORD length, BOOL isKeyFrame)
{
    DWORD flags = (length && isKeyFrame) ? CAPTURE_AVI_INDEX_KEY_FRAME : 0;
    DWORD id = avi->Format.Handler
        ? CAPTURE_AVI_FOURCC('0', '0', 'd', 'c')
        : CAPTURE_AVI_FOURCC('0', '0', 'd', 'b');

    if (!CaptureAviChunk(avi, id, data, length, flags))
        return FALSE;

    avi->VideoFrames++;
    return TRUE;
}

BOOL CaptureAviAudio(PCAPTURE_AVI avi, const VOID *data, DWORD length)
{
    if (!CaptureAviChunk(avi, CAPTURE_AVI_FOURCC('0', '1', 'w', 'b'), data, length, CAPTURE_AVI_INDEX_KEY_FRAME))
        return FALSE;

    avi->AudioBytes += length;
    return TRUE;
}

DWORD CaptureAviBytes(const CAPTURE_AVI *avi)
{
    return avi->Bytes + CAPTURE_AVI_CHUNK_HEADER + avi->IndexBytes;
}

BOOL CaptureAviClose(PCAPTURE_AVI avi)
{
    DWORD blockAlign = avi->Format.AudioChannels * CAPTURE_AVI_SAMPLE_BYTES;
    DWORD moviEnd = avi->Bytes;
    DWORD seconds = (DWORD)((ULONGLONG)avi->VideoFrames * avi->Format.FrameScale / avi->Format.FrameRate);
    DWORD maxBytesPerSecond = seconds ? (moviEnd - avi->MoviOffset) / seconds : moviEnd - avi->MoviOffset;
    DWORD avihOffset = CAPTURE_AVI_LIST_HEADER * 2 + CAPTURE_AVI_CHUNK_HEADER;
    BOOL isComplete;

    CaptureAviHeader(avi, CAPTURE_AVI_FOURCC('i', 'd', 'x', '1'), avi->IndexBytes, 0);

    if (avi->IndexBytes)
        CaptureAviWrite(avi, avi->Index, avi->IndexBytes);

    CaptureAviPatch(avi, 4, avi->Bytes - CAPTURE_AVI_CHUNK_HEADER);
    CaptureAviPatch(avi, avi->MoviListOffset + 4, moviEnd - avi->MoviOffset);
    CaptureAviPatch(avi, avihOffset + CAPTURE_AVI_AVIH_MAX_RATE, maxBytesPerSecond);
    CaptureAviPatch(avi, avihOffset + CAPTURE_AVI_AVIH_FRAMES, avi->VideoFrames);
    CaptureAviPatch(avi, avihOffset + CAPTURE_AVI_AVIH_BUFFER, avi->LargestChunk + CAPTURE_AVI_CHUNK_HEADER);
    CaptureAviPatch(avi, avi->VideoStrhOffset + CAPTURE_AVI_STRH_LENGTH, avi->VideoFrames);
    CaptureAviPatch(avi, avi->VideoStrhOffset + CAPTURE_AVI_STRH_BUFFER, avi->LargestChunk);
    CaptureAviPatch(avi, avi->AudioStrhOffset + CAPTURE_AVI_STRH_LENGTH, blockAlign ? avi->AudioBytes / blockAlign : 0);
    isComplete = !avi->IsBroken;

    if (avi->Index)
        avi->Io.Resize(avi->Io.Context, avi->Index, 0);    /* 0 bytes = free it */

    avi->Index = NULL;
    avi->IndexBytes = avi->IndexCapacity = 0;
    return isComplete;
}
