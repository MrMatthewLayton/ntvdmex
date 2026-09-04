/* wow_test.c -- off-VM battery for the WOW32 TRANSLATION LAYER. GH #128.
 *
 * ── WHY THIS EXISTS, AND WHY IT IS LATE ─────────────────────────────────────
 * Before this file the project had 842 off-VM checks covering the DOS kernel,
 * the VDDs, the OPL synth and the NE loader -- and **zero** covering the Win16
 * translation layer, which is the part this host actually writes. So every
 * defect in it was found by a human looking at a screen, and every fix was
 * validated the same way. Session 51 shipped three visual regressions in a row
 * that way; this is the answer to that, not more care.
 *
 * Three parts, in increasing specificity:
 *
 *   1. MACRO HYGIENE -- scan the real headers for duplicate `*_ARG_*` names.
 *      This is not stylistic. `InvertRect` redefined `IR_ARG_RECT` from 2 to 0
 *      and the preprocessor takes the LAST definition before the use, so BOTH
 *      handlers read offset 0 and InvalidateRect fetched its `lpRect` out of
 *      `bErase` -- for eight sessions, silently, invalidating the whole client
 *      area every time. `SetMenu` hit the identical trap against SendMessage's
 *      `SM_ARG_HWND` the day it was written. The compiler DID warn both times
 *      and nobody read it. A prefix here is a namespace; this makes a collision
 *      in it a FAILING TEST rather than a line of build output.
 *
 *   2. OFFSET TILING -- for each service, the argument offsets must tile its
 *      declared width exactly: no gaps, no overlaps, nothing off the end. The
 *      widths are not invented here; they come from `tools/ne/neneeds.py`, which
 *      reads them out of the real Microsoft binaries. An offset table that does
 *      not add up is wrong by construction, and this catches it without a rig,
 *      a guest, or a screenshot.
 *      ⚠ THE ARGUMENT BLOCK IS REVERSED. Win16 is FAR PASCAL: arguments are
 *        pushed LEFT TO RIGHT, so the block's base is the LAST push and offset 0
 *        is the RIGHTMOST parameter. Each table below is therefore written in
 *        reverse prototype order, which is also how it must be read.
 *
 *   3. THE SEMANTIC DELTAS -- src/wow/wowconv.h, pinned directly.
 *
 * Build+run via tools/dostest/run.sh. Needs no Windows, no VM and no guest.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/wow/wowconv.h"

static int pass, fail, skip;
static void ok(int c, const char *what)
{
    if (c) { ++pass; printf("  PASS  %s\n", what); }
    else   { ++fail; printf("  FAIL  %s\n", what); }
}

/* ── PART 1 + 2 SHARED: the macro table scanned out of the real headers ───── */
#define MAXDEF 512
typedef struct { char name[64]; long val; char file[32]; int line; } def_t;
static def_t g_def[MAXDEF];
static int   g_ndef;

static const char *HEADERS[] = {
    "src/wow/wowuser.h", "src/wow/wowgdi.h", "src/wow/wow32.h",
    "src/wow/wowres.h",  "src/wow/wowcommdlg.h", "src/wow/wowshell.h",
};

/* A `#define <NAME>_ARG_<X> <number>` line, and nothing else. Deliberately
   strict: a macro defined to an expression is not an offset table entry and
   pretending otherwise would invent coverage. */
static void scan_header(const char *root, const char *rel)
{
    char path[512], line[1024];
    FILE *f;
    int lineno = 0;
    snprintf(path, sizeof path, "%s/%s", root, rel);
    f = fopen(path, "r");
    if (!f) { ++skip; printf("  SKIP  %s not found\n", rel); return; }
    while (fgets(line, sizeof line, f)) {
        char name[128]; long v; char *p = line, *q;
        ++lineno;
        while (*p == ' ' || *p == '\t') ++p;
        if (strncmp(p, "#define", 7)) continue;
        p += 7;
        while (*p == ' ' || *p == '\t') ++p;
        q = name;
        while (*p && *p != ' ' && *p != '\t' && (q - name) < (int)sizeof name - 1)
            *q++ = *p++;
        *q = 0;
        if (!strstr(name, "_ARG_")) continue;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '(') continue;                 /* not a plain number */
        {   char *end; v = strtol(p, &end, 0);
            if (end == p) continue; }
        if (g_ndef >= MAXDEF) continue;
        snprintf(g_def[g_ndef].name, sizeof g_def[g_ndef].name, "%s", name);
        snprintf(g_def[g_ndef].file, sizeof g_def[g_ndef].file, "%s", rel);
        g_def[g_ndef].val = v;
        g_def[g_ndef].line = lineno;
        ++g_ndef;
    }
    fclose(f);
}

