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
 * Build+run via tests/probes/dos/run.sh. Needs no Windows, no VM and no guest.
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
/* ⚠⚠ THIS CAP SILENTLY FAILED 27 CHECKS FOR A WHOLE SESSION. (session 56)
     The scanner's overflow arm was `continue`, so once the headers held more
     than MAXDEF `*_ARG_*` macros every one found after the cap was DROPPED --
     and part 2 then reported them as `macro WOWGDI_CDIB_ARG_HDC is not defined`, for
     macros that are defined, in a file the scanner reads, at line 695. Session
     55 added 13 services and crossed the line; the battery has been reporting
     27 failures ever since and they were read as a known-bad tail rather than
     as the instrument breaking.
   ⇒ Room to grow, AND a loud overflow: an instrument that cannot hold its
     input must say so, not quietly answer about a subset. This is the same
     rule the bus applies to a refused port claim. */
#define MAXDEF 2048
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
        if (g_ndef >= MAXDEF) {
            /* Not `continue`. See the MAXDEF note: dropping a macro here makes
               part 2 report it as UNDEFINED, which sends the reader to the
               header to look for something that is already there. */
            fprintf(stderr, "wow_test: FATAL -- more than %d `*_ARG_*` macros; "
                            "raise MAXDEF. Every macro past the cap would be "
                            "reported as 'not defined'.\n", MAXDEF);
            exit(2);
        }
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
  { "USER LoadMenu downcall (0x96)", 16,
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
    { {"WOWGDI_CDIB_ARG_USAGE",2}, {"WOWGDI_CDIB_ARG_BMI",4}, {"WOWGDI_CDIB_ARG_BITS",4},
      {"WOWGDI_CDIB_ARG_INIT",4}, {"WOWGDI_CDIB_ARG_BMIH",4}, {"WOWGDI_CDIB_ARG_HDC",2}, {0,0} } },
  { "GDI SetDIBitsToDevice(...12 args...)", 28,
    { {"WOWGDI_SDD_ARG_USAGE",2}, {"WOWGDI_SDD_ARG_BMI",4}, {"WOWGDI_SDD_ARG_BITS",4},
      {"WOWGDI_SDD_ARG_NSCANS",2}, {"WOWGDI_SDD_ARG_START",2}, {"WOWGDI_SDD_ARG_SRCY",2},
      {"WOWGDI_SDD_ARG_SRCX",2}, {"WOWGDI_SDD_ARG_H",2}, {"WOWGDI_SDD_ARG_W",2},
      {"WOWGDI_SDD_ARG_DSTY",2}, {"WOWGDI_SDD_ARG_DSTX",2}, {"WOWGDI_SDD_ARG_HDC",2} } },
  { "GDI SetDIBits/GetDIBits(hDC, hBM, start, lines, bits, bmi, usage)", 18,
    { {"WOWGDI_DIB_ARG_USAGE",2}, {"WOWGDI_DIB_ARG_BMI",4}, {"WOWGDI_DIB_ARG_BITS",4},
      {"WOWGDI_DIB_ARG_LINES",2}, {"WOWGDI_DIB_ARG_START",2}, {"WOWGDI_DIB_ARG_HBM",2},
      {"WOWGDI_DIB_ARG_HDC",2}, {0,0} } },
  { "GDI StretchDIBits(...)", 32,
    { {"WOWGDI_SDI_ARG_ROP",4}, {"WOWGDI_SDI_ARG_USAGE",2}, {"WOWGDI_SDI_ARG_BMI",4},
      {"WOWGDI_SDI_ARG_BITS",4}, {"WOWGDI_SDI_ARG_SRCH",2}, {"WOWGDI_SDI_ARG_SRCW",2},
      {"WOWGDI_SDI_ARG_SRCY",2}, {"WOWGDI_SDI_ARG_SRCX",2}, {"WOWGDI_SDI_ARG_DSTH",2},
      {"WOWGDI_SDI_ARG_DSTW",2}, {"WOWGDI_SDI_ARG_DSTY",2}, {"WOWGDI_SDI_ARG_DSTX",2},
      {"WOWGDI_SDI_ARG_HDC",2} } },

  /* --- #295: the metafile enumerator and its record player (12 bytes each, the
         thunk width in docs/inventory/win16-surface.md) ---------------------- */
  { "GDI EnumMetaFile(hdc, hmf, lpfn, lParam)", 12,
    { {"WOWGDI_EMF_ARG_LPARAM",4}, {"WOWGDI_EMF_ARG_PROC",4}, {"WOWGDI_EMF_ARG_HMF",2},
      {"WOWGDI_EMF_ARG_HDC",2}, {0,0} } },
  { "GDI PlayMetaFileRecord(hdc, lpht, lpmr, nHandles)", 12,
    { {"WOWGDI_PMFR_ARG_NHANDLES",2}, {"WOWGDI_PMFR_ARG_MR",4}, {"WOWGDI_PMFR_ARG_HT",4},
      {"WOWGDI_PMFR_ARG_HDC",2}, {0,0} } },

  /* --- krnl386 ------------------------------------------------------------ */
  { "krnl386 GetPrivateProfileInt(app, key, nDefault, file)", 14,
    { {"WOW32_GETPRIVATEPROFILEINT_ARG_FILE",4}, {"WOW32_GETPRIVATEPROFILEINT_ARG_DEFAULT",2}, {"WOW32_GETPRIVATEPROFILEINT_ARG_KEY",4},
      {"WOW32_GETPRIVATEPROFILEINT_ARG_APP",4}, {0,0} } },
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
    ok(WowConvNumColors(1)  == 2,   "NUMCOLORS: 1bpp -> 2 (a mono device really is 2)");
    ok(WowConvNumColors(4)  == 16,  "NUMCOLORS: 4bpp -> 16");
    ok(WowConvNumColors(8)  == 256, "NUMCOLORS: 8bpp -> 256");
    ok(WowConvNumColors(16) == 256, "NUMCOLORS: 16bpp -> 256, never -1");
    ok(WowConvNumColors(32) == 256, "NUMCOLORS: 32bpp -> 256, never -1");
    ok(WowConvNumColors(32) >  2,   "  ...and WINMINE's SIGNED `jle 2` takes the COLOUR branch");
    ok(WowConvNumColors(32) != 2,   "  ...and SOL's `cmp ax,2 / jne` skips its mono flag");
    ok(WowConvNumColors(1)  == 2,   "  ...while a REAL mono device still reports mono");

    /* hbrBackground: three cases, and 0 must stay 0. */
    ok(WowConvBackgroundBrushKind(0) == WOWCONV_HBR_NONE,
       "hbrBackground: 0 is NO ERASE, not a default");
    ok(WowConvBackgroundBrushKind(6) == WOWCONV_HBR_SYSCOLOR,
       "hbrBackground: 6 is COLOR_WINDOW+1, a system colour");
    ok(WowConvBackgroundBrushKind(21) == WOWCONV_HBR_SYSCOLOR,
       "hbrBackground: 21 is COLOR_BTNHIGHLIGHT+1, the last Win3.1 index");
    ok(WowConvBackgroundBrushKind(22) == WOWCONV_HBR_HANDLE,
       "hbrBackground: 22 is past the Win3.1 indices, so a real brush");
    ok(WowConvBackgroundBrushKind(0x2018) == WOWCONV_HBR_HANDLE,
       "hbrBackground: a GDI token is a real brush");

    /* Win16 RECT: 8 bytes, four SIGNED 16-bit ints. */
    WowConvRect16Put(r, 0, -2);
    WowConvRect16Put(r, 1, -48);
    WowConvRect16Put(r, 2, 1680);
    WowConvRect16Put(r, 3, 974);
    ok(WowConvRect16Get(r, 0) == -2 && WowConvRect16Get(r, 1) == -48,
       "RECT16: negative left/top survive the round trip (Minesweeper's -2,-48)");
    ok(WowConvRect16Get(r, 2) == 1680 && WowConvRect16Get(r, 3) == 974,
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
    pix = WowConvDibCoreToInfo(core, sizeof core, out, sizeof out, &pal);
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
    ok(WowConvDibCoreToInfo(core, sizeof core, out, sizeof out, &pal) == 0,
       "DIB core: a 40-byte header is REFUSED here, not silently converted");
    core[0] = 12; core[10] = 7;          /* 7bpp does not exist */
    ok(WowConvDibCoreToInfo(core, sizeof core, out, sizeof out, &pal) == 0,
       "DIB core: a bit count that cannot exist is REFUSED");
    core[10] = 4;
    ok(WowConvDibCoreToInfo(core, 12, out, sizeof out, &pal) == 0,
       "DIB core: a length with no room for pixels is REFUSED");
    ok(WowConvDibCoreToInfo(core, sizeof core, out, 8, &pal) == 0,
       "DIB core: an output buffer too small is REFUSED, never overrun");
}

