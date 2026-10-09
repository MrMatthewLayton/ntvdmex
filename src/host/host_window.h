/* host_window.h -- the window: menus, the tray, the status strip, the clipboard, mouse capture,
 *   fullscreen, scaling and the window procedure.
 *
 * Declarations only (#335): defined in host_window.c. */
#ifndef NTVDMEX_HOST_WINDOW_H
#define NTVDMEX_HOST_WINDOW_H
#include "host_state.h"

extern HHOOK g_LowLevelKeyboard;

extern DWORD g_PitDeliverSkipped;
extern DWORD g_Irq1AsyncRetry;
VOID HostRecordFinish(VOID);
INT OtherHostsRunning(VOID);
extern INT g_MouseRawOk;
extern volatile LONG g_MouseAutoCaptureDone;
extern volatile LONG g_MouseWantRelease;
extern DWORD g_MouseAltCalls;
extern INT g_HostCursorMode;
VOID InputCaptureSet(HWND window, INT isOn);
extern INT g_FrameSkip;
VOID MenuViewSync(HWND window);
VOID HostApplyWindowSize(HWND window, DWORD index);
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
VOID HostPresentHook(PVOID context);
VOID TrayRemove(HWND window);
extern INT g_LowLevelKeyboardOn;
VOID HostPanicRelease(VOID);
DWORD WINAPI CaptureWatchdogThread(LPVOID parameter);
DWORD WINAPI UiThread(LPVOID argument);
#endif
