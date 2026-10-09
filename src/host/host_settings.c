/* host_settings.c -- the Settings dialog: applying, loading and showing every setting and where its
 *   value came from.
 *
 * Part of the host's single translation unit: #included by main.c after host_internal.h. */

/* The reported DOS version, when something overrides the dialog's (s80): the XP shell's
   5.00, or cfg\dosver.txt. The dialog SHOWS it and does not push over it. */
static INT          g_DosVersionForced = 0;
INT          g_JoystickPovMap;   /* JoystickGamepad: map the pad's D-pad
                                       (POV hat) onto axis A -- what a DOS
                                       platformer actually wants from a pad */
#define UITICK_CHOICES 5   /* the Settings combo's entries */
static const INT UITICK_MS[UITICK_CHOICES] = { UITICK_AUTO, 5, 10, 15, 20 };   /* Settings combo index -> ms */
/* ── EVERY SETTING SAYS ITS VALUE AND WHERE IT CAME FROM. (GH #144) ──────────────
     See knob-with-two-sources: the reported DOS version sat at 5.00 on the rig from a
     registry value nobody could see, because only the FILE override ever printed. A
     file override changes a machine variable, not g_Settings, so it is noted here at the
     point it is read; SettingsLogSources() prints the lot once they have all run. */
static PCSTR g_SettingsOverrideBy[SET_COUNT];      /* what overrode the row, if anything */
static DWORD       g_SettingsOverrideValue[SET_COUNT];    /* ...and the value it put in force   */
static VOID SettingsNoteOverride(INT settingId, PCSTR source, DWORD value)
{
    g_SettingsOverrideBy[settingId] = source; g_SettingsOverrideValue[settingId] = value;
}
static PCSTR g_ShellOverride;               /* #203: cfg\shell.txt beat DosPrompt */

/* ⚠ THE ROWS SettingsApply() AND ITS NEIGHBOURS ACTUALLY READ. A row not in this
     list is stored and shown in the dialog and changes nothing (GH #136), and the log
     says so rather than letting its value look like a claim about the machine. Keep
     it in step with SettingsApply: adding a read there means adding the id here. */
static const BYTE g_SettingsLiveIds[] = {
    SET_DOSMAJ, SET_DOSMIN, SET_PITPACE, SET_UITICK, SET_SPEEDMODE, SET_XMS, SET_EMS,
    SET_WINSIZE, SET_RENDERER, SET_SCALER, SET_FILTER, SET_ASPECT, SET_FRAMESKIP,
    SET_VSYNC, SET_BLINKCURSOR, SET_AUTOFS, SET_VOLUME, SET_MUTE, SET_RATE,
    SET_OSD, SET_BUFFERED,                       /* #217 */
    SET_BEHAVE,                                  /* #167 */
    SET_TINT,                                    /* #229 */
    SET_AUDIOAPI,                                /* #234 */
    SET_SBMODEL,                                 /* #231 */
    SET_OPL,                                     /* #232 */
    SET_GUSADDR, SET_GUSIRQ, SET_GUSDMA, SET_MPUADDR,   /* #235 (read at start-up) */
    SET_SBADDR, SET_SBIRQ, SET_SBDMA, SET_SPEAKER, SET_GUS,
    SET_MSENS, SET_TYPEMATIC, SET_JOYTYPE, SET_JOYPAD,
    SET_KBLAYOUT,                                /* s82 #136 */
    /* #136 (s92). HostCursorMode and FloppyUsePhysical were READ by SettingsApply since
       s84 and simply never listed here, so the report called two working rows dead.
       ConventionalKB, Midi and SeamlessMouse are new consumers: g_DosMemoryTop,
       AudioWaveMidiOpen, CaptureAllowed. */
    SET_HOSTCURSOR, SET_FLOPPYPHYS, SET_CONVKB, SET_MIDI, SET_SEAMLESS,
    SET_FIT,                                     /* #325 */
};
static INT SettingsIsLive(INT settingId)
{
    UINT index;
    for (index = 0; index < sizeof g_SettingsLiveIds; ++index) if (g_SettingsLiveIds[index] == settingId) return 1;
    return 0;
}
/* ── #136: A ROW THAT STAYS DEAD SAYS WHY, IN THE REPORT. ────────────────────────────
     "stored only" alone reads like an oversight waiting for a line in SettingsApply;
     each of these is a decision, and the reason is what a reader needs to not re-open it.
     Only rows NOT in g_SettingsLiveIds ever reach this. */
static PCSTR SettingsDeadWhy(INT settingId)
{
    switch (settingId) {
    case SET_UMB:   return "there are no upper memory blocks to provide: XMS 10h answers "
                           "B1h and AH=5803h is refused, as with no EMM386 / DOS=UMB";
    case SET_A20:   return "the 1 MB address wrap is not modelled, so the line is always "
                           "enabled -- the gate FLAG follows the guest through 8042 / 92h / XMS";
    case SET_CDPHYS: return "no CD-ROM drive is mounted into DOS yet (#240/#241)";
    default:        return "not used";
    }
}
static PCSTR SettingsDeadWhyText(INT index)
{
    switch (index) {
    case SET_STR_CDROM:     return "no CD-ROM drive is mounted into DOS yet (#240/#241)";
    case SET_STR_SOUNDFONT: return "no SoundFont synth in NTVDMEX; MIDI=SoundFont uses a "
                                   "host SF2 driver, which keeps its own list";
    default:                return "not used";
    }
}

static VOID SettingsLogSources(VOID)
{
    static PCSTR const source[] = { "default", "registry",
                                       "registry value OUT OF RANGE -> default" };
    static CHAR buffer[12288];            /* 41 rows + four 260-byte paths */
    CHAR *cursor = buffer, item[64];
    INT index;
    cursor = LogPut(cursor, "STAGE2: settings -- value, source, and whether the host uses it (GH #144)\r\n");
    for (index = 0; index < SET_COUNT; ++index) {
        const SET_DEF *definition = &g_SetDefinitions[index];
        cursor = LogPut(cursor, "  "); cursor = LogPut(cursor, definition->RegistryName); cursor = LogPut(cursor, " = ");
        cursor = LogDecimal(cursor, g_Settings.Values[index]);
        if (definition->Kind == SK_COMBO && SettingsItem(definition->Items, (INT)g_Settings.Values[index], item, sizeof item)) {
            cursor = LogPut(cursor, " \""); cursor = LogPut(cursor, item); cursor = LogPut(cursor, "\"");
        }
        cursor = LogPut(cursor, " ["); cursor = LogPut(cursor, source[g_Settings.Sources[index] < 3 ? g_Settings.Sources[index] : 0]); cursor = LogPut(cursor, "]");
        if (g_SettingsOverrideBy[index]) {
            cursor = LogPut(cursor, " OVERRIDDEN by "); cursor = LogPut(cursor, g_SettingsOverrideBy[index]);
            cursor = LogPut(cursor, " -> "); cursor = LogDecimal(cursor, g_SettingsOverrideValue[index]);
        }
        if (SettingsIsLive(index)) cursor = LogPut(cursor, "\r\n");
        else { cursor = LogPut(cursor, " (stored only -- "); cursor = LogPut(cursor, SettingsDeadWhy(index));
               cursor = LogPut(cursor, ", GH #136)\r\n"); }
    }
    for (index = 0; index < SET_STR_COUNT; ++index) {
        cursor = LogPut(cursor, "  "); cursor = LogPut(cursor, g_SetStringDefinitions[index].RegistryName); cursor = LogPut(cursor, " = \"");
        cursor = LogPut(cursor, g_Settings.Strings[index]); cursor = LogPut(cursor, "\" [");
        cursor = LogPut(cursor, source[g_Settings.StringSources[index] < 3 ? g_Settings.StringSources[index] : 0]); cursor = LogPut(cursor, "]");
        if (index == SET_STR_SHELL && g_ShellOverride) {
            cursor = LogPut(cursor, " OVERRIDDEN by "); cursor = LogPut(cursor, g_ShellOverride);
        }
        if (index == SET_STR_FLOPPYA || index == SET_STR_SHELL || index == SET_STR_TEXTFONT)
            cursor = LogPut(cursor, "\r\n");
        else { cursor = LogPut(cursor, " (stored only -- "); cursor = LogPut(cursor, SettingsDeadWhyText(index));
               cursor = LogPut(cursor, ", GH #136)\r\n"); }
    }
    LogAppend(LOG_PATH, buffer, cursor);
}

