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
 * Every setting is one row of g_SetDefinitions: registry name, dialog control, kind, default,
 * range, and (for a combo) its items. Defaults, registry load, registry save, clamp,
 * dialog fill and dialog read are all LOOPS OVER THAT TABLE. The first cut of this
 * file wrote each of those out by hand per setting; at seven settings that was fine,
 * and at forty-odd it is four places to forget the same knob. Adding a setting is now
 * one row plus one enum member plus one line of layout in ntvdmhost.rc.
 *
 * ⚠ SOME OF THESE ARE STORED BUT NOT HONOURED. The CPU, Display, Audio, Drives pages
 *   and parts of Input came from the menu scaffold, where they were IDM_STUB. Most are
 *   live now; as of #136 (s92) the rows still stored only are Umb, A20, CdRomUsePhysical,
 *   CdRomImage and SoundFontPath, and the startup report (STAGE2: settings) prints WHY for
 *   each. settings_apply() in main.c is the ONLY place a stored value reaches the
 *   machine, so that function -- with SET_LIVE_IDS beside it -- is the honest list of
 *   what actually works. Wiring one up means adding a line there, not adding storage.
 *
 * No CRT: kernel32/user32/advapi32 only, like the rest of the host.
 */
#ifndef NTVDMEX_SETTINGS_H
#define NTVDMEX_SETTINGS_H

#include <windows.h>
#include "../../res/settings_ids.h"
#include "cpuspeed.h"          /* GH #56: the speed list SET_SPEEDMODE selects from */
#include "../vdd/present_scale.h"  /* the aspect list SET_ASPECT selects from       */

#define NTVDMEX_REG_KEY "Software\\NTVDMEX"
#define NTVDMEX_PATH_MAX 260
#define SETTINGS_DOS_MAJOR_MAX 255     /* "major.minor": a byte, then two digits */
#define SETTINGS_DOS_MINOR_MAX 99
#define SETTINGS_UNSIGNED_MAX  100000000u   /* an edit box's absurd value */

/* ── THE NUMERIC SETTINGS. ───────────────────────────────────────────────────────
     Order here must match g_SetDefinitions below; SET_COUNT closes the array and is what
     sizes the value block, so a row added to one and not the other fails to build. */
typedef enum {
    SET_DOSMAJ = 0, SET_DOSMIN, SET_PITPACE, SET_UITICK,
    /* ── ★ THE PROCESSOR PAGE IS ONE CONTROL NOW (session 60). CpuType/CpuCore/
         Cycles were read by nothing (DOSBox vocabulary on a real-CPU host); Fpu and
         Turbo were live but pointless as knobs; the granularity slider and the
         core-affinity box were removed to file knobs. SpeedMode -- the optional
         speed LIMIT -- is all that survives. Removing an enum member is safe because
         settings persist BY NAME (g_SetDefinitions[i].reg), so an orphaned registry value is
         simply never read again. */
    SET_SPEEDMODE,
    SET_CONVKB, SET_XMS, SET_EMS, SET_UMB, SET_A20,
    SET_WINSIZE, SET_RENDERER, SET_SCALER, SET_FILTER, SET_ASPECT,
    SET_FRAMESKIP, SET_VSYNC, SET_BLINKCURSOR, SET_AUTOFS, SET_OSD, SET_BUFFERED,
    SET_VOLUME, SET_MUTE, SET_RATE, SET_SBMODEL, SET_SBADDR, SET_SBIRQ, SET_SBDMA,
    SET_OPL, SET_MIDI, SET_SPEAKER, SET_GUS,
    SET_SEAMLESS, SET_MSENS, SET_KBLAYOUT, SET_TYPEMATIC,
    SET_JOYTYPE, SET_JOYPAD,
    SET_BEHAVE,                 /* #167: where MS-DOS 6.22 and stock NTVDM differ */
    SET_TINT,                   /* #229: colour filter                            */
    SET_AUDIOAPI,               /* #234: WinMM / DirectSound                      */
    SET_GUSADDR, SET_GUSIRQ, SET_GUSDMA, SET_MPUADDR,   /* #235: device resources */
    SET_HOSTCURSOR,             /* s84: Always / Never / Smart (#218's idle rule)  */
    SET_FLOPPYPHYS, SET_CDPHYS, /* s84: the physical drive, or a mounted image      */
    SET_FIT,                    /* #325: maximised/fullscreen -- whole pixels or fill */
    SET_COUNT
} SET_ID;

/* ── THE STRING SETTINGS, kept separate because REG_SZ is a different call. ───── */
typedef enum {
    SET_STR_FLOPPYA = 0, SET_STR_CDROM, SET_STR_SOUNDFONT,
    SET_STR_SHELL,
    SET_STR_TEXTFONT,              /* #321 */
    SET_STR_COUNT
} SET_STR_ID;

enum { AUTOFS_ALWAYS = 0, AUTOFS_GRAPHICS = 1, AUTOFS_NEVER = 2 };

/* BehaveLike's values (#167). */
enum { BEHAVE_NTVDM = 0, BEHAVE_DOS622 };

/* ShowHostCursor's values (s84). Smart is #218's rule: hide after CURSOR_IDLE_MS still
   over the video. Always never hides it there; Never always does. Captured, the
   pointer is hidden whatever this says. */
enum { HOSTCUR_ALWAYS = 0, HOSTCUR_NEVER, HOSTCUR_SMART };

/* PcSpeaker's values. Named, because `s->v[SET_SPEAKER] == 2` at a call site is
   a number nobody can check against the item list four hundred lines away. */
