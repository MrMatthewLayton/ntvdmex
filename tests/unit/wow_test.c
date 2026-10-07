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

static INT g_Passes, g_Failures, g_Skips;
static VOID WowTestCheck(INT condition, PCSTR description)
{
    if (condition) { ++g_Passes; printf("  PASS  %s\n", description); }
    else   { ++g_Failures; printf("  FAIL  %s\n", description); }
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
typedef struct { CHAR Name[64]; long Value; CHAR File[32]; INT Line; } WOW_TEST_DEFINITION;
static WOW_TEST_DEFINITION g_Definitions[MAXDEF];
static INT   g_DefinitionCount;

static PCSTR g_Headers[] = {
    "src/wow/wowuser.h", "src/wow/wowgdi.h", "src/wow/wow32.h",
    "src/wow/wowres.h",  "src/wow/wowcommdlg.h", "src/wow/wowshell.h",
};

/* A `#define <NAME>_ARG_<X> <number>` line, and nothing else. Deliberately
   strict: a macro defined to an expression is not an offset table entry and
   pretending otherwise would invent coverage. */
static VOID WowTestScanHeader(PCSTR root, PCSTR relativePath)
{
    CHAR path[512], line[1024];
    FILE *file;
    INT lineNumber = 0;
    snprintf(path, sizeof path, "%s/%s", root, relativePath);
    file = fopen(path, "r");
    if (!file) { ++g_Skips; printf("  SKIP  %s not found\n", relativePath); return; }
    while (fgets(line, sizeof line, file)) {
        CHAR name[128]; long value; PSTR cursor = line, nameCursor;
        ++lineNumber;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (strncmp(cursor, "#define", 7)) continue;
        cursor += 7;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        nameCursor = name;
        while (*cursor && *cursor != ' ' && *cursor != '\t' && (nameCursor - name) < (INT)sizeof name - 1)
            *nameCursor++ = *cursor++;
        *nameCursor = 0;
        if (!strstr(name, "_ARG_")) continue;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor == '(') continue;                 /* not a plain number */
        {   PSTR end; value = strtol(cursor, &end, 0);
            if (end == cursor) continue; }
        if (g_DefinitionCount >= MAXDEF) {
            /* Not `continue`. See the MAXDEF note: dropping a macro here makes
               part 2 report it as UNDEFINED, which sends the reader to the
               header to look for something that is already there. */
            fprintf(stderr, "wow_test: FATAL -- more than %d `*_ARG_*` macros; "
                            "raise MAXDEF. Every macro past the cap would be "
                            "reported as 'not defined'.\n", MAXDEF);
            exit(2);
        }
        snprintf(g_Definitions[g_DefinitionCount].Name, sizeof g_Definitions[g_DefinitionCount].Name, "%s", name);
        snprintf(g_Definitions[g_DefinitionCount].File, sizeof g_Definitions[g_DefinitionCount].File, "%s", relativePath);
        g_Definitions[g_DefinitionCount].Value = value;
        g_Definitions[g_DefinitionCount].Line = lineNumber;
        ++g_DefinitionCount;
    }
    fclose(file);
}

/* -1 when absent, so a table naming a macro that does not exist FAILS rather
   than silently tiling with a zero. */
static long WowTestDefinitionValue(PCSTR name, PINT found)
{
    INT index;
    if (found) *found = 0;
    for (index = 0; index < g_DefinitionCount; ++index)
        if (!strcmp(g_Definitions[index].Name, name)) {
            if (found) *found = 1;
            return g_Definitions[index].Value;
        }
    return -1;
}

static VOID WowTestMacroHygiene(VOID)
{
    INT first, second, duplicateCount = 0;
    CHAR description[256];
    printf("\n-- part 1: no `*_ARG_*` macro may be defined twice --\n");
    for (first = 0; first < g_DefinitionCount; ++first) {
        for (second = first + 1; second < g_DefinitionCount; ++second) {
            if (strcmp(g_Definitions[first].Name, g_Definitions[second].Name)) continue;
            ++duplicateCount;
            snprintf(description, sizeof description,
                     "%s defined twice: %s:%d = %ld and %s:%d = %ld"
                     " -- the LAST one wins at every use below it",
                     g_Definitions[first].Name, g_Definitions[first].File, g_Definitions[first].Line, g_Definitions[first].Value,
                     g_Definitions[second].File, g_Definitions[second].Line, g_Definitions[second].Value);
            WowTestCheck(0, description);
        }
    }
    if (!duplicateCount) {
        snprintf(description, sizeof description,
                 "%d argument-offset macros, all uniquely named", g_DefinitionCount);
        WowTestCheck(1, description);
    }
}

