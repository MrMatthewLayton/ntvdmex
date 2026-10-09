/* host_input.c -- keyboard and joystick input: scancodes, typematic repeat, the synthetic-key
 *   driver, modifier tracking and the low-level keyboard hook.
 *
 * Part of the host's single translation unit: #included by main.c after host_internal.h. */

/* Scripted synthetic keystrokes, on the share so a test sequence can be changed between
   runs without a rebuild. Whitespace-separated tokens, played once in order:
     4d     -- scancode 4D: make, brief hold, break
     e4d    -- the same as an EXTENDED key (E0 prefix) -- every arrow is one of these
     w1500  -- wait 1500 ms
   A hardcoded "tap UP 400 times" cannot reach a specific screen, and worse, UP is a no-op
   on a menu whose first item is already selected -- a probe that cannot tell success from
   failure. A script can say "wait for the intro, Enter, DOWN, DOWN, Enter". */
#define KEYS_PATH CFG_("keys.txt")

/* ── MEASURE THE KEYSTROKE ITSELF, BECAUSE FOUR HYPOTHESES HAVE NOW MISSED. ──────────
     Session 26: the user reports Skyroads key lag whenever the pacer runs, and it has
     survived every fix aimed at a mechanism I INFERRED -- pacer period, pacer injection,
     the frame timer's rate, pacer thread priority. Each was argued from a counter that
     measures something ADJACENT to a keystroke. So measure the keystroke, and split the
     path at the only place it can be split:
       A. QUEUE DELAY -- GetMessageTime() says when the key was POSTED; comparing with
          now says how long it sat before the UI thread got to it. This is UI-thread
          starvation, per key, and it needs no control run to interpret.
       B. DELIVERY -- from the scancode entering the 8042 FIFO to the exec loop actually
          vectoring the guest through INT 09h. This is everything downstream of us.
     If A is large the UI thread is not running; if B is large the guest is not being
     reached; if BOTH are small the lag is not in this path at all and I am still wrong.
     One producer (UI thread) and one consumer (exec thread), so the ring needs no lock --
     a torn sample costs one bucket, not a wrong conclusion. */
#define KEYLAT_RING 32
static volatile LONGLONG g_KeyLatencyTimes[KEYLAT_RING];
static volatile LONG     g_KeyLatencyHead, g_KeyLatencyTail;
static VOID KeyLatencyBucket(DWORD *histogram, DWORD milliseconds)
{
    UINT bucket = 0;
    while (bucket < 7 && milliseconds >= (DWORD)(1u << bucket)) ++bucket;   /* 0,1,2,4,8,16,32,64+ */
    histogram[bucket]++;
}
static VOID KeyLatencyPush(VOID)                      /* UI thread: a scancode was queued */
{
    LARGE_INTEGER now;
    LONG head = g_KeyLatencyHead;
    if (!QueryPerformanceCounter(&now)) return;
    g_KeyLatencyTimes[head & (KEYLAT_RING - 1)] = now.QuadPart;
    g_KeyLatencyHead = head + 1;
}
static VOID KeyLatencyPop(VOID)                       /* exec thread: INT 09h went in     */
{
    LARGE_INTEGER now;
    LONG tail = g_KeyLatencyTail;
    if (tail == g_KeyLatencyHead) return;                /* nothing outstanding */
    if (QueryPerformanceCounter(&now)) {
        DWORD milliseconds = QpcMicroseconds(now.QuadPart - g_KeyLatencyTimes[tail & (KEYLAT_RING - 1)]) / MICROSECONDS_PER_MILLISECOND_U;
        KeyLatencyBucket(g_KeyDeliveryHistogram, milliseconds);
        if (milliseconds > g_KeyDeliveryMaximumMs) g_KeyDeliveryMaximumMs = milliseconds;
        ++g_KeyDeliveryCount;
    }
    g_KeyLatencyTail = tail + 1;
}

/* ONE path for a keystroke, whoever produced it. The window proc used to latch
   g_Irq1Pending itself and never call HostIrqSink, which meant real keys bypassed BOTH
   the async delivery added for input lag AND the PIC that gates re-entry -- so every fix
   aimed at the keyboard was dead code for actual keys, and the synthetic probe tested a path
   real keys do not take. Everything goes through here now, so the probe and a human press
   the same button. */
/* ⚠ THE Win16 SIDE IS NO LONGER FED FROM HERE. (session 42) Session 41 hung a
   Win16 WM_KEYDOWN off this function, on the reasoning that one keystroke should
   have one path. That was right about the DOS path and wrong about the other one:
   a Win16 window is a REAL Win32 window now, so its keyboard input arrives as real
   Win32 messages addressed to it, with the OS's own focus deciding which window
   gets them -- see WowWinProc. Feeding the Win16 queue from the 8042 as well
   would deliver every key twice and to a window the OS had not focused. */
