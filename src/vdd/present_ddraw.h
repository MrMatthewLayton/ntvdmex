/*
 * present_ddraw.h -- the DirectDraw presentation backend.  (M3 slice-3, ADR-0008)
 *
 * The mode-agnostic frame sink behind the VDD bus: the video VDD produces an
 * `NTVDD_FRAME` (text or graphics, palettised or ARGB) and this layer blits it
 * into a host window via DirectDraw 7, in either WINDOWED or exclusive
 * FULLSCREEN mode.  One software conversion path (index -> ARGB through the
 * frame palette) feeds a 32bpp surface in both modes, so palette management and
 * mode-specific code stay out of the hot path.
 *
 * This is the half of the "merge the two binaries" decision that owns the
 * window's client area; the host owns the window + message loop (Luna chrome is
 * the OS-painted non-client frame).  DirectDraw is bound at runtime
 * (LoadLibrary/GetProcAddress) with an inline IID, so the no-CRT minimal-import
 * rule holds: no -lddraw / -ldxguid.
 */
#ifndef NTVDMEX_PRESENT_DDRAW_H
#define NTVDMEX_PRESENT_DDRAW_H

/* PresentSnapshotDib: the snapshot's scale. */
#define PRESENT_SNAPSHOT_1X 0
#define PRESENT_SNAPSHOT_2X 1

#include <windows.h>
#include "ntvdd.h"
#include "bmp_format.h"      /* the BMP file format */

/* Windowed mode presents via GDI StretchDIBits (cursor-friendly, expose-correct);
   exclusive fullscreen uses DirectDraw. The video blits into the client area
   above a host-drawn status bar PRESENT_STATUS_HEIGHT pixels tall. */
#define PRESENT_STATUS_HEIGHT 22

/* DirectDraw objects are held as void* so this header stays ddraw.h-free. */
typedef struct _PRESENT_DDRAW {
    HWND  Window;
    HMODULE DirectDrawModule;
    PVOID DirectDraw;   /* IDirectDraw7                                         */
    PVOID Primary;      /* primary surface (desktop, or fullscreen flip chain)  */
    PVOID Back;         /* fullscreen back buffer (NULL when windowed)          */
    PVOID Clipper;      /* windowed clipper on Window (NULL when fullscreen)      */
    PVOID StagingSurface;       /* logical-size 32bpp offscreen we convert frames into  */
    INT   StagingWidth, StagingHeight;   /* current fbsurf size                                  */
    INT   IsFullscreen;
    INT   FullscreenWidth, FullscreenHeight;   /* exclusive-fullscreen mode size (default 640x480)     */
    INT   StatusHeight; /* reserved bottom strip for the status bar (windowed)  */
    /* ── THE DISPLAY PAGE, AS FOUR FIELDS. ───────────────────────────────────
         Set by settings_apply() in the host; read on every present. All four
         default to what this host did before they existed, so a machine with no
         stored settings behaves exactly as it always has. */
    INT   IsVsync;      /* 1 = time the blit near vblank (the historical default)*/
    INT   MonitorLines, MonitorHz;/* monitor lines + refresh, read once by PresentWaitVerticalBlank (0 = not yet, -1 = unknown) */
    INT   Filter;       /* PRESENT_FILTER_* -- nearest / bilinear / sharp (#325)  */
    INT   Fit;          /* PRESENT_FIT_* -- maximised/fullscreen: whole pixels or fill */
    INT   Aspect;       /* PRESENT_ASPECT_* -- Native (square pixels) or forced  */
    INT   Scaler;       /* PRESENT_SCALER_* (present_scale.h)                   */
    /* ── FULLSCREEN (s64). ───────────────────────────────────────────────────
         By DEFAULT fullscreen is a borderless window drawn by PresentGdi, because
         DirectDraw's stretch blt is filtered by the driver and there is no way to
         forbid that -- see the long note over PresentGdi. So:
           IsFullscreenDirectDraw  0 = borderless window (default, SHARP)
                         1 = exclusive DirectDraw (ddrawfs.flag; kept for tearing)
           FullscreenModeWidth/Height only consulted by the exclusive path; 0 = no mode change
           IsFullscreenInteger    snap the fullscreen picture to whole pixel multiples    */
    INT   IsFullscreenDirectDraw;
    INT   FullscreenModeWidth, FullscreenModeHeight;
    INT   IsFullscreenInteger;
    /* double-buffer snapshot: filled under the caller's lock by _snapshot(),
       blitted (vsync'd) outside it by _present(). Removes the concurrent-write
       tearing of the live framebuffer. 8bpp + palette (all our frames). */
    /* ── THE SNAPSHOT NOW CARRIES DEPTH. (s74) VESA direct-colour modes hand us
         32-bit ARGB, not palette indices, so there are two buffers and `SnapshotBpp`
         says which one holds this frame. The 8bpp path is byte-for-byte what it
         always was -- Doom, Heretic, Hexen and ZAR all run through it and the
         release was imminent when this landed. Sized to the widest mode
         g_VideoVesaModes[] advertises: every mode we publish must be one we can DISPLAY,
         or the list is promising something the presenter drops on the floor. */
    BYTE  Snapshot[NTVDD_FRAME_MAX_WIDTH * NTVDD_FRAME_MAX_HEIGHT];
    UINT32 Snapshot32[NTVDD_FRAME_MAX_WIDTH * NTVDD_FRAME_MAX_HEIGHT];   /* ARGB, when SnapshotBpp == 32 */
    BYTE  SnapshotBpp;                  /* 8 = Snapshot[] + SnapshotPalette, 32 = Snapshot32[]    */
    UINT32 SnapshotPalette[NTVDD_PALETTE_ENTRIES];
    INT   SnapshotWidth, SnapshotHeight, IsSnapshotValid;
    /* Raster split (s70, see NTVDD_FRAME): the frame-start and mid-frame palettes,
       the row each mid-frame entry applies from, and the frame stamps. `IsSnapshotSplit`
       is 0 for the ordinary one-palette frame, which keeps the fast paths. */
    UINT32 SnapshotPaletteBase[NTVDD_PALETTE_ENTRIES], SnapshotPaletteSplit[NTVDD_PALETTE_ENTRIES], SnapshotSplitFrame[NTVDD_PALETTE_ENTRIES];
    WORD SnapshotSplitRow[NTVDD_PALETTE_ENTRIES];
    /* ── s84 (user): WHAT DOES DRAWING THE PICTURE COST? Measured before anyone builds a
         windowed DirectDraw path: per present, split by path, in microseconds (QPC).
         `win` is the GDI path (window, or borderless fullscreen), `fs` exclusive
         DirectDraw. Includes the VSync wait when Force VSync is on (default off). */
    ULONG PresentWindowCount, PresentFullscreenCount;
    ULONGLONG PresentWindowUs, PresentFullscreenUs;
    ULONG PresentWindowMax, PresentFullscreenMax;
    /* s86: DID THE EXCLUSIVE FLIP WAIT FOR THE BLANK? Right after Flip, GetFlipStatus:
       `done` = the flip had already happened (nobody waited for a retrace), `pend` = it
       was queued for one. `mid` = the beam was mid-screen when we flipped. `drop` = a
       frame skipped because the previous flip was still queued. `bufs` = surfaces in
       the flip chain. `ourwait` = flips never waited, so we time them (see PresentFullscreen).
       `drv` = 1 when cfg\ddflip_driver.flag restores the old path (one back buffer,
       blocking driver-timed flip). */
    ULONG FlipDone, FlipPending, FlipMidScreen, FlipDropped;
    INT           IsFlipDriverTimed, FlipBuffers, IsFlipOurWait, FlipStreak;
    UINT32 SnapshotFrameNumber;
    INT      IsSnapshotSplit;
    UINT32 RowPalette[NTVDD_PALETTE_ENTRIES]; INT RowPaletteY;   /* the palette resolved for one row       */
    /* s81 (#138): a transient line of text drawn over the picture until `HintUntil`
       (GetTickCount ms) -- "press the Windows key to release the mouse" in fullscreen,
       where there is no status strip to say it. GDI path only. */
    PCSTR HintText;
    /* #154: a character-cell selection, inverted after each GDI present, in SOURCE
       frame pixels (IsSelection 0 = none) -- and where the last frame went, so the host
       can turn a click into a cell. */
    INT   IsSelection, SelectionX0, SelectionY0, SelectionX1, SelectionY1;
    INT   LastDestinationX, LastDestinationY, LastDestinationWidth, LastDestinationHeight, LastSourceWidth, LastSourceHeight;
    ULONG HintUntil;
    /* #217: the two Display settings, stored INVERTED so a zeroed presenter is the
       shipped default (messages shown, picture composed off-screen). The off-screen
       picture is a memory DC over a DIB section, kept while the client size holds. */
    INT   IsOsdOff, IsUnbuffered;
    INT   Tint;          /* #229: PRESENT_TINT_* (present_scale.h), 0 = Default */
    INT   IsModeVesa;    /* #228: the guest is in a VESA mode (Auto aspect = square pixels) */
    PVOID MemoryDc, MemoryBitmap, MemoryOld;
    INT   MemoryWidth, MemoryHeight;
} PRESENT_DDRAW, *PPRESENT_DDRAW;
typedef const PRESENT_DDRAW *PCPRESENT_DDRAW;

