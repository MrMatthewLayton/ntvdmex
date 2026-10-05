/* sysfont.h -- fill the VGA character tables from the machine's own fonts (#322).
 *
 * NTVDMEX ships no font data. At start-up, before the tables are copied into guest
 * memory (vdd_video_install_fonts), every one of the 256 code page 437 characters is
 * drawn with GDI and read back as 8 pixels per row (bit 7 = leftmost):
 *
 *   - TEXT -- letters, digits, punctuation, and the accented letters and symbols that
 *     also exist in Windows-1252 -- from FIXEDSYS (the system's fixed ANSI font).
 *   - DOS GRAPHICS -- box drawing, blocks, shading, the 01h-1Fh symbols, Greek and
 *     maths -- from TERMINAL (the OEM font, which has the whole of code page 437).
 *     Fixedsys has none of these.
 *   - The 8x8 table entirely from Terminal's own 8x8 size: Fixedsys has no 8-pixel
 *     size, and a table squashed out of a taller font is the defect that once garbled
 *     Skyroads' text (see vdd_video_install_fonts). A font is never derived.
 *
 * A glyph shorter than its cell is centred; box-drawing and block glyphs are then
 * extended to the cell's top/bottom edge wherever they touch their own, so lines join
 * from one character cell to the next. A glyph taller than its cell loses rows that
 * are blank in every glyph of that font first.
 *
 * The choice of font is temporary by decision (#322): the font is to become a user
 * setting (#321). */
#ifndef SYSFONT_H
#define SYSFONT_H

#include <windows.h>
#include "vga_font.h"

#define SYSFONT_MAXH 32

typedef struct {
    HFONT font;
    int   w, h;                                /* the cell GDI actually gave us */
    unsigned char g[256][SYSFONT_MAXH];        /* glyphs, h rows each           */
    int   ok;
} sysfont_face_t;

/* Draw one byte in `f` into a 1bpp DIB and read back its rows. */
static void sysfont_render(HDC dc, unsigned char *bits, sysfont_face_t *f, unsigned char ch,
                           unsigned char *out)
{
    RECT r = { 0, 0, 16, SYSFONT_MAXH };
    int y;
    HFONT old = (HFONT)SelectObject(dc, f->font);
    FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkColor(dc, RGB(0, 0, 0));
    SetBkMode(dc, OPAQUE);
    TextOutA(dc, 0, 0, (LPCSTR)&ch, 1);
    GdiFlush();
    for (y = 0; y < f->h && y < SYSFONT_MAXH; ++y)
        out[y] = bits[y * 4];                  /* 16 px wide -> 4-byte stride; byte 0 = x 0..7 */
    SelectObject(dc, old);
}

/* Open a raster face and render all 256 bytes. `want_h` is a request; the face keeps
   whatever height GDI chose, and is refused unless it is 8 pixels wide. */
static int sysfont_open(HDC dc, unsigned char *bits, sysfont_face_t *f, const char *name,
                        BYTE charset, int want_h)
{
    TEXTMETRICA tm;
    HFONT old;
    unsigned c;
    f->ok = 0;
    f->font = CreateFontA(want_h, 8, 0, 0, FW_NORMAL, 0, 0, 0, charset, OUT_RASTER_PRECIS,
                          CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN,
                          name);
    if (!f->font) return 0;
    old = (HFONT)SelectObject(dc, f->font);
    GetTextMetricsA(dc, &tm);
    SelectObject(dc, old);
    f->w = tm.tmAveCharWidth;
    f->h = tm.tmHeight;
    if (f->w != 8 || f->h < 1 || f->h > SYSFONT_MAXH) return 0;
    for (c = 0; c < 256; ++c) sysfont_render(dc, bits, f, (unsigned char)c, f->g[c]);
    f->ok = 1;
    return 1;
}

/* ── THE CODE PAGE 437 TERMINAL, READ FROM ITS FILE. GDI picks "Terminal" by the
     machine's OEM code page -- on a UK XP that is 850, whose font has accented letters
     where 437 has some box pieces and the Greek/maths symbols (measured on the rig:
     the charset probe showed Á Â À © where ╡ ╢ ╖ belong). XP keeps the 437 fonts on
     every locale, so the
     faces are read straight out of their files (CGA80WOA.FON 8x8, VGAOEM.FON 8x12): an NE file whose RT_FONT resources are
     Windows FNT 2.0/3.0 bitmaps. Still the user's own system font; nothing shipped. */
static DWORD sysfont_rd(const unsigned char *b, DWORD n, DWORD o, int sz)
{
    DWORD v = 0; int i;
    if (o + (DWORD)sz > n) return 0;
    for (i = sz - 1; i >= 0; --i) v = (v << 8) | b[o + i];
    return v;
}