static VOID HostKeyScancode(BYTE rawScancode, INT extended, INT isBreak)
{
    HOST_LOCK();
    if (extended) VddInputPushScanCode(&g_Input, INPUT_SCAN_PREFIX_E0);
    VddInputPushScanCode(&g_Input, isBreak ? (BYTE)(rawScancode | INPUT_SCAN_BREAK_BIT) : rawScancode);
    HOST_UNLOCK();
    KeyLatencyPush();                  /* start the clock on this keystroke's delivery */
    /* The VDD raises IRQ1 itself now, on the 8042's empty->full transition and again as the
       guest drains the FIFO -- so the host must NOT latch one per byte here as well, or the
       interrupts run ahead of the bytes again. */
    if (g_KeyEvent) SetEvent(g_KeyEvent);
}
enum { KEYBOARD_DELAY_MAX = 3, KEYBOARD_SPEED_MAX = 31, TYPEMATIC_DELAY_STEP_US = 250000, TYPEMATIC_PERIOD_SLOWEST_US = 400000, TYPEMATIC_PERIOD_STEP_US = 12000 };   /* SPI_GETKEYBOARDDELAY 0-3 = 250-1000 ms; SPEED 0-31 = 400-28 ms */
/* XP exposes the two values it programs into the keyboard controller:
     SPI_GETKEYBOARDDELAY  0..3  -> 250, 500, 750, 1000 ms
     SPI_GETKEYBOARDSPEED  0..31 -> about 2.5/s at 0 up to about 30/s at 31,
                                    linear in PERIOD rather than in rate. */
static VOID HostKeyTypematicInitialize(VOID)
{
    DWORD delay = 1, speed = KEYBOARD_SPEED_MAX;
    if (!SystemParametersInfoA(SPI_GETKEYBOARDDELAY, 0, &delay, 0)) delay = 1;
    if (!SystemParametersInfoA(SPI_GETKEYBOARDSPEED, 0, &speed, 0)) speed = KEYBOARD_SPEED_MAX;
    if (delay > KEYBOARD_DELAY_MAX)  delay = KEYBOARD_DELAY_MAX;
    if (speed > KEYBOARD_SPEED_MAX) speed = KEYBOARD_SPEED_MAX;
    g_TypematicSpiDelay = delay; g_TypematicSpiSpeed = speed;
    g_TypematicDelayMicroseconds  = (delay + 1) * TYPEMATIC_DELAY_STEP_US;
    g_TypematicPeriodMicroseconds = TYPEMATIC_PERIOD_SLOWEST_US - (speed * TYPEMATIC_PERIOD_STEP_US);        /* 0 -> 400 ms, 31 -> 28 ms */
}
#define KEY_TYPEMATIC_DELAY_US  g_TypematicDelayMicroseconds
#define KEY_TYPEMATIC_PERIOD_US g_TypematicPeriodMicroseconds
static BYTE  g_TypematicScanCode, g_TypematicExtended, g_TypematicOn;
static LONGLONG g_TypematicDue;
static UINT32 g_TypematicSent, g_TypematicOsRepeats;  /* ours generated / OS ones suppressed */

static VOID HostKeyTypematicPress(BYTE scanCode, INT extended)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_TypematicScanCode = scanCode; g_TypematicExtended = (BYTE)(extended ? 1 : 0); g_TypematicOn = 1;
    g_TypematicDue = now.QuadPart + QpcTicks(KEY_TYPEMATIC_DELAY_US);
}

static VOID HostKeyTypematicRelease(BYTE scanCode, INT extended)
{
    if (g_TypematicOn && g_TypematicScanCode == scanCode && g_TypematicExtended == (BYTE)(extended ? 1 : 0)) g_TypematicOn = 0;
}

/* Pumped from both threads; cheap and lock-free until it actually fires. */
/* The 8042 presents the next queued scancode only after the keyboard's transfer time
   (see INPUT_KEYBOARD_TRANSFER_US in vdd_input.h). Nothing raises IRQ1 for it unless someone looks, so
   both pumps look. Lock-free when nothing is queued or an interrupt is already up. */