enum { TYPEMATIC_RATE_MIN = 2 };   /* SettingsApply: below it, the BIOS default rate stands */
static VOID SettingsApply(HWND window, const NTVDMEX_SETTINGS *settings, INT live)
{
    g_MouseSensitivity        = (INT)settings->Values[SET_MSENS];
    /* #136: the keyboard layout the BIOS translates with (vdd_input.c, from XP's own
       tables). Live: the next keystroke uses it. */
    g_Input.Layout      = (BYTE)(settings->Values[SET_KBLAYOUT] <= INPUT_LAYOUT_LAST ? settings->Values[SET_KBLAYOUT] : 0);
    /* ── THE JOYSTICK ROWS GO LIVE (session 62). The type reaches the gameport
         VDD (how many axes/buttons the adapter wires); the D-pad mapping stays
         host-side because it shapes the SAMPLE, not the device model. Live: the
         poll thread and the port trap both re-read these on every pass. */
    g_Joystick.Type       = (BYTE)(settings->Values[SET_JOYTYPE] <= JOYSTICK_TYPE_4AXIS ? settings->Values[SET_JOYTYPE] : 0);
    g_JoystickPovMap     = (INT)(settings->Values[SET_JOYPAD] ? 1 : 0);
    JoystickPollEnsure();               /* spawns the winmm poll thread ONLY if a
                                        joystick is configured -- no thread, and no
                                        timing risk, in the default (None) config */
    BiosBdaRefreshEquipment();    /* #253: bit 12 (game adapter) follows the type
                                        into 0040:0010 as well as INT 11h */
    g_PitPaceOn     = (INT)(settings->Values[SET_PITPACE] ? 1 : 0);
    g_UiTickMinimumMs = UITICK_MS[settings->Values[SET_UITICK] < UITICK_CHOICES ? settings->Values[SET_UITICK] : 0];
    g_Video.IsCursorBlink = (BYTE)(settings->Values[SET_BLINKCURSOR] ? 1 : 0);
    g_BehaveDos622 = (settings->Values[SET_BEHAVE] == BEHAVE_DOS622);         /* #167 */
    /* #232: OPL2 (an AdLib: bank-1 ports dead) or OPL3 (YMF262). Live -- the chip model
       reads it on every access, as a jumpered card would at power-up. */
    g_Opl.IsOpl3 = (BYTE)(settings->Values[SET_OPL] == 1 ? 1 : 0);
    g_FrameSkip      = (INT)settings->Values[SET_FRAMESKIP];
    g_XmsOn         = (INT)(settings->Values[SET_XMS] ? 1 : 0);
    g_EmsOn         = (INT)(settings->Values[SET_EMS] ? 1 : 0);
    /* GH #56. Live: the throttle thread re-reads the duty every millisecond and the
       interpreter re-reads the budget every slice, so changing the speed in the
       dialog bites on the next millisecond rather than at the next launch. */
    g_CpuSpeedIndex     = (INT)(settings->Values[SET_SPEEDMODE] < CPUSPEED_COUNT ? settings->Values[SET_SPEEDMODE] : 0);
    /* ⚠ FPU AND TURBO WERE REMOVED AS SETTINGS (session 60). g_FpuPresent stays 1
         -- we run on a real x87, so advertising it is always correct, and a checkbox
         whose only other position makes a guest wrong is not worth having. Turbo was
         just "select Unlimited", which the speed list already offers directly. */
    CpuSpeedRecompute();
    /* ── ★ THE GUEST'S KEY REPEAT IS THE GUEST'S, NOT THE HOST'S. (session 57)
         g_TypematicPeriodMicroseconds is seeded from the host's own SPI_GETKEYBOARDSPEED, which
         is right for a Windows application and wrong for a DOS one: a DOS
         program's repeat rate is the BIOS's, and the IBM BIOS default is about
         ten characters a second. The setting's own default is 10 for that
         reason. ⚠ THE DELAY IS LEFT ON THE HOST'S SETTING deliberately -- the
         row is `TypematicRate`, it says nothing about the delay before the
         first repeat, and inventing a second meaning for one control is how a
         knob comes to do something its label does not say. */
    if (settings->Values[SET_TYPEMATIC] >= TYPEMATIC_RATE_MIN)
        g_TypematicPeriodMicroseconds = MICROSECONDS_PER_SECOND_U / (UINT32)settings->Values[SET_TYPEMATIC];
    /* s84 (user): the physical drive, or a mounted image. The physical one needs no
       setting -- DOS already reaches the host's A: -- so only IMAGE mode (chosen, or
       forced because this PC has no floppy drive) names a file here. A blank image
       path means an EMPTY drive: NULL falls through to FLOPPY_IMG_PATH, which exists
       only on the test rig (the harness's disk), so on a user's PC it is simply absent. */
    g_FloppyImage     = ((!settings->Values[SET_FLOPPYPHYS] || !HostHasFloppy()) && settings->Strings[SET_STR_FLOPPYA][0])
                     ? settings->Strings[SET_STR_FLOPPYA] : NULL;
    g_HostCursorMode   = (INT)settings->Values[SET_HOSTCURSOR];                   /* s84 */
    /* #136: read at start-up only -- see g_DosMemoryTop. */
    g_ConventionalKbWant   = BiosClampConventionalKb((UINT)settings->Values[SET_CONVKB]);
    /* #136: seamless mouse. Turning it on while a guest holds the pointer gives the
       pointer back now (we are on the UI thread when `live`), rather than leaving a
       capture that the policy can no longer release by clicking. */
    InterlockedExchange(&g_MouseSeamless, settings->Values[SET_SEAMLESS] ? 1 : 0);
    if (live && window && g_MouseSeamless && g_Captured) InputCaptureSet(window, FALSE);
    /* The SbDma list is 1|3|5, and 5 is not an 8-bit channel on any real 8237 --
       on an SB16 it is the SIXTEEN-bit one. Selecting it therefore moves H and
       leaves D where it was, rather than pointing the 8-bit engine at a channel
       whose registers live at completely different ports. */
    g_SbConfig.IoBase = (WORD)(SB_DEFAULT_BASE + SB_BASE_STEP * (settings->Values[SET_SBADDR] & SB_BASE_CHOICE_MASK));
    { static const BYTE irqs[4] = { 5, 7, 10, 11 };
      static const BYTE dmas[3] = { 1, 3, 5 };
      BYTE ch = dmas[settings->Values[SET_SBDMA] < ARRAYSIZE(dmas) ? settings->Values[SET_SBDMA] : 0];
      g_SbConfig.Irq = irqs[settings->Values[SET_SBIRQ] & SB_IRQ_CHOICE_MASK];
      if (ch < DMA_FIRST_16BIT_CHANNEL) { g_SbConfig.Dma8Channel = ch; g_SbConfig.Dma16Channel = 0; }
      else        { g_SbConfig.Dma8Channel = SB_DEFAULT_DMA8; g_SbConfig.Dma16Channel = ch; } }
    /* ── #231: THE MODEL IS A CARD, NOT A LABEL. It was stored and read by nothing, so
         all three answered as one SB16 that called itself an SB 2.0 (T3) in BLASTER.
         Now each is itself -- the DSP version it reports, the commands it has, and
         the BLASTER string a real one's installer writes:
             SB Pro   DSP 3.02   A220 I5 D1 T4               (no 16-bit channel)
             SB16     DSP 4.05   A220 I5 D1 H5 P330 T6
             AWE32    DSP 4.12   A220 I5 D1 H5 P330 T6       (E620 with #233's EMU8000)
         A 16-bit channel chosen on the DMA row still wins for H; an SB Pro has none. */
    {   BYTE model = (BYTE)(settings->Values[SET_SBMODEL] <= SB_MODEL_LAST ? settings->Values[SET_SBMODEL] : 0);
        g_Sb.Model = model;
        g_SbConfig.Emu8kBase = (model == SB_MODEL_AWE32) ? (WORD)(g_SbConfig.IoBase + SB_EMU8K_PORT_OFFSET) : 0;  /* #233 */
        if (model == SB_MODEL_SBPRO) {
            g_SbConfig.Type = SB_BLASTER_TYPE_SBPRO; g_SbConfig.Dma16Channel = 0; g_SbConfig.MpuBase = 0;
            if (!g_DspVersionForced) { g_SbVersionMajor = SB_DSP_VERSION_SBPRO_MAJOR; g_SbVersionMinor = SB_DSP_VERSION_SBPRO_MINOR; }
        } else {
            {   static const WORD mpuBases[5] = { 0x300, 0x310, 0x320, 0x330, 0x340 };
                g_SbConfig.Type = SB_BLASTER_TYPE_SB16;                         /* P follows the MPU's port (#235) */
                g_SbConfig.MpuBase = mpuBases[settings->Values[SET_MPUADDR] <= ARRAYSIZE(mpuBases) - 1 ? settings->Values[SET_MPUADDR] : MPU_DEFAULT_BASE_CHOICE]; }
            if (!g_SbConfig.Dma16Channel) g_SbConfig.Dma16Channel = SB_DEFAULT_DMA16;
            if (!g_DspVersionForced) { g_SbVersionMajor = SB_DSP_VERSION_MAJOR; g_SbVersionMinor = model == SB_MODEL_AWE32 ? SB_DSP_VERSION_AWE32_MINOR : SB_DSP_VERSION_MINOR; }
        } }
    /* Not over a FORCED version: XP's COMMAND.COM (5.00) and cfg\dosver.txt both win at
       startup, so they win here too -- pushing 6.22 into a session whose shell requires
       5.00 is how its next command would say "Incorrect DOS version". */
    if (g_DosMachine && !g_DosVersionForced) DosInt21SetVersion(g_DosMachine, (BYTE)settings->Values[SET_DOSMAJ],
                                                              (BYTE)settings->Values[SET_DOSMIN]);
    /* ⚠ THROTTLE GRANULARITY AND CORE-AFFINITY ARE NO LONGER SETTINGS (session 60).
         Granularity defaults to AUTO (g_CpuSpeedGranularityMs = 0), which is the behaviour
         that made a slow speed smooth, so it needs no control; cpugran.txt still
         overrides it for the rig. Affinity defaults OFF and stays a file knob
         (cpuaff.txt) because measured it broke the guest's timer -- exposed for a
         future re-test, not for a user to find. */
    /* The cursor is the one setting with a VISIBLE side effect, so it goes through
       the same helper the menu item and Ctrl+F8 use rather than poking the flag. */
}

