/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Video: Mode Y, the A000 trap, the instruction interpreters' host callbacks
 *   (v86interp.h, pm32interp.h) and the profiler.
 *
 * Its own translation unit (#335): declared in host_video.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "host_state.h"
#include "log.h"
#include "host_video.h"
#include "main.h"
#include "host_dpmi.h"
#include "host_bios.h"
#include "host_timing.h"
/* Used before their definitions below. */
static VOID ModeYRemapSelectBody(PVOID context, INT mask);
static VOID ModeYGr4CloseRun(VOID);
static INT ModeYInterpServes(VOID);
static VOID InterpreterMemoryBadNote(UINT32 linear, INT write);
static VOID HostProfileDump(VOID);

/* The interpreter templates' host callbacks, then the templates themselves. */
static inline __attribute__((always_inline)) BYTE V86HostRead8(UINT32 linear);
static inline __attribute__((always_inline)) VOID V86HostWrite8(UINT32 linear, BYTE value);
static inline __attribute__((always_inline)) const volatile BYTE *V86HostCodePointer(UINT32 linear);
static UINT32 V86HostIn(WORD port, INT width);
static VOID V86HostOut(WORD port, INT width, UINT32 byteValue);
static BYTE Pm32HostRead8(UINT32 linear);
static VOID Pm32HostWrite8(UINT32 linear, BYTE value);
static INT Pm32HostCanAccess(UINT32 linear, INT width, INT isWrite);
static UINT32 Pm32HostIn(WORD port, INT width);
static VOID Pm32HostOut(WORD port, INT width, UINT32 value);
#include "v86interp.h"
#include "pm32interp.h"

/* #183: present = sample the exec thread's host EIP ~1 kHz and log the hottest 16-byte
 * buckets at exit (STAGE2: HOSTPROF). Map them with `i686-w64-mingw32-nm -n`.
 */
#define HOSTPROF_FLAG   CFG_("hostprof.flag")
enum
{
    MYPM_STOP_RETURNED, MYPM_STOP_WINDOW_CLOSED, MYPM_STOP_DECLINED, MYPM_STOP_CAP, MYPM_STOP_NOT_FLAT, MYPM_STOP_IRQ_WAITING, MYPM_STOP_REASONS, MYPM_CHECK_MASK = 0x3F
};
INT            g_P12Offset       = 0;  /* P12OFF_FLAG: revert to the A0000 page trap */
/* North star 1, design C (s80) -- see ModeYNeedsInterp(). */
INT            g_ModeYInterpOffset = 0;  /* MYINTERP_OFF_FLAG */
INT            g_ModeYInterp     = 0;  /* inside HostInterp on mode Y's behalf */
DWORD g_ModeYSlices = 0;
DWORD g_ModeYInstructions = 0;
DWORD g_ModeYBails = 0;
DWORD g_ModeYBailMp = 0;
INT            g_ModeYRingOn    = 0;  /* MYRING_FLAG: record the ring (a copy per instruction) */
INT            g_ModeYPmOffset     = 0;  /* MYPM_OFF_FLAG */
INT            g_ModeYPmDetect  = 0;  /* MYPM_DETECT_FLAG */
static DWORD g_ModeYPmRuns = 0;
static DWORD g_ModeYPmInstructions = 0;
static DWORD g_ModeYPmBails = 0;
static DWORD g_ModeYPmBailMp = 0;
static DWORD          g_ModeYPmStop[MYPM_STOP_REASONS];       /* returned, window closed, declined, cap, not32, irq (#172) */
/* - THE SOURCE BESIDE THE PICTURE. (s68) A planar frame is st->fb rendered from
 * st->plane[] at crtc_start_live, and a watchpoint on the byte a visible sprite
 * must live in recorded NO sprite write on either page. One of the two is lying;
 * the only way to tell is to have both from the same instant. Called from the
 * periodic capture right after its BMP, under cfg\planedump.flag: writes the BMP's
 * path with `.pln` = a 16-byte header (start, offset in 2-byte units, gw, gh) +
 * the four 64K planes. Caller holds no lock; this takes it.
 */
VOID PlanesDumpBeside(PCSTR bitmapPath)
{
    CHAR path[200];
    INT length = 0;
    HANDLE file;
    DWORD header[4];
    DWORD bytesWritten;
    INT plane;

    while (bitmapPath[length] && length < 190)
    {
        path[length] = bitmapPath[length];
        ++length;
    }
    if (length < 4)
        return;
    path[length - 3] = 'p';
    path[length - 2] = 'l';
    path[length - 1] = 'n';
    path[length] = 0;
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;
    HOST_LOCK();
    header[0] = g_Video.CrtcStartLive;
    header[1] = g_Video.CrtcOffset;
    header[2] = g_Video.GraphicsWidth;
    header[3] = g_Video.GraphicsHeight;
    WriteFile(file, header, sizeof header, &bytesWritten, NULL);
    for (plane = 0; plane < VIDEO_PLANES; ++plane)                        /* the live backing: host sections when remapped (s74b) */
        WriteFile(file, g_Video.YMapPlane ? g_Video.YMapPlane(g_Video.YMapContext, plane) : g_Video.Planes[plane], VIDEO_PLANE_SIZE, &bytesWritten, NULL);
    HOST_UNLOCK();
    CloseHandle(file);
}

enum
{
    MODEY_VIEW_LINEAR = 4, MODEY_VIEW_SCRATCH = 5
};   /* g_ModeYSeconds/View: 0-3 the planes, then these */
static HANDLE g_ModeYSeconds[MODEY_NSEC];
PVOID g_ModeYView[MODEY_NSEC];           /* host-side views, always mapped */
static HANDLE g_BarSecond;
static BYTE   g_ModeYSeed[MODEY_WIN];            /* scratch contents as it was seeded */
INT    g_ModeYRemap      = 0;             /* the window is ours */
static INT    g_ModeYCurrent        = -1;            /* section index currently at A0000 */
static INT    g_ModeYPreviousMask  = 0;             /* mask live while the scratch was up */
DWORD g_ModeYSwaps = 0;
DWORD g_ModeYFanouts = 0;
DWORD g_ModeYFail = 0;
static DWORD g_ModeYTimelineSelector[YTL_SECS];
static DWORD g_ModeYTimelineSwap[YTL_SECS];
static DWORD g_ModeYTimelineFanout[YTL_SECS];
static DWORD g_ModeYTimelineFanoutBytes[YTL_SECS];
static DWORD g_ModeYTimelineFlip[YTL_SECS];
static UINT64 g_ModeYTimelineCycles[YTL_SECS];
DWORD  g_ModeYTimelineT0 = 0;
static UINT64 g_ModeYTimelineTscBase = 0;
static LONGLONG g_ModeYTimelineQpcBase = 0;
static DWORD  g_ModeYFanoutBytes = 0;   /* changed bytes fanned out, whole run */
/* [CAUTION]: fanB IS NOT A STORE COUNT. Moving 0x03 -> 0x0c keeps the scratch (both want it), so
 * the diff against the seed ACCUMULATES across windows and the same bytes are counted
 * again each time -- measured ~90M/s in Doom's low detail, which no renderer stores.
 * `fanN` counts bytes that changed SINCE THE PREVIOUS WINDOW CLOSED, against a shadow
 * of the scratch: a true lower bound on the guest's multi-plane stores (a store of the
 * value already there is still invisible -- that is the defect itself).
 */
static BYTE   g_ModeYShadow[MODEY_WIN];
static DWORD  g_ModeYFanoutNew = 0;
static DWORD  g_ModeYTimelineFanoutCount[YTL_SECS];
UINT64 ModeYTimelineRdtsc(VOID)
{
    UINT low;
    UINT high;

    __asm__ __volatile__("rdtsc" : "=a"(low), "=d"(high));
    return ((UINT64)high << DWORD_SHIFT) | low;
}

#define YBAR_PER_PAGE   (YBAR_OFF_HI - YBAR_OFF_LO)     /* 2560 */
static BYTE  g_ModeYFanoutBarSeen[(3u * YBAR_PER_PAGE + 7u) / 8u];
#define YSMP_A  (176u * MODEY_ROW_BYTES)    /* band A, rows 168-183 (256B spans rows 176-179) */
#define YSMP_B  (192u * MODEY_ROW_BYTES)    /* band B, rows 184-199 (256B spans rows 192-195) */
static BYTE  g_ModeYSample[2][4][YSMP_LEN];      /* [band][plane] last seen */
static INT   g_ModeYSampleHave[2][4];
static BYTE  g_ModeYSampleLast[2][YSMP_LEN];    /* last CHANGED window, any plane */
static INT   g_ModeYSampleLastPlane[2] = { -1, -1 };
DWORD g_ModeYSampleCrossSame[2];
DWORD g_ModeYSampleCrossDiff[2];
DWORD g_ModeYSampleWrites[2];
DWORD g_ModeYSampleCrossEqualBytes[2];
DWORD g_ModeYSampleCrossTotalBytes[2];
DWORD g_ModeYSampleP1Equal[2][4];
DWORD g_ModeYSampleP1Total[2][4];
/* [CAUTION]: AND `cross_eqb` IS A STATE MEASUREMENT, NOT A DELIVERY ONE (Importance = 1):
 * It compares the WHOLE 256-byte window whenever ANY byte of it changed, so 255 of
 * those bytes can be stale content left by an earlier event. That makes it very
 * nearly `bar_planes_equal` computed a second way -- which is why it landed on 56/79%
 * against that number's 66.8% -- and it is NOT independent evidence that the guest
 * DELIVERED collapsed bytes. Same family of error as the all-or-nothing predicate it
 * replaced: I read a number as answering a question it does not address.
 * - THE DELIVERY MEASUREMENT IS THE CHANGED BYTES ONLY. For each byte this pass
 *   actually wrote, was the value already present in the other plane? Stale bytes are
 *   excluded by construction, so a high rate means the guest HANDED us the same byte for
 *   two different planes -- which state cannot fake.
 */
DWORD g_ModeYSampleDeliveredEqual[2];
DWORD g_ModeYSampleDeliveredTotal[2];
static VOID ModeYSampleCheck(INT band, UINT offset, INT plane)
{
    const BYTE *view = (const BYTE *)g_ModeYView[plane] + offset;
    UINT index;
    BYTE prev[YSMP_LEN];
    INT had = g_ModeYSampleHave[band][plane];
    INT changed = !had;

    for (index = 0; index < YSMP_LEN && !changed; ++index)
        if (g_ModeYSample[band][plane][index] != view[index])
            changed = 1;
    if (!changed)
        return;                                 /* nobody wrote this window */
    for (index = 0; index < YSMP_LEN; ++index)
        prev[index] = g_ModeYSample[band][plane][index];
    /* Against plane 1 BEFORE this window is stored, so pl==1 compares with its own
     * previous content (a self-consistency baseline) rather than with itself.
     */
    if (g_ModeYSampleHave[band][1])
    {
        for (index = 0; index < YSMP_LEN; ++index)
        {
            g_ModeYSampleP1Total[band][plane]++;
            if (g_ModeYSample[band][1][index] == view[index])
                g_ModeYSampleP1Equal[band][plane]++;
        }
    }
    for (index = 0; index < YSMP_LEN; ++index)
        g_ModeYSample[band][plane][index] = view[index];
    g_ModeYSampleHave[band][plane] = 1;
    g_ModeYSampleWrites[band]++;
    if (g_ModeYSampleLastPlane[band] >= 0 && g_ModeYSampleLastPlane[band] != plane)
    {
        INT same = 1;
        for (index = 0; index < YSMP_LEN; ++index)
        {
            g_ModeYSampleCrossTotalBytes[band]++;
            if (g_ModeYSampleLast[band][index] == view[index])
                g_ModeYSampleCrossEqualBytes[band]++;
            else
                same = 0;
            /* DELIVERY: only bytes this pass actually changed. `prev` was captured
             * before the store above overwrote it.
             */
            if (had && prev[index] != view[index])
            {
                g_ModeYSampleDeliveredTotal[band]++;
                if (g_ModeYSampleLast[band][index] == view[index])
                    g_ModeYSampleDeliveredEqual[band]++;
            }
        }
        if (same)
            g_ModeYSampleCrossSame[band]++;
        else
            g_ModeYSampleCrossDiff[band]++;
    }
    for (index = 0; index < YSMP_LEN; ++index)
        g_ModeYSampleLast[band][index] = view[index];
    g_ModeYSampleLastPlane[band] = plane;
}

/* Record a fan-out write at plane offset k under `mask`. Returns nothing; cheap
 * enough to sit in the fan-out's inner loop (3 compares for a non-bar byte).
 */
static VOID ModeYFanoutBarNote(UINT linearOffset, INT mask)
{
    UINT page;

    for (page = 0; page < 3; ++page)
    {
        UINT offset = linearOffset - page * 0x4000u;
        UINT index;
        UINT band;
        if (linearOffset < page * 0x4000u || offset < YBAR_OFF_LO || offset >= YBAR_OFF_HI)
            continue;
        band = (offset < YBAR_OFF_MID) ? 0u : 1u;
        g_ModeYFanoutBarWrites[band]++;
        if ((mask & VIDEO_ALL_PLANES) == VIDEO_ALL_PLANES)
            g_ModeYFanoutBar4Way[band]++;
        index = page * YBAR_PER_PAGE + (offset - YBAR_OFF_LO);
        if (!(g_ModeYFanoutBarSeen[index >> BITMAP_BYTE_SHIFT] & (1u << (index & BITMAP_BIT_MASK))))
        {
            g_ModeYFanoutBarSeen[index >> BITMAP_BYTE_SHIFT] |= (BYTE)(1u << (index & BITMAP_BIT_MASK));
            g_ModeYFanoutBarDistinct[band]++;
        }
        return;
    }
}

static INT    g_ModeYWriteMode      = 0;             /* GC write mode, tracked for latch copies */
static INT    g_ModeYLatch      = 0;             /* write mode 1 seen in the current window */
/* VirtualQuery a probe address into the log -- the shape of the A0000 region after each
 * step is the only thing that distinguishes "the range is reserved by the VDM" from
 * "we asked for it wrongly", and those need completely different answers.
 */
static VOID ModeYRemapProbe(PCSTR when, DWORD address)
{
    MEMORY_BASIC_INFORMATION memoryInfo;
    CHAR buffer[200];
    CHAR *cursor = buffer;

    cursor = LogPut(cursor, "MODEY-REMAP probe "); cursor = LogPut(cursor, when);
    cursor = LogPut(cursor, " @0x"); cursor = LogHex(cursor, address);
    if (VirtualQuery((LPCVOID)(ULONG_PTR)address, &memoryInfo, sizeof memoryInfo) == sizeof memoryInfo)
    {
        cursor = LogPut(cursor, " allocbase=0x"); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)memoryInfo.AllocationBase);
        cursor = LogPut(cursor, " base=0x");      cursor = LogHex(cursor, (DWORD)(ULONG_PTR)memoryInfo.BaseAddress);
        cursor = LogPut(cursor, " size=0x");      cursor = LogHex(cursor, (DWORD)memoryInfo.RegionSize);
        cursor = LogPut(cursor, " state=0x");     cursor = LogHex(cursor, memoryInfo.State);
        cursor = LogPut(cursor, " type=0x");      cursor = LogHex(cursor, memoryInfo.Type);
        cursor = LogPut(cursor, memoryInfo.State == MEM_FREE     ? " (FREE)"
                  : memoryInfo.State == MEM_RESERVE  ? " (RESERVED)" : " (COMMIT)");
    }
    else
        cursor = LogPut(cursor, " <VirtualQuery failed>");
    cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, buffer, cursor); SerialOut(buffer, cursor);
}

