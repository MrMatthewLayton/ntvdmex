/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Start-up recovery: the failure counter, the start mode it selects, and what safe mode skips.
 *
 * The function definitions of dos_recovery.h, which keeps their declarations and doc comments (#335).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "dos_recovery.h"

DOS_START_MODE DosRecoveryDecideStartMode(_In_ UINT failureCount)
{
    if (failureCount >= DOS_RECOVERY_UNINSTALL_FAILURES) return DOS_START_UNINSTALL;
    if (failureCount >= DOS_RECOVERY_SAFE_MODE_FAILURES) return DOS_START_SAFE;
    return DOS_START_NORMAL;
}

DOS_SAFE_SKIPS DosRecoveryGetSafeSkips(_In_ DOS_START_MODE startMode)
{
    DOS_SAFE_SKIPS skips;
    BYTE isSkipped = (startMode == DOS_START_SAFE) ? TRUE : FALSE;
    skips.VddPlugins = isSkipped; skips.AudioOut = isSkipped; skips.RealSpeaker = isSkipped;
    skips.Joystick = isSkipped; skips.WowShims = isSkipped; skips.Fullscreen = isSkipped;
    return skips;
}

UINT DosRecoveryParseFailureCount(_In_reads_opt_(length) PCSTR text, _In_ UINT length)
{
    UINT value = 0, characterIndex, digitCount = 0;
    if (!text) return 0;
    for (characterIndex = 0; characterIndex < length; ++characterIndex)
    {
        if (text[characterIndex] >= DOS_RECOVERY_FIRST_DIGIT
            && text[characterIndex] <= DOS_RECOVERY_LAST_DIGIT)
        {
            value = value * DOS_RECOVERY_DECIMAL_BASE
                  + (UINT)(text[characterIndex] - DOS_RECOVERY_FIRST_DIGIT);
            if (++digitCount > DOS_RECOVERY_MAX_DIGITS) return 0;  /* absurd -> treat as zero */
        }
        else if (digitCount) break;                /* stop at the first non-digit */
        else if (text[characterIndex] != DOS_RECOVERY_SPACE
                 && text[characterIndex] != DOS_RECOVERY_CARRIAGE_RETURN
                 && text[characterIndex] != DOS_RECOVERY_LINE_FEED
                 && text[characterIndex] != DOS_RECOVERY_TAB)
            return 0;                                /* leading junk -> zero */
    }
    return value;
}