/* ── PART 2: the offset tables must tile their declared width ─────────────── */
typedef struct { PCSTR Macro; INT Size; } WOW_TEST_FIELD;
typedef struct {
    PCSTR Service;    /* what it is, for the failure message      */
    INT         Width;      /* argument BYTES, from tools/ne/neneeds.py  */
    WOW_TEST_FIELD     Fields[14];
} WOW_TEST_SERVICE;

/* ⚠ EVERY `width` HERE CAME OUT OF A REAL BINARY, via
     `tools/ne/neneeds.py guest/win16/<prog>.exe --todo`, which reads the
     argument-byte count off the module's own thunk. None of them is a guess. */
static const WOW_TEST_SERVICE g_Services[] = {
  /* --- the two that collided, and the reason part 1 exists --------------- */
  { "USER InvalidateRect(hWnd, lpRect, bErase)", 8,
    { {"WOWUSER_IR_ARG_ERASE",2}, {"WOWUSER_IR_ARG_RECT",4}, {"WOWUSER_IR_ARG_HWND",2}, {0,0} } },
  { "USER InvertRect(hDC, lpRect)", 6,
    { {"WOWUSER_INVR_ARG_RECT",4}, {"WOWUSER_INVR_ARG_HDC",2}, {0,0} } },
  { "USER SetMenu(hWnd, hMenu)", 4,
    { {"WOWUSER_SETMENU_ARG_MENU",2}, {"WOWUSER_SETMENU_ARG_HWND",2}, {0,0} } },

  /* --- session 50's new services ---------------------------------------- */
  { "USER SetTimer(hWnd, nIDEvent, wElapse, lpTimerFunc)", 10,
    { {"WOWUSER_ST_ARG_PROC",4}, {"WOWUSER_ST_ARG_ELAPSE",2}, {"WOWUSER_ST_ARG_ID",2},
      {"WOWUSER_ST_ARG_HWND",2}, {0,0} } },
  { "USER KillTimer(hWnd, nIDEvent)", 4,
    { {"WOWUSER_KT_ARG_ID",2}, {"WOWUSER_KT_ARG_HWND",2}, {0,0} } },
  { "USER FindWindow(lpClassName, lpWindowName)", 8,
    { {"WOWUSER_FW_ARG_NAME",4}, {"WOWUSER_FW_ARG_CLASS",4}, {0,0} } },
  { "USER FrameRect(hDC, lpRect, hBrush)", 8,
    { {"WOWUSER_FRAMER_ARG_BRUSH",2}, {"WOWUSER_FRAMER_ARG_RECT",4}, {"WOWUSER_FRAMER_ARG_HDC",2}, {0,0} } },
  { "USER FillRect(hDC, lpRect, hBrush)", 8,
    { {"WOWUSER_FR_ARG_BRUSH",2}, {"WOWUSER_FR_ARG_RECT",4}, {"WOWUSER_FR_ARG_HDC",2}, {0,0} } },
  { "USER DrawText(hDC, lpString, nCount, lpRect, uFormat)", 14,
    { {"WOWUSER_DT_ARG_FORMAT",2}, {"WOWUSER_DT_ARG_RECT",4}, {"WOWUSER_DT_ARG_COUNT",2},
      {"WOWUSER_DT_ARG_STR",4}, {"WOWUSER_DT_ARG_HDC",2}, {0,0} } },
  { "USER SetDlgItemText(hDlg, nIDDlgItem, lpString)", 8,
    { {"WOWUSER_SDIT_ARG_TEXT",4}, {"WOWUSER_SDIT_ARG_ID",2}, {"WOWUSER_SDIT_ARG_HDLG",2}, {0,0} } },
  { "USER GetDlgItemInt(hDlg, nID, lpTranslated, bSigned)", 10,
    { {"WOWUSER_GDII_ARG_SIGNED",2}, {"WOWUSER_GDII_ARG_XLATED",4}, {"WOWUSER_GDII_ARG_ID",2},
      {"WOWUSER_GDII_ARG_HDLG",2}, {0,0} } },
  { "USER CheckRadioButton(hDlg, first, last, check)", 8,
    { {"WOWUSER_CRB_ARG_CHECK",2}, {"WOWUSER_CRB_ARG_LAST",2}, {"WOWUSER_CRB_ARG_FIRST",2},
      {"WOWUSER_CRB_ARG_HDLG",2}, {0,0} } },
  { "USER CheckDlgButton(hDlg, nIDButton, uCheck)", 6,
    { {"WOWUSER_CDB_ARG_CHECK",2}, {"WOWUSER_CDB_ARG_ID",2}, {"WOWUSER_CDB_ARG_HDLG",2}, {0,0} } },
  { "USER IsDlgButtonChecked(hDlg, nIDButton)", 4,
    { {"WOWUSER_IDBC_ARG_ID",2}, {"WOWUSER_IDBC_ARG_HDLG",2}, {0,0} } },
  { "USER AdjustWindowRect(lpRect, dwStyle, bMenu)", 10,
    { {"WOWUSER_AWR_ARG_MENU",2}, {"WOWUSER_AWR_ARG_STYLE",4}, {"WOWUSER_AWR_ARG_RECT",4}, {0,0} } },
  { "USER LoadMenu downcall (0x96)", 16,
    { {"WOWUSER_LOADMENU_ARG_LOCAL0",2}, {"WOWUSER_LOADMENU_ARG_LOCAL2",2},
      {"WOWUSER_LOADMENU_ARG_LOCAL4",2}, {"WOWUSER_LOADMENU_ARG_RES",4},
      {"WOWUSER_LOADMENU_ARG_NAME",4}, {"WOWUSER_LOADMENU_ARG_HINST",2}, {0,0} } },
  { "USER GetLastActivePopup(hwndOwner)", 2,
    { {"WOWUSER_GLAP_ARG_HWND",2}, {0,0} } },

  /* --- long-standing ones, so the table is not only new code ------------- */
  { "USER GetClientRect(hWnd, lpRect)", 6,
    { {"WOWUSER_GCR_ARG_RECT",4}, {"WOWUSER_GCR_ARG_HWND",2}, {0,0} } },

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

static VOID WowTestOffsetTiling(VOID)
{
    UINT serviceIndex;
    CHAR description[320];
    printf("\n-- part 2: every argument offset table must tile its width --\n");
    for (serviceIndex = 0; serviceIndex < sizeof g_Services / sizeof g_Services[0]; ++serviceIndex) {
        const WOW_TEST_SERVICE *service = &g_Services[serviceIndex];
        BYTE cover[64];
        INT fieldIndex, isBad = 0, coveredBytes = 0, missingCount = 0;
        memset(cover, 0, sizeof cover);
        if (service->Width > (INT)sizeof cover) { ++g_Skips; continue; }
        for (fieldIndex = 0; fieldIndex < 14 && service->Fields[fieldIndex].Macro; ++fieldIndex) {
            INT found = 0, byteIndex;
            long offset = WowTestDefinitionValue(service->Fields[fieldIndex].Macro, &found);
            if (!found) {
                snprintf(description, sizeof description, "%s: macro %s is not defined",
                         service->Service, service->Fields[fieldIndex].Macro);
                WowTestCheck(0, description); ++missingCount; isBad = 1; continue;
            }
            coveredBytes += service->Fields[fieldIndex].Size;
            for (byteIndex = 0; byteIndex < service->Fields[fieldIndex].Size; ++byteIndex) {
                long byteOffset = offset + byteIndex;
                if (byteOffset < 0 || byteOffset >= service->Width) {
                    snprintf(description, sizeof description,
                             "%s: %s = %ld puts byte %ld outside the %d-byte block",
                             service->Service, service->Fields[fieldIndex].Macro, offset, byteOffset, service->Width);
                    WowTestCheck(0, description); isBad = 1; break;
                }
                if (cover[byteOffset]) {
                    snprintf(description, sizeof description,
                             "%s: %s = %ld OVERLAPS an earlier field at byte %ld",
                             service->Service, service->Fields[fieldIndex].Macro, offset, byteOffset);
                    WowTestCheck(0, description); isBad = 1; break;
                }
                cover[byteOffset] = 1;
            }
        }
        if (missingCount) continue;
        if (!isBad && coveredBytes != service->Width) {
            snprintf(description, sizeof description,
                     "%s: fields total %d bytes but the thunk declares %d",
                     service->Service, coveredBytes, service->Width);
            WowTestCheck(0, description); isBad = 1;
        }
        if (!isBad) {
            for (fieldIndex = 0; fieldIndex < service->Width; ++fieldIndex)
                if (!cover[fieldIndex]) {
                    snprintf(description, sizeof description,
                             "%s: byte %d of %d is named by no field",
                             service->Service, fieldIndex, service->Width);
                    WowTestCheck(0, description); isBad = 1; break;
                }
        }
        if (!isBad) {
            snprintf(description, sizeof description, "%s: %d bytes, tiled exactly",
                     service->Service, service->Width);
            WowTestCheck(1, description);
        }
    }
}

/* ── PART 3: the semantic deltas ─────────────────────────────────────────── */
static VOID WowTestConversions(VOID)
{
    BYTE core[12 + 16 * 3 + 8], output[40 + 256 * 4];
    UINT palette = 0, pixels;
    BYTE rect[WOWCONV_RECT16_SIZE];
    printf("\n-- part 3: the Win16/Win32 semantic deltas (wowconv.h) --\n");

    /* NUMCOLORS. The two call sites this has to satisfy are real and read out
       of the binaries: WINMINE `cmp ax,2 / jle` (signed) and SOL `cmp ax,2 /
       jne`. Both must land on "colour" for a modern display. */
    WowTestCheck(WowConvNumColors(1)  == 2,   "NUMCOLORS: 1bpp -> 2 (a mono device really is 2)");
    WowTestCheck(WowConvNumColors(4)  == 16,  "NUMCOLORS: 4bpp -> 16");
    WowTestCheck(WowConvNumColors(8)  == 256, "NUMCOLORS: 8bpp -> 256");
    WowTestCheck(WowConvNumColors(16) == 256, "NUMCOLORS: 16bpp -> 256, never -1");
    WowTestCheck(WowConvNumColors(32) == 256, "NUMCOLORS: 32bpp -> 256, never -1");
    WowTestCheck(WowConvNumColors(32) >  2,   "  ...and WINMINE's SIGNED `jle 2` takes the COLOUR branch");
    WowTestCheck(WowConvNumColors(32) != 2,   "  ...and SOL's `cmp ax,2 / jne` skips its mono flag");
    WowTestCheck(WowConvNumColors(1)  == 2,   "  ...while a REAL mono device still reports mono");

    /* hbrBackground: three cases, and 0 must stay 0. */
    WowTestCheck(WowConvBackgroundBrushKind(0) == WOWCONV_HBR_NONE,
       "hbrBackground: 0 is NO ERASE, not a default");
    WowTestCheck(WowConvBackgroundBrushKind(6) == WOWCONV_HBR_SYSCOLOR,
       "hbrBackground: 6 is COLOR_WINDOW+1, a system colour");
    WowTestCheck(WowConvBackgroundBrushKind(21) == WOWCONV_HBR_SYSCOLOR,
       "hbrBackground: 21 is COLOR_BTNHIGHLIGHT+1, the last Win3.1 index");
    WowTestCheck(WowConvBackgroundBrushKind(22) == WOWCONV_HBR_HANDLE,
       "hbrBackground: 22 is past the Win3.1 indices, so a real brush");
    WowTestCheck(WowConvBackgroundBrushKind(0x2018) == WOWCONV_HBR_HANDLE,
       "hbrBackground: a GDI token is a real brush");

    /* Win16 RECT: 8 bytes, four SIGNED 16-bit ints. */
    WowConvRect16Put(rect, 0, -2);
    WowConvRect16Put(rect, 1, -48);
    WowConvRect16Put(rect, 2, 1680);
    WowConvRect16Put(rect, 3, 974);
    WowTestCheck(WowConvRect16Get(rect, 0) == -2 && WowConvRect16Get(rect, 1) == -48,
       "RECT16: negative left/top survive the round trip (Minesweeper's -2,-48)");
    WowTestCheck(WowConvRect16Get(rect, 2) == 1680 && WowConvRect16Get(rect, 3) == 974,
       "RECT16: 1680x974 survives the round trip");
    WowTestCheck(WOWCONV_RECT16_SIZE == 8,
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
    pixels = WowConvDibCoreToInfo(core, sizeof core, output, sizeof output, &palette);
    WowTestCheck(pixels == 12 + 16 * 3, "DIB core: pixels start after 16 RGBTRIPLEs (3 bytes each)");
    WowTestCheck(palette == 16,          "DIB core: 4bpp means a full 16-entry table, no biClrUsed");
    WowTestCheck(output[0] == 40,       "DIB core: biSize becomes 40");
    WowTestCheck(output[4] == 0x47 && output[5] == 0 && output[8] == 0x60 && output[9] == 0,
       "DIB core: bcWidth/bcHeight widen to 32-bit WITHOUT spanning each other");
    WowTestCheck(output[14] == 4,       "DIB core: biBitCount carried across");
    WowTestCheck(output[40] == 0x11 && output[41] == 0x22 && output[42] == 0x33 && output[43] == 0,
       "DIB core: palette entry 0 widens RGBTRIPLE -> RGBQUAD");
    WowTestCheck(output[44] == 0x44 && output[45] == 0x55 && output[46] == 0x66 && output[47] == 0,
       "DIB core: entry 1 lands at the RGBQUAD stride, not the RGBTRIPLE one");

    /* Refusals. Guessing at a format is worse than declining it. */
    core[0] = 40;
    WowTestCheck(WowConvDibCoreToInfo(core, sizeof core, output, sizeof output, &palette) == 0,
       "DIB core: a 40-byte header is REFUSED here, not silently converted");
    core[0] = 12; core[10] = 7;          /* 7bpp does not exist */
    WowTestCheck(WowConvDibCoreToInfo(core, sizeof core, output, sizeof output, &palette) == 0,
       "DIB core: a bit count that cannot exist is REFUSED");
    core[10] = 4;
    WowTestCheck(WowConvDibCoreToInfo(core, 12, output, sizeof output, &palette) == 0,
       "DIB core: a length with no room for pixels is REFUSED");
    WowTestCheck(WowConvDibCoreToInfo(core, sizeof core, output, 8, &palette) == 0,
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
static VOID WowTestModal(VOID)
{
    INT isEnded, isAlive, hasProcedure, isExpired, runCount = 0, seen[5];
    printf("\n-- part 4: the modal dialog loop (wowconv.h, src/wow/wowdlg.h) --\n");

    /* 1. The procedure rule. */
    WowTestCheck(WowConvWindowProcedure(0x11110000u, 0) == 0x11110000u,
       "winproc: a class window procedure drives its window");
    WowTestCheck(WowConvWindowProcedure(0, 0x22220000u) == 0x22220000u,
       "winproc: a #32770 dialog has only a DLGPROC, and it drives it");
    WowTestCheck(WowConvWindowProcedure(0x11110000u, 0x22220000u) == 0x11110000u,
       "winproc: with BOTH, the class procedure wins (CALC's SciCalc dialog)");
    WowTestCheck(WowConvWindowProcedure(0, 0) == 0,
       "winproc: neither means NOTHING can be told about this window");

    /* 2. The exit rule, case by case. */
    WowTestCheck(WowConvModalExit(0, 1, 1, 0) == WOWCONV_MODAL_RUN,
       "modal: alive, drivable, not ended, not expired -> KEEP PUMPING");
    WowTestCheck(WowConvModalExit(1, 1, 1, 0) == WOWCONV_MODAL_END,
       "modal: EndDialog -> the caller gets nResult");
    WowTestCheck(WowConvModalExit(0, 0, 1, 0) == WOWCONV_MODAL_GONE,
       "modal: the window was destroyed -> DialogBox returns 0, never waits");
    WowTestCheck(WowConvModalExit(0, 1, 0, 0) == WOWCONV_MODAL_NOPROC,
       "modal: nothing to dispatch to -> return 0 AT ONCE (s56's behaviour)");
    WowTestCheck(WowConvModalExit(0, 1, 1, 1) == WOWCONV_MODAL_EXPIRED,
       "modal: the bounded input wait ran out -> return 0, so a harness ends");

    /* ★ THE ORDER, WHICH IS THE ONE THING A READER WOULD GET WRONG. A dialog
         procedure that calls EndDialog and whose window is then torn down has
         ANSWERED; reporting GONE there would throw away the OK the user just
         clicked and hand back 0 instead. */
    WowTestCheck(WowConvModalExit(1, 0, 1, 0) == WOWCONV_MODAL_END,
       "modal: ENDED OUTRANKS a destroyed window -- the answer is not lost");
    WowTestCheck(WowConvModalExit(1, 0, 0, 1) == WOWCONV_MODAL_END,
       "modal: ...and outranks every other reason to stop, together");
    WowTestCheck(WowConvModalExit(0, 0, 0, 1) == WOWCONV_MODAL_GONE,
       "modal: a gone window outranks having no procedure and no input");
    WowTestCheck(WowConvModalExit(0, 1, 0, 1) == WOWCONV_MODAL_NOPROC,
       "modal: an undrivable dialog is refused BEFORE it can time out");

    /* ★★ TOTALITY. Sixteen combinations, every one of them classified, and RUN
         reachable from exactly one -- the one where the dialog is alive,
         drivable, unfinished and has not run out of time. A new fact added to
         this decision without a rule for it would show up here as a hang. */
    for (isExpired = 0; isExpired < 5; ++isExpired) seen[isExpired] = 0;
    for (isEnded = 0; isEnded < 2; ++isEnded) for (isAlive = 0; isAlive < 2; ++isAlive)
    for (hasProcedure = 0; hasProcedure < 2; ++hasProcedure) for (isExpired = 0; isExpired < 2; ++isExpired) {
        INT verdict = WowConvModalExit(isEnded, isAlive, hasProcedure, isExpired);
        if (verdict < 0 || verdict > 4) { WowTestCheck(0, "modal: a verdict outside the five"); return; }
        ++seen[verdict];
        if (verdict == WOWCONV_MODAL_RUN) ++runCount;
    }
    WowTestCheck(runCount == 1, "modal: EXACTLY ONE of the 16 states keeps pumping");
    WowTestCheck(seen[WOWCONV_MODAL_END] + seen[WOWCONV_MODAL_GONE]
       + seen[WOWCONV_MODAL_NOPROC] + seen[WOWCONV_MODAL_EXPIRED] == 15,
       "modal: the other 15 all LEAVE -- the loop has no state that hangs");
    WowTestCheck(seen[WOWCONV_MODAL_END] == 8,
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
static VOID WowTestPut16(PBYTE bytes, INT offset, UINT value) { bytes[offset] = (BYTE)value; bytes[offset + 1] = (BYTE)(value >> 8); }
static VOID WowTestPut32(PBYTE bytes, INT offset, unsigned long value)
{ WowTestPut16(bytes, offset, (UINT)(value & 0xffff)); WowTestPut16(bytes, offset + 2, (UINT)(value >> 16)); }
static VOID WowTestMetafile(VOID)
{
    BYTE metafile[128];
    unsigned long first, end = 0, offset, bytes = 0;
    UINT objectCount = 0, function = 0, sequence[8];
    INT count = 0;
    printf("\n-- part 5: the Windows metafile walk (wowconv.h, #295) --\n");
    memset(metafile, 0, sizeof metafile);
    /* header: type 1, 9 words, version 0x300, mtSize, 1 object */
    WowTestPut16(metafile, 0, 1); WowTestPut16(metafile, 2, 9); WowTestPut16(metafile, 4, 0x300);
    WowTestPut16(metafile, 10, 1); WowTestPut32(metafile, 12, 8); WowTestPut16(metafile, 16, 0);
    offset = 18;
    WowTestPut32(metafile, offset, 8); WowTestPut16(metafile, offset + 4, 0x02FA); offset += 16;   /* CreatePenIndirect */
    WowTestPut32(metafile, offset, 4); WowTestPut16(metafile, offset + 4, 0x012D); offset += 8;    /* SelectObject(0)   */
    WowTestPut32(metafile, offset, 7); WowTestPut16(metafile, offset + 4, 0x041B); offset += 14;   /* Rectangle         */
    WowTestPut32(metafile, offset, 3); WowTestPut16(metafile, offset + 4, 0x0000); offset += 6;    /* META_EOF          */
    WowTestPut32(metafile, 6, (unsigned long)(offset / 2));                      /* mtSize, in WORDS  */

    first = WowConvMetafileHeader(metafile, sizeof metafile, &objectCount, &end);
    WowTestCheck(first == 18, "WMF: the first record follows the 18-byte (9-WORD) header");
    WowTestCheck(objectCount == 1, "WMF: nObj is mtNoObjects -- the HANDLETABLE's length");
    WowTestCheck(end == offset, "WMF: the walk is bounded by mtSize*2, not by the buffer's slack");

    for (offset = first; count < 8 && WowConvMetafileRecord(metafile, end, offset, &bytes, &function); offset += bytes) {
        sequence[count++] = function;
        if (function == 0) break;
    }
    WowTestCheck(count == 4 && sequence[0] == 0x02FA && sequence[1] == 0x012D && sequence[2] == 0x041B && sequence[3] == 0,
       "WMF: four records in order -- rdSize is WORDS, so each step lands on a header");
    WowTestCheck(bytes == 6, "WMF: META_EOF is a 3-WORD record");
    WowTestCheck(!WowConvMetafileRecord(metafile, end, end, &bytes, &function),
       "WMF: nothing is a record at the end of the metafile");

    /* Refusals: each would otherwise loop or read past the buffer. */
    WowTestPut32(metafile, 18, 2);
    WowTestCheck(!WowConvMetafileRecord(metafile, end, 18, &bytes, &function),
       "WMF: rdSize < 3 is REFUSED -- it would never advance");
    WowTestPut32(metafile, 18, 0x40000000UL);
    WowTestCheck(!WowConvMetafileRecord(metafile, end, 18, &bytes, &function),
       "WMF: a record claiming more than the metafile holds is REFUSED");
    WowTestPut32(metafile, 18, 8);
    WowTestCheck(!WowConvMetafileRecord(metafile, end, end - 4, &bytes, &function),
       "WMF: fewer than 6 bytes left is not a record");
    WowTestPut16(metafile, 2, 10);
    WowTestCheck(WowConvMetafileHeader(metafile, sizeof metafile, &objectCount, &end) == 0,
       "WMF: a header size other than 9 WORDS is not a WMF");
    WowTestPut16(metafile, 2, 9); WowTestPut16(metafile, 0, 3);
    WowTestCheck(WowConvMetafileHeader(metafile, sizeof metafile, &objectCount, &end) == 0,
       "WMF: mtType must be 1 (memory) or 2 (disk)");
    WowTestPut16(metafile, 0, 2); WowTestPut32(metafile, 6, 4);
    WowTestCheck(WowConvMetafileHeader(metafile, sizeof metafile, &objectCount, &end) == 0,
       "WMF: an mtSize smaller than the header itself is REFUSED");
    WowTestPut32(metafile, 6, 0x1000);
    WowTestCheck(WowConvMetafileHeader(metafile, 40, &objectCount, &end) == 18 && end == 40,
       "WMF: an mtSize past the buffer is bounded by the buffer, never trusted");
    WowTestCheck(WowConvMetafileHeader(metafile, 17, &objectCount, &end) == 0,
       "WMF: fewer than 18 bytes has no header");
}

INT main(INT argc, PSTR *argv)
{
    PCSTR root = (argc > 1) ? argv[1] : "../..";
    UINT index;
    printf("== WOW32 translation-layer battery (GH #128) ==\n");
    for (index = 0; index < sizeof g_Headers / sizeof g_Headers[0]; ++index)
        WowTestScanHeader(root, g_Headers[index]);
    if (!g_DefinitionCount) {
        printf("  FAIL  no argument-offset macros found under %s -- wrong root?\n", root);
        return 1;
    }
    WowTestMacroHygiene();
    WowTestOffsetTiling();
    WowTestConversions();
    WowTestModal();
    WowTestMetafile();
    printf("\n%d checks, %d failed, %d skipped\n", g_Passes + g_Failures, g_Failures, g_Skips);
    return g_Failures ? 1 : 0;
}
