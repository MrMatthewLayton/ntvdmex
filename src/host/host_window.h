/* host_window.h -- what host_window.c offers the host's other files.
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
#endif