/* - THE REPORT IS BUFFERED, BECAUSE THIS RUNS BEFORE THE PREAMBLE. The remap has to
 * happen before the UI thread exists -- the renderer dereferences the A0000 window
 * every few milliseconds and there is an instant during the swap when it is unmapped
 * -- and every LogWrite() before the preamble TRUNCATES the file. Two separate
 * reports have already been lost to that.
 */
static CHAR  g_RemapReport[2048];
static PSTR g_RemapReportCursor = g_RemapReport;
static VOID ModeYRemapEmit(PCSTR begin, PCSTR end)
{
    while (begin < end && g_RemapReportCursor < g_RemapReport + sizeof g_RemapReport - 1)
        *g_RemapReportCursor++ = *begin++;
    *g_RemapReportCursor = 0;
}

VOID ModeYRemapFlushReport(VOID)
{
    if (g_RemapReportCursor == g_RemapReport)
        return;
    LogAppend(LOG_PATH, g_RemapReport, g_RemapReportCursor);
    SerialOut(g_RemapReport, g_RemapReportCursor);
    g_RemapReportCursor = g_RemapReport;
}

static VOID ModeYRemapLog(PCSTR what, DWORD error)
{
    CHAR buffer[160];
    CHAR *cursor = buffer;

    cursor = LogPut(cursor, "MODEY-REMAP "); cursor = LogPut(cursor, what);
    if (error)
    {
        cursor = LogPut(cursor, " err=0x");
        cursor = LogHex(cursor, error);
    }
    cursor = LogPut(cursor, "\r\n");
    ModeYRemapEmit(buffer, cursor);
}

/* Take ownership of the A0000 window. Returns 0 and leaves everything as it was if any
 * step fails -- the heuristic path still works, so a failure here must not be fatal.
 */
INT ModeYRemapInitialize(VOID)
{
    static BYTE savedWindowA[MODEY_WIN];
    static BYTE savedWindowB[MODEY_WIN];
    UINT index;

    for (index = 0; index < MODEY_WIN; ++index)
        savedWindowA[index] = ((volatile BYTE *)(ULONG_PTR)VIDEO_APERTURE_BASE)[index];
    for (index = 0; index < MODEY_WIN; ++index)
        savedWindowB[index] = ((volatile BYTE *)(ULONG_PTR)VIDEO_MONO_BASE)[index];

    ModeYRemapProbe("before unmap", VIDEO_APERTURE_BASE);
    if (!UnmapViewOfFile((LPVOID)(ULONG_PTR)VIDEO_APERTURE_BASE))
    {
        ModeYRemapLog("unmap of the original A0000 view FAILED", GetLastError());
        return 0;
    }
    ModeYRemapProbe("after unmap", VIDEO_APERTURE_BASE);
    /* Text memory first: it must be back before anything reads B8000. */
    g_BarSecond = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE,
                                0, MODEY_WIN, NULL);
    if (!g_BarSecond || !MapViewOfFileEx(g_BarSecond, FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE,
                                    0, 0, MODEY_WIN, (LPVOID)(ULONG_PTR)VIDEO_MONO_BASE))
    {
        ModeYRemapLog("could not re-establish B0000 -- text memory is GONE", GetLastError());
        return 0;
    }
    for (index = 0; index < MODEY_WIN; ++index)
        ((volatile BYTE *)(ULONG_PTR)VIDEO_MONO_BASE)[index] = savedWindowB[index];

    /* - CLAIM A0000 BEFORE ASKING FOR ANY FLOATING VIEW. Unmapping the original view
     * makes A0000-AFFFF the LOWEST FREE 64K-aligned hole in the address space, and
     * MapViewOfFile with no address hint takes the lowest hole -- so the first
     * host-side view landed exactly on the address we were about to need, and the
     * fixed map then failed with ERROR_INVALID_ADDRESS against our own mapping.
     * Measured, and it reads as though the range were forbidden:
     *   after unmap      A0000 size=0x20000 (FREE)
     *   after failed map A0000 size=0x10000 (COMMIT, MEM_MAPPED)  <- ours
     * Take the fixed address first; everything else can live anywhere.
     */
    for (index = 0; index < MODEY_NSEC; ++index)
    {
        g_ModeYSeconds[index] = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE,
                                       0, MODEY_WIN, NULL);
        if (!g_ModeYSeconds[index])
        {
            ModeYRemapLog("plane section allocation FAILED", GetLastError());
            return 0;
        }
    }
    if (!MapViewOfFileEx(g_ModeYSeconds[MODEY_VIEW_LINEAR], FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE,
                         0, 0, MODEY_WIN, (LPVOID)(ULONG_PTR)VIDEO_APERTURE_BASE))
    {
        ModeYRemapLog("could not map a replacement at A0000", GetLastError());
        ModeYRemapProbe("after failed map", VIDEO_APERTURE_BASE);
        return 0;
    }
    for (index = 0; index < MODEY_NSEC; ++index)
    {
        g_ModeYView[index] = MapViewOfFile(g_ModeYSeconds[index], FILE_MAP_ALL_ACCESS, 0, 0, MODEY_WIN);
        if (!g_ModeYView[index])
        {
            ModeYRemapLog("host-side plane view FAILED", GetLastError());
            return 0;
        }
        /* - SEED EVERY PLANE WITH A DISTINCT MARKER, NOT WITH THE APERTURE. Seeding all
         * four from the same flat buffer makes them IDENTICAL, and any region the guest
         * never writes per-plane then renders as four equal pixels -- indistinguishable
         * from the plane-collapse bug this whole change exists to remove. A per-plane
         * marker makes "never written" visible and attributable instead: the oracle
         * sees index 0/1/2/3 rather than a plausible picture.
         */
        { UINT byteIndex;
        BYTE *destination = (BYTE *)g_ModeYView[index];
          for (byteIndex = 0; byteIndex < MODEY_WIN; ++byteIndex)
              destination[byteIndex] = (BYTE)(index < VIDEO_PLANES ? index : savedWindowA[byteIndex]); }
    }
    g_ModeYCurrent = MODEY_VIEW_LINEAR;
    g_ModeYRemap = 1;
    ModeYRemapLog("A0000 is ours: 4 planes + linear + scratch", 0);
    return 1;
}

/* A LATCH COPY MOVES ALL FOUR PLANES AT ONCE, AND ONE MAPPING CANNOT DO THAT:
 * VGA write mode 1: reading an address loads every plane into the chip's latches, and
 * the next store writes them all back at the destination. It is the standard mode-Y
 * way to move a region inside video memory, and Doom uses it to carry the STATUS BAR
 * between its three pages rather than redraw it -- measured, 120 times a run, always
 * as `mask := 0x0F` (write mode 0), `write mode := 1`, copy, `write mode := 0`.
 *
 * With A0000 pointing at one buffer the guest can only move the bytes it can see, so
 * every plane ended up with the same data: the four-equal-pixel collapse the WAD
 * oracle found in the bar (STBAR 60% wrong, identical in every frame) while the 3D
 * view and the title screen -- plain write-mode-0 stores -- were pixel-exact.
 *
 * - THE COPY IS RECOVERABLE, AND IT IS SOLVED RATHER THAN GUESSED. What the window
 *   leaves behind is `scratch[dst] = seed[src]` for every byte it moved. The offsets it
 *   changed are known exactly (scratch vs seed), so a single displacement `src = dst -
 *   delta` explains the whole window if it explains EVERY changed byte -- and that is
 *   checkable. Try the plausible page strides, VERIFY each against every changed byte,
 *   and only apply one that survives. A window nothing explains falls back to the plain
 *   fan-out and is counted, so "we could not solve it" can never masquerade as success.
 */
/* Does one displacement explain EVERY byte the burst changed? */
static INT ModeYLatchVerify(INT32 delta)
{
    const BYTE *scratch = (const BYTE *)g_ModeYView[MODEY_VIEW_SCRATCH];
    UINT index;
    UINT moved = 0;

    for (index = 0; index < MODEY_WIN; ++index)
    {
        INT32 source;
        if (scratch[index] == g_ModeYSeed[index])
            continue;
        ++moved;
        source = (INT32)index - delta;
        if (source < 0 || source >= (INT32)MODEY_WIN || g_ModeYSeed[source] != scratch[index])
            return 0;
    }
    return moved != 0;
}

/* - DERIVE THE DISPLACEMENT FROM THE DATA, DO NOT GUESS AT PAGE STRIDES. A candidate
 * list of plausible strides solved 71 of 120 bursts and left 49 unexplained, which is
 * the shape of a guess: it works where the guess was right. The burst itself says what
 * the source was -- the longest run of changed bytes IS a verbatim copy of some run in
 * the pre-burst image, so find that run in the seed and the displacement falls out.
 * Every hit is then VERIFIED against every changed byte before it is used, so a wrong
 * match cannot be applied; a burst nothing explains is counted, never guessed at.
 */
static INT32 ModeYLatchDelta(VOID)
{
    const BYTE *scratch = (const BYTE *)g_ModeYView[MODEY_VIEW_SCRATCH];
    /* stays unsigned: UINT here moves the compiled code */
    unsigned index;
    unsigned runLow = 0;
    unsigned runLength = 0;
    unsigned bestLow = 0;
    unsigned bestLength = 0;
    unsigned cap;

    for (index = 0; index < MODEY_WIN; ++index)
    {
        if (scratch[index] != g_ModeYSeed[index])
        {
            if (!runLength)
                runLow = index;
            ++runLength;
            if (runLength > bestLength)
            {
                bestLength = runLength;
                bestLow = runLow;
            }
        }
        else
            runLength = 0;
    }
    if (bestLength < 8)
        return 0;                                  /* too little to identify a source */
    cap = bestLength > 24 ? 24 : bestLength;
    for (index = 0; index + cap <= MODEY_WIN; ++index)
    {
        UINT run;
        if (g_ModeYSeed[index] != scratch[bestLow])
            continue;                                                      /* cheap first-byte reject */
        for (run = 1; run < cap; ++run)
            if (g_ModeYSeed[index + run] != scratch[bestLow + run])
                break;
        if (run < cap)
            continue;
        { INT32 delta = (INT32)bestLow - (INT32)index;
          if (delta && ModeYLatchVerify(delta))
              return delta; }
    }
    return 0;
}

/* WHERE DOES A PROTECTED-MODE GUEST WRITE THE MAP MASK? (s80, north star 1):
 * Design C for Doom needs a 32-bit interpreter, and how much of one depends on what
 * code runs between Doom's map-mask writes. The OUT that writes SR2 traps, and the main
 * loop has just recorded its CS:EIP (g_dpmi_last_*), so each distinct site is known --
 * with the masks it writes and the bytes around it, which anchor a disassembly of the
 * LE image. Bounded: 16 sites.
 */
#define YPM_SITES   16
static struct
{
    DWORD Linear;
    DWORD Count;
    DWORD Multi;
    WORD Masks;
    BYTE Bytes[48];
} g_ModeYPmSite[YPM_SITES];
static UINT g_ModeYPmCount = 0;
static UINT g_ModeYPmLost = 0;
static VOID ModeYPmSiteNote(INT mask)
{
    DWORD linear;
    UINT index;
    UINT byteIndex;

    if (!g_DpmiPm || !g_DpmiLastCs)
        return;
    linear = DpmiSelectorBase((WORD)g_DpmiLastCs) + g_DpmiLastEip;
    for (index = 0; index < g_ModeYPmCount; ++index)
        if (g_ModeYPmSite[index].Linear == linear)
            break;
    if (index == g_ModeYPmCount)
    {
        const BYTE *code = (const BYTE *)(ULONG_PTR)(linear - 32);
        if (g_ModeYPmCount >= YPM_SITES)
        {
            ++g_ModeYPmLost;
            return;
        }
        g_ModeYPmSite[index].Linear = linear;
        if (HostReadable(code, 48))
            for (byteIndex = 0; byteIndex < 48; ++byteIndex)
                g_ModeYPmSite[index].Bytes[byteIndex] = code[byteIndex];
        ++g_ModeYPmCount;
    }
    g_ModeYPmSite[index].Count++;
    if (mask >= 0)
    {
        g_ModeYPmSite[index].Masks |= (WORD)(1u << (mask & VIDEO_ALL_PLANES));
        if ((mask & VIDEO_ALL_PLANES) & ((mask & VIDEO_ALL_PLANES) - 1))
            g_ModeYPmSite[index].Multi++;
    }
}

VOID ModeYRemapSelect(PVOID context, INT mask)
{
    UINT64 cycleStart;
    DWORD seconds;
    DWORD swapsBase;
    DWORD fan0;
    DWORD fanoutBytesBase;
    DWORD function0;

    if (!g_ModeYRemap)
        return;
    /* The timeline measures MODE Y. A planar 16-colour guest (mode 12h) also moves this
     * window on every map-mask write, and the RDTSC/GetTickCount toll cost Lemmings'
     * interpreted run ~4% of its throughput -- so it goes straight to the work.
     */
    if (g_Video.ModeKind != VIDEO_KIND_LINEAR8)
    {
        ModeYRemapSelectBody(context, mask);
        return;
    }
    if (!g_ModeYTimelineT0)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        g_ModeYTimelineT0 = GetTickCount();
        g_ModeYTimelineTscBase = ModeYTimelineRdtsc();
        g_ModeYTimelineQpcBase = now.QuadPart;
    }
    seconds = (GetTickCount() - g_ModeYTimelineT0) / MILLISECONDS_PER_SECOND_U;
    ModeYPmSiteNote(mask);
    swapsBase = g_ModeYSwaps;
    fan0 = g_ModeYFanouts;
    fanoutBytesBase = g_ModeYFanoutBytes;
    function0 = g_ModeYFanoutNew;
    cycleStart = ModeYTimelineRdtsc();
    ModeYRemapSelectBody(context, mask);
    if (seconds < YTL_SECS)
    {
        g_ModeYTimelineCycles[seconds]  += ModeYTimelineRdtsc() - cycleStart;
        g_ModeYTimelineSelector[seconds]  += 1;
        g_ModeYTimelineSwap[seconds] += g_ModeYSwaps - swapsBase;
        g_ModeYTimelineFanout[seconds]  += g_ModeYFanouts - fan0;
        g_ModeYTimelineFanoutBytes[seconds] += g_ModeYFanoutBytes - fanoutBytesBase;
        g_ModeYTimelineFanoutCount[seconds] += g_ModeYFanoutNew - function0;
        /* Cumulative, reported as deltas. CR0C (start address HIGH), not the paired
         * counter: Doom flips pages by writing 0Ch alone -- its pages are 0x4000 apart,
         * so the low byte never changes -- and crtc_start_writes counts only on 0Dh.
         */
        g_ModeYTimelineFlip[seconds]  = g_Video.CrtcWrites[0x0C];
    }
}