static VOID HostKeyPresent(VOID)
{
    if (g_Input.ScanCodeHead == g_Input.ScanCodeTail || g_Input.IsScanCodeIrqUp) return;   /* racy, benign */
    HOST_LOCK();
    VddInputPoll(&g_Input);
    HOST_UNLOCK();
}
static VOID HostKeyTypematic(VOID)
{
    LARGE_INTEGER now;
    if (!g_TypematicOn || !g_QpcFrequency.QuadPart) return;
    QueryPerformanceCounter(&now);
    if (now.QuadPart < g_TypematicDue) return;
    /* A stall must not turn into a burst of makes: schedule from NOW, not from the
       missed deadline, so we never try to "catch up" the repeats we owe. That is
       the same mistake the PIT catch-up made, and it is worse here -- a burst of
       makes is indistinguishable to the guest from the player hammering the key. */
    g_TypematicDue = now.QuadPart + QpcTicks(KEY_TYPEMATIC_PERIOD_US);
    HostKeyScancode(g_TypematicScanCode, g_TypematicExtended, INPUT_KEY_MAKE);
    g_TypematicSent++;
}

/* Defined with the rest of the mouse state, below. A scripted run needs it because
   a guest that finds an INT 33h driver asks for a CLICK and ignores the keyboard --
   Lemmings' level briefing says "Press mouse button to continue" to us and "Press
   Space" to a DOS with no driver, so without this the harness cannot get past it. */