/* Load the 8 x `want_h` OEM face from Fonts\`file` into `f`. 1 on success. */
static int sysfont_open_fon(sysfont_face_t *f, const char *file, int want_h)
{
    char path[MAX_PATH];
    HANDLE h;
    DWORD n = 0, rd = 0, ne, rt, shift, o;
    unsigned char *b;
    int found = 0;
    f->ok = 0; f->font = NULL; f->w = 0; f->h = 0;
    if (!GetWindowsDirectoryA(path, MAX_PATH - 32)) return 0;
    lstrcatA(path, "\\Fonts\\"); lstrcatA(path, file);
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    n = GetFileSize(h, NULL);
    if (n == INVALID_FILE_SIZE || n < 0x80 || n > (4u << 20)) { CloseHandle(h); return 0; }
    b = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, n);
    if (!b) { CloseHandle(h); return 0; }
    ReadFile(h, b, n, &rd, NULL);
    CloseHandle(h);
    ne = sysfont_rd(b, rd, 0x3C, 4);
    if (rd != n || b[0] != 'M' || b[1] != 'Z' || ne + 0x40 > n || b[ne] != 'N' || b[ne + 1] != 'E') {
        HeapFree(GetProcessHeap(), 0, b); return 0;
    }
    rt = ne + sysfont_rd(b, n, ne + 0x24, 2);              /* resource table */
    shift = sysfont_rd(b, n, rt, 2);
    o = rt + 2;
    while (!found && o + 8 <= n) {
        DWORD type = sysfont_rd(b, n, o, 2), cnt = sysfont_rd(b, n, o + 2, 2), i;
        if (!type) break;
        o += 8;
        for (i = 0; i < cnt && o + 12 <= n; ++i, o += 12) {
            DWORD fo = sysfont_rd(b, n, o, 2) << shift, ver, pw, ph, cs, first, last, tab, c;
            if (type != 0x8008 || found || fo + 148 > n) continue;   /* RT_FONT only */
            ver = sysfont_rd(b, n, fo, 2);
            cs  = b[fo + 85];
            pw  = sysfont_rd(b, n, fo + 86, 2);
            ph  = sysfont_rd(b, n, fo + 88, 2);
            first = b[fo + 95]; last = b[fo + 96];
            if ((ver != 0x200 && ver != 0x300) || cs != OEM_CHARSET || pw != 8
                || (int)ph != want_h || ph > SYSFONT_MAXH) continue;
            tab = fo + (ver == 0x300 ? 148 : 118);
            for (c = 0; c < 256; ++c) {
                DWORD e, go, y;
                for (y = 0; y < SYSFONT_MAXH; ++y) f->g[c][y] = 0;
                if (c < first || c > last) continue;
                e  = tab + (c - first) * (ver == 0x300 ? 6 : 4);
                go = fo + (ver == 0x300 ? sysfont_rd(b, n, e + 2, 4) : sysfont_rd(b, n, e + 2, 2));
                for (y = 0; y < ph && go + y < n; ++y) f->g[c][y] = b[go + y];
            }
            f->w = 8; f->h = (int)ph; f->ok = 1; found = 1;
        }
    }
    HeapFree(GetProcessHeap(), 0, b);
    return found;
}

/* Is CP437 code `c` text Fixedsys can draw? Writes the Windows-1252 byte to *ansi. */
static int sysfont_is_text(unsigned c, unsigned char *ansi)
{
    char in = (char)c;
    WCHAR wc = 0;
    BOOL used = FALSE;
    char out = 0;
    if (c < 0x20 || c == 0x7F) return 0;               /* CP437 symbols there, not text */
    if (c < 0x7F) { *ansi = (unsigned char)c; return 1; }
    if (MultiByteToWideChar(437, MB_USEGLYPHCHARS, &in, 1, &wc, 1) != 1) return 0;
    if (wc >= 0x2190 && wc <= 0x25FF) return 0;       /* arrows, maths, boxes, blocks, shapes */
    if (WideCharToMultiByte(1252, WC_NO_BEST_FIT_CHARS, &wc, 1, &out, 1, NULL, &used) != 1
        || used)
        return 0;
    *ansi = (unsigned char)out;
    return 1;
}

static int sysfont_is_boxblock(unsigned c)
{
    char in = (char)c;
    WCHAR wc = 0;
    if (MultiByteToWideChar(437, MB_USEGLYPHCHARS, &in, 1, &wc, 1) != 1) return 0;
    return wc >= 0x2500 && wc <= 0x259F;
}

/* Rows blank in every glyph of a face, from the top. */
static int sysfont_blank_top(const sysfont_face_t *f)
{
    int y; unsigned c;
    for (y = 0; y < f->h; ++y)
        for (c = 0; c < 256; ++c) if (f->g[c][y]) return y;
    return f->h;
}

/* Put one glyph of `f` into a cell `H` rows high. */
static void sysfont_fit(const sysfont_face_t *f, unsigned char ch, int boxblock,
                        unsigned char *cell, int H, int crop_top)
{
    const unsigned char *g = f->g[ch];
    int fh = f->h, off, y;
    for (y = 0; y < H; ++y) cell[y] = 0;
    if (fh > H) {                                       /* too tall: drop blank rows */
        int top = crop_top < fh - H ? crop_top : fh - H;
        for (y = 0; y < H; ++y) cell[y] = g[top + y];
        return;
    }
    off = (H - fh) / 2;
    for (y = 0; y < fh; ++y) cell[off + y] = g[y];
    if (boxblock) {                                     /* reach the cell edges */
        if (g[0])      for (y = 0; y < off; ++y)            cell[y] = g[0];
        if (g[fh - 1]) for (y = off + fh; y < H; ++y)       cell[y] = g[fh - 1];
    }
}