enum {
    SPKOUT_OFF = 0, SPKOUT_CARD, SPKOUT_REAL, SPKOUT_BOTH
};
#define SPKOUT_TO_CARD(v) ((v) == SPKOUT_CARD || (v) == SPKOUT_BOTH)
#define SPKOUT_TO_REAL(v) ((v) == SPKOUT_REAL || (v) == SPKOUT_BOTH)

enum {
    SK_CHECK = 0,   /* checkbox -> 0 or 1                                        */
    SK_UINT,        /* edit box -> unsigned, clamped to [lo,hi]                  */
    SK_COMBO,       /* drop-list -> zero-based index, clamped to [0,hi]          */
    SK_VER,         /* editable combo holding "major.minor" -- writes TWO rows   */
    SK_DERIVED,     /* no control of its own; written by the SK_VER row above it */
    SK_SLIDER       /* trackbar -> unsigned in [lo,hi]; its value label is ctl+1000 (#291) */
};

typedef struct _SET_DEF {
    PCSTR       RegistryName;   /* registry value name                           */
    INT         ControlId;      /* dialog control id (0 for SK_DERIVED)          */
    INT         Kind;           /* SK_*                                          */
    DWORD       Default;        /* default value (an INDEX for SK_COMBO)         */
    DWORD       Low, High;      /* inclusive clamp; for SK_COMBO High = last valid index */
    PCSTR       Items;          /* SK_COMBO: '|'-separated item text, else NULL  */
} SET_DEF, *PSET_DEF; typedef const SET_DEF *PCSET_DEF;

typedef struct _SET_STR_DEF {
    PCSTR       RegistryName;
    INT         ControlId;
    PCSTR       Default;
} SET_STR_DEF;

/* ⚠ SB defaults are the card we actually emulate (vdd_sb.h: base 0x220, IRQ 5,
     DMA 1/5) and the audio path really does run at 44100 (audio_wave.h). They are
     not folklore: a default that disagrees with the hardware would have the dialog
     describing a machine that does not exist. */
extern const SET_DEF g_SetDefinitions[SET_COUNT];

extern const SET_STR_DEF g_SetStringDefinitions[SET_STR_COUNT];

/* ── WHERE A VALUE CAME FROM. (GH #144) ─────────────────────────────────────────
     The DOS version once read 5.00 on every rig run from a registry value nobody had
     looked at, because only the FILE override ever printed a line. Every row now
     carries its source so the startup log can say it for all of them. A file override
     is recorded by the host (it changes a machine variable, not v[]); see
     settings_note_override in main.c. */
enum { SETSRC_DEFAULT = 0, SETSRC_REG, SETSRC_REG_BAD };

typedef struct _NTVDMEX_SETTINGS {
    DWORD Values[SET_COUNT];
    char  Strings[SET_STR_COUNT][NTVDMEX_PATH_MAX];   /* char, not CHAR: the spelling moves code (#333) */
    BYTE  Sources[SET_COUNT];             /* SETSRC_*, as loaded at startup */
    BYTE  StringSources[SET_STR_COUNT];
} NTVDMEX_SETTINGS, *PNTVDMEX_SETTINGS; typedef const NTVDMEX_SETTINGS *PCNTVDMEX_SETTINGS;

/* ── Named access, so callers read like they used to. ───────────────────────────
     settings_apply() says g_set.Values[SET_MSENS], not g_set.Values[37]. */
#define SETV(settings, id)  ((settings)->Values[(id)])

VOID SettingsCopyString(PSTR destination, PCSTR source, INT capacity);

/* The defaults ARE the shipped behaviour, with one deliberate exception: the host
   cursor defaults to VISIBLE. It was hidden unconditionally long before it was a
   toggle, on the theory that it cost input lag; that was never measured, and a
   pointer you cannot see over the window is worse for every non-game guest. */
VOID SettingsDefaults(NTVDMEX_SETTINGS *settings);

/* Clamp on the way IN, not only at the dialog. A hand-edited registry value of zero
   for the UI tick would spin the UI thread flat out, and an out-of-range combo index
   would select nothing at all -- the control would come up blank and OK would then
   write the blank back. */
VOID SettingsClamp(NTVDMEX_SETTINGS *settings);




/* HKEY_CURRENT_USER, not LOCAL_MACHINE: the VDM runs as the logged-in user and must
   not need administrator rights to remember a checkbox. */
VOID SettingsLoad(NTVDMEX_SETTINGS *settings);

VOID SettingsSave(const NTVDMEX_SETTINGS *settings);

/* ── THE VERSION FIELD IS "major.minor" TEXT, PARSED LENIENTLY. ──────────────────
     It is a combo the user can type into, because the list can never be complete:
     6.22 is the oracle, 5.00 is what XP's own COMMAND.COM demands, and the next
     guest that refuses to run will want some third number. Accepts "5", "5.0",
     "5.00", " 6.22 ". */
VOID SettingsParseVersion(PCSTR text, DWORD *major, DWORD *minor);

/* Parse an unsigned decimal out of an edit box. Returns 0 on "nothing usable here",
   which the caller treats as "keep the value you already had" -- an ES_NUMBER edit
   can still be EMPTY, and an empty box must not read as zero. */
INT SettingsParseUnsigned(PCSTR text, DWORD *out);

/* Copy item n of a '|'-separated list into out. Returns 0 when n is past the end,
   which is how the combo-filling loop knows to stop -- the table stores the text,
   not a count, so there is only one place to edit when a list gains an entry. */
INT SettingsItem(PCSTR items, INT number, PSTR out, INT capacity);

#endif /* NTVDMEX_SETTINGS_H */