/* [INFO]: NORTH STAR 1's measurement, printed -- see g_ytl_*. Decimal, one value per second of
 * mode-Y activity, so a row reads straight across as a rate. Called from BOTH exits:
 * a DOS/4GW guest leaves through the watchdog's forced exit and never reaches the STAGE2
 * summary, and Doom is the guest this exists for. Prints once.
 */
#define MY_SITE_MAX     16
static struct
{
    DWORD Cs;
    DWORD Ip;
    DWORD Count;
    BYTE Bytes[8];
} g_ModeYSite[MY_SITE_MAX];
static UINT g_ModeYSiteCount = 0;
VOID ModeYBailNote(DWORD cs, DWORD ip, const volatile BYTE *bytes)
{
    UINT index;
    UINT byteIndex;

    for (index = 0; index < g_ModeYSiteCount; ++index)
        if (g_ModeYSite[index].Cs == cs && g_ModeYSite[index].Ip == ip)
        {
            g_ModeYSite[index].Count++;
            return;
        }
    if (g_ModeYSiteCount >= MY_SITE_MAX)
        return;
    g_ModeYSite[index].Cs = cs;
    g_ModeYSite[index].Ip = ip;
    g_ModeYSite[index].Count = 1;
    for (byteIndex = 0; byteIndex < 8; ++byteIndex)
        g_ModeYSite[index].Bytes[byteIndex] = bytes[byteIndex];
    ++g_ModeYSiteCount;
}

VOID ModeYTimelineReport(VOID)
{
    static INT done = 0;
    CHAR buffer[1400];
    CHAR *cursor = buffer;
    LARGE_INTEGER now;
    UINT64 cpu;
    DWORD microsecondsRun;
    DWORD second;
    DWORD last = 0;
    INT row;
    static PCSTR names[9] = { "sel", "swap", "fan", "fanB", "us", "flip", "fanN", "ins", "ius" };
    if (done || !g_ModeYTimelineT0)
    {
        if (!done)
            HostProfileDump();
        return;
    }
    done = 1;
    HostProfileDump();
    if (g_ModeYSlices && g_ModeYRingOn)
        ModeYRingDump("at exit -- the last instructions interpreted for mode Y");
    QueryPerformanceCounter(&now);
    microsecondsRun = QpcMicroseconds(now.QuadPart - g_ModeYTimelineQpcBase);
    cpu = microsecondsRun ? (ModeYTimelineRdtsc() - g_ModeYTimelineTscBase) / microsecondsRun : 0;    /* cycles per us */
    for (second = 0; second < YTL_SECS; ++second)
        if (g_ModeYTimelineSelector[second])
            last = second;
    cursor = LogPut(cursor, "STAGE2: MODEYTL cyc_per_us="); cursor = LogDecimal(cursor, (DWORD)cpu);
    cursor = LogPut(cursor, " secs="); cursor = LogDecimal(cursor, last + 1);
    cursor = LogPut(cursor, " fanB_total="); cursor = LogDecimal(cursor, g_ModeYFanoutBytes);
    cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, buffer, cursor); SerialOut(buffer, cursor); cursor = buffer;
    cursor = LogPut(cursor, "STAGE2: MODEY-INTERP "); cursor = LogPut(cursor, g_ModeYInterpOffset ? "OFF (knob)" : "on");
    cursor = LogPut(cursor, " slices="); cursor = LogDecimal(cursor, g_ModeYSlices);
    cursor = LogPut(cursor, " instrs="); cursor = LogDecimal(cursor, g_ModeYInstructions);
    cursor = LogPut(cursor, " bails="); cursor = LogDecimal(cursor, g_ModeYBails);
    cursor = LogPut(cursor, " bails_under_multiplane="); cursor = LogDecimal(cursor, g_ModeYBailMp);
    cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, buffer, cursor); SerialOut(buffer, cursor); cursor = buffer;
    cursor = LogPut(cursor, "STAGE2: MODEY-PM "); cursor = LogPut(cursor, (g_ModeYPmOffset || g_ModeYInterpOffset) ? "OFF (knob)" : "on");
    cursor = LogPut(cursor, g_ModeYPmDetect ? " DETECT" : "");
    cursor = LogPut(cursor, " runs="); cursor = LogDecimal(cursor, g_ModeYPmRuns);
    cursor = LogPut(cursor, " instrs="); cursor = LogDecimal(cursor, g_ModeYPmInstructions);
    cursor = LogPut(cursor, " stop[returned,closed,declined,cap,not32,irq]=");
    {
        INT index2;
        for (index2 = 0; index2 < MYPM_STOP_REASONS; ++index2)
        {
            cursor = LogPut(cursor, index2 ? "," : "");
            cursor = LogDecimal(cursor, g_ModeYPmStop[index2]);
        }
    }
    cursor = LogPut(cursor, " bails_under_multiplane="); cursor = LogDecimal(cursor, g_ModeYPmBailMp);
    cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, buffer, cursor); SerialOut(buffer, cursor); cursor = buffer;
    { UINT index, byteIndex;
      for (index = 0; index < g_ModeYPmCount; ++index)
      {
          cursor = LogPut(cursor, "  MODEY-PM site lin=0x"); cursor = LogHex(cursor, g_ModeYPmSite[index].Linear);
          cursor = LogPut(cursor, " n="); cursor = LogDecimal(cursor, g_ModeYPmSite[index].Count);
          cursor = LogPut(cursor, " multi="); cursor = LogDecimal(cursor, g_ModeYPmSite[index].Multi);
          cursor = LogPut(cursor, " masks=0x"); cursor = LogHex(cursor, g_ModeYPmSite[index].Masks);
          cursor = LogPut(cursor, " bytes[-32..+16]:");
          for (byteIndex = 0; byteIndex < 48; ++byteIndex)
          {
              cursor = LogPut(cursor, " ");
              cursor = LogHexByte(cursor, g_ModeYPmSite[index].Bytes[byteIndex]);
          }
          cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, buffer, cursor); SerialOut(buffer, cursor); cursor = buffer;
      }
      if (g_ModeYPmLost) { cursor = LogPut(cursor, "  MODEY-PM sites lost="); cursor = LogDecimal(cursor, g_ModeYPmLost);
                        cursor = LogPut(cursor, "\r\n");
                        LogAppend(LOG_PATH, buffer, cursor);
                        SerialOut(buffer, cursor);
                        cursor = buffer; }
      for (index = 0; index < g_ModeYSiteCount; ++index)
      {
          cursor = LogPut(cursor, "  MODEY-INTERP bail cs:ip="); cursor = LogHex(cursor, g_ModeYSite[index].Cs);
          cursor = LogPut(cursor, ":"); cursor = LogHex(cursor, g_ModeYSite[index].Ip);
          cursor = LogPut(cursor, " n="); cursor = LogDecimal(cursor, g_ModeYSite[index].Count); cursor = LogPut(cursor, " bytes:");
          for (byteIndex = 0; byteIndex < 8; ++byteIndex)
          {
              cursor = LogPut(cursor, " ");
              cursor = LogHexByte(cursor, g_ModeYSite[index].Bytes[byteIndex]);
          }
          cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, buffer, cursor); SerialOut(buffer, cursor); cursor = buffer;
      } }
    for (row = 0; row < 9; ++row)
    {
        DWORD previousFlip = 0;
        cursor = LogPut(cursor, "STAGE2: MODEYTL "); cursor = LogPut(cursor, names[row]); cursor = LogPut(cursor, "=");
        for (second = 0; second <= last; ++second)
        {
            DWORD value = 0;
            switch (row)
            {
            case 0:
                value = g_ModeYTimelineSelector[second];
            break;

            case 1:
                value = g_ModeYTimelineSwap[second];
            break;

            case 2:
                value = g_ModeYTimelineFanout[second];
            break;

            case 3:
                value = g_ModeYTimelineFanoutBytes[second];
            break;

            case 4:
                value = cpu ? (DWORD)(g_ModeYTimelineCycles[second] / cpu) : 0;
            break;

            case 5:
                value = g_ModeYTimelineFlip[second] ? g_ModeYTimelineFlip[second] - previousFlip : 0;
                    if (g_ModeYTimelineFlip[second])
                        previousFlip = g_ModeYTimelineFlip[second];
                    break;

            case 6:
                value = g_ModeYTimelineFanoutCount[second];
            break;

            case 7:
                value = g_ModeYTimelineIns[second];
            break;

            case 8:
                value = cpu ? (DWORD)(g_ModeYTimelineInterpreterCycles[second] / cpu) : 0;
            break;
            }
            cursor = LogPut(cursor, second ? "," : ""); cursor = LogDecimal(cursor, value);
        }
        cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, buffer, cursor); SerialOut(buffer, cursor); cursor = buffer;
    }
}

static VOID ModeYRemapSelectBody(PVOID context, INT mask)
{
    INT want;
    INT plane;
    INT selectedCount = 0;
    INT selector[VIDEO_PLANES];

    (VOID)context;
    if (!g_ModeYRemap)
        return;
    ++g_ModeYSelectorCalls;
    ModeYGr4CloseRun();   /* the window is about to move: end the current read run */

    /* FAN OUT ONLY WHAT THE GUEST ACTUALLY WROTE:
     * A multi-plane mask means one store lands in several planes at once, so a
     * scratch window's WRITES do belong to every selected plane -- but the bytes
     * nobody touched do not. Copying the whole scratch pushed the plane it was
     * seeded from over the top of all the others, making four identical planes and
     * therefore duplicated columns.
     * That is what the user could still see after the remap landed: Doom clears with
     * mask 0x0F at level start (44 windows a run, measured), so every plane went
     * identical across the page. The 3D view redraws completely every frame and
     * healed itself; the STATUS BAR is only updated where it changes, so the
     * corruption stayed -- "the game bar at the bottom still has quite a lot of
     * pixelated graphics" -- and a screen wipe looked pixelated until the full redraw
     * behind it cleaned up. Diffing against the seed is exact and costs one pass over
     * 64K, 44 times a run.
     */
    if (g_ModeYCurrent == MODEY_VIEW_SCRATCH && g_ModeYPreviousMask)
    {
        UINT index;
        const BYTE *scratch = (const BYTE *)g_ModeYView[MODEY_VIEW_SCRATCH];
        /* fanN is mode Y's measurement; a planar guest pays nothing for it -- a
         * separate pass rather than a per-byte test, because this loop is hot for a
         * mode-12h guest (Lemmings: ~38k windows a run) and -O2 does not unswitch.
         */
        if (g_Video.ModeKind == VIDEO_KIND_LINEAR8)
            for (index = 0; index < MODEY_WIN; ++index)
                if (scratch[index] != g_ModeYShadow[index])
                {
                    ++g_ModeYFanoutNew;
                    g_ModeYShadow[index] = scratch[index];
                }
        for (index = 0; index < MODEY_WIN; ++index)
        {
            BYTE byteValue;
            if (scratch[index] == g_ModeYSeed[index])
                continue;                                         /* untouched: not this mask's business */
            ++g_ModeYFanoutBytes;
            byteValue = scratch[index];
            ModeYFanoutBarNote(index, g_ModeYPreviousMask);      /* is this how the bar collapses? */
            for (plane = 0; plane < VIDEO_PLANES; ++plane)
                if (g_ModeYPreviousMask & (1 << plane))
                    ((BYTE *)g_ModeYView[plane])[index] = byteValue;
        }
        ++g_ModeYFanouts;
    }
    if (mask < 0)
    {
        want = MODEY_VIEW_LINEAR;
        g_ModeYPreviousMask = 0;
    }
    else
    {
        for (plane = 0; plane < VIDEO_PLANES; ++plane)
            if (mask & (1 << plane))
                selector[selectedCount++] = plane;
        if (!selectedCount) /* mask 0: nothing to point at */
        {
            ++g_ModeYSelectorZero;
            return;
        }
        want = (selectedCount == 1) ? selector[0] : MODEY_VIEW_SCRATCH;
        g_ModeYPreviousMask = (selectedCount == 1) ? 0 : mask;
        /* The interpreter serves this window (ModeYNeedsInterp): its stores never
         * touch A0000's mapping, so there is nothing to seed and nothing to fan out --
         * which was ~93% of Doom-low's time and ~38% of Wolf3D's. Point the window at
         * the first selected plane so an instruction the interpreter declines (it runs
         * natively, counted as bail_mp) still lands in one right plane.
         */
        if (selectedCount > 1 && ModeYInterpServes())
        {
            want = selector[0];
            g_ModeYPreviousMask = 0;
        }
    }
    if (want == g_ModeYCurrent)
    {
        ++g_ModeYSelectorSame;
        return;
    }
    /* Before the mapping moves: what did the plane we are leaving actually receive? */
    if (g_ModeYCurrent >= 0 && g_ModeYCurrent < VIDEO_PLANES)
    {
        ModeYSampleCheck(0, YSMP_A, g_ModeYCurrent);
        ModeYSampleCheck(1, YSMP_B, g_ModeYCurrent);
    }
    if (!UnmapViewOfFile((LPVOID)(ULONG_PTR)VIDEO_APERTURE_BASE))
    {
        ++g_ModeYFail;
        return;
    }
    if (want == MODEY_VIEW_SCRATCH) {                                     /* seed the scratch so a
                                                            read-modify-write sees data,
                                                            and remember the seed so the
                                                            fan-out can tell writes from
                                                            bytes nobody touched */
        UINT index;
        BYTE *destination = (BYTE *)g_ModeYView[MODEY_VIEW_SCRATCH];
        const BYTE *source = (const BYTE *)g_ModeYView[selector[0]];
        for (index = 0; index < MODEY_WIN; ++index)
        {
            destination[index] = source[index];
            g_ModeYSeed[index] = source[index];
        }
        if (g_Video.ModeKind == VIDEO_KIND_LINEAR8)
            for (index = 0; index < MODEY_WIN; ++index)
                g_ModeYShadow[index] = source[index];
    }
    if (!MapViewOfFileEx(g_ModeYSeconds[want], FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE,
                         0, 0, MODEY_WIN, (LPVOID)(ULONG_PTR)VIDEO_APERTURE_BASE))
    {
        ++g_ModeYFail;
        /* Never leave the window unmapped: put SOMETHING back or the guest's next
         * store faults into a hole.
         */
        MapViewOfFileEx(g_ModeYSeconds[g_ModeYCurrent < 0 ? MODEY_VIEW_LINEAR : g_ModeYCurrent], FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE,
                        0, 0, MODEY_WIN, (LPVOID)(ULONG_PTR)VIDEO_APERTURE_BASE);
        return;
    }
    g_ModeYCurrent = want;
    ++g_ModeYSwaps;
}