/* -1 when absent, so a table naming a macro that does not exist FAILS rather
   than silently tiling with a zero. */
static long defval(const char *name, int *found)
{
    int i;
    if (found) *found = 0;
    for (i = 0; i < g_ndef; ++i)
        if (!strcmp(g_def[i].name, name)) {
            if (found) *found = 1;
            return g_def[i].val;
        }
    return -1;
}

static void part1_macro_hygiene(void)
{
    int i, j, dup = 0;
    char what[256];
    printf("\n-- part 1: no `*_ARG_*` macro may be defined twice --\n");
    for (i = 0; i < g_ndef; ++i) {
        for (j = i + 1; j < g_ndef; ++j) {
            if (strcmp(g_def[i].name, g_def[j].name)) continue;
            ++dup;
            snprintf(what, sizeof what,
                     "%s defined twice: %s:%d = %ld and %s:%d = %ld"
                     " -- the LAST one wins at every use below it",
                     g_def[i].name, g_def[i].file, g_def[i].line, g_def[i].val,
                     g_def[j].file, g_def[j].line, g_def[j].val);
            ok(0, what);
        }
    }
    if (!dup) {
        snprintf(what, sizeof what,
                 "%d argument-offset macros, all uniquely named", g_ndef);
        ok(1, what);
    }
}

/* ── PART 2: the offset tables must tile their declared width ─────────────── */
typedef struct { const char *macro; int size; } field_t;
typedef struct {
    const char *service;    /* what it is, for the failure message      */
    int         width;      /* argument BYTES, from tools/ne/neneeds.py  */
    field_t     f[14];
} svc_t;

/* ⚠ EVERY `width` HERE CAME OUT OF A REAL BINARY, via
     `tools/ne/neneeds.py guest/win16/<prog>.exe --todo`, which reads the
     argument-byte count off the module's own thunk. None of them is a guess. */