/* Bring up DirectDraw in windowed mode on `window`. 0 = ok, <0 = failed. */
INT  PresentDdrawInitialize(_Out_ PPRESENT_DDRAW presenter, _In_ HWND window);
VOID PresentDdrawShutdown(_Inout_ PPRESENT_DDRAW presenter);

/* Toggle exclusive fullscreen (recreates the mode-specific surfaces). */
INT  PresentDdrawSetFullscreen(_Inout_ PPRESENT_DDRAW presenter, _In_ INT isOn);

/* Snapshot a frame into the back-buffer -- call UNDER the bus lock (consistent
   copy while the V86 thread can't write the framebuffer). */
VOID PresentDdrawSnapshot(_Inout_ PPRESENT_DDRAW presenter, _In_opt_ PCNTVDD_FRAME frame);

/* Blit the snapshot to the screen, vsync'd -- call OUTSIDE the lock (the slow
   blit then never starves the V86 thread). */
VOID PresentDdrawPresent(_Inout_ PPRESENT_DDRAW presenter);

/* Snapshot + present in one call (for the standalone present_demo). */
VOID PresentDdrawFrame(_Inout_ PPRESENT_DDRAW presenter, _In_opt_ PCNTVDD_FRAME frame);

/* Serialise the current 8bpp snapshot to an indexed .bmp at `path` (occlusion-proof:
   reads presenter->Snapshot, not the screen). For headless/remote visual validation -- the host
   screenshots itself so a graphical run is verifiable off the SMB share without VNC.
   0 = ok, <0 = nothing valid to save / write failed. */
INT PresentDdrawSaveBmp(_In_ PPRESENT_DDRAW presenter, _In_ PCSTR path);

#endif /* NTVDMEX_PRESENT_DDRAW_H */