BYTE *ModeYRemapPlane(PVOID context, INT plane)
{
    (VOID)context;
    return (BYTE *)g_ModeYView[plane & VIDEO_PLANE_INDEX_MASK];
}

/* DOES THE GUEST EVER READ A PLANE OTHER THAN THE ONE IT IS WRITING?:
 * A0000 holds ONE section, and `ModeYRemapSelect` positions it from the WRITE MASK.
 * A guest read of A0000 is served by the mapping directly -- the VDD never sees it --
 * so it returns the WRITE plane, whatever GR4 says. On the hardware those two are
 * independent registers.
 * Doom's `I_ReadScreen` reads all four planes into a linear buffer by cycling GR4
 * alone; the write mask is irrelevant to it and is left wherever the last blit put it.
 * Under this host every one of those four passes would read THE SAME PLANE, producing
 * a linear buffer whose every four-pixel group holds one plane's byte -- which is the
 * collapse, arriving via a path no write-side measurement could ever have seen.
 * - THE PAIR IS THE MEASUREMENT. `gr4_hist` says only that GR4 was written; the defect
 *   is GR4 disagreeing with the mapped plane, and only the host knows `g_ModeYCurrent`. If
 *   `mismatch` is ~0 this candidate dies in one run, like the linear section did.
 *   Measure first: do NOT move the window here yet. Remapping on GR4 would change what
 *   the guest sees mid-run and there would be no clean before/after.
 *
 * - - **AND THE MEASUREMENT IS IN.** Doom's screen read-back (I_ReadScreen) selects READ
 *   MAP SELECT (GC index 4), then for each of the four planes writes GR4 := plane -- its
 *   ONLY port write -- and reads 16000 bytes (64000/4) of video memory into
 *   `scr[plane + 4*i]` (observed: the port trace and the buffer it produces).
 *
 * It cycles the READ plane four times and **never writes the map mask**. Under this
 * host every one of those four passes is served by whatever section the WRITE mask
 * last left at A0000, so the buffer comes back as `scr[p + 4i] = plane_M[i]` for all
 * four p: every four-pixel group holding ONE plane's byte, replicated. That IS the
 * collapse, and it arrives through a path no write-side instrument could see -- which
 * is why every writer was excluded and the content was still there.
 * The run-length histogram agrees to the count: 193 runs of 4 GR4 writes with no
 * intervening mask change, plus 35 of 5, = 228 -- I_ReadScreen's call count. It is the
 * SCREEN WIPE (wipe_StartScreen / wipe_EndScreen). The 3D view is redrawn every frame
 * and heals; the status bar is repainted only where it CHANGES (ST_diffDraw), so its
 * collapsed pixels are never rewritten and the damage is permanent. That is also the
 * "the wipe looked pixelated until the full redraw cleaned up" note from session 22.
 *
 * - THE FIX: FOLLOW GR4. Point the window at the read plane. Safe because Doom sets the
 *   map mask before every plane WRITE (2,029,794 mask writes against 39,975 GR4 writes)
 *   and the pairing matrix shows the order is GR4-then-mask, so a write is always
 *   preceded by a mask change that puts the window back. Not done when the scratch is up
 *   (`g_ModeYCurrent == 5`): a multi-plane write window is mid-flight and I_ReadScreen never runs
 *   under one.
 */
DWORD g_ModeYGr4Calls = 0;
DWORD g_ModeYGr4Mismatch = 0;
DWORD g_ModeYGr4Pair[4][6];
/* AND THE MISMATCH ONLY BITES IF NO MASK CHANGE FOLLOWS:
 * `mismatch` is sampled at the instant GR4 is written, and 74% of those instants have
 * the window one plane behind -- but that is HARMLESS in the ordinary blit, where the
 * guest sets GR4 = p and then immediately sets the mask to 1<<p, moving the window
 * before any read happens. The pairing matrix cannot tell that apart from the case
 * that matters.
 * - COUNT GR4 WRITES BETWEEN CONSECUTIVE MASK CHANGES. A pure-READ pass sets GR4 four
 *   times and never touches the mask, so the window sits still for all four and every
 *   read returns ONE plane -- filling the guest's buffer with one plane's bytes
 *   replicated across each four-pixel group. That is the collapse, and a run length of
 *   4 with no intervening select is its fingerprint. A run of 1 is the ordinary blit and
 *   is fine. This is the counter that can come out either way.
 */
DWORD g_ModeYGr4SinceSelector = 0;
DWORD g_ModeYGr4Runs[10];
DWORD g_ModeYGr4RunPlanes[VIDEO_PLANES];
static VOID ModeYGr4CloseRun(VOID)
{
    if (g_ModeYGr4SinceSelector)
    {
        g_ModeYGr4Runs[g_ModeYGr4SinceSelector < 9 ? g_ModeYGr4SinceSelector : 9]++;
        /* A run of 4+ is a read pass: record which plane the window was stranded on,
         * because that plane's bytes are what the guest took away four times.
         */
        if (g_ModeYGr4SinceSelector >= 4 && g_ModeYCurrent >= 0 && g_ModeYCurrent < 4)
            g_ModeYGr4RunPlanes[g_ModeYCurrent]++;
        g_ModeYGr4SinceSelector = 0;
    }
}

DWORD g_ModeYGr4Moves = 0;
VOID ModeYRemapReadMap(PVOID context, INT plane)
{
    (VOID)context;
    if (!g_ModeYRemap)
        return;
    ++g_ModeYGr4Calls;
    ++g_ModeYGr4SinceSelector;
    if (g_ModeYCurrent >= 0 && g_ModeYCurrent < MODEY_NSEC)
        g_ModeYGr4Pair[plane & VIDEO_PLANE_INDEX_MASK][g_ModeYCurrent]++;
    if (plane != g_ModeYCurrent)
        ++g_ModeYGr4Mismatch;

    plane &= VIDEO_PLANE_INDEX_MASK;
    if (g_ModeYCurrent == MODEY_VIEW_SCRATCH || g_ModeYCurrent == plane)
        return;                                                                    /* scratch in flight, or already there */
    if (!UnmapViewOfFile((LPVOID)(ULONG_PTR)VIDEO_APERTURE_BASE))
    {
        ++g_ModeYFail;
        return;
    }
    if (!MapViewOfFileEx(g_ModeYSeconds[plane], FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE,
                         0, 0, MODEY_WIN, (LPVOID)(ULONG_PTR)VIDEO_APERTURE_BASE))
    {
        ++g_ModeYFail;
        MapViewOfFileEx(g_ModeYSeconds[g_ModeYCurrent < 0 ? MODEY_VIEW_LINEAR : g_ModeYCurrent], FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE,
                        0, 0, MODEY_WIN, (LPVOID)(ULONG_PTR)VIDEO_APERTURE_BASE);
        return;
    }
    g_ModeYCurrent = plane;
    ++g_ModeYGr4Moves;     /* NOT g_ModeYSwaps: the map-mask identity must keep balancing */
}

/* - THE COPY'S BOUNDARY IS THE WRITE-MODE CHANGE, NOT THE MASK CHANGE. Solving over a
 * whole mask window found nothing (44 windows, 0 solved): Doom sets `mask := 0x0F`
 * once and then does MANY separate latch copies under it, so the changed bytes are the
 * union of several moves and no single displacement explains them. Between `write mode
 * := 1` and `write mode := 0` there is exactly one burst, which is the thing a single
 * displacement CAN describe. Seed at the start of the burst, solve at its end.
 */
VOID ModeYRemapWriteMode(PVOID context, INT writeMode)
{
    (VOID)context;
    if (!g_ModeYRemap)
    {
        g_ModeYWriteMode = writeMode;
        return;
    }
    if (writeMode == 1 && g_ModeYWriteMode != 1)
    {
        if (g_ModeYCurrent == MODEY_VIEW_SCRATCH) {                          /* multi-plane: the scratch is the
                                                       only place the copy is visible */
            UINT index;
            const BYTE *scratch = (const BYTE *)g_ModeYView[MODEY_VIEW_SCRATCH];
            for (index = 0; index < MODEY_WIN; ++index)
                g_ModeYSeed[index] = scratch[index];
            g_ModeYLatch = 1;
        }
        /* A single-plane mask needs nothing: the guest is moving bytes inside the plane
         * that IS mapped, which is exactly what the hardware would do.
         */
    }
    else if (writeMode != 1 && g_ModeYWriteMode == 1 && g_ModeYLatch)
    {
        INT32 delta = ModeYLatchDelta();
        UINT index;
        const BYTE *scratch = (const BYTE *)g_ModeYView[MODEY_VIEW_SCRATCH];
        if (delta)
        {
            INT plane;
            for (index = 0; index < MODEY_WIN; ++index)
            {
                INT32 source;
                if (scratch[index] == g_ModeYSeed[index])
                    continue;
                source = (INT32)index - delta;
                for (plane = 0; plane < VIDEO_PLANES; ++plane)
                    if (g_ModeYPreviousMask & (1 << plane))
                        ((BYTE *)g_ModeYView[plane])[index] = ((BYTE *)g_ModeYView[plane])[source];
            }
            ++g_ModeYLatchOk;
        }
        else
        {
            /* [CAUTION]: THE UNSOLVED PATH USED TO DROP THE BURST ENTIRELY (Importance = 2):
             * The solver is 0-for-120: `dl` has never once been non-zero on a real
             * run, so this branch IS the burst path, not an edge case. It counted the
             * failure and did nothing -- and then the re-seed at the bottom of this
             * function overwrote g_ModeYSeed with the scratch, which is the very
             * comparison ModeYRemapSelect()'s fan-out uses to decide what to
             * propagate. So by the time the mask changed, every byte matched the seed
             * and the fan-out copied NOTHING. Measured, and this is what sent me
             * looking: `fanout_bar distinct=0` over a whole run while the burst
             * instrument reported ~10,014 changed bytes.
             * The guest computed those bytes and we threw them away.
             * - WHAT WE CAN AND CANNOT PUT BACK. Write mode 1 gives each plane its OWN
             *   latched byte, and the scratch holds ONE byte per offset -- it cannot
             *   represent four latches, which is precisely why every inference scheme
             *   over it has failed and why the solver never succeeds. Recovering the
             *   true per-plane bytes needs the accesses themselves, and the A0000 page
             *   trap that would provide them is MEASURED TWICE ON THIS BOX TO FREEZE
             *   THE GUEST (see A000Protect) -- so that door is shut.
             *   What we can do is stop discarding: propagate the scratch byte to the
             *   planes the mask selected, exactly as the write-mode-0 fan-out does.
             *   For the 6 CONSTANT bursts a run (fills of 0x00) that is EXACTLY right.
             *   For the 114 copy bursts it is an approximation -- plane 0's source byte
             *   reaching all four -- but it puts the guest's own data where stale
             *   content sits today.
             *
             * [CAUTION]: THAT TRADE IS NOT SELF-EVIDENT: it swaps "stale in four planes" for
             * "collapsed across four planes", and this project has been burned by
             * reasoning about which wrong picture is less wrong. So it is judged on
             * the WAD ORACLE (planejudge.py, plane-vs-STBAR), not on
             * bar_planes_equal, which by construction gets WORSE when we fan out.
             *
             * [CAUTION]: **AND IT WAS TRIED, MEASURED, AND NOT KEPT. DO NOT RE-APPLY IT.**
             * Propagating the scratch byte to the selected planes here delivers the
             * data -- `fanout_bar` band B went 0 -> 7352 writes over 192 distinct bar
             * offsets -- and the WAD oracle says it buys NOTHING:
             *     plane 0  34.4% -> 34.9%      plane 2  29.8% -> 29.8%
             *     plane 1  70.7% -> 70.2%      plane 3  27.8% -> 27.6%
             * Under a point either way, and it makes plane 1 -- the one plane that is
             * mostly CORRECT -- slightly worse, by smearing plane 0's bytes across it.
             * The informative part is what that implies: those 192 offsets were no
             * more wrong before than after, so **the bytes we discard are not what is
             * wrong with the status bar.** Session 23's refutation of the latch copy
             * stands, though not for the reason it gave (it argued from which rows the
             * bursts touch; the stronger argument is that delivering the data changes
             * nothing).
             * So keep DROPPING them, and keep COUNTING the drop, rather than shipping
             * an approximation that would read to the next session like the latch path
             * is handled. The real repair needs per-plane latch capture; see above for
             * why the page trap cannot provide it on this hardware, which leaves the
             * mode-12h-style interpreter as the only remaining candidate.
             */
            ++g_ModeYLatchUnsolved;
        }
        /* - WHAT IS A BURST, ACTUALLY? Two inference schemes have now failed on it --
         * plausible page strides explained 71 of 120 (the shape of a lucky guess) and
         * deriving the displacement from the copied run explained NONE. That second
         * result is the informative one: if the burst were a verbatim region copy, its
         * longest changed run would appear verbatim in the pre-burst image, and it does
         * not. So describe the burst instead of guessing at it: how much changed, over
         * what span, and whether the destination is a CONSTANT (a latched fill) rather
         * than a copy. Bounded to the first few, because one example settles it.
         */
        /* [CAUTION]: THIS BOUND WAS 6, AND IT PRODUCED A WRONG ROOT CAUSE. Six descriptions of
         * 160 bursts, of which only TWO had any changed bytes -- both at 0x3a1c..0x3e7f
         * -- were generalised into "the bursts only ever touch rows 186..199", and that
         * sentence retired the latch copy as the status bar's cause. Two samples out of
         * a hundred and sixty. A BOUND ON AN INSTRUMENT IS A CLAIM ABOUT WHAT IS
         * REPRESENTATIVE, and this one was never checked. 160 lines is ~19 KB; describe
         * them ALL, and let the row coverage be measured rather than extrapolated.
         */
        if (g_ModeYLatchDescriptor < 4096)
        {
            UINT index2;
            UINT count = 0;
            UINT low = MODEY_WIN;
            UINT high = 0;
            UINT uniqueCount = 0;
            UINT barBytes = 0;
            BYTE first = 0;
            INT constant = 1;
            for (index2 = 0; index2 < MODEY_WIN; ++index2)
            {
                UINT row;
                if (scratch[index2] == g_ModeYSeed[index2])
                    continue;
                if (!count)
                {
                    low = index2;
                    first = scratch[index2];
                }
                if (scratch[index2] != first)
                    constant = 0;
                high = index2;
                ++count;
                /* how much of this burst lands in the status bar at all */
                row = (index2 % 0x4000u) / 80u;
                if (row >= 168 && row < 200)
                    ++barBytes;
            }
            (VOID)uniqueCount;
            { CHAR writeModeLine[224], *lineCursor = writeModeLine;
              ++g_ModeYLatchDescriptor;
              lineCursor = LogPut(lineCursor, "MODEY-LATCH burst changed="); lineCursor = LogHex(lineCursor, count);
              lineCursor = LogPut(lineCursor, " span=0x"); lineCursor = LogHex(lineCursor, low);
              lineCursor = LogPut(lineCursor, "..0x"); lineCursor = LogHex(lineCursor, high);
              /* - IN THE UNITS OF THE CLAIM. A plane offset is row*80 + x/4, so a span
               * in hex says nothing about which rows a burst reaches -- and reading
               * 0x3a1c..0x3e7f as "the status bar" without converting it is exactly
               * how the wrong cause survived. Print the rows next to the offsets.
               */
              lineCursor = LogPut(lineCursor, " rows="); lineCursor = LogHex(lineCursor, count ? (low % 0x4000u) / 80u : 0);
              lineCursor = LogPut(lineCursor, "..");     lineCursor = LogHex(lineCursor, count ? (high % 0x4000u) / 80u : 0);
              lineCursor = LogPut(lineCursor, " barbytes="); lineCursor = LogHex(lineCursor, barBytes);
              lineCursor = LogPut(lineCursor, constant ? " DEST IS CONSTANT 0x" : " dest varies, first=0x");
              lineCursor = LogHexByte(lineCursor, first);
              lineCursor = LogPut(lineCursor, " mask=0x"); lineCursor = LogHexByte(lineCursor, (UINT)g_ModeYPreviousMask);
              lineCursor = LogPut(lineCursor, "\r\n");
              LogAppend(LOG_PATH, writeModeLine, lineCursor);
              SerialOut(writeModeLine, lineCursor); }
        }
        for (index = 0; index < MODEY_WIN; ++index)
            g_ModeYSeed[index] = scratch[index];                                           /* re-seed for the byte diff */
        g_ModeYLatch = 0;
    }
    g_ModeYWriteMode = writeMode;
}

