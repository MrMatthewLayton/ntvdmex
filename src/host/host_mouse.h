/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The mouse: INT 33h, its coordinate model, event queue and callbacks, and the
 *   graphics cursor.
 *
 * Declarations only (#335): defined in host_mouse.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_HOST_MOUSE_H
#define NTVDMEX_HOST_MOUSE_H

#include "host_state.h"

extern volatile LONG g_MouseButtons;
extern volatile LONG g_MouseX;
extern volatile LONG g_MouseY;
extern volatile LONG g_MouseDx;
extern volatile LONG g_MouseDy;
extern DWORD g_MouseRawAbsolute;
extern DWORD g_MouseI33Other;
extern DWORD g_MouseWmInput;
extern DWORD g_MouseI33[16];
extern DWORD g_MouseI33AxOverflow;
extern DWORD g_MouseI33SiteOverflow;
extern DWORD g_MouseI33SiteCount;
extern INT g_MouseAbsent;
extern volatile LONG g_MouseHidden;
extern volatile LONG g_MousePressCount[MS_BTNS];
extern volatile LONG g_MouseReleaseCount[MS_BTNS];
extern DWORD g_MouseEdges;
extern DWORD g_MouseEventInstalls;
extern volatile LONG g_MouseEventPend;
extern DWORD g_MouseShapeSets;
extern volatile LONG g_MouseGraphicsCursorDefined;
extern DWORD g_MouseGraphicsCursorBadPointer;
extern DWORD g_MouseAccelerationCalls;
extern volatile LONG g_MouseTextCursorXor;
extern volatile LONG g_MouseTextCursorAnd;
extern DWORD g_MouseStateBadPointer;
extern DWORD g_MouseI33Unimplemented;

VOID HostMouseButton(INT button, INT down);
UINT I33Width(VOID);
UINT I33Height(VOID);
INT I33XShift(VOID);
LONG I33VirtualX(LONG pixelX);
INT I33Text(VOID);
LONG I33VirtualY(LONG pixelY);
LONG I33VirtualMaximumY(VOID);
VOID MouseEventRaise(LONG bits);
VOID MouseButtonEdges(LONG prev, LONG now);
VOID MouseChildExited(VOID);
INT CaptureAllowed(VOID);
INT MouseGoesToGuest(VOID);
LONG I33ClampX(LONG virtualX);
LONG I33ClampY(LONG virtualY);
VOID I33ResetState(VOID);
VOID MouseInt33(volatile BYTE *tib, INT source);
INT MouseAnyHandler(VOID);
INT MouseEventQueueTake(MOUSE_EVENT_ENTRY *event, LONG *outAx, WORD *segment, DWORD *offset);
VOID MouseCallbackTry(volatile BYTE *tib);
VOID MouseCallbackReturn(volatile BYTE *tib);
VOID MouseDrawGraphicsCursor(BYTE *pixels, INT width, INT height, INT stride);

#endif
