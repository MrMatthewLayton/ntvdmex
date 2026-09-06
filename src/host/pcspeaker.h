/*
 * pcspeaker.h -- the REAL PC speaker, the one soldered to the motherboard.
 *
 * WHY THIS EXISTS. The speaker the emulator synthesises goes into the mixer and
 * out of the sound card, which is what DOSBox and VDMSound do and what lets a
 * beep be attenuated by the volume setting and summed with FM and PCM. It is
 * also, on a machine whose line output has nothing plugged into it, completely
 * inaudible -- and indistinguishable from a broken emulator in every counter the
 * host has. So the Audio page can ask for the real thing instead, or as well.
 *
 * ── ⚠ NOT Beep(). ───────────────────────────────────────────────────────────────
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
 *   IOCTL_BEEP_SET = CTL_CODE(FILE_DEVICE_BEEP=1, 0, METHOD_BUFFERED=0,
 *                             FILE_ANY_ACCESS=0)
 *                  = (1 << 16) | (0 << 14) | (0 << 2) | 0 = 0x00010000
 *
 * ⚠ OPENED LAZILY, ON THE FIRST TONE. Same rule as the disk images: a blocking
 *   Win32 call at startup, before the host has a window, presents as a hang and
 *   not as an error, and that has wedged the test rig before. A guest that never
 *   beeps never opens it.
 * ⚠ AND IT MUST BE STOPPED ON THE WAY OUT. The driver keeps sounding after the
 *   process that started it exits; a host that crashes mid-note would leave the
 *   machine screaming until it is rebooted.
 *
 * No CRT: kernel32 only, like the rest of the host.
 */
#ifndef NTVDMEX_PCSPEAKER_H
#define NTVDMEX_PCSPEAKER_H

#include <windows.h>

#define PCSPK_IOCTL_BEEP_SET 0x00010000u
/* Beep.sys accepts 37..32767 Hz. Outside that it fails the request, so clamp
   rather than hand it something it will refuse -- a refused IOCTL and a silent
   speaker look identical from here. */
#define PCSPK_HZ_MIN 37u
#define PCSPK_HZ_MAX 32767u
#define PCSPK_FOREVER 0xFFFFFFFFu

typedef struct pcspk {
    HANDLE h;
    int    tried;        /* 0 = never opened, 1 = opened, -1 = open failed     */
    DWORD  open_err;     /* GetLastError from a failed open, for the log       */
    DWORD  cur_hz;       /* what the DEVICE is sounding right now; 0 = silent  */
    DWORD  sets;         /* IOCTLs issued -- 0 with a tone playing is a bug    */
    DWORD  fails;        /* IOCTLs the driver refused                          */
    int    via;          /* which path opened it: 0 = GLOBALROOT, 1 = \\.\      */
} pcspk;

/* One IOCTL. `hz` 0 stops the tone; anything else starts or changes it. */
static int pcspk_ioctl(pcspk *p, DWORD hz)
{
    struct { DWORD freq, dur; } bp;
    DWORD ret = 0;
    bp.freq = hz;
    bp.dur  = hz ? PCSPK_FOREVER : 0;
    if (!DeviceIoControl(p->h, PCSPK_IOCTL_BEEP_SET, &bp, sizeof bp,
                         NULL, 0, &ret, NULL)) { p->fails++; return -1; }
    p->sets++;
    return 0;
}

/* Sound `hz`, or stop when it is 0. Cheap and idempotent: nothing is sent to the
   driver unless the tone actually CHANGED, so a caller can poll this every frame
   without turning a five-millisecond timer into five-millisecond driver calls. */
static void pcspk_set(pcspk *p, DWORD hz)
{
    if (hz) {
        if (hz < PCSPK_HZ_MIN || hz > PCSPK_HZ_MAX) hz = 0;   /* refuse, don't alias */
    }
    if (hz == p->cur_hz) return;
    if (!p->tried) {
        /* ── ⚠ THERE IS NO `\\.\Beep`. MEASURED, session 53. ────────────────────
             That is the obvious spelling and it returns ERROR_FILE_NOT_FOUND on a
             box where `sc query beep` says the driver is RUNNING -- because
             Beep.sys creates \Device\Beep and NO \DosDevices symlink for it, so
             there is nothing for the `\\.\` prefix to resolve. kernel32's own
             Beep() does not use that prefix either; it opens the NATIVE path.
             \\?\GLOBALROOT is the documented Win32 door onto the native object
             namespace, and it is how a user-mode process reaches a device whose
             author never published a DOS name.
           The old spelling is still tried second: it costs one failed open on a
           machine where it was never going to work, and if some configuration
           does publish the symlink, that machine keeps working. */
        static const char *const PATHS[2] = {
            "\\\\?\\GLOBALROOT\\Device\\Beep", "\\\\.\\Beep"
        };
        int k;
        if (!hz) return;                       /* nothing to say: stay unopened */
        for (k = 0; k < 2; ++k) {
            p->h = CreateFileA(PATHS[k], GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, 0, NULL);
            if (p->h != INVALID_HANDLE_VALUE) { p->via = k; break; }
            p->open_err = GetLastError();
        }
        if (p->h == INVALID_HANDLE_VALUE) { p->tried = -1; return; }
        p->tried = 1;
    }
    if (p->tried != 1) return;
    if (pcspk_ioctl(p, hz) == 0) p->cur_hz = hz;
}

/* Silence it and let go. Safe to call when it was never opened. */
static void pcspk_close(pcspk *p)
{
    if (p->tried == 1 && p->h != INVALID_HANDLE_VALUE) {
        if (p->cur_hz) pcspk_ioctl(p, 0);      /* ⚠ or it sounds after we exit */
        CloseHandle(p->h);
    }
    p->h = INVALID_HANDLE_VALUE;
    p->cur_hz = 0;
    p->tried = 0;
}

#endif /* NTVDMEX_PCSPEAKER_H */
