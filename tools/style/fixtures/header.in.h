#ifndef WIDGET_H
#define WIDGET_H

/* widget.h -- the widget: one per guest window.
 * It keeps the frame and the palette. */

#include "ntvdmex_types.h"
#include <windows.h>

VOID WidgetReset(PWIDGET widget);

typedef struct _WIDGET
{
    DWORD Frame;
} WIDGET, *PWIDGET;

extern WIDGET g_Widget;

#define WIDGET_FRAMES 4   /*  frames   kept */
#define WIDGET_PALETTE_ENTRIES 256 /* entries */

#endif