/* In mode 12h, mark the A0000 graphics window NOACCESS so direct guest writes
 * fault to us; restore RW otherwise.
 */
static VOID A000Protect(INT isOn)
{
    DWORD old;

    if (g_NoA000)
        isOn = 0;                        /* diagnostic knob -- see NOA000_FLAG */
    if (isOn == g_A000Protection)
        return;
    if (VirtualProtect((LPVOID)VIDEO_APERTURE_BASE, X86_SEGMENT_SIZE_U,
                       isOn ? PAGE_NOACCESS : PAGE_EXECUTE_READWRITE, &old))
        g_A000Protection = isOn;
}

/* ---- how mode 12h is intercepted (GH #55) --------------------------------- *
 * MEASURED, twice, on the physical box: arming the A0000 page trap FREEZES the
 * guest. `PAGE_NOACCESS` and `PAGE_READONLY` behave identically (so it is not
 * reads-vs-writes, it is protecting the range at all), io_events stops at 10
 * against 22,532,292 with the trap off, and the exec thread never comes back out
 * of VdmStartExecution -- the guest's CS:IP in the TIB stays frozen at whatever
 * the last event left it. The M3 planar trap was VM-confirmed on HVF and NEVER on
 * real hardware, and there is precedent for exactly this class of difference
 * (session 8: HVF reflects IOPL-0 I/O as event 0, real silicon as event 3).
 *
 * So do not protect the page. Instead, while a planar mode is current, run the
 * guest in the HOST INTERPRETER, whose A0000 accesses go through the planar write
 * engine by construction (V86HostRead8/V86HostWrite8). The interpreter is the CPU for as long
 * as mode 12h is set; it yields whenever an IRQ is pending, and any opcode it does
 * not model drops that one instruction back to V86.
 */
INT g_P12Interp = 0;    /* planar mode is current -> interpret the guest */

VOID VideoTrapSync(VOID)
{
    INT planar = VddVideoIsPlanarActive(&g_Video);

    if (planar && !g_P12Offset)
    {
        A000Protect(FALSE);
        g_P12Interp = 1;
    }
    else
    {
        A000Protect(planar);
        g_P12Interp = 0;
    }
}

/* NORTH STAR 1: MODE Y'S MULTI-PLANE STORES GO THROUGH THE ADDRESS GENERATOR (Importance = 3):
 * (s80, design C of docs/research/modey-cost-measurement.md -- the user's choice.)
 * The remap serves a SINGLE-plane mask exactly: A0000 is that plane's own section and
 * a native store lands where the hardware would put it. Two cases have no mapping at
 * all, because one virtual page cannot store into several planes:
 *   - a MULTI-PLANE map mask (Doom's low detail 0x03/0x0c, Wolf3D's wall columns,
 *     Mario's 0x05/0x0a), and
 *   - WRITE MODE != 0 (a latch copy moves all four planes at once).
 * For those the scratch + fan-out approximated -- wrongly, because a value diff cannot
 * see a store of the value already there -- and slowly (measured: Doom low ~93% of
 * every second, Wolf3D ~38%).
 * - So for exactly those windows the host interpreter is the CPU, and every A0000 store
 *   goes through VddVideoPlanarWrite() into every selected plane AT THE TIME OF THE WRITE.
 *   The window is known exactly -- it opens and closes on a trapped OUT to 3C5h/3CFh --
 *   so the rest of the program runs natively. It cannot be entered by a page fault:
 *   protecting A0000 freezes a V86 guest on real hardware (see above).
 *
 * [CAUTION]: REAL MODE ONLY for now: v86interp.h is 16-bit. Doom's renderer is 32-bit protected-
 * mode code and keeps the old path (g_DpmiPm) until the interpreter learns 32-bit
 * addressing -- the second half of this north star.
 */
/* Does the interpreter serve mode Y's multi-plane windows for this guest at all? Then
 * the remap need not build a scratch window for them (see ModeYRemapSelectBody).
 */
static INT ModeYInterpServes(VOID)
{
    if (g_ModeYInterpOffset || g_Video.ModeKind != VIDEO_KIND_LINEAR8)
        return 0;
    /* A PM guest's drawers are interpreted (ModeYPmRun); its OTHER code runs natively,
     * so with the detector on keep the scratch window to catch what that code stores.
     */
    if (g_DpmiPm)
        return !g_ModeYPmOffset && !g_ModeYPmDetect;
    return 1;
}

/* Is the guest RIGHT NOW in a window no mapping can serve? */
INT ModeYNeedsInterp(VOID)
{
    BYTE mask;

    if (!g_ModeYRemap || !ModeYInterpServes() || g_Video.IsChain4)
        return 0;
    mask = (BYTE)(g_Video.MapMask & VIDEO_ALL_PLANES);
    return (mask & (BYTE)(mask - 1)) != 0 || (g_Video.WriteMode & VIDEO_WRITE_MODE_MASK) != 0;
}

/* ====================================================================== *
 *  Mode-12h fill-loop fast path: a small, bounded, flags-accurate 8086    *
 *  interpreter.                                                           *
 *                                                                         *
 *  In mode 12h the A0000 window is PAGE_NOACCESS, so every guest pixel    *
 *  touch faults to us. QuickBASIC's PAINT/LINE fills are tight per-pixel  *
 *  loops (e.g. `MOV AL,ES:[SI] / OR AL,AL / JNZ / DEC DI / JNZ`), so one  *
 *  fill = hundreds of thousands of V86 round-trips and never finishes.    *
 *                                                                         *
 *  Fix: when an A0000 access faults, run the *whole* inner loop here --   *
 *  loads/stores (planar engine for A0000, flat for normal RAM), the       *
 *  arithmetic/logic group (computing CF/PF/AF/ZF/SF/OF), INC/DEC, string  *
 *  ops (REP, honouring DF), MOV, TEST, the flag ops, and Jcc/JMP/LOOP --  *
 *  until we hit an opcode we don't model or an iteration cap. Then we     *
 *  write the architectural state back and return to V86. It NEVER         *
 *  derails: any unmodeled byte stops the interpreter with EIP exactly on  *
 *  that instruction, so V86 re-executes it. 16-bit only (0x66/0x67/LOCK   *
 *  bail). One fault now drives the entire fill instead of one-per-pixel.  *
 * ======================================================================
 */
enum
{
    PAGE_MAP_UNKNOWN = 0, PAGE_MAP_OK = 1, PAGE_MAP_BAD = 2
};   /* g_PageMap: per 4 KB page of the low 1 MB */
/* [CAUTION]: A STRAY GUEST POINTER MUST NOT JAM THE MACHINE (Importance = 2):
 * imem treats a guest LINEAR address as a HOST virtual address -- true for the low
 * megabyte NTVDM identity-maps, but the UMB region (0xC0000-0xEFFFF) has HOLES that
 * are not committed, and a guest (or an interpreter state bug) that dereferences one
 * raises an access violation that takes the whole host down -- exactly the "NTVDMEX
 * can jam the machine" failure this project has a standing rule against.
 *
 * [INFO]: MEASURED (s69, user's live rig): Lemmings' sprite blitter faulted reading guest
 * linear 0xd4013 -- es=0xa000 in the code, yet the interpreter's effective address
 * landed in the 0xD0000 UMB hole. Whatever the root cause of the bad address, the
 * host must DEGRADE, not die: an unmapped read is 0xFF on real hardware (floating
 * bus) and an unmapped write is dropped.
 * - A per-4KB-page validity cache, probed once with VirtualQuery. imem is hot (every
 *   interpreted byte), so the fast path is a single array lookup; the syscall happens
 *   at most once per page. 0=unknown, 1=ok, 2=bad. The low conventional memory and our
 *   own aperture never reach the probe (handled above / by the A000 branch), so the
 *   cost falls only on the upper-memory accesses that are the anomaly.
 */
