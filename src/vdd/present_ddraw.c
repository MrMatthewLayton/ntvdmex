/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * See present_ddraw.h.  WINDOWED mode presents via GDI
 * StretchDIBits (no cursor flicker, repaints correctly on expose, blits above
 * the status bar); EXCLUSIVE FULLSCREEN uses DirectDraw 7.  Pure C, no-CRT
 * (ddraw bound via GetProcAddress, IID inline).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <ddraw.h>
#include "present_ddraw.h"
#include "present_scale.h"

/* Timing the blit near the vertical blank (PresentWaitVerticalBlank). */
#define PRESENT_MODULE_DDRAW                    "ddraw.dll"
#define PRESENT_EXPORT_DIRECT_DRAW_CREATE_EX    "DirectDrawCreateEx"
#define PRESENT_MONITOR_HZ_MIN                  40
#define PRESENT_ROP_PATAND                      0x00A000C9L     /* DPa: destination AND pattern (no windows.h name) */
#define PRESENT_MONITOR_HZ_MAX                  240
#define PRESENT_MONITOR_HZ_DEFAULT              60              /* When the driver will not say */
#define PRESENT_FRAME_LINES_NUMERATOR           21u             /* A frame is ~21/20 of the visible lines */
#define PRESENT_FRAME_LINES_DENOMINATOR         20u
#define PRESENT_VBLANK_SPIN_US                  1200            /* Sleep to here, spin the rest */
#define PRESENT_VBLANK_SPIN_MAX                 3000
#define PRESENT_VBLANK_WRAP_LINES               4               /* Just after the blank */

/* Depths. */
#define PRESENT_BPP_INDEXED                     8
#define PRESENT_BPP_DIRECT                      32
#define PRESENT_ARGB_BYTES                      4
#define PRESENT_RGB_MASK                        0x00FFFFFFu
#define PRESENT_SURFACE_BPP_15                  15
#define PRESENT_SURFACE_BPP_16                  16
#define PRESENT_SURFACE_BPP_24                  24
#define PRESENT_SURFACE_BPP_32                  32
#define PRESENT_CHANNEL_BITS                    8
#define PRESENT_BYTES_PER_PIXEL_24              3
#define PRESENT_GREY_RED                        30              /* An 8-bit surface: grey, percent */
#define PRESENT_GREY_GREEN                      59
#define PRESENT_GREY_BLUE                       11

/* The scanline mask and the hint. */
#define PRESENT_SCANLINE_PATTERN_SIZE           8               /* An 8x8 monochrome pattern */
#define PRESENT_HINT_TOP                        12
#define PRESENT_HINT_PAD_X                      10
#define PRESENT_HINT_PAD_Y                      5
#define PRESENT_HINT_TEXT_COLOUR                RGB(255, 255, 255)

/* Exclusive fullscreen. */
#define PRESENT_DISPLAY_BPP                     32
#define PRESENT_DISPLAY_BPP_FALLBACK            16
#define PRESENT_FALLBACK_WIDTH                  640 /* The old forced mode, and the smallest staging surface */
#define PRESENT_FALLBACK_HEIGHT                 480
#define PRESENT_BACK_BUFFERS                    2               /* Triple-buffered */
#define PRESENT_TRIPLE_BUFFERS                  3
#define PRESENT_MID_SCREEN_MARGIN               32              /* Lines from either edge */
#define PRESENT_FLIP_STREAK_OUR_WAIT            30              /* Flips that never waited, then we time them */

/* The .bmp snapshot (PresentDdrawSaveBmp). */

/* IID_IDirectDraw7 = 15e65ec0-3b9c-11d2-b92f-00609797ea5b (inline -> no dxguid). */
static const GUID g_PresentIidDirectDraw7 =
    { 0x15e65ec0, 0x3b9c, 0x11d2, { 0xb9,0x2f,0x00,0x60,0x97,0x97,0xea,0x5b } };
typedef HRESULT (WINAPI *PFN_DIRECT_DRAW_CREATE_EX)(GUID *, LPVOID *, REFIID, IUnknown *);

#define PRESENT_DIRECT_DRAW     ((LPDIRECTDRAW7)presenter->DirectDraw)
#define PRESENT_SURFACE(p)      ((LPDIRECTDRAWSURFACE7)(p))

/* time a blit near the monitor's vertical blank (reduces tearing).
 *
 * [CAUTION]: NOT WaitForVerticalBlank. On XP that call is a BUSY LOOP on the scanline register,
 * and once presents were raised by the guest's frame (s73 Auto) it ran ~60 times a
 * second for up to a frame each -- a whole core, on a two-core box, and the guest's
 * own thread lost it: BOUNCEBX polls stalled 13.5 ms (dtmax was 2.4) and it dropped
 * one frame in twenty.
 * - And not "Sleep(1) and look again" either: the blank is ~1.4 ms wide at 60 Hz and a
 *   1 ms sleep sailed past it half the time, then waited a WHOLE extra frame -- 1002
 *   presents for 1788 guest frames. So compute where the beam is, SLEEP to just short
 *   of the blank, and spin only the last millisecond. The monitor's line count and
 *   refresh are read once (GetDisplayMode / GetMonitorFrequency) and fall back to
 *   60 Hz if the driver will not say. Bounded: at most ~2 frames however it goes.
 */
static VOID PresentMonitorQuery(PPRESENT_DDRAW presenter)
{
    DWORD hz;
    if (presenter->MonitorLines || !presenter->DirectDraw) return;
    {   DDSURFACEDESC2 description; ZeroMemory(&description, sizeof description); description.dwSize = sizeof description;
        presenter->MonitorLines = (SUCCEEDED(IDirectDraw7_GetDisplayMode(PRESENT_DIRECT_DRAW, &description)) && description.dwHeight) ? (INT)description.dwHeight : 0;
        presenter->MonitorHz = (SUCCEEDED(IDirectDraw7_GetMonitorFrequency(PRESENT_DIRECT_DRAW, &hz)) && hz >= PRESENT_MONITOR_HZ_MIN && hz <= PRESENT_MONITOR_HZ_MAX) ? (INT)hz : PRESENT_MONITOR_HZ_DEFAULT;
        if (!presenter->MonitorLines) presenter->MonitorLines = -1; }             /* asked once; unknown: spin-free fallback below */
}

static VOID PresentWaitVerticalBlank(PPRESENT_DDRAW presenter)
{
    DWORD scanLine = 0, height, periodUs, spin;
    HRESULT result;
    if (!presenter->IsVsync || !presenter->DirectDraw) return;
    PresentMonitorQuery(presenter);
    height = presenter->MonitorLines > 0 ? (DWORD)presenter->MonitorLines : 0;
    periodUs = MICROSECONDS_PER_SECOND_U / (DWORD)(presenter->MonitorHz ? presenter->MonitorHz : PRESENT_MONITOR_HZ_DEFAULT);
    result = IDirectDraw7_GetScanLine(PRESENT_DIRECT_DRAW, &scanLine);
    if (result == DDERR_VERTICALBLANKINPROGRESS) return;  /* already in the blank: go */
    if (result != DD_OK) return;                          /* cannot tell: do not wait */
    if (height && scanLine < height)
    {
        /* lines to go, as time; the blank starts at `height` (the CRTC counts on
         * through it), so sleep for all of it but the last ~1.2 ms
         */
        DWORD sleepUs = (DWORD)((UINT64)(height - scanLine) * periodUs / (height * PRESENT_FRAME_LINES_NUMERATOR / PRESENT_FRAME_LINES_DENOMINATOR));
        if (sleepUs > PRESENT_VBLANK_SPIN_US) Sleep((sleepUs - PRESENT_VBLANK_SPIN_US) / MICROSECONDS_PER_MILLISECOND);
    }
    for (spin = 0; spin < PRESENT_VBLANK_SPIN_MAX; ++spin)                   /* the last stretch: ~1-2 ms */
    {
        result = IDirectDraw7_GetScanLine(PRESENT_DIRECT_DRAW, &scanLine);
        if (result != DD_OK) return;                      /* VERTICALBLANKINPROGRESS = go */
        if (height && scanLine < PRESENT_VBLANK_WRAP_LINES) return;               /* wrapped: just after the blank */
    }
}

/* THE SCALE2X TARGET, AS ONE STATIC BUFFER:
 * The largest frame doubled (#325: it was 1280x960 -- 640x480 doubled -- so a
 * 720x400 text frame silently skipped the scaler). It lives here rather than in PRESENT_DDRAW so the
 * struct stays something a caller can hold by value (present_demo does), and it
 * is static rather than allocated because the present path runs on the UI thread
 * at video rate and must never wait on the heap.
 */
static BYTE g_PresentScaled[(PRESENT_SCALE2X_FACTOR * NTVDD_FRAME_MAX_WIDTH) * (PRESENT_SCALE2X_FACTOR * NTVDD_FRAME_MAX_HEIGHT)];

/* The scanline mask: an 8x8 monochrome pattern, black on every other row. ANDed
 * over the destination it darkens alternate PHYSICAL rows -- which is where a
 * scanline lives, on the screen, not in the frame. Built once and kept: a brush
 * per present would be a GDI object churned sixty times a second.
 */