enum { SYNTHKEY_HOLD_MS = 60, SYNTHKEY_GAP_MS = 250, SYNTHKEY_TAP_MS = 40, SYNTHKEY_SLEEP_SLICE_MS = 100, SYNTHKEY_MENU_DELAY_MS = 9000, SYNTHKEY_MENU_ROUNDS = 400, SYNTHKEY_HEX_DIGITS_MAX = 2 };   /* synthkey.txt's driver: hold, gap, tap, wait slice, menu walk; a scancode is up to two hex digits */
static DWORD WINAPI SynthKeyThread(LPVOID parameter)
{
    INT round;
    (VOID)parameter;
    /* Reach the MENU before testing menu keys. The intro/attract loop reads no keyboard at
       all (measured: int16=[0,0,0,0], p60=0 for a whole run), so arrows sent during it prove
       nothing -- Enter is what gets from the intro to the menu, per the bug report. */
    /* A script on the share wins, if there is one: it can aim at a particular screen. */
    { HANDLE handle = CreateFileA(KEYS_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
      if (handle != INVALID_HANDLE_VALUE) {
          CHAR script[1024]; DWORD bytesRead = 0; DWORD index = 0;
          ReadFile(handle, script, sizeof script - 1, &bytesRead, NULL);
          CloseHandle(handle);
          script[bytesRead] = 0;
          while (index < bytesRead && g_Running) {
              INT extended = 0; DWORD value = 0; INT digits = 0;
              while (index < bytesRead && (script[index] == ' ' || script[index] == '\t' || script[index] == '\r' || script[index] == '\n')) ++index;
              if (index >= bytesRead) break;
              /* m<0|1|2> -- a full click of that button (left/right/middle). Not a
                 keystroke, but it belongs in the same script: the point of the script
                 is to reach a screen, and mouse-driven guests cannot be reached with
                 scancodes. `dm0`/`um0` are not provided; nothing has wanted a held
                 button yet, and an unreleased one is a nasty thing to leave behind. */
              if (script[index] == 'm' || script[index] == 'M') {
                  INT button = 0;
                  ++index;
                  if (index < bytesRead && script[index] >= '0' && script[index] <= '2') { button = script[index] - '0'; ++index; }
                  HostMouseButton(button, INPUT_PRESSED);
                  Sleep(SYNTHKEY_HOLD_MS);
                  HostMouseButton(button, INPUT_RELEASED);
                  Sleep(SYNTHKEY_GAP_MS);
                  continue;
              }
              if (script[index] == 'w' || script[index] == 'W') {           /* w<decimal ms> */
                  ++index;
                  while (index < bytesRead && script[index] >= '0' && script[index] <= '9') { value = value*DECIMAL_RADIX + (DWORD)(script[index]-'0'); ++index; }
                  { DWORD slept = 0;                       /* sleep in slices so a wind-down
                                                              is not stuck behind a long wait */
                    while (slept < value && g_Running) { Sleep(value - slept > SYNTHKEY_SLEEP_SLICE_MS ? SYNTHKEY_SLEEP_SLICE_MS : value - slept);
                                                     slept += SYNTHKEY_SLEEP_SLICE_MS; } }
                  continue;
              }
              /* ── A MODIFIER HAS TO BE HELD, AND EVERY TOKEN HERE WAS A TAP. ──────
                   Each token below is sent as make-then-break, which is right for a
                   character and useless for SHIFT: `2a 34 aa` released shift before
                   the period arrived, so a test that typed `>` typed `.` instead --
                   and the shell dutifully echoed `echo hello world . hi.txt`, i.e.
                   the harness silently tested something other than redirection.
                   `d` = make only, `u` = break only. `d2a 34 u2a` is a held shift. */
              if (script[index] == 'd' || script[index] == 'D' || script[index] == 'u' || script[index] == 'U') {
                  INT isUp = (script[index] == 'u' || script[index] == 'U');
                  ++index;
                  if (index < bytesRead && (script[index] == 'e' || script[index] == 'E')) { extended = 1; ++index; }
                  while (index < bytesRead && digits < SYNTHKEY_HEX_DIGITS_MAX) {
                      CHAR character = script[index]; INT digit = -1;
                      if (character >= '0' && character <= '9') digit = character - '0';
                      else if (character >= 'a' && character <= 'f') digit = character - 'a' + HEX_DIGIT_A_VALUE;
                      else if (character >= 'A' && character <= 'F') digit = character - 'A' + HEX_DIGIT_A_VALUE;
                      if (digit < 0) break;
                      value = (value << NIBBLE_SHIFT) | (DWORD)digit; ++index; ++digits;
                  }
                  if (!digits) continue;
                  HostKeyScancode((BYTE)value, extended, isUp);
                  Sleep(SYNTHKEY_TAP_MS);
                  continue;
              }
              if (script[index] == 'e' || script[index] == 'E') { extended = 1; ++index; }
              while (index < bytesRead && digits < SYNTHKEY_HEX_DIGITS_MAX) {               /* up to two hex digits */
                  CHAR character = script[index]; INT digit = -1;
                  if (character >= '0' && character <= '9') digit = character - '0';
                  else if (character >= 'a' && character <= 'f') digit = character - 'a' + HEX_DIGIT_A_VALUE;
                  else if (character >= 'A' && character <= 'F') digit = character - 'A' + HEX_DIGIT_A_VALUE;
                  if (digit < 0) break;
                  value = (value << NIBBLE_SHIFT) | (DWORD)digit; ++index; ++digits;
              }
              if (!digits) { ++index; continue; }              /* skip a token we do not grok */
              HostKeyScancode((BYTE)value, extended, INPUT_KEY_MAKE);
              Sleep(SYNTHKEY_HOLD_MS);                                    /* a human-length hold */
              HostKeyScancode((BYTE)value, extended, INPUT_KEY_BREAK);
              Sleep(SYNTHKEY_GAP_MS);
          }
          return 0;
      } }

    Sleep(SYNTHKEY_MENU_DELAY_MS);
    for (round = 0; round < SYNTHKEY_MENU_ROUNDS && g_Running; ++round) {
        /* An EXTENDED key (the arrows a player actually holds) at the OS auto-repeat rate,
           through exactly what WM_KEYDOWN does: E0 prefix + make code on the raw FIFO with
           an IRQ1 per byte, AND the BIOS ring entry that INT 16h returns. Feeding only the
           FIFO -- which the first version of this probe did -- means a game that reads INT
           16h never sees the key at all, so the probe passed while a real press killed it. */
        /* DOWN, not UP, and no Enter first. The menu opens with "Start!" already selected --
           the TOP item -- so a working UP arrow moves the highlight nowhere and a probe
           built on it cannot tell success from failure. DOWN has somewhere to go, and
           staying out of the game keeps the highlight on screen where a screenshot sees it. */
        HostKeyScancode(INPUT_SCANCODE_DOWN, INPUT_KEY_EXTENDED, INPUT_KEY_MAKE);                   /* INT 09h fills the BIOS ring now */
        Sleep(SYNTHKEY_GAP_MS);                                      /* menu-paced taps, not a hold */
        HostKeyScancode(INPUT_SCANCODE_DOWN, INPUT_KEY_EXTENDED, INPUT_KEY_BREAK);                   /* release each time          */
        Sleep(SYNTHKEY_GAP_MS);
    }
    return 0;
}
static volatile LONG g_WindowsKeyDown;         /* Win held -- maintained by the hook, see below */
static LRESULT CALLBACK LowLevelKeyboardProcedure(INT code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && g_Captured && GetForegroundWindow() == g_Window) {
        KBDLLHOOKSTRUCT *hook = (KBDLLHOOKSTRUCT *)lParam;
        INT altDown  = (hook->flags & LLKHF_ALTDOWN) != 0;
        INT ctrl = (GetKeyState(VK_CONTROL) & HOST_KEY_DOWN_BIT) != 0;
        INT down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        /* ⚠⚠ NEVER SWALLOW A KEY-UP. Capture is entered with Win+F10 while the hook is
             NOT yet installed, so Windows SEES the Win key go down; the hook is installed
             a moment later and used to eat the key-UP, leaving the system believing Win
             was held forever. Every subsequent keystroke then became Win+key -- pressing
             D minimised the window, because Win+D is Show Desktop (user-reported, and it
             is what pointed straight at this). Swallowing a down without its up is a
             stuck modifier; only downs may be eaten. */
        if (!down) { if (hook->vkCode == VK_LWIN || hook->vkCode == VK_RWIN)
                         InterlockedExchange(&g_WindowsKeyDown, 0);
                     return CallNextHookEx(g_LowLevelKeyboard, code, wParam, lParam); }
        /* Swallow only, and deliberately do NOT push these to the guest from here: a
           low-level hook that blocks is torn down by Windows, and HostKeyScancode
           takes g_Lock. Losing Alt+Tab to the guest costs nothing; stalling the hook
           would cost every key. */
        if (hook->vkCode == VK_LWIN || hook->vkCode == VK_RWIN) {
            /* Swallowing the down also stops the SYSTEM tracking it, so GetAsyncKeyState
               would report Win as up and the Win+F10 release chord could never fire while
               captured -- the exact state you need it in. Track it here instead. */
            InterlockedExchange(&g_WindowsKeyDown, 1);
            return 1;
        }
        if (hook->vkCode == VK_TAB    && altDown)  return 1;
        if (hook->vkCode == VK_ESCAPE && (ctrl || altDown)) return 1;
    }
    return CallNextHookEx(g_LowLevelKeyboard, code, wParam, lParam);
}