static BYTE g_PageMap[X86_REAL_MODE_SIZE_U >> PAGE_SHIFT];     /* one entry per 4KB page of the low 1MB */
DWORD g_InterpreterMemoryBadReads;
DWORD g_InterpreterMemoryBadWrites;
DWORD g_InterpreterMemoryBadLogged;
INT InterpreterMemoryPageOk(UINT32 linear)
{
    UINT32 page = linear >> PAGE_SHIFT;

    if (linear >= X86_REAL_MODE_SIZE_U)
        return 1;                                             /* HMA and above: leave to the raw path */
    if (g_PageMap[page] == PAGE_MAP_UNKNOWN)
    {
        MEMORY_BASIC_INFORMATION memoryInfo;
        INT isOk = 0;
        if (VirtualQuery((LPCVOID)(ULONG_PTR)(page << PAGE_SHIFT), &memoryInfo, sizeof memoryInfo) == sizeof memoryInfo)
            isOk = (memoryInfo.State == MEM_COMMIT) &&
                 !(memoryInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        g_PageMap[page] = (BYTE)(isOk ? PAGE_MAP_OK : PAGE_MAP_BAD);
    }
    return g_PageMap[page] == PAGE_MAP_OK;
}

/* Flat/planar guest memory for the interpreter: A0000 goes through the VGA
 * engine (a read loads the latches), everything else is the directly-mapped
 * V86 address space. These are the host hooks v86interp.h requires.
 */
/* #183 (s87): inline, with everything but "a known-good page of plain RAM" out of line.
 * The interpreter fetches every code byte through V86HostRead8, and it was a real cdecl call
 * per byte. g_PageMap[pg]==1 is exactly the old InterpreterMemoryPageOk() answer for a page already
 * probed below 1 MB; anything else (the aperture, HMA, an unprobed or bad page) takes
 * the slow path, which is the old function unchanged.
 */
static __attribute__((noinline)) BYTE InterpreterMemoryRead8Slow(UINT32 linear)
{ if (linear >= VIDEO_APERTURE_BASE && linear < VIDEO_MONO_BASE)
    return VddVideoPlanarRead(&g_Video, linear - VIDEO_APERTURE_BASE);
  if (!InterpreterMemoryPageOk(linear))
  {
      g_InterpreterMemoryBadReads++;
      InterpreterMemoryBadNote(linear, X86_ACCESS_READ);
      return VDD_UNCLAIMED_READ_BYTE;
  }
  return *(volatile BYTE *)linear; }
static __attribute__((noinline)) VOID InterpreterMemoryWrite8Slow(UINT32 linear, BYTE value)
{ if (linear >= VIDEO_APERTURE_BASE && linear < VIDEO_MONO_BASE)
{
    VddVideoPlanarWrite(&g_Video, linear - VIDEO_APERTURE_BASE, value);
    return;
}

  if (!InterpreterMemoryPageOk(linear))
  {
      g_InterpreterMemoryBadWrites++;
      InterpreterMemoryBadNote(linear, X86_ACCESS_WRITE);
      return;
  }
  *(volatile BYTE *)linear = value; }
static inline __attribute__((always_inline)) BYTE V86HostRead8(UINT32 linear)
{ if (linear < X86_REAL_MODE_SIZE_U && g_PageMap[linear >> PAGE_SHIFT] == PAGE_MAP_OK && (linear < VIDEO_APERTURE_BASE || linear >= VIDEO_MONO_BASE))
      return *(volatile BYTE *)linear;
  return InterpreterMemoryRead8Slow(linear); }
static inline __attribute__((always_inline)) VOID V86HostWrite8(UINT32 linear, BYTE value)
{ if (linear < X86_REAL_MODE_SIZE_U && g_PageMap[linear >> PAGE_SHIFT] == PAGE_MAP_OK && (linear < VIDEO_APERTURE_BASE || linear >= VIDEO_MONO_BASE))
      {
          *(volatile BYTE *)linear = value;
          return;
      }
  InterpreterMemoryWrite8Slow(linear, value); }
static inline __attribute__((always_inline)) const volatile BYTE *V86HostCodePointer(UINT32 linear)
{ if (linear < X86_REAL_MODE_SIZE_U && (linear & PAGE_LAST_BYTE_U) <= PAGE_LAST_PARAGRAPH_U && g_PageMap[linear >> PAGE_SHIFT] == PAGE_MAP_OK
      && (linear < VIDEO_APERTURE_BASE || linear >= VIDEO_MONO_BASE))
      return (const volatile BYTE *)(ULONG_PTR)linear;
  return 0; }

/* Port I/O dispatched to the device bus (same path as HostTryIo). The
 * interpreter already runs under g_Lock, which is what the bus needs.
 */
/* -- THE PIT MUST READ REAL TIME ON THIS PATH TOO (s70). The reflected port
 * path calls HostPitSync() before every 0x40-0x43 access "so a poll always reads
 * real time"; this path did not, so a count LOADED here and LATCHED here each saw
 * total_clocks as of the last pacer round -- stale by up to a period, in different
 * amounts. Lemmings' High-Performance calibration runs interpreted (planar mode):
 * its 320-hblank count came back 306..330 lines across runs (0x2D84..0x315B), in
 * BOTH directions, which no missed-pulse story explains, and its timer tick -- the
 * row where it switches palettes -- landed anywhere from row 153 to 165. Generate
 * only: delivery takes g_Lock by TRY and the interpreter already holds it.
 */
static UINT32 V86HostIn(WORD port, INT width)
{ UINT32 value = 0;
  if (port >= PIT_PORT_COUNTER0 && port <= PIT_PORT_CONTROL)
      HostPitGenerate();
  VddBusIo(&g_Bus, port, (BYTE)width, VDD_IO_IN, &value);
  return value; }
static VOID V86HostOut(WORD port, INT width, UINT32 byteValue)
{ UINT32 value = byteValue;
  if (port >= PIT_PORT_COUNTER0 && port <= PIT_PORT_CONTROL)
      HostPitGenerate();
  VddBusIo(&g_Bus, port, (BYTE)width, VDD_IO_OUT, &value);
  if (port == PIT_PORT_CONTROL)
      PitLatchNote((BYTE)byteValue);                               /* same instrument as the reflected path */
  if (port == PIT_PORT_COUNTER0)
      HostPitResyncCheck(); }         /* and the same resync rule */

enum
{
    PM32_PAGE_READ = 1, PM32_PAGE_WRITE = 2
};   /* g_Pm32PageCache access bits */
/* THE FLAT 32-BIT INTERPRETER'S HOST HOOKS (north star 1, Doom):
 * Same division as imem_*: A0000-AFFFF goes through the VGA engine (a read loads the
 * latches, a write reaches every plane the map mask selects), everything else is flat
 * host memory -- a DOS/4GW client's linear addresses ARE host VAs. Pm32HostCanAccess() is the
 * guard that turns a stray pointer into a DECLINE instead of a host access violation:
 * a small page cache over VirtualQuery, so the syscall happens once per page.
 */
#define P32_PGC     256
static struct
{
    DWORD Page;
    BYTE Access;
} g_Pm32PageCache[P32_PGC];
static INT Pm32PageOk(DWORD page, INT isWrite)
{
    UINT slot = (UINT)(page * KNUTH_HASH_MULTIPLIER_U) >> TOP_BYTE_SHIFT;

    if (g_Pm32PageCache[slot].Page != page + 1)
    {
        MEMORY_BASIC_INFORMATION memoryInfo;
        BYTE access = 0;
        if (VirtualQuery((LPCVOID)(ULONG_PTR)(page << PAGE_SHIFT), &memoryInfo, sizeof memoryInfo) == sizeof memoryInfo
            && memoryInfo.State == MEM_COMMIT && !(memoryInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        {
            access = PM32_PAGE_READ;
            if (memoryInfo.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY
                               | PAGE_EXECUTE_WRITECOPY))
                access = PM32_PAGE_READ | PM32_PAGE_WRITE;
        }
        g_Pm32PageCache[slot].Page = page + 1;
        g_Pm32PageCache[slot].Access = access;
    }
    return isWrite ? (g_Pm32PageCache[slot].Access & PM32_PAGE_WRITE) != 0 : (g_Pm32PageCache[slot].Access & PM32_PAGE_READ) != 0;
}

static DWORD g_Pm32VgaCount = 0;      /* aperture accesses by the interpreter (ModeYPmRun) */
static BYTE Pm32HostRead8(UINT32 linear)
{
    if (linear >= VIDEO_APERTURE_BASE && linear < VIDEO_MONO_BASE)
    {
        ++g_Pm32VgaCount;
        return VddVideoPlanarRead(&g_Video, linear - VIDEO_APERTURE_BASE);
    }
    return *(volatile BYTE *)(ULONG_PTR)linear;
}

static VOID Pm32HostWrite8(UINT32 linear, BYTE value)
{
    if (linear >= VIDEO_APERTURE_BASE && linear < VIDEO_MONO_BASE)
    {
        ++g_Pm32VgaCount;
        VddVideoPlanarWrite(&g_Video, linear - VIDEO_APERTURE_BASE, value);
        return;
    }
    *(volatile BYTE *)(ULONG_PTR)linear = value;
}

static INT Pm32HostCanAccess(UINT32 linear, INT width, INT isWrite)
{
    UINT32 end = linear + (UINT32)width - 1;

    if (end < linear)
        return 0;
    if (linear >= VIDEO_APERTURE_BASE && end < VIDEO_MONO_BASE)
        return 1;
    if (linear < VIDEO_MONO_BASE && end >= VIDEO_APERTURE_BASE)
        return 0;                                                                  /* straddles the aperture */
    if (!Pm32PageOk(linear >> PAGE_SHIFT, isWrite))
        return 0;
    return ((end >> PAGE_SHIFT) == (linear >> PAGE_SHIFT)) || Pm32PageOk(end >> PAGE_SHIFT, isWrite);
}

static UINT32 Pm32HostIn(WORD port, INT width)
{
    return V86HostIn(port, width);
}

static VOID     Pm32HostOut(WORD port, INT width, UINT32 value)
{
    V86HostOut(port, width, value);
}

/* Is a PROTECTED-mode guest in a window no mapping can serve? (The real-mode twin is
 * ModeYNeedsInterp.)
 */
INT ModeYPmNeedsInterp(VOID)
{
    BYTE mask;

    if (!g_DpmiPm || g_ModeYPmOffset || g_ModeYInterpOffset || !g_ModeYRemap)
        return 0;
    if (g_Video.ModeKind != VIDEO_KIND_LINEAR8 || g_Video.IsChain4)
        return 0;
    mask = (BYTE)(g_Video.MapMask & VIDEO_ALL_PLANES);
    return (mask & (BYTE)(mask - 1)) != 0 || (g_Video.WriteMode & VIDEO_WRITE_MODE_MASK) != 0;
}

/* DOOM'S DRAWERS THROUGH THE ADDRESS GENERATOR. (s80, north star 1) (Importance = 3):
 * Doom sets its two-plane mask with an OUT at the top of R_DrawColumnLow / the span
 * drawer; that OUT has just trapped and been serviced. Run the guest here from the next
 * instruction until the drawer RETURNS -- a RET that takes ESP above where we started --
 * so every store it makes goes through VddVideoPlanarWrite into both planes.
 * - ...BUT ONLY ONCE THE RUN HAS TOUCHED THE APERTURE. Doom also sets masks through a
 *   generic helper (`out dx,ax / pop ebx / ret`) and then does the work in the CALLER --
 *   the status-bar latch copies, the 0Fh clears. Stopping at the helper's own RET handed
 *   that work to the real CPU, and the detector (modeypm_detect.flag) measured it: up to
 *   1,233 native stores a second under a multi-plane mask. A drawer writes and returns;
 *   a helper returns having written nothing, and interpretation follows it home. Also stop when
 *   the window closes (a single-plane mask is served natively and exactly), on a decline
 *   (that instruction, and what follows until the next trap, runs natively -- counted),
 *   or at a cap. Everything Doom does between drawers stays on the real CPU: interpreting
 *   the whole renderer while the mask happens to be multi-plane would cost it its frame
 *   rate (docs/research/modey-cost-measurement.md).
 */
#define MYPM_CAP    400000L

/* #172: AN INTERRUPT IS WAITING AND THE RUN HAS STOPPED DRAWING:
 * Doom's quit prompt leaves the map mask multi-plane, so every 3DAh read of I_WaitVBL
 * trapped straight in here -- and a poll loop never RETURNS and never touches the
 * aperture, so each run went to MYPM_CAP: 400,000 interpreted instructions, ~27 ms on
 * the rig, holding g_Lock. No tick could be placed (the cooperative check is in the PM
 * loop; the async arm needs g_Lock) and the mixer waited behind the lock too. Measured:
 * 54 IRQ0s in the 1.45 s wait (37/s against the 140 Doom programmed), each 27 ms apart,
 * all at the poll loop's EIP. Real hardware takes the interrupt between instructions.
 * So stop when something is waiting to be delivered -- but only after MYPM_IDLE
 * instructions WITHOUT an aperture access: a drawer stores every few instructions and
 * is never cut short (the rest of it would run natively under a multi-plane mask and
 * leak). A poll loop is released within MYPM_IDLE instructions, ~0.6 ms.
 */
#define MYPM_IDLE   4096L
static INT ModeYPmIrqWaiting(VOID)
{
    return g_PmTickOwed > 0 || g_Irq1Pending > 0
        || (g_Pic.Master.Irr & (BYTE)~g_Pic.Master.Imr) != 0;
}

VOID ModeYPmRun(volatile BYTE *tib)
{
    PM32_CPU cpu;
    INT32 steps = 0;
    INT32 idleFrom = 0;
    UINT32 espStart;
    INT why;
    DWORD vgaSeen;
    WORD selector[X86_SEGMENT_REGISTERS];
    INT index;
    WORD cs = (WORD)VDM_REG16(tib, VTIB_CS);
    WORD ss = (WORD)VDM_REG16(tib, VTIB_SS);

    if (!DpmiSelectorIs32(cs) || !DpmiSelectorIs32(ss))
    {
        g_ModeYPmStop[MYPM_STOP_NOT_FLAT]++;
        return;
    }
    selector[X86_SREG_ES] = (WORD)VDM_REG(tib, VTIB_ES);
    selector[X86_SREG_CS] = cs;
    selector[X86_SREG_SS] = ss;
    selector[X86_SREG_DS] = (WORD)VDM_REG(tib, VTIB_DS);
    selector[X86_SREG_FS] = (WORD)VDM_REG(tib, VTIB_FS);
    selector[X86_SREG_GS] = (WORD)VDM_REG(tib, VTIB_GS);
    for (index = 0; index < X86_SEGMENT_REGISTERS; ++index)
        cpu.SegmentBases[index] = (selector[index] & X86_SELECTOR_NULL_MASK) ? DpmiSelectorBase(selector[index]) : 0;
    cpu.Registers[X86_REG_AX] = VDM_REG(tib, VTIB_EAX);
    cpu.Registers[X86_REG_CX] = VDM_REG(tib, VTIB_ECX);
    cpu.Registers[X86_REG_DX] = VDM_REG(tib, VTIB_EDX);
    cpu.Registers[X86_REG_BX] = VDM_REG(tib, VTIB_EBX);
    cpu.Registers[X86_REG_SP] = VDM_REG(tib, VTIB_ESP);
    cpu.Registers[X86_REG_BP] = VDM_REG(tib, VTIB_EBP);
    cpu.Registers[X86_REG_SI] = VDM_REG(tib, VTIB_ESI);
    cpu.Registers[X86_REG_DI] = VDM_REG(tib, VTIB_EDI);
    cpu.Eip = VDM_REG(tib, VTIB_EIP);
    cpu.Flags = VDM_REG(tib, VTIB_EFLAGS);
    espStart = cpu.Registers[X86_REG_SP];
    { DWORD vgaCountStart = g_Pm32VgaCount;
    vgaSeen = vgaCountStart;
    HOST_LOCK();
    for (;;)
    {
        BYTE opcodeByte = Pm32HostRead8(cpu.SegmentBases[X86_SREG_CS] + cpu.Eip);
        INT wasResult = (opcodeByte == X86_OP_RET || opcodeByte == X86_OP_RET_IMM);
        if (steps >= MYPM_CAP)
        {
            why = MYPM_STOP_CAP;
            break;
        }
        if (!Pm32Step(&cpu))
        {
            why = MYPM_STOP_DECLINED;
            break;
        }
        ++steps;
        if (wasResult && cpu.Registers[X86_REG_SP] > espStart && g_Pm32VgaCount != vgaCountStart)
        {
            why = MYPM_STOP_RETURNED;
            break;
        }
        if ((steps & MYPM_CHECK_MASK) == 0)
        {
            if (!ModeYPmNeedsInterp())
            {
                why = MYPM_STOP_WINDOW_CLOSED;
                break;
            }
            if (g_Pm32VgaCount != vgaSeen)
            {
                vgaSeen = g_Pm32VgaCount;
                idleFrom = steps;
            }
            else if (steps - idleFrom >= MYPM_IDLE && ModeYPmIrqWaiting())
            {
                why = MYPM_STOP_IRQ_WAITING;
                break;
            }
        }
    }
    HOST_UNLOCK();
    }
    g_ModeYPmRuns++;
    g_ModeYPmInstructions += (DWORD)steps;
    g_ModeYPmStop[why]++;
    if (why == MYPM_STOP_DECLINED)
    {
        BYTE mapMask = (BYTE)(g_Video.MapMask & VIDEO_ALL_PLANES);
        g_ModeYPmBails++;
        if (mapMask & (BYTE)(mapMask - 1))
            g_ModeYPmBailMp++;
        ModeYBailNote(cs, cpu.Eip, (const volatile BYTE *)(ULONG_PTR)(cpu.SegmentBases[X86_SREG_CS] + cpu.Eip));
    }
    if (!steps)
        return;
    VDM_REG(tib, VTIB_EAX) = cpu.Registers[X86_REG_AX];
    VDM_REG(tib, VTIB_ECX) = cpu.Registers[X86_REG_CX];
    VDM_REG(tib, VTIB_EDX) = cpu.Registers[X86_REG_DX];
    VDM_REG(tib, VTIB_EBX) = cpu.Registers[X86_REG_BX];
    VDM_REG(tib, VTIB_ESP) = cpu.Registers[X86_REG_SP];
    VDM_REG(tib, VTIB_EBP) = cpu.Registers[X86_REG_BP];
    VDM_REG(tib, VTIB_ESI) = cpu.Registers[X86_REG_SI];
    VDM_REG(tib, VTIB_EDI) = cpu.Registers[X86_REG_DI];
    VDM_REG(tib, VTIB_EIP) = cpu.Eip;
    /* the arithmetic flags and DF only: IF, IOPL, VM and the rest are the monitor's */
    VDM_REG(tib, VTIB_EFLAGS) = (VDM_REG(tib, VTIB_EFLAGS) & ~EFLAGS_STATUS_DF_U) | (cpu.Flags & EFLAGS_STATUS_DF_U);
}

/* The interpreter's live register file, for the fatal dump and the OOR logger. A
 * crash or a stray access inside istep (s69) leaves the VDM context a whole slice
 * stale; this is the es/di the effective address was actually built from. NULL when
 * the interpreter is not running.
 */
const V86_CPU *g_InterpreterCpu;

/* Name the FIRST few out-of-range accesses: the guest cs:ip, and the interpreter's
 * live es/di/ds -- which is what says whether a bad address is a wrong SEGMENT or an
 * unmasked OFFSET (s69, Lemmings' blit to a 0xD0000 hole). Bounded; graceful.
 */
static VOID InterpreterMemoryBadNote(UINT32 linear, INT write)
{
    CHAR buffer[224];
    CHAR *cursor = buffer;

    if (g_InterpreterMemoryBadLogged >= 12)
        return;
    g_InterpreterMemoryBadLogged++;
    cursor = LogPut(cursor, "IMEM-OOR "); cursor = LogPut(cursor, write ? "W" : "R"); cursor = LogPut(cursor, " lin=0x"); cursor = LogHex(cursor, linear);
    cursor = LogPut(cursor, " guest cs:ip=0x"); cursor = LogHex(cursor, g_V86InstructionPointer);
    if (g_InterpreterCpu) { const V86_CPU *cpu = g_InterpreterCpu;
        cursor = LogPut(cursor, " es=0x"); cursor = LogHex(cursor, cpu->Segments[0]);
        cursor = LogPut(cursor, " ds=0x"); cursor = LogHex(cursor, cpu->Segments[3]);
        cursor = LogPut(cursor, " di=0x"); cursor = LogHex(cursor, cpu->Registers[7]);
        cursor = LogPut(cursor, " si=0x");
        cursor = LogHex(cursor, cpu->Registers[6]); }
    cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, buffer, cursor);
}

/* Where the guest was at the last interpreted instruction. Every planar VRAM write
 * arrives through V86HostWrite8 above, i.e. from the interpreter, so this is exact at the
 * moment the video VDD's watchpoint fires.
 */
UINT32 HostGuestPc(VOID)
{
    return g_V86InstructionPointer;
}

/* WHAT DID THE INTERPRETER JUST RUN? (s80, north star 1):
 * Design C hands Wolf3D's renderer to the interpreter, and the first run ended with the
 * guest executing the interrupt vector table (0000:0078) as code -- an instruction we
 * MODEL got something wrong (an unmodelled one bails to the real CPU, which cannot).
 * A ring of the last 64 interpreted instructions, written only while interpreting for
 * mode Y, dumped once when the guest lands at CS=0: the culprit is in it.
 */
#define MY_RING     64
static struct
{
    WORD Cs;
    WORD Ip;
    WORD Sp;
    WORD Ss;
    BYTE Bytes[6];
} g_ModeYRing[MY_RING];
static UINT g_ModeYRingPosition = 0;
static INT      g_ModeYRingDumped = 0;
VOID ModeYRingDump(PCSTR why)
{
    CHAR lineBuffer[160];
    CHAR *cursor;
    UINT age;
    UINT byteIndex;

    if (g_ModeYRingDumped)
        return;
    g_ModeYRingDumped = 1;
    cursor = lineBuffer; cursor = LogPut(cursor, "MODEY-INTERP RING ("); cursor = LogPut(cursor, why); cursor = LogPut(cursor, "), oldest first:\r\n");
    LogAppend(LOG_PATH, lineBuffer, cursor);
    for (age = 0; age < MY_RING; ++age)
    {
        UINT index = (g_ModeYRingPosition + age) % MY_RING;
        if (!g_ModeYRing[index].Cs && !g_ModeYRing[index].Ip)
            continue;
        cursor = lineBuffer;
        cursor = LogPut(cursor, "  "); cursor = LogHex(cursor, g_ModeYRing[index].Cs); cursor = LogPut(cursor, ":"); cursor = LogHex(cursor, g_ModeYRing[index].Ip);
        cursor = LogPut(cursor, " ss:sp="); cursor = LogHex(cursor, g_ModeYRing[index].Ss); cursor = LogPut(cursor, ":"); cursor = LogHex(cursor, g_ModeYRing[index].Sp);
        cursor = LogPut(cursor, "  ");
        for (byteIndex = 0; byteIndex < 6; ++byteIndex)
        {
            cursor = LogHexByte(cursor, g_ModeYRing[index].Bytes[byteIndex]);
            cursor = LogPut(cursor, " ");
        }
        cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, lineBuffer, cursor);
    }
}

