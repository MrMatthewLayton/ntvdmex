/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The video-capture AVI writer (GH #155), checked by reading its file back the way a
 * player does: every RIFF size must enclose exactly its contents, the headers must say what
 * was written, and every index entry must point at the chunk it names.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/host/capture_avi.h"
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
                                    else{g_Failed++; printf("  FAIL  %s\n",(message));} }while(0)

static INT g_Total;
static INT g_Failed;
static BYTE g_File[1 << 20];
static DWORD g_FileBytes;
static INT g_FailWrites;            /* > 0: fail the Nth write from now */

static BOOL TestWrite(PVOID context, const VOID *data, DWORD length)
{
    (VOID)context;

    if (g_FailWrites && --g_FailWrites == 0)
        return FALSE;

    memcpy(g_File + g_FileBytes, data, length);
    g_FileBytes += length;
    return TRUE;
}

static BOOL TestWriteAt(PVOID context, DWORD offset, const VOID *data, DWORD length)
{
    (VOID)context;
    memcpy(g_File + offset, data, length);
    return TRUE;
}

static PVOID TestResize(PVOID context, PVOID block, DWORD bytes)
{
    (VOID)context;

    if (!bytes)
    {
        free(block);
        return NULL;
    }

    return realloc(block, bytes);
}

static DWORD Get32(DWORD offset)
{
    return g_File[offset] | (g_File[offset + 1] << 8) | (g_File[offset + 2] << 16) | ((DWORD)g_File[offset + 3] << 24);
}

static BOOL IsId(DWORD offset, const char *id)
{
    return memcmp(g_File + offset, id, 4) == 0;
}

/* The offset of the first chunk `id` inside [start, end), or 0. */
static DWORD Find(DWORD start, DWORD end, const char *id)
{
    while (start + 8 <= end)
    {
        if (IsId(start, id))
            return start;

        start += 8 + ((Get32(start + 4) + 1) & ~1u);
    }

    return 0;
}

