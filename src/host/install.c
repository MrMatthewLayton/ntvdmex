/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Becoming the machine's VDM, reversibly: the IFEO Debugger value, read, written and removed.
 *
 * The function definitions of install.h, which keeps their declarations and doc comments (#335).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "install.h"

INT InstallIsSamePath(PCSTR first, PCSTR second)
{
    INT firstIndex = 0;
    INT secondIndex = 0;
    INT firstEnd;
    INT secondEnd;

    if (!first || !second)
        return 0;
    while (first[firstIndex] == ' ' || first[firstIndex] == '\t' || first[firstIndex] == '"')
        ++firstIndex;
    while (second[secondIndex] == ' ' || second[secondIndex] == '\t' || second[secondIndex] == '"')
        ++secondIndex;
    firstEnd = firstIndex;
    while (first[firstEnd])
        ++firstEnd;
    secondEnd = secondIndex;
    while (second[secondEnd])
        ++secondEnd;
    while (firstEnd > firstIndex && (first[firstEnd-1] == ' ' || first[firstEnd-1] == '\t' || first[firstEnd-1] == '"'))
        --firstEnd;
    while (secondEnd > secondIndex && (second[secondEnd-1] == ' ' || second[secondEnd-1] == '\t' || second[secondEnd-1] == '"'))
        --secondEnd;
    if (firstEnd - firstIndex != secondEnd - secondIndex)
        return 0;
    while (firstIndex < firstEnd)
    {
        CHAR firstChar = first[firstIndex];
        CHAR secondChar = second[secondIndex];
        if (firstChar >= 'A' && firstChar <= 'Z')
            firstChar = (CHAR)(firstChar - 'A' + 'a');
        if (secondChar >= 'A' && secondChar <= 'Z')
            secondChar = (CHAR)(secondChar - 'A' + 'a');
        /* A forward slash is a legal separator in a Win32 path and a person who
         * types one has still named the same file.
         */
        if (firstChar == '/')
            firstChar = '\\';
        if (secondChar == '/')
            secondChar = '\\';
        if (firstChar != secondChar)
            return 0;
        ++firstIndex;
        ++secondIndex;
    }
    return 1;
}

INSTALL_STATE InstallClassify(PCSTR current, PCSTR self)
{
    INT index = 0;

    if (!current)
        return INSTALL_ABSENT;
    while (current[index] == ' ' || current[index] == '\t' || current[index] == '"')
        ++index;
    if (!current[index])
        return INSTALL_ABSENT;                       /* present but empty is not installed */
    return InstallIsSamePath(current, self) ? INSTALL_OURS : INSTALL_OTHER;
}

INT InstallNamesNtvdmex(PCSTR current)
{
    static const CHAR wanted[] = INSTALL_HOST_NAME;
    PCSTR scan;
    PCSTR nameStart;
    INT index;
    INT length = 0;

    if (!current)
        return 0;
    while (*current == ' ' || *current == '\t' || *current == '"')
        ++current;
    for (scan = current; *scan && *scan != '"'; ++scan)             /* up to a closing quote */
    {
        length = (INT)(scan - current) + 1;
        if (length >= INSTALL_HOST_NAME_LENGTH)                      /* ends in "ntvdmhost.exe"? */
        {
            for (index = 0, nameStart = scan - (INSTALL_HOST_NAME_LENGTH - 1); index < INSTALL_HOST_NAME_LENGTH; ++index)
            {
                CHAR character = nameStart[index];
                if (character >= 'A' && character <= 'Z')
                    character = (CHAR)(character - 'A' + 'a');
                if (character != wanted[index])
                    break;
            }
            if (index == INSTALL_HOST_NAME_LENGTH && (nameStart == current || nameStart[-1] == '\\' || nameStart[-1] == '/')
                && (scan[1] == 0 || scan[1] == '"' || scan[1] == ' ' || scan[1] == '\t'))
                return 1;
        }
    }
    return 0;
}

INSTALL_ACTION InstallPlanEx(
    INSTALL_STATE state,
    INT isWantInstalled,
    INT hasPrevious,
    INT isOtherUs,
    INT isForce)
{
    if (!isWantInstalled && state == INSTALL_OTHER && (isOtherUs || isForce))
        return hasPrevious ? INSTALL_ACT_RESTORE : INSTALL_ACT_DELETE;
    return InstallPlan(state, isWantInstalled, hasPrevious);
}

INSTALL_ACTION InstallPlan(INSTALL_STATE state, INT isWantInstalled, INT hasPrevious)
{
    if (isWantInstalled)
        return (state == INSTALL_OURS) ? INSTALL_ACT_NOTHING : INSTALL_ACT_WRITE;
    /* [CAUTION]: UNINSTALLING SOMEBODY ELSE'S VALUE IS REFUSED, NOT SILENTLY DONE. If the
     * Debugger points at another program we never installed, deleting it would
     * break whatever that is and we would have no way to tell the user what we
     * removed. Absent is already the goal, so that is NOTHING, not an error.
     */
    if (state == INSTALL_ABSENT)
        return INSTALL_ACT_NOTHING;
    if (state == INSTALL_OTHER)
        return INSTALL_ACT_REFUSE;
    return hasPrevious ? INSTALL_ACT_RESTORE : INSTALL_ACT_DELETE;
}
