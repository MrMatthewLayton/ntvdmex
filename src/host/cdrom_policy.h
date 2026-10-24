/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * When a change on Settings > Drives > CD-ROM has to re-mount the ISO drive, and which
 * letter each mounted image -- the ISO and the floppy -- takes.  GH #240, #241.
 *
 * Pure: the host's HostCdromApply (host_cdrom.c) asks it, and cdrom_policy_test pins it.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_HOST_CDROM_POLICY_H
#define NTVDMEX_HOST_CDROM_POLICY_H

#include "../ntvdmex_types.h"

/* WHICH LETTER THE ISO DRIVE TAKES. MSCDEX's rule is the first free letter after the
 * machine's last drive -- but a mapped network drive at Z: (s101: the test machine's
 * \\server\storage) left no letter after it, and the drive was never mounted. DOS
 * itself never saw those: a redirector's letters come after MSCDEX's. So the last drive
 * counted is the last LOCAL one; failing that, the lowest free letter from D: up.
 * presentMask / remoteMask: bit n = drive 'A'+n (GetLogicalDrives, and the DRIVE_REMOTE
 * ones among them). Returns the drive index, or -1 when every letter is taken.
 */
#define CDROM_POLICY_LETTERS            26
#define CDROM_POLICY_FIRST_CHOICE       3   /* D: -- A:/B: are floppies, C: the system */
#define DRIVE_LETTER_AUTOMATIC          0   /* A stored image letter of 0: the first free one */
#define DRIVE_LETTER_FIRST_HARD_DISK    2   /* C: -- the ISO is never offered A: or B: */

/* Two image paths name the same file, as Win32 compares them: letters case-blind. */
static inline INT CdromPolicySamePath(_In_ PCSTR first, _In_ PCSTR second)
{
    for (;; ++first, ++second)
    {
        CHAR firstCharacter = *first;
        CHAR secondCharacter = *second;

        if (firstCharacter >= 'A' && firstCharacter <= 'Z')
            firstCharacter = (CHAR)(firstCharacter | ASCII_CASE_BIT);

        if (secondCharacter >= 'A' && secondCharacter <= 'Z')
            secondCharacter = (CHAR)(secondCharacter | ASCII_CASE_BIT);

        if (firstCharacter != secondCharacter)
            return 0;

        if (!firstCharacter)
            return 1;
    }
}

/* 1 = unmount what is there (if anything) and mount what is wanted (if anything).
 * Physical -> physical changes nothing: the host's own drives are DOS's already. Image ->
 * image with the same file changes nothing either -- the guest keeps its current
 * directory on the disc. Anything else is a different drive: physical -> image mounts,
 * image -> physical unmounts, another image path swaps the disc.
 */
static inline INT CdromPolicyNeedsRemount(
    _In_ INT isMountedImage,
    _In_ PCSTR mountedPath,
    _In_ INT isWantedImage,
    _In_ PCSTR wantedPath)
{
    if (!isMountedImage && !isWantedImage)
        return 0;

    if (isMountedImage != isWantedImage)
        return 1;

    return !CdromPolicySamePath(mountedPath ? mountedPath : "", wantedPath ? wantedPath : "");
}

static inline INT CdromPolicyChooseLetter(_In_ DWORD presentMask, _In_ DWORD remoteMask)
{
    INT lastLocal = -1;
    INT letter;

    for (letter = 0; letter < CDROM_POLICY_LETTERS; ++letter)
        if ((presentMask & (1u << letter)) && !(remoteMask & (1u << letter)))
            lastLocal = letter;

    for (letter = lastLocal + 1; letter < CDROM_POLICY_LETTERS; ++letter)
        if (!(presentMask & (1u << letter)))
            return letter;

    for (letter = CDROM_POLICY_FIRST_CHOICE; letter < CDROM_POLICY_LETTERS; ++letter)
        if (!(presentMask & (1u << letter)))
            return letter;

    return -1;
}

/* THE LETTER A MOUNTED IMAGE TAKES -- THE USER'S, IF WINDOWS IS NOT USING IT. (s101, user:
 * "always respect the drives that are set up in Windows ... mounting a floppy should specify
 * an available drive letter, and the same goes for CD/DVD-ROM ISOs.") Settings > Drives
 * stores one per image: 0 = automatic, else 1..26 = A:..Z:. A stored letter Windows now
 * uses, or the other image holds (`avoidMask`), is not taken: the choice falls back to
 * automatic -- CdromPolicyChooseLetter with those letters counted as present.
 * Returns the drive index, or -1 when every letter is taken.
 */
static inline INT DriveLetterPolicyChoose(
    _In_ DWORD storedLetter,
    _In_ DWORD presentMask,
    _In_ DWORD remoteMask,
    _In_ DWORD avoidMask)
{
    DWORD takenMask = presentMask | avoidMask;

    if (storedLetter >= 1 && storedLetter <= CDROM_POLICY_LETTERS && !(takenMask & (1u << (storedLetter - 1))))
        return (INT)storedLetter - 1;

    return CdromPolicyChooseLetter(takenMask, remoteMask);
}

/* May Settings > Drives offer `letter` for an image? A letter Windows is not using, or
 * the one this image already holds (`ownMask` -- the ISO's mapping IS a Windows drive while
 * it is mounted); never one the other image holds (`avoidMask`); and for the ISO not A:
 * or B:, the floppies' letters. The floppy image may be A: or B: on a PC without the drive.
 */
static inline INT DriveLetterPolicyIsOffered(
    _In_ INT letter,
    _In_ DWORD presentMask,
    _In_ DWORD ownMask,
    _In_ DWORD avoidMask,
    _In_ INT isFloppy)
{
    DWORD bit;

    if (letter < 0 || letter >= CDROM_POLICY_LETTERS || (!isFloppy && letter < DRIVE_LETTER_FIRST_HARD_DISK))
        return 0;

    bit = 1u << letter;
    return !(avoidMask & bit) && (!(presentMask & bit) || (ownMask & bit));
}

#endif /* NTVDMEX_HOST_CDROM_POLICY_H */