/* An injected interrupt, as a ring entry: cs=FFFE, ip=vector, ss:sp = the cs:ip it
 * interrupted. Only once mode Y has been interpreted -- the ring is its instrument.
 */
VOID ModeYRingNoteIrq(UINT vector, WORD cs, WORD ip, WORD ss, WORD sp)
{
    UINT index;
    UINT byteIndex;

    (VOID)ss;
    (VOID)sp;
    if (!g_ModeYRingOn || !g_ModeYSlices || g_ModeYRingDumped)
        return;
    index = g_ModeYRingPosition++ % MY_RING;
    g_ModeYRing[index].Cs = 0xFFFE;
    g_ModeYRing[index].Ip = (WORD)vector;
    g_ModeYRing[index].Ss = cs;
    g_ModeYRing[index].Sp = ip;
    for (byteIndex = 0; byteIndex < 6; ++byteIndex)
        g_ModeYRing[index].Bytes[byteIndex] = 0;
}

enum
{
    HOST_PROFILE_STOP_WAIT_MS = 5
};   /* let the sampler see the stop */
/* #183: WHERE DOES THE HOST'S TIME GO? A SAMPLING PROFILER, OPT-IN:
 * The interpreter's cost was estimated from the outside (instructions/s, us/s), and a
 * change aimed at the estimate -- a jump table for opcode dispatch, +42% off-VM --
 * moved the rig by nothing. So measure the inside: a thread suspends the exec thread
 * about every millisecond, reads its EIP, and if it lies in our own image counts it in
 * a 16-byte bucket. Suspending a thread that holds a lock is safe here because the
 * sampler takes none. Buckets are RVAs; `nm -n` on the same build names them.
 */
#define HPROF_SHIFT     4
static volatile LONG g_HostProfileOn;
static HANDLE   g_HostProfileThread;
static DWORD   *g_HostProfile;                 /* one counter per 16 bytes of the image */
static DWORD g_HostProfileCount;
static DWORD g_HostProfileSamples;
static DWORD g_HostProfileIn;
static DWORD g_HostProfileBase;
static DWORD g_HostProfileSize;
/* ...and everything that is NOT our image, because half the samples were not: the
 * guest running natively (VM flag), the kernel, and other user-mode code by 64 KB.
 */
static DWORD g_HostProfileV86;
static DWORD g_HostProfileKernel;
static DWORD g_HostProfileOther;
static DWORD    g_HostProfileSegment[0x8000];     /* user space below 2 GB, per 64 KB */
static DWORD WINAPI HostProfileThread(LPVOID parameter)
{
    (VOID)parameter;
    while (g_HostProfileOn)
    {
        CONTEXT context;
        Sleep(1);
        if (SuspendThread(g_HostProfileThread) == (DWORD)-1)
            break;
        context.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(g_HostProfileThread, &context))
        {
            ++g_HostProfileSamples;
            if (context.EFlags & EFLAGS_VM)
                ++g_HostProfileV86;
            else if (context.Eip >= g_HostProfileBase && context.Eip < g_HostProfileBase + g_HostProfileSize)
            {
                ++g_HostProfileIn;
                ++g_HostProfile[(context.Eip - g_HostProfileBase) >> HPROF_SHIFT];
            }
            else if (context.Eip >= 0x80000000u)
                ++g_HostProfileKernel;
            else
            {
                ++g_HostProfileOther;
                ++g_HostProfileSegment[context.Eip >> WORD_SHIFT];
            }
        }
        ResumeThread(g_HostProfileThread);
    }
    return 0;
}

VOID HostProfileStart(VOID)
{
    const BYTE *image = (const BYTE *)GetModuleHandleA(NULL);
    DWORD newHeaderOffset;
    DWORD size;

    if (GetFileAttributesA(HOSTPROF_FLAG) == INVALID_FILE_ATTRIBUTES)
        return;
    newHeaderOffset = *(const DWORD *)(image + DOS_MZ_NEW_HEADER);
    size = *(const DWORD *)(image + newHeaderOffset + FIELD_OFFSET(IMAGE_NT_HEADERS32, OptionalHeader.SizeOfImage));                     /* SizeOfImage */
    g_HostProfileBase = (DWORD)(ULONG_PTR)image;
    g_HostProfileSize = size;
    g_HostProfileCount = (size >> HPROF_SHIFT) + 1;
    g_HostProfile = (DWORD *)VirtualAlloc(NULL, g_HostProfileCount * sizeof(DWORD), MEM_COMMIT, PAGE_READWRITE);
    if (!g_HostProfile)
        return;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &g_HostProfileThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    g_HostProfileOn = 1;
    { HANDLE thread = CreateThread(NULL, 0, HostProfileThread, NULL, 0, NULL);
      if (thread)
      {
          SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
          CloseHandle(thread);
      }
      }
}

static VOID HostProfileDump(VOID)
{
    CHAR buffer[160];
    CHAR *cursor;
    INT rank;

    if (!g_HostProfile)
        return;
    g_HostProfileOn = 0;
    Sleep(HOST_PROFILE_STOP_WAIT_MS);
    cursor = LogPut(buffer, "STAGE2: HOSTPROF samples="); cursor = LogDecimal(cursor, g_HostProfileSamples);
    cursor = LogPut(cursor, " in_host_image="); cursor = LogDecimal(cursor, g_HostProfileIn);
    cursor = LogPut(cursor, " image_base=0x"); cursor = LogHex(cursor, g_HostProfileBase);
    cursor = LogPut(cursor, " (RVA buckets of 16 bytes, hottest first)\r\n"); LogAppend(LOG_PATH, buffer, cursor);
    cursor = LogPut(buffer, "STAGE2: HOSTPROF guest_v86="); cursor = LogDecimal(cursor, g_HostProfileV86);
    cursor = LogPut(cursor, " kernel="); cursor = LogDecimal(cursor, g_HostProfileKernel);
    cursor = LogPut(cursor, " other_user="); cursor = LogDecimal(cursor, g_HostProfileOther); cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, buffer, cursor);
    for (rank = 0; rank < 12; ++rank)
    {
        DWORD index;
        DWORD best = 0;
        DWORD bestIndex = 0;
        for (index = 0; index < 0x8000; ++index) if (g_HostProfileSegment[index] > best)
        {
            best = g_HostProfileSegment[index];
            bestIndex = index;
        }
        if (!best)
            break;
        cursor = LogPut(buffer, "  HOSTPROF other_user 64K@0x"); cursor = LogHex(cursor, bestIndex << WORD_SHIFT);
        cursor = LogPut(cursor, " n="); cursor = LogDecimal(cursor, best); cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, buffer, cursor);
        g_HostProfileSegment[bestIndex] = 0;
    }
    for (rank = 0; rank < 400; ++rank)
    {
        DWORD index;
        DWORD best = 0;
        DWORD bestIndex = 0;
        for (index = 0; index < g_HostProfileCount; ++index) if (g_HostProfile[index] > best)
        {
            best = g_HostProfile[index];
            bestIndex = index;
        }
        if (!best)
            break;
        cursor = LogPut(buffer, "  HOSTPROF rva=0x"); cursor = LogHex(cursor, bestIndex << HPROF_SHIFT);
        cursor = LogPut(cursor, " n="); cursor = LogDecimal(cursor, best); cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, buffer, cursor);
        g_HostProfile[bestIndex] = 0;
    }
}

