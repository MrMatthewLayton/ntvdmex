/* present_ddraw.c -- see present_ddraw.h.  DirectDraw 7 frame presentation in
 * pure C (COBJMACROS), windowed + exclusive fullscreen, no-CRT (ddraw bound via
 * GetProcAddress, IID defined inline). */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <ddraw.h>
#include "present_ddraw.h"

/* IID_IDirectDraw7 = 15e65ec0-3b9c-11d2-b92f-00609797ea5b -- defined inline so we
   don't need to link dxguid (keeps the no-CRT minimal-import build). */
static const GUID IID_IDirectDraw7_local =
    { 0x15e65ec0, 0x3b9c, 0x11d2, { 0xb9,0x2f,0x00,0x60,0x97,0x97,0xea,0x5b } };

typedef HRESULT (WINAPI *PFN_DDCREATEEX)(GUID *, LPVOID *, REFIID, IUnknown *);

#define DD   ((LPDIRECTDRAW7)pd->dd)
#define SURF(p) ((LPDIRECTDRAWSURFACE7)(p))

static void rel_surf(void **s)
{ if (*s) { IDirectDrawSurface7_Release(SURF(*s)); *s = 0; } }

/* drop the mode-specific surfaces (primary/back/clipper); keep dd + fbsurf. */
static void release_mode_surfaces(present_ddraw *pd)
{
    if (pd->clipper) { IDirectDrawClipper_Release((LPDIRECTDRAWCLIPPER)pd->clipper); pd->clipper = 0; }
    pd->back = 0;                                  /* attached to primary's chain */
    rel_surf(&pd->primary);
}

static void restore_lost(present_ddraw *pd)
{
    if (pd->primary) IDirectDrawSurface7_Restore(SURF(pd->primary));
    if (pd->fbsurf)  IDirectDrawSurface7_Restore(SURF(pd->fbsurf));
}

/* (re)create the logical-size 32bpp offscreen surface we convert frames into. */
static int ensure_fbsurf(present_ddraw *pd, int w, int h)
{
    DDSURFACEDESC2 d; LPDIRECTDRAWSURFACE7 s = 0;
    if (pd->fbsurf && pd->fb_w == w && pd->fb_h == h) return 0;
    rel_surf(&pd->fbsurf);
    ZeroMemory(&d, sizeof d); d.dwSize = sizeof d;
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    d.dwWidth = (DWORD)w; d.dwHeight = (DWORD)h;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    if (FAILED(IDirectDraw7_CreateSurface(DD, &d, &s, NULL))) return -1;
    pd->fbsurf = s; pd->fb_w = w; pd->fb_h = h;
    return 0;
}

/* trailing-zero shift + bit-count of an RGB channel mask. */
static void mask_info(DWORD m, int *shift, int *bits)
{
    int s = 0, b = 0;
    if (m) { while (!(m & 1)) { m >>= 1; ++s; } while (m & 1) { m >>= 1; ++b; } }
    *shift = s; *bits = b;
}

/* convert one ntvdd_frame into the locked fbsurf, packing each pixel to the
   surface's ACTUAL pixel format (8bpp index -> ARGB -> 16/24/32bpp via masks).
   The offscreen surface inherits the desktop depth, which on XP/Cirrus is often
   16bpp -- writing fixed 32-bit pixels there shreds the row into stripes. */
static void upload(present_ddraw *pd, const ntvdd_frame *f)
{
    DDSURFACEDESC2 d; LPDIRECTDRAWSURFACE7 s = SURF(pd->fbsurf);
    DWORD bpp; int rsh, rb, gsh, gb, bsh, bb; int y, x;
    ZeroMemory(&d, sizeof d); d.dwSize = sizeof d;
    if (FAILED(IDirectDrawSurface7_Lock(s, NULL, &d,
                   DDLOCK_WAIT | DDLOCK_SURFACEMEMORYPTR, NULL)))
        return;
    bpp = d.ddpfPixelFormat.dwRGBBitCount;
    mask_info(d.ddpfPixelFormat.dwRBitMask, &rsh, &rb);
    mask_info(d.ddpfPixelFormat.dwGBitMask, &gsh, &gb);
    mask_info(d.ddpfPixelFormat.dwBBitMask, &bsh, &bb);
    for (y = 0; y < f->h; ++y) {
        BYTE *drow = (BYTE *)d.lpSurface + (size_t)y * d.lPitch;
        const uint8_t  *s8  = f->pixels + (size_t)y * f->stride;
        const uint32_t *s32 = (const uint32_t *)(f->pixels + (size_t)y * f->stride);
        for (x = 0; x < f->w; ++x) {
            uint32_t argb = (f->bpp == 8)
                ? (f->palette ? f->palette[s8[x]] : (0xFF000000u | (s8[x] * 0x010101u)))
                : s32[x];
            uint32_t r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
            if (bpp == 32) {
                ((DWORD *)drow)[x] = argb;
            } else if (bpp == 16 || bpp == 15) {
                ((WORD *)drow)[x] = (WORD)(((r >> (8 - rb)) << rsh)
                                         | ((g >> (8 - gb)) << gsh)
                                         | ((b >> (8 - bb)) << bsh));
            } else if (bpp == 24) {
                BYTE *p = drow + x * 3; p[0] = (BYTE)b; p[1] = (BYTE)g; p[2] = (BYTE)r;
            } else {                       /* 8bpp desktop: approximate by intensity */
                drow[x] = (BYTE)((r * 30 + g * 59 + b * 11) / 100);
            }
        }
    }
    IDirectDrawSurface7_Unlock(s, NULL);
}

