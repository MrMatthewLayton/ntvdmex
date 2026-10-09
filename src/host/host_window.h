/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The window: menus, the tray, the status strip, the clipboard, mouse capture,
 *   fullscreen, scaling and the window procedure.
 *
 * Declarations only (#335): defined in host_window.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_HOST_WINDOW_H
#define NTVDMEX_HOST_WINDOW_H

#include "host_state.h"

extern HHOOK g_LowLevelKeyboard;

extern DWORD g_PitDeliverSkipped;
extern DWORD g_Irq1AsyncRetry;
extern INT g_MouseRawOk;
extern volatile LONG g_MouseAutoCaptureDone;
extern volatile LONG g_MouseWantRelease;
extern DWORD g_MouseAltCalls;
extern INT g_HostCursorMode;
extern INT g_FrameSkip;
extern DWORD g_WindowSizeLive;
extern DWORD g_AspectLive;
extern const INT g_SettingsPages[NTVDMEX_PAGE_COUNT];
/* Defined in host_window.c (#335). */
extern DWORD g_UiTickSkips;
extern DWORD g_UiTimerPresents;
extern DWORD g_UiHookPresents;
extern DWORD g_Irq1In09;
extern DWORD g_Irq1In08;
extern DWORD g_Irq1Checks;
extern DWORD g_Irq1NoIf;
extern INT g_Headless;
extern DWORD g_Irq1Injected;
extern UINT g_CaptureMs;
extern DWORD g_CaptureStart;
extern DWORD g_CaptureDelayMs;
extern INT g_Capture;
extern volatile LONG g_CloseRequest;
extern INT g_TopIsShell;
extern DWORD g_PauseMs;
extern DWORD g_PauseCooperative;
extern DWORD g_PauseCount;
extern INT g_TextDump;
extern INT g_LowLevelKeyboardOn;

VOID HostRecordFinish(VOID);
INT OtherHostsRunning(VOID);
VOID InputCaptureSet(HWND window, INT isOn);
VOID MenuViewSync(HWND window);
VOID HostApplyWindowSize(HWND window, DWORD index);
VOID HostPresentHook(PVOID context);
VOID TrayRemove(HWND window);
VOID HostPanicRelease(VOID);
DWORD WINAPI CaptureWatchdogThread(LPVOID parameter);
DWORD WINAPI UiThread(LPVOID argument);

#endif