static INT32 HostInterp(volatile BYTE *tib, INT32 cap)
{
    V86_CPU cpu;
    INT32 iters;
    INT modeY;

    /* THE FULL 32-BIT REGISTERS, IN AND OUT. (s80) (Importance = 2):
     * This loaded and stored 16 bits, so the high half of every register was ZEROED
     * on the way in and DROPPED on the way out. Pure 16-bit code cannot tell; a 386
     * real-mode program can. Measured on Wolf3D under mode-Y interpretation: its
     * FixedByFrac runs `mov eax,[bp+6]` (interpreted, 0x66) then `cdq / idiv dword`
     * (declined -> the real CPU) -- which divided a truncated EAX, overflowed, took
     * INT 0, and the IRET chain ended at 0000:0078 with the game dead. The
     * interpreter's own 8/16-bit writes (V86Set8/V86Set16) preserve the high halves, so
     * carrying all 32 bits is transparent to 16-bit code and correct for 386 code.
     * ESP keeps its own high word: V86 addresses the stack through SP only.
     */
    cpu.Registers[X86_REG_AX] = VDM_REG(tib, VTIB_EAX);
    cpu.Registers[X86_REG_CX] = VDM_REG(tib, VTIB_ECX);
    cpu.Registers[X86_REG_DX] = VDM_REG(tib, VTIB_EDX);
    cpu.Registers[X86_REG_BX] = VDM_REG(tib, VTIB_EBX);
    cpu.Registers[X86_REG_SP] = (WORD)VDM_REG(tib, VTIB_ESP);
    cpu.Registers[X86_REG_BP] = VDM_REG(tib, VTIB_EBP);
    cpu.Registers[X86_REG_SI] = VDM_REG(tib, VTIB_ESI);
    cpu.Registers[X86_REG_DI] = VDM_REG(tib, VTIB_EDI);
    cpu.Segments[X86_SREG_ES] = (WORD)VDM_REG(tib, VTIB_ES);
    cpu.Segments[X86_SREG_CS] = (WORD)VDM_REG(tib, VTIB_CS);
    cpu.Segments[X86_SREG_SS] = (WORD)VDM_REG(tib, VTIB_SS);
    cpu.Segments[X86_SREG_DS] = (WORD)VDM_REG(tib, VTIB_DS);
    cpu.Segments[X86_SREG_FS] = (WORD)VDM_REG(tib, VTIB_FS);
    cpu.Segments[X86_SREG_GS] = (WORD)VDM_REG(tib, VTIB_GS);
    cpu.Ip = (WORD)VDM_REG(tib, VTIB_EIP);
    cpu.Flags = VDM_REG(tib, VTIB_EFLAGS);
    /* Mode Y (s80): the guest's interrupt flag is IF OR VIF -- a native STI under VME sets
     * only VIF -- so give the interpreter the same answer the gate uses. See the
     * write-back, which carries the interpreted flag back into both.
     */
    if (g_ModeYInterp && (cpu.Flags & EFLAGS_VIF))
        cpu.Flags |= EFLAGS_IF_U;

    HOST_LOCK();
    g_InterpreterCpu = &cpu;
    /* Hoisted: a global read after every istep() call is a reload the compiler cannot
     * drop, and it cost the mode-12h path ~6% of its throughput (Lemmings, two
     * interleaved A/B pairs: 1.044G vs 1.108G instructions in the same 54 s).
     */
    modeY = g_ModeYInterp;
    if (!modeY)
    {
        /* THE MODE-12h LOOP, EXACTLY AS IT WAS. Mode Y's checks live in the copy below:
         * two extra tests per instruction cost Lemmings ~2% of its interpreted
         * throughput (interleaved A/B against the confirmed build), and that path is
         * user-confirmed as it stands.
         */
        for (iters = 0; iters < cap; ++iters)
        {
            if (!V86Step(&cpu))
                break;
            /* YIELD WHEN AN INTERRUPT IS PENDING. A real CPU takes interrupts in the
             * middle of a loop; the interpreter is standing in for that CPU and must
             * do the same, or a guest whose loop can only END when an interrupt
             * fires runs here forever.
             * This is not hypothetical -- it is why mode 12h never worked. BLIT's
             * outer loop is `DO WHILE INKEY$ = ""`, and QuickBASIC polls for the key
             * in memory. Escalated to the interpreter, that loop burned the whole
             * 2,000,000-iteration cap with no way for a keystroke or a tick to ever
             * reach it, returned, re-faulted, re-escalated: TEN I/O events in thirty
             * seconds and a frozen screen, while the same program with the A0000
             * trap disabled produced 22.5 MILLION.
             * Checked every 256 instructions so the cost is negligible against the
             * fill loops this batching exists to accelerate.
             */
            if ((iters & BYTE_MASK) == 0xFF)
            {
                INT irq;
                INT pend = (g_Irq0Pending != 0);
                for (irq = 0; !pend && irq < ARRAYSIZE(g_IrqNPending); ++irq)
                    pend = (g_IrqNPending[irq] != 0);
                if (pend)
                {
                    ++iters;
                    break;
                }
            }
        }
    }
    else
    {
        /* MODE Y (north star 1). Same loop, plus: the optional instruction ring
         * (MYRING_FLAG), a stop the moment the guest lands in the vector table, an IRQ
         * yield only when the guest's IF would let it be taken, and a hand-back when
         * the multi-plane / latch window closes.
         */
        const INT ring = g_ModeYRingOn;
        for (iters = 0; iters < cap; ++iters)
        {
            if (ring)
            {
                UINT index = g_ModeYRingPosition++ % MY_RING;
                UINT byteIndex;
                const volatile BYTE *codeBytes = (const volatile BYTE *)(((UINT32)cpu.Segments[X86_SREG_CS] << PARAGRAPH_SHIFT) + cpu.Ip);
                g_ModeYRing[index].Cs = cpu.Segments[X86_SREG_CS];
                g_ModeYRing[index].Ip = cpu.Ip;
                g_ModeYRing[index].Ss = cpu.Segments[X86_SREG_SS];
                g_ModeYRing[index].Sp = (WORD)cpu.Registers[X86_REG_SP];
                for (byteIndex = 0; byteIndex < ARRAYSIZE(g_ModeYRing[index].Bytes); ++byteIndex)
                    g_ModeYRing[index].Bytes[byteIndex] = codeBytes[byteIndex];
            }
            if (!V86Step(&cpu))
                break;
            if (cpu.Segments[X86_SREG_CS] == 0 && cpu.Ip < IVT_SIZE)
            {
                ++iters;
                ModeYRingDump("interpreter reached CS=0");
                break;
            }
            if ((iters & BYTE_MASK) == 0xFF)
            {
                INT irq;
                INT pend = (g_Irq0Pending != 0);
                for (irq = 0; !pend && irq < ARRAYSIZE(g_IrqNPending); ++irq)
                    pend = (g_IrqNPending[irq] != 0);
                /* Only when the guest could TAKE it: yielding inside a CLI region hands
                 * the loop a chance to inject there (see the write-back below).
                 */
                if (pend && (cpu.Flags & EFLAGS_IF))
                {
                    ++iters;
                    break;
                }
                /* The window has closed (single-plane mask / write mode 0 again): hand
                 * the CPU back. Staying would still be CORRECT, only slower.
                 */
                if (!ModeYNeedsInterp())
                {
                    ++iters;
                    break;
                }
            }
        }
    }
    g_InterpreterCpu = NULL;
    HOST_UNLOCK();

    if (iters == 0)
        return 0;                                      /* first opcode unmodeled */

    VDM_REG(tib, VTIB_EAX) = cpu.Registers[X86_REG_AX];
    VDM_REG(tib, VTIB_ECX) = cpu.Registers[X86_REG_CX];
    VDM_REG(tib, VTIB_EDX) = cpu.Registers[X86_REG_DX];
    VDM_REG(tib, VTIB_EBX) = cpu.Registers[X86_REG_BX];
    VDM_SET16(tib, VTIB_ESP, cpu.Registers[X86_REG_SP]);
    VDM_REG(tib, VTIB_EBP) = cpu.Registers[X86_REG_BP];
    VDM_REG(tib, VTIB_ESI) = cpu.Registers[X86_REG_SI];
    VDM_REG(tib, VTIB_EDI) = cpu.Registers[X86_REG_DI];
    VDM_SET16(tib, VTIB_EIP, cpu.Ip);
    /* SEGMENTS TOO. They were loaded but never stored, so every segment load the
     * interpreter modelled (POP ES / MOV DS,AX / far CALL / INT / IRET) was thrown
     * away the moment we returned to V86 -- the guest carried on with the SEGMENT
     * it had before the batch and the OFFSET the batch had reached. Harmless while
     * batching was confined to a fill loop that never reloads a segment; fatal for
     * continuous interpretation, where CS changes on every interrupt.
     */
    VDM_REG(tib, VTIB_ES) = cpu.Segments[X86_SREG_ES];
    VDM_REG(tib, VTIB_CS) = cpu.Segments[X86_SREG_CS];
    VDM_REG(tib, VTIB_SS) = cpu.Segments[X86_SREG_SS];
    VDM_REG(tib, VTIB_DS) = cpu.Segments[X86_SREG_DS];
    VDM_REG(tib, VTIB_FS) = cpu.Segments[X86_SREG_FS];
    VDM_REG(tib, VTIB_GS) = cpu.Segments[X86_SREG_GS];
    /* update only the low 16 flag bits (arith + DF); keep VM/IOPL/IF etc. */
    VDM_REG(tib, VTIB_EFLAGS) = (VDM_REG(tib, VTIB_EFLAGS) & HIGH_WORD_MASK_U) | (cpu.Flags & WORD_MASK_U);
    /* -- AND VIF WITH IT, FOR MODE Y. (s80) The loop's gate delivers when IF *or*
     * VIF is set (GuestIfEnabled), because under VME a native STI sets VIF. An
     * interpreted CLI clears only IF here -- VIF stayed set, the gate saw "enabled",
     * and an IRQ was injected into a region the guest had closed: Wolf3D's ISR tail
     * (`pop ax / pop ds / iret`) ran on a frame that was not its own and IRET'd to
     * 0000:0078. Keep the two in step so the interpreter's answer is the answer.
     *
     * [CAUTION]: Mode Y only: the mode-12h path (Lemmings) is user-confirmed as it stands.
     */
    if (g_ModeYInterp)
    {
        if (cpu.Flags & EFLAGS_IF)
            VDM_REG(tib, VTIB_EFLAGS) |=  EFLAGS_VIF;
        else
            VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_VIF;
    }
    return iters;
}

/* APPROXIMATE CPU SPEED: THE INTERPRETER HALF. (GH #56) (Importance = 1):
 * Here we know exactly how much work was done, so the throttle is precise rather
 * than statistical: time the slice, charge the instructions against the budget the
 * setting allows, and sleep the difference. cpuspeed.h carries the arithmetic and
 * the reason the microsecond remainder has to be kept.
 *
 * [CAUTION]: EVERY HostInterp CALL GOES THROUGH HERE, including the single-instruction one.
 * A one-instruction slice owes a fraction of a microsecond, which is exactly the
 * debt that must accumulate rather than round to nothing -- and A0000 stores in
 * mode 12h arrive one at a time in their thousands, so it is not a rounding
 * detail, it is most of the guest's execution in that mode.
 *
 * [CAUTION]: SLEEPING HERE IS SAFE AND SLEEPING INSIDE HostInterp WOULD NOT BE: this is
 * outside the HOST_LOCK, so a throttled guest does not hold the bus lock while it
 * waits and the audio pump is untouched.
 */
INT32 HostInterpPaced(volatile BYTE *tib, INT32 cap)
{
    static CPUSPEED_PACE pace;              /* exec thread only -- no lock needed */
    DWORD instructionsPerSecond = CpuSpeedInstructionsPerSecond((UINT)g_CpuSpeedIndex);
    LARGE_INTEGER before;
    LARGE_INTEGER after;
    INT32 ran;
    INT milliseconds;

    if (!instructionsPerSecond)
        return HostInterp(tib, cap);                                   /* Unlimited: not one branch */
    QueryPerformanceCounter(&before);
    ran = HostInterp(tib, cap);
    QueryPerformanceCounter(&after);
    if (ran <= 0)
        return ran;
    milliseconds = CpuSpeedCharge(&pace, (DWORD)ran, instructionsPerSecond,
                         (INT64)QpcMicroseconds(after.QuadPart - before.QuadPart));
    if (milliseconds > 0)
    {
        g_CpuSpeedHeldMs += (DWORD)milliseconds;
        Sleep((DWORD)milliseconds);
    }
    return ran;
}

/* -- THE 16-BIT INTERPRETER RUNNING PROTECTED-MODE CODE (run 53). It drives v86interp.h with
 * LDT bases and LAR/LSL answers from the DPMI host, so it lives with the interpreter (#335).
 */
#define DPMI_INTERP_STEPS_MAX   20000000L   /* DpmiRunPmInterp: modelled steps before giving up */
static UINT32 DpmiSegmentToLinear(WORD selector)
{
    return DpmiSelectorBase(selector);
}

static VOID DpmiInterpreterCpuLoad(V86_CPU *cpu, volatile BYTE *tib)
{
    cpu->Registers[X86_REG_AX]=(WORD)VDM_REG(tib,VTIB_EAX);
    cpu->Registers[X86_REG_CX]=(WORD)VDM_REG(tib,VTIB_ECX);
    cpu->Registers[X86_REG_DX]=(WORD)VDM_REG(tib,VTIB_EDX);
    cpu->Registers[X86_REG_BX]=(WORD)VDM_REG(tib,VTIB_EBX);
    cpu->Registers[X86_REG_SP]=(WORD)VDM_REG(tib,VTIB_ESP);
    cpu->Registers[X86_REG_BP]=(WORD)VDM_REG(tib,VTIB_EBP);
    cpu->Registers[X86_REG_SI]=(WORD)VDM_REG(tib,VTIB_ESI);
    cpu->Registers[X86_REG_DI]=(WORD)VDM_REG(tib,VTIB_EDI);
    cpu->Segments[X86_SREG_ES]=(WORD)VDM_REG(tib,VTIB_ES);
    cpu->Segments[X86_SREG_CS]=(WORD)VDM_REG(tib,VTIB_CS);
    cpu->Segments[X86_SREG_SS]=(WORD)VDM_REG(tib,VTIB_SS);
    cpu->Segments[X86_SREG_DS]=(WORD)VDM_REG(tib,VTIB_DS);
    cpu->Segments[X86_SREG_FS]=(WORD)VDM_REG(tib,VTIB_FS);
    cpu->Segments[X86_SREG_GS]=(WORD)VDM_REG(tib,VTIB_GS);
    cpu->Ip=(WORD)VDM_REG(tib,VTIB_EIP);
    cpu->Flags=VDM_REG(tib,VTIB_EFLAGS);
}

static VOID DpmiInterpreterCpuStore(V86_CPU *cpu, volatile BYTE *tib)
{
    VDM_SET16(tib,VTIB_EAX,cpu->Registers[X86_REG_AX]);
    VDM_SET16(tib,VTIB_ECX,cpu->Registers[X86_REG_CX]);
    VDM_SET16(tib,VTIB_EDX,cpu->Registers[X86_REG_DX]);
    VDM_SET16(tib,VTIB_EBX,cpu->Registers[X86_REG_BX]);
    VDM_SET16(tib,VTIB_ESP,cpu->Registers[X86_REG_SP]);
    VDM_SET16(tib,VTIB_EBP,cpu->Registers[X86_REG_BP]);
    VDM_SET16(tib,VTIB_ESI,cpu->Registers[X86_REG_SI]);
    VDM_SET16(tib,VTIB_EDI,cpu->Registers[X86_REG_DI]);
    VDM_SET16(tib,VTIB_ES,cpu->Segments[X86_SREG_ES]);
    VDM_SET16(tib,VTIB_CS,cpu->Segments[X86_SREG_CS]);
    VDM_SET16(tib,VTIB_SS,cpu->Segments[X86_SREG_SS]);
    VDM_SET16(tib,VTIB_DS,cpu->Segments[X86_SREG_DS]);
    VDM_SET16(tib,VTIB_FS,cpu->Segments[X86_SREG_FS]);
    VDM_SET16(tib,VTIB_GS,cpu->Segments[X86_SREG_GS]);
    VDM_SET16(tib,VTIB_EIP,cpu->Ip);
    VDM_REG(tib,VTIB_EFLAGS) = (VDM_REG(tib,VTIB_EFLAGS) & HIGH_WORD_MASK_U) | (cpu->Flags & WORD_MASK_U);
}

/* Returns 0 = client exited (INT 21h AH=4Ch), -1 = stopped on an unmodeled/
 * unserviceable opcode (already logged). Never touches the kernel PM path.
 */
INT DpmiRunPmInterp(DOS_MACHINE *machine, volatile BYTE *tib)
{
    V86_CPU cpu;
    INT32 guard = 0;
    CHAR lineBuffer[256];
    PSTR lineCursor;

    g_V86SegmentToLinear = DpmiSegmentToLinear;                 /* interpreter now resolves LDT bases */
    g_V86SelectorDescriptor = DpmiSelectorDescriptor;               /* ...and answers LAR/LSL from g_Ldt[] (run 55) */
    DpmiInterpreterCpuLoad(&cpu, tib);
    lineCursor = lineBuffer;
    lineCursor = LogPut(lineCursor, "DPMI-INTERP: run 53 host PM begins CS:IP=0x"); lineCursor = LogHex(lineCursor, cpu.Segments[X86_SREG_CS]);
    lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, cpu.Ip);
    lineCursor = LogPut(lineCursor, " DS=0x"); lineCursor = LogHex(lineCursor, cpu.Segments[X86_SREG_DS]); lineCursor = LogPut(lineCursor, " SS=0x"); lineCursor = LogHex(lineCursor, cpu.Segments[X86_SREG_SS]);
    lineCursor = LogPut(lineCursor, "\r\n"); LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor);
    for (;;)
    {
        if (V86Step(&cpu)) /* modeled step */
        {
            if (++guard > DPMI_INTERP_STEPS_MAX)
                break;
            continue;
        }
        { UINT32 site = V86SegmentBase(cpu.Segments[X86_SREG_CS]) + cpu.Ip;
          BYTE opcode = V86HostRead8(site);
          BYTE nextByte = V86HostRead8(site+1);
          if (opcode == X86_OP_INT)                     /* INT nn -> shared DPMI/DOS dispatch */
          {
              INT status;
              DpmiInterpreterCpuStore(&cpu, tib);
              VDM_REG(tib, VTIB_EVENT) = VDM_EVENT_BOP;   /* mimic a serviceable BOP for the dispatcher */
              status = DpmiServicePmInt(machine, tib, (DWORD)nextByte, (UINT)guard);
              if (status <= 0)
                  return status;                      /* 0 = 4Ch exit, -1 = unserviceable */
              DpmiInterpreterCpuLoad(&cpu, tib);        /* pick up results + any selector/mode change */
              continue;
          }
          lineCursor = lineBuffer;                             /* the spike's to-do signal */
          lineCursor = LogPut(lineCursor, "DPMI-INTERP: unmodeled opcode at CS:IP=0x"); lineCursor = LogHex(lineCursor, cpu.Segments[X86_SREG_CS]);
          lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, cpu.Ip);
          lineCursor = LogPut(lineCursor, " bytes="); lineCursor = LogDump(lineCursor, (const BYTE*)(ULONG_PTR)site, 8);
          lineCursor = LogPut(lineCursor, " (steps=0x"); lineCursor = LogHex(lineCursor, (UINT)guard); lineCursor = LogPut(lineCursor, ")\r\n");
          LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor);
          DpmiInterpreterCpuStore(&cpu, tib);
          return -1;
        }
    }
    DpmiInterpreterCpuStore(&cpu, tib);                 /* guard cap hit (possible infinite loop) */
    lineCursor = lineBuffer; lineCursor = LogPut(lineCursor, "DPMI-INTERP: guard cap hit (spin?)\r\n");
    LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor);
    return -1;
}
