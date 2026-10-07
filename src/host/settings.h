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
static const SET_DEF g_SetDefinitions[SET_COUNT] = {
/*  registry name        control            kind        dflt lo   hi    items */
{ "DosVersionMajor",   IDC_S_DOSVER,      SK_VER,        6,  1, 255, NULL },
{ "DosVersionMinor",   0,                 SK_DERIVED,   22,  0,  99, NULL },
{ "PitPace",           IDC_S_PITPACE,     SK_CHECK,      1,  0,   1, NULL },
/* ★ s73: a CHOICE, not a number. Index 0 = Auto -- the screen is updated when the
     guest's own frame completes (the 0x3DA present hook), which is what stock NTVDM
     effectively does and why BOUNCEBX is smooth there. The fixed floors remain for
     experiments; 5 ms was measured to saturate the UI thread and cost keyboard
     response (Skyroads, s61), which is why 15 was the floor before Auto existed.
     New registry name so an old "UiTickMs"=15 is not read as index 15. */
{ "UiTickMode",        IDC_S_UITICK,      SK_COMBO,      0,  0,   4,
  "Auto (Recommended)|5 ms (fast - may cost input response)|10 ms|15 ms|20 ms" },

/* ── ★ THE ONE PROCESSOR CONTROL: AN OPTIONAL SPEED LIMIT. ──────────────────────
     Off (Unlimited) by default. It slows software that runs too fast on a modern
     PC -- a delay-loop or unpaced demo -- and it CANNOT do more than that: it runs
     16-bit code on the real CPU, so there is nothing to clock down and the only
     lever is a duty cycle. It is an approximation, deliberately labelled as one; see
     src/host/cpuspeed.h for what "66 MHz" can and cannot mean here.
   ★ THE DEFAULT SURVIVES EVERY RESHUFFLE: index 0 is Unlimited, so an untouched
     machine never throttles. ⚠ A CHOSEN value does not survive the session-60 trim
     of the ladder (18 entries -> 6); an out-of-range stored index clamps back to
     Unlimited, which is visible rather than silently wrong. */
{ "CpuSpeed",          IDC_S_SPEEDMODE,   SK_COMBO,      0,  0,
                                          CPUSPEED_COUNT - 1, CPUSPEED_ITEMS },
/* #136: memory FITTED, in KB. 640 = the machine every build has run (INT 12h 639, EBDA and
   MCB top 9FC0h); less moves all of those together -- see bios_bda.h. Start-up only, and
   refused (logged, 640 kept) for a program that would not fit under it. */
{ "ConventionalKB",    IDC_S_CONVKB,      SK_UINT,     640, 64,  640, NULL },
{ "Xms",               IDC_S_XMS,         SK_CHECK,      1,  0,   1, NULL },
{ "Ems",               IDC_S_EMS,         SK_CHECK,      1,  0,   1, NULL },
/* ⚠ Umb and A20 are STORED ONLY (#136): no UMB provider exists, and the A20 address wrap
   is not modelled. settings_dead_why in main.c says so in the startup report. */
{ "Umb",               IDC_S_UMB,         SK_CHECK,      1,  0,   1, NULL },
{ "A20",               IDC_S_A20,         SK_CHECK,      1,  0,   1, NULL },

/* ⚠ 1x, NOT 2x. This defaulted to "2x" for as long as it did nothing; the moment
     it became live that default would have made every clean machine open a
     1280x800 client -- wider than the 1024x768 desktop the test rig runs. The
     rule at the top of this file is that THE DEFAULTS ARE THE SHIPPED BEHAVIOUR,
     and the shipped behaviour is one pixel per pixel. */
/* s81 (#156): "Custom" removed -- it behaved as 1x. A stored 3 clamps back to the default. */
/* #325: EXACT. 1x = one desktop pixel per frame pixel, Nx = an N x N block; the window
   is sized to the picture, whatever the mode. 4x added (320x200 at 4x is 1280x800).
   Meanings unchanged, so the stored index is kept. */
{ "WindowSize",        IDC_S_WINSIZE,     SK_COMBO,      0,  0,   3, "1x|2x|3x|4x" },
/* s81 (#147), user decision: GDI + DirectDraw only, and the choice is REAL. The window is
   always GDI; "DirectDraw" makes fullscreen the exclusive DirectDraw mode (no tearing)
   instead of the borderless GDI window. There was never Direct3D or OpenGL code.
   s84 (user): the dialog calls it "Renderer", for the window AND fullscreen. The window
   stays GDI until the present-timing measurement says a windowed DirectDraw path would
   buy anything (it would look the same and cannot flip; see STAGE2 `present`).
   #237 (user, s84): the "(sharp)" / "(soft)" suffixes are gone -- since #223 DirectDraw
   stretches with StretchDIBits into the back buffer's DC and is as sharp as GDI; any
   smoothing now comes only from Scaler / Filtering.
   ⛔ A NEW REGISTRY NAME, DELIBERATELY. The old "Renderer" value was stored for months
     while it did nothing, so machines carry whatever was once clicked -- the rig had 1.
     Honouring it made the user's fullscreen suddenly blurry (s81 check, "a regression").
     A value chosen when it meant nothing must not start meaning something. */
{ "FullscreenRenderer", IDC_S_RENDERER,   SK_COMBO,      0,  0,   1,
                                          "GDI|DirectDraw" },
{ "Scaler",            IDC_S_SCALER,      SK_COMBO,      0,  0,   4, "None|Scale2x|hq2x|Scanlines|CRT" },
/* #325: only consulted when the picture is not a whole multiple of the frame (a forced
   ratio, Fill, a window too big for the screen). Sharp -- whole-multiple point-sampling,
   then the remainder smoothed -- is APPENDED, so a stored 0/1 keeps its meaning; it is
   the default for a machine that never chose. */
{ "Filtering",         IDC_S_FILTER,      SK_COMBO,      2,  0,   2, PRESENT_FILTER_ITEMS },
/* ── ★ WAS A CHECKBOX, IS NOW THE ASPECT LOCK. (session 54) ─────────────────────
     None / 4:3 / 16:9 / 16:10, and it does two things at once: it constrains the
     WINDOW to that shape while you drag it, and it letterboxes the frame inside the
     client if the two ever disagree anyway (maximised, or a drag Windows would not
     let us constrain). With the lock on and the window on-aspect there are no bars.
   ★ AND 0/1 KEEP THEIR OLD MEANINGS -- 0 was off, 1 was "correct aspect" = 4:3 --
     so an existing registry value migrates for free. The list and the enum behind
     it live in present_scale.h, checked by present_test.c. */
/* ⇒ #325 (user, s93): NATIVE -- square pixels, the frame's own shape -- is index 0 and
     the default; "Auto = every VGA mode at 4:3" is gone. ⛔ A NEW REGISTRY NAME: a stored
     "AspectRatio" 0 meant Auto, and must not silently become Native. */
{ "DisplayAspect",     IDC_S_ASPECT,      SK_COMBO,      0,  0,
                                          PRESENT_ASPECT_COUNT - 1, PRESENT_ASPECT_ITEMS },
{ "FrameSkip",         IDC_S_FRAMESKIP,   SK_COMBO,      0,  0,   2, "0|1|2" },
/* s73: OFF by default. With the Auto screen update every guest frame is blitted at
     the moment it completes; making that blit also wait for the monitor's blank
     delays the NEXT frame's render to a random phase and cost 3% of BOUNCEBX's
     frames on the rig. Stock NTVDM does not vsync its blit either, and it is smooth. */
{ "VSync",             IDC_S_VSYNC,       SK_CHECK,      0,  0,   1, NULL },
{ "BlinkTextCursor",   IDC_S_BLINKCURSOR, SK_CHECK,      1,  0,   1, NULL },
/* ── START FULLSCREEN (s68, user ask). Always = every program starts fullscreen;
     Graphics only = a program that begins in text mode (DOOM) starts in a window and
     flips the moment it sets a graphics mode; Never = always a window. It fires ONCE
     per process and never touches Alt+Enter -- the user can still flip by hand either
     way. Default Never = the shipped behaviour to date. Values: AUTOFS_* below. */
{ "StartFullscreen",   IDC_S_AUTOFS,      SK_COMBO,      2,  0,   2, "Always|Graphics only|Never" },
/* ── #217 (user, s83 sweep): the messages drawn over the picture (the mouse-capture
     hint, the Mark hint) FLICKERED, because the window was painted in three strokes --
     bars, frame, then the message -- and the screen showed the frame alone in between.
     Now the whole picture is composed off-screen and blitted once. Both are choices
     because "some users won't want the messages, or buffering"; both default ON
     (user decision, 2026-09-28). */
{ "OnScreenMessages",  IDC_S_OSD,         SK_CHECK,      1,  0,   1, NULL },
{ "BufferedDrawing",   IDC_S_BUFFERED,    SK_CHECK,      1,  0,   1, NULL },
/* ⛔ FULLSCREEN HAS NO SETTINGS AT ALL, AND THAT IS THE FIX. (s64)
     It briefly had two -- a resolution list and a "sharp pixels" checkbox -- and the
     user's verdict was "there's a lot of knobs to fiddle now, and none of them seem
     to actually achieve sharp pixels". BOTH HALVES WERE RIGHT, and the second is why
     the first happened: I could not make it sharp, so I kept adding ways to ask.
     The real cause was the DirectDraw stretch blt being FILTERED BY THE DRIVER (see
     the long note over gdi_present). Fullscreen is now a borderless window drawn by
     the same GDI path as the window, which is point-sampled and always was sharp.
     With no display-mode change there is no resolution to choose, and whole-multiple
     scaling is not something anyone would switch off. So: ZERO KNOBS.
     The escape hatch for the old exclusive path is a file knob, ddrawfs.flag. */

{ "MasterVolume",      IDC_S_VOLUME,      SK_UINT,     100,  0, 100, NULL },
{ "Mute",              IDC_S_MUTE,        SK_CHECK,      0,  0,   1, NULL },
{ "SampleRate",        IDC_S_RATE,        SK_COMBO,      1,  0,   2, "22050|44100|48000" },
{ "SbModel",           IDC_S_SBMODEL,     SK_COMBO,      0,  0,   2, "Sound Blaster 16|Sound Blaster AWE32|Sound Blaster Pro" },
{ "SbAddress",         IDC_S_SBADDR,      SK_COMBO,      0,  0,   3, "220|240|260|280" },
{ "SbIrq",             IDC_S_SBIRQ,       SK_COMBO,      0,  0,   3, "5|7|10|11" },
{ "SbDma",             IDC_S_SBDMA,       SK_COMBO,      0,  0,   2, "1|3|5" },
{ "Opl",               IDC_S_OPL,         SK_COMBO,      1,  0,   1, "OPL2|OPL3" },
/* #136: which host midiOut device the MPU-401 (and the GUS's MIDI UART) play through --
   MIDI_ROUTE_* in src/vdd/midi_route.h. Host GM = device 0, as always; MT-32 / SoundFont
   = a host DRIVER found by name (Munt, BASSMIDI...), which also gets SysEx. Start-up only. */
{ "Midi",              IDC_S_MIDI,        SK_COMBO,      0,  0,   2, "Host GM|MT-32|SoundFont" },
/* ── ⚠ THE PC SPEAKER HAS TWO PLACES TO COME OUT OF, AND THEY ARE NOT THE SAME
     DEVICE. The emulated one is a square wave in the mixer, out of the SOUND
     CARD -- which is what DOSBox does, is the only path that can be attenuated
     by the volume above or summed with FM and PCM, and is completely inaudible
     on a machine with nothing plugged into its line out. The other is the
     transducer soldered to the motherboard, driven through Beep.sys (see
     src/host/pcspeaker.h). Neither is right for everyone, so it is a choice.
   ★ THE OLD VALUES MIGRATE FOR FREE. This was a checkbox: 0 = off, 1 = on. As
     indices those are exactly Off and Sound card, which is what they meant. */
{ "PcSpeaker",         IDC_S_SPEAKER,     SK_COMBO,      1,  0,   3, "Off|Sound Card|Real PC Speaker|Both" },
/* ── THE GUS, WIRED (s80). This row was "Gus", default 0, and nothing read it -- the
     checkbox did nothing while the card was controlled by cfg\nogus.flag alone. A dialog
     that was ever OK'd has therefore SAVED Gus=0 to HKCU, meaning nothing; reading that
     value now would silently switch off a card the user has confirmed works. So the row
     has a NEW name and the old value is ignored. Default ON: it is part of the machine.
     Decided at startup (the environment's ULTRASND= is built before the devices), so a
     change takes effect at the next program start. */
{ "GusEnabled",        IDC_S_GUS,         SK_CHECK,      1,  0,   1, NULL },
/* "Tandy" (a Tandy / CMS checkbox) was REMOVED in s80: nothing modelled it, and Tandy
   sound is part of a whole different MACHINE (PCjr/Tandy 1000 video modes and BIOS ID),
   not a card a VGA-era PC could carry. A stored HKCU "Tandy" value is simply ignored. */

/* "HideHostCursor" was REMOVED by #218 (user, s83 sweep: the toggle "feels jaggy"):
   the pointer now hides by itself after 5 s still over the video. A stored value is
   simply never read again (settings persist by name). */
/* #136: on = never capture; the guest follows Windows' pointer over the picture
   (capture_allowed in main.c). Off = the capture policy, as always. Live. */
{ "SeamlessMouse",     IDC_S_SEAMLESS,    SK_CHECK,      0,  0,   1, NULL },
{ "MouseSensitivity",  IDC_S_MSENS,       SK_SLIDER,   100, 10, 400, NULL },
{ "KeyboardLayout",    IDC_S_KBLAYOUT,    SK_COMBO,      0,  0,   3, "US|United Kingdom|German|French" },
{ "TypematicRate",     IDC_S_TYPEMATIC,   SK_UINT,      10,  2,  30, NULL },
{ "JoystickType",      IDC_S_JOYTYPE,     SK_COMBO,      0,  0,   2, "None|2 axis, 2 button|4 axis, 4 button" },
{ "JoystickGamepad",   IDC_S_JOYPAD,      SK_CHECK,      0,  0,   1, NULL },

/* "BootFrom" and "DriveCPath" were REMOVED in s84 (user): both were stored and read by
   nothing -- the VDM already reaches the host's own drives. A stored value is ignored. */
/* ── #167: ONE CHOICE FOR EVERY ROW WHERE THE TWO REFERENCES DISAGREE. (user, 2026-09-28)
     Some answers differ between genuine MS-DOS 6.22 and the Windows XP NTVDM this
     project replaces, and neither is a defect. Rather than a knob per register, one
     setting says which machine to be; each such row reads it. Default NTVDM: that is
     what NTVDMEX stands in for. Values: BEHAVE_* below. First member: XMS 08h's BH. */
{ "BehaveLike",        IDC_S_BEHAVE,      SK_COMBO,      0,  0,   1, "Windows XP NTVDM|Genuine MS-DOS" },
/* #229 (docs/EMULATION.md): Default / Sepia / Monochrome white, green, orange. */
{ "ColourFilter",      IDC_S_TINT,        SK_COMBO,      0,  0,
                                          PRESENT_TINT_COUNT - 1, PRESENT_TINT_ITEMS },
/* #234: default WinMM, what every build so far has used; DirectSound falls back to it. */
{ "AudioOutput",       IDC_S_AUDIOAPI,    SK_COMBO,      0,  0,   1, "WinMM|DirectSound" },
/* #235 (docs/EMULATION.md): the other cards' resources, as their jumpers offered them.
   Defaults are the cards as built so far: GUS 240h / IRQ 11 / DMA 3, MPU-401 330h. The
   OPL has no such choice on any real card (AdLib is 388h), so it has no row. */
{ "GusAddress",        IDC_S_GUSADDR,     SK_COMBO,      3,  0,   5, "210|220|230|240|250|260" },
{ "GusIrq",            IDC_S_GUSIRQ,      SK_COMBO,      4,  0,   6, "2|3|5|7|11|12|15" },
{ "GusDma",            IDC_S_GUSDMA,      SK_COMBO,      1,  0,   4, "1|3|5|6|7" },
{ "MpuAddress",        IDC_S_MPUADDR,     SK_COMBO,      3,  0,   4, "300|310|320|330|340" },
/* s84 (user): #218's smart hide becomes one of three. Default Smart = the behaviour since
   #218. ⛔ A NEW NAME, DELIBERATELY: "ShowHostCursor" was an old 0/1 checkbox (644a6e0),
   and the rig still carried a 0 that read as "Always" the first time this row used that
   name -- and "HideHostCursor" meant something else again. */
{ "HostCursorMode",    IDC_S_HOSTCURSOR,  SK_COMBO,      2,  0,   2, "Always|Never|Smart" },
/* s84 (user): each removable drive is the PHYSICAL one or a mounted image, shown as a
   pair of radios whose first one is the row's control. With no physical drive the dialog
   greys that radio and the host treats the setting as "image"; a blank image path is an
   EMPTY drive. See settings_drive_radios / settings_apply in main.c.
   #136: FloppyUsePhysical is live (it decides whether FloppyAImage is what INT 13h opens);
   CdRomUsePhysical is STORED ONLY until a CD-ROM is mounted into DOS (#240/#241). */
{ "FloppyUsePhysical", IDC_S_FLOPPY_PHYS, SK_CHECK,      1,  0,   1, NULL },
{ "CdRomUsePhysical",  IDC_S_CD_PHYS,     SK_CHECK,      1,  0,   1, NULL },
/* #325: how the picture fills an area the user did not size -- maximised or fullscreen.
   Whole pixels = the largest whole multiple that fits, black borders round it; Fill =
   the largest on-ratio picture, sharp-filtered. */
{ "DisplayFit",        IDC_S_FIT,         SK_COMBO,      0,  0,   1, PRESENT_FIT_ITEMS },
};

