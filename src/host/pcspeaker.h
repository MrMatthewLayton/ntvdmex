/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The REAL PC speaker, the one soldered to the motherboard.
 *
 * WHY THIS EXISTS. The speaker the emulator synthesises goes into the mixer and
 * out of the sound card, which is what DOSBox and VDMSound do and what lets a
 * beep be attenuated by the volume setting and summed with FM and PCM. It is
 * also, on a machine whose line output has nothing plugged into it, completely
 * inaudible -- and indistinguishable from a broken emulator in every counter the
 * host has. So the Audio page can ask for the real thing instead, or as well.
 *
 * [CAUTION]: NOT Beep() (Importance = 1):
 * The obvious route is kernel32's Beep(freq, ms), and it is the wrong shape twice
 * over: it BLOCKS the calling thread for the whole duration, and it wants that
 * duration UP FRONT. A guest does not work that way. It sets PIT channel 2, opens
 * the port-0x61 gate, and closes it whenever it likes -- possibly after four
 * milliseconds, possibly after four seconds, and in the case of a game doing 1-bit
 * sample playback, thousands of times a second. There is no (frequency, duration)
 * pair that expresses "sound this note until I say stop".
 *
 * Beep() is a wrapper over the driver underneath, and the driver DOES have that
 * shape: \Device\Beep accepts IOCTL_BEEP_SET with a duration of 0xFFFFFFFF meaning
 * "until told otherwise", and returns immediately. So we talk to it directly --
 * one IOCTL when the tone starts, one when it changes, one when it stops. That is
 * asynchronous, gate-accurate, and costs nothing while nothing is sounding.
 *
 * IOCTL_BEEP_SET = CTL_CODE(FILE_DEVICE_BEEP=1, 0, METHOD_BUFFERED=0,
 *                           FILE_ANY_ACCESS=0)
 *                = (1 << 16) | (0 << 14) | (0 << 2) | 0 = 0x00010000
 *
 * [CAUTION]: OPENED LAZILY, ON THE FIRST TONE. Same rule as the disk images: a blocking
 * Win32 call at startup, before the host has a window, presents as a hang and
 * not as an error, and that has wedged the test machine before. A guest that never
 * beeps never opens it.
 *
 * [CAUTION]: AND IT MUST BE STOPPED ON THE WAY OUT. The driver keeps sounding after the
 * process that started it exits; a host that crashes mid-note would leave the
 * machine screaming until it is rebooted.
 *
 * No CRT: kernel32 only, like the rest of the host.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_PCSPEAKER_H
#define NTVDMEX_PCSPEAKER_H

#include <windows.h>

#define PCSPEAKER_DEVICE_GLOBALROOT     "\\\\?\\GLOBALROOT\\Device\\Beep"   /* The Beep driver, by its NT name ... */
#define PCSPEAKER_DEVICE_DOS            "\\\\.\\Beep"                       /* ... or its DOS device name */

#define PCSPEAKER_IOCTL_BEEP_SET        0x00010000u

/* Beep.sys accepts 37..32767 Hz. Outside that it fails the request, so clamp
 * rather than hand it something it will refuse -- a refused IOCTL and a silent
 * speaker look identical from here.
 */
#define PCSPEAKER_HZ_MIN                37u
#define PCSPEAKER_HZ_MAX                32767u
#define PCSPEAKER_FOREVER               0xFFFFFFFFu
#define PCSPEAKER_PATHS                 2

/* OpenState: whether the device has been opened. */
#define PCSPEAKER_NOT_TRIED             0
#define PCSPEAKER_OPENED                1
#define PCSPEAKER_OPEN_FAILED           (-1)

typedef struct _PCSPEAKER
{
    HANDLE Handle;
    INT    OpenState;    /* PCSPEAKER_NOT_TRIED / _OPENED / _OPEN_FAILED */
    DWORD  OpenError;    /* GetLastError from a failed open, for the log */
    DWORD  CurrentHz;    /* what the DEVICE is sounding right now; 0 = silent */
    DWORD  IoctlCount;   /* IOCTLs issued -- 0 with a tone playing is a bug */
    DWORD  FailedCount;  /* IOCTLs the driver refused */
    INT    OpenPath;     /* which path opened it: 0 = GLOBALROOT, 1 = \\.\ */
} PCSPEAKER, *PPCSPEAKER;

/* Sound `hz`, or stop when it is 0. Cheap and idempotent: nothing is sent to the
 * driver unless the tone actually CHANGED, so a caller can poll this every frame
 * without turning a five-millisecond timer into five-millisecond driver calls.
 */
VOID PcSpeakerSet(PPCSPEAKER speaker, DWORD hz);

/* Silence it and let go. Safe to call when it was never opened. */
VOID PcSpeakerClose(PPCSPEAKER speaker);

#endif /* NTVDMEX_PCSPEAKER_H */
