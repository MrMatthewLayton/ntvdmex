/* present_ddraw.c -- see present_ddraw.h.  WINDOWED mode presents via GDI
 * StretchDIBits (no cursor flicker, repaints correctly on expose, blits above
 * the status bar); EXCLUSIVE FULLSCREEN uses DirectDraw 7.  Pure C, no-CRT
 * (ddraw bound via GetProcAddress, IID inline). */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <ddraw.h>
#include "present_ddraw.h"
#include "present_scale.h"

/* IID_IDirectDraw7 = 15e65ec0-3b9c-11d2-b92f-00609797ea5b (inline -> no dxguid). */
static const GUID IID_IDirectDraw7_local =
    { 0x15e65ec0, 0x3b9c, 0x11d2, { 0xb9,0x2f,0x00,0x60,0x97,0x97,0xea,0x5b } };
typedef HRESULT (WINAPI *PFN_DDCREATEEX)(GUID *, LPVOID *, REFIID, IUnknown *);

#define DD   ((LPDIRECTDRAW7)pd->dd)
#define SURF(p) ((LPDIRECTDRAWSURFACE7)(p))

/* time a blit near the monitor's vertical blank (reduces tearing).
   ⚠ NOT WaitForVerticalBlank. On XP that call is a BUSY LOOP on the scanline register,
   and once presents were raised by the guest's frame (s73 Auto) it ran ~60 times a
   second for up to a frame each -- a whole core, on a two-core box, and the guest's
   own thread lost it: BOUNCEBX polls stalled 13.5 ms (dtmax was 2.4) and it dropped
   one frame in twenty.
   ► And not "Sleep(1) and look again" either: the blank is ~1.4 ms wide at 60 Hz and a
     1 ms sleep sailed past it half the time, then waited a WHOLE extra frame -- 1002
     presents for 1788 guest frames. So compute where the beam is, SLEEP to just short
     of the blank, and spin only the last millisecond. The monitor's line count and
     refresh are read once (GetDisplayMode / GetMonitorFrequency) and fall back to
     60 Hz if the driver will not say. Bounded: at most ~2 frames however it goes. */
static void wait_vblank(present_ddraw *pd)
{
    DWORD sl = 0, height, hz, per_us, k;
    HRESULT hr;
    if (!pd->vsync || !pd->dd) return;
    if (!pd->mon_h) {
        DDSURFACEDESC2 d; ZeroMemory(&d, sizeof d); d.dwSize = sizeof d;
        pd->mon_h = (SUCCEEDED(IDirectDraw7_GetDisplayMode(DD, &d)) && d.dwHeight) ? (int)d.dwHeight : 0;
        pd->mon_hz = (SUCCEEDED(IDirectDraw7_GetMonitorFrequency(DD, &hz)) && hz >= 40 && hz <= 240) ? (int)hz : 60;
        if (!pd->mon_h) pd->mon_h = -1;               /* asked once; unknown: spin-free fallback below */
    }
    height = pd->mon_h > 0 ? (DWORD)pd->mon_h : 0;
    per_us = 1000000u / (DWORD)(pd->mon_hz ? pd->mon_hz : 60);
    hr = IDirectDraw7_GetScanLine(DD, &sl);
    if (hr == DDERR_VERTICALBLANKINPROGRESS) return;      /* already in the blank: go   */
    if (hr != DD_OK) return;                              /* cannot tell: do not wait   */
    if (height && sl < height) {
        /* lines to go, as time; the blank starts at `height` (the CRTC counts on
           through it), so sleep for all of it but the last ~1.2 ms */
        DWORD us = (DWORD)((uint64_t)(height - sl) * per_us / (height * 21u / 20u));
        if (us > 1200) Sleep((us - 1200) / 1000);
    }
    for (k = 0; k < 3000; ++k) {                          /* the last stretch: ~1-2 ms */
        hr = IDirectDraw7_GetScanLine(DD, &sl);
        if (hr != DD_OK) return;                          /* VERTICALBLANKINPROGRESS = go */
        if (height && sl < 4) return;                     /* wrapped: just after the blank */
    }
}

/* ── THE SCALE2X TARGET, AS ONE STATIC BUFFER. ───────────────────────────────────
     640x480 doubled is 1.2 MB. It lives here rather than in present_ddraw so the
     struct stays something a caller can hold by value (present_demo does), and it
     is static rather than allocated because the present path runs on the UI thread
     at video rate and must never wait on the heap. */