static const SET_STR_DEF g_SetStringDefinitions[SET_STR_COUNT] = {
{ "FloppyAImage", IDC_S_FLOPPYA,   "" },
{ "CdRomImage",   IDC_S_CDROM,     "" },
{ "SoundFontPath",IDC_S_SOUNDFONT, "" },
/* #203: the COMMAND.COM the DOS prompt runs. Empty = Windows XP's own. The file is the
   user's to supply (a 6.22 copy is Microsoft's); cfg\shell.txt still outranks it. */
{ "DosPrompt",    IDC_S_SHELL,     "" },
/* #321: the text-mode font. Empty = the default (Fixedsys + code page 437 Terminal, see
   src/host/sysfont.h); otherwise the name of any font installed on this machine, laid
   over the default character by character. NTVDMEX ships no font. Live. */
{ "TextFont",     IDC_S_TEXTFONT,  "" },
};

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

static VOID SettingsCopyString(PSTR destination, PCSTR source, INT capacity)
{
    INT index = 0;
    while (source[index] && index < capacity - 1) { destination[index] = source[index]; ++index; }
    destination[index] = 0;
}

/* The defaults ARE the shipped behaviour, with one deliberate exception: the host
   cursor defaults to VISIBLE. It was hidden unconditionally long before it was a
   toggle, on the theory that it cost input lag; that was never measured, and a
   pointer you cannot see over the window is worse for every non-game guest. */