static const svc_t SVC[] = {
  /* --- the two that collided, and the reason part 1 exists --------------- */
  { "USER InvalidateRect(hWnd, lpRect, bErase)", 8,
    { {"IR_ARG_ERASE",2}, {"IR_ARG_RECT",4}, {"IR_ARG_HWND",2}, {0,0} } },
  { "USER InvertRect(hDC, lpRect)", 6,
    { {"INVR_ARG_RECT",4}, {"INVR_ARG_HDC",2}, {0,0} } },
  { "USER SetMenu(hWnd, hMenu)", 4,
    { {"SETMENU_ARG_MENU",2}, {"SETMENU_ARG_HWND",2}, {0,0} } },

  /* --- session 50's new services ---------------------------------------- */
  { "USER SetTimer(hWnd, nIDEvent, wElapse, lpTimerFunc)", 10,
    { {"ST_ARG_PROC",4}, {"ST_ARG_ELAPSE",2}, {"ST_ARG_ID",2},
      {"ST_ARG_HWND",2}, {0,0} } },
  { "USER KillTimer(hWnd, nIDEvent)", 4,
    { {"KT_ARG_ID",2}, {"KT_ARG_HWND",2}, {0,0} } },
  { "USER FindWindow(lpClassName, lpWindowName)", 8,
    { {"FW_ARG_NAME",4}, {"FW_ARG_CLASS",4}, {0,0} } },
  { "USER FrameRect(hDC, lpRect, hBrush)", 8,
    { {"FRAMER_ARG_BRUSH",2}, {"FRAMER_ARG_RECT",4}, {"FRAMER_ARG_HDC",2}, {0,0} } },
  { "USER FillRect(hDC, lpRect, hBrush)", 8,
    { {"FR_ARG_BRUSH",2}, {"FR_ARG_RECT",4}, {"FR_ARG_HDC",2}, {0,0} } },
  { "USER DrawText(hDC, lpString, nCount, lpRect, uFormat)", 14,
    { {"DT_ARG_FORMAT",2}, {"DT_ARG_RECT",4}, {"DT_ARG_COUNT",2},
      {"DT_ARG_STR",4}, {"DT_ARG_HDC",2}, {0,0} } },
  { "USER SetDlgItemText(hDlg, nIDDlgItem, lpString)", 8,
    { {"SDIT_ARG_TEXT",4}, {"SDIT_ARG_ID",2}, {"SDIT_ARG_HDLG",2}, {0,0} } },
  { "USER GetDlgItemInt(hDlg, nID, lpTranslated, bSigned)", 10,
    { {"GDII_ARG_SIGNED",2}, {"GDII_ARG_XLATED",4}, {"GDII_ARG_ID",2},
      {"GDII_ARG_HDLG",2}, {0,0} } },
  { "USER CheckRadioButton(hDlg, first, last, check)", 8,
    { {"CRB_ARG_CHECK",2}, {"CRB_ARG_LAST",2}, {"CRB_ARG_FIRST",2},
      {"CRB_ARG_HDLG",2}, {0,0} } },
  { "USER CheckDlgButton(hDlg, nIDButton, uCheck)", 6,
    { {"CDB_ARG_CHECK",2}, {"CDB_ARG_ID",2}, {"CDB_ARG_HDLG",2}, {0,0} } },
  { "USER IsDlgButtonChecked(hDlg, nIDButton)", 4,
    { {"IDBC_ARG_ID",2}, {"IDBC_ARG_HDLG",2}, {0,0} } },
  { "USER AdjustWindowRect(lpRect, dwStyle, bMenu)", 10,
    { {"AWR_ARG_MENU",2}, {"AWR_ARG_STYLE",4}, {"AWR_ARG_RECT",4}, {0,0} } },
  { "USER LoadMenu downcall (0x96, read off USER.EXE seg1:0x480a)", 16,
    { {"LOADMENU_ARG_LOCAL0",2}, {"LOADMENU_ARG_LOCAL2",2},
      {"LOADMENU_ARG_LOCAL4",2}, {"LOADMENU_ARG_RES",4},
      {"LOADMENU_ARG_NAME",4}, {"LOADMENU_ARG_HINST",2}, {0,0} } },
  { "USER GetLastActivePopup(hwndOwner)", 2,
    { {"GLAP_ARG_HWND",2}, {0,0} } },

  /* --- long-standing ones, so the table is not only new code ------------- */
  { "USER GetClientRect(hWnd, lpRect)", 6,
    { {"GCR_ARG_RECT",4}, {"GCR_ARG_HWND",2}, {0,0} } },

  /* --- GDI ---------------------------------------------------------------- */
  { "GDI CreateDIBitmap(hDC, lpbmih, dwInit, lpbInit, lpbmi, wUsage)", 20,
    { {"CDIB_ARG_USAGE",2}, {"CDIB_ARG_BMI",4}, {"CDIB_ARG_BITS",4},
      {"CDIB_ARG_INIT",4}, {"CDIB_ARG_BMIH",4}, {"CDIB_ARG_HDC",2}, {0,0} } },
  { "GDI SetDIBitsToDevice(...12 args...)", 28,
    { {"SDD_ARG_USAGE",2}, {"SDD_ARG_BMI",4}, {"SDD_ARG_BITS",4},
      {"SDD_ARG_NSCANS",2}, {"SDD_ARG_START",2}, {"SDD_ARG_SRCY",2},
      {"SDD_ARG_SRCX",2}, {"SDD_ARG_H",2}, {"SDD_ARG_W",2},
      {"SDD_ARG_DSTY",2}, {"SDD_ARG_DSTX",2}, {"SDD_ARG_HDC",2} } },
  { "GDI SetDIBits/GetDIBits(hDC, hBM, start, lines, bits, bmi, usage)", 18,
    { {"DIB_ARG_USAGE",2}, {"DIB_ARG_BMI",4}, {"DIB_ARG_BITS",4},
      {"DIB_ARG_LINES",2}, {"DIB_ARG_START",2}, {"DIB_ARG_HBM",2},
      {"DIB_ARG_HDC",2}, {0,0} } },
  { "GDI StretchDIBits(...)", 32,
    { {"SDI_ARG_ROP",4}, {"SDI_ARG_USAGE",2}, {"SDI_ARG_BMI",4},
      {"SDI_ARG_BITS",4}, {"SDI_ARG_SRCH",2}, {"SDI_ARG_SRCW",2},
      {"SDI_ARG_SRCY",2}, {"SDI_ARG_SRCX",2}, {"SDI_ARG_DSTH",2},
      {"SDI_ARG_DSTW",2}, {"SDI_ARG_DSTY",2}, {"SDI_ARG_DSTX",2},
      {"SDI_ARG_HDC",2} } },

  /* --- krnl386 ------------------------------------------------------------ */
  { "krnl386 GetPrivateProfileInt(app, key, nDefault, file)", 14,
    { {"GPPI_ARG_FILE",4}, {"GPPI_ARG_DEFAULT",2}, {"GPPI_ARG_KEY",4},
      {"GPPI_ARG_APP",4}, {0,0} } },
};