/*
 * ── PART 4: THE MODAL DIALOG LOOP'S TWO DECISIONS. (session 57) ─────────────
 * `DialogBox` does not return until `EndDialog`, so the host runs a message loop
 * on the guest's behalf -- and the failure mode of getting that wrong is not a
 * wrong pixel, it is a HANG. Session 56 declined to write the loop for exactly
 * that reason. So both decisions inside it are pure functions in wowconv.h and
 * both are pinned here, off-VM, with no rig and no guest:
 *
 *   1. WHICH PROCEDURE drives a window -- a `#32770` dialog has no class window
 *      procedure at all, and getting the order wrong sends every message to the
 *      wrong 16-bit address, which is the "answered by an unrelated function"
 *      shape this project treats as worse than not answering.
 *   2. WHEN THE LOOP MUST STOP. The property that matters is TOTALITY: there
 *      must be no combination of facts for which the loop neither runs nor
 *      leaves. That is checked here by enumerating all sixteen.
 */
static void part4_modal(void)
{
    int e, a, h, x, run = 0, seen[5];
    printf("\n-- part 4: the modal dialog loop (wowconv.h, src/wow/wowdlg.h) --\n");

    /* 1. The procedure rule. */
    ok(WowConvWindowProcedure(0x11110000u, 0) == 0x11110000u,
       "winproc: a class window procedure drives its window");
    ok(WowConvWindowProcedure(0, 0x22220000u) == 0x22220000u,
       "winproc: a #32770 dialog has only a DLGPROC, and it drives it");
    ok(WowConvWindowProcedure(0x11110000u, 0x22220000u) == 0x11110000u,
       "winproc: with BOTH, the class procedure wins (CALC's SciCalc dialog)");
    ok(WowConvWindowProcedure(0, 0) == 0,
       "winproc: neither means NOTHING can be told about this window");

    /* 2. The exit rule, case by case. */
    ok(WowConvModalExit(0, 1, 1, 0) == WOWCONV_MODAL_RUN,
       "modal: alive, drivable, not ended, not expired -> KEEP PUMPING");
    ok(WowConvModalExit(1, 1, 1, 0) == WOWCONV_MODAL_END,
       "modal: EndDialog -> the caller gets nResult");
    ok(WowConvModalExit(0, 0, 1, 0) == WOWCONV_MODAL_GONE,
       "modal: the window was destroyed -> DialogBox returns 0, never waits");
    ok(WowConvModalExit(0, 1, 0, 0) == WOWCONV_MODAL_NOPROC,
       "modal: nothing to dispatch to -> return 0 AT ONCE (s56's behaviour)");
    ok(WowConvModalExit(0, 1, 1, 1) == WOWCONV_MODAL_EXPIRED,
       "modal: the bounded input wait ran out -> return 0, so a harness ends");

    /* ★ THE ORDER, WHICH IS THE ONE THING A READER WOULD GET WRONG. A dialog
         procedure that calls EndDialog and whose window is then torn down has
         ANSWERED; reporting GONE there would throw away the OK the user just
         clicked and hand back 0 instead. */
    ok(WowConvModalExit(1, 0, 1, 0) == WOWCONV_MODAL_END,
       "modal: ENDED OUTRANKS a destroyed window -- the answer is not lost");
    ok(WowConvModalExit(1, 0, 0, 1) == WOWCONV_MODAL_END,
       "modal: ...and outranks every other reason to stop, together");
    ok(WowConvModalExit(0, 0, 0, 1) == WOWCONV_MODAL_GONE,
       "modal: a gone window outranks having no procedure and no input");
    ok(WowConvModalExit(0, 1, 0, 1) == WOWCONV_MODAL_NOPROC,
       "modal: an undrivable dialog is refused BEFORE it can time out");

    /* ★★ TOTALITY. Sixteen combinations, every one of them classified, and RUN
         reachable from exactly one -- the one where the dialog is alive,
         drivable, unfinished and has not run out of time. A new fact added to
         this decision without a rule for it would show up here as a hang. */
    for (x = 0; x < 5; ++x) seen[x] = 0;
    for (e = 0; e < 2; ++e) for (a = 0; a < 2; ++a)
    for (h = 0; h < 2; ++h) for (x = 0; x < 2; ++x) {
        int v = WowConvModalExit(e, a, h, x);
        if (v < 0 || v > 4) { ok(0, "modal: a verdict outside the five"); return; }
        ++seen[v];
        if (v == WOWCONV_MODAL_RUN) ++run;
    }
    ok(run == 1, "modal: EXACTLY ONE of the 16 states keeps pumping");
    ok(seen[WOWCONV_MODAL_END] + seen[WOWCONV_MODAL_GONE]
       + seen[WOWCONV_MODAL_NOPROC] + seen[WOWCONV_MODAL_EXPIRED] == 15,
       "modal: the other 15 all LEAVE -- the loop has no state that hangs");
    ok(seen[WOWCONV_MODAL_END] == 8,
       "modal: ended is half of them, whatever else is true");
}

