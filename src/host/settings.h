/* settings.h -- NTVDMEX settings, stored in the Windows registry.
 *
 * WHY THIS EXISTS. Every knob in this host arrived as a TEXT FILE on the test share:
 * pitpace.txt, msens.txt, uitick.txt, dosver.txt and a dozen more. That is a fine
 * harness mechanism -- a headless run can write a file and re-launch -- and a poor
 * user interface: nobody at the machine should have to know that "0x20" in qimode.txt
 * turns on a synthetic key script. The registry is where a Windows program of this
 * era keeps its configuration, so that is where these live.
 *
 * ── THE TEXT FILES STILL WIN, AND THAT IS DELIBERATE. ────────────────────────────
 * Precedence is: built-in default  <  registry  <  text file on the share.
 * The rig drives this host by writing those files and re-launching it; if the
 * registry silently overrode them, every headless measurement would start reporting
 * whatever was last clicked in a dialog on that machine -- which is exactly the class
 * of "the instrument lied" failure this project keeps paying for. A file present on
 * the share is an explicit instruction from a test, so it outranks the stored setting.
 * Nothing in the harness had to change for these to be added.
 *
 * ── ONE TABLE, FOUR JOBS. ────────────────────────────────────────────────────────
 * Every setting is one row of SET_DEFS: registry name, dialog control, kind, default,
 * range, and (for a combo) its items. Defaults, registry load, registry save, clamp,
 * dialog fill and dialog read are all LOOPS OVER THAT TABLE. The first cut of this
 * file wrote each of those out by hand per setting; at seven settings that was fine,
 * and at forty-odd it is four places to forget the same knob. Adding a setting is now
 * one row plus one enum member plus one line of layout in ntvdmhost.rc.
 *
 * ⚠ MOST OF THESE ARE STORED BUT NOT YET HONOURED. The CPU, Display, Audio, Drives
 *   pages and parts of Input came from the menu scaffold, where they were IDM_STUB.
 *   They now round-trip through HKCU faithfully -- and nothing downstream reads them
 *   yet. settings_apply() in main.c is the ONLY place a stored value reaches the
 *   machine, so that function is the honest list of what actually works; a setting
 *   absent from it is a setting the emulator does not consult. Wiring one up means
 *   adding a line there, not adding storage.
 *
 * No CRT: kernel32/user32/advapi32 only, like the rest of the host.
 */
#ifndef NTVDMEX_SETTINGS_H
#define NTVDMEX_SETTINGS_H

#include <windows.h>
#include "../../res/settings_ids.h"

#define NTVDMEX_REG_KEY "Software\\NTVDMEX"
#define NTVDMEX_PATH_MAX 260

/* ── THE NUMERIC SETTINGS. ───────────────────────────────────────────────────────
     Order here must match SET_DEFS below; SET_COUNT closes the array and is what
     sizes the value block, so a row added to one and not the other fails to build. */
typedef enum {
    SET_DOSMAJ = 0, SET_DOSMIN, SET_PITPACE, SET_UITICK,
    SET_CPUTYPE, SET_CPUCORE, SET_FPU, SET_SPEEDMODE, SET_CYCLES, SET_TURBO,
    SET_CONVKB, SET_XMS, SET_EMS, SET_UMB, SET_A20,
    SET_WINSIZE, SET_RENDERER, SET_SCALER, SET_FILTER, SET_ASPECT,
    SET_FRAMESKIP, SET_VSYNC, SET_BLINKCURSOR,
    SET_VOLUME, SET_MUTE, SET_RATE, SET_SBMODEL, SET_SBADDR, SET_SBIRQ, SET_SBDMA,
    SET_OPL, SET_MIDI, SET_SPEAKER, SET_GUS, SET_TANDY,
    SET_HOSTCURSOR, SET_SEAMLESS, SET_MSENS, SET_KBLAYOUT, SET_TYPEMATIC,
    SET_JOYTYPE, SET_JOYPAD,
    SET_BOOTFROM,
    SET_COUNT
} set_id;

/* ── THE STRING SETTINGS, kept separate because REG_SZ is a different call. ───── */
typedef enum {
    SET_STR_DRIVEC = 0, SET_STR_FLOPPYA, SET_STR_CDROM, SET_STR_SOUNDFONT,
    SET_STR_COUNT
} set_str_id;

enum {
    SK_CHECK = 0,   /* checkbox -> 0 or 1                                        */
    SK_UINT,        /* edit box -> unsigned, clamped to [lo,hi]                  */
    SK_COMBO,       /* drop-list -> zero-based index, clamped to [0,hi]          */
    SK_VER,         /* editable combo holding "major.minor" -- writes TWO rows   */
    SK_DERIVED      /* no control of its own; written by the SK_VER row above it */
};

