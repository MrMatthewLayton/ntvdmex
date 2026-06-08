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
    /* double-buffer snapshot: filled under the caller's lock by _snapshot(),
       blitted (vsync'd) outside it by _present(). Removes the concurrent-write
       tearing of the live framebuffer. 8bpp + palette (all our frames). */
    uint8_t  snap[640 * 480];
    uint32_t snap_pal[256];
    int   snap_w, snap_h, snap_valid;
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

#endif /* NTVDMEX_PRESENT_DDRAW_H */
