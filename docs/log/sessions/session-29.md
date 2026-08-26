# Session 29 — 2026-08-26

> Archived verbatim from `return-ntvdm.md`, which was the single rolling handoff
> file until 2026-08-26. Split by session so it can be read by topic and date
> rather than by scrolling. **Nothing has been edited** — including conclusions
> that later sessions REFUTED, which are kept on purpose: the refutations are
> some of the most valuable material here.

```
═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ SESSION 29 (2026-08-26). THE SHELL OF THE APP: A CONSTANT CAPTION, A    ██
██     STATUS STRIP THAT REPORTS THE MACHINE, AND FIVE MENUS FOLDED INTO A     ██
██     TABBED SETTINGS DIALOG. UI ONLY -- no emulation path was touched.       ██
═══════════════════════════════════════════════════════════════════════════════

User requirements, all three delivered:

**1. THE CAPTION IS A CONSTANT: "Microsoft Windows XP Virtual DOS Machine".** It used
   to append " - PROG.EXE" and, while captured, "[captured -- Win+F10 releases]". Both
   are gone from the title and live on the status strip now.

**2. THE STATUS STRIP HAS TWO PARTS.**
```
   | DOOM.EXE   |   32-bit   |   Protected mode        Captured -- Win+F10 releases |
```
   Left  = program name, client width, CPU mode. Right = capture state + the chord.
   ► The mode pair is new information, not a relocation. A DPMI guest crossing into
     32-bit protected mode is the largest single change of behaviour this host has,
     and until now the only way to know it had happened was to read the log AFTERWARDS.
   ► `status_update()` is POLLED from the UI tick and pushes a part only when its text
     changes. Deliberately not a dirty flag: two of the three facts (`g_dpmi_pm`,
     `g_dpmi_client32`) are set on the V86 thread deep inside the mode switch, and a
     flag there is one more thing every future mode-change site must remember.
   ⚠ IN EXCLUSIVE FULLSCREEN NEITHER PART IS VISIBLE -- the DirectDraw primary covers
     the strip exactly as it covers the menu bar. That is a REGRESSION IN REACH for the
     release chord, which the caption used to carry everywhere. Win+F10 still works;
     nothing on screen says so while fullscreen AND captured. Fix = an on-screen hint
     in the present path, not a return to the caption.

**3. CPU / DISPLAY / AUDIO / INPUT / DRIVE ARE DELETED FROM THE MENU BAR.** Their
   configuration is now six tabs of the Settings dialog:
```
   |General|CPU|Display|Audio|Input|Drives|________________________|
   |  ...settings...                                              |
   |______________________________________________________________|
                            (Restore Defaults)        (OK) (Cancel)
```
   The bar is now **File | Edit | View | Machine | Capture | Debug | Help**. What stayed
   behind is what was never a setting: Fullscreen, Show Menu Bar, Show Host Cursor
   (View); Restart, Pause, Capture Input, Send Ctrl+Alt+Del, the Mount commands
   (Machine). Those are ACTIONS -- you reach for them mid-game, not behind an OK button.
   File's old "Configuration" submenu (Edit Config File / Open Config Folder / ...)
   described a config file that never existed and is gone; the store is HKCU.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ▶▶ THE SETTINGS STORE IS NOW ONE TABLE, AND MOST OF IT IS NOT HONOURED YET   │
└──────────────────────────────────────────────────────────────────────────────┘
  `SET_DEFS[]` in `src/host/settings.h` is the single source: registry name, control
  id, kind, default, range, combo items. Defaults, registry load, registry save,
  clamp, dialog fill and dialog read are all LOOPS OVER IT. Adding a knob is one table
  row + one enum member + one line of `.rc` layout. The hand-written version was fine
  at 7 settings and would have been 4 places to forget the same knob at 46.

⚠⚠ **40 OF THE 46 SETTINGS ARE STORED BUT NOT HONOURED.** They came from the old menu
   scaffold, where they were `IDM_STUB`. They now round-trip through HKCU faithfully
   and change nothing. **This was the user's explicit call** -- I proposed rendering
   them greyed-out so a dialog could not claim to have applied something it had not,
   and the user chose enabled-and-persisted. So:
   ► **`settings_apply()` IS THE HONEST LIST OF WHAT WORKS.** Eight values reach the
     machine: DOS major/minor, PitPace, UiTickMs, BlinkTextCursor, ShowHostCursor,
     MouseSensitivity. A setting absent from that function is one the emulator does not
     consult. Wiring one up means adding a line THERE -- the storage already exists.
   ► STAGE0 logs only the live ones. Logging all 46 would print 40 numbers no run can
     support, which is the "counter's layout is a claim" failure in another costume.

⚠ **THE TEXT-FILE PRECEDENCE CONTRACT IS UNCHANGED AND STILL LOAD-BEARING:**
```
        built-in default   <   registry   <   text file on the share