typedef struct {
    const char *reg;    /* registry value name                                   */
    int         ctl;    /* dialog control id (0 for SK_DERIVED)                  */
    int         kind;   /* SK_*                                                  */
    DWORD       dflt;   /* default value (an INDEX for SK_COMBO)                 */
    DWORD       lo, hi; /* inclusive clamp; for SK_COMBO hi = last valid index    */
    const char *items;  /* SK_COMBO: '|'-separated item text, else NULL          */
} set_def;

typedef struct {
    const char *reg;
    int         ctl;
    const char *dflt;
} set_str_def;

/* ⚠ SB defaults are the card we actually emulate (vdd_sb.h: base 0x220, IRQ 5,
     DMA 1/5) and the audio path really does run at 44100 (audio_wave.h). They are
     not folklore: a default that disagrees with the hardware would have the dialog
     describing a machine that does not exist. */
static const set_def SET_DEFS[SET_COUNT] = {
/*  registry name        control            kind        dflt lo   hi    items */
{ "DosVersionMajor",   IDC_S_DOSVER,      SK_VER,        6,  1, 255, NULL },
{ "DosVersionMinor",   0,                 SK_DERIVED,   22,  0,  99, NULL },
{ "PitPace",           IDC_S_PITPACE,     SK_CHECK,      1,  0,   1, NULL },
{ "UiTickMs",          IDC_S_UITICK,      SK_UINT,      15,  1, 200, NULL },

{ "CpuType",           IDC_S_CPUTYPE,     SK_COMBO,      2,  0,   4, "8086|286|386|486|Pentium" },
{ "CpuCore",           IDC_S_CPUCORE,     SK_COMBO,      0,  0,   3, "Auto|Normal|Dynamic|Simple" },
{ "Fpu",               IDC_S_FPU,         SK_CHECK,      1,  0,   1, NULL },
{ "SpeedMode",         IDC_S_SPEEDMODE,   SK_COMBO,      0,  0,   2, "Auto|Maximum|Fixed cycles" },
{ "Cycles",            IDC_S_CYCLES,      SK_UINT,    3000, 100, 1000000, NULL },
{ "Turbo",             IDC_S_TURBO,       SK_CHECK,      0,  0,   1, NULL },
{ "ConventionalKB",    IDC_S_CONVKB,      SK_UINT,     640, 64,  640, NULL },
{ "Xms",               IDC_S_XMS,         SK_CHECK,      1,  0,   1, NULL },
{ "Ems",               IDC_S_EMS,         SK_CHECK,      1,  0,   1, NULL },
{ "Umb",               IDC_S_UMB,         SK_CHECK,      1,  0,   1, NULL },
{ "A20",               IDC_S_A20,         SK_CHECK,      1,  0,   1, NULL },

{ "WindowSize",        IDC_S_WINSIZE,     SK_COMBO,      1,  0,   3, "1x|2x|3x|Custom" },
{ "Renderer",          IDC_S_RENDERER,    SK_COMBO,      0,  0,   3, "GDI|DirectDraw|Direct3D 9|OpenGL" },
{ "Scaler",            IDC_S_SCALER,      SK_COMBO,      0,  0,   4, "None|Scale2x|hq2x|Scanlines|CRT" },
{ "Filtering",         IDC_S_FILTER,      SK_COMBO,      0,  0,   1, "Nearest|Bilinear" },
{ "AspectRatio",       IDC_S_ASPECT,      SK_CHECK,      0,  0,   1, NULL },
{ "FrameSkip",         IDC_S_FRAMESKIP,   SK_COMBO,      0,  0,   2, "0|1|2" },
{ "VSync",             IDC_S_VSYNC,       SK_CHECK,      1,  0,   1, NULL },
{ "BlinkTextCursor",   IDC_S_BLINKCURSOR, SK_CHECK,      1,  0,   1, NULL },

{ "MasterVolume",      IDC_S_VOLUME,      SK_UINT,     100,  0, 100, NULL },
{ "Mute",              IDC_S_MUTE,        SK_CHECK,      0,  0,   1, NULL },
{ "SampleRate",        IDC_S_RATE,        SK_COMBO,      1,  0,   2, "22050|44100|48000" },
{ "SbModel",           IDC_S_SBMODEL,     SK_COMBO,      0,  0,   2, "SB16|AWE32|SB Pro" },
{ "SbAddress",         IDC_S_SBADDR,      SK_COMBO,      0,  0,   3, "220|240|260|280" },
{ "SbIrq",             IDC_S_SBIRQ,       SK_COMBO,      0,  0,   3, "5|7|10|11" },
{ "SbDma",             IDC_S_SBDMA,       SK_COMBO,      0,  0,   2, "1|3|5" },
{ "Opl",               IDC_S_OPL,         SK_COMBO,      1,  0,   1, "OPL2|OPL3" },
{ "Midi",              IDC_S_MIDI,        SK_COMBO,      0,  0,   2, "Host GM|MT-32|SoundFont" },
{ "PcSpeaker",         IDC_S_SPEAKER,     SK_CHECK,      1,  0,   1, NULL },
{ "Gus",               IDC_S_GUS,         SK_CHECK,      0,  0,   1, NULL },
{ "Tandy",             IDC_S_TANDY,       SK_CHECK,      0,  0,   1, NULL },

{ "ShowHostCursor",    IDC_S_HOSTCURSOR,  SK_CHECK,      1,  0,   1, NULL },
{ "SeamlessMouse",     IDC_S_SEAMLESS,    SK_CHECK,      0,  0,   1, NULL },
{ "MouseSensitivity",  IDC_S_MSENS,       SK_UINT,     100, 10, 1000, NULL },
{ "KeyboardLayout",    IDC_S_KBLAYOUT,    SK_COMBO,      0,  0,   3, "US|United Kingdom|German|French" },
{ "TypematicRate",     IDC_S_TYPEMATIC,   SK_UINT,      10,  2,  30, NULL },
{ "JoystickType",      IDC_S_JOYTYPE,     SK_COMBO,      0,  0,   2, "None|2 axis, 2 button|4 axis, 4 button" },
{ "JoystickGamepad",   IDC_S_JOYPAD,      SK_CHECK,      0,  0,   1, NULL },

{ "BootFrom",          IDC_S_BOOTFROM,    SK_COMBO,      0,  0,   2, "Hard disk (C:)|Floppy (A:)|CD-ROM (D:)" },
};

