/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The PC speaker through Beep.sys: set its tone and close it.
 *
 * The function definitions of pcspeaker.h, which keeps their declarations and doc comments (#335).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "pcspeaker.h"

/* One IOCTL. `hz` 0 stops the tone; anything else starts or changes it. */
static INT PcSpeakerIoctl(PPCSPEAKER speaker, DWORD hz)
{
    struct
    {
        DWORD Frequency, Duration;
    } beep;
    DWORD returned = 0;
    beep.Frequency = hz;
    beep.Duration  = hz ? PCSPEAKER_FOREVER : 0;
    if (!DeviceIoControl(speaker->Handle, PCSPEAKER_IOCTL_BEEP_SET, &beep, sizeof beep,
                         NULL, 0, &returned, NULL))
    {
        speaker->FailedCount++;
        return -1;
    }
    speaker->IoctlCount++;
    return 0;
}

VOID PcSpeakerSet(PPCSPEAKER speaker, DWORD hz)
{
    if (hz)
    {
        if (hz < PCSPEAKER_HZ_MIN || hz > PCSPEAKER_HZ_MAX) hz = 0;   /* refuse, don't alias */
    }
    if (hz == speaker->CurrentHz) return;
    if (!speaker->OpenState)
    {
        /* [CAUTION]: THERE IS NO `\\.\Beep`. MEASURED, session 53 (Importance = 1):
         * That is the obvious spelling and it returns ERROR_FILE_NOT_FOUND on a
         * box where `sc query beep` says the driver is RUNNING -- because
         * Beep.sys creates \Device\Beep and NO \DosDevices symlink for it, so
         * there is nothing for the `\\.\` prefix to resolve. kernel32's own
         * Beep() does not use that prefix either; it opens the NATIVE path.
         * \\?\GLOBALROOT is the documented Win32 door onto the native object
         * namespace, and it is how a user-mode process reaches a device whose
         * author never published a DOS name.
         * The old spelling is still tried second: it costs one failed open on a
         * machine where it was never going to work, and if some configuration
         * does publish the symlink, that machine keeps working.
         */
        static const CHAR *const paths[PCSPEAKER_PATHS] = {
            PCSPEAKER_DEVICE_GLOBALROOT, PCSPEAKER_DEVICE_DOS
        };
        INT index;
        if (!hz) return;                       /* nothing to say: stay unopened */
        for (index = 0; index < PCSPEAKER_PATHS; ++index)
        {
            speaker->Handle = CreateFileA(paths[index], GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, 0, NULL);
            if (speaker->Handle != INVALID_HANDLE_VALUE)
            {
                speaker->OpenPath = index;
                break;
            }
            speaker->OpenError = GetLastError();
        }
        if (speaker->Handle == INVALID_HANDLE_VALUE)
        {
            speaker->OpenState = PCSPEAKER_OPEN_FAILED;
            return;
        }
        speaker->OpenState = PCSPEAKER_OPENED;
    }
    if (speaker->OpenState != PCSPEAKER_OPENED) return;
    if (PcSpeakerIoctl(speaker, hz) == 0) speaker->CurrentHz = hz;
}

VOID PcSpeakerClose(PPCSPEAKER speaker)
{
    if (speaker->OpenState == PCSPEAKER_OPENED && speaker->Handle != INVALID_HANDLE_VALUE)
    {
        if (speaker->CurrentHz) PcSpeakerIoctl(speaker, 0);      /* [CAUTION] or it sounds after we exit */
        CloseHandle(speaker->Handle);
    }
    speaker->Handle = INVALID_HANDLE_VALUE;
    speaker->CurrentHz = 0;
    speaker->OpenState = PCSPEAKER_NOT_TRIED;
}
