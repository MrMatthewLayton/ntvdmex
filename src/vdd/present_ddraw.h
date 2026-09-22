/*
 * present_ddraw.h -- the DirectDraw presentation backend.  (M3 slice-3, ADR-0008)
 *
 * The mode-agnostic frame sink behind the VDD bus: the video VDD produces an
 * `ntvdd_frame` (text or graphics, palettised or ARGB) and this layer blits it
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

#include <windows.h>
#include "ntvdd.h"

/* Windowed mode presents via GDI StretchDIBits (cursor-friendly, expose-correct);
   exclusive fullscreen uses DirectDraw. The video blits into the client area
   above a host-drawn status bar PRESENT_STATUS_H pixels tall. */
#define PRESENT_STATUS_H 22

/* DirectDraw objects are held as void* so this header stays ddraw.h-free. */
typedef struct present_ddraw {
    HWND  hwnd;
    HMODULE ddmod;
    void *dd;           /* IDirectDraw7                                         */
    void *primary;      /* primary surface (desktop, or fullscreen flip chain)  */
    void *back;         /* fullscreen back buffer (NULL when windowed)          */
    void *clipper;      /* windowed clipper on hwnd (NULL when fullscreen)      */
    void *fbsurf;       /* logical-size 32bpp offscreen we convert frames into  */
    int   fb_w, fb_h;   /* current fbsurf size                                  */
    int   fullscreen;
    int   fs_w, fs_h;   /* exclusive-fullscreen mode size (default 640x480)     */
    int   status_h;     /* reserved bottom strip for the status bar (windowed)  */
    /* ── THE DISPLAY PAGE, AS FOUR FIELDS. ───────────────────────────────────
         Set by settings_apply() in the host; read on every present. All four
         default to what this host did before they existed, so a machine with no
         stored settings behaves exactly as it always has. */
    int   vsync;        /* 1 = time the blit near vblank (the historical default)*/
    int   mon_h, mon_hz;/* monitor lines + refresh, read once by wait_vblank (0 = not yet, -1 = unknown) */
    int   filter;       /* 0 = nearest, 1 = bilinear (GDI HALFTONE)             */
    int   aspect;       /* 1 = letterbox to 4:3 rather than fill the client     */
    int   scaler;       /* PRESENT_SCALER_* (present_scale.h)                   */
    /* ── FULLSCREEN (s64). ───────────────────────────────────────────────────
         By DEFAULT fullscreen is a borderless window drawn by gdi_present, because
         DirectDraw's stretch blt is filtered by the driver and there is no way to
         forbid that -- see the long note over gdi_present. So:
           fs_use_ddraw  0 = borderless window (default, SHARP)
                         1 = exclusive DirectDraw (ddrawfs.flag; kept for tearing)
           fs_mode_w/h   only consulted by the exclusive path; 0 = no mode change
           integer_scale snap the picture to whole pixel multiples -- BOTH windowed
                         and fullscreen, and ON BY DEFAULT. It used to be
                         `fs_integer`: fullscreen-only AND opt-in via a flag file
                         nobody had, so it never ran anywhere. A 320x200 frame
                         stretched into a 1680-wide window is 5.25x, which
                         nearest-neighbour renders as columns of 5 and 6 pixels that
                         shimmer as the view moves -- measured on the rig, and the
                         whole of "Doom's raycaster columns are too wide and
                         flickery, and the status bar is broken". An uneven pixel
                         grid is a defect, not a preference, so the opt-out is now
                         the flag: cfg\stretch.flag fills the area as before.     */
    int   fs_use_ddraw;
    int   fs_mode_w, fs_mode_h;
    int   integer_scale;
    /* double-buffer snapshot: filled under the caller's lock by _snapshot(),
       blitted (vsync'd) outside it by _present(). Removes the concurrent-write
       tearing of the live framebuffer. 8bpp + palette (all our frames). */
    /* ── THE SNAPSHOT NOW CARRIES DEPTH. (s74) VESA direct-colour modes hand us
         32-bit ARGB, not palette indices, so there are two buffers and `snap_bpp`
         says which one holds this frame. The 8bpp path is byte-for-byte what it
         always was -- Doom, Heretic, Hexen and ZAR all run through it and the
         release was imminent when this landed. Sized to the widest mode
         vesa_modes[] advertises: every mode we publish must be one we can DISPLAY,
         or the list is promising something the presenter drops on the floor. */
    uint8_t  snap[NTVDD_FRAME_MAXW * NTVDD_FRAME_MAXH];
    uint32_t snap32[NTVDD_FRAME_MAXW * NTVDD_FRAME_MAXH];   /* ARGB, when snap_bpp == 32 */
    uint8_t  snap_bpp;                  /* 8 = snap[] + snap_pal, 32 = snap32[]    */
    uint32_t snap_pal[256];
    int   snap_w, snap_h, snap_valid;
    /* Raster split (s70, see ntvdd_frame): the frame-start and mid-frame palettes,
       the row each mid-frame entry applies from, and the frame stamps. `snap_split`
       is 0 for the ordinary one-palette frame, which keeps the fast paths. */
    uint32_t snap_pal_base[256], snap_pal_split[256], snap_split_frame[256];
    uint16_t snap_split_row[256];
    uint32_t snap_frame_no;
    int      snap_split;
    uint32_t rowpal[256]; int rowpal_y;   /* the palette resolved for one row       */
} present_ddraw;

/* Bring up DirectDraw in windowed mode on `hwnd`. 0 = ok, <0 = failed. */
int  present_ddraw_init(present_ddraw *pd, HWND hwnd);
void present_ddraw_shutdown(present_ddraw *pd);

/* Toggle exclusive fullscreen (recreates the mode-specific surfaces). */
int  present_ddraw_set_fullscreen(present_ddraw *pd, int on);

/* Snapshot a frame into the back-buffer -- call UNDER the bus lock (consistent
   copy while the V86 thread can't write the framebuffer). */
void present_ddraw_snapshot(present_ddraw *pd, const ntvdd_frame *f);

/* Blit the snapshot to the screen, vsync'd -- call OUTSIDE the lock (the slow
   blit then never starves the V86 thread). */
void present_ddraw_present(present_ddraw *pd);

/* Snapshot + present in one call (for the standalone present_demo). */
void present_ddraw_frame(present_ddraw *pd, const ntvdd_frame *f);

/* Serialise the current 8bpp snapshot to an indexed .bmp at `path` (occlusion-proof:
   reads pd->snap, not the screen). For headless/remote visual validation -- the host
   screenshots itself so a graphical run is verifiable off the SMB share without VNC.
   0 = ok, <0 = nothing valid to save / write failed. */
int present_ddraw_save_bmp(present_ddraw *pd, const char *path);

#endif /* NTVDMEX_PRESENT_DDRAW_H */