/* ── ★ fsinteger.flag -- SNAP FULLSCREEN TO WHOLE PIXEL MULTIPLES. OFF BY DEFAULT. ──
     I added whole-multiple scaling to cure blurry fullscreen. It was never the cause
     (the DirectDraw stretch blt's filtering was), and once that was fixed the snapping
     had exactly one remaining effect: BARS. The user, with the desktop at 1680x1050:
       "fullscreen now shows sharp pixels, but there is a letterbox around the output
        which didn't happen before... it seems the desktop resolution needs to be
        either 1:1 or 2:1 to see sharp pixels edge to edge. This still doesn't feel
        right."
     Correct. 1680/320 = 5.25, so snapping drops to 5x = 1600x1000 and leaves 80x50 of
     black. At 2560x1600 (8x) and 1280x800 (4x) it divides exactly and the bars vanish
     -- which is the whole of the "1:1 or 2:1" pattern they spotted.
   ► AND THE BARS BUY NOTHING THEY WANT, by their own evidence: the MAXIMIZED WINDOW is
     a 5.25x non-integer scale and they call it sharp. Nearest-neighbour at a fractional
     factor gives hard edges with occasional 6-pixel-wide columns among the 5s; that is
     visibly fine, and it fills the screen. Even pixels are the purist's answer to a
     question this user is not asking.
   ⇒ DEFAULT IS FILL, exactly like the window. Same code path, same fit, same result at
     any resolution, and fullscreen keeps its zero user-facing settings. The flag exists
     because "every pixel identical" is a legitimate taste, not because anyone must
     choose. */
#define FSINT_FLAG   CFG_("fsinteger.flag")

static VOID SettingsApplyPresent(PRESENT_DDRAW *present, const NTVDMEX_SETTINGS *settings)
{
    present->IsVsync  = (INT)(settings->Values[SET_VSYNC]  ? 1 : 0);
    present->Filter = (INT)(settings->Values[SET_FILTER] <= PRESENT_FILTER_SHARP ? settings->Values[SET_FILTER] : PRESENT_FILTER_SHARP);   /* #325 */
    present->Fit    = (INT)(settings->Values[SET_FIT] ? PRESENT_FIT_FILL : PRESENT_FIT_WHOLE);
    present->IsOsdOff    = settings->Values[SET_OSD]      ? 0 : 1;      /* #217 */
    present->Tint       = (INT)settings->Values[SET_TINT];             /* #229 */
    present->IsUnbuffered = settings->Values[SET_BUFFERED] ? 0 : 1;
    present->Aspect = (INT)settings->Values[SET_ASPECT];   /* PRESENT_ASPECT_*, not a flag */
    present->Scaler = (INT)settings->Values[SET_SCALER];
    /* Neither of these is a user setting -- see the note in settings.h about why
       fullscreen ended up with none. Both are file knobs, both default OFF. */
    present->IsFullscreenInteger   = (GetFileAttributesA(FSINT_FLAG)   != INVALID_FILE_ATTRIBUTES);
    /* Renderer = DirectDraw is the user-facing switch for the exclusive path (#147);
       the old file knob still forces it, for comparisons. */
    present->IsFullscreenDirectDraw = (settings->Values[SET_RENDERER] == 1)
                    || (GetFileAttributesA(DDRAWFS_FLAG) != INVALID_FILE_ATTRIBUTES);
}

static VOID SettingsApplyDevices(const NTVDMEX_SETTINGS *settings)
{
    VddAudioSetMaster(&g_Audio, settings->Values[SET_VOLUME], (INT)settings->Values[SET_MUTE]);
    /* The speaker VDD stays on the bus either way: port 0x61 must keep answering
       because guests time delay loops off its refresh bit. The setting decides
       only whether anything is audible. */
    /* ── TWO OUTPUTS, ONE SETTING. The mixer's square wave goes out of the SOUND
         CARD; Beep.sys drives the transducer on the MOTHERBOARD. "Both" is both,
         and they are genuinely independent -- a machine can have speakers plugged
         in, a case speaker, neither, or both, and only the person at it knows. */
    VddAudioSetSpeaker(&g_Audio, &g_Speaker, SPKOUT_TO_CARD(settings->Values[SET_SPEAKER]));
    g_SpeakerReal = g_Safe.RealSpeaker ? 0 : SPKOUT_TO_REAL(settings->Values[SET_SPEAKER]);   /* #132 */
    /* ⚠ Switching the real speaker OFF has to silence it, not merely stop driving
         it: the driver keeps sounding whatever it was last told to sound. */
    if (!g_SpeakerReal) PcSpeakerSet(&g_PcSpeaker, 0);
    SettingsApplyPresent(&g_PresentDdraw, settings);
}
enum { SETTINGS_RATE_CHOICES = 3, SETTINGS_RATE_DEFAULT = 1 };   /* SettingsOutputHz: 22050 / 44100 / 48000 */
/* The output rate is a CONSTRUCTION parameter, not something to push: the mixer
   and the waveOut device must be opened at the same rate or every sample is
   resampled to a clock nothing is running at. One function so the two callers
   cannot disagree. */
static UINT32 SettingsOutputHz(const NTVDMEX_SETTINGS *settings)
{
    static const UINT32 rates[SETTINGS_RATE_CHOICES] = { 22050u, 44100u, 48000u };
    return rates[settings->Values[SET_RATE] < SETTINGS_RATE_CHOICES ? settings->Values[SET_RATE] : SETTINGS_RATE_DEFAULT];
}

/* #321: the text-mode font, live. Rebuilt only when the NAME changed -- a build draws
   ~1,500 glyphs -- then copied into guest memory and redrawn. Logged, so a run says
   which font it was drawing with from that point on. */