static int setup_windowed(present_ddraw *pd)
{
    DDSURFACEDESC2 d; LPDIRECTDRAWSURFACE7 pr = 0; LPDIRECTDRAWCLIPPER cl = 0;
    ZeroMemory(&d, sizeof d); d.dwSize = sizeof d;
    d.dwFlags = DDSD_CAPS; d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    if (FAILED(IDirectDraw7_CreateSurface(DD, &d, &pr, NULL))) return -1;
    pd->primary = pr;
    if (SUCCEEDED(IDirectDraw7_CreateClipper(DD, 0, &cl, NULL))) {
        IDirectDrawClipper_SetHWnd(cl, 0, pd->hwnd);
        IDirectDrawSurface7_SetClipper(pr, cl);
        pd->clipper = cl;
    }
    return 0;
}

static int setup_fullscreen(present_ddraw *pd)
{
    DDSURFACEDESC2 d; DDSCAPS2 caps; LPDIRECTDRAWSURFACE7 pr = 0, bk = 0;
    if (FAILED(IDirectDraw7_SetDisplayMode(DD, (DWORD)pd->fs_w, (DWORD)pd->fs_h, 32, 0, 0)) &&
        FAILED(IDirectDraw7_SetDisplayMode(DD, (DWORD)pd->fs_w, (DWORD)pd->fs_h, 16, 0, 0)))
        return -1;          /* upload() packs to whatever depth the mode gives  */
    ZeroMemory(&d, sizeof d); d.dwSize = sizeof d;
    d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT; d.dwBackBufferCount = 1;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    if (FAILED(IDirectDraw7_CreateSurface(DD, &d, &pr, NULL))) return -1;
    pd->primary = pr;
    ZeroMemory(&caps, sizeof caps); caps.dwCaps = DDSCAPS_BACKBUFFER;
    if (FAILED(IDirectDrawSurface7_GetAttachedSurface(pr, &caps, &bk))) return -1;
    pd->back = bk;
    return 0;
}

int present_ddraw_init(present_ddraw *pd, HWND hwnd)
{
    PFN_DDCREATEEX create; LPDIRECTDRAW7 dd = 0;
    ZeroMemory(pd, sizeof *pd);
    pd->hwnd = hwnd; pd->fs_w = 640; pd->fs_h = 480;
    pd->ddmod = LoadLibraryA("ddraw.dll");
    if (!pd->ddmod) return -1;
    create = (PFN_DDCREATEEX)GetProcAddress(pd->ddmod, "DirectDrawCreateEx");
    if (!create) return -1;
    if (FAILED(create(NULL, (LPVOID *)&dd, &IID_IDirectDraw7_local, NULL))) return -1;
    pd->dd = dd;
    if (FAILED(IDirectDraw7_SetCooperativeLevel(DD, hwnd, DDSCL_NORMAL))) return -1;
    return setup_windowed(pd);
}

void present_ddraw_shutdown(present_ddraw *pd)
{
    if (!pd->dd) return;
    rel_surf(&pd->fbsurf);
    release_mode_surfaces(pd);
    if (pd->fullscreen) IDirectDraw7_RestoreDisplayMode(DD);
    IDirectDraw7_Release(DD); pd->dd = 0;
    if (pd->ddmod) { FreeLibrary(pd->ddmod); pd->ddmod = 0; }
}

int present_ddraw_set_fullscreen(present_ddraw *pd, int on)
{
    if (!pd->dd || on == pd->fullscreen) return 0;
    release_mode_surfaces(pd);
    if (on) {
        if (FAILED(IDirectDraw7_SetCooperativeLevel(DD, pd->hwnd,
                       DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT))) return -1;
        if (setup_fullscreen(pd)) return -1;
    } else {
        IDirectDraw7_RestoreDisplayMode(DD);
        if (FAILED(IDirectDraw7_SetCooperativeLevel(DD, pd->hwnd, DDSCL_NORMAL))) return -1;
        if (setup_windowed(pd)) return -1;
    }
    pd->fullscreen = on;
    return 0;
}

void present_ddraw_frame(present_ddraw *pd, const ntvdd_frame *f)
{
    if (!pd->dd || !f || !f->w || !f->h) return;
    if (ensure_fbsurf(pd, f->w, f->h)) return;
    upload(pd, f);
    if (pd->fullscreen) {
        RECT dst; dst.left = 0; dst.top = 0; dst.right = pd->fs_w; dst.bottom = pd->fs_h;
        if (IDirectDrawSurface7_Blt(SURF(pd->back), &dst, SURF(pd->fbsurf), NULL,
                                    DDBLT_WAIT, NULL) == DDERR_SURFACELOST) { restore_lost(pd); return; }
        if (IDirectDrawSurface7_Flip(SURF(pd->primary), NULL, DDFLIP_WAIT) == DDERR_SURFACELOST)
            restore_lost(pd);
    } else {
        RECT rc; POINT pt; pt.x = 0; pt.y = 0;
        GetClientRect(pd->hwnd, &rc);
        ClientToScreen(pd->hwnd, &pt);
        OffsetRect(&rc, pt.x, pt.y);
        if (IDirectDrawSurface7_Blt(SURF(pd->primary), &rc, SURF(pd->fbsurf), NULL,
                                    DDBLT_WAIT, NULL) == DDERR_SURFACELOST)
            restore_lost(pd);
    }
}

void present_ddraw_sink(void *ctx, const ntvdd_frame *f)
{ present_ddraw_frame((present_ddraw *)ctx, f); }