```
  `settings_load()` still runs at the TOP of the knob block in WinMain. Do not tidy it.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ▶▶ NEW INSTRUMENT: tools/dlgcheck/dlgcheck.py -- READ THE RESOURCE, NOT THE  │
│    .rc. It found a real defect and then caught itself telling 45 lies.       │
└──────────────────────────────────────────────────────────────────────────────┘
  `python3 tools/dlgcheck/dlgcheck.py build/ntvdmhost.exe --ids res/settings_ids.h`
  parses RT_DIALOG templates out of the LINKED PE -- the bytes the loader will parse,
  not the source that produced them -- and reports every control's rectangle plus
  out-of-bounds, overlaps, and duplicate IDs. It exists because "the settings dialog
  has never been opened by a human" is an open item and opening one costs a trip to
  the bare-metal box; a dialog that compiles is not a dialog that renders.

  ★ IT REPORTED 47 PROBLEMS AND 45 OF THEM WERE ITS OWN. **A COMBOBOX's height in a
    dialog template is the height of its DROPPED LIST, not of the closed control.**
    Taking the number literally makes every combo appear to swallow the two rows below
    it and hang off the page. The tool now models the resting height (~12 dlu) and says
    so in a comment. The remaining 2 were real: an `IDC_S_BOOTFROM` false positive of
    the same kind, and a genuine 2-dlu overlap of the Audio page's `D&MA:` label into
    its combo, which is fixed.
    ► This is the session-23/24 lesson again, arriving inside an hour of writing a new
      instrument: **an instrument's model of its subject is a claim and must be checked
      before its output is believed.**

  ▶ ALSO VERIFIED, OFF-VM: all 46 table-referenced control IDs exist in some page (a
    missing one makes `settings_ctl()` return NULL and the knob silently do nothing);
    no duplicate IDs across pages (lookup is by search, so a duplicate resolves to
    whichever page is searched first); all 7 dialog templates embedded; imports still
    XP-only; all 17 native batteries pass and rebuild from current source.

▶ TRAPS WORTH KNOWING FOR THE NEXT .rc EDIT
  * **windres UPPERCASES a window class name**: `"SysTabControl32"` is stored as
    `SYSTABCONTROL32`. Harmless -- Win32 class lookup is case-insensitive -- but it
    means grepping the binary for the mixed-case name finds nothing and looks like the
    control was dropped.
  * Page dialogs need `DS_CONTROL` (implies `WS_EX_CONTROLPARENT`) or Tab stops dead at
    the page boundary.
  * Pages must be `HWND_TOP`, not inserted after the tab control: below it in z-order
    they are painted over by the tab's own background and never appear.
  * `EnableThemeDialogTexture` (uxtheme.dll, bound by name, kept loaded) is what stops
    a page rendering as a grey slab on the tab's themed background.
  * `TCM_ADJUSTRECT` computes where the pages go. The height of a tab row belongs to
    the visual style; hard-coding it is how a dialog is right on one theme and clipped
    on the next.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ▶▶ ALL THREE VERIFIED ON THE BARE-METAL BOX, WITH PICTURES. AND THE RIG CAN  │
│    NOW SEE ITS OWN WINDOW FOR THE FIRST TIME.                                │
└──────────────────────────────────────────────────────────────────────────────┘
★ **`scripts/bm/rigshot.c` (build: `scripts/build-rigshot.sh`) CLOSES A GAP THAT HAD
  BEEN OPEN SINCE THE HOST GREW A WINDOW.** The host's own screenshot captures
  `g_vid.frame` -- the GUEST FRAMEBUFFER -- which is right for "does Doom's status bar
  render" and USELESS for anything about the host: the caption, the status strip, the
  menu bar, a dialog. None of those are in the guest's framebuffer. So every UI change
  this project ever made was verified by a human walking to the box, which is exactly
  why "the settings dialog has never been opened by a human" survived a whole session.
  rigshot BitBlts the real desktop to a .bmp on the share, plus enough remote poking to
  get a dialog on screen:
```
    rigshot shot <out.bmp>    | rigshot cmd <n>    (PostMessage WM_COMMAND to the VDM)
    rigshot click <x> <y>     | rigshot key <vk>   | rigshot fg <caption> | rigshot list