/* ── SETTINGS: THE STORE, AND WHAT APPLYING THEM MEANS. ──────────────────────────
     g_Settings is the live copy, and the three SettingsApply* functions below are
     deliberately the ONLY places a setting reaches the machine, so "what does this
     knob actually do" has one answer and the dialog cannot drift from startup.
     They are split by WHEN they can run, not by what they configure:

       SettingsApply()          -- knobs that exist before anything is built.
                                    ⚠ Every one of these is ALSO a text file on the
                                      test share and THE FILE WINS (see settings.h),
                                      so this runs BEFORE the file-knob block in
                                      WinMain and never again afterwards.
       SettingsApplyDevices()  -- the mixer and the presenter. Both zero their own
                                    struct when they initialise, so pushing into them
                                    any earlier writes into a struct that is about to
                                    be wiped. None of these has a file-knob twin, so
                                    running it late costs the precedence rule nothing.
       SettingsApplyPresent()  -- the display half of the above on its own, for the
                                    UI thread, which builds its presenter later still.

   ⚠ THESE FUNCTIONS ARE ALSO THE HONEST LIST OF WHAT WORKS. What is NOT here is not
     honoured, however faithfully it round-trips through HKCU:
       CPU page     -- CpuType/CpuCore/Fpu/Cycles/Turbo. This host runs 16-bit code
                       on the REAL CPU, so there is no core to choose and no cycles
                       to count. ★ SpeedMode IS NOW LIVE (GH #56): it is the
                       approximate-speed dropdown, and because a real CPU cannot be
                       clocked down it is a DUTY CYCLE -- see src/host/cpuspeed.h,
                       which also carries the one calibration constant.
       ★ #136 (s92): ConventionalKB (g_DosMemoryTop, start-up only), Midi (AudioWaveMidiOpen,
                       start-up only), SeamlessMouse (CaptureAllowed) are live; so are
                       HostCursorMode and FloppyUsePhysical, which were read here since s84.
       STILL STORED ONLY, each for a reason the startup report prints (SettingsDeadWhy):
         Umb          -- there are no upper memory blocks to link (XMS 10h = B1h, AH=5803h
                         refused). Providing them is an arena in C000-EFFF, not a switch.
         A20          -- "A20 Line Always Enabled": the 1 MB wrap is not modelled, so the
                         line IS always enabled and the unchecked state cannot be honoured
                         without remapping views on every gate toggle (dos_xms.h). The
                         gate FLAG already follows the guest; locking it would change the
                         default, which today honours a guest's disable.
         CdRomUsePhysical, CdRomImage -- no CD-ROM is mounted into DOS (#240/#241).
         SoundFontPath -- there is no SF2 synth in this host; Midi=SoundFont routes to a
                         host SoundFont DRIVER, which keeps its own list (midi_route.h).
       (FloppyAImage IS live: it is what INT 13h opens. JoystickType and
       JoystickGamepad ARE live as of session 62: the gameport VDD and the winmm
       poll thread consume them.) */
