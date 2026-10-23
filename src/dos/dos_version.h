/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Which INT 21h services the configured DOS version has: the PURE half. (#28)
 *
 * The reported version is a setting (Settings > General, cfg\dosver.txt), and until
 * #28 it changed only what AH=30h and AX=3306h SAY. A program that asks for 3.31 and
 * then calls AH=6Ch was still served a DOS 4.0 function, which is a machine no one
 * ever had. This header says, for a version, whether a function exists, so the
 * dispatcher can answer the way that version's DOS answers a call it does not have.
 *
 * [INFO]: THE TABLE IS RALF BROWN'S INTERRUPT LIST, INT 21h, the "DOS x.y+" line of each
 * entry (the source docs/ref/SOURCES.md names for MS-DOS). It is a citation, not a
 * measurement -- no oracle here boots 3.31 or 4.01.
 *
 * [INFO]: WHAT AN ABSENT FUNCTION ANSWERS is the measured 6.22 dispatcher's answer, taken
 * as every version's: a function past the table returns AL=00h, CF clear, nothing
 * else touched (p_defs / p_subfn on the 6.22 oracle; dos_int21.c, DosIsDefinedBy622).
 * An AH=33h subfunction a version lacks answers AL=FFh, as 6.22 answers 3307h.
 * [CAUTION] Applying 6.22's dispatcher to older versions is an assumption.
 *
 * [CAUTION]: WHAT THIS DOES NOT GATE, ON PURPOSE:
 * - AH=71h, the long-filename API. It is not a DOS version's feature here but stock
 *   XP NTVDM's, which provides it whatever version it reports (#210), so every
 *   configured version keeps it.
 * - Versions before 2.0. DOS 1.x has no handle calls at all; nobody configures it, and
 *   the table below starts at the 3.x additions.
 * - The default, 6.22 (and anything newer), loses nothing: every row's version is at
 *   or below 5.00, which is the guarantee tests/unit/dosver_test.c pins.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_DOS_VERSION_H
#define NTVDMEX_DOS_VERSION_H

#include "../ntvdmex_types.h"

/* major.minor as one number: 3.31 -> 331, 6.22 -> 622, 7.10 -> 710. DOS's minor is
 * decimal hundredths, so this orders correctly.
 */
#define DOS_VERSION_NUMBER(major, minor)    ((UINT)(major) * 100u + (UINT)(minor))

/* What the dispatcher should do with a call. */
#define DOS_VERSION_HAS                     0       /* The version has it: serve it */
#define DOS_VERSION_LACKS_FUNCTION          1       /* AL=00h, CF clear, as 6.22 past its table */
#define DOS_VERSION_LACKS_SUBFUNCTION       2       /* AL=FFh, as 6.22 answers an unknown 33xxh */

#define DOS_VERSION_FN_BREAK_STATE          0x33    /* AH=33h: Ctrl-Break, boot drive, true version */
#define DOS_VERSION_FN_BOOT_DRIVE           0x05    /* AX=3305h */
#define DOS_VERSION_FN_TRUE_VERSION         0x06    /* AX=3306h */

typedef struct DOS_VERSION_ROW
{
    BYTE Function;
    WORD Since;     /* DOS_VERSION_NUMBER of the first version that has it */
} DOS_VERSION_ROW;

/* RBIL INT 21h, by function. */
static const DOS_VERSION_ROW g_DosVersionRows[] =
{
    { 0x59, 300 },  /* get extended error information */
    { 0x5A, 300 },  /* create temporary file */
    { 0x5B, 300 },  /* create new file */
    { 0x5C, 300 },  /* lock / unlock file region */
    { 0x5D, 310 },  /* server function call and friends */
    { 0x5E, 310 },  /* machine name, printer setup */
    { 0x5F, 310 },  /* redirection list */
    { 0x60, 300 },  /* truename */
    { 0x62, 300 },  /* get PSP */
    { 0x65, 330 },  /* get extended country information */
    { 0x66, 330 },  /* get / set global code page */
    { 0x67, 330 },  /* set handle count */
    { 0x68, 330 },  /* commit file */
    { 0x69, 400 },  /* get / set disk serial number */
    { 0x6A, 400 },  /* commit file (again) */
    { 0x6C, 400 },  /* extended open / create */
};

/* AH=33h subfunctions, RBIL INT 21h AX=3305h and AX=3306h. */
static const DOS_VERSION_ROW g_DosVersionBreakRows[] =
{
    { DOS_VERSION_FN_BOOT_DRIVE, 400 },
    { DOS_VERSION_FN_TRUE_VERSION, 500 },
};

/* What `major`.`minor` does with INT 21h AH=`function` (AL=`subfunction`). */
static inline INT DosVersionAnswer(
    _In_ BYTE major,
    _In_ BYTE minor,
    _In_ BYTE function,
    _In_ BYTE subfunction)
{
    UINT version = DOS_VERSION_NUMBER(major, minor);
    UINT index;

    if (function == DOS_VERSION_FN_BREAK_STATE)
    {
        for (index = 0; index < sizeof g_DosVersionBreakRows / sizeof g_DosVersionBreakRows[0]; ++index)
            if (g_DosVersionBreakRows[index].Function == subfunction && version < g_DosVersionBreakRows[index].Since)
                return DOS_VERSION_LACKS_SUBFUNCTION;

        return DOS_VERSION_HAS;
    }

    for (index = 0; index < sizeof g_DosVersionRows / sizeof g_DosVersionRows[0]; ++index)
        if (g_DosVersionRows[index].Function == function && version < g_DosVersionRows[index].Since)
            return DOS_VERSION_LACKS_FUNCTION;

    return DOS_VERSION_HAS;
}

#endif