static VOID SettingsDefaults(NTVDMEX_SETTINGS *settings)
{
    INT index;
    for (index = 0; index < SET_COUNT; ++index) { settings->Values[index] = g_SetDefinitions[index].Default; settings->Sources[index] = SETSRC_DEFAULT; }
    for (index = 0; index < SET_STR_COUNT; ++index) {
        SettingsCopyString(settings->Strings[index], g_SetStringDefinitions[index].Default, NTVDMEX_PATH_MAX);
        settings->StringSources[index] = SETSRC_DEFAULT;
    }
}

/* Clamp on the way IN, not only at the dialog. A hand-edited registry value of zero
   for the UI tick would spin the UI thread flat out, and an out-of-range combo index
   would select nothing at all -- the control would come up blank and OK would then
   write the blank back. */
static VOID SettingsClamp(NTVDMEX_SETTINGS *settings)
{
    INT index;
    for (index = 0; index < SET_COUNT; ++index) {
        const SET_DEF *definition = &g_SetDefinitions[index];
        INT isBad = (definition->Kind == SK_COMBO) ? (settings->Values[index] > definition->High)
                                        : (settings->Values[index] < definition->Low || settings->Values[index] > definition->High);
        if (isBad) {
            settings->Values[index] = definition->Default;
            if (settings->Sources[index] == SETSRC_REG) settings->Sources[index] = SETSRC_REG_BAD;   /* say so */
        }
    }
    /* A version is a PAIR: an out-of-range major that fell back to 6 with a minor of
       00 would report "6.00", a DOS that never shipped. Reset both together. */
    if (settings->Values[SET_DOSMAJ] == g_SetDefinitions[SET_DOSMAJ].Default
        && settings->Values[SET_DOSMIN] > g_SetDefinitions[SET_DOSMIN].High) {
        settings->Values[SET_DOSMIN] = g_SetDefinitions[SET_DOSMIN].Default;
        if (settings->Sources[SET_DOSMIN] == SETSRC_REG) settings->Sources[SET_DOSMIN] = SETSRC_REG_BAD;
    }
}

