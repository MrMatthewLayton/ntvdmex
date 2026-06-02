/*
 * console.c - see console.h.
 */
#include "console.h"

#include <string.h>   /* memset / memmove -- defined in runtime.c (no CRT) */

/* Classic VGA attribute 7: light gray text on a black background. */
#define CON_FG RGB(170, 170, 170)
#define CON_BG RGB(0, 0, 0)

struct Console {
    char  cells[CON_ROWS][CON_COLS]; /* the character grid (no attributes yet) */
    int   cx, cy;                    /* cursor column / row */
    BOOL  cursor_on;                 /* current blink phase */
    HFONT font;                      /* fixed-pitch raster font */
    int   cell_w, cell_h;            /* glyph cell size, in pixels */
};

static void console_clear(Console *con)
{
    memset(con->cells, ' ', sizeof(con->cells));
    con->cx = 0;
    con->cy = 0;
}

Console *console_create(void)
{
    /* Exactly one console exists for the process lifetime, so it lives in static
       storage -- no heap, no CRT allocator. */
    static Console storage;
    Console *con = &storage;

    /* OEM_FIXED_FONT is the stock DOS-box raster font, present on every
       Windows and already fixed-pitch -- exactly the look we want, with no
       dependency on a particular TrueType face being installed. */
    con->font = (HFONT)GetStockObject(OEM_FIXED_FONT);

    /* Measure the cell so the window can be sized to an exact 80x25 grid. */
    {
        HDC         hdc = GetDC(NULL);
        HFONT       old = (HFONT)SelectObject(hdc, con->font);
        TEXTMETRICA tm;
        GetTextMetricsA(hdc, &tm);
        con->cell_w = tm.tmAveCharWidth; /* == max width for a fixed-pitch font */
        con->cell_h = tm.tmHeight;
        SelectObject(hdc, old);
        ReleaseDC(NULL, hdc);
    }

    console_clear(con);
    con->cursor_on = TRUE;
    return con;
}

void console_destroy(Console *con)
{
    /* Static storage and a stock GDI font: nothing to release. */
    (void)con;
}

void console_pixel_size(const Console *con, int *width, int *height)
{
    if (width)  *width  = con->cell_w * CON_COLS;
    if (height) *height = con->cell_h * CON_ROWS;
}

static void console_newline(Console *con)
{
    con->cx = 0;
    if (++con->cy >= CON_ROWS) {
        /* Scroll up one line and clear the freed bottom row. */
        memmove(con->cells[0], con->cells[1],
                (CON_ROWS - 1) * CON_COLS);
        memset(con->cells[CON_ROWS - 1], ' ', CON_COLS);
        con->cy = CON_ROWS - 1;
    }
}

void console_write(Console *con, const char *text)
{
    for (; *text; ++text) {
        char c = *text;
        if (c == '\n') {
            console_newline(con);
            continue;
        }
        if (c == '\r') {
            con->cx = 0;
            continue;
        }
        con->cells[con->cy][con->cx] = c;
        if (++con->cx >= CON_COLS)
            console_newline(con);
    }
}

BOOL console_toggle_cursor(Console *con)
{
    con->cursor_on = !con->cursor_on;
    return TRUE;
}

void console_paint(Console *con, HDC hdc)
{
    HFONT old = (HFONT)SelectObject(hdc, con->font);
    int   row;

    SetTextColor(hdc, CON_FG);
    SetBkColor(hdc, CON_BG);
    SetBkMode(hdc, OPAQUE); /* each row's OPAQUE draw also paints the background */

    for (row = 0; row < CON_ROWS; ++row) {
        TextOutA(hdc, 0, row * con->cell_h, con->cells[row], CON_COLS);
    }

    /* Blinking underline cursor at the current cell, DOS-style. */
    if (con->cursor_on) {
        RECT caret;
        caret.left   = con->cx * con->cell_w;
        caret.right  = caret.left + con->cell_w;
        caret.bottom = (con->cy + 1) * con->cell_h;
        caret.top    = caret.bottom - 2;
        SetBkColor(hdc, CON_FG);
        ExtTextOutA(hdc, 0, 0, ETO_OPAQUE, &caret, NULL, 0, NULL);
    }

    SelectObject(hdc, old);
}