static VOID SettingsApplyTextFont(VOID)
{
    CHAR lineBuffer[480], *lineCursor = lineBuffer;
    if (!lstrcmpA(g_TextFontLive, g_Settings.Strings[SET_STR_TEXTFONT])) return;
    lstrcpynA(g_TextFontLive, g_Settings.Strings[SET_STR_TEXTFONT], sizeof g_TextFontLive);
    lineCursor = LogPut(lineCursor, "settings: text font changed -- ");
    lineCursor = LogPut(lineCursor, SysFontBuild(g_TextFontLive, &g_SysFontReport));
    lineCursor = LogPut(lineCursor, "\r\n");
    LogAppend(LOG_PATH, lineBuffer, lineCursor);
    VddVideoRefreshFonts(&g_Video);
}

static VOID SettingsApplyLive(HWND window)
{
    SettingsApply(window, &g_Settings, SETTINGS_APPLY_LIVE);
    SettingsApplyTextFont();
    SettingsApplyPresent(&g_PresentDdraw, &g_Settings);
    /* ⚠ THE ASPECT CHANGES THE BASE SIZE, so it has to re-size too -- picking 16:9
         while the window is 4:3-shaped and leaving it alone would show the lock as
         doing nothing until the next drag. Tracked separately from the scale so
         neither one triggers on the other's account. */
    if (g_Settings.Values[SET_WINSIZE] != g_WindowSizeLive || g_Settings.Values[SET_ASPECT] != g_AspectLive) {
        g_WindowSizeLive = g_Settings.Values[SET_WINSIZE];
        g_AspectLive  = g_Settings.Values[SET_ASPECT];
        HostApplyWindowSize(window, g_WindowSizeLive);
    }
    MenuViewSync(window);
}

static PCSTR const g_SettingsTabs[NTVDMEX_PAGE_COUNT] = {
    "MS-DOS", "Machine", "Video", "Audio", "Input", "Drives"
};
static HWND g_SettingsPage[NTVDMEX_PAGE_COUNT];

/* Find a control by ID across every page. Control IDs are unique across the whole
   dialog (see settings_ids.h) precisely so this can exist: the table then does not
   have to carry a page column, and moving a control from one page to another is a
   pure .rc edit. */
static HWND SettingsControl(INT controlId)
{
    INT index;
    for (index = 0; index < NTVDMEX_PAGE_COUNT; ++index) {
        HWND control = g_SettingsPage[index] ? GetDlgItem(g_SettingsPage[index], controlId) : NULL;
        if (control) return control;
    }
    return NULL;
}
enum { SETTINGS_COMBO_DROPPED_WIDTH = 130, SETTINGS_FONT_DROPPED_WIDTH = 240, SETTINGS_SLIDER_PAGE = 10, SETTINGS_ITEM_HEIGHT = 14, SETTINGS_ITEM_INSET = 3, LOGFONT_PITCH_MASK = 3 };   /* the Settings dialog */
static VOID SettingsFillCombos(VOID)
{
    static PCSTR const versions[] = { "6.22", "5.00", "4.01", "3.31", "7.10" };
    CHAR item[64]; INT index, index2;
    HWND control;
    for (index = 0; index < SET_COUNT; ++index) {
        const SET_DEF *definition = &g_SetDefinitions[index];
        if (definition->Kind != SK_COMBO || !definition->ControlId) continue;
        control = SettingsControl(definition->ControlId);
        if (!control) continue;
        for (index2 = 0; SettingsItem(definition->Items, index2, item, (INT)sizeof item); ++index2)
            SendMessageA(control, CB_ADDSTRING, 0, (LPARAM)item);
    }
    /* The version list is a convenience, not a constraint: that combo is EDITABLE
       (CBS_DROPDOWN) because the next guest to refuse to start will want some number
       nobody has thought of yet. That is also why it is not an SK_COMBO index. */
    control = SettingsControl(IDC_S_DOSVER);
    if (control) for (index = 0; index < (INT)(sizeof versions / sizeof versions[0]); ++index)
        SendMessageA(control, CB_ADDSTRING, 0, (LPARAM)versions[index]);
    /* s84: the Video tab's three-column boxes are narrow; let their LISTS open wide
       enough for the longest item ("Monochrome orange", "Graphics only"). */
    {   static const INT narrow[] = { IDC_S_RENDERER, IDC_S_WINSIZE, IDC_S_AUTOFS,
                                      IDC_S_SCALER, IDC_S_FILTER, IDC_S_ASPECT,
                                      IDC_S_TINT, IDC_S_FRAMESKIP, IDC_S_FIT };
        for (index = 0; index < (INT)(sizeof narrow / sizeof narrow[0]); ++index)
            if ((control = SettingsControl(narrow[index])) != NULL) SendMessageA(control, CB_SETDROPPEDWIDTH, SETTINGS_COMBO_DROPPED_WIDTH, 0);
    }
}
enum { SETTINGS_SHELL_XP = 0, SETTINGS_SHELL_OWN = 1 };   /* the shell radios: XP's own COMMAND.COM, or a chosen one */
/* #203: the DOS prompt's two radios, and the path box + Browse only live under "Another". */
static VOID SettingsShellRadios(INT own)
{
    HWND xpShell = SettingsControl(IDC_S_SHELL_XP), ownShell = SettingsControl(IDC_S_SHELL_OWN);
    HWND edit = SettingsControl(IDC_S_SHELL),    browse = SettingsControl(IDC_S_SHELL_BROWSE);
    if (xpShell) SendMessageA(xpShell, BM_SETCHECK, own ? BST_UNCHECKED : BST_CHECKED, 0);
    if (ownShell) SendMessageA(ownShell, BM_SETCHECK, own ? BST_CHECKED : BST_UNCHECKED, 0);
    if (edit) EnableWindow(edit, own);
    if (browse) EnableWindow(browse, own);
}

/* One Open dialog for every path box: the shell, the floppy and ISO images, the
   SoundFont. The box's current text is where it starts. */
static VOID SettingsBrowse(HWND page, INT editId, PCSTR filter, PCSTR title)
{
    CHAR file[MAX_PATH]; OPENFILENAMEA openFile; INT index;
    HWND edit = SettingsControl(editId);
    file[0] = 0;
    if (edit) GetWindowTextA(edit, file, sizeof file);
    for (index = 0; index < (INT)sizeof openFile; ++index) ((PSTR)&openFile)[index] = 0;
    openFile.lStructSize = sizeof openFile;
    openFile.hwndOwner   = GetParent(page);
    openFile.lpstrFilter = filter;
    openFile.lpstrFile   = file;
    openFile.nMaxFile    = sizeof file;
    openFile.lpstrTitle  = title;
    openFile.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&openFile) && edit) SetWindowTextA(edit, file);
}
static VOID SettingsShellBrowse(HWND page)
{
    SettingsBrowse(page, IDC_S_SHELL,
        SETTINGS_FILTER_SHELL,
        SETTINGS_TITLE_SHELL);
}

/* ── s84 (user): EACH REMOVABLE DRIVE IS THE PHYSICAL ONE OR A MOUNTED IMAGE. ──────
     The PHYS radio is the table row (SK_CHECK); this keeps its partner opposite, the
     path box and Browse live only under "image", and -- with no physical drive on this
     PC -- greys the physical choice and selects the image. */
static VOID SettingsDriveRadios(INT physicalId, INT imageId, INT editId, INT browseId,
                                  INT have, INT isPhysical)
{
    HWND physical = SettingsControl(physicalId), image = SettingsControl(imageId);
    HWND edit = SettingsControl(editId), browse = SettingsControl(browseId);
    if (!have) isPhysical = 0;
    if (physical) { EnableWindow(physical, have); SendMessageA(physical, BM_SETCHECK, isPhysical ? BST_CHECKED : BST_UNCHECKED, 0); }
    if (image) SendMessageA(image, BM_SETCHECK, isPhysical ? BST_UNCHECKED : BST_CHECKED, 0);
    if (edit) EnableWindow(edit, !isPhysical);
    if (browse) EnableWindow(browse, !isPhysical);
}
static VOID SettingsFloppyRadios(INT isPhysical)
{
    SettingsDriveRadios(IDC_S_FLOPPY_PHYS, IDC_S_FLOPPY_IMG, IDC_S_FLOPPYA,
                          IDC_S_FLOPPY_BROWSE, HostHasFloppy(), isPhysical);
}
static VOID SettingsCdRadios(INT isPhysical)
{
    SettingsDriveRadios(IDC_S_CD_PHYS, IDC_S_CD_IMG, IDC_S_CDROM,
                          IDC_S_CD_BROWSE, HostHasCdrom(), isPhysical);
}

