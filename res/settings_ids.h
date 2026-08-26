/* settings_ids.h -- dialog + control IDs for the tabbed Settings dialog.
 *
 * Included by BOTH res/ntvdmhost.rc (which windres compiles) and src/host/settings.h.
 * That is why it contains nothing but #defines: windres understands the preprocessor
 * and nothing else, so a stray declaration here breaks the resource build rather than
 * the C build, which is a confusing place to find out.
 *
 * ── THE SHAPE. ──────────────────────────────────────────────────────────────────
 * IDD_SETTINGS is a frame: a tab control, OK, Cancel and Restore Defaults. Each tab
 * is its OWN child dialog resource (IDD_PAGE_*, style DS_CONTROL | WS_CHILD), created
 * at WM_INITDIALOG and parked in the tab's display rectangle. That is the standard
 * Win32 tab-with-pages pattern and it keeps each page's layout in one readable block
 * rather than one 200-control dialog with a visibility rule per control.
 *
 * Control IDs are unique ACROSS ALL PAGES, not just within one. settings.h looks a
 * control up by asking every page for it (see settings_ctl in main.c), so a duplicate
 * ID would silently resolve to whichever page was searched first.
 */
#ifndef NTVDMEX_SETTINGS_IDS_H
#define NTVDMEX_SETTINGS_IDS_H

#define IDD_SETTINGS          200
#define IDC_S_TAB             201   /* the SysTabControl32 that owns the pages        */
#define IDC_S_DEFAULTS        202   /* button:   restore every page's defaults        */

#define IDD_PAGE_GENERAL      210
#define IDD_PAGE_CPU          211
#define IDD_PAGE_DISPLAY      212
#define IDD_PAGE_AUDIO        213
#define IDD_PAGE_INPUT        214
#define IDD_PAGE_DRIVES       215
#define NTVDMEX_PAGE_COUNT      6

/* ── General ──────────────────────────────────────────────────────────────────── */
#define IDC_S_DOSVER          230   /* combo:    reported MS-DOS version, "6.22"      */
#define IDC_S_DOSVER_NOTE     231   /* static:   why the version is a knob            */
#define IDC_S_PITPACE         232   /* checkbox: pace the PIT from a 1 kHz thread     */
#define IDC_S_UITICK          233   /* edit:     UI/present tick floor, milliseconds  */

/* ── CPU ──────────────────────────────────────────────────────────────────────── */
#define IDC_S_CPUTYPE         240
#define IDC_S_CPUCORE         241
#define IDC_S_FPU             242
#define IDC_S_SPEEDMODE       243
#define IDC_S_CYCLES          244
#define IDC_S_TURBO           245
#define IDC_S_CONVKB          246
#define IDC_S_XMS             247
#define IDC_S_EMS             248
#define IDC_S_UMB             249
#define IDC_S_A20             250

/* ── Display ──────────────────────────────────────────────────────────────────── */
#define IDC_S_WINSIZE         260
#define IDC_S_RENDERER        261
#define IDC_S_SCALER          262
#define IDC_S_FILTER          263
#define IDC_S_ASPECT          264
#define IDC_S_FRAMESKIP       265
#define IDC_S_VSYNC           266
#define IDC_S_BLINKCURSOR     267   /* checkbox: blink the text-mode cursor           */

/* ── Audio ────────────────────────────────────────────────────────────────────── */
#define IDC_S_VOLUME          280
#define IDC_S_MUTE            281
#define IDC_S_RATE            282
#define IDC_S_SBMODEL         283
#define IDC_S_SBADDR          284
#define IDC_S_SBIRQ           285
#define IDC_S_SBDMA           286
#define IDC_S_OPL             287
#define IDC_S_MIDI            288
#define IDC_S_SOUNDFONT       289
#define IDC_S_SPEAKER         290
#define IDC_S_GUS             291
#define IDC_S_TANDY           292

/* ── Input ────────────────────────────────────────────────────────────────────── */
#define IDC_S_HOSTCURSOR      310   /* checkbox: show the host arrow over the video   */
#define IDC_S_SEAMLESS        311
#define IDC_S_MSENS           312   /* edit:     mouse sensitivity, percent           */
#define IDC_S_KBLAYOUT        313
#define IDC_S_TYPEMATIC       314
#define IDC_S_JOYTYPE         315
#define IDC_S_JOYPAD          316

/* ── Drives ───────────────────────────────────────────────────────────────────── */
#define IDC_S_DRIVEC          330
#define IDC_S_FLOPPYA         331
#define IDC_S_CDROM           332
#define IDC_S_BOOTFROM        333

#endif
