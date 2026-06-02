/*
 * console.h - Text-grid model and rendering for the DOS-style display.
 *
 * For the M0 shell preview this is a self-contained, non-interactive 80x25
 * character grid drawn with a fixed-pitch raster font (classic light-gray on
 * black). It owns no window; main.c drives it. When the real V86/DOS core
 * arrives (M2/M3), this grid becomes the sink that INT 10h / direct B800:0000
 * text-memory writes render into.
 */
#ifndef NTVDMEX_CONSOLE_H
#define NTVDMEX_CONSOLE_H

#include <windows.h>

/* Classic VGA text mode 3 dimensions. */
#define CON_COLS 80
#define CON_ROWS 25

typedef struct Console Console;

/* Create the console model and acquire its font/metrics. NULL on failure. */
Console *console_create(void);
void     console_destroy(Console *con);

/* Pixel size of the full grid, so the caller can size the client area to it. */
void console_pixel_size(const Console *con, int *width, int *height);

/* Write text at the cursor, advancing it. '\n' starts a new line; the grid
   scrolls up when the cursor passes the last row. */
void console_write(Console *con, const char *text);

/* Advance the cursor blink phase (call on a timer). Returns TRUE if the caller
   should repaint. */
BOOL console_toggle_cursor(Console *con);

/* Paint the whole grid into hdc, top-left aligned at the origin. */
void console_paint(Console *con, HDC hdc);

#endif /* NTVDMEX_CONSOLE_H */