/* ── #321: THE TEXT-MODE FONT ROW. ───────────────────────────────────────────────────
     The list is every fixed-pitch font installed on this PC plus "(Default)", which
     stores as the empty string. Choosing one builds the tables it WOULD produce into a
     private copy -- the machine is untouched until OK/Apply -- and the page shows that
     copy: a line of text and a line of the DOS graphics characters, plus how many
     characters the font supplied and whether the default itself is degraded. */
#define TEXTFONT_DEFAULT_ITEM "(Default: Fixedsys and Terminal)"
static SYSFONT_TABLES g_TextFontPreview;
static SYSFONT_REPORT g_TextFontPreviewReport;

static INT CALLBACK SettingsFontEnum(const LOGFONTA *logFont, const TEXTMETRICA *textMetric,
                                       DWORD type, LPARAM lParam)
{
    HWND control = (HWND)lParam;
    (VOID)textMetric; (VOID)type;
    if ((logFont->lfPitchAndFamily & LOGFONT_PITCH_MASK) != FIXED_PITCH || logFont->lfFaceName[0] == '@') return 1;
    if (SendMessageA(control, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)logFont->lfFaceName) == CB_ERR)
        SendMessageA(control, CB_ADDSTRING, 0, (LPARAM)logFont->lfFaceName);
    return 1;
}

static VOID SettingsTextFontFill(VOID)
{
    HWND control = SettingsControl(IDC_S_TEXTFONT);
    HDC deviceContext;
    LOGFONTA logFont;
    if (!control) return;
    SendMessageA(control, CB_ADDSTRING, 0, (LPARAM)TEXTFONT_DEFAULT_ITEM);
    ZeroMemory(&logFont, sizeof logFont);
    logFont.lfCharSet = DEFAULT_CHARSET;
    deviceContext = GetDC(NULL);
    if (deviceContext) { EnumFontFamiliesExA(deviceContext, &logFont, (FONTENUMPROCA)SettingsFontEnum, (LPARAM)control, 0);
              ReleaseDC(NULL, deviceContext); }
    SendMessageA(control, CB_SETDROPPEDWIDTH, SETTINGS_FONT_DROPPED_WIDTH, 0);
}

/* The selected item as the stored string: "" for the default. */
static VOID SettingsTextFontGet(PSTR out, INT cap)
{
    HWND control = SettingsControl(IDC_S_TEXTFONT);
    LRESULT selector = control ? SendMessageA(control, CB_GETCURSEL, 0, 0) : CB_ERR;
    CHAR text[NTVDMEX_PATH_MAX];
    out[0] = 0;
    if (selector == CB_ERR) return;
    text[0] = 0;
    SendMessageA(control, CB_GETLBTEXT, (WPARAM)selector, (LPARAM)text);
    if (lstrcmpA(text, TEXTFONT_DEFAULT_ITEM)) lstrcpynA(out, text, cap);
}

static VOID SettingsTextFontPreview(VOID)
{
    CHAR face[NTVDMEX_PATH_MAX], text[200];
    HWND info = SettingsControl(IDC_S_TEXTFONT_INFO), view = SettingsControl(IDC_S_TEXTFONT_VIEW);
    SettingsTextFontGet(face, sizeof face);
    SysFontBuildInto(face, &g_TextFontPreview, &g_TextFontPreviewReport);
    if (!face[0])
        lstrcpyA(text, SETTINGS_FONT_DEFAULT_TEXT);
    else if (g_TextFontPreviewReport.User == SYSFONT_USER_OK && g_TextFontPreviewReport.UserCodePage)
        wsprintfA(text, SETTINGS_FONT_CODE_PAGE_FORMAT,
                  g_TextFontPreviewReport.UserCodePage);
    else if (g_TextFontPreviewReport.User == SYSFONT_USER_OK)
        wsprintfA(text, SETTINGS_FONT_PARTIAL_FORMAT,
                  g_TextFontPreviewReport.UserGlyphs[2]);
    else if (g_TextFontPreviewReport.User == SYSFONT_USER_NOSIZE)
        lstrcpyA(text, SETTINGS_FONT_NO_8_PIXEL);
    else
        lstrcpyA(text, SETTINGS_FONT_MISSING);
    if (SysFontIsDefaultDegraded(&g_TextFontPreviewReport))
        lstrcatA(text, SETTINGS_FONT_437_MISSING);
    if (info) SetWindowTextA(info, text);
    if (view) InvalidateRect(view, NULL, TRUE);
}

static VOID SettingsTextFontSelect(PCSTR face)
{
    HWND control = SettingsControl(IDC_S_TEXTFONT);
    LRESULT found;
    if (!control) return;
    found = SendMessageA(control, CB_FINDSTRINGEXACT, (WPARAM)-1,
                     (LPARAM)(face[0] ? face : TEXTFONT_DEFAULT_ITEM));
    if (found == CB_ERR && face[0])              /* stored, but no longer installed: keep it */
        found = SendMessageA(control, CB_ADDSTRING, 0, (LPARAM)face);
    SendMessageA(control, CB_SETCURSEL, (WPARAM)(found == CB_ERR ? 0 : found), 0);
    SettingsTextFontPreview();
}
enum { FONT_PREVIEW_COLUMNS = 64, FONT_PREVIEW_ROWS = 2, FONT_PREVIEW_CELL_WIDTH = 8, FONT_PREVIEW_CELL_HEIGHT = 16, FONT_PREVIEW_WIDTH = 512, FONT_PREVIEW_HEIGHT = 32, FONT_PREVIEW_HIGH_FIRST = 0x80, FONT_PREVIEW_BOX_FIRST = 0xB0, FONT_PREVIEW_BOX_COUNT = 48, FONT_PREVIEW_GREEK_FIRST = 0xE0, FONT_PREVIEW_GREY = 0xAA };   /* the Settings font preview: two rows of 8x16 cells */
/* Two lines of 64 cells from the previewed 8x16 table, light grey on black, 1:1. */
static VOID SettingsTextFontDraw(const DRAWITEMSTRUCT *drawItem)
{
    static const CHAR text[] = SETTINGS_FONT_SAMPLE_TEXT;
    BYTE row[FONT_PREVIEW_ROWS][FONT_PREVIEW_COLUMNS];
    static BYTE pixels[FONT_PREVIEW_HEIGHT][FONT_PREVIEW_COLUMNS];
    struct { BITMAPINFOHEADER Header; RGBQUAD Palette[2]; } bitmapInfo;
    INT index, pixelRow, left, top;
    RECT rect = drawItem->rcItem;
    for (index = 0; index < FONT_PREVIEW_COLUMNS; ++index) {
        row[0][index] = (BYTE)(index < (INT)sizeof text - 1 ? text[index] : FONT_PREVIEW_HIGH_FIRST + (index - (INT)sizeof text + 1));
        row[1][index] = (BYTE)(index < FONT_PREVIEW_BOX_COUNT ? FONT_PREVIEW_BOX_FIRST + index : FONT_PREVIEW_GREEK_FIRST + (index - FONT_PREVIEW_BOX_COUNT));
    }
    for (pixelRow = 0; pixelRow < FONT_PREVIEW_HEIGHT; ++pixelRow)
        for (index = 0; index < FONT_PREVIEW_COLUMNS; ++index) pixels[pixelRow][index] = g_TextFontPreview.Table16[row[pixelRow / FONT_PREVIEW_CELL_HEIGHT][index]][pixelRow % FONT_PREVIEW_CELL_HEIGHT];
    ZeroMemory(&bitmapInfo, sizeof bitmapInfo);
    bitmapInfo.Header.biSize = sizeof bitmapInfo.Header; bitmapInfo.Header.biWidth = FONT_PREVIEW_WIDTH; bitmapInfo.Header.biHeight = -FONT_PREVIEW_HEIGHT;
    bitmapInfo.Header.biPlanes = 1; bitmapInfo.Header.biBitCount = 1; bitmapInfo.Header.biCompression = BI_RGB;
    bitmapInfo.Palette[1].rgbRed = bitmapInfo.Palette[1].rgbGreen = bitmapInfo.Palette[1].rgbBlue = FONT_PREVIEW_GREY;
    FillRect(drawItem->hDC, &rect, (HBRUSH)GetStockObject(BLACK_BRUSH));
    left = rect.left + ((rect.right - rect.left) - FONT_PREVIEW_WIDTH) / 2; if (left < rect.left) left = rect.left;
    top = rect.top + ((rect.bottom - rect.top) - FONT_PREVIEW_HEIGHT) / 2;   if (top < rect.top)  top = rect.top;
    SetDIBitsToDevice(drawItem->hDC, left, top, FONT_PREVIEW_WIDTH, FONT_PREVIEW_HEIGHT, 0, 0, 0, FONT_PREVIEW_HEIGHT, pixels, (BITMAPINFO *)&bitmapInfo, DIB_RGB_COLORS);
}