static uint8_t s_scaled[1280 * 960];

/* The scanline mask: an 8x8 monochrome pattern, black on every other row. ANDed
   over the destination it darkens alternate PHYSICAL rows -- which is where a
   scanline lives, on the screen, not in the frame. Built once and kept: a brush
   per present would be a GDI object churned sixty times a second. */
static HBRUSH scanline_brush(void)
{
    static HBRUSH s_br = NULL;
    static const WORD rows[8] = { 0xFFFF, 0x0000, 0xFFFF, 0x0000,
                                  0xFFFF, 0x0000, 0xFFFF, 0x0000 };
    HBITMAP bm;
    if (s_br) return s_br;
    bm = CreateBitmap(8, 8, 1, 1, rows);
    if (!bm) return NULL;
    s_br = CreatePatternBrush(bm);
    DeleteObject(bm);
    return s_br;
}

/* ---- windowed AND (by default) fullscreen: GDI StretchDIBits ---------------
 * ── ★★★ THIS PATH IS SHARP AND THE DIRECTDRAW ONE IS NOT. (s64) ─────────────────
 *   The user settled it with a comparison no log could have produced: "even if I
 *   maximize the window on the desktop, with or without aspect ratio, the pixels stay
 *   sharp. In fullscreen they are NEVER sharp, regardless of what knobs I twiddle."
 *   Same frame, same aspect maths, same monitor, same non-integer 5.25x scale -- the
 *   only difference is WHICH BLITTER DRAWS IT:
 *     GDI StretchDIBits + COLORONCOLOR -> point sampling. Hard pixel edges.
 *     DirectDraw stretch Blt           -> the DRIVER decides, and it filters.
 *   DirectDraw offers no reliable way to demand point sampling on a stretch blt
 *   (DDBLTFX can ASK for arithmetic stretching; there is no flag that forbids it), so
 *   the fix is not to configure the stretch but to STOP ASKING FOR ONE.
 * ► So fullscreen is now a BORDERLESS WINDOW presented through here, not an exclusive
 *   DirectDraw mode. That also gets back the instant toggle (no mode switch to resync)
 *   and removes the fullscreen resolution knob entirely -- there is no mode to choose.
 *   The exclusive path is kept behind a file knob (ddrawfs.flag) rather than deleted,
 *   because "no tearing" was its original argument and that deserves a way back.
 * ⚠ TWO THINGS DIFFER IN FULLSCREEN and both are handled below: there is no status
 *   strip to reserve room for, and the fit is snapped to whole multiples. */
