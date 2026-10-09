/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Keyboard and joystick input: scancodes, typematic repeat, the synthetic-key driver, modifier tracking and the low-level keyboard hook.
 *
 * Declarations only (#335): defined in host_input.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_HOST_INPUT_H
#define NTVDMEX_HOST_INPUT_H
#include "host_state.h"

VOID KeyLatencyPop(VOID);
VOID HostKeyScancode(BYTE rawScancode, INT extended, INT isBreak);
VOID HostKeyTypematicInitialize(VOID);
extern UINT32 g_TypematicSent;
extern UINT32 g_TypematicOsRepeats;
VOID HostKeyPresent(VOID);
VOID HostKeyTypematic(VOID);
DWORD WINAPI SynthKeyThread(LPVOID parameter);
LRESULT CALLBACK LowLevelKeyboardProcedure(INT code, WPARAM wParam, LPARAM lParam);
UINT64 JoystickNowMicroseconds(PVOID context);
extern LONG g_JoystickThreadStarted;
VOID JoystickPollEnsure(VOID);
VOID KeyMessageNote(VOID);
VOID KeyPushMake(LPARAM lParam);
VOID KeyPushBreak(LPARAM lParam);
VOID HostReleaseModifiers(VOID);

#endif