static VOID SettingsRegistryPut(HKEY key, PCSTR name, DWORD value)
{
    RegSetValueExA(key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof value);
}

/* 1 = the value was there and was used. A value of the wrong TYPE is not "there". */
static INT SettingsRegistryTry(HKEY key, PCSTR name, DWORD *out)
{
    DWORD value = 0, size = sizeof value, type = 0;
    if (RegQueryValueExA(key, name, NULL, &type, (BYTE *)&value, &size) == ERROR_SUCCESS
        && type == REG_DWORD && size == sizeof value) { *out = value; return 1; }
    return 0;
}

static INT SettingsRegistryGetString(HKEY key, PCSTR name, PSTR out, INT capacity)
{
    DWORD size = (DWORD)capacity, type = 0;
    if (RegQueryValueExA(key, name, NULL, &type, (BYTE *)out, &size) != ERROR_SUCCESS
        || type != REG_SZ || size == 0)
        return 0;                                /* leave the default in place        */
    if ((INT)size >= capacity) size = (DWORD)capacity - 1;
    out[size] = 0;                                 /* RegQueryValueEx may not NUL it    */
    return 1;
}

/* HKEY_CURRENT_USER, not LOCAL_MACHINE: the VDM runs as the logged-in user and must
   not need administrator rights to remember a checkbox. */
