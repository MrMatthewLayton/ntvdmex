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
 * No CRT: kernel32/user32/advapi32 only, like the rest of the host.
 */
#ifndef NTVDMEX_SETTINGS_H
#define NTVDMEX_SETTINGS_H

#include <windows.h>
#include "../../res/settings_ids.h"

#define NTVDMEX_REG_KEY "Software\\NTVDMEX"

typedef struct {
    DWORD show_host_cursor;   /* host arrow over the video area                     */
    DWORD blink_text_cursor;  /* text-mode cursor blinks, as a real CRTC does       */
    DWORD mouse_sens;         /* INT 33h motion scale, percent                      */
    DWORD dos_major;          /* reported MS-DOS version, e.g. 6                    */
    DWORD dos_minor;          /* ...and .22                                         */
    DWORD pit_pace;           /* pace the PIT from the 1 kHz thread (session 25)    */
    DWORD ui_tick_ms;         /* present/UI tick floor in ms                        */
} ntvdmex_settings;

/* The defaults ARE the shipped behaviour, with one deliberate exception: the host
   cursor now defaults to VISIBLE. It was hidden unconditionally long before it was a
   toggle, on the theory that it cost input lag; that was never measured, and a
   pointer you cannot see over the window is worse for every non-game guest. */
static void settings_defaults(ntvdmex_settings *s)
{
    s->show_host_cursor  = 1;
    s->blink_text_cursor = 1;
    s->mouse_sens        = 100;
    s->dos_major         = 6;
    s->dos_minor         = 22;
    s->pit_pace          = 1;
    s->ui_tick_ms        = 15;
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

/* HKEY_CURRENT_USER, not LOCAL_MACHINE: the VDM runs as the logged-in user and must
   not need administrator rights to remember a checkbox. */
static void settings_load(ntvdmex_settings *s)
{
    HKEY k;
    settings_defaults(s);
    if (RegOpenKeyExA(HKEY_CURRENT_USER, NTVDMEX_REG_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return;                                  /* never stored yet -> defaults      */
    s->show_host_cursor  = settings_reg_get(k, "ShowHostCursor",   s->show_host_cursor);
    s->blink_text_cursor = settings_reg_get(k, "BlinkTextCursor",  s->blink_text_cursor);
    s->mouse_sens        = settings_reg_get(k, "MouseSensitivity", s->mouse_sens);
    s->dos_major         = settings_reg_get(k, "DosVersionMajor",  s->dos_major);
    s->dos_minor         = settings_reg_get(k, "DosVersionMinor",  s->dos_minor);
    s->pit_pace          = settings_reg_get(k, "PitPace",          s->pit_pace);
    s->ui_tick_ms        = settings_reg_get(k, "UiTickMs",         s->ui_tick_ms);
    RegCloseKey(k);
    /* Clamp on the way IN, not only at the dialog. A hand-edited registry value of
       zero for the tick would spin the UI thread flat out. */
    if (s->mouse_sens < 10 || s->mouse_sens > 1000) s->mouse_sens = 100;
    if (s->ui_tick_ms < 1 || s->ui_tick_ms > 200)   s->ui_tick_ms = 15;
    if (s->dos_major < 1 || s->dos_major > 255)     { s->dos_major = 6; s->dos_minor = 22; }
    if (s->dos_minor > 99)                          s->dos_minor = 0;
}

static void settings_save(const ntvdmex_settings *s)
{
    HKEY k; DWORD disp = 0;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, NTVDMEX_REG_KEY, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &k, &disp) != ERROR_SUCCESS)
        return;
    settings_reg_put(k, "ShowHostCursor",   s->show_host_cursor);
    settings_reg_put(k, "BlinkTextCursor",  s->blink_text_cursor);
    settings_reg_put(k, "MouseSensitivity", s->mouse_sens);
    settings_reg_put(k, "DosVersionMajor",  s->dos_major);
    settings_reg_put(k, "DosVersionMinor",  s->dos_minor);
    settings_reg_put(k, "PitPace",          s->pit_pace);
    settings_reg_put(k, "UiTickMs",         s->ui_tick_ms);
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

#endif /* NTVDMEX_SETTINGS_H */