INT main(VOID)
{
    CAPTURE_AVI avi;
    CAPTURE_AVI_IO io = { TestWrite, TestWriteAt, TestResize, NULL };
    CAPTURE_AVI_FORMAT format;
    BYTE videoFormat[41];               /* odd on purpose: 'strf' must be padded */
    BYTE frame[333];                    /* odd on purpose: chunks must be padded */
    short audio[2 * 630];               /* 1/70 s of stereo at 44100 Hz */
    DWORD hdrl;
    DWORD avih;
    DWORD movi;
    DWORD moviEnd;
    DWORD idx1;
    DWORD entry;
    DWORD strl;
    DWORD strh;
    INT index;

    printf("== #155: AVI writer ==\n");
    memset(videoFormat, 0, sizeof(videoFormat));
    videoFormat[0] = 40;
    memset(frame, 0xAB, sizeof(frame));
    memset(audio, 0x11, sizeof(audio));
    memset(&format, 0, sizeof(format));
    format.Width = 320;
    format.Height = 200;
    format.FrameRate = 70;
    format.FrameScale = 1;
    format.Handler = 0x64697678;        /* 'xvid' */
    format.VideoFormat = videoFormat;
    format.VideoFormatBytes = sizeof(videoFormat);
    format.AudioHz = 44100;
    format.AudioChannels = 2;

    CHECK(CaptureAviOpen(&avi, &io, &format), "open: headers written");
    CHECK(CaptureAviVideo(&avi, frame, sizeof(frame), TRUE), "video: a key frame");
    CHECK(CaptureAviAudio(&avi, audio, sizeof(audio)), "audio: 1/70 s");
    CHECK(CaptureAviVideo(&avi, NULL, 0, FALSE), "video: a repeated frame (length 0)");
    CHECK(CaptureAviAudio(&avi, audio, sizeof(audio)), "audio: another 1/70 s");
    CHECK(CaptureAviVideo(&avi, frame, 100, FALSE), "video: a delta frame");
    CHECK(CaptureAviBytes(&avi) == g_FileBytes + 8 + 5 * 16, "bytes: the file so far plus the index to come");
    CHECK(CaptureAviClose(&avi), "close: index written, sizes patched");

    /* The outer RIFF encloses the whole file. */
    CHECK(IsId(0, "RIFF") && IsId(8, "AVI "), "RIFF 'AVI '");
    CHECK(Get32(4) == g_FileBytes - 8, "RIFF size = file - 8");

    /* hdrl and avih. */
    hdrl = 12;
    CHECK(IsId(hdrl, "LIST") && IsId(hdrl + 8, "hdrl"), "LIST 'hdrl' first");
    avih = hdrl + 12;
    CHECK(IsId(avih, "avih") && Get32(avih + 4) == 56, "avih, 56 bytes");
    CHECK(Get32(avih + 8) == 14285, "avih: 14285 us per frame (70 fps)");
    CHECK(Get32(avih + 8 + 16) == 3, "avih: 3 frames, the repeat included");
    CHECK(Get32(avih + 8 + 24) == 2, "avih: 2 streams");
    CHECK(Get32(avih + 8 + 32) == 320 && Get32(avih + 8 + 36) == 200, "avih: 320 x 200");
    CHECK(Get32(avih + 8 + 12) & 0x10, "avih: AVIF_HASINDEX");

    /* The hdrl list size lands exactly on the movi list. */
    movi = hdrl + 8 + Get32(hdrl + 4);
    CHECK(IsId(movi, "LIST") && IsId(movi + 8, "movi"), "hdrl size lands exactly on LIST 'movi'");

    /* Video stream: 'strl' -> 'strh' 'vids' with the handler and length; 'strf' padded. */
    strl = avih + 8 + 56;
    CHECK(IsId(strl, "LIST") && IsId(strl + 8, "strl"), "video strl");
    strh = strl + 12;
    CHECK(IsId(strh + 8, "vids") && Get32(strh + 12) == 0x64697678, "video strh: vids, handler xvid");
    CHECK(Get32(strh + 8 + 20) == 1 && Get32(strh + 8 + 24) == 70, "video strh: 70/1 fps");
    CHECK(Get32(strh + 8 + 32) == 3, "video strh: length 3 frames");
    CHECK(Get32(strh + 8 + 36) >= sizeof(frame), "video strh: suggested buffer holds the largest frame");
    CHECK(IsId(strh + 64, "strf") && Get32(strh + 68) == 41, "video strf: 41 bytes as given");
    CHECK(strl + 8 + Get32(strl + 4) == strh + 64 + 8 + 42, "video strl size includes strf's pad byte");

    /* Audio stream. */
    strl = strl + 8 + Get32(strl + 4);
    strh = strl + 12;
    CHECK(IsId(strl + 8, "strl") && IsId(strh + 8, "auds"), "audio strl / strh 'auds'");
    CHECK(Get32(strh + 8 + 20) == 4 && Get32(strh + 8 + 24) == 44100 * 4 && Get32(strh + 8 + 44) == 4,
          "audio strh: scale 4, rate 176400 bytes/s, sample size 4");
    CHECK(Get32(strh + 8 + 32) == 2 * 630, "audio strh: length 1260 sample frames");
    CHECK(IsId(strh + 64, "strf") && Get32(strh + 64 + 8 + 4) == 44100, "audio strf: PCM 44100");
    CHECK(strl + 8 + Get32(strl + 4) == movi, "audio strl ends where movi begins");

    /* movi and idx1. */
    moviEnd = movi + 8 + Get32(movi + 4);
    idx1 = moviEnd;
    CHECK(IsId(idx1, "idx1"), "movi size lands exactly on idx1");
    CHECK(Get32(idx1 + 4) == 5 * 16, "idx1: 5 entries");
    CHECK(idx1 + 8 + Get32(idx1 + 4) == g_FileBytes, "idx1 ends the file");
    CHECK(Find(movi + 12, moviEnd, "00dc") == movi + 12, "first movi chunk is the video frame");

    for (index = 0; index < 5; ++index)
    {
        DWORD chunk;
        entry = idx1 + 8 + index * 16;
        chunk = movi + 8 + Get32(entry + 8);

        if (memcmp(g_File + chunk, g_File + entry, 4) != 0 || Get32(chunk + 4) != Get32(entry + 12))
            break;
    }

    CHECK(index == 5, "idx1: every entry points at a chunk with its id and length (offsets from 'movi')");
    CHECK(Get32(idx1 + 8 + 4) == 0x10, "idx1: the first frame is a key frame");
    CHECK(Get32(idx1 + 8 + 2 * 16 + 4) == 0 && Get32(idx1 + 8 + 2 * 16 + 12) == 0, "idx1: the repeat is length 0, not a key frame");

    /* Uncompressed video is '00db'. */
    g_FileBytes = 0;
    format.Handler = 0;
    CaptureAviOpen(&avi, &io, &format);
    CaptureAviVideo(&avi, frame, 64, TRUE);
    CaptureAviClose(&avi);
    movi = 12 + 8 + Get32(12 + 4);                      /* past LIST 'hdrl' */
    CHECK(Find(movi + 12, g_FileBytes, "00db") != 0, "uncompressed frames are '00db'");

    /* A failed write breaks the file and says so. */
    g_FileBytes = 0;
    g_FailWrites = 0;
    CaptureAviOpen(&avi, &io, &format);
    g_FailWrites = 2;
    CHECK(!CaptureAviVideo(&avi, frame, 64, TRUE), "a failed write is reported");
    CHECK(!CaptureAviAudio(&avi, audio, 64), "and every write after it is refused");
    CHECK(!CaptureAviClose(&avi), "and close says the file is incomplete");

    /* A format it cannot write is refused. */
    format.FrameRate = 0;
    CHECK(!CaptureAviOpen(&avi, &io, &format), "open: a zero frame rate is refused");

    printf("\n%d checks, %d failed\n", g_Total, g_Failed);
    return g_Failed ? 1 : 0;
}