/* Fill vga_font_8x8/8x14/8x16. Returns a one-line summary for the log. */
static const char *sysfont_build(void)
{
    static char sum[200];
    static sysfont_face_t fx, t8, t14, t16;            /* static: ~8 KB each */
    BITMAPINFO bi;
    void *bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp, oldbmp;
    unsigned c;
    int crop_fx = 0;
    const char *src8 = "GDI by name", *src12 = "GDI by name";
    if (!dc) return "sysfont: no DC -- tables left empty";
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 16;
    bi.bmiHeader.biHeight = -SYSFONT_MAXH;              /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 1;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) { DeleteDC(dc); return "sysfont: no DIB -- tables left empty"; }
    oldbmp = (HBITMAP)SelectObject(dc, bmp);

    sysfont_open(dc, (unsigned char *)bits, &fx,  "Fixedsys", ANSI_CHARSET, 0);
    /* Terminal at code page 437, from its files; by name only if a file is missing. */
    /* Measured on the rig's UK XP: CGA80WOA.FON is the 437 Terminal 8x8 (by name GDI
       gave CGA80850.FON, code page 850); VGAOEM.FON and EGA80WOA.FON are 437 8x12.
       DOSAPP.FON there has no 8-wide face at all. */
    if (sysfont_open_fon(&t8, "CGA80WOA.FON", 8)) src8 = "CGA80WOA.FON";
    else if (sysfont_open_fon(&t8, "DOSAPP.FON", 8)) src8 = "DOSAPP.FON";
    else sysfont_open(dc, (unsigned char *)bits, &t8, "Terminal", OEM_CHARSET, 8);
    if (sysfont_open_fon(&t14, "VGAOEM.FON", 12))        src12 = "VGAOEM.FON";
    else if (sysfont_open_fon(&t14, "EGA80WOA.FON", 12)) src12 = "EGA80WOA.FON";
    else sysfont_open(dc, (unsigned char *)bits, &t14, "Terminal", OEM_CHARSET, 14);
    t16.ok = 0;                                         /* no 8x16 Terminal exists */
    /* A Terminal size GDI could not match exactly (it substitutes the nearest) must not
       be cropped into a smaller cell -- that would be a derived font again. Refused; the
       summary says MISSING, and that table's graphics stay blank rather than wrong. */
    if (t8.ok  && t8.h  != 8)  t8.ok  = 0;
    if (t14.ok && t14.h > 14)  t14.ok = 0;
    if (t16.ok && t16.h > 16)  t16.ok = 0;
    if (fx.ok) crop_fx = sysfont_blank_top(&fx);

    /* Each table takes the TALLEST accepted Terminal face that fits it. Measured on
       XP: GDI offers 8x8, 8x12 and -- for a 16-pixel request -- 12x16, which is refused
       (not 8 wide); the 8x16 table then uses the 8x12, centred and edge-extended. */
    {   sysfont_face_t *g14 = t14.ok ? &t14 : t8.ok ? &t8 : NULL;
        sysfont_face_t *g16 = t16.ok ? &t16 : t14.ok ? &t14 : t8.ok ? &t8 : NULL;
        for (c = 0; c < 256; ++c) {
            unsigned char ansi = 0;
            int text = fx.ok && sysfont_is_text(c, &ansi);
            int bb = sysfont_is_boxblock(c);
            if (t8.ok) sysfont_fit(&t8, (unsigned char)c, bb, vga_font_8x8[c], 8, 0);
            if (text)     sysfont_fit(&fx, ansi, 0, vga_font_8x14[c], 14, crop_fx);
            else if (g14) sysfont_fit(g14, (unsigned char)c, bb, vga_font_8x14[c], 14, 0);
            if (text)     sysfont_fit(&fx, ansi, 0, vga_font_8x16[c], 16, crop_fx);
            else if (g16) sysfont_fit(g16, (unsigned char)c, bb, vga_font_8x16[c], 16, 0);
        }
    }

    SelectObject(dc, oldbmp);
    DeleteObject(bmp);
    if (fx.font) DeleteObject(fx.font);
    if (t8.font) DeleteObject(t8.font);
    if (t14.font) DeleteObject(t14.font);
    DeleteDC(dc);
    wsprintfA(sum, "sysfont: Fixedsys %s %dx%d (blank top %d); Terminal-437 8x8 %s %dx%d from %s, "
                   "8x12 (for 8x14 and 8x16) %s %dx%d from %s",
              fx.ok ? "ok" : "MISSING", fx.w, fx.h, crop_fx,
              t8.ok ? "ok" : "MISSING", t8.w, t8.h, src8,
              t14.ok ? "ok" : "MISSING", t14.w, t14.h, src12);
    return sum;
}

#endif /* SYSFONT_H */