static void part2_offset_tiling(void)
{
    unsigned s;
    char what[320];
    printf("\n-- part 2: every argument offset table must tile its width --\n");
    for (s = 0; s < sizeof SVC / sizeof SVC[0]; ++s) {
        const svc_t *v = &SVC[s];
        unsigned char cover[64];
        int i, bad = 0, total = 0, missing = 0;
        memset(cover, 0, sizeof cover);
        if (v->width > (int)sizeof cover) { ++skip; continue; }
        for (i = 0; i < 14 && v->f[i].macro; ++i) {
            int found = 0, j;
            long off = defval(v->f[i].macro, &found);
            if (!found) {
                snprintf(what, sizeof what, "%s: macro %s is not defined",
                         v->service, v->f[i].macro);
                ok(0, what); ++missing; bad = 1; continue;
            }
            total += v->f[i].size;
            for (j = 0; j < v->f[i].size; ++j) {
                long b = off + j;
                if (b < 0 || b >= v->width) {
                    snprintf(what, sizeof what,
                             "%s: %s = %ld puts byte %ld outside the %d-byte block",
                             v->service, v->f[i].macro, off, b, v->width);
                    ok(0, what); bad = 1; break;
                }
                if (cover[b]) {
                    snprintf(what, sizeof what,
                             "%s: %s = %ld OVERLAPS an earlier field at byte %ld",
                             v->service, v->f[i].macro, off, b);
                    ok(0, what); bad = 1; break;
                }
                cover[b] = 1;
            }
        }
        if (missing) continue;
        if (!bad && total != v->width) {
            snprintf(what, sizeof what,
                     "%s: fields total %d bytes but the thunk declares %d",
                     v->service, total, v->width);
            ok(0, what); bad = 1;
        }
        if (!bad) {
            for (i = 0; i < v->width; ++i)
                if (!cover[i]) {
                    snprintf(what, sizeof what,
                             "%s: byte %d of %d is named by no field",
                             v->service, i, v->width);
                    ok(0, what); bad = 1; break;
                }
        }
        if (!bad) {
            snprintf(what, sizeof what, "%s: %d bytes, tiled exactly",
                     v->service, v->width);
            ok(1, what);
        }
    }
}