/* ── THE GAMEPORT'S CLOCK AND ITS POLL THREAD. (session 62) ──────────────────────
     The VDD times its one-shots off this injected clock -- QPC, the same timebase
     as everything else here. Absolute microseconds, not a delta: only differences
     are ever taken. */
static UINT64 JoystickNowMicroseconds(PVOID context)
{
    LARGE_INTEGER now; (VOID)context;
    QueryPerformanceCounter(&now);
    return QpcMicroseconds64(now.QuadPart);
}
enum { JOYSTICK_ABSENT_POLL_MS = 250, JOYSTICK_API_RETRY_MS = 1000, JOYSTICK_POLL_MS = 15 };   /* the poll thread: no stick configured, no joyGetPosEx, and the sample period */
/* joyGetPosEx costs a driver round-trip, so it must NEVER run inside the port
   trap -- the guest polls 0x201 in a tight CLI loop precisely while measuring an
   axis. This thread samples at ~66 Hz into g_Joystick and the trap reads only the
   cached bytes. winmm binds dynamically like waveOut (audio_wave.c): no new
   import, and a machine with no multimedia stack still boots. XP-safe on
   purpose -- joyGetPosEx sees an Xbox 360 pad through xusb; XInput does not
   exist down-level and would be a new allowlist DLL. */
typedef DWORD (WINAPI *PFN_JOY_GET_POS_EX)(UINT, JOYINFOEX *);
static DWORD WINAPI JoystickPollThread(LPVOID param)
{
    HMODULE module = NULL; PFN_JOY_GET_POS_EX getPositionEx = NULL;
    (VOID)param;
    /* ⚠ BELOW the pacer, ALWAYS. joyGetPosEx is a legacy-driver round trip that can
         block for milliseconds, and the s61 Skyroads timing is fragile to exactly
         this kind of contention on a 2-core box. This thread must never be able to
         delay a frame present or a PIT tick, so it sits below NORMAL -- the present
         pacer (g_PitPacePriority = NORMAL) and the exec thread always win. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        if (g_Joystick.Type == JOYSTICK_TYPE_NONE) { g_Joystick.IsPresent = 0; Sleep(JOYSTICK_ABSENT_POLL_MS); continue; }
        if (!getPositionEx) {
            if (!module) module = LoadLibraryA(HOST_MODULE_WINMM);
            getPositionEx = module ? (PFN_JOY_GET_POS_EX)GetProcAddress(module, HOST_EXPORT_JOY_GET_POS_EX) : NULL;
            if (!getPositionEx) { g_Joystick.IsPresent = 0; Sleep(JOYSTICK_API_RETRY_MS); continue; }
        }
        {   JOYINFOEX info; UINT axes[JOYSTICK_AXES]; INT index;
            info.dwSize = sizeof info;
            info.dwFlags = JOY_RETURNALL;                    /* JOY_RETURNALL: X Y Z R U V POV buttons */
            if (getPositionEx(0, &info) == 0) {            /* JOYSTICKID1, JOYERR_NOERROR */
                /* winmm's view of a 360 pad on XP: X/Y = left stick, U/R = right
                   stick, Z = triggers (unused here), POV = the D-pad. */
                axes[0] = (info.dwXpos >> BYTE_SHIFT) & BYTE_MASK; axes[1] = (info.dwYpos >> BYTE_SHIFT) & BYTE_MASK;
                axes[2] = (info.dwUpos >> BYTE_SHIFT) & BYTE_MASK; axes[3] = (info.dwRpos >> BYTE_SHIFT) & BYTE_MASK;
                if (g_JoystickPovMap && info.dwPOV < JOYSTICK_POV_FULL_CIRCLE) {
                    /* Hundredths of a degree, 0 = up. Snap the eight sectors
                       onto axis A extremes -- a DOS platformer reads digital
                       directions out of the analog port, and a held D-pad must
                       pin the axis, not average with a centred stick. */
                    INT povOctant = (INT)(((info.dwPOV + JOYSTICK_POV_OCTANT_U / 2) / JOYSTICK_POV_OCTANT_U) & JOYSTICK_POV_OCTANT_MASK);
                    if (povOctant == JOYSTICK_POV_NORTH_WEST || povOctant == JOYSTICK_POV_NORTH || povOctant == JOYSTICK_POV_NORTH_EAST) axes[1] = JOYSTICK_AXIS_MIN;
                    if (povOctant >= JOYSTICK_POV_SOUTH_EAST && povOctant <= JOYSTICK_POV_SOUTH_WEST) axes[1] = JOYSTICK_AXIS_MAX;
                    if (povOctant >= JOYSTICK_POV_NORTH_EAST && povOctant <= JOYSTICK_POV_SOUTH_EAST) axes[0] = JOYSTICK_AXIS_MAX;
                    if (povOctant >= JOYSTICK_POV_SOUTH_WEST && povOctant <= JOYSTICK_POV_NORTH_WEST) axes[0] = JOYSTICK_AXIS_MIN;
                }
                for (index = 0; index < JOYSTICK_AXES; ++index) g_Joystick.Axis[index] = (BYTE)axes[index];
                g_Joystick.Buttons = (BYTE)(info.dwButtons & JOYSTICK_BUTTON_MASK);
                g_Joystick.IsPresent = 1;
            } else {
                g_Joystick.IsPresent = 0;                 /* unplugged mid-run is fine */
            }
        }
        Sleep(JOYSTICK_POLL_MS);
    }
}