static const set_str_def SET_STR_DEFS[SET_STR_COUNT] = {
{ "DriveCPath",   IDC_S_DRIVEC,    "" },
{ "FloppyAImage", IDC_S_FLOPPYA,   "" },
{ "CdRomImage",   IDC_S_CDROM,     "" },
{ "SoundFontPath",IDC_S_SOUNDFONT, "" },
};

typedef struct {
    DWORD v[SET_COUNT];
    char  s[SET_STR_COUNT][NTVDMEX_PATH_MAX];
} ntvdmex_settings;

/* ── Named access, so callers read like they used to. ───────────────────────────
     settings_apply() says g_set.v[SET_MSENS], not g_set.v[37]. */
#define SETV(s, id)  ((s)->v[(id)])

static void settings_strcpy(char *dst, const char *src, int cap)
{
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

/* The defaults ARE the shipped behaviour, with one deliberate exception: the host
   cursor defaults to VISIBLE. It was hidden unconditionally long before it was a
   toggle, on the theory that it cost input lag; that was never measured, and a
   pointer you cannot see over the window is worse for every non-game guest. */
static void settings_defaults(ntvdmex_settings *s)
{
    int i;
    for (i = 0; i < SET_COUNT; ++i) s->v[i] = SET_DEFS[i].dflt;
    for (i = 0; i < SET_STR_COUNT; ++i)
        settings_strcpy(s->s[i], SET_STR_DEFS[i].dflt, NTVDMEX_PATH_MAX);
}

/* Clamp on the way IN, not only at the dialog. A hand-edited registry value of zero
   for the UI tick would spin the UI thread flat out, and an out-of-range combo index
   would select nothing at all -- the control would come up blank and OK would then
   write the blank back. */
static void settings_clamp(ntvdmex_settings *s)
{
    int i;
    for (i = 0; i < SET_COUNT; ++i) {
        const set_def *d = &SET_DEFS[i];
        if (d->kind == SK_COMBO) { if (s->v[i] > d->hi) s->v[i] = d->dflt; }
        else if (s->v[i] < d->lo || s->v[i] > d->hi) s->v[i] = d->dflt;
    }
    /* A version is a PAIR: an out-of-range major that fell back to 6 with a minor of
       00 would report "6.00", a DOS that never shipped. Reset both together. */
    if (s->v[SET_DOSMAJ] == SET_DEFS[SET_DOSMAJ].dflt
        && s->v[SET_DOSMIN] > SET_DEFS[SET_DOSMIN].hi)
        s->v[SET_DOSMIN] = SET_DEFS[SET_DOSMIN].dflt;
}

static DWORD settings_reg_get(HKEY k, const char *name, DWORD dflt)
{
    DWORD v = 0, cb = sizeof v, type = 0;
    if (RegQueryValueExA(k, name, NULL, &type, (BYTE *)&v, &cb) == ERROR_SUCCESS
        && type == REG_DWORD && cb == sizeof v)
        return v;
    return dflt;
}

static void settings_reg_put(HKEY k, const char *name, DWORD v)
{
    RegSetValueExA(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof v);
}

static void settings_reg_get_sz(HKEY k, const char *name, char *out, int cap)
{
    DWORD cb = (DWORD)cap, type = 0;
    if (RegQueryValueExA(k, name, NULL, &type, (BYTE *)out, &cb) != ERROR_SUCCESS
        || type != REG_SZ || cb == 0)
        return;                                  /* leave the default in place        */
    if ((int)cb >= cap) cb = (DWORD)cap - 1;
    out[cb] = 0;                                 /* RegQueryValueEx may not NUL it    */
}

/* HKEY_CURRENT_USER, not LOCAL_MACHINE: the VDM runs as the logged-in user and must
   not need administrator rights to remember a checkbox. */
static void settings_load(ntvdmex_settings *s)
{
    HKEY k; int i;
    settings_defaults(s);
    if (RegOpenKeyExA(HKEY_CURRENT_USER, NTVDMEX_REG_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return;                                  /* never stored yet -> defaults      */
    for (i = 0; i < SET_COUNT; ++i)
        s->v[i] = settings_reg_get(k, SET_DEFS[i].reg, s->v[i]);
    for (i = 0; i < SET_STR_COUNT; ++i)
        settings_reg_get_sz(k, SET_STR_DEFS[i].reg, s->s[i], NTVDMEX_PATH_MAX);
    RegCloseKey(k);
    settings_clamp(s);
}

static void settings_save(const ntvdmex_settings *s)
{
    HKEY k; DWORD disp = 0; int i;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, NTVDMEX_REG_KEY, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &k, &disp) != ERROR_SUCCESS)
        return;
    for (i = 0; i < SET_COUNT; ++i)
        settings_reg_put(k, SET_DEFS[i].reg, s->v[i]);
    for (i = 0; i < SET_STR_COUNT; ++i) {
        int n = 0; while (s->s[i][n]) ++n;
        RegSetValueExA(k, SET_STR_DEFS[i].reg, 0, REG_SZ,
                       (const BYTE *)s->s[i], (DWORD)n + 1);
    }
    RegCloseKey(k);
}

/* ── THE VERSION FIELD IS "major.minor" TEXT, PARSED LENIENTLY. ──────────────────
     It is a combo the user can type into, because the list can never be complete:
     6.22 is the oracle, 5.00 is what XP's own COMMAND.COM demands, and the next
     guest that refuses to run will want some third number. Accepts "5", "5.0",
     "5.00", " 6.22 ". */
static void settings_parse_ver(const char *t, DWORD *maj, DWORD *min)
{
    DWORD a = 0, b = 0; int i = 0, seen = 0;
    while (t[i] == ' ' || t[i] == '\t') ++i;
    while (t[i] >= '0' && t[i] <= '9') { a = a * 10 + (DWORD)(t[i] - '0'); ++i; seen = 1; }
    if (t[i] == '.') {
        ++i;
        while (t[i] >= '0' && t[i] <= '9') { b = b * 10 + (DWORD)(t[i] - '0'); ++i; }
    }
    if (!seen || a < 1 || a > 255 || b > 99) return;   /* keep the previous value */
    *maj = a; *min = b;
}

/* Parse an unsigned decimal out of an edit box. Returns 0 on "nothing usable here",
   which the caller treats as "keep the value you already had" -- an ES_NUMBER edit
   can still be EMPTY, and an empty box must not read as zero. */
static int settings_atou(const char *t, DWORD *out)
{
    DWORD a = 0; int i = 0, seen = 0;
    while (t[i] == ' ' || t[i] == '\t') ++i;
    while (t[i] >= '0' && t[i] <= '9') {
        a = a * 10 + (DWORD)(t[i] - '0');
        if (a > 100000000u) return 0;                /* absurd: reject, don't wrap */
        ++i; seen = 1;
    }
    if (!seen) return 0;
    *out = a;
    return 1;
}

/* Copy item n of a '|'-separated list into out. Returns 0 when n is past the end,
   which is how the combo-filling loop knows to stop -- the table stores the text,
   not a count, so there is only one place to edit when a list gains an entry. */
static int settings_item(const char *items, int n, char *out, int cap)
{
    int i = 0, k = 0;
    if (!items) return 0;
    while (n > 0 && items[i]) { if (items[i] == '|') --n; ++i; }
    if (!items[i]) return 0;
    while (items[i] && items[i] != '|' && k < cap - 1) out[k++] = items[i++];
    out[k] = 0;
    return 1;
}

#endif /* NTVDMEX_SETTINGS_H */