/* ── PART 3: the semantic deltas ─────────────────────────────────────────── */
static void part3_conversions(void)
{
    unsigned char core[12 + 16 * 3 + 8], out[40 + 256 * 4];
    unsigned pal = 0, pix;
    unsigned char r[WOWCONV_RECT16_SIZE];
    printf("\n-- part 3: the Win16/Win32 semantic deltas (wowconv.h) --\n");

    /* NUMCOLORS. The two call sites this has to satisfy are real and read out
       of the binaries: WINMINE `cmp ax,2 / jle` (signed) and SOL `cmp ax,2 /
       jne`. Both must land on "colour" for a modern display. */
    ok(wowconv_numcolors(1)  == 2,   "NUMCOLORS: 1bpp -> 2 (a mono device really is 2)");
    ok(wowconv_numcolors(4)  == 16,  "NUMCOLORS: 4bpp -> 16");
    ok(wowconv_numcolors(8)  == 256, "NUMCOLORS: 8bpp -> 256");
    ok(wowconv_numcolors(16) == 256, "NUMCOLORS: 16bpp -> 256, never -1");
    ok(wowconv_numcolors(32) == 256, "NUMCOLORS: 32bpp -> 256, never -1");
    ok(wowconv_numcolors(32) >  2,   "  ...and WINMINE's SIGNED `jle 2` takes the COLOUR branch");
    ok(wowconv_numcolors(32) != 2,   "  ...and SOL's `cmp ax,2 / jne` skips its mono flag");
    ok(wowconv_numcolors(1)  == 2,   "  ...while a REAL mono device still reports mono");

    /* hbrBackground: three cases, and 0 must stay 0. */
    ok(wowconv_hbrback_kind(0) == WOWCONV_HBR_NONE,
       "hbrBackground: 0 is NO ERASE, not a default");
    ok(wowconv_hbrback_kind(6) == WOWCONV_HBR_SYSCOLOR,
       "hbrBackground: 6 is COLOR_WINDOW+1, a system colour");
    ok(wowconv_hbrback_kind(21) == WOWCONV_HBR_SYSCOLOR,
       "hbrBackground: 21 is COLOR_BTNHIGHLIGHT+1, the last Win3.1 index");
    ok(wowconv_hbrback_kind(22) == WOWCONV_HBR_HANDLE,
       "hbrBackground: 22 is past the Win3.1 indices, so a real brush");
    ok(wowconv_hbrback_kind(0x2018) == WOWCONV_HBR_HANDLE,
       "hbrBackground: a GDI token is a real brush");

    /* Win16 RECT: 8 bytes, four SIGNED 16-bit ints. */
    wowconv_rect16_put(r, 0, -2);
    wowconv_rect16_put(r, 1, -48);
    wowconv_rect16_put(r, 2, 1680);
    wowconv_rect16_put(r, 3, 974);
    ok(wowconv_rect16_get(r, 0) == -2 && wowconv_rect16_get(r, 1) == -48,
       "RECT16: negative left/top survive the round trip (Minesweeper's -2,-48)");
    ok(wowconv_rect16_get(r, 2) == 1680 && wowconv_rect16_get(r, 3) == 974,
       "RECT16: 1680x974 survives the round trip");
    ok(WOWCONV_RECT16_SIZE == 8,
       "RECT16: is 8 bytes, NOT Win32's 16");

    /* BITMAPCOREHEADER -> BITMAPINFOHEADER, the form every SOL.EXE card uses. */
    memset(core, 0, sizeof core);
    core[0] = 12;                        /* bcSize                   */
    core[4] = 0x47;                      /* bcWidth  = 71            */
    core[6] = 0x60;                      /* bcHeight = 96            */
    core[8] = 1;                         /* bcPlanes                 */
    core[10] = 4;                        /* bcBitCount = 4 -> 16 pal */
    core[12] = 0x11; core[13] = 0x22; core[14] = 0x33;   /* entry 0  */
    core[15] = 0x44; core[16] = 0x55; core[17] = 0x66;   /* entry 1  */
    pix = wowconv_dib_core_to_info(core, sizeof core, out, sizeof out, &pal);
    ok(pix == 12 + 16 * 3, "DIB core: pixels start after 16 RGBTRIPLEs (3 bytes each)");
    ok(pal == 16,          "DIB core: 4bpp means a full 16-entry table, no biClrUsed");
    ok(out[0] == 40,       "DIB core: biSize becomes 40");
    ok(out[4] == 0x47 && out[5] == 0 && out[8] == 0x60 && out[9] == 0,
       "DIB core: bcWidth/bcHeight widen to 32-bit WITHOUT spanning each other");
    ok(out[14] == 4,       "DIB core: biBitCount carried across");
    ok(out[40] == 0x11 && out[41] == 0x22 && out[42] == 0x33 && out[43] == 0,
       "DIB core: palette entry 0 widens RGBTRIPLE -> RGBQUAD");
    ok(out[44] == 0x44 && out[45] == 0x55 && out[46] == 0x66 && out[47] == 0,
       "DIB core: entry 1 lands at the RGBQUAD stride, not the RGBTRIPLE one");

    /* Refusals. Guessing at a format is worse than declining it. */
    core[0] = 40;
    ok(wowconv_dib_core_to_info(core, sizeof core, out, sizeof out, &pal) == 0,
       "DIB core: a 40-byte header is REFUSED here, not silently converted");
    core[0] = 12; core[10] = 7;          /* 7bpp does not exist */
    ok(wowconv_dib_core_to_info(core, sizeof core, out, sizeof out, &pal) == 0,
       "DIB core: a bit count that cannot exist is REFUSED");
    core[10] = 4;
    ok(wowconv_dib_core_to_info(core, 12, out, sizeof out, &pal) == 0,
       "DIB core: a length with no room for pixels is REFUSED");
    ok(wowconv_dib_core_to_info(core, sizeof core, out, 8, &pal) == 0,
       "DIB core: an output buffer too small is REFUSED, never overrun");
}

int main(int argc, char **argv)
{
    const char *root = (argc > 1) ? argv[1] : "../..";
    unsigned i;
    printf("== WOW32 translation-layer battery (GH #128) ==\n");
    for (i = 0; i < sizeof HEADERS / sizeof HEADERS[0]; ++i)
        scan_header(root, HEADERS[i]);
    if (!g_ndef) {
        printf("  FAIL  no argument-offset macros found under %s -- wrong root?\n", root);
        return 1;
    }
    part1_macro_hygiene();
    part2_offset_tiling();
    part3_conversions();
    printf("\n%d checks, %d failed, %d skipped\n", pass + fail, fail, skip);
    return fail ? 1 : 0;
}