static VOID SettingsLoad(NTVDMEX_SETTINGS *settings)
{
    HKEY key; INT index;
    SettingsDefaults(settings);
    if (RegOpenKeyExA(HKEY_CURRENT_USER, NTVDMEX_REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;                                  /* never stored yet -> defaults      */
    for (index = 0; index < SET_COUNT; ++index)
        if (SettingsRegistryTry(key, g_SetDefinitions[index].RegistryName, &settings->Values[index])) settings->Sources[index] = SETSRC_REG;
    for (index = 0; index < SET_STR_COUNT; ++index)
        if (SettingsRegistryGetString(key, g_SetStringDefinitions[index].RegistryName, settings->Strings[index], NTVDMEX_PATH_MAX))
            settings->StringSources[index] = SETSRC_REG;
    RegCloseKey(key);
    SettingsClamp(settings);
}

static VOID SettingsSave(const NTVDMEX_SETTINGS *settings)
{
    HKEY key; DWORD disposition = 0; INT index;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, NTVDMEX_REG_KEY, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &key, &disposition) != ERROR_SUCCESS)
        return;
    for (index = 0; index < SET_COUNT; ++index)
        SettingsRegistryPut(key, g_SetDefinitions[index].RegistryName, settings->Values[index]);
    for (index = 0; index < SET_STR_COUNT; ++index) {
        INT length = 0; while (settings->Strings[index][length]) ++length;
        RegSetValueExA(key, g_SetStringDefinitions[index].RegistryName, 0, REG_SZ,
                       (const BYTE *)settings->Strings[index], (DWORD)length + 1);
    }
    RegCloseKey(key);
}

