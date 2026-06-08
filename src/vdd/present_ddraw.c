/* present_ddraw.c -- see present_ddraw.h.  WINDOWED mode presents via GDI
 * StretchDIBits (no cursor flicker, repaints correctly on expose, blits above
 * the status bar); EXCLUSIVE FULLSCREEN uses DirectDraw 7.  Pure C, no-CRT
 * (ddraw bound via GetProcAddress, IID inline). */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <ddraw.h>
#include "present_ddraw.h"

/* IID_IDirectDraw7 = 15e65ec0-3b9c-11d2-b92f-00609797ea5b (inline -> no dxguid). */
static const GUID IID_IDirectDraw7_local =
    { 0x15e65ec0, 0x3b9c, 0x11d2, { 0xb9,0x2f,0x00,0x60,0x97,0x97,0xea,0x5b } };
typedef HRESULT (WINAPI *PFN_DDCREATEEX)(GUID *, LPVOID *, REFIID, IUnknown *);

#define DD   ((LPDIRECTDRAW7)pd->dd)
#define SURF(p) ((LPDIRECTDRAWSURFACE7)(p))

/* time a blit near the monitor's vertical blank (reduces tearing). */
static void wait_vblank(present_ddraw *pd)
{
    if (pd->dd) IDirectDraw7_WaitForVerticalBlank(DD, DDWAITVB_BLOCKBEGIN, NULL);
}

/* ---- windowed: GDI StretchDIBits from the snapshot ---------------------- */
static void gdi_present(present_ddraw *pd)
{
    HDC hdc; RECT rc; int cw, ch; unsigned i;
    struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } bi;
    hdc = GetDC(pd->hwnd);
    if (!hdc) return;
    GetClientRect(pd->hwnd, &rc);
    cw = rc.right; ch = rc.bottom - (pd->status_h ? pd->status_h : PRESENT_STATUS_H);
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    ZeroMemory(&bi, sizeof bi);
    bi.h.biSize = sizeof(BITMAPINFOHEADER);
    bi.h.biWidth = (LONG)pd->snap_w; bi.h.biHeight = -(LONG)pd->snap_h;  /* top-down */
    bi.h.biPlanes = 1; bi.h.biBitCount = 8; bi.h.biCompression = BI_RGB;
    for (i = 0; i < 256; ++i) {
        uint32_t a = pd->snap_pal[i];
        bi.c[i].rgbRed = (BYTE)(a >> 16); bi.c[i].rgbGreen = (BYTE)(a >> 8);
        bi.c[i].rgbBlue = (BYTE)a; bi.c[i].rgbReserved = 0;
    }
    wait_vblank(pd);
    SetStretchBltMode(hdc, COLORONCOLOR);
    StretchDIBits(hdc, 0, 0, cw, ch, 0, 0, pd->snap_w, pd->snap_h,
                  pd->snap, (BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(pd->hwnd, hdc);
}

/* ---- fullscreen: DirectDraw 7 ------------------------------------------- */
static void rel_surf(void **s)
{ if (*s) { IDirectDrawSurface7_Release(SURF(*s)); *s = 0; } }

static void fs_teardown(present_ddraw *pd)
{
    pd->back = 0;
    rel_surf(&pd->primary);
}

static int fs_setup(present_ddraw *pd)
{
    DDSURFACEDESC2 d; DDSCAPS2 caps; LPDIRECTDRAWSURFACE7 pr = 0, bk = 0;
    if (FAILED(IDirectDraw7_SetDisplayMode(DD, (DWORD)pd->fs_w, (DWORD)pd->fs_h, 32, 0, 0)) &&
        FAILED(IDirectDraw7_SetDisplayMode(DD, (DWORD)pd->fs_w, (DWORD)pd->fs_h, 16, 0, 0)))
        return -1;
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

/* convert an ntvdd_frame into the locked back buffer, packed to its depth. */
static void mask_info(DWORD m, int *shift, int *bits)
{ int s=0,b=0; if(m){while(!(m&1)){m>>=1;++s;}while(m&1){m>>=1;++b;}} *shift=s; *bits=b; }

static void fs_present(present_ddraw *pd)
{
    DDSURFACEDESC2 d; LPDIRECTDRAWSURFACE7 bk = SURF(pd->back);
    DWORD bpp; int rsh,rb,gsh,gb,bsh,bb; int y, x, dy, dx;
    if (!bk) return;
    ZeroMemory(&d, sizeof d); d.dwSize = sizeof d;
    if (IDirectDrawSurface7_Lock(bk, NULL, &d, DDLOCK_WAIT|DDLOCK_SURFACEMEMORYPTR, NULL)
            == DDERR_SURFACELOST) { IDirectDrawSurface7_Restore(SURF(pd->primary)); return; }
    bpp = d.ddpfPixelFormat.dwRGBBitCount;
    mask_info(d.ddpfPixelFormat.dwRBitMask, &rsh, &rb);
    mask_info(d.ddpfPixelFormat.dwGBitMask, &gsh, &gb);
    mask_info(d.ddpfPixelFormat.dwBBitMask, &bsh, &bb);
    /* nearest-neighbour stretch of the snapshot into fs_w x fs_h */
    for (dy = 0; dy < pd->fs_h; ++dy) {
        BYTE *drow = (BYTE *)d.lpSurface + (size_t)dy * d.lPitch;
        y = dy * pd->snap_h / pd->fs_h;
        for (dx = 0; dx < pd->fs_w; ++dx) {
            uint32_t argb, r, g, b;
            x = dx * pd->snap_w / pd->fs_w;
            argb = pd->snap_pal[pd->snap[(size_t)y * pd->snap_w + x]];
            r=(argb>>16)&0xFF; g=(argb>>8)&0xFF; b=argb&0xFF;
            if (bpp == 32) ((DWORD*)drow)[dx] = argb;
            else if (bpp == 16 || bpp == 15)
                ((WORD*)drow)[dx] = (WORD)(((r>>(8-rb))<<rsh)|((g>>(8-gb))<<gsh)|((b>>(8-bb))<<bsh));
            else if (bpp == 24) { BYTE *p=drow+dx*3; p[0]=(BYTE)b; p[1]=(BYTE)g; p[2]=(BYTE)r; }
            else drow[dx] = (BYTE)((r*30+g*59+b*11)/100);
        }
    }
    IDirectDrawSurface7_Unlock(bk, NULL);
    if (IDirectDrawSurface7_Flip(SURF(pd->primary), NULL, DDFLIP_WAIT) == DDERR_SURFACELOST)
        IDirectDrawSurface7_Restore(SURF(pd->primary));
}

/* ---- public API --------------------------------------------------------- */
int present_ddraw_init(present_ddraw *pd, HWND hwnd)
{
    PFN_DDCREATEEX create; LPDIRECTDRAW7 dd = 0;
    ZeroMemory(pd, sizeof *pd);
    pd->hwnd = hwnd; pd->fs_w = 640; pd->fs_h = 480; pd->status_h = PRESENT_STATUS_H;
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
    pd->snap_w = f->w; pd->snap_h = f->h; pd->snap_valid = 1;
}

/* blit the back-buffer to the screen, vsync'd (call outside the lock). */
void present_ddraw_present(present_ddraw *pd)
{
    if (!pd->snap_valid) return;
    if (pd->fullscreen && pd->dd) fs_present(pd);
    else                          gdi_present(pd);
}

void present_ddraw_frame(present_ddraw *pd, const ntvdd_frame *f)
{ present_ddraw_snapshot(pd, f); present_ddraw_present(pd); }
