/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * DOS error codes: AH=59h's class/action/locus, and Win32 errors as DOS ones.
 *
 * The function definitions of dos_err.h, which keeps their declarations and doc comments (#335).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "dos_err.h"

BOOL DosErrClassify(_In_ WORD errorCode, _Out_ PWORD classAndAction, _Out_ PBYTE locus)
{
    UINT rowIndex;

    *classAndAction = 0;
    *locus = 0;

    if (!errorCode)
        return TRUE;                            /* no error: zeroes are correct */

    for (rowIndex = 0; rowIndex < DOS_ERR_ROWS; ++rowIndex)
    {
        if (g_DosErrTable[rowIndex].Code != errorCode)
            continue;

        *classAndAction = g_DosErrTable[rowIndex].ClassAndAction;
        *locus = g_DosErrTable[rowIndex].Locus;
        return TRUE;
    }

    return FALSE;                               /* caller logs UNMEASURED */
}

BOOL DosErrFromWin32(_In_ DWORD win32Error, _Out_ PWORD dosError)
{
    switch (win32Error)
    {
    /* MEASURED, both sides. See the log lines quoted above: */
    case DOS_ERR_WIN32_FILE_NOT_FOUND:                       /* err.after.3D.missing  AX=0002 */
        *dosError = DOS_ERR_FILE_NOT_FOUND;
        return TRUE;

    /* Also the bad-drive door: "Y:\..." arrives here as 3, not 15. */
    case DOS_ERR_WIN32_PATH_NOT_FOUND:                       /* err.after.3D.baddrive AX=0003 */
        *dosError = DOS_ERR_PATH_NOT_FOUND;
        return TRUE;

    case DOS_ERR_WIN32_ACCESS_DENIED:                        /* err.after.3D.readonly AX=0005 */
        *dosError = DOS_ERR_ACCESS_DENIED;
        return TRUE;

    /* AN IDENTITY, NOT A MEASUREMENT, AND LABELLED AS SUCH. DOS error 4 IS
     * "too many open files" and the handler already answers 4 when it runs out
     * of its own slots, so the two names denote one condition. NOT provoked by
     * a probe: to promote it, extend p_err.asm to exhaust the handle table.
     */
    case DOS_ERR_WIN32_TOO_MANY_OPEN:
        *dosError = DOS_ERR_TOO_MANY_OPEN_FILES;
        return TRUE;

    /* #168: p_file int21.6C.exists -- 6Ch "fail if it exists" on a file that does:
     * 6.22 answers AX=0050. CREATE_NEW reports ERROR_FILE_EXISTS; CreateDirectory
     * and MoveFile say ERROR_ALREADY_EXISTS for the same condition.
     */
    case DOS_ERR_WIN32_FILE_EXISTS:
    case DOS_ERR_WIN32_ALREADY_EXISTS:
        *dosError = DOS_ERR_FILE_EXISTS;
        return TRUE;

    /* #34: THE HARDWARE ERRORS, 19-31, ARE THE SAME NUMBERS ON BOTH SIDES. Win32
     * kept DOS's codes for them (ERROR_WRITE_PROTECT 19 .. ERROR_GEN_FAILURE 31;
     * an empty floppy drive is ERROR_NOT_READY, 21). An identity, and labelled as
     * one -- and the code that matters: 19-31 is what turns a failure into a
     * CRITICAL error that goes to the program's INT 24h (DosCrit*, below).
     */
    case DOS_ERR_WRITE_PROTECT:
    case DOS_ERR_BAD_UNIT:
    case DOS_ERR_NOT_READY:
    case DOS_ERR_BAD_COMMAND:
    case DOS_ERR_CRC:
    case DOS_ERR_BAD_LENGTH:
    case DOS_ERR_SEEK:
    case DOS_ERR_NOT_DOS_DISK:
    case DOS_ERR_SECTOR_NOT_FOUND:
    case DOS_ERR_OUT_OF_PAPER:
    case DOS_ERR_WRITE_FAULT:
    case DOS_ERR_READ_FAULT:
    case DOS_ERR_GEN_FAILURE:
        *dosError = (WORD)win32Error;
        return TRUE;

    default:                                         /* caller logs win32= and keeps 2 */
        *dosError = DOS_ERR_FILE_NOT_FOUND;
        return FALSE;
    }
}