static HBRUSH PresentScanlineBrush(VOID)
{
    static HBRUSH brush = NULL;
    static const WORD rows[PRESENT_SCANLINE_PATTERN_SIZE] = { 0xFFFF, 0x0000, 0xFFFF, 0x0000,
                                  0xFFFF, 0x0000, 0xFFFF, 0x0000 };
    HBITMAP bitmap;
    if (brush) return brush;
    bitmap = CreateBitmap(PRESENT_SCANLINE_PATTERN_SIZE, PRESENT_SCANLINE_PATTERN_SIZE, 1, 1, rows);
    if (!bitmap) return NULL;
    brush = CreatePatternBrush(bitmap);
    DeleteObject(bitmap);
    return brush;
}

/* ---- windowed AND (by default) fullscreen: GDI StretchDIBits ---------------
 * THIS PATH IS SHARP AND THE DIRECTDRAW ONE IS NOT. (s64) (Importance = 3):
 * The user settled it with a comparison no log could have produced: "even if I
 * maximize the window on the desktop, with or without aspect ratio, the pixels stay
 * sharp. In fullscreen they are NEVER sharp, regardless of what knobs I twiddle."
 * Same frame, same aspect maths, same monitor, same non-integer 5.25x scale -- the
 * only difference is WHICH BLITTER DRAWS IT:
 *   GDI StretchDIBits + COLORONCOLOR -> point sampling. Hard pixel edges.
 *   DirectDraw stretch Blt           -> the DRIVER decides, and it filters.
 * DirectDraw offers no reliable way to demand point sampling on a stretch blt
 * (DDBLTFX can ASK for arithmetic stretching; there is no flag that forbids it), so
 * the fix is not to configure the stretch but to STOP ASKING FOR ONE.
 * - So fullscreen is now a BORDERLESS WINDOW presented through here, not an exclusive
 *   DirectDraw mode. That also gets back the instant toggle (no mode switch to resync)
 *   and removes the fullscreen resolution knob entirely -- there is no mode to choose.
 *   The exclusive path is kept behind a file knob (ddrawfs.flag) rather than deleted,
 *   because "no tearing" was its original argument and that deserves a way back.
 *
 * [CAUTION]: TWO THINGS DIFFER IN FULLSCREEN and both are handled below: there is no status
 * strip to reserve room for, and the fit is snapped to whole multiples.
 */
static const UINT32 *PresentRowPalette(PPRESENT_DDRAW presenter, INT row);   /* fwd: with _snapshot */
static UINT32 PresentSnapshotPixel(PPRESENT_DDRAW presenter, INT row, INT column);   /* fwd: depth-agnostic pixel */
/* #138: the release hint, top centre of the picture, while it is due. Drawn after the
 * frame on BOTH fullscreen paths (the GDI window here, the DirectDraw back buffer via
 * GetDC in PresentFullscreen), so every present repaints it until it expires. s81: the user
 * saw it windowed and not in fullscreen -- fullscreen was on the DirectDraw path.
 */
static VOID PresentHintDraw(PPRESENT_DDRAW presenter, HDC dc, INT destinationX, INT destinationY, INT destinationWidth)
{
    SIZE textSize; INT length = 0, textX, textY = destinationY + PRESENT_HINT_TOP;
    if (presenter->IsOsdOff) return;                /* #217: Show on-screen messages off */
    if (!presenter->HintText || (LONG)(presenter->HintUntil - GetTickCount()) <= 0) return;
    while (presenter->HintText[length]) ++length;
    SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    GetTextExtentPoint32A(dc, presenter->HintText, length, &textSize);
    textX = destinationX + (destinationWidth - textSize.cx) / 2;
    { RECT box; box.left = textX - PRESENT_HINT_PAD_X; box.top = textY - PRESENT_HINT_PAD_Y;
      box.right = textX + textSize.cx + PRESENT_HINT_PAD_X; box.bottom = textY + textSize.cy + PRESENT_HINT_PAD_Y;
      FillRect(dc, &box, (HBRUSH)GetStockObject(BLACK_BRUSH)); }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, PRESENT_HINT_TEXT_COLOUR);
    TextOutA(dc, textX, textY, presenter->HintText, length);
}

/* -- #217: THE OFF-SCREEN PICTURE. A memory DC over a DIB section the size of the
 * picture area, recreated only when that size changes. NULL = draw to the window, as
 * before (the setting is off, or GDI could not give us one).
 */
static HDC PresentMemoryTarget(PPRESENT_DDRAW presenter, HDC windowDc, INT clientWidth, INT clientHeight)
{
    if (presenter->IsUnbuffered) return NULL;
    if (presenter->MemoryDc && (presenter->MemoryWidth != clientWidth || presenter->MemoryHeight != clientHeight))
    {
        SelectObject((HDC)presenter->MemoryDc, (HGDIOBJ)presenter->MemoryOld);
        DeleteObject((HGDIOBJ)presenter->MemoryBitmap); DeleteDC((HDC)presenter->MemoryDc);
        presenter->MemoryDc = presenter->MemoryBitmap = presenter->MemoryOld = 0;
    }
    if (!presenter->MemoryDc)
    {
        HDC memoryDc = CreateCompatibleDC(windowDc);
        HBITMAP bitmap = memoryDc ? CreateCompatibleBitmap(windowDc, clientWidth, clientHeight) : NULL;
        if (!bitmap)
        {
            if (memoryDc) DeleteDC(memoryDc);
            return NULL;
        }
        presenter->MemoryOld = SelectObject(memoryDc, bitmap);
        presenter->MemoryDc = memoryDc; presenter->MemoryBitmap = bitmap; presenter->MemoryWidth = clientWidth; presenter->MemoryHeight = clientHeight;
    }
    return (HDC)presenter->MemoryDc;
}

static VOID PresentMemoryRelease(PPRESENT_DDRAW presenter)
{
    if (!presenter->MemoryDc) return;
    SelectObject((HDC)presenter->MemoryDc, (HGDIOBJ)presenter->MemoryOld);
    DeleteObject((HGDIOBJ)presenter->MemoryBitmap); DeleteDC((HDC)presenter->MemoryDc);
    presenter->MemoryDc = presenter->MemoryBitmap = presenter->MemoryOld = 0; presenter->MemoryWidth = presenter->MemoryHeight = 0;
}

/* The snapshot as a DIB that StretchDIBits can take: 8bpp + palette, or 32bpp when
 * the frame is direct colour or raster-split. Shared by the window/borderless path and
 * (#223) the exclusive DirectDraw path, so both draw the same picture the same way.
 * `scale2x` lets the caller ask for the pixel-art doubler first (GDI's Scaler).
 */
typedef struct _PRESENT_SNAPSHOT_DIB
{
    BITMAPINFOHEADER Header;
    RGBQUAD Colors[NTVDD_PALETTE_ENTRIES];
} PRESENT_SNAPSHOT_DIB, *PPRESENT_SNAPSHOT_DIB;
static const BYTE *PresentSnapshotDib(PPRESENT_DDRAW presenter, PPRESENT_SNAPSHOT_DIB dib, INT *sourceWidthOut, INT *sourceHeightOut,
                               INT isScale2x)
{
    static UINT32 resolved32[NTVDD_FRAME_MAX_WIDTH * NTVDD_FRAME_MAX_HEIGHT];  /* a split frame resolved per row, or ARGB */
    const BYTE *pixels = presenter->Snapshot;
    INT sourceWidth = presenter->SnapshotWidth, sourceHeight = presenter->SnapshotHeight;
    UINT index;
    /* A direct-colour frame takes the same 32bpp DIB route a raster-split frame
     * does -- it is already ARGB, so it needs no resolving, just no palette.
     */
    INT isDirect = (presenter->SnapshotBpp == PRESENT_BPP_DIRECT);
    /* #325: no 640x480 cap -- the split palette is per ENTRY (from a row), so any frame
     * size resolves; s_rgb32 is already the largest frame.
     */
    INT isSplit = !isDirect && presenter->IsSnapshotSplit;
    /* Scale2x first: it is a property of the FRAME, so it happens before the
     * stretch and the stretch then works from a source with twice the detail.
     * (A split frame skips it; scale2x is an 8bpp pixel-art scaler and cannot read
     * Snapshot32 either.)
     */
    if (isScale2x && !isSplit && !isDirect && PresentScalerDoubles(presenter->Scaler) && sourceWidth > 0 && sourceHeight > 0
        && sourceWidth <= NTVDD_FRAME_MAX_WIDTH && sourceHeight <= NTVDD_FRAME_MAX_HEIGHT)
    {
        PresentScale2x8(presenter->Snapshot, sourceWidth, sourceHeight, sourceWidth, g_PresentScaled);
        pixels = g_PresentScaled; sourceWidth *= PRESENT_SCALE2X_FACTOR; sourceHeight *= PRESENT_SCALE2X_FACTOR;
    }
    ZeroMemory(dib, sizeof *dib);
    dib->Header.biSize = sizeof(BITMAPINFOHEADER);
    dib->Header.biWidth = (LONG)sourceWidth; dib->Header.biHeight = -(LONG)sourceHeight;                /* top-down */
    dib->Header.biPlanes = 1; dib->Header.biBitCount = PRESENT_BPP_INDEXED; dib->Header.biCompression = BI_RGB;
    for (index = 0; index < NTVDD_PALETTE_ENTRIES; ++index)
    {
        UINT32 argb = presenter->SnapshotPalette[index];
        dib->Colors[index].rgbRed = (BYTE)(argb >> PRESENT_RED_SHIFT); dib->Colors[index].rgbGreen = (BYTE)(argb >> PRESENT_GREEN_SHIFT);
        dib->Colors[index].rgbBlue = (BYTE)argb; dib->Colors[index].rgbReserved = 0;
    }
    if (isSplit || isDirect)           /* resolve to a 32bpp DIB */
    {
        INT row, column;
        for (row = 0; row < sourceHeight; ++row)
        {
            UINT32 *destinationRow = resolved32 + (size_t)row * sourceWidth;
            for (column = 0; column < sourceWidth; ++column) destinationRow[column] = PresentSnapshotPixel(presenter, row, column) & PRESENT_RGB_MASK;
        }
        dib->Header.biBitCount = PRESENT_BPP_DIRECT; pixels = (const BYTE *)resolved32;
    }
    *sourceWidthOut = sourceWidth; *sourceHeightOut = sourceHeight;
    return pixels;
}