static VOID SettingsToDialog(const NTVDMEX_SETTINGS *settings)
{
    CHAR text[NTVDMEX_PATH_MAX]; INT index;
    for (index = 0; index < SET_COUNT; ++index) {
        const SET_DEF *definition = &g_SetDefinitions[index];
        HWND control = definition->ControlId ? SettingsControl(definition->ControlId) : NULL;
        if (!control) continue;
        switch (definition->Kind) {
        case SK_CHECK:
            SendMessageA(control, BM_SETCHECK, settings->Values[index] ? BST_CHECKED : BST_UNCHECKED, 0);
            break;
        case SK_UINT:
            wsprintfA(text, SETTINGS_FORMAT_NUMBER, (UINT)settings->Values[index]); SetWindowTextA(control, text);
            break;
        case SK_SLIDER: {                    /* #291: a trackbar and its "N%" label */
            HWND label = SettingsControl(definition->ControlId + IDC_S_SLIDER_VALUE_OFFSET);
            SendMessageA(control, TBM_SETRANGE, FALSE, MAKELPARAM(definition->Low, definition->High));
            SendMessageA(control, TBM_SETPAGESIZE, 0, SETTINGS_SLIDER_PAGE);
            SendMessageA(control, TBM_SETPOS, TRUE, (LPARAM)settings->Values[index]);
            if (label) { wsprintfA(text, SETTINGS_FORMAT_PERCENT, (UINT)settings->Values[index]); SetWindowTextA(label, text); }
            break; }
        case SK_COMBO:
            SendMessageA(control, CB_SETCURSEL, (WPARAM)settings->Values[index], 0);
            break;
        case SK_VER:
            /* "6.22", not "6.2200" -- two digits, zero-padded, as DOS says it. The
               minor is the NEXT row (SK_DERIVED); that adjacency is the contract. */
            wsprintfA(text, SETTINGS_FORMAT_VERSION, (UINT)settings->Values[index], (UINT)settings->Values[index + 1]);
            SetWindowTextA(control, text);
            break;
        default: break;                      /* SK_DERIVED has no control of its own */
        }
    }
    for (index = 0; index < SET_STR_COUNT; ++index) {
        HWND control = SettingsControl(g_SetStringDefinitions[index].ControlId);
        if (control) SetWindowTextA(control, settings->Strings[index]);
    }
    SettingsShellRadios(settings->Strings[SET_STR_SHELL][0] != 0);
    SettingsTextFontSelect(settings->Strings[SET_STR_TEXTFONT]);                 /* #321 */
    SettingsFloppyRadios(settings->Values[SET_FLOPPYPHYS] != 0);                /* s84 */
    SettingsCdRadios(settings->Values[SET_CDPHYS] != 0);
    /* ── SAY WHICH VERSION PROGRAMS ACTUALLY SEE. (s80, user: "if I'm in Windows XP's
         command.com but reporting 6.22, is that right?") The box holds the SETTING; a
         session can be running a different, forced number, and the dialog said nothing. */
    /* ⇒ s81, the user's redesign: the SETTING and the SESSION each get their own row.
         One combo plus a note that contradicted it ("6.22" above, "reports 5.00" below)
         read as a bug. Row two states the number this session is really using and, in
         one sentence, why -- or that it is simply the setting. */
    /* s84 (the user's layout): one line, "MS-DOS 6.22" -- plus, only when a cfg file
       forces a different number than the setting, which file. */
    {   HWND dosVersionNow = SettingsControl(IDC_S_DOSVER_NOW);
        if (dosVersionNow && g_DosMachine) {
            wsprintfA(text, SETTINGS_DOS_VERSION_FORMAT,
                      (UINT)g_DosMachine->VersionMajor, (UINT)g_DosMachine->VersionMinor);
            if (g_DosVersionForced && g_DosVersionWhy) lstrcatA(text, SETTINGS_DOS_VERSION_FORCED);
            SetWindowTextA(dosVersionNow, text);
        }
    }
}

/* Read the pages back. A field that will not parse, or is out of range, LEAVES THE
   PREVIOUS VALUE -- it does not fall back to the default. Half-typing a number and
   clicking OK should not silently reset the knob you were adjusting. */