static const uint32_t *row_pal(present_ddraw *pd, int y);   /* fwd: with _snapshot */
static void gdi_present(present_ddraw *pd)
{
    HDC hdc; RECT rc; int cw, ch, dx, dy, dw, dh; unsigned i;
    const uint8_t *pix = pd->snap;
    int sw = pd->snap_w, sh = pd->snap_h;
    struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } bi;
    static uint32_t s_rgb32[640 * 480];   /* a raster-split frame, resolved per row */
    int split = pd->snap_split && sw <= 640 && sh <= 480;
    hdc = GetDC(pd->hwnd);
    if (!hdc) return;
    GetClientRect(pd->hwnd, &rc);
    cw = rc.right;
    ch = rc.bottom - (pd->fullscreen ? 0
                                     : (pd->status_h ? pd->status_h : PRESENT_STATUS_H));
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;

    /* Scale2x first: it is a property of the FRAME, so it happens before the
       stretch and the stretch then works from a source with twice the detail. */
    /* (A split frame skips scale2x: the doubled source would need a doubled
        32bpp buffer, and the combination is rare -- a 16-colour raster trick under a
        pixel-art scaler. The picture is still right, just not doubled.) */
    if (!split && present_scaler_doubles(pd->scaler) && sw > 0 && sh > 0
        && sw * 2 <= 1280 && sh * 2 <= 960) {
        present_scale2x_8(pd->snap, sw, sh, sw, s_scaled);
        pix = s_scaled; sw *= 2; sh *= 2;
    }

    ZeroMemory(&bi, sizeof bi);
    bi.h.biSize = sizeof(BITMAPINFOHEADER);
    bi.h.biWidth = (LONG)sw; bi.h.biHeight = -(LONG)sh;                  /* top-down */
    bi.h.biPlanes = 1; bi.h.biBitCount = 8; bi.h.biCompression = BI_RGB;
    for (i = 0; i < 256; ++i) {
        uint32_t a = pd->snap_pal[i];
        bi.c[i].rgbRed = (BYTE)(a >> 16); bi.c[i].rgbGreen = (BYTE)(a >> 8);
        bi.c[i].rgbBlue = (BYTE)a; bi.c[i].rgbReserved = 0;
    }
    if (split) {                       /* two palettes on one screen: resolve to 32bpp */
        int y, x;
        for (y = 0; y < sh; ++y) {
            const uint32_t *pal = row_pal(pd, y);
            const uint8_t *srow = pd->snap + (size_t)y * sw;
            uint32_t *drow = s_rgb32 + (size_t)y * sw;
            for (x = 0; x < sw; ++x) drow[x] = pal[srow[x]] & 0x00FFFFFFu;
        }
        bi.h.biBitCount = 32; pix = (const uint8_t *)s_rgb32;
    }
    /* ⚠ THE INTEGER FIT USES sw/sh, WHICH ARE POST-SCALE2X. That is deliberate: the
         blit source is what has to divide into the destination. Snapping to a multiple
         of the ORIGINAL 320 while blitting a 640-wide scale2x source would give 2.5x
         and put the uneven pixels straight back. */
    if (pd->fullscreen && pd->fs_integer)
        present_fit_int(cw, ch, sw, sh, pd->aspect, &dx, &dy, &dw, &dh);
    else
        present_fit(cw, ch, pd->aspect, &dx, &dy, &dw, &dh);
    wait_vblank(pd);
    /* Letterboxing leaves bars, and they must be PAINTED: the client area is ours
       (WM_ERASEBKGND returns 1), so whatever was there last -- the previous mode's
       frame, at the previous size -- would otherwise stay on screen forever. */
    if (dx || dy) {
        RECT full; full.left = 0; full.top = 0; full.right = cw; full.bottom = ch;
        FillRect(hdc, &full, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    SetStretchBltMode(hdc, pd->filter ? HALFTONE : COLORONCOLOR);
    if (pd->filter) SetBrushOrgEx(hdc, 0, 0, NULL);   /* HALFTONE requires this */
    StretchDIBits(hdc, dx, dy, dw, dh, 0, 0, sw, sh,
                  pix, (BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY);
    if (present_scaler_scanlines(pd->scaler)) {
        HBRUSH br = scanline_brush();
        if (br) {
            HGDIOBJ old = SelectObject(hdc, br);
            PatBlt(hdc, dx, dy, dw, dh, 0x00A000C9L);    /* PATAND */
            SelectObject(hdc, old);
        }
    }
    ReleaseDC(pd->hwnd, hdc);
}

/* ---- fullscreen: DirectDraw 7 ------------------------------------------- */
static void rel_surf(void **s)
{ if (*s) { IDirectDrawSurface7_Release(SURF(*s)); *s = 0; } }

static void fs_teardown(present_ddraw *pd)
{
    pd->back = 0;
    rel_surf(&pd->fbsurf);
    pd->fb_w = pd->fb_h = 0;
    rel_surf(&pd->primary);
}

/* ── ★★ FULLSCREEN KEEPS THE DESKTOP'S OWN MODE. (s64) ───────────────────────────────
     This used to `SetDisplayMode(640, 480)` and then stretch the frame across all of
     it, and BOTH halves of that were wrong on a modern panel:
       * The monitor is not 4:3. Handed a 640x480 signal, an LCD stretches it across a
         16:9 panel itself -- so the picture came out wide no matter what our aspect
         setting said, and nothing we did to the pixels could have corrected it. That
         is the "pixels get stretched regardless of the setting" report, and the stretch
         was happening in the DISPLAY, downstream of everything we control.
       * fs_present then ignored pd->aspect completely, so even the part we DID control
         was filling rather than fitting.
     Taking the desktop's current mode fixes both: the panel shows its native signal
     1:1, and we letterbox/pillarbox inside it with present_fit -- the same function the
     windowed path uses, so the two modes finally agree about what a setting means.
   ⚠ NO SetDisplayMode AT ALL now. Exclusive mode does not require one, and not calling
     it also means Alt+Enter no longer makes the monitor resync twice per toggle.
   ⚠ THE COST IS THAT THE DESTINATION IS NOW BIG. A software nearest-neighbour loop over
     1440x1080 is ~1.5M pixels a frame and this host cannot afford that, so the picture
     goes through an OFFSCREEN SURFACE and a hardware stretch Blt: we convert only
     snap_w x snap_h (64k pixels for mode 13h) and the GPU does the scaling. fs_present
     keeps the old software loop as a fallback for a driver that refuses the blt. */
static int fs_setup(present_ddraw *pd)
{
    DDSURFACEDESC2 d; DDSCAPS2 caps; LPDIRECTDRAWSURFACE7 pr = 0, bk = 0, fb = 0;

    /* ★ AN EXPLICIT MODE IS TRIED FIRST, AND ONLY IF THE USER ASKED FOR ONE. Try 32bpp
         then 16, the same pair this always used; if the display refuses both, fall
         through to the desktop mode rather than failing the toggle outright. */
    pd->fs_w = 0; pd->fs_h = 0;
    if (pd->fs_mode_w > 0 && pd->fs_mode_h > 0) {
        if (SUCCEEDED(IDirectDraw7_SetDisplayMode(DD, (DWORD)pd->fs_mode_w,
                                                  (DWORD)pd->fs_mode_h, 32, 0, 0)) ||
            SUCCEEDED(IDirectDraw7_SetDisplayMode(DD, (DWORD)pd->fs_mode_w,
                                                  (DWORD)pd->fs_mode_h, 16, 0, 0))) {
            pd->fs_w = pd->fs_mode_w; pd->fs_h = pd->fs_mode_h;
        }
    }
    if (!pd->fs_w) {
        ZeroMemory(&d, sizeof d); d.dwSize = sizeof d;
        if (SUCCEEDED(IDirectDraw7_GetDisplayMode(DD, &d)) && d.dwWidth && d.dwHeight) {
            pd->fs_w = (int)d.dwWidth;
            pd->fs_h = (int)d.dwHeight;
        } else {
            /* Could not ask. Fall back to the old behaviour rather than guessing -- a
               forced 640x480 is worse than the desktop mode but better than no picture. */
            pd->fs_w = 640; pd->fs_h = 480;
            if (FAILED(IDirectDraw7_SetDisplayMode(DD, 640, 480, 32, 0, 0)) &&
                FAILED(IDirectDraw7_SetDisplayMode(DD, 640, 480, 16, 0, 0)))
                return -1;
        }
    }

    ZeroMemory(&d, sizeof d); d.dwSize = sizeof d;
    d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT; d.dwBackBufferCount = 1;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    if (FAILED(IDirectDraw7_CreateSurface(DD, &d, &pr, NULL))) return -1;
    pd->primary = pr;
    ZeroMemory(&caps, sizeof caps); caps.dwCaps = DDSCAPS_BACKBUFFER;
    if (FAILED(IDirectDrawSurface7_GetAttachedSurface(pr, &caps, &bk))) return -1;
    pd->back = bk;

    /* The staging surface the guest frame is converted into, at FRAME size. Video
       memory if the driver will give it (the stretch blt is then card-to-card),
       system memory if not. Failing both is not fatal -- fs_present falls back to
       the software path, which needs no surface at all. */
    ZeroMemory(&d, sizeof d); d.dwSize = sizeof d;
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    d.dwWidth = 640; d.dwHeight = 480;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
    if (FAILED(IDirectDraw7_CreateSurface(DD, &d, &fb, NULL))) {
        ZeroMemory(&d, sizeof d); d.dwSize = sizeof d;
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        d.dwWidth = 640; d.dwHeight = 480;
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        if (FAILED(IDirectDraw7_CreateSurface(DD, &d, &fb, NULL))) fb = 0;
    }
    pd->fbsurf = fb;
    pd->fb_w = fb ? 640 : 0; pd->fb_h = fb ? 480 : 0;
    return 0;
}

/* convert an ntvdd_frame into the locked back buffer, packed to its depth. */
static void mask_info(DWORD m, int *shift, int *bits)
{ int s=0,b=0; if(m){while(!(m&1)){m>>=1;++s;}while(m&1){m>>=1;++b;}} *shift=s; *bits=b; }

/* Write one snapshot pixel, packed to whatever depth the locked surface is. */
static void put_px(BYTE *drow, int dx, DWORD bpp, uint32_t argb,
                   int rsh, int rb, int gsh, int gb, int bsh, int bb)
{
    uint32_t r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    if (bpp == 32) ((DWORD *)drow)[dx] = argb;
    else if (bpp == 16 || bpp == 15)
        ((WORD *)drow)[dx] = (WORD)(((r>>(8-rb))<<rsh)|((g>>(8-gb))<<gsh)|((b>>(8-bb))<<bsh));
    else if (bpp == 24) { BYTE *p = drow + dx*3; p[0]=(BYTE)b; p[1]=(BYTE)g; p[2]=(BYTE)r; }
    else drow[dx] = (BYTE)((r*30 + g*59 + b*11) / 100);
}

/* Convert the snapshot 1:1 into the staging surface's top-left corner. No scaling
   here on purpose -- that is the GPU's job in fs_present. 0 = ok. */
static int fs_stage(present_ddraw *pd, LPDIRECTDRAWSURFACE7 fb)
{
    DDSURFACEDESC2 d;
    DWORD bpp; int rsh,rb,gsh,gb,bsh,bb; int y, x;
    ZeroMemory(&d, sizeof d); d.dwSize = sizeof d;
    if (FAILED(IDirectDrawSurface7_Lock(fb, NULL, &d,
                                        DDLOCK_WAIT|DDLOCK_SURFACEMEMORYPTR, NULL)))
        return -1;
    bpp = d.ddpfPixelFormat.dwRGBBitCount;
    mask_info(d.ddpfPixelFormat.dwRBitMask, &rsh, &rb);
    mask_info(d.ddpfPixelFormat.dwGBitMask, &gsh, &gb);
    mask_info(d.ddpfPixelFormat.dwBBitMask, &bsh, &bb);
    for (y = 0; y < pd->snap_h; ++y) {
        BYTE *drow = (BYTE *)d.lpSurface + (size_t)y * d.lPitch;
        const uint8_t *srow = pd->snap + (size_t)y * pd->snap_w;
        const uint32_t *pal = row_pal(pd, y);
        for (x = 0; x < pd->snap_w; ++x)
            put_px(drow, x, bpp, pal[srow[x]], rsh,rb,gsh,gb,bsh,bb);
    }
    IDirectDrawSurface7_Unlock(fb, NULL);
    return 0;
}

/* The fallback: nearest-neighbour straight into the back buffer, honouring the same
   fitted rectangle. Only reached if the driver refuses a stretch blt. */
static void fs_present_sw(present_ddraw *pd, int fx, int fy, int fw, int fh)
{
    DDSURFACEDESC2 d; LPDIRECTDRAWSURFACE7 bk = SURF(pd->back);
    DWORD bpp; int rsh,rb,gsh,gb,bsh,bb; int y, x, dy, dx;
    ZeroMemory(&d, sizeof d); d.dwSize = sizeof d;
    if (IDirectDrawSurface7_Lock(bk, NULL, &d, DDLOCK_WAIT|DDLOCK_SURFACEMEMORYPTR, NULL)
            == DDERR_SURFACELOST) { IDirectDrawSurface7_Restore(SURF(pd->primary)); return; }
    bpp = d.ddpfPixelFormat.dwRGBBitCount;
    mask_info(d.ddpfPixelFormat.dwRBitMask, &rsh, &rb);
    mask_info(d.ddpfPixelFormat.dwGBitMask, &gsh, &gb);
    mask_info(d.ddpfPixelFormat.dwBBitMask, &bsh, &bb);
    for (dy = 0; dy < pd->fs_h; ++dy) {
        BYTE *drow = (BYTE *)d.lpSurface + (size_t)dy * d.lPitch;
        int insidey = (dy >= fy && dy < fy + fh);
        y = insidey ? (dy - fy) * pd->snap_h / fh : 0;
        for (dx = 0; dx < pd->fs_w; ++dx) {
            /* The bars are part of the picture: this is a FLIP CHAIN, so a pixel we
               do not write keeps whatever the buffer held two frames ago. */
            if (!insidey || dx < fx || dx >= fx + fw) {
                put_px(drow, dx, bpp, 0, rsh,rb,gsh,gb,bsh,bb);
                continue;
            }
            x = (dx - fx) * pd->snap_w / fw;
            put_px(drow, dx, bpp, row_pal(pd, y)[pd->snap[(size_t)y * pd->snap_w + x]],
                   rsh,rb,gsh,gb,bsh,bb);
        }
    }
    IDirectDrawSurface7_Unlock(bk, NULL);
}

static void fs_present(present_ddraw *pd)
{
    LPDIRECTDRAWSURFACE7 bk = SURF(pd->back), fb = SURF(pd->fbsurf);
    int fx, fy, fw, fh, done = 0;
    if (!bk) return;
    /* ★ THE SAME FIT THE WINDOW USES. present_fit centres an on-aspect rectangle in
         the destination and returns the whole destination for "None" (fill). One
         function for both modes is the point: a setting that meant one thing windowed
         and another fullscreen is exactly the bug being fixed.
       ★ ...unless sharp pixels were asked for, in which case each axis snaps to a
         whole multiple of the FRAME -- which is why this needs snap_w/snap_h and the
         windowed caller does not. See present_fit_int. */
    if (pd->fs_integer)
        present_fit_int(pd->fs_w, pd->fs_h, pd->snap_w, pd->snap_h, pd->aspect,
                        &fx, &fy, &fw, &fh);
    else
        present_fit(pd->fs_w, pd->fs_h, pd->aspect, &fx, &fy, &fw, &fh);

    if (fb && pd->snap_w > 0 && pd->snap_h > 0 && fs_stage(pd, fb) == 0) {
        RECT src, dst;
        src.left = 0; src.top = 0;
        src.right = pd->snap_w; src.bottom = pd->snap_h;
        dst.left = fx; dst.top = fy; dst.right = fx + fw; dst.bottom = fy + fh;
        if (fx || fy) {                          /* letterboxed -> clear the bars */
            DDBLTFX bfx;
            ZeroMemory(&bfx, sizeof bfx); bfx.dwSize = sizeof bfx; bfx.dwFillColor = 0;
            IDirectDrawSurface7_Blt(bk, NULL, NULL, NULL,
                                    DDBLT_COLORFILL | DDBLT_WAIT, &bfx);
        }
        done = SUCCEEDED(IDirectDrawSurface7_Blt(bk, &dst, fb, &src, DDBLT_WAIT, NULL));
    }
    if (!done) fs_present_sw(pd, fx, fy, fw, fh);

    if (IDirectDrawSurface7_Flip(SURF(pd->primary), NULL, DDFLIP_WAIT) == DDERR_SURFACELOST)
        IDirectDrawSurface7_Restore(SURF(pd->primary));
}

/* ---- public API --------------------------------------------------------- */
int present_ddraw_init(present_ddraw *pd, HWND hwnd)
{
    PFN_DDCREATEEX create; LPDIRECTDRAW7 dd = 0;
    ZeroMemory(pd, sizeof *pd);
    pd->hwnd = hwnd; pd->fs_w = 640; pd->fs_h = 480; pd->status_h = PRESENT_STATUS_H;
    /* The struct was just zeroed, and zero is the WRONG default for exactly one of
       these: this host has always waited for vblank. The other three (nearest, fill
       the client, no scaler) are what it has always done, so zero is right. */
    pd->vsync = 1;
    pd->ddmod = LoadLibraryA("ddraw.dll");          /* for fullscreen (optional)   */
    if (pd->ddmod) {
        create = (PFN_DDCREATEEX)GetProcAddress(pd->ddmod, "DirectDrawCreateEx");
        if (create && SUCCEEDED(create(NULL, (LPVOID *)&dd, &IID_IDirectDraw7_local, NULL))) {
            pd->dd = dd;
            IDirectDraw7_SetCooperativeLevel(DD, hwnd, DDSCL_NORMAL);
        }
    }
    return 0;                                        /* windowed (GDI) always works */
}

void present_ddraw_shutdown(present_ddraw *pd)
{
    if (pd->fullscreen && pd->dd) { fs_teardown(pd); IDirectDraw7_RestoreDisplayMode(DD); }
    if (pd->dd) { IDirectDraw7_Release(DD); pd->dd = 0; }
    if (pd->ddmod) { FreeLibrary(pd->ddmod); pd->ddmod = 0; }
}

int present_ddraw_set_fullscreen(present_ddraw *pd, int on)
{
    if (on == pd->fullscreen) return 0;
    /* ★ THE DEFAULT IS A BORDERLESS WINDOW, NOT AN EXCLUSIVE MODE. See the long note
         over gdi_present: exclusive DirectDraw is where the blurring comes from. The
         caller has already made the window chromeless and screen-sized, so all that is
         left to do is record the state and let gdi_present do what it does windowed --
         which the user has demonstrated is sharp. */
    if (!pd->fs_use_ddraw) {
        pd->fullscreen = on;
        InvalidateRect(pd->hwnd, NULL, TRUE);
        return 0;
    }
    if (!pd->dd) return -1;                          /* no DirectDraw -> stay windowed */
    if (on) {
        if (FAILED(IDirectDraw7_SetCooperativeLevel(DD, pd->hwnd,
                       DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT))) return -1;
        if (fs_setup(pd)) { IDirectDraw7_SetCooperativeLevel(DD, pd->hwnd, DDSCL_NORMAL); return -1; }
    } else {
        fs_teardown(pd);
        IDirectDraw7_RestoreDisplayMode(DD);
        IDirectDraw7_SetCooperativeLevel(DD, pd->hwnd, DDSCL_NORMAL);
        InvalidateRect(pd->hwnd, NULL, TRUE);
    }
    pd->fullscreen = on;
    return 0;
}

/* copy the live frame into the back-buffer (call under the bus lock). */
void present_ddraw_snapshot(present_ddraw *pd, const ntvdd_frame *f)
{
    int y;
    if (!f || !f->w || !f->h || !f->pixels || f->bpp != 8 || f->w > 640 || f->h > 480) {
        pd->snap_valid = 0; return;
    }
    for (y = 0; y < (int)f->h; ++y)
        CopyMemory(pd->snap + (size_t)y * f->w, f->pixels + (size_t)y * f->stride, f->w);
    if (f->palette) CopyMemory(pd->snap_pal, f->palette, 256 * sizeof(uint32_t));
    pd->snap_split = ntvdd_frame_has_split(f);
    if (pd->snap_split) {
        CopyMemory(pd->snap_pal_base,   f->palette_base,  256 * sizeof(uint32_t));
        CopyMemory(pd->snap_pal_split,  f->palette_split, 256 * sizeof(uint32_t));
        CopyMemory(pd->snap_split_frame,f->split_frame,   256 * sizeof(uint32_t));
        CopyMemory(pd->snap_split_row,  f->split_row,     256 * sizeof(uint16_t));
        pd->snap_frame_no = f->frame_no;
    }
    pd->rowpal_y = -1;
    pd->snap_w = f->w; pd->snap_h = f->h; pd->snap_valid = 1;
}

/* The palette for source row `y` of the snapshot: the single palette on an ordinary
   frame; on a raster-split frame the per-entry resolution of ntvdd_frame_pal_at,
   computed once per row. */
static const uint32_t *row_pal(present_ddraw *pd, int y)
{
    ntvdd_frame f; unsigned i;
    if (!pd->snap_split) return pd->snap_pal;
    if (pd->rowpal_y == y) return pd->rowpal;
    f.palette = pd->snap_pal; f.palette_base = pd->snap_pal_base;
    f.palette_split = pd->snap_pal_split; f.split_row = pd->snap_split_row;
    f.split_frame = pd->snap_split_frame; f.frame_no = pd->snap_frame_no;
    for (i = 0; i < 256; ++i) pd->rowpal[i] = ntvdd_frame_pal_at(&f, (unsigned)y, i);
    pd->rowpal_y = y;
    return pd->rowpal;
}

/* blit the back-buffer to the screen, vsync'd (call outside the lock). */
void present_ddraw_present(present_ddraw *pd)
{
    if (!pd->snap_valid) return;
    /* `back` is only non-NULL when the exclusive path actually set up, so this also
       covers "we asked for DirectDraw fullscreen and it refused" -- which must fall
       back to drawing something rather than to drawing nothing. */
    if (pd->fullscreen && pd->dd && pd->back) fs_present(pd);
    else                                      gdi_present(pd);
}

void present_ddraw_frame(present_ddraw *pd, const ntvdd_frame *f)
{ present_ddraw_snapshot(pd, f); present_ddraw_present(pd); }

/* Save the current 8bpp snapshot as an indexed .bmp at `path`. This is OCCLUSION-PROOF
   -- it serialises our own back-buffer (pd->snap + pd->snap_pal), not the on-screen
   pixels -- so the host can screenshot ITSELF for headless/remote visual validation:
   a graphical run (mode 13h, Skyroads, the PM demos) is verified by reading the .bmp
   off the SMB share instead of via VNC or a physical monitor (VNC capture is dead on
   the real box). Call on the UI thread (which owns snap) or under the bus lock.
   Returns 0 on success, <0 if there's nothing valid to save or the write failed. */
static void st_le16(BYTE *p, unsigned v) { p[0]=(BYTE)v; p[1]=(BYTE)(v>>8); }
static void st_le32(BYTE *p, DWORD v)    { p[0]=(BYTE)v; p[1]=(BYTE)(v>>8); p[2]=(BYTE)(v>>16); p[3]=(BYTE)(v>>24); }

int present_ddraw_save_bmp(present_ddraw *pd, const char *path)
{
    int w = pd->snap_w, h = pd->snap_h, x, y;
    DWORD rowb, imgsz, off, wr;
    HANDLE hf;
    BYTE fh[14], ih[40];
    static BYTE pal[256 * 4];
    static BYTE row[640 + 4];
    int split = pd->snap_split;
    static BYTE row24[640 * 3 + 4];
    if (!pd->snap_valid || w <= 0 || h <= 0 || w > 640 || h > 480) return -1;
    /* A raster-split frame cannot be an 8bpp BMP (one palette per file), so it is
       written as 24bpp with every row resolved. Ordinary frames stay 8bpp: the
       oracle tools compare palette INDICES and must keep them. */
    rowb  = split ? (((DWORD)w * 3 + 3) & ~3u) : (((DWORD)w + 3) & ~3u);
    imgsz = rowb * (DWORD)h;
    off   = split ? 14 + 40 : 14 + 40 + 256 * 4;

    /* BITMAPFILEHEADER */
    fh[0] = 'B'; fh[1] = 'M';
    st_le32(fh + 2, off + imgsz);               /* whole file size                   */
    st_le16(fh + 6, 0); st_le16(fh + 8, 0);
    st_le32(fh + 10, off);
    /* BITMAPINFOHEADER (positive height -> bottom-up rows) */
    st_le32(ih + 0, 40);
    st_le32(ih + 4, (DWORD)w);  st_le32(ih + 8, (DWORD)h);
    st_le16(ih + 12, 1);        st_le16(ih + 14, (WORD)(split ? 24 : 8)); /* 1 plane */
    st_le32(ih + 16, 0);        st_le32(ih + 20, imgsz);   /* BI_RGB                 */
    st_le32(ih + 24, 2835);     st_le32(ih + 28, 2835);    /* ~72 dpi                */
    st_le32(ih + 32, split ? 0 : 256); st_le32(ih + 36, 0); /* colours used          */
    /* palette: snap_pal is 0xAARRGGBB -> RGBQUAD {B,G,R,0} */
    for (x = 0; x < 256; ++x) {
        uint32_t a = pd->snap_pal[x];
        pal[x*4+0] = (BYTE)(a & 0xFF);
        pal[x*4+1] = (BYTE)((a >> 8) & 0xFF);
        pal[x*4+2] = (BYTE)((a >> 16) & 0xFF);
        pal[x*4+3] = 0;
    }
    hf = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return -1;
    WriteFile(hf, fh, 14, &wr, NULL);
    WriteFile(hf, ih, 40, &wr, NULL);
    if (!split) WriteFile(hf, pal, sizeof pal, &wr, NULL);
    for (y = h - 1; y >= 0; --y) {              /* BMP is bottom-up                  */
        if (split) {
            const uint32_t *rp = row_pal(pd, y);
            for (x = 0; x < w; ++x) {
                uint32_t a = rp[pd->snap[(size_t)y * (size_t)w + (size_t)x]];
                row24[x*3+0] = (BYTE)(a & 0xFF); row24[x*3+1] = (BYTE)((a >> 8) & 0xFF);
                row24[x*3+2] = (BYTE)((a >> 16) & 0xFF);
            }
            for (x = w * 3; x < (int)rowb; ++x) row24[x] = 0;
            WriteFile(hf, row24, rowb, &wr, NULL);
            continue;
        }
        for (x = 0; x < w; ++x) row[x] = pd->snap[(size_t)y * (size_t)w + (size_t)x];
        for (x = w; x < (int)rowb; ++x) row[x] = 0;
        WriteFile(hf, row, rowb, &wr, NULL);
    }
    CloseHandle(hf);
    return 0;
}