/* -- #325: DRAW THE PICTURE WITH THE CHOSEN FILTER. A whole multiple of the source is
 * always point-sampled (every filter agrees there, and it is the default case: a
 * Native window, or a Whole-pixels fit). Otherwise Nearest point-samples -- uneven
 * rows -- Bilinear smooths everything (HALFTONE), and Sharp enlarges by the largest
 * whole multiple point-sampled into a scratch bitmap and smooths only the remainder,
 * so edges stay crisp and rows stay even. Shared by the GDI window and the
 * DirectDraw back buffer's DC. Returns non-zero when something was drawn.
 */
static INT PresentBlitPicture(HDC dc, INT destinationX, INT destinationY, INT destinationWidth, INT destinationHeight,
                        const BYTE *pixels, PPRESENT_SNAPSHOT_DIB dib, INT sourceWidth, INT sourceHeight, INT filter)
{
    static HDC scratchDc; static HBITMAP scratchBitmap, scratchOld; static INT scratchWidth, scratchHeight;
    INT scaleX, scaleY, integerWidth, integerHeight;
    if (sourceWidth < 1 || sourceHeight < 1) return 0;
    if ((destinationWidth % sourceWidth == 0 && destinationHeight % sourceHeight == 0) || filter == PRESENT_FILTER_NEAREST)
    {
        SetStretchBltMode(dc, COLORONCOLOR);
        return StretchDIBits(dc, destinationX, destinationY, destinationWidth, destinationHeight, 0, 0, sourceWidth, sourceHeight, pixels, (BITMAPINFO *)dib,
                             DIB_RGB_COLORS, SRCCOPY) > 0;
    }
    if (filter == PRESENT_FILTER_BILINEAR)
    {
        SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, NULL);
        return StretchDIBits(dc, destinationX, destinationY, destinationWidth, destinationHeight, 0, 0, sourceWidth, sourceHeight, pixels, (BITMAPINFO *)dib,
                             DIB_RGB_COLORS, SRCCOPY) > 0;
    }
    scaleX = destinationWidth / sourceWidth; scaleY = destinationHeight / sourceHeight;
    if (scaleX < 1) scaleX = 1;
    if (scaleY < 1) scaleY = 1;
    integerWidth = sourceWidth * scaleX; integerHeight = sourceHeight * scaleY;
    if (!scratchDc) scratchDc = CreateCompatibleDC(dc);
    if (!scratchDc) return 0;
    if (!scratchBitmap || scratchWidth != integerWidth || scratchHeight != integerHeight)
    {
        if (scratchBitmap)
        {
            SelectObject(scratchDc, scratchOld);
            DeleteObject(scratchBitmap);
            scratchBitmap = NULL;
        }
        scratchBitmap = CreateCompatibleBitmap(dc, integerWidth, integerHeight);
        if (!scratchBitmap)
        {
            scratchWidth = scratchHeight = 0;
            return 0;
        }
        scratchOld = (HBITMAP)SelectObject(scratchDc, scratchBitmap);
        scratchWidth = integerWidth; scratchHeight = integerHeight;
    }
    SetStretchBltMode(scratchDc, COLORONCOLOR);
    StretchDIBits(scratchDc, 0, 0, integerWidth, integerHeight, 0, 0, sourceWidth, sourceHeight, pixels, (BITMAPINFO *)dib, DIB_RGB_COLORS, SRCCOPY);
    SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, NULL);
    return StretchBlt(dc, destinationX, destinationY, destinationWidth, destinationHeight, scratchDc, 0, 0, integerWidth, integerHeight, SRCCOPY);
}