/* Spawn the joystick poll thread AT MOST ONCE, and only when a joystick is
   configured. Called from SettingsApply, which runs at startup and on every
   dialog OK. The default (JoystickType=None) never reaches the create, so a
   machine that has never enabled a gamepad has NO poll thread at all -- the
   whole point, so a non-joystick game (Skyroads and friends) keeps the exact
   thread landscape the s61 timing was tuned against. Once created the thread
   lives on and idles at 4 Hz if the type is later set back to None; recreating
   and joining a thread on a setting change is not worth the complexity when the
   idle cost is a Sleep. */
static LONG g_JoystickThreadStarted = 0;
static VOID JoystickPollEnsure(VOID)
{
    if (g_Joystick.Type == JOYSTICK_TYPE_NONE || g_Safe.Joystick) return;   /* s90 #132 */
    if (InterlockedExchange(&g_JoystickThreadStarted, 1)) return;   /* once */
    { HANDLE thread = CreateThread(NULL, 0, JoystickPollThread, NULL, 0, NULL);
      if (thread) CloseHandle(thread);
      else InterlockedExchange(&g_JoystickThreadStarted, 0); }       /* retry next apply */
}

enum { MODIFIER_LEFT_SHIFT = 0, MODIFIER_RIGHT_SHIFT = 1, MODIFIER_LEFT_CTRL = 2, MODIFIER_RIGHT_CTRL = 3, MODIFIER_LEFT_ALT = 4, MODIFIER_RIGHT_ALT = 5, MODIFIER_KEYS = 6 };   /* g_ModifiersDown's bits */
#define LPARAM_KEY_PREVIOUS 0x40000000   /* bit 30: the key was already down (a repeat) */
#define LPARAM_KEY_EXTENDED 0x01000000   /* WM_KEYDOWN lParam bit 24 */
/* One keystroke, ONE path -- shared by WM_KEYDOWN and WM_SYSKEYDOWN, because F10 and
   Alt arrive as SYSTEM keys and are just as much the guest's as any other. */
static VOID KeyMessageNote(VOID)
{
    /* How long did this key sit in the queue before we got to it? GetMessageTime says
       when it was posted; this is UI-thread starvation measured on the key itself. */
    DWORD queueDelay = GetTickCount() - (DWORD)GetMessageTime();
    if ((LONG)queueDelay < 0) queueDelay = 0;
    KeyLatencyBucket(g_KeyMessageHistogram, queueDelay); ++g_KeyMessageCount;
    if (queueDelay > g_KeyMessageMaximumMs) g_KeyMessageMaximumMs = queueDelay;
}
/* ── #274: THE TWO KEYS WHOSE BYTES ARE NOT `[E0] code` / `[E0] code|80h`. ────────────
     Pause and Ctrl+Break (VddInputHostKeyBytes has the sequences and the sources).
     Both send everything on the PRESS, nothing on the release, and never auto-repeat --
     so pressing one also ends the previous key's typematic, as on the real keyboard,
     where the repeat belongs to the last key pressed. Pause used to go out as a plain
     45/C5, which our BIOS (correctly) read as NumLock: the Pause key toggled NumLock.
     Ctrl+Break went out as E0 46 ... E0 C6 at key-up and repeated while held, so
     holding it fired INT 1Bh at the typematic rate. Returns 1 if it handled the key. */