```
  Drive it through controld: `exec cmd /c ""<share>\uishot.bat""`.
  ⚠ **CLICK, NOT `TCM_SETCURSEL`, to change a tab.** TCM_SETCURSEL crosses a process
    boundary fine and does NOT raise TCN_SELCHANGE; the notification that would
    (WM_NOTIFY) carries a pointer Windows will not marshal between processes. Setting
    the selection without the page following would have "verified" a broken dialog.
  ⚠ `-ffreestanding -fno-builtin` are load-bearing in the build script: without them
    GCC recognises the hand-rolled `slen` loop as strlen and emits a CALL to it, which
    does not exist in a `-nostdlib` link.
  ⚠ **A MINIMIZED WINDOW IS STILL `WS_VISIBLE`.** It lists, it is found by
    FindWindow, and it captures as bare desktop. `fg` uses SW_RESTORE now. Cost one
    confusing screenshot where the host was provably running and simply not on screen.

▶ MEASURED, ON 192.168.1.29, WITH THE BINARY MD5-MATCHED TO THE BUILD:
```
  COMMAND.COM guest : caption "Microsoft Windows XP Virtual DOS Machine" (no program
                      name, no capture suffix); bar = File Edit View Machine Capture
                      Debug Help; strip = "COMMAND.COM | 16-bit | Real mode"
  DOOM.EXE guest    : strip = "DOOM.EXE | 32-bit | Protected mode"   <- THE MODE
                      FIELDS FLIP, which is the whole point of adding them
  capture toggled x4: right part alternates "Captured -- Win+F10 releases" /
                      "Win+F10 captures input" every time, left part stable
  Settings dialog   : OPENED AND PHOTOGRAPHED. All six tabs render themed, pages swap
                      on click, values load. OK dismissed it cleanly and the host
                      survived; `reg query HKCU\Software\NTVDMEX` came back with all
                      47 values (43 DWORD + 4 REG_SZ) -- DosVersionMinor 0x16=22,
                      Cycles 0xbb8=3000, ConventionalKB 0x280=640, SampleRate=44100.
```
  ► So **OPEN ITEM 4 IS EFFECTIVELY CLOSED**: the dialog renders, switches and commits.
    A human still has not TYPED in it, so keyboard navigation across the page boundary
    (the thing `DS_CONTROL` is for) is the one part still taken on trust.
  ► The "window minimized itself on the first capture toggle" observation is **CLOSED,
    and it was NOT the software: the user was at the box using the window.** Filed here
    only because of the shape of the mistake -- I was driving a machine a person was
    also driving, and read their input as the program's behaviour. Four clean toggles
    afterwards said "not reproducible", which was the right conclusion for the wrong
    reason. ⚠ THE RIG IS NOT NECESSARILY UNATTENDED. Before calling desktop-level
    behaviour a defect, establish that nobody is touching it.

▶ STILL UNVERIFIED: **Ctrl+Tab does not switch pages.** A real property sheet does;
  this hand-rolled tab dialog has no handler for it. Not a defect against the spec,
  but the first thing a keyboard user will try.
```
