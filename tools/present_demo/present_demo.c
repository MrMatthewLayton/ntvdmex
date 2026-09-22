/*
 * present_demo.c -- a standalone exerciser for the DirectDraw presentation layer
 * (src/vdd/present_ddraw.c).  M3 slice-3 visual gate: it owns a window, builds an
 * animated 320x200x8 test frame each tick, and presents it via present_ddraw --
 * the same frame sink the video VDD will feed.  Alt+Enter / F11 toggles
 * exclusive fullscreen; Esc leaves fullscreen, or quits when windowed.
 *
 * No CRT (src/runtime.c supplies the entry + mem*); the only purpose is to eyeball
 * the windowed + fullscreen blit path on real XP before wiring it into the host.
 */
#include <windows.h>
#include "present_ddraw.h"
#include "present_scale.h"      /* PRESENT_ASPECT_* -- the 'A' key cycles them */

#define FB_W 320
#define FB_H 200

static present_ddraw g_pd;
static uint8_t       g_pixels[FB_W * FB_H];
static uint32_t      g_palette[256];
static ntvdd_frame   g_frame;
static DWORD         g_tick = 0;

static const char g_class[] = "NtvdmexPresentDemo";

static void build_palette(void)
{
    int i;
    for (i = 0; i < 256; ++i)                       /* a simple colourful ramp   */
        g_palette[i] = 0xFF000000u
                     | ((DWORD)(i)        << 16)    /* R */
                     | ((DWORD)((i * 2) & 0xFF) << 8)  /* G */
                     | ((DWORD)(255 - i));          /* B */
}

static void build_frame(void)
{
    int x, y;
    for (y = 0; y < FB_H; ++y)
        for (x = 0; x < FB_W; ++x)
            g_pixels[y * FB_W + x] = (uint8_t)((x ^ y) + g_tick);   /* animated XOR */
    g_frame.w = FB_W; g_frame.h = FB_H; g_frame.bpp = 8;
    g_frame.stride = FB_W; g_frame.pixels = g_pixels; g_frame.palette = g_palette;
    ++g_tick;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        build_frame();
        present_ddraw_frame(&g_pd, &g_frame);
        return 0;
    case WM_SYSKEYDOWN:                              /* Alt+Enter                 */
        if (wp == VK_RETURN) { present_ddraw_set_fullscreen(&g_pd, !g_pd.fullscreen); return 0; }
        break;
    case WM_KEYDOWN:
        if (wp == VK_F11) { present_ddraw_set_fullscreen(&g_pd, !g_pd.fullscreen); return 0; }
        /* ── ★ 'A' CYCLES THE ASPECT, IN BOTH MODES. (s64) ───────────────────────────
             Added because fullscreen was ignoring pd->aspect entirely and there was no
             way to SEE that without a guest. The whole point of this demo is to prove
             the blit path on real XP before trusting it in the host, and an aspect it
             cannot exercise is an aspect it cannot prove. The caption reports the
             current setting so the screen says which one you are looking at. */
        if (wp == 'A') {
            char t[96], *q = t;
            static const char *const NAMES[PRESENT_ASPECT_COUNT] =
                { "None (fill)", "4:3", "16:9", "16:10" };
            const char *n;
            unsigned i;
            g_pd.aspect = (g_pd.aspect + 1) % PRESENT_ASPECT_COUNT;
            n = NAMES[g_pd.aspect];
            for (i = 0; "present demo -- Alt+Enter=fullscreen, A=aspect, Esc=quit ["[i]; ++i)
                *q++ = "present demo -- Alt+Enter=fullscreen, A=aspect, Esc=quit ["[i];
            while (*n) *q++ = *n++;
            *q++ = ']'; *q = 0;
            SetWindowTextA(hwnd, t);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            if (g_pd.fullscreen) present_ddraw_set_fullscreen(&g_pd, 0);
            else PostQuitMessage(0);
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    WNDCLASSA wc; HWND hwnd; MSG m; RECT rc;
    (void)hPrev; (void)lpCmd;

    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = g_class;
    if (!RegisterClassA(&wc)) return 1;

    rc.left = 0; rc.top = 0; rc.right = FB_W * 2; rc.bottom = FB_H * 2;   /* 2x scale */
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA(g_class, "present demo -- Alt+Enter=fullscreen, A=aspect, Esc=quit [None (fill)]",
                         WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                         rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    if (present_ddraw_init(&g_pd, hwnd) != 0) {
        MessageBoxA(hwnd, "DirectDraw init failed", "present_demo", MB_OK);
        return 1;
    }
    /* Match the host's fullscreen defaults, or the demo proves a path nobody runs:
       fill (no whole-multiple snapping) and no display-mode change, so fullscreen is
       a borderless window drawn by exactly the same code as the windowed view. */
    g_pd.integer_scale = 0;

    build_palette();
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);
    SetTimer(hwnd, 1, 16, NULL);                     /* ~60 Hz                    */

    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }

    present_ddraw_shutdown(&g_pd);
    return 0;
}
