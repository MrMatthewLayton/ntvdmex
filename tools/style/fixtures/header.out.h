/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The widget: one per guest window.
 *
 * It keeps the frame and the palette.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef WIDGET_H
#define WIDGET_H

#include <windows.h>

#include "ntvdmex_types.h"

#define WIDGET_FRAMES           4       /* Frames kept */
#define WIDGET_PALETTE_ENTRIES  256     /* Entries */

typedef struct _WIDGET
{
    DWORD Frame;
} WIDGET, *PWIDGET;

extern WIDGET g_Widget;

VOID WidgetReset(PWIDGET widget);

#endif
