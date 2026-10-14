/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Video capture (GH #155): see capture_video.h.
 *
 * The codec is reached through Video for Windows (msvfw32.dll, bound at run time): the
 * ICCompressorChoose dialog to pick one, ICSeqCompressFrame* to compress. Interfaces only.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "capture_video.h"
#include <windows.h>
#include <mmsystem.h>
#pragma push_macro("PERCENT")
#undef PERCENT
#include <vfw.h>
#pragma pop_macro("PERCENT")
#include "capture_avi.h"
#include "host_state.h"
#include "audio_wave.h"
#include "log.h"

#define CAPTURE_VIDEO_MODULE                "msvfw32.dll"
#define CAPTURE_VIDEO_RING                  (1u << 20)  /* samples: ~5.9 s of stereo at 44.1 kHz */
#define CAPTURE_VIDEO_POLL_MS               5
#define CAPTURE_VIDEO_STOP_TIMEOUT_MS       10000
#define CAPTURE_VIDEO_FPS_TALL              60          /* 480 lines and up: a 60 Hz mode */
#define CAPTURE_VIDEO_FPS_VGA               70          /* 200/350/400-line VGA modes run at 70 Hz */
#define CAPTURE_VIDEO_TALL_LINES            400
#define CAPTURE_VIDEO_RGB_BYTES             3
#define CAPTURE_VIDEO_RGB_BITS              24
#define CAPTURE_VIDEO_ROW_ALIGN             3           /* DIB rows are padded to 4 bytes */
#define CAPTURE_VIDEO_PALETTE_ENTRIES       256
#define CAPTURE_VIDEO_PALETTED_BPP          8
#define CAPTURE_VIDEO_XRGB_BPP              32
#define CAPTURE_VIDEO_XRGB_BYTES            4
#define CAPTURE_VIDEO_BITMAP_HEADER_BYTES   40
#define CAPTURE_VIDEO_FORMAT_MAX            (CAPTURE_VIDEO_BITMAP_HEADER_BYTES + CAPTURE_AVI_FORMAT_EXTRA_MAX)
#define CAPTURE_VIDEO_STATE_MAX             65536       /* A codec's settings blob */
#define CAPTURE_VIDEO_NAME_MAX              1000        /* capture_video_000 .. 999 */
#define CAPTURE_VIDEO_DIALOG_TITLE          "Video Compression"
#define CAPTURE_VIDEO_UNCOMPRESSED          mmioFOURCC('D', 'I', 'B', ' ')
#define CAPTURE_VIDEO_REG_CODEC             "VideoCodec"
#define CAPTURE_VIDEO_REG_STATE             "VideoCodecState"
#define CAPTURE_VIDEO_REG_QUALITY           "VideoCodecQuality"
#define CAPTURE_VIDEO_REG_KEY_RATE          "VideoCodecKeyRate"
#define CAPTURE_VIDEO_REG_DATA_RATE         "VideoCodecDataRate"
#define CAPTURE_VIDEO_LOG_LINE              512

typedef BOOL (VFWAPI *PFN_IC_COMPRESSOR_CHOOSE)(HWND owner, UINT flags, LPVOID input, LPVOID data, PCOMPVARS vars, LPSTR title);
typedef VOID (VFWAPI *PFN_IC_COMPRESSOR_FREE)(PCOMPVARS vars);
typedef BOOL (VFWAPI *PFN_IC_SEQ_START)(PCOMPVARS vars, LPBITMAPINFO input);
typedef LPVOID (VFWAPI *PFN_IC_SEQ_FRAME)(PCOMPVARS vars, UINT flags, LPVOID bits, BOOL *isKey, LONG *size);
typedef VOID (VFWAPI *PFN_IC_SEQ_END)(PCOMPVARS vars);
typedef HIC (VFWAPI *PFN_IC_OPEN)(DWORD type, DWORD handler, UINT mode);
typedef LRESULT (VFWAPI *PFN_IC_CLOSE)(HIC codec);
typedef LRESULT (VFWAPI *PFN_IC_SEND_MESSAGE)(HIC codec, UINT message, DWORD_PTR first, DWORD_PTR second);

/* msvfw32, bound once. */
static HMODULE g_CaptureVideoVfw;
static PFN_IC_COMPRESSOR_CHOOSE g_IcCompressorChoose;
static PFN_IC_COMPRESSOR_FREE g_IcCompressorFree;
static PFN_IC_SEQ_START g_IcSeqStart;
static PFN_IC_SEQ_FRAME g_IcSeqFrame;
static PFN_IC_SEQ_END g_IcSeqEnd;
static PFN_IC_OPEN g_IcOpen;
static PFN_IC_CLOSE g_IcClose;
static PFN_IC_SEND_MESSAGE g_IcSendMessage;

/* The ring between the audio thread (tap) and the writer. */
static volatile LONG g_CaptureVideoIsRecording;
static volatile LONG g_CaptureVideoIsStopping;
static volatile LONG g_CaptureVideoHead;
static volatile LONG g_CaptureVideoTail;
static volatile LONG g_CaptureVideoDropped;
static INT16 g_CaptureVideoRing[CAPTURE_VIDEO_RING];
static HANDLE g_CaptureVideoThread;

/* One segment: a file, its format and its codec. Writer thread only. */
typedef struct _CAPTURE_VIDEO_SEGMENT
{
    HANDLE File;
    CHAR Path[MAX_PATH];
    CAPTURE_AVI Avi;
    DWORD Width;
    DWORD Height;
    DWORD Fps;
    DWORD ImageBytes;
    BITMAPINFOHEADER Input;
    BYTE Format[CAPTURE_VIDEO_FORMAT_MAX];
    DWORD FormatBytes;
    COMPVARS Codec;
    BOOL HasCodec;
    BYTE *Rgb;                  /* this frame, 24-bit bottom-up */
    BYTE *Previous;             /* the last frame written, to spot repeats */
    BOOL HasPrevious;
    ULONGLONG SamplesWritten;   /* this segment's audio, in samples (L and R counted) */
    DWORD Frames;
    DWORD Repeats;
    DWORD Late;                 /* frames written as repeats because the writer was behind */
} CAPTURE_VIDEO_SEGMENT, *PCAPTURE_VIDEO_SEGMENT;

/* The picture as grabbed under the host lock, converted outside it. */
static BYTE *g_CaptureVideoPixels;
static DWORD g_CaptureVideoPixelsCapacity;
static UINT32 g_CaptureVideoPalette[CAPTURE_VIDEO_PALETTE_ENTRIES];
static UINT32 g_CaptureVideoPaletteSplit[CAPTURE_VIDEO_PALETTE_ENTRIES];
static WORD g_CaptureVideoSplitRow[CAPTURE_VIDEO_PALETTE_ENTRIES];
static UINT32 g_CaptureVideoSplitFrame[CAPTURE_VIDEO_PALETTE_ENTRIES];
static NTVDD_FRAME g_CaptureVideoFrame;

static BOOL CaptureVideoBind(VOID)
{
    if (g_IcSendMessage)
        return TRUE;

    if (!g_CaptureVideoVfw)
        g_CaptureVideoVfw = LoadLibraryA(CAPTURE_VIDEO_MODULE);

    if (!g_CaptureVideoVfw)
        return FALSE;

    g_IcCompressorChoose = (PFN_IC_COMPRESSOR_CHOOSE)GetProcAddress(g_CaptureVideoVfw, "ICCompressorChoose");
    g_IcCompressorFree = (PFN_IC_COMPRESSOR_FREE)GetProcAddress(g_CaptureVideoVfw, "ICCompressorFree");
    g_IcSeqStart = (PFN_IC_SEQ_START)GetProcAddress(g_CaptureVideoVfw, "ICSeqCompressFrameStart");
    g_IcSeqFrame = (PFN_IC_SEQ_FRAME)GetProcAddress(g_CaptureVideoVfw, "ICSeqCompressFrame");
    g_IcSeqEnd = (PFN_IC_SEQ_END)GetProcAddress(g_CaptureVideoVfw, "ICSeqCompressFrameEnd");
    g_IcOpen = (PFN_IC_OPEN)GetProcAddress(g_CaptureVideoVfw, "ICOpen");
    g_IcClose = (PFN_IC_CLOSE)GetProcAddress(g_CaptureVideoVfw, "ICClose");

    if (!g_IcCompressorChoose || !g_IcCompressorFree || !g_IcSeqStart || !g_IcSeqFrame || !g_IcSeqEnd || !g_IcOpen || !g_IcClose)
        return FALSE;

    g_IcSendMessage = (PFN_IC_SEND_MESSAGE)GetProcAddress(g_CaptureVideoVfw, "ICSendMessage");
    return g_IcSendMessage != NULL;
}

/* remembered codec: */

static DWORD CaptureVideoRegistryDword(HKEY key, PCSTR name, DWORD fallback)
{
    DWORD value = fallback;
    DWORD bytes = sizeof(value);
    DWORD type = 0;

    if (RegQueryValueExA(key, name, NULL, &type, (BYTE *)&value, &bytes) != ERROR_SUCCESS || type != REG_DWORD)
        return fallback;

    return value;
}

/* The saved choice into `vars` (hic open, state set). FALSE = none saved: uncompressed. */
static BOOL CaptureVideoLoadCodec(PCOMPVARS vars, DWORD fps)
{
    HKEY key;
    DWORD handler;
    BYTE *state;
    DWORD stateBytes = 0;
    DWORD type = 0;

    ZeroMemory(vars, sizeof(*vars));

    if (RegOpenKeyExA(HKEY_CURRENT_USER, NTVDMEX_REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return FALSE;

    handler = CaptureVideoRegistryDword(key, CAPTURE_VIDEO_REG_CODEC, 0);

    if (!handler || handler == CAPTURE_VIDEO_UNCOMPRESSED)
    {
        RegCloseKey(key);
        return FALSE;
    }

    vars->cbSize = sizeof(*vars);
    vars->dwFlags = ICMF_COMPVARS_VALID;
    vars->fccType = ICTYPE_VIDEO;
    vars->fccHandler = handler;
    vars->lQ = (LONG)CaptureVideoRegistryDword(key, CAPTURE_VIDEO_REG_QUALITY, (DWORD)ICQUALITY_DEFAULT);
    vars->lKey = (LONG)CaptureVideoRegistryDword(key, CAPTURE_VIDEO_REG_KEY_RATE, fps);
    vars->lDataRate = (LONG)CaptureVideoRegistryDword(key, CAPTURE_VIDEO_REG_DATA_RATE, 0);
    vars->hic = g_IcOpen(ICTYPE_VIDEO, handler, ICMODE_COMPRESS);

    if (!vars->hic)
    {
        RegCloseKey(key);
        return FALSE;
    }

    if (RegQueryValueExA(key, CAPTURE_VIDEO_REG_STATE, NULL, &type, NULL, &stateBytes) == ERROR_SUCCESS
        && type == REG_BINARY
        && stateBytes
        && stateBytes <= CAPTURE_VIDEO_STATE_MAX)
    {
        state = (BYTE *)HeapAlloc(GetProcessHeap(), 0, stateBytes);

        if (state && RegQueryValueExA(key, CAPTURE_VIDEO_REG_STATE, NULL, &type, state, &stateBytes) == ERROR_SUCCESS)
            g_IcSendMessage(vars->hic, ICM_SETSTATE, (DWORD_PTR)state, stateBytes);

        if (state)
            HeapFree(GetProcessHeap(), 0, state);
    }

    RegCloseKey(key);
    return TRUE;
}

BOOL CaptureVideoChooseCodec(HWND owner)
{
    COMPVARS vars;
    BITMAPINFOHEADER sample;
    HKEY key;
    DWORD handler;

    if (!CaptureVideoBind())
        return FALSE;

    /* A sample of what we will feed it, so the dialog lists only codecs that take it. */
    ZeroMemory(&sample, sizeof(sample));
    sample.biSize = sizeof(sample);
    sample.biWidth = 320;
    sample.biHeight = 200;
    sample.biPlanes = 1;
    sample.biBitCount = CAPTURE_VIDEO_RGB_BITS;
    sample.biCompression = BI_RGB;
    ZeroMemory(&vars, sizeof(vars));
    vars.cbSize = sizeof(vars);
    vars.dwFlags = ICMF_COMPVARS_VALID;
    vars.fccType = ICTYPE_VIDEO;
    vars.lQ = ICQUALITY_DEFAULT;

    if (!CaptureVideoLoadCodec(&vars, CAPTURE_VIDEO_FPS_VGA))
    {
        ZeroMemory(&vars, sizeof(vars));
        vars.cbSize = sizeof(vars);
        vars.dwFlags = ICMF_COMPVARS_VALID;
        vars.fccType = ICTYPE_VIDEO;
        vars.fccHandler = CAPTURE_VIDEO_UNCOMPRESSED;
        vars.lQ = ICQUALITY_DEFAULT;
    }

    if (!g_IcCompressorChoose(owner, ICMF_CHOOSE_KEYFRAME | ICMF_CHOOSE_DATARATE, &sample, NULL, &vars, (LPSTR)CAPTURE_VIDEO_DIALOG_TITLE))
    {
        if (vars.hic)
            g_IcClose(vars.hic);    /* the saved codec opened above */

        return FALSE;
    }

    handler = vars.hic ? vars.fccHandler : CAPTURE_VIDEO_UNCOMPRESSED;

    if (RegCreateKeyExA(HKEY_CURRENT_USER, NTVDMEX_REG_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) == ERROR_SUCCESS)
    {
        DWORD quality = (DWORD)vars.lQ;
        DWORD keyRate = (DWORD)vars.lKey;
        DWORD dataRate = (DWORD)vars.lDataRate;
        RegSetValueExA(key, CAPTURE_VIDEO_REG_CODEC, 0, REG_DWORD, (const BYTE *)&handler, sizeof(handler));
        RegSetValueExA(key, CAPTURE_VIDEO_REG_QUALITY, 0, REG_DWORD, (const BYTE *)&quality, sizeof(quality));
        RegSetValueExA(key, CAPTURE_VIDEO_REG_KEY_RATE, 0, REG_DWORD, (const BYTE *)&keyRate, sizeof(keyRate));
        RegSetValueExA(key, CAPTURE_VIDEO_REG_DATA_RATE, 0, REG_DWORD, (const BYTE *)&dataRate, sizeof(dataRate));
        RegDeleteValueA(key, CAPTURE_VIDEO_REG_STATE);

        if (vars.hic)
        {
            DWORD stateBytes = (DWORD)g_IcSendMessage(vars.hic, ICM_GETSTATE, 0, 0);

            if (stateBytes && stateBytes <= CAPTURE_VIDEO_STATE_MAX)
            {
                BYTE *state = (BYTE *)HeapAlloc(GetProcessHeap(), 0, stateBytes);

                if (state && g_IcSendMessage(vars.hic, ICM_GETSTATE, (DWORD_PTR)state, stateBytes) >= 0)
                    RegSetValueExA(key, CAPTURE_VIDEO_REG_STATE, 0, REG_BINARY, state, stateBytes);

                if (state)
                    HeapFree(GetProcessHeap(), 0, state);
            }
        }

        RegCloseKey(key);
    }

    g_IcCompressorFree(&vars);
    return TRUE;
}

/* the tap: */

/* Audio thread: copy, never wait. A full ring drops and counts. */
static VOID CaptureVideoTap(PVOID context, const INT16 *samples, UINT32 count)
{
    LONG head = g_CaptureVideoHead;
    LONG tail = g_CaptureVideoTail;
    UINT32 used = (UINT32)(head - tail) & (CAPTURE_VIDEO_RING - 1);
    UINT32 space = CAPTURE_VIDEO_RING - 1 - used;
    UINT32 index;

    (VOID)context;

    if (!g_CaptureVideoIsRecording)
        return;

    if (count > space)
    {
        InterlockedExchangeAdd(&g_CaptureVideoDropped, (LONG)(count - space));
        count = space & ~1u;            /* whole L/R pairs */
    }

    for (index = 0; index < count; ++index)
        g_CaptureVideoRing[((UINT32)head + index) & (CAPTURE_VIDEO_RING - 1)] = samples[index];

    InterlockedExchange(&g_CaptureVideoHead, (LONG)(((UINT32)head + count) & (CAPTURE_VIDEO_RING - 1)));
}

static UINT32 CaptureVideoAvailable(VOID)
{
    return (UINT32)(g_CaptureVideoHead - g_CaptureVideoTail) & (CAPTURE_VIDEO_RING - 1);
}

/* the file I/O: */

static BOOL CaptureVideoFileWrite(PVOID context, const VOID *data, DWORD length)
{
    DWORD written = 0;

    return WriteFile((HANDLE)context, data, length, &written, NULL) && written == length;
}

static BOOL CaptureVideoFileWriteAt(PVOID context, DWORD offset, const VOID *data, DWORD length)
{
    HANDLE file = (HANDLE)context;
    DWORD written = 0;
    LONG high = 0;
    DWORD end = SetFilePointer(file, 0, &high, FILE_CURRENT);
    BOOL isWritten;

    SetFilePointer(file, (LONG)offset, &high, FILE_BEGIN);
    isWritten = WriteFile(file, data, length, &written, NULL) && written == length;
    high = 0;
    SetFilePointer(file, (LONG)end, &high, FILE_BEGIN);
    return isWritten;
}

static PVOID CaptureVideoResize(PVOID context, PVOID block, DWORD bytes)
{
    (VOID)context;

    if (!bytes)
    {
        if (block)
            HeapFree(GetProcessHeap(), 0, block);

        return NULL;
    }

    return block
        ? HeapReAlloc(GetProcessHeap(), 0, block, bytes)
        : HeapAlloc(GetProcessHeap(), 0, bytes);
}

/* the picture: */

/* Copy the guest's frame and its palettes under the host lock. FALSE = nothing to show. */
static BOOL CaptureVideoGrab(VOID)
{
    PCNTVDD_FRAME frame = &g_Video.Frame;
    DWORD bytes;
    DWORD row;
    DWORD rowBytes;

    HOST_LOCK();

    if (!frame->Pixels
        || !frame->Width
        || !frame->Height
        || (frame->BitsPerPixel != CAPTURE_VIDEO_PALETTED_BPP && frame->BitsPerPixel != CAPTURE_VIDEO_XRGB_BPP))
    {
        HOST_UNLOCK();
        return FALSE;
    }

    rowBytes = frame->Width * (frame->BitsPerPixel / CAPTURE_VIDEO_PALETTED_BPP);
    bytes = rowBytes * frame->Height;

    if (bytes > g_CaptureVideoPixelsCapacity)
    {
        BYTE *grown = (BYTE *)CaptureVideoResize(NULL, g_CaptureVideoPixels, bytes);

        if (!grown)
        {
            HOST_UNLOCK();
            return FALSE;
        }

        g_CaptureVideoPixels = grown;
        g_CaptureVideoPixelsCapacity = bytes;
    }

    for (row = 0; row < frame->Height; ++row)
        CopyMemory(g_CaptureVideoPixels + row * rowBytes, frame->Pixels + (SIZE_T)row * frame->Stride, rowBytes);

    g_CaptureVideoFrame = *frame;
    g_CaptureVideoFrame.Pixels = g_CaptureVideoPixels;
    g_CaptureVideoFrame.Stride = rowBytes;
    g_CaptureVideoFrame.Palette = NULL;
    g_CaptureVideoFrame.PaletteBase = NULL;
    g_CaptureVideoFrame.PaletteSplit = NULL;
    g_CaptureVideoFrame.SplitRow = NULL;
    g_CaptureVideoFrame.SplitFrame = NULL;

    if (frame->BitsPerPixel == CAPTURE_VIDEO_PALETTED_BPP)
    {
        if (frame->Palette)
            CopyMemory(g_CaptureVideoPalette, frame->Palette, sizeof(g_CaptureVideoPalette));

        g_CaptureVideoFrame.Palette = g_CaptureVideoPalette;

        if (frame->PaletteBase && frame->PaletteSplit && frame->SplitRow && frame->SplitFrame)
        {
            CopyMemory(g_CaptureVideoPalette, frame->PaletteBase, sizeof(g_CaptureVideoPalette));
            CopyMemory(g_CaptureVideoPaletteSplit, frame->PaletteSplit, sizeof(g_CaptureVideoPaletteSplit));
            CopyMemory(g_CaptureVideoSplitRow, frame->SplitRow, sizeof(g_CaptureVideoSplitRow));
            CopyMemory(g_CaptureVideoSplitFrame, frame->SplitFrame, sizeof(g_CaptureVideoSplitFrame));
            g_CaptureVideoFrame.PaletteBase = g_CaptureVideoPalette;
            g_CaptureVideoFrame.PaletteSplit = g_CaptureVideoPaletteSplit;
            g_CaptureVideoFrame.SplitRow = g_CaptureVideoSplitRow;
            g_CaptureVideoFrame.SplitFrame = g_CaptureVideoSplitFrame;
        }
    }

    HOST_UNLOCK();
    return TRUE;
}

/* The grabbed frame -> 24-bit bottom-up RGB (a DIB), each row's palette resolved. */
static VOID CaptureVideoConvert(PCAPTURE_VIDEO_SEGMENT segment)
{
    PCNTVDD_FRAME frame = &g_CaptureVideoFrame;
    DWORD stride = (segment->Width * CAPTURE_VIDEO_RGB_BYTES + CAPTURE_VIDEO_ROW_ALIGN) & ~(DWORD)CAPTURE_VIDEO_ROW_ALIGN;
    DWORD row;

    for (row = 0; row < segment->Height; ++row)
    {
        const BYTE *source = frame->Pixels + (SIZE_T)row * frame->Stride;
        BYTE *target = segment->Rgb + (SIZE_T)(segment->Height - 1 - row) * stride;
        DWORD column;

        for (column = 0; column < segment->Width; ++column)
        {
            UINT32 colour;

            if (frame->BitsPerPixel == CAPTURE_VIDEO_PALETTED_BPP)
                colour = frame->PaletteBase
                    ? VddFramePaletteAt(frame, row, source[column])
                    : frame->Palette[source[column]];
            else
                colour = ((const UINT32 *)source)[column];

            target[column * CAPTURE_VIDEO_RGB_BYTES] = (BYTE)colour;                 /* B */
            target[column * CAPTURE_VIDEO_RGB_BYTES + 1] = (BYTE)(colour >> 8);      /* G */
            target[column * CAPTURE_VIDEO_RGB_BYTES + 2] = (BYTE)(colour >> 16);     /* R */
        }

        for (column *= CAPTURE_VIDEO_RGB_BYTES; column < stride; ++column)
            target[column] = 0;
    }
}

static BOOL CaptureVideoSame(const BYTE *left, const BYTE *right, DWORD bytes)
{
    const DWORD *leftWords = (const DWORD *)left;
    const DWORD *rightWords = (const DWORD *)right;
    DWORD words = bytes / sizeof(DWORD);       /* DIB rows: always whole DWORDs */
    DWORD index;

    for (index = 0; index < words; ++index)
        if (leftWords[index] != rightWords[index])
            return FALSE;

    return TRUE;
}

/* segments: */

static VOID CaptureVideoLog(PCSTR text, PCSTR path)
{
    CHAR line[CAPTURE_VIDEO_LOG_LINE];
    CHAR *cursor = LogPut(line, text);

    cursor = LogPut(cursor, path);
    cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, line, cursor);
}

/* A new file at the grabbed frame's size. FALSE = could not (logged). */
static BOOL CaptureVideoOpenSegment(PCAPTURE_VIDEO_SEGMENT segment)
{
    CAPTURE_AVI_IO io;
    CAPTURE_AVI_FORMAT format;
    DWORD stride;
    INT number;

    ZeroMemory(segment, sizeof(*segment));
    segment->Width = g_CaptureVideoFrame.Width;
    segment->Height = g_CaptureVideoFrame.Height;
    segment->Fps = segment->Height > CAPTURE_VIDEO_TALL_LINES ? CAPTURE_VIDEO_FPS_TALL : CAPTURE_VIDEO_FPS_VGA;
    stride = (segment->Width * CAPTURE_VIDEO_RGB_BYTES + CAPTURE_VIDEO_ROW_ALIGN) & ~(DWORD)CAPTURE_VIDEO_ROW_ALIGN;
    segment->ImageBytes = stride * segment->Height;
    segment->Rgb = (BYTE *)HeapAlloc(GetProcessHeap(), 0, segment->ImageBytes);
    segment->Previous = (BYTE *)HeapAlloc(GetProcessHeap(), 0, segment->ImageBytes);

    if (!segment->Rgb || !segment->Previous)
        return FALSE;

    segment->Input.biSize = sizeof(segment->Input);
    segment->Input.biWidth = (LONG)segment->Width;
    segment->Input.biHeight = (LONG)segment->Height;
    segment->Input.biPlanes = 1;
    segment->Input.biBitCount = CAPTURE_VIDEO_RGB_BITS;
    segment->Input.biCompression = BI_RGB;
    segment->Input.biSizeImage = segment->ImageBytes;

    /* The codec, if one was chosen and it takes this size; else uncompressed. */
    if (CaptureVideoLoadCodec(&segment->Codec, segment->Fps))
    {
        DWORD formatBytes = (DWORD)g_IcSendMessage(segment->Codec.hic, ICM_COMPRESS_GET_FORMAT, (DWORD_PTR)&segment->Input, 0);

        if (formatBytes >= CAPTURE_VIDEO_BITMAP_HEADER_BYTES
            && formatBytes <= CAPTURE_VIDEO_FORMAT_MAX
            && g_IcSeqStart(&segment->Codec, (LPBITMAPINFO)&segment->Input)
            && segment->Codec.lpbiOut)
        {
            CopyMemory(segment->Format, segment->Codec.lpbiOut, formatBytes);
            segment->FormatBytes = formatBytes;
            segment->HasCodec = TRUE;
        }
        else
        {
            g_IcClose(segment->Codec.hic);
            ZeroMemory(&segment->Codec, sizeof(segment->Codec));
            CaptureVideoLog("STAGE2: video capture: the chosen codec refused this frame size -> uncompressed ", "");
        }
    }

    if (!segment->HasCodec)
    {
        CopyMemory(segment->Format, &segment->Input, sizeof(segment->Input));
        segment->FormatBytes = sizeof(segment->Input);
    }

    /* The first free capture_video_NNN.avi. */
    for (number = 0; number < CAPTURE_VIDEO_NAME_MAX; ++number)
    {
        wsprintfA(segment->Path, "%scapture_video_%03d.avi", NTVDMEX_OUT, number);

        if (GetFileAttributesA(segment->Path) == INVALID_FILE_ATTRIBUTES)
            break;
    }

    segment->File = CreateFileA(segment->Path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);

    if (segment->File == INVALID_HANDLE_VALUE)
    {
        CaptureVideoLog("STAGE2: video capture: cannot create ", segment->Path);
        return FALSE;
    }

    io.Write = CaptureVideoFileWrite;
    io.WriteAt = CaptureVideoFileWriteAt;
    io.Resize = CaptureVideoResize;
    io.Context = segment->File;
    ZeroMemory(&format, sizeof(format));
    format.Width = segment->Width;
    format.Height = segment->Height;
    format.FrameRate = segment->Fps;
    format.FrameScale = 1;
    format.Handler = segment->HasCodec ? segment->Codec.fccHandler : 0;
    format.VideoFormat = segment->Format;
    format.VideoFormatBytes = segment->FormatBytes;
    format.AudioHz = g_Wave.SampleHz;
    format.AudioChannels = AUDIO_STEREO_CHANNELS;

    if (!CaptureAviOpen(&segment->Avi, &io, &format))
    {
        CaptureVideoLog("STAGE2: video capture: cannot write ", segment->Path);
        return FALSE;
    }

    {
        CHAR line[CAPTURE_VIDEO_LOG_LINE];
        CHAR *cursor = LogPut(line, "STAGE2: video capture started -> ");
        cursor = LogPut(cursor, segment->Path);
        cursor = LogPut(cursor, " ");           cursor = LogDecimal(cursor, segment->Width);
        cursor = LogPut(cursor, "x");           cursor = LogDecimal(cursor, segment->Height);
        cursor = LogPut(cursor, " @ ");         cursor = LogDecimal(cursor, segment->Fps);
        cursor = LogPut(cursor, " fps, codec 0x"); cursor = LogHex(cursor, format.Handler);
        cursor = LogPut(cursor, " (0 = uncompressed) (#155)\r\n");
        LogAppend(LOG_PATH, line, cursor);
    }

    return TRUE;
}

static VOID CaptureVideoCloseSegment(PCAPTURE_VIDEO_SEGMENT segment)
{
    BOOL isComplete;
    CHAR line[CAPTURE_VIDEO_LOG_LINE];
    CHAR *cursor;

    if (segment->HasCodec)
    {
        g_IcSeqEnd(&segment->Codec);
        g_IcClose(segment->Codec.hic);
    }

    isComplete = CaptureAviClose(&segment->Avi);
    CloseHandle(segment->File);
    cursor = LogPut(line, "STAGE2: video capture closed: ");
    cursor = LogPut(cursor, segment->Path);
    cursor = LogPut(cursor, " frames=");      cursor = LogDecimal(cursor, segment->Frames);
    cursor = LogPut(cursor, " repeats=");     cursor = LogDecimal(cursor, segment->Repeats);
    cursor = LogPut(cursor, " late=");        cursor = LogDecimal(cursor, segment->Late);
    cursor = LogPut(cursor, " bytes=");       cursor = LogDecimal(cursor, segment->Avi.Bytes);
    cursor = LogPut(cursor, " audio_dropped="); cursor = LogDecimal(cursor, (UINT)g_CaptureVideoDropped);
    cursor = LogPut(cursor, isComplete ? "\r\n" : " INCOMPLETE (a write failed)\r\n");
    LogAppend(LOG_PATH, line, cursor);
    HeapFree(GetProcessHeap(), 0, segment->Rgb);
    HeapFree(GetProcessHeap(), 0, segment->Previous);
    segment->File = INVALID_HANDLE_VALUE;
}

/* One video frame: the current picture, or a repeat. TRUE = the guest changed mode. */
static BOOL CaptureVideoWriteFrame(PCAPTURE_VIDEO_SEGMENT segment, BOOL isLate)
{
    BOOL isKey = TRUE;
    LONG size = (LONG)segment->ImageBytes;
    LPVOID data = segment->Rgb;

    segment->Frames++;

    if (isLate || !CaptureVideoGrab())
    {
        segment->Repeats++;
        segment->Late += isLate;
        CaptureAviVideo(&segment->Avi, NULL, 0, FALSE);
        return FALSE;
    }

    if (g_CaptureVideoFrame.Width != segment->Width || g_CaptureVideoFrame.Height != segment->Height)
    {
        segment->Repeats++;
        CaptureAviVideo(&segment->Avi, NULL, 0, FALSE);
        return TRUE;
    }

    CaptureVideoConvert(segment);

    if (segment->HasPrevious && CaptureVideoSame(segment->Rgb, segment->Previous, segment->ImageBytes))
    {
        segment->Repeats++;
        CaptureAviVideo(&segment->Avi, NULL, 0, FALSE);
        return FALSE;
    }

    CopyMemory(segment->Previous, segment->Rgb, segment->ImageBytes);
    segment->HasPrevious = TRUE;

    if (segment->HasCodec)
    {
        data = g_IcSeqFrame(&segment->Codec, 0, segment->Rgb, &isKey, &size);

        if (!data)
        {
            segment->Repeats++;
            CaptureAviVideo(&segment->Avi, NULL, 0, FALSE);
            return FALSE;
        }
    }

    CaptureAviVideo(&segment->Avi, data, (DWORD)size, isKey);
    return FALSE;
}

/* the writer: */

static DWORD WINAPI CaptureVideoWriter(LPVOID parameter)
{
    CAPTURE_VIDEO_SEGMENT segment;
    static INT16 chunk[CAPTURE_VIDEO_RING / 8];
    BOOL isOpen = FALSE;

    (VOID)parameter;

    for (;;)
    {
        BOOL isStopping = g_CaptureVideoIsStopping;
        BOOL hasModeChanged = FALSE;
        DWORD framesDue = 0;

        /* [CAUTION]: ONCE STOPPING, NEVER THE HOST LOCK. CaptureVideoStop runs on exit paths
         * that may hold it; a grab here would wait for it while Stop waits for us. The last
         * frames are written as repeats instead.
         */
        if (!isOpen && isStopping)
            break;

        if (!isOpen)
        {
            if (!CaptureVideoGrab())
            {
                if (isStopping)
                    break;

                Sleep(CAPTURE_VIDEO_POLL_MS);
                g_CaptureVideoTail = g_CaptureVideoHead;   /* no picture yet: no sound either */
                continue;
            }

            if (!CaptureVideoOpenSegment(&segment))
            {
                if (segment.File && segment.File != INVALID_HANDLE_VALUE)
                    CloseHandle(segment.File);

                if (segment.Codec.hic)
                    g_IcClose(segment.Codec.hic);

                if (segment.Rgb)
                    HeapFree(GetProcessHeap(), 0, segment.Rgb);

                if (segment.Previous)
                    HeapFree(GetProcessHeap(), 0, segment.Previous);

                break;
            }

            isOpen = TRUE;
        }

        /* Each whole frame period of sound in the ring: its audio chunk, then one frame. */
        for (;;)
        {
            ULONGLONG frameEnd = (ULONGLONG)(segment.Frames + framesDue + 1) * g_Wave.SampleHz * AUDIO_STEREO_CHANNELS / segment.Fps;
            UINT32 need = (UINT32)(frameEnd - segment.SamplesWritten) & ~1u;
            UINT32 tail = (UINT32)g_CaptureVideoTail;
            UINT32 index;

            if (need > CaptureVideoAvailable() || need > ARRAYSIZE(chunk))
                break;

            for (index = 0; index < need; ++index)
                chunk[index] = g_CaptureVideoRing[(tail + index) & (CAPTURE_VIDEO_RING - 1)];

            InterlockedExchange(&g_CaptureVideoTail, (LONG)((tail + need) & (CAPTURE_VIDEO_RING - 1)));
            CaptureAviAudio(&segment.Avi, chunk, need * sizeof(INT16));
            segment.SamplesWritten += need;
            framesDue++;
        }

        /* Behind by several frames: all but the last are repeats, so it catches up. */
        while (framesDue > 1)
        {
            CaptureVideoWriteFrame(&segment, TRUE);
            framesDue--;
        }

        if (framesDue)
            hasModeChanged = CaptureVideoWriteFrame(&segment, isStopping);

        if (hasModeChanged || CaptureAviBytes(&segment.Avi) > CAPTURE_AVI_SPLIT_BYTES || isStopping)
        {
            CaptureVideoCloseSegment(&segment);
            isOpen = FALSE;

            if (isStopping)
                break;

            continue;
        }

        if (!framesDue)
            Sleep(CAPTURE_VIDEO_POLL_MS);
    }

    if (isOpen)
        CaptureVideoCloseSegment(&segment);

    InterlockedExchange(&g_CaptureVideoIsRecording, 0);
    AudioWaveSetTap(NULL, NULL);
    return 0;
}

/* the API: */

BOOL CaptureVideoStart(VOID)
{
    if (g_CaptureVideoIsRecording)
        return FALSE;

    CaptureVideoStop();         /* a writer that gave up: reap it */

    if (!CaptureVideoBind())
    {
        CaptureVideoLog("STAGE2: video capture: msvfw32.dll is not available", "");
        return FALSE;
    }

    g_CaptureVideoHead = g_CaptureVideoTail = 0;
    g_CaptureVideoDropped = 0;
    g_CaptureVideoIsStopping = 0;
    InterlockedExchange(&g_CaptureVideoIsRecording, 1);
    AudioWaveSetTap(CaptureVideoTap, NULL);
    g_CaptureVideoThread = CreateThread(NULL, 0, CaptureVideoWriter, NULL, 0, NULL);

    if (!g_CaptureVideoThread)
    {
        AudioWaveSetTap(NULL, NULL);
        InterlockedExchange(&g_CaptureVideoIsRecording, 0);
        return FALSE;
    }

    SetThreadPriority(g_CaptureVideoThread, THREAD_PRIORITY_BELOW_NORMAL);
    return TRUE;
}

VOID CaptureVideoStop(VOID)
{
    if (!g_CaptureVideoThread)
        return;

    InterlockedExchange(&g_CaptureVideoIsStopping, 1);
    WaitForSingleObject(g_CaptureVideoThread, CAPTURE_VIDEO_STOP_TIMEOUT_MS);
    CloseHandle(g_CaptureVideoThread);
    g_CaptureVideoThread = NULL;
}

BOOL CaptureVideoIsRecording(VOID)
{
    return g_CaptureVideoIsRecording != 0;
}