static VOID SettingsFromDialog(NTVDMEX_SETTINGS *settings)
{
    CHAR text[NTVDMEX_PATH_MAX]; INT index;
    for (index = 0; index < SET_COUNT; ++index) {
        const SET_DEF *definition = &g_SetDefinitions[index];
        HWND control = definition->ControlId ? SettingsControl(definition->ControlId) : NULL;
        if (!control) continue;
        switch (definition->Kind) {
        case SK_CHECK:
            settings->Values[index] = (SendMessageA(control, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1u : 0u;
            break;
        case SK_UINT: {
            DWORD value;
            if (!GetWindowTextA(control, text, 32)) break;
            if (!SettingsParseUnsigned(text, &value)) break;
            if (value >= definition->Low && value <= definition->High) settings->Values[index] = value;
            break; }
        case SK_COMBO: {
            LRESULT selector = SendMessageA(control, CB_GETCURSEL, 0, 0);
            if (selector != CB_ERR && (DWORD)selector <= definition->High) settings->Values[index] = (DWORD)selector;
            break; }
        case SK_SLIDER: {
            LRESULT value = SendMessageA(control, TBM_GETPOS, 0, 0);
            if ((DWORD)value >= definition->Low && (DWORD)value <= definition->High) settings->Values[index] = (DWORD)value;
            break; }
        case SK_VER:
            if (GetWindowTextA(control, text, 32))
                SettingsParseVersion(text, &settings->Values[index], &settings->Values[index + 1]);
            break;
        default: break;
        }
    }
    for (index = 0; index < SET_STR_COUNT; ++index) {
        HWND control = SettingsControl(g_SetStringDefinitions[index].ControlId);
        if (!control) continue;
        GetWindowTextA(control, text, NTVDMEX_PATH_MAX);
        SettingsCopyString(settings->Strings[index], text, NTVDMEX_PATH_MAX);
    }
    /* #321: a drop-list's text is its item; the default item stores as "". */
    SettingsTextFontGet(settings->Strings[SET_STR_TEXTFONT], NTVDMEX_PATH_MAX);
    /* #203: "Windows XP's own" means the empty string, whatever the greyed box holds. */
    {   HWND xpShell = SettingsControl(IDC_S_SHELL_XP);
        if (xpShell && SendMessageA(xpShell, BM_GETCHECK, 0, 0) == BST_CHECKED) settings->Strings[SET_STR_SHELL][0] = 0;
    }
}
/* EnableThemeDialogTexture is what stops a page rendering as a grey slab on the
   tab's themed background. It lives in uxtheme.dll (XP and later), so it is bound by
   name; a box with no theming simply gets the old grey rather than a dialog that
   fails to open. The module is loaded once and never freed -- the tab texture is
   drawn by uxtheme on every later WM_ERASEBKGND, not just at init. */
typedef HRESULT (WINAPI *PFN_ENABLE_THEME_DIALOG_TEXTURE)(HWND, DWORD);
static HMODULE   g_UxTheme;
static PFN_ENABLE_THEME_DIALOG_TEXTURE  g_EnableThemeDialogTexture;
enum { SETTINGS_DRIVE_IMAGE = 0, SETTINGS_DRIVE_PHYSICAL = 1 };   /* SettingsFloppy/CdRadios: an image file, or the real drive */
/* The Processor page says WHICH processor -- the user's own -- so fill the static
   with its brand string. It is read from the same place a DOS tool or Windows shows
   it (HKLM\HARDWARE\...\CentralProcessor\0\ProcessorNameString), trimmed of the
   leading spaces Intel pads it with. If the read fails the template's placeholder
   stands, so a missing key costs a generic line, not a blank. */
static VOID SettingsFillCpuInfo(HWND dialog)
{
    HWND control = GetDlgItem(dialog, IDC_S_CPUINFO);
    HKEY key;
    CHAR name[128]; DWORD size = sizeof name - 1, type = 0;
    CHAR out[160];
    PCSTR source = name;
    if (!control) return;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            HOST_REG_CPU_KEY,
            0, KEY_READ, &key) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExA(key, HOST_REG_CPU_NAME, NULL, &type,
                         (BYTE *)name, &size) == ERROR_SUCCESS
        && type == REG_SZ && size) {
        name[size < sizeof name ? size : sizeof name - 1] = 0;
        while (*source == ' ') ++source;                 /* Intel pads the string with spaces */
        {   PSTR cursor = out;                     /* ...inside it too: "CPU     E8600  @" */
            for (; *source && cursor < out + sizeof out - 1; ++source)
                if (*source != ' ' || (cursor > out && cursor[-1] != ' ')) *cursor++ = *source;
            *cursor = 0; }                          /* s84: the row's label says what it is */
        SetWindowTextA(control, out);
    }
    RegCloseKey(key);
}
enum { THEME_ETDT_ENABLE = 0x2, THEME_ETDT_USETABTEXTURE = 0x4 };   /* uxtheme.h's EnableThemeDialogTexture flags */
static INT_PTR CALLBACK SettingsPageProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    (VOID)wParam; (VOID)lParam;
    /* #291: a slider's label follows it as it moves. */
    if (message == WM_HSCROLL && lParam) {
        INT controlId = GetDlgCtrlID((HWND)lParam);
        HWND label = controlId ? GetDlgItem(dialog, controlId + IDC_S_SLIDER_VALUE_OFFSET) : NULL;
        if (label) {
            CHAR text[16];
            wsprintfA(text, SETTINGS_FORMAT_PERCENT, (UINT)SendMessageA((HWND)lParam, TBM_GETPOS, 0, 0));
            SetWindowTextA(label, text);
        }
        return TRUE;
    }
    if (message == WM_INITDIALOG) {
        if (!g_UxTheme) {
            g_UxTheme = LoadLibraryA(HOST_MODULE_UXTHEME);
            if (g_UxTheme)
                g_EnableThemeDialogTexture = (PFN_ENABLE_THEME_DIALOG_TEXTURE)GetProcAddress(g_UxTheme, HOST_EXPORT_ENABLE_THEME_DIALOG_TEXTURE);
        }
        if (g_EnableThemeDialogTexture) g_EnableThemeDialogTexture(dialog, THEME_ETDT_ENABLE | THEME_ETDT_USETABTEXTURE);   /* ETDT_ENABLE | ETDT_USETABTEXTURE */
        SettingsFillCpuInfo(dialog);            /* no-op on pages without the static */
        return TRUE;
    }
    /* ── #224: THE SPEED LIST, OWNER-DRAWN. A rung at or above this PC's own clock
         cannot be a throttle, so it is drawn greyed and choosing it snaps back to the
         last rung that can. Host shows the PC's own speed beside it. */
    if (message == WM_MEASUREITEM && ((MEASUREITEMSTRUCT *)lParam)->CtlID == IDC_S_SPEEDMODE) {
        ((MEASUREITEMSTRUCT *)lParam)->itemHeight = SETTINGS_ITEM_HEIGHT;
        return TRUE;
    }
    if (message == WM_DRAWITEM && ((DRAWITEMSTRUCT *)lParam)->CtlID == IDC_S_SPEEDMODE) {
        DRAWITEMSTRUCT *drawItem = (DRAWITEMSTRUCT *)lParam;
        CHAR text[96];
        INT selector = (drawItem->itemState & ODS_SELECTED) != 0;
        INT isOk  = (INT)drawItem->itemID < 0 || CpuSpeedIsAvailable((UINT)drawItem->itemID, HostCpuMhz());
        FillRect(drawItem->hDC, &drawItem->rcItem, GetSysColorBrush(selector && isOk ? COLOR_HIGHLIGHT : COLOR_WINDOW));
        if ((INT)drawItem->itemID >= 0) {
            SendMessageA(drawItem->hwndItem, CB_GETLBTEXT, drawItem->itemID, (LPARAM)text);
            if (drawItem->itemID == 0 && HostCpuMhz())
                wsprintfA(text + lstrlenA(text), SETTINGS_FORMAT_THIS_PC, HostCpuMhz());
            SetBkMode(drawItem->hDC, TRANSPARENT);
            SetTextColor(drawItem->hDC, GetSysColor(!isOk ? COLOR_GRAYTEXT
                                              : selector ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
            drawItem->rcItem.left += SETTINGS_ITEM_INSET;
            DrawTextA(drawItem->hDC, text, -1, &drawItem->rcItem, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        }
        return TRUE;
    }
    if (message == WM_DRAWITEM && ((DRAWITEMSTRUCT *)lParam)->CtlID == IDC_S_TEXTFONT_VIEW) {
        SettingsTextFontDraw((DRAWITEMSTRUCT *)lParam);                 /* #321 */
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wParam) == IDC_S_TEXTFONT && HIWORD(wParam) == CBN_SELCHANGE) {
        SettingsTextFontPreview();
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wParam) == IDC_S_SPEEDMODE && HIWORD(wParam) == CBN_SELCHANGE) {
        static LRESULT lastOk = 0;
        LRESULT selection = SendMessageA((HWND)lParam, CB_GETCURSEL, 0, 0);
        if (selection != CB_ERR && !CpuSpeedIsAvailable((UINT)selection, HostCpuMhz()))
            SendMessageA((HWND)lParam, CB_SETCURSEL, (WPARAM)lastOk, 0);
        else if (selection != CB_ERR) lastOk = selection;
        return TRUE;
    }
    if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED) {   /* #203, the General page */
        switch (LOWORD(wParam)) {
        case IDC_S_SHELL_XP:     SettingsShellRadios(SETTINGS_SHELL_XP); return TRUE;
        case IDC_S_SHELL_OWN:    SettingsShellRadios(SETTINGS_SHELL_OWN); return TRUE;
        case IDC_S_SHELL_BROWSE: SettingsShellBrowse(dialog); return TRUE;
        /* s84: the Drives tab's radio pairs and the three new Browse buttons. */
        case IDC_S_FLOPPY_PHYS:  SettingsFloppyRadios(SETTINGS_DRIVE_PHYSICAL); return TRUE;
        case IDC_S_FLOPPY_IMG:   SettingsFloppyRadios(SETTINGS_DRIVE_IMAGE); return TRUE;
        case IDC_S_CD_PHYS:      SettingsCdRadios(SETTINGS_DRIVE_PHYSICAL); return TRUE;
        case IDC_S_CD_IMG:       SettingsCdRadios(SETTINGS_DRIVE_IMAGE); return TRUE;
        case IDC_S_FLOPPY_BROWSE:
            SettingsBrowse(dialog, IDC_S_FLOPPYA, SETTINGS_FILTER_FLOPPY,
                            SETTINGS_TITLE_FLOPPY);
            return TRUE;
        case IDC_S_CD_BROWSE:
            SettingsBrowse(dialog, IDC_S_CDROM, SETTINGS_FILTER_ISO,
                            SETTINGS_TITLE_ISO);
            return TRUE;
        case IDC_S_SF_BROWSE:
            SettingsBrowse(dialog, IDC_S_SOUNDFONT, SETTINGS_FILTER_SOUNDFONT,
                            SETTINGS_TITLE_SOUNDFONT);
            return TRUE;
        }
    }
    return FALSE;
}
static VOID SettingsShowPage(INT page)
{
    INT index;
    for (index = 0; index < NTVDMEX_PAGE_COUNT; ++index)
        if (g_SettingsPage[index]) ShowWindow(g_SettingsPage[index], index == page ? SW_SHOW : SW_HIDE);
}
/* ── Ctrl+Tab / Ctrl+Shift+Tab (and Ctrl+PgDn / Ctrl+PgUp) switch pages. (s81, #137) ──
     A modal dialog's own loop runs IsDialogMessage on every key, so a Ctrl+Tab never
     reaches SettingsDialogProcedure -- the dialog manager takes it as a plain Tab and moves
     focus. A property sheet gets this behaviour for free; a hand-built tab dialog has
     to ask for it, and a message-filter hook on this thread, for the dialog's lifetime,
     is the documented way to see a dialog's messages before the dialog manager does. */