static VOID PresentGdi(PPRESENT_DDRAW presenter)
{
    HDC dc, windowDc, memoryDc; RECT clientRect; INT clientWidth, clientHeight, destinationX, destinationY, destinationWidth, destinationHeight;
    const BYTE *pixels;
    INT sourceWidth, sourceHeight;
    PRESENT_SNAPSHOT_DIB dib;
    windowDc = GetDC(presenter->Window);
    if (!windowDc) return;
    GetClientRect(presenter->Window, &clientRect);
    clientWidth = clientRect.right;
    clientHeight = clientRect.bottom - (presenter->IsFullscreen ? 0
                                     : (presenter->StatusHeight ? presenter->StatusHeight : PRESENT_STATUS_HEIGHT));
    if (clientWidth < 1) clientWidth = 1;
    if (clientHeight < 1) clientHeight = 1;
    /* #217: everything below draws into `hdc` -- the off-screen picture when buffered,
     * the window otherwise -- and a buffered picture reaches the window in ONE blit.
     */
    memoryDc = PresentMemoryTarget(presenter, windowDc, clientWidth, clientHeight);
    dc = memoryDc ? memoryDc : windowDc;

    pixels = PresentSnapshotDib(presenter, &dib, &sourceWidth, &sourceHeight, PRESENT_SNAPSHOT_2X);
    /* [CAUTION]: THE INTEGER FIT USES sw/sh, WHICH ARE POST-SCALE2X. That is deliberate: the
     * blit source is what has to divide into the destination. Snapping to a multiple
     * of the ORIGINAL 320 while blitting a 640-wide scale2x source would give 2.5x
     * and put the uneven pixels straight back.
     */
    {   /* #325: one layout for every path, from the FRAME's size (not the post-Scale2x
             source): a window is sized to the picture, maximised/fullscreen fit by the
             Fit setting. */
        INT isScreen = presenter->IsFullscreen || IsZoomed(presenter->Window);
        PresentLayout(presenter->Aspect, isScreen ? presenter->Fit : PRESENT_FIT_WHOLE, isScreen, clientWidth, clientHeight,
                       presenter->SnapshotWidth, presenter->SnapshotHeight, &destinationX, &destinationY, &destinationWidth, &destinationHeight); }
    if (!memoryDc) PresentWaitVerticalBlank(presenter);                      /* buffered: waits before its one blit */
    /* Letterboxing leaves bars, and they must be PAINTED: the client area is ours
     * (WM_ERASEBKGND returns 1), so whatever was there last -- the previous mode's
     * frame, at the previous size -- would otherwise stay on screen forever.
     */
    if (destinationX || destinationY)
    {
        RECT fullRect; fullRect.left = 0; fullRect.top = 0; fullRect.right = clientWidth; fullRect.bottom = clientHeight;
        FillRect(dc, &fullRect, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    PresentBlitPicture(dc, destinationX, destinationY, destinationWidth, destinationHeight, pixels, &dib, sourceWidth, sourceHeight, presenter->Filter);   /* #325 */
    if (PresentScalerHasScanlines(presenter->Scaler))
    {
        HBRUSH brush = PresentScanlineBrush();
        if (brush)
        {
            HGDIOBJ oldObject = SelectObject(dc, brush);
            PatBlt(dc, destinationX, destinationY, destinationWidth, destinationHeight, PRESENT_ROP_PATAND);    /* PATAND */
            SelectObject(dc, oldObject);
        }
    }
    /* #154: remember where the frame went, and show a text selection by inverting it. */
    presenter->LastDestinationX = destinationX; presenter->LastDestinationY = destinationY; presenter->LastDestinationWidth = destinationWidth; presenter->LastDestinationHeight = destinationHeight;
    presenter->LastSourceWidth = presenter->SnapshotWidth; presenter->LastSourceHeight = presenter->SnapshotHeight;
    if (presenter->IsSelection && presenter->SnapshotWidth > 0 && presenter->SnapshotHeight > 0)
    {
        RECT selectionRect;
        selectionRect.left   = destinationX + presenter->SelectionX0 * destinationWidth / presenter->SnapshotWidth;
        selectionRect.right  = destinationX + presenter->SelectionX1 * destinationWidth / presenter->SnapshotWidth;
        selectionRect.top    = destinationY + presenter->SelectionY0 * destinationHeight / presenter->SnapshotHeight;
        selectionRect.bottom = destinationY + presenter->SelectionY1 * destinationHeight / presenter->SnapshotHeight;
        InvertRect(dc, &selectionRect);
    }
    PresentHintDraw(presenter, dc, destinationX, destinationY, destinationWidth);                 /* #138 */
    if (memoryDc)
    {
        PresentWaitVerticalBlank(presenter);
        BitBlt(windowDc, 0, 0, clientWidth, clientHeight, memoryDc, 0, 0, SRCCOPY);
    }
    ReleaseDC(presenter->Window, windowDc);
}

/* ---- fullscreen: DirectDraw 7 ------------------------------------------- */
static VOID PresentReleaseSurface(PVOID *surface)
{
    if (*surface)
    {
        IDirectDrawSurface7_Release(PRESENT_SURFACE(*surface));
        *surface = 0;
    }
}

static VOID PresentFullscreenTeardown(PPRESENT_DDRAW presenter)
{
    presenter->Back = 0;
    PresentReleaseSurface(&presenter->StagingSurface);
    presenter->StagingWidth = presenter->StagingHeight = 0;
    PresentReleaseSurface(&presenter->Primary);
}

/* FULLSCREEN KEEPS THE DESKTOP'S OWN MODE. (s64) (Importance = 2):
 * This used to `SetDisplayMode(640, 480)` and then stretch the frame across all of
 * it, and BOTH halves of that were wrong on a modern panel:
 *   * The monitor is not 4:3. Handed a 640x480 signal, an LCD stretches it across a
 *     16:9 panel itself -- so the picture came out wide no matter what our aspect
 *     setting said, and nothing we did to the pixels could have corrected it. That
 *     is the "pixels get stretched regardless of the setting" report, and the stretch
 *     was happening in the DISPLAY, downstream of everything we control.
 *   * PresentFullscreen then ignored presenter->Aspect completely, so even the part we DID control
 *     was filling rather than fitting.
 * Taking the desktop's current mode fixes both: the panel shows its native signal
 * 1:1, and we letterbox/pillarbox inside it with PresentFit -- the same function the
 * windowed path uses, so the two modes finally agree about what a setting means.
 *
 * [CAUTION]: NO SetDisplayMode AT ALL now. Exclusive mode does not require one, and not calling
 * it also means Alt+Enter no longer makes the monitor resync twice per toggle.
 *
 * [CAUTION]: THE COST IS THAT THE DESTINATION IS NOW BIG. A software nearest-neighbour loop over
 * 1440x1080 is ~1.5M pixels a frame and this host cannot afford that, so the picture goes through
 * an OFFSCREEN SURFACE and a hardware stretch Blt: we convert only SnapshotWidth x SnapshotHeight
 * (64k pixels for mode 13h) and the GPU does the scaling. PresentFullscreen keeps the old software
 * loop as a fallback for a driver that refuses the blt.
 */
static INT PresentFullscreenSetup(PPRESENT_DDRAW presenter)
{
    DDSURFACEDESC2 description; DDSCAPS2 caps; LPDIRECTDRAWSURFACE7 primary = 0, back = 0, staging = 0;
    INT backBuffers;

    /* [INFO]: AN EXPLICIT MODE IS TRIED FIRST, AND ONLY IF THE USER ASKED FOR ONE. Try 32bpp
     * then 16, the same pair this always used; if the display refuses both, fall
     * through to the desktop mode rather than failing the toggle outright.
     */
    presenter->FullscreenWidth = 0; presenter->FullscreenHeight = 0;
    if (presenter->FullscreenModeWidth > 0 && presenter->FullscreenModeHeight > 0)
    {
        if (SUCCEEDED(IDirectDraw7_SetDisplayMode(PRESENT_DIRECT_DRAW, (DWORD)presenter->FullscreenModeWidth,
                                                  (DWORD)presenter->FullscreenModeHeight, PRESENT_DISPLAY_BPP, 0, 0)) ||
            SUCCEEDED(IDirectDraw7_SetDisplayMode(PRESENT_DIRECT_DRAW, (DWORD)presenter->FullscreenModeWidth,
                                                  (DWORD)presenter->FullscreenModeHeight, PRESENT_DISPLAY_BPP_FALLBACK, 0, 0)))
        {
            presenter->FullscreenWidth = presenter->FullscreenModeWidth; presenter->FullscreenHeight = presenter->FullscreenModeHeight;
        }
    }
    if (!presenter->FullscreenWidth)
    {
        ZeroMemory(&description, sizeof description); description.dwSize = sizeof description;
        if (SUCCEEDED(IDirectDraw7_GetDisplayMode(PRESENT_DIRECT_DRAW, &description)) && description.dwWidth && description.dwHeight)
        {
            presenter->FullscreenWidth = (INT)description.dwWidth;
            presenter->FullscreenHeight = (INT)description.dwHeight;
        }
        else
        {
            /* Could not ask. Fall back to the old behaviour rather than guessing -- a
             * forced 640x480 is worse than the desktop mode but better than no picture.
             */
            presenter->FullscreenWidth = PRESENT_FALLBACK_WIDTH; presenter->FullscreenHeight = PRESENT_FALLBACK_HEIGHT;
            if (FAILED(IDirectDraw7_SetDisplayMode(PRESENT_DIRECT_DRAW, PRESENT_FALLBACK_WIDTH, PRESENT_FALLBACK_HEIGHT, PRESENT_DISPLAY_BPP, 0, 0)) &&
                FAILED(IDirectDraw7_SetDisplayMode(PRESENT_DIRECT_DRAW, PRESENT_FALLBACK_WIDTH, PRESENT_FALLBACK_HEIGHT, PRESENT_DISPLAY_BPP_FALLBACK, 0, 0)))
                return -1;
        }
    }

    /* s86: TRIPLE-BUFFERED, so the buffer we draw into is never the one on screen nor
     * the one queued for the next retrace -- see the flip in PresentFullscreen. Two back
     * buffers first; one if the card will not give us two (or ddflip_driver.flag).
     */
    presenter->FlipBuffers = 0;
    for (backBuffers = presenter->IsFlipDriverTimed ? 1 : PRESENT_BACK_BUFFERS; backBuffers >= 1 && !primary; --backBuffers)
    {
        ZeroMemory(&description, sizeof description); description.dwSize = sizeof description;
        description.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT; description.dwBackBufferCount = (DWORD)backBuffers;
        description.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
        if (SUCCEEDED(IDirectDraw7_CreateSurface(PRESENT_DIRECT_DRAW, &description, &primary, NULL))) presenter->FlipBuffers = backBuffers + 1;
        else primary = 0;
    }
    if (!primary) return -1;
    presenter->Primary = primary;
    ZeroMemory(&caps, sizeof caps); caps.dwCaps = DDSCAPS_BACKBUFFER;
    if (FAILED(IDirectDrawSurface7_GetAttachedSurface(primary, &caps, &back))) return -1;
    presenter->Back = back;

    /* The staging surface the guest frame is converted into, at FRAME size. Video
     * memory if the driver will give it (the stretch blt is then card-to-card),
     * system memory if not. Failing both is not fatal -- PresentFullscreen falls back to
     * the software path, which needs no surface at all.
     */
    ZeroMemory(&description, sizeof description); description.dwSize = sizeof description;
    description.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    description.dwWidth = PRESENT_FALLBACK_WIDTH; description.dwHeight = PRESENT_FALLBACK_HEIGHT;
    description.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
    if (FAILED(IDirectDraw7_CreateSurface(PRESENT_DIRECT_DRAW, &description, &staging, NULL)))
    {
        ZeroMemory(&description, sizeof description); description.dwSize = sizeof description;
        description.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        description.dwWidth = PRESENT_FALLBACK_WIDTH; description.dwHeight = PRESENT_FALLBACK_HEIGHT;
        description.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        if (FAILED(IDirectDraw7_CreateSurface(PRESENT_DIRECT_DRAW, &description, &staging, NULL))) staging = 0;
    }
    presenter->StagingSurface = staging;
    presenter->StagingWidth = staging ? PRESENT_FALLBACK_WIDTH : 0; presenter->StagingHeight = staging ? PRESENT_FALLBACK_HEIGHT : 0;
    return 0;
}

/* convert an NTVDD_FRAME into the locked back buffer, packed to its depth. */
static VOID PresentMaskInfo(DWORD mask, INT *shift, INT *bits)
{
    INT shiftCount=0,bitCount=0;
    if(mask)
    {
        while(!(mask&1))
        {
            mask>>=1;
            ++shiftCount;
        } while(mask&1)
        {
            mask>>=1;
            ++bitCount;
        }
    }
    *shift=shiftCount;
    *bits=bitCount;
}

/* The snapshot's pixel at (x,y) as ARGB, whatever depth it was captured at. One
 * accessor so the depth is resolved in exactly one place: the alternative is the
 * same `SnapshotBpp` test copied into four loops, which is how one of them ends up
 * not having it.
 */
static UINT32 PresentSnapshotPixel(PPRESENT_DDRAW presenter, INT row, INT column)
{
    if (presenter->SnapshotBpp == PRESENT_BPP_DIRECT) return presenter->Snapshot32[(size_t)row * presenter->SnapshotWidth + column];
    return PresentRowPalette(presenter, row)[presenter->Snapshot[(size_t)row * presenter->SnapshotWidth + column]];
}

/* Write one snapshot pixel, packed to whatever depth the locked surface is. */
static VOID PresentPutPixel(BYTE *destinationRow, INT column, DWORD bitsPerPixel, UINT32 argb,
                   INT redShift, INT redBits, INT greenShift, INT greenBits, INT blueShift, INT blueBits)
{
    UINT32 red = (argb >> PRESENT_RED_SHIFT) & PRESENT_CHANNEL_MASK, green = (argb >> PRESENT_GREEN_SHIFT) & PRESENT_CHANNEL_MASK, blue = argb & PRESENT_CHANNEL_MASK;
    if (bitsPerPixel == PRESENT_SURFACE_BPP_32) ((DWORD *)destinationRow)[column] = argb;
    else if (bitsPerPixel == PRESENT_SURFACE_BPP_16 || bitsPerPixel == PRESENT_SURFACE_BPP_15)
        ((WORD *)destinationRow)[column] = (WORD)(((red>>(PRESENT_CHANNEL_BITS-redBits))<<redShift)|((green>>(PRESENT_CHANNEL_BITS-greenBits))<<greenShift)|((blue>>(PRESENT_CHANNEL_BITS-blueBits))<<blueShift));
    else if (bitsPerPixel == PRESENT_SURFACE_BPP_24)
    {
        BYTE *pixel = destinationRow + column*PRESENT_BYTES_PER_PIXEL_24;
        pixel[0]=(BYTE)blue;
        pixel[1]=(BYTE)green;
        pixel[2]=(BYTE)red;
    }
    else destinationRow[column] = (BYTE)((red*PRESENT_GREY_RED + green*PRESENT_GREY_GREEN + blue*PRESENT_GREY_BLUE) / PERCENT);
}

/* Convert the snapshot 1:1 into the staging surface's top-left corner. No scaling
 * here on purpose -- that is the GPU's job in PresentFullscreen. 0 = ok.
 */
static INT PresentFullscreenStage(PPRESENT_DDRAW presenter, LPDIRECTDRAWSURFACE7 staging)
{
    DDSURFACEDESC2 description;
    DWORD bitsPerPixel; INT redShift,redBits,greenShift,greenBits,blueShift,blueBits; INT row, column;
    ZeroMemory(&description, sizeof description); description.dwSize = sizeof description;
    if (FAILED(IDirectDrawSurface7_Lock(staging, NULL, &description,
                                        DDLOCK_WAIT|DDLOCK_SURFACEMEMORYPTR, NULL)))
        return -1;
    /* Belt and braces: never write more than the surface the driver handed back. */
    if ((INT)description.dwWidth < presenter->SnapshotWidth || (INT)description.dwHeight < presenter->SnapshotHeight)
    {
        IDirectDrawSurface7_Unlock(staging, NULL);
        return -1;
    }
    bitsPerPixel = description.ddpfPixelFormat.dwRGBBitCount;
    PresentMaskInfo(description.ddpfPixelFormat.dwRBitMask, &redShift, &redBits);
    PresentMaskInfo(description.ddpfPixelFormat.dwGBitMask, &greenShift, &greenBits);
    PresentMaskInfo(description.ddpfPixelFormat.dwBBitMask, &blueShift, &blueBits);
    for (row = 0; row < presenter->SnapshotHeight; ++row)
    {
        BYTE *destinationRow = (BYTE *)description.lpSurface + (size_t)row * description.lPitch;
        for (column = 0; column < presenter->SnapshotWidth; ++column)
            PresentPutPixel(destinationRow, column, bitsPerPixel, PresentSnapshotPixel(presenter, row, column), redShift,redBits,greenShift,greenBits,blueShift,blueBits);
    }
    IDirectDrawSurface7_Unlock(staging, NULL);
    return 0;
}

/* The fallback: nearest-neighbour straight into the back buffer, honouring the same
 * fitted rectangle. Only reached if the driver refuses a stretch blt.
 */
static VOID PresentFullscreenSoftware(PPRESENT_DDRAW presenter, INT fitX, INT fitY, INT fitWidth, INT fitHeight)
{
    DDSURFACEDESC2 description; LPDIRECTDRAWSURFACE7 back = PRESENT_SURFACE(presenter->Back);
    DWORD bitsPerPixel; INT redShift,redBits,greenShift,greenBits,blueShift,blueBits; INT sourceRow, sourceColumn, destinationRow, column;
    ZeroMemory(&description, sizeof description); description.dwSize = sizeof description;
    if (IDirectDrawSurface7_Lock(back, NULL, &description, DDLOCK_WAIT|DDLOCK_SURFACEMEMORYPTR, NULL)
            == DDERR_SURFACELOST)
    {
        IDirectDrawSurface7_Restore(PRESENT_SURFACE(presenter->Primary));
        return;
    }
    bitsPerPixel = description.ddpfPixelFormat.dwRGBBitCount;
    PresentMaskInfo(description.ddpfPixelFormat.dwRBitMask, &redShift, &redBits);
    PresentMaskInfo(description.ddpfPixelFormat.dwGBitMask, &greenShift, &greenBits);
    PresentMaskInfo(description.ddpfPixelFormat.dwBBitMask, &blueShift, &blueBits);
    for (destinationRow = 0; destinationRow < presenter->FullscreenHeight; ++destinationRow)
    {
        BYTE *rowPointer = (BYTE *)description.lpSurface + (size_t)destinationRow * description.lPitch;
        INT isInsideY = (destinationRow >= fitY && destinationRow < fitY + fitHeight);
        sourceRow = isInsideY ? (destinationRow - fitY) * presenter->SnapshotHeight / fitHeight : 0;
        for (column = 0; column < presenter->FullscreenWidth; ++column)
        {
            /* The bars are part of the picture: this is a FLIP CHAIN, so a pixel we
             * do not write keeps whatever the buffer held two frames ago.
             */
            if (!isInsideY || column < fitX || column >= fitX + fitWidth)
            {
                PresentPutPixel(rowPointer, column, bitsPerPixel, 0, redShift,redBits,greenShift,greenBits,blueShift,blueBits);
                continue;
            }
            sourceColumn = (column - fitX) * presenter->SnapshotWidth / fitWidth;
            PresentPutPixel(rowPointer, column, bitsPerPixel, PresentSnapshotPixel(presenter, sourceRow, sourceColumn),
                   redShift,redBits,greenShift,greenBits,blueShift,blueBits);
        }
    }
    IDirectDrawSurface7_Unlock(back, NULL);
}

/* -- THE STAGING SURFACE MUST HOLD THE WHOLE FRAME. (s84, user: "fullscreen VESA --
 * ZAR, Duke3D, VESACUBE -- crashes NTVDMEX with DirectDraw") It was created once at
 * 640x480, and PresentFullscreenStage copies SnapshotWidth x SnapshotHeight into it: a 1024x768 or 800x600 VESA
 * frame wrote past the end of the surface (HOSTFAULT write AV in PresentFullscreen). Grow it
 * to the frame when the frame outgrows it -- video memory if the driver gives it,
 * system memory if not -- and if neither, drop it: PresentFullscreen then takes the
 * software path, which needs no staging surface. Returns the surface or NULL.
 */
static LPDIRECTDRAWSURFACE7 PresentFullscreenStageSurface(PPRESENT_DDRAW presenter, INT width, INT height)
{
    DDSURFACEDESC2 description;
    PVOID staging = 0;
    if (presenter->StagingSurface && width <= presenter->StagingWidth && height <= presenter->StagingHeight) return PRESENT_SURFACE(presenter->StagingSurface);
    PresentReleaseSurface(&presenter->StagingSurface);
    presenter->StagingWidth = presenter->StagingHeight = 0;
    if (width <= 0 || height <= 0) return NULL;
    if (width < PRESENT_FALLBACK_WIDTH) width = PRESENT_FALLBACK_WIDTH;
    if (height < PRESENT_FALLBACK_HEIGHT) height = PRESENT_FALLBACK_HEIGHT;
    ZeroMemory(&description, sizeof description); description.dwSize = sizeof description;
    description.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    description.dwWidth = (DWORD)width; description.dwHeight = (DWORD)height;
    description.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
    if (FAILED(IDirectDraw7_CreateSurface(PRESENT_DIRECT_DRAW, &description, (LPDIRECTDRAWSURFACE7 *)&staging, NULL)))
    {
        description.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        if (FAILED(IDirectDraw7_CreateSurface(PRESENT_DIRECT_DRAW, &description, (LPDIRECTDRAWSURFACE7 *)&staging, NULL)))
            staging = 0;
    }
    presenter->StagingSurface = staging;
    if (staging)
    {
        presenter->StagingWidth = width;
        presenter->StagingHeight = height;
    }
    return PRESENT_SURFACE(staging);
}

static VOID PresentFullscreen(PPRESENT_DDRAW presenter)
{
    LPDIRECTDRAWSURFACE7 back = PRESENT_SURFACE(presenter->Back), staging;
    INT fitX, fitY, fitWidth, fitHeight, isDone = 0;
    if (!back) return;
    /* [INFO]: THE SAME LAYOUT THE WINDOW USES (PresentLayout). One function for both
     * renderers is the point: a setting that meant one thing windowed and another
     * fullscreen is exactly the bug it exists to prevent.
     */
    /* #325: exclusive fullscreen is "the screen": the same layout as the window. */
    PresentLayout(presenter->Aspect, presenter->Fit, PRESENT_LAYOUT_SCREEN, presenter->FullscreenWidth, presenter->FullscreenHeight, presenter->SnapshotWidth, presenter->SnapshotHeight,
                   &fitX, &fitY, &fitWidth, &fitHeight);
    /* ...and remember it: the mouse maps through the rectangle actually drawn. */
    presenter->LastDestinationX = fitX; presenter->LastDestinationY = fitY; presenter->LastDestinationWidth = fitWidth; presenter->LastDestinationHeight = fitHeight;
    presenter->LastSourceWidth = presenter->SnapshotWidth; presenter->LastSourceHeight = presenter->SnapshotHeight;

    /* -- SHARP PIXELS ON THE DIRECTDRAW PATH TOO. (#223; user: "Smoothness should come
     * from scaler/filter. With them off it should be stretched, sharp pixels,
     * regardless of which renderer is used.") A stretching Blt is FILTERED BY THE
     * DRIVER and DirectDraw cannot forbid it. With Filtering = Nearest the frame is
     * drawn into the back buffer's DC by GDI's StretchDIBits in COLORONCOLOR mode --
     * the same exact-pixel routine, and the same picture (PresentSnapshotDib, with Scale2x),
     * as the GDI renderer. [CAUTION] Not a hand-written per-pixel stretch into video memory:
     * that was tried first and was unplayably slow on the rig (it read VRAM back to
     * copy repeated rows). Bilinear keeps the driver's stretch, which IS bilinear.
     */
    if (presenter->Filter != PRESENT_FILTER_BILINEAR && presenter->SnapshotWidth > 0 && presenter->SnapshotHeight > 0)
    {
        HDC dc;
        if (fitX || fitY)                        /* letterboxed -> clear the bars */
        {
            DDBLTFX bltFx;
            ZeroMemory(&bltFx, sizeof bltFx); bltFx.dwSize = sizeof bltFx; bltFx.dwFillColor = 0;
            IDirectDrawSurface7_Blt(back, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &bltFx);
        }
        if (SUCCEEDED(IDirectDrawSurface7_GetDC(back, &dc)))
        {
            PRESENT_SNAPSHOT_DIB dib; INT sourceWidth, sourceHeight;
            const BYTE *pixels = PresentSnapshotDib(presenter, &dib, &sourceWidth, &sourceHeight, PRESENT_SNAPSHOT_2X);
            isDone = PresentBlitPicture(dc, fitX, fitY, fitWidth, fitHeight, pixels, &dib, sourceWidth, sourceHeight, presenter->Filter);   /* #325 */
            IDirectDrawSurface7_ReleaseDC(back, dc);
        }
    }
    staging = isDone ? NULL : PresentFullscreenStageSurface(presenter, presenter->SnapshotWidth, presenter->SnapshotHeight);
    if (!isDone && staging && presenter->SnapshotWidth > 0 && presenter->SnapshotHeight > 0 && PresentFullscreenStage(presenter, staging) == 0)
    {
        RECT source, destination;
        source.left = 0; source.top = 0;
        source.right = presenter->SnapshotWidth; source.bottom = presenter->SnapshotHeight;
        destination.left = fitX; destination.top = fitY; destination.right = fitX + fitWidth; destination.bottom = fitY + fitHeight;
        if (fitX || fitY)                        /* letterboxed -> clear the bars */
        {
            DDBLTFX bltFx;
            ZeroMemory(&bltFx, sizeof bltFx); bltFx.dwSize = sizeof bltFx; bltFx.dwFillColor = 0;
            IDirectDrawSurface7_Blt(back, NULL, NULL, NULL,
                                    DDBLT_COLORFILL | DDBLT_WAIT, &bltFx);
        }
        isDone = SUCCEEDED(IDirectDrawSurface7_Blt(back, &destination, staging, &source, DDBLT_WAIT, NULL));
    }
    if (!isDone) PresentFullscreenSoftware(presenter, fitX, fitY, fitWidth, fitHeight);
    /* #227: the Scanlines/CRT scaler on the exclusive path too -- the same AND pattern
     * the GDI path lays over its stretched picture, on the back buffer's DC.
     */
    if (PresentScalerHasScanlines(presenter->Scaler))
    {
        HDC dc; HBRUSH brush = PresentScanlineBrush();
        if (brush && SUCCEEDED(IDirectDrawSurface7_GetDC(back, &dc)))
        {
            HGDIOBJ oldObject = SelectObject(dc, brush);
            PatBlt(dc, fitX, fitY, fitWidth, fitHeight, PRESENT_ROP_PATAND);      /* PATAND */
            SelectObject(dc, oldObject);
            IDirectDrawSurface7_ReleaseDC(back, dc);
        }
    }
    {   HDC dc;                                   /* #138 on the exclusive path */
        if (presenter->HintText && !presenter->IsOsdOff && SUCCEEDED(IDirectDrawSurface7_GetDC(back, &dc)))
        {
            PresentHintDraw(presenter, dc, fitX, fitY, fitWidth);
            IDirectDrawSurface7_ReleaseDC(back, dc);
        } }

    /* [WARNING]: THE FLIP: TRIPLE-BUFFERED, NEVER BLOCKING, THE DRIVER TIMES IT. (user, s86)
     * (Importance = 1): With one back buffer and DDFLIP_WAIT, DirectDraw fullscreen tore far more
     * than GDI. The first fix timed the flip ourselves (PresentWaitVerticalBlank + NOVSYNC) -- no
     * tearing, but the counters below then showed EVERY flip still pending a retrace: this driver
     * (Quadro K4000, 321.01) already waits, so we were waiting TWICE. Present cost a whole frame
     * (16 ms mean), the UI thread fell behind a 70 Hz guest, and each snapshot was taken mid-way
     * through the guest's NEXT frame -- Mario's menu and status text, erased and redrawn every
     * frame, flickered.
     * - So: two back buffers, and Flip with DONOTWAIT. The buffer we draw into is never
     *   on screen nor queued; if a flip is still pending, this frame is DROPPED rather
     *   than waited for (the next present carries a newer one), so the UI thread stays
     *   on the guest's beat. The driver's own vsync does the timing.
     * - A driver whose flips do NOT wait (they land at once -- a mirror driver can do
     *   that) gets our timing back automatically: 30 such flips in a row with VSync on
     *   switches to PresentWaitVerticalBlank + NOVSYNC, which measured tear-free.
     *
     * [CAUTION]: Only one back buffer (the card refused two, or ddflip_driver.flag): the old
     * blocking DDFLIP_WAIT, since dropping would mean drawing into the queued buffer.
     */
    {   LPDIRECTDRAWSURFACE7 primary = PRESENT_SURFACE(presenter->Primary);
        DWORD flags, scanLine = 0; HRESULT result;
        INT isOurWait = presenter->IsVsync && presenter->IsFlipOurWait;
        PresentMonitorQuery(presenter);
        if (isOurWait)
        {
            PresentWaitVerticalBlank(presenter);
            flags = DDFLIP_WAIT | DDFLIP_NOVSYNC;
        }
        else if (presenter->FlipBuffers >= PRESENT_TRIPLE_BUFFERS) flags = DDFLIP_DONOTWAIT;
        else                       flags = DDFLIP_WAIT;
        if (IDirectDraw7_GetScanLine(PRESENT_DIRECT_DRAW, &scanLine) == DD_OK && presenter->MonitorLines > 0 &&
            scanLine >= PRESENT_MID_SCREEN_MARGIN && scanLine + PRESENT_MID_SCREEN_MARGIN < (DWORD)presenter->MonitorLines)
            ++presenter->FlipMidScreen;
        result = IDirectDrawSurface7_Flip(primary, NULL, flags);
        if (result == DDERR_SURFACELOST)
        {
            IDirectDrawSurface7_Restore(primary);
            return;
        }
        if (result == DDERR_WASSTILLDRAWING)
        {
            ++presenter->FlipDropped;
            return;
        }
        if (result == DD_OK)
        {
            result = IDirectDrawSurface7_GetFlipStatus(primary, DDGFS_ISFLIPDONE);
            if (result == DD_OK)
            {
                ++presenter->FlipDone;
                if (!isOurWait && presenter->IsVsync && !presenter->IsFlipDriverTimed && ++presenter->FlipStreak >= PRESENT_FLIP_STREAK_OUR_WAIT)
                    presenter->IsFlipOurWait = 1;
            }
            else
            {
                if (result == DDERR_WASSTILLDRAWING) ++presenter->FlipPending;
                presenter->FlipStreak = 0;
            }
        }
    }
}

/* ---- public API --------------------------------------------------------- */
INT PresentDdrawInitialize(PPRESENT_DDRAW presenter, HWND window)
{
    PFN_DIRECT_DRAW_CREATE_EX directDrawCreateEx; LPDIRECTDRAW7 directDraw = 0;
    ZeroMemory(presenter, sizeof *presenter);
    presenter->Window = window; presenter->FullscreenWidth = PRESENT_FALLBACK_WIDTH; presenter->FullscreenHeight = PRESENT_FALLBACK_HEIGHT; presenter->StatusHeight = PRESENT_STATUS_HEIGHT;
    /* The struct was just zeroed, and zero is the WRONG default for exactly one of
     * these: this host has always waited for vblank. The other three (nearest, fill
     * the client, no scaler) are what it has always done, so zero is right.
     */
    presenter->IsVsync = 1;
    presenter->DirectDrawModule = LoadLibraryA(PRESENT_MODULE_DDRAW);          /* for fullscreen (optional) */
    if (presenter->DirectDrawModule)
    {
        directDrawCreateEx = (PFN_DIRECT_DRAW_CREATE_EX)GetProcAddress(presenter->DirectDrawModule, PRESENT_EXPORT_DIRECT_DRAW_CREATE_EX);
        if (directDrawCreateEx && SUCCEEDED(directDrawCreateEx(NULL, (LPVOID *)&directDraw, &g_PresentIidDirectDraw7, NULL)))
        {
            presenter->DirectDraw = directDraw;
            IDirectDraw7_SetCooperativeLevel(PRESENT_DIRECT_DRAW, window, DDSCL_NORMAL);
        }
    }
    return 0;                                        /* windowed (GDI) always works */
}

VOID PresentDdrawShutdown(PPRESENT_DDRAW presenter)
{
    PresentMemoryRelease(presenter);                /* #217 */
    if (presenter->IsFullscreen && presenter->DirectDraw)
    {
        PresentFullscreenTeardown(presenter);
        IDirectDraw7_RestoreDisplayMode(PRESENT_DIRECT_DRAW);
    }
    if (presenter->DirectDraw)
    {
        IDirectDraw7_Release(PRESENT_DIRECT_DRAW);
        presenter->DirectDraw = 0;
    }
    if (presenter->DirectDrawModule)
    {
        FreeLibrary(presenter->DirectDrawModule);
        presenter->DirectDrawModule = 0;
    }
}

INT PresentDdrawSetFullscreen(PPRESENT_DDRAW presenter, INT isOn)
{
    if (isOn == presenter->IsFullscreen) return 0;
    /* [INFO]: THE DEFAULT IS A BORDERLESS WINDOW, NOT AN EXCLUSIVE MODE. See the long note
     * over PresentGdi: exclusive DirectDraw is where the blurring comes from. The
     * caller has already made the window chromeless and screen-sized, so all that is
     * left to do is record the state and let PresentGdi do what it does windowed --
     * which the user has demonstrated is sharp.
     */
    if (!presenter->IsFullscreenDirectDraw)
    {
        presenter->IsFullscreen = isOn;
        InvalidateRect(presenter->Window, NULL, TRUE);
        return 0;
    }
    if (!presenter->DirectDraw) return -1;           /* no DirectDraw -> stay windowed */
    if (isOn)
    {
        if (FAILED(IDirectDraw7_SetCooperativeLevel(PRESENT_DIRECT_DRAW, presenter->Window,
                       DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT))) return -1;
        if (PresentFullscreenSetup(presenter))
        {
            IDirectDraw7_SetCooperativeLevel(PRESENT_DIRECT_DRAW, presenter->Window, DDSCL_NORMAL);
            return -1;
        }
    }
    else
    {
        PresentFullscreenTeardown(presenter);
        IDirectDraw7_RestoreDisplayMode(PRESENT_DIRECT_DRAW);
        IDirectDraw7_SetCooperativeLevel(PRESENT_DIRECT_DRAW, presenter->Window, DDSCL_NORMAL);
        InvalidateRect(presenter->Window, NULL, TRUE);
    }
    presenter->IsFullscreen = isOn;
    return 0;
}

/* copy the live frame into the back-buffer (call under the bus lock). */
VOID PresentDdrawSnapshot(PPRESENT_DDRAW presenter, PCNTVDD_FRAME frame)
{
    INT row;
    if (!frame || !frame->Width || !frame->Height || !frame->Pixels || (frame->BitsPerPixel != PRESENT_BPP_INDEXED && frame->BitsPerPixel != PRESENT_BPP_DIRECT)
        || frame->Width > NTVDD_FRAME_MAX_WIDTH || frame->Height > NTVDD_FRAME_MAX_HEIGHT)
    {
        presenter->IsSnapshotValid = 0; return;
    }
    presenter->SnapshotBpp = frame->BitsPerPixel;
    if (frame->BitsPerPixel == PRESENT_BPP_DIRECT)
    {
        /* Direct colour: no palette is involved at all, and a split palette is
         * meaningless here -- the pixels already carry their own colour.
         */
        for (row = 0; row < (INT)frame->Height; ++row)
            CopyMemory(presenter->Snapshot32 + (size_t)row * frame->Width,
                       frame->Pixels + (size_t)row * frame->Stride, (size_t)frame->Width * PRESENT_ARGB_BYTES);
        presenter->IsSnapshotSplit = 0;
        presenter->RowPaletteY = -1;
        presenter->SnapshotWidth = frame->Width; presenter->SnapshotHeight = frame->Height; presenter->IsSnapshotValid = 1;
        if (presenter->Tint)                             /* #229: per pixel, only here */
        {
            size_t index, count = (size_t)frame->Width * frame->Height;
            for (index = 0; index < count; ++index) presenter->Snapshot32[index] = PresentTint(presenter->Snapshot32[index], presenter->Tint);
        }
        return;
    }
    for (row = 0; row < (INT)frame->Height; ++row)
        CopyMemory(presenter->Snapshot + (size_t)row * frame->Width, frame->Pixels + (size_t)row * frame->Stride, frame->Width);
    if (frame->Palette) CopyMemory(presenter->SnapshotPalette, frame->Palette, NTVDD_PALETTE_ENTRIES * sizeof(UINT32));
    presenter->IsSnapshotSplit = VddFrameHasSplit(frame);
    if (presenter->IsSnapshotSplit)
    {
        CopyMemory(presenter->SnapshotPaletteBase,   frame->PaletteBase,  NTVDD_PALETTE_ENTRIES * sizeof(UINT32));
        CopyMemory(presenter->SnapshotPaletteSplit,  frame->PaletteSplit, NTVDD_PALETTE_ENTRIES * sizeof(UINT32));
        CopyMemory(presenter->SnapshotSplitFrame,frame->SplitFrame,   NTVDD_PALETTE_ENTRIES * sizeof(UINT32));
        CopyMemory(presenter->SnapshotSplitRow,  frame->SplitRow,     NTVDD_PALETTE_ENTRIES * sizeof(WORD));
        presenter->SnapshotFrameNumber = frame->FrameNumber;
    }
    if (presenter->Tint)                    /* #229: recolour the COLOURS, not the pixels */
    {
        INT index;
        for (index = 0; index < NTVDD_PALETTE_ENTRIES; ++index)
        {
            presenter->SnapshotPalette[index] = PresentTint(presenter->SnapshotPalette[index], presenter->Tint);
            if (presenter->IsSnapshotSplit)
            {
                presenter->SnapshotPaletteBase[index]  = PresentTint(presenter->SnapshotPaletteBase[index],  presenter->Tint);
                presenter->SnapshotPaletteSplit[index] = PresentTint(presenter->SnapshotPaletteSplit[index], presenter->Tint);
            }
        }
    }
    presenter->RowPaletteY = -1;
    presenter->SnapshotWidth = frame->Width; presenter->SnapshotHeight = frame->Height; presenter->IsSnapshotValid = 1;
}

/* The palette for source row `y` of the snapshot: the single palette on an ordinary
 * frame; on a raster-split frame the per-entry resolution of VddFramePaletteAt,
 * computed once per row.
 */
static const UINT32 *PresentRowPalette(PPRESENT_DDRAW presenter, INT row)
{
    NTVDD_FRAME frame; UINT index;
    if (!presenter->IsSnapshotSplit) return presenter->SnapshotPalette;
    if (presenter->RowPaletteY == row) return presenter->RowPalette;
    frame.Palette = presenter->SnapshotPalette; frame.PaletteBase = presenter->SnapshotPaletteBase;
    frame.PaletteSplit = presenter->SnapshotPaletteSplit; frame.SplitRow = presenter->SnapshotSplitRow;
    frame.SplitFrame = presenter->SnapshotSplitFrame; frame.FrameNumber = presenter->SnapshotFrameNumber;
    for (index = 0; index < NTVDD_PALETTE_ENTRIES; ++index) presenter->RowPalette[index] = VddFramePaletteAt(&frame, (UINT)row, index);
    presenter->RowPaletteY = row;
    return presenter->RowPalette;
}

/* blit the back-buffer to the screen, vsync'd (call outside the lock). */
VOID PresentDdrawPresent(PPRESENT_DDRAW presenter)
{
    if (!presenter->IsSnapshotValid) return;
    /* `back` is only non-NULL when the exclusive path actually set up, so this also
     * covers "we asked for DirectDraw fullscreen and it refused" -- which must fall
     * back to drawing something rather than to drawing nothing.
     */
    LARGE_INTEGER frequency, start, end;
    INT isFullscreen = (presenter->IsFullscreen && presenter->DirectDraw && presenter->Back);
    QueryPerformanceCounter(&start);
    if (isFullscreen) PresentFullscreen(presenter);
    else    PresentGdi(presenter);
    QueryPerformanceCounter(&end);
    if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart)    /* s84 */
    {
        ULONG elapsedUs = (ULONG)(((end.QuadPart - start.QuadPart) * MICROSECONDS_PER_SECOND) / frequency.QuadPart);
        if (isFullscreen)
        {
            presenter->PresentFullscreenCount++;
            presenter->PresentFullscreenUs  += elapsedUs;
            if (elapsedUs > presenter->PresentFullscreenMax)  presenter->PresentFullscreenMax  = elapsedUs;
        }
        else
        {
            presenter->PresentWindowCount++;
            presenter->PresentWindowUs += elapsedUs;
            if (elapsedUs > presenter->PresentWindowMax) presenter->PresentWindowMax = elapsedUs;
        }
    }
}

VOID PresentDdrawFrame(PPRESENT_DDRAW presenter, PCNTVDD_FRAME frame)
{
    PresentDdrawSnapshot(presenter, frame);
    PresentDdrawPresent(presenter);
}

/* Save the current 8bpp snapshot as an indexed .bmp at `path`. This is OCCLUSION-PROOF
 * -- it serialises our own back-buffer (presenter->Snapshot + presenter->SnapshotPalette), not the on-screen
 * pixels -- so the host can screenshot ITSELF for headless/remote visual validation:
 * a graphical run (mode 13h, Skyroads, the PM demos) is verified by reading the .bmp
 * off the SMB share instead of via VNC or a physical monitor (VNC capture is dead on
 * the real box). Call on the UI thread (which owns snap) or under the bus lock.
 * Returns 0 on success, <0 if there's nothing valid to save or the write failed.
 */
static VOID PresentStoreLe16(BYTE *bytes, UINT value)
{
    bytes[0]=(BYTE)value;
    bytes[1]=(BYTE)(value>>BYTE_SHIFT);
}

static VOID PresentStoreLe32(BYTE *bytes, DWORD value)
{
    bytes[0]=(BYTE)value;
    bytes[1]=(BYTE)(value>>BYTE_SHIFT);
    bytes[2]=(BYTE)(value>>WORD_SHIFT);
    bytes[3]=(BYTE)(value>>TOP_BYTE_SHIFT);
}

INT PresentDdrawSaveBmp(PPRESENT_DDRAW presenter, PCSTR path)
{
    INT width = presenter->SnapshotWidth, height = presenter->SnapshotHeight, column, row;
    DWORD rowBytes, imageBytes, dataOffset, written;
    HANDLE file;
    BYTE fileHeader[BMP_FILE_HEADER_BYTES], infoHeader[BMP_INFO_HEADER_BYTES];
    static BYTE palette[NTVDD_PALETTE_ENTRIES * BMP_QUAD_BYTES];
    static BYTE row8[NTVDD_FRAME_MAX_WIDTH + BMP_ROW_SLACK];
    /* 24bpp output is needed for a split palette AND for a direct-colour frame:
     * neither can be described by one 256-entry BMP palette.
     */
    INT isTrueColour = presenter->IsSnapshotSplit || presenter->SnapshotBpp == PRESENT_BPP_DIRECT;
    static BYTE row24[NTVDD_FRAME_MAX_WIDTH * PRESENT_BYTES_PER_PIXEL_24 + BMP_ROW_SLACK];
    /* #325: any frame up to the maximum (720x400 text, VESA) -- this was 640x480. */
    if (!presenter->IsSnapshotValid || width <= 0 || height <= 0 || width > NTVDD_FRAME_MAX_WIDTH || height > NTVDD_FRAME_MAX_HEIGHT) return -1;
    /* A raster-split frame cannot be an 8bpp BMP (one palette per file), so it is
     * written as 24bpp with every row resolved. Ordinary frames stay 8bpp: the
     * oracle tools compare palette INDICES and must keep them.
     */
    rowBytes  = isTrueColour ? (((DWORD)width * PRESENT_BYTES_PER_PIXEL_24 + BMP_ROW_PAD) & ~BMP_ROW_ALIGN_MASK) : (((DWORD)width + BMP_ROW_PAD) & ~BMP_ROW_ALIGN_MASK);
    imageBytes = rowBytes * (DWORD)height;
    dataOffset   = isTrueColour ? BMP_FILE_HEADER_BYTES + BMP_INFO_HEADER_BYTES : BMP_FILE_HEADER_BYTES + BMP_INFO_HEADER_BYTES + NTVDD_PALETTE_ENTRIES * BMP_QUAD_BYTES;

    /* BITMAPFILEHEADER */
    fileHeader[0] = 'B'; fileHeader[1] = 'M';
    PresentStoreLe32(fileHeader + BMP_FILE_SIZE_OFFSET, dataOffset + imageBytes);               /* whole file size */
    PresentStoreLe16(fileHeader + BMP_RESERVED1_OFFSET, 0); PresentStoreLe16(fileHeader + BMP_RESERVED2_OFFSET, 0);
    PresentStoreLe32(fileHeader + BMP_DATA_OFFSET_OFFSET, dataOffset);
    /* BITMAPINFOHEADER (positive height -> bottom-up rows) */
    PresentStoreLe32(infoHeader + BMP_INFO_SIZE_OFFSET, BMP_INFO_HEADER_BYTES);
    PresentStoreLe32(infoHeader + BMP_WIDTH_OFFSET, (DWORD)width);  PresentStoreLe32(infoHeader + BMP_HEIGHT_OFFSET, (DWORD)height);
    PresentStoreLe16(infoHeader + BMP_PLANES_OFFSET, 1);        PresentStoreLe16(infoHeader + BMP_BPP_OFFSET, (WORD)(isTrueColour ? PRESENT_SURFACE_BPP_24 : PRESENT_BPP_INDEXED)); /* 1 plane */
    /* `split` is computed by the caller as "needs 24bpp", which a direct-colour
     * snapshot also does -- see the assignment below.
     */
    PresentStoreLe32(infoHeader + BMP_COMPRESSION_OFFSET, 0);        PresentStoreLe32(infoHeader + BMP_IMAGE_SIZE_OFFSET, imageBytes);   /* BI_RGB */
    PresentStoreLe32(infoHeader + BMP_X_PPM_OFFSET, BMP_PIXELS_PER_METRE);     PresentStoreLe32(infoHeader + BMP_Y_PPM_OFFSET, BMP_PIXELS_PER_METRE);    /* ~72 dpi */
    PresentStoreLe32(infoHeader + BMP_COLOURS_USED_OFFSET, isTrueColour ? 0 : NTVDD_PALETTE_ENTRIES); PresentStoreLe32(infoHeader + BMP_COLOURS_IMPORTANT_OFFSET, 0); /* colours used */
    /* palette: SnapshotPalette is 0xAARRGGBB -> RGBQUAD {B,G,R,0} */
    for (column = 0; column < NTVDD_PALETTE_ENTRIES; ++column)
    {
        UINT32 argb = presenter->SnapshotPalette[column];
        palette[column*BMP_QUAD_BYTES+0] = (BYTE)(argb & PRESENT_CHANNEL_MASK);
        palette[column*BMP_QUAD_BYTES+1] = (BYTE)((argb >> PRESENT_GREEN_SHIFT) & PRESENT_CHANNEL_MASK);
        palette[column*BMP_QUAD_BYTES+2] = (BYTE)((argb >> PRESENT_RED_SHIFT) & PRESENT_CHANNEL_MASK);
        palette[column*BMP_QUAD_BYTES+3] = 0;
    }
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return -1;
    WriteFile(file, fileHeader, BMP_FILE_HEADER_BYTES, &written, NULL);
    WriteFile(file, infoHeader, BMP_INFO_HEADER_BYTES, &written, NULL);
    if (!isTrueColour) WriteFile(file, palette, sizeof palette, &written, NULL);
    for (row = height - 1; row >= 0; --row)     /* BMP is bottom-up */
    {
        if (isTrueColour)
        {
            for (column = 0; column < width; ++column)
            {
                UINT32 argb = PresentSnapshotPixel(presenter, row, column);
                row24[column*PRESENT_BYTES_PER_PIXEL_24+0] = (BYTE)(argb & PRESENT_CHANNEL_MASK); row24[column*PRESENT_BYTES_PER_PIXEL_24+1] = (BYTE)((argb >> PRESENT_GREEN_SHIFT) & PRESENT_CHANNEL_MASK);
                row24[column*PRESENT_BYTES_PER_PIXEL_24+2] = (BYTE)((argb >> PRESENT_RED_SHIFT) & PRESENT_CHANNEL_MASK);
            }
            for (column = width * PRESENT_BYTES_PER_PIXEL_24; column < (INT)rowBytes; ++column) row24[column] = 0;
            WriteFile(file, row24, rowBytes, &written, NULL);
            continue;
        }
        for (column = 0; column < width; ++column) row8[column] = presenter->Snapshot[(size_t)row * (size_t)width + (size_t)column];
        for (column = width; column < (INT)rowBytes; ++column) row8[column] = 0;
        WriteFile(file, row8, rowBytes, &written, NULL);
    }
    CloseHandle(file);
    return 0;
}