static INT HostKeySpecial(BYTE rawScancode, INT extended, INT isBreak)
{
    BYTE bytes[6];
    INT count, noReport, index;
    count = VddInputHostKeyBytes(rawScancode, extended, isBreak, bytes, &noReport);
    if (!noReport) return 0;
    if (count) {
        g_TypematicOn = 0;
        HOST_LOCK();
        for (index = 0; index < count; ++index) VddInputPushScanCode(&g_Input, bytes[index]);
        HOST_UNLOCK();
        KeyLatencyPush();
        if (g_KeyEvent) SetEvent(g_KeyEvent);
    }
    return 1;
}
static VOID KeyPushMake(LPARAM lParam)
{
    BYTE rawScancode = (BYTE)((lParam >> WORD_SHIFT) & BYTE_MASK);
    INT extended = (lParam & LPARAM_KEY_EXTENDED) != 0;
    /* Bit 30 = the key was ALREADY down, i.e. OS auto-repeat. We generate typematic
       ourselves, so swallow it -- two sources would double the repeat rate. Counted,
       not silently dropped: the count is how we tell "the OS stopped sending them"
       from "we stopped listening". */
    if (lParam & LPARAM_KEY_PREVIOUS) { g_TypematicOsRepeats++; return; }
    if (rawScancode && HostKeySpecial(rawScancode, extended, INPUT_KEY_MAKE)) return;   /* #274: Pause, Ctrl+Break */
    if (rawScancode) { HostKeyScancode(rawScancode, extended, INPUT_KEY_MAKE); HostKeyTypematicPress(rawScancode, extended);
                 ModifierTrack(rawScancode, extended, INPUT_PRESSED); }
}
static VOID KeyPushBreak(LPARAM lParam)
{
    BYTE rawScancode = (BYTE)((lParam >> WORD_SHIFT) & BYTE_MASK);
    INT extended = (lParam & LPARAM_KEY_EXTENDED) != 0;
    if (rawScancode && HostKeySpecial(rawScancode, extended, INPUT_KEY_BREAK)) return;   /* #274: they send no break */
    if (rawScancode) { HostKeyTypematicRelease(rawScancode, extended);   /* stop repeating first */
                 HostKeyScancode(rawScancode, extended, INPUT_KEY_BREAK);
                 ModifierTrack(rawScancode, extended, INPUT_RELEASED); }
}
/* ── LOSING FOCUS RELEASES THE MODIFIERS. ─────────────────────────────────────────────
     Windows delivers a key's UP to whichever window has focus WHEN IT IS RELEASED. So
     Alt+Tab away from us sends the guest Alt's make and never its break: 0040:0017 says
     Alt is held for the rest of the run, and the first letter typed on return is an Alt
     accelerator -- or, for a game reading port 60h, Ctrl stays "fired". Tracked here
     from what we actually pushed (not from the BDA, which a guest hooking INT 09h never
     updates), and released as synthetic breaks on WM_KILLFOCUS. */
static BYTE g_ModifiersDown;                              /* bits: 0 LSh 1 RSh 2 LCtl 3 RCtl 4 LAlt 5 RAlt */
static VOID ModifierTrack(BYTE rawScancode, INT extended, INT down)
{
    INT bit = -1;
    if (!extended) { if (rawScancode == INPUT_SCAN_LEFT_SHIFT) bit = MODIFIER_LEFT_SHIFT; else if (rawScancode == INPUT_SCAN_RIGHT_SHIFT) bit = MODIFIER_RIGHT_SHIFT;
                else if (rawScancode == INPUT_SCAN_CTRL) bit = MODIFIER_LEFT_CTRL; else if (rawScancode == INPUT_SCAN_ALT) bit = MODIFIER_LEFT_ALT; }
    else      { if (rawScancode == INPUT_SCAN_CTRL) bit = MODIFIER_RIGHT_CTRL; else if (rawScancode == INPUT_SCAN_ALT) bit = MODIFIER_RIGHT_ALT; }
    if (bit < 0) return;
    if (down) g_ModifiersDown |= (BYTE)(1u << bit); else g_ModifiersDown &= (BYTE)~(1u << bit);
}
static VOID HostReleaseModifiers(VOID)
{
    static const struct { BYTE ScanCode; INT IsExtended; } mods[MODIFIER_KEYS] =
        { {INPUT_SCAN_LEFT_SHIFT,0}, {INPUT_SCAN_RIGHT_SHIFT,0}, {INPUT_SCAN_CTRL,0}, {INPUT_SCAN_CTRL,1}, {INPUT_SCAN_ALT,0}, {INPUT_SCAN_ALT,1} };
    INT index;
    for (index = 0; index < MODIFIER_KEYS; ++index)
        if (g_ModifiersDown & (1u << index)) {
            HostKeyTypematicRelease(mods[index].ScanCode, mods[index].IsExtended);
            HostKeyScancode(mods[index].ScanCode, mods[index].IsExtended, INPUT_KEY_BREAK);
        }
    g_ModifiersDown = 0;
}
