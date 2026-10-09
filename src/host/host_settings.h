/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The Settings dialog: applying, loading and showing every setting and where its
 *   value came from.
 *
 * Declarations only (#335): defined in host_settings.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_HOST_SETTINGS_H
#define NTVDMEX_HOST_SETTINGS_H

#include "host_state.h"

extern INT g_JoystickPovMap;

extern INT g_DosVersionForced;
extern PCSTR g_ShellOverride;

VOID SettingsNoteOverride(INT settingId, PCSTR source, DWORD value);
VOID SettingsLogSources(VOID);
VOID SettingsApply(HWND window, const NTVDMEX_SETTINGS *settings, INT live);
VOID SettingsApplyPresent(PRESENT_DDRAW *present, const NTVDMEX_SETTINGS *settings);
VOID SettingsApplyDevices(const NTVDMEX_SETTINGS *settings);
UINT32 SettingsOutputHz(const NTVDMEX_SETTINGS *settings);
VOID SettingsApplyLive(HWND window);
INT_PTR CALLBACK SettingsPageProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK SettingsDialogProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);

#endif