/* ── THE VERSION FIELD IS "major.minor" TEXT, PARSED LENIENTLY. ──────────────────
     It is a combo the user can type into, because the list can never be complete:
     6.22 is the oracle, 5.00 is what XP's own COMMAND.COM demands, and the next
     guest that refuses to run will want some third number. Accepts "5", "5.0",
     "5.00", " 6.22 ". */
static VOID SettingsParseVersion(PCSTR text, DWORD *major, DWORD *minor)
{
    DWORD majorValue = 0, minorValue = 0; INT index = 0, isSeen = 0;
    while (text[index] == ' ' || text[index] == '\t') ++index;
    while (text[index] >= '0' && text[index] <= '9') { majorValue = majorValue * 10 + (DWORD)(text[index] - '0'); ++index; isSeen = 1; }
    if (text[index] == '.') {
        ++index;
        while (text[index] >= '0' && text[index] <= '9') { minorValue = minorValue * 10 + (DWORD)(text[index] - '0'); ++index; }
    }
    if (!isSeen || majorValue < 1 || majorValue > 255 || minorValue > 99) return;   /* keep the previous value */
    *major = majorValue; *minor = minorValue;
}

/* Parse an unsigned decimal out of an edit box. Returns 0 on "nothing usable here",
   which the caller treats as "keep the value you already had" -- an ES_NUMBER edit
   can still be EMPTY, and an empty box must not read as zero. */
static INT SettingsParseUnsigned(PCSTR text, DWORD *out)
{
    DWORD value = 0; INT index = 0, isSeen = 0;
    while (text[index] == ' ' || text[index] == '\t') ++index;
    while (text[index] >= '0' && text[index] <= '9') {
        value = value * 10 + (DWORD)(text[index] - '0');
        if (value > 100000000u) return 0;                /* absurd: reject, don't wrap */
        ++index; isSeen = 1;
    }
    if (!isSeen) return 0;
    *out = value;
    return 1;
}

/* Copy item n of a '|'-separated list into out. Returns 0 when n is past the end,
   which is how the combo-filling loop knows to stop -- the table stores the text,
   not a count, so there is only one place to edit when a list gains an entry. */
static INT SettingsItem(PCSTR items, INT number, PSTR out, INT capacity)
{
    INT index = 0, length = 0;
    if (!items) return 0;
    while (number > 0 && items[index]) { if (items[index] == '|') --number; ++index; }
    if (!items[index]) return 0;
    while (items[index] && items[index] != '|' && length < capacity - 1) out[length++] = items[index++];
    out[length] = 0;
    return 1;
}

#endif /* NTVDMEX_SETTINGS_H */