/*
 * ── PART 5: WALKING A WINDOWS METAFILE. (#295) ─────────────────────────────
 * EnumMetaFile hands the guest one record per callback, and the walk is the part
 * that can go wrong without failing: sizes are in WORDS, a record whose rdSize is
 * below its own header never advances, and one that overruns the buffer reads
 * past it. The bytes below are the shape Win32 records for SelectObject(pen) +
 * Rectangle -- built by hand from the documented layout, not captured.
 */
static void put16(unsigned char *b, int o, unsigned v) { b[o] = (unsigned char)v; b[o + 1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *b, int o, unsigned long v)
{ put16(b, o, (unsigned)(v & 0xffff)); put16(b, o + 2, (unsigned)(v >> 16)); }
static void part5_metafile(void)
{
    unsigned char m[128];
    unsigned long first, end = 0, off, bytes = 0;
    unsigned nobj = 0, fn = 0, seq[8];
    int n = 0;
    printf("\n-- part 5: the Windows metafile walk (wowconv.h, #295) --\n");
    memset(m, 0, sizeof m);
    /* header: type 1, 9 words, version 0x300, mtSize, 1 object */
    put16(m, 0, 1); put16(m, 2, 9); put16(m, 4, 0x300);
    put16(m, 10, 1); put32(m, 12, 8); put16(m, 16, 0);
    off = 18;
    put32(m, off, 8); put16(m, off + 4, 0x02FA); off += 16;   /* CreatePenIndirect */
    put32(m, off, 4); put16(m, off + 4, 0x012D); off += 8;    /* SelectObject(0)   */
    put32(m, off, 7); put16(m, off + 4, 0x041B); off += 14;   /* Rectangle         */
    put32(m, off, 3); put16(m, off + 4, 0x0000); off += 6;    /* META_EOF          */
    put32(m, 6, (unsigned long)(off / 2));                      /* mtSize, in WORDS  */

    first = WowConvMetafileHeader(m, sizeof m, &nobj, &end);
    ok(first == 18, "WMF: the first record follows the 18-byte (9-WORD) header");
    ok(nobj == 1, "WMF: nObj is mtNoObjects -- the HANDLETABLE's length");
    ok(end == off, "WMF: the walk is bounded by mtSize*2, not by the buffer's slack");

    for (off = first; n < 8 && WowConvMetafileRecord(m, end, off, &bytes, &fn); off += bytes) {
        seq[n++] = fn;
        if (fn == 0) break;
    }
    ok(n == 4 && seq[0] == 0x02FA && seq[1] == 0x012D && seq[2] == 0x041B && seq[3] == 0,
       "WMF: four records in order -- rdSize is WORDS, so each step lands on a header");
    ok(bytes == 6, "WMF: META_EOF is a 3-WORD record");
    ok(!WowConvMetafileRecord(m, end, end, &bytes, &fn),
       "WMF: nothing is a record at the end of the metafile");

    /* Refusals: each would otherwise loop or read past the buffer. */
    put32(m, 18, 2);
    ok(!WowConvMetafileRecord(m, end, 18, &bytes, &fn),
       "WMF: rdSize < 3 is REFUSED -- it would never advance");
    put32(m, 18, 0x40000000UL);
    ok(!WowConvMetafileRecord(m, end, 18, &bytes, &fn),
       "WMF: a record claiming more than the metafile holds is REFUSED");
    put32(m, 18, 8);
    ok(!WowConvMetafileRecord(m, end, end - 4, &bytes, &fn),
       "WMF: fewer than 6 bytes left is not a record");
    put16(m, 2, 10);
    ok(WowConvMetafileHeader(m, sizeof m, &nobj, &end) == 0,
       "WMF: a header size other than 9 WORDS is not a WMF");
    put16(m, 2, 9); put16(m, 0, 3);
    ok(WowConvMetafileHeader(m, sizeof m, &nobj, &end) == 0,
       "WMF: mtType must be 1 (memory) or 2 (disk)");
    put16(m, 0, 2); put32(m, 6, 4);
    ok(WowConvMetafileHeader(m, sizeof m, &nobj, &end) == 0,
       "WMF: an mtSize smaller than the header itself is REFUSED");
    put32(m, 6, 0x1000);
    ok(WowConvMetafileHeader(m, 40, &nobj, &end) == 18 && end == 40,
       "WMF: an mtSize past the buffer is bounded by the buffer, never trusted");
    ok(WowConvMetafileHeader(m, 17, &nobj, &end) == 0,
       "WMF: fewer than 18 bytes has no header");
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
    part4_modal();
    part5_metafile();
    printf("\n%d checks, %d failed, %d skipped\n", pass + fail, fail, skip);
    return fail ? 1 : 0;
}