static HWND  g_SettingsDialog;
static HHOOK g_SettingsHook;
static LRESULT CALLBACK SettingsMessageFilter(INT code, WPARAM wParam, LPARAM lParam)
{
    MSG *message = (MSG *)lParam;
    if (code == MSGF_DIALOGBOX && g_SettingsDialog && message->message == WM_KEYDOWN
        && (GetKeyState(VK_CONTROL) & HOST_KEY_DOWN_BIT)
        && (message->wParam == VK_TAB || message->wParam == VK_NEXT || message->wParam == VK_PRIOR)) {
        HWND tabControl = GetDlgItem(g_SettingsDialog, IDC_S_TAB);
        INT count = (INT)SendMessageA(tabControl, TCM_GETITEMCOUNT, 0, 0);
        INT current = (INT)SendMessageA(tabControl, TCM_GETCURSEL, 0, 0);
        INT back = (message->wParam == VK_PRIOR)
                || (message->wParam == VK_TAB && (GetKeyState(VK_SHIFT) & HOST_KEY_DOWN_BIT));
        if (count > 0) {
            INT next = (current + (back ? count - 1 : 1)) % count;
            SendMessageA(tabControl, TCM_SETCURSEL, (WPARAM)next, 0);
            SettingsShowPage(next);
            return 1;                                  /* eaten: not a focus move */
        }
    }
    return CallNextHookEx(g_SettingsHook, code, wParam, lParam);
}
static INT_PTR CALLBACK SettingsDialogProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG: {
        HWND tabControl = GetDlgItem(dialog, IDC_S_TAB);
        g_SettingsDialog = dialog;
        if (!g_SettingsHook)
            g_SettingsHook = SetWindowsHookExA(WH_MSGFILTER, SettingsMessageFilter, NULL,
                                                GetCurrentThreadId());
        HINSTANCE instance = GetModuleHandleA(NULL);
        RECT tabRect; TCITEMA tabItem; INT index;
        for (index = 0; index < NTVDMEX_PAGE_COUNT; ++index) {
            tabItem.mask = TCIF_TEXT; tabItem.pszText = (LPSTR)g_SettingsTabs[index];
            SendMessageA(tabControl, TCM_INSERTITEMA, (WPARAM)index, (LPARAM)&tabItem);
        }
        /* WHERE THE PAGES GO: the tab control's own rectangle in dialog coordinates,
           less the strip the tabs themselves occupy. TCM_ADJUSTRECT computes the
           second part and is the only sound way to get it -- the height of a tab row
           belongs to the visual style, not to us, and hard-coding it is how a dialog
           comes out right on one theme and clipped on the next. */
        GetWindowRect(tabControl, &tabRect);
        MapWindowPoints(NULL, dialog, (POINT *)&tabRect, 2);
        SendMessageA(tabControl, TCM_ADJUSTRECT, FALSE, (LPARAM)&tabRect);
        for (index = 0; index < NTVDMEX_PAGE_COUNT; ++index) {
            g_SettingsPage[index] = CreateDialogParamA(instance, MAKEINTRESOURCEA(g_SettingsPages[index]),
                                            dialog, SettingsPageProcedure, 0);
            if (!g_SettingsPage[index]) continue;
            /* HWND_TOP, not the tab: a page placed BELOW the tab control in z-order
               is drawn over by the tab's own background and never seen. */
            SetWindowPos(g_SettingsPage[index], HWND_TOP, tabRect.left, tabRect.top,
                         tabRect.right - tabRect.left, tabRect.bottom - tabRect.top, SWP_HIDEWINDOW);
        }
        SettingsFillCombos();
        SettingsTextFontFill();                 /* #321: the installed fonts */
        /* ⚠ THE SAVED COPY, NOT THE LIVE ONE. This dialog edits the configuration
             that persists; showing session overrides here would mean pressing OK
             after changing an unrelated setting silently made every display
             experiment permanent. See the note by g_SettingsDisk. */
        SettingsToDialog(&g_SettingsDisk);
        SettingsShowPage(0);
        {   CHAR buffer[8]; DWORD got = 0; INT page;          /* SETSHOT_PATH: start on page N */
            HANDLE file = CreateFileA(SETSHOT_PATH, GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, 0, NULL);
            if (file != INVALID_HANDLE_VALUE) {
                if (ReadFile(file, buffer, 1, &got, NULL) && got && buffer[0] >= '0'
                    && (page = buffer[0] - '0') < NTVDMEX_PAGE_COUNT) {
                    SendMessageA(tabControl, TCM_SETCURSEL, (WPARAM)page, 0);
                    SettingsShowPage(page);
                }
                CloseHandle(file);
            } }
        return TRUE; }
    case WM_NOTIFY:
        if (((NMHDR *)lParam)->idFrom == IDC_S_TAB && ((NMHDR *)lParam)->code == (UINT)TCN_SELCHANGE) {
            SettingsShowPage((INT)SendMessageA(GetDlgItem(dialog, IDC_S_TAB),
                                                TCM_GETCURSEL, 0, 0));
            return TRUE;
        }
        return FALSE;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_S_DEFAULTS: {
            /* EVERY page, not just the one on screen. A "Restore Defaults" that
               silently meant "this tab only" would be the more surprising of the
               two readings, and there is no second button to offer the other. */
            NTVDMEX_SETTINGS defaults; SettingsDefaults(&defaults);
            SettingsToDialog(&defaults);           /* shown, not applied -- OK commits */
            return TRUE; }
        case IDC_S_APPLY:                     /* s84: OK's commit, without closing */
        case IDOK: {
            NTVDMEX_SETTINGS edited = g_SettingsDisk;
            SettingsFromDialog(&edited);
            SettingsClamp(&edited);
            /* Both copies: this IS the saved configuration, and it also becomes what
               is in force -- so OK deliberately drops any session-only override the
               View menu had applied. Keeping them would mean the machine no longer
               matched the dialog the user had just pressed OK on. */
            g_SettingsDisk = edited;
            g_Settings      = edited;
            SettingsSave(&g_SettingsDisk);       /* the registry IS the store        */
            SettingsApplyLive(GetParent(dialog));
            SettingsApplyDevices(&g_Settings);   /* the mixer + the presenter exist by now */
            /* ⚠ The construction-time ones (the card's port/IRQ/DMA, the output
                 rate, the window scale, the memory managers) are stored and take
                 effect at the NEXT launch. That is not a gap to hide: a Sound
                 Blaster that changes port while a game is mid-transfer, or an XMS
                 driver that vanishes from under a program that holds handles, is a
                 crash dressed up as a feature. */
            if (LOWORD(wParam) == IDC_S_APPLY) {
                SettingsToDialog(&g_SettingsDisk);   /* the clamped values, and the session row */
                return TRUE;
            }
            EndDialog(dialog, IDOK);
            return TRUE; }
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    case WM_DESTROY: {
        INT index;                                /* so a second open cannot use stale HWNDs */
        if (g_SettingsHook) { UnhookWindowsHookEx(g_SettingsHook); g_SettingsHook = 0; }
        g_SettingsDialog = 0;
        for (index = 0; index < NTVDMEX_PAGE_COUNT; ++index) {
            if (g_SettingsPage[index]) DestroyWindow(g_SettingsPage[index]);
            g_SettingsPage[index] = NULL;
        }
        return FALSE; }
    case WM_CLOSE:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}
