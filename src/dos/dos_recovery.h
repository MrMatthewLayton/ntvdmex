/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * What to do when the host will not start.  GH #132.
 *
 * NTVDMEX installs itself as the machine's VDM through an IFEO Debugger value on
 * ntvdm.exe. That is one registry value away from being reversible -- and one
 * registry value away from being CATASTROPHIC, because if the host wedges on
 * startup then every DOS and every Win16 launch on the machine is broken, and
 * the fix requires editing the registry with no working VDM to do it from.
 *
 * [CAUTION]: THIS IS NOT HYPOTHETICAL. Session 52 wedged the bare-metal rig TWICE in one
 * day, both times on a blocking Win32 call before the host had a window:
 * GetDiskFreeSpaceA on an empty floppy drive, and a modal "cannot find the
 * file" box under stock ntvdm. Neither could be recovered remotely -- kill,
 * reboot and shutdown all failed -- and one of them ALSO left the IFEO key
 * removed, so every later run silently measured stock ntvdm.
 *
 * The policy: count CONSECUTIVE failed STARTS. A start has FAILED until the host
 * has a window on the desktop (or its tray icon, for a Win16 launch): both s52
 * wedges were blocking Win32 calls BEFORE the window existed, and a host with a
 * window can always be closed, so the machine has not lost its VDM.
 *
 * [CAUTION]: IT USED TO SAY "until the run ends cleanly", AND THAT UNINSTALLED US ON A
 * HEALTHY MACHINE (2026-09-12). Closing the game window ends in TerminateProcess
 * (s63, deliberately -- a guest spinning in VdmStartExecution never returns), so
 * it never reached the clean-exit path; neither does a guest that crashes
 * (Mario, Heretic). The user play-tested three games, quit each by its X button,
 * and the FOURTH launch removed the IFEO key: "I couldn't get any DOS apps
 * running in NTVDMEX" -- every one of them had silently run under stock ntvdm.
 * A crash mid-game is a bug in that game's run, not evidence the VDM is broken.
 *
 * 0 .. SAFE-1        run normally
 * SAFE .. GONE-1     run in SAFE MODE -- skip everything optional
 * GONE and above     SELF-UNINSTALL: drop the IFEO value so the machine's own
 *                    ntvdm takes over again, and say so loudly
 *
 * [INFO]: SELF-UNINSTALL IS THE POINT. Safe mode is a nicety; the thing that matters
 * is that a machine cannot be left with no working VDM. Three strikes and we
 * take ourselves out of the path rather than break every 16-bit program on
 * the box until someone edits the registry by hand.
 *
 * No Windows calls, only Windows types (src/ntvdmex_types.h), so
 * tests/unit/recovery_test.c can pin it.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_DOS_RECOVERY_H
#define NTVDMEX_DOS_RECOVERY_H

#include "../ntvdmex_types.h"

#define DOS_RECOVERY_SAFE_MODE_FAILURES     2   /* This many consecutive failures -> safe mode */
#define DOS_RECOVERY_UNINSTALL_FAILURES     3   /* ...and this many -> take ourselves out */

typedef enum _DOS_START_MODE
{
    DOS_START_NORMAL = 0,
    DOS_START_SAFE   = 1,
    DOS_START_UNINSTALL = 2
} DOS_START_MODE, *PDOS_START_MODE;

/* `failureCount` is the number of consecutive starts that did NOT end cleanly, read
 * before this one is counted.
 */
DOS_START_MODE DosRecoveryDecideStartMode(_In_ UINT failureCount);

/* SAFE MODE: WHAT IT SKIPS. (s90, the remainder of #132):
 * Two failed starts in a row mean something in start-up is wedging or crashing
 * the host. Safe mode keeps the machine itself -- CPU, memory, DOS, video in a
 * window, keyboard and mouse -- and drops everything that reaches OUTSIDE the
 * process to optional hardware or foreign code, which is where both s52 wedges
 * and every "host never got a window" report has come from:
 * VddPlugins    third-party VDD DLLs from cfg\vdd.txt (foreign code, in-process)
 * AudioOut      opening waveOut/DirectSound (a broken driver blocks there); the
 *               mixer still runs and still paces the guest, into silence
 * RealSpeaker   Beep.sys through \\?\GLOBALROOT -- a device open
 * Joystick      the joyGetPosEx poll thread (winmm joystick drivers)
 * WowShims      bin\wowshim\ (two more DLLs loaded into the process)
 * Fullscreen    DirectDraw exclusive mode; the window stays a window
 * NOT a skip, deliberately: the physical floppy. The s52 empty-drive wedge was a
 * MODAL ERROR BOX, which SetErrorMode now suppresses for the whole process, and
 * there is no start-up probe of A: left to skip -- a flag here would gate nothing.
 * All or nothing by design: the count says start-up failed, not WHICH part, and a
 * third failure uninstalls us anyway.
 */
typedef struct _DOS_SAFE_SKIPS
{
    BYTE VddPlugins, AudioOut, RealSpeaker, Joystick,
         WowShims, Fullscreen;
} DOS_SAFE_SKIPS, *PDOS_SAFE_SKIPS;

typedef const DOS_SAFE_SKIPS *PCDOS_SAFE_SKIPS;

DOS_SAFE_SKIPS DosRecoveryGetSafeSkips(_In_ DOS_START_MODE startMode);

/* The counter file's text: decimal digits, perhaps after blanks. */
#define DOS_RECOVERY_FIRST_DIGIT        '0'
#define DOS_RECOVERY_LAST_DIGIT         '9'
#define DOS_RECOVERY_DECIMAL_BASE       10
#define DOS_RECOVERY_MAX_DIGITS         4       /* More than this is absurd -> zero */
#define DOS_RECOVERY_SPACE              ' '     /* The blanks skipped before the count */
#define DOS_RECOVERY_CARRIAGE_RETURN    '\r'
#define DOS_RECOVERY_LINE_FEED          '\n'
#define DOS_RECOVERY_TAB                '\t'

/* Parse the counter file's contents. Anything unreadable counts as ZERO, not as
 * a failure: a corrupt counter must not be able to uninstall us on its own, and
 * "the file is missing" is the normal state on a healthy machine.
 */
UINT DosRecoveryParseFailureCount(_In_reads_opt_(length) PCSTR text, _In_ UINT length);

#endif /* NTVDMEX_DOS_RECOVERY_H */
