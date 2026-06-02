/*
 * main.c - NTVDMEX shell preview (milestone M0).
 *
 * A single fixed-size window whose client area is a DOS-style 80x25 text
 * console (see console.c). The window frame itself is left to the OS, so on a
 * themed Windows XP machine it renders with the Luna title bar and border --
 * the Common-Controls 6.0 manifest (res/ntvdmex.manifest) is what opts us into
 * the visual-styles engine that makes that happen.
 *
 * This preview is deliberately non-interactive: it shows the prompt and a
 * blinking cursor but does not process input. Its job is to lock down the
 * build toolchain and the window/console shell before the V86 + DOS core lands.
 */
#include <windows.h>
#include <commctrl.h>

#include "console.h"

static const char  g_class_name[] = "NtvdmexShellWindow";
static const char  g_window_title[] = "NTVDMEX";
static const UINT  IDT_CURSOR_BLINK = 1;

static Console *g_console = NULL;

/* The idle "booted" screen of the non-functional shell. */
static void console_show_banner(Console *con)
{
    console_write(con,
        "NTVDMEX  -  New Technology Virtual DOS Manager, Extended\n"
        "Version 0.1.0   Shell preview (milestone M0)\n"
        "(C) 2026 Matthew Layton.  MIT License.\n"
        "\n"
        "Real-CPU (V86) DOS/Win16 host for Windows XP SP3.\n"
        "This is a non-functional command-line preview: the prompt below is\n"
        "for show only -- no commands are processed yet.\n"
        "\n"
        "C:\\>");
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_CREATE:
        console_show_banner(g_console);
        /* Classic ~530ms caret blink. */
        SetTimer(hwnd, IDT_CURSOR_BLINK, 530, NULL);
        return 0;

    case WM_TIMER:
        if (wparam == IDT_CURSOR_BLINK && console_toggle_cursor(g_console))
            InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC         hdc = BeginPaint(hwnd, &ps);
        console_paint(g_console, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, IDT_CURSOR_BLINK);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSEXA wc;
    HWND        hwnd;
    MSG         msg;
    RECT        rc;
    int         grid_w, grid_h;
    DWORD       style;
    INITCOMMONCONTROLSEX icc;

    (void)hPrevInstance;
    (void)lpCmdLine;

    /* Load the v6 common controls so the manifest's visual style is active. */
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    g_console = console_create();
    if (!g_console) {
        MessageBoxA(NULL, "Failed to create console.", g_window_title,
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.cbClsExtra    = 0;
    wc.cbWndExtra    = 0;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszMenuName  = NULL;
    wc.lpszClassName = g_class_name;
    wc.hIconSm       = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassExA(&wc)) {
        MessageBoxA(NULL, "Failed to register window class.", g_window_title,
                    MB_OK | MB_ICONERROR);
        console_destroy(g_console);
        return 1;
    }

    /* Fixed-size window: a caption with system menu, minimize and close, but no
       resize border or maximize box -- the 80x25 grid has one true size. Grow
       the requested client rect to the full window rect for that style. */
    style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    console_pixel_size(g_console, &grid_w, &grid_h);
    rc.left = 0; rc.top = 0; rc.right = grid_w; rc.bottom = grid_h;
    AdjustWindowRectEx(&rc, style, FALSE, 0);

    hwnd = CreateWindowExA(
        0, g_class_name, g_window_title, style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInstance, NULL);

    if (!hwnd) {
        MessageBoxA(NULL, "Failed to create window.", g_window_title,
                    MB_OK | MB_ICONERROR);
        console_destroy(g_console);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    console_destroy(g_console);
    return (int)msg.wParam;
}
