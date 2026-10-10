/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Tiny no-CRT logging helpers for the host.
 *
 * LogPut/LogHex/LogDump build text into a caller-owned buffer; LogWrite/LogAppend flush
 * it to a file. Header-only static-inline (no shared state); the writers take the
 * log path explicitly rather than hard-coding it (the spike hard-coded
 * C:\ntvdmex\vdmhost.log). Ported from the first spike (vdmhost). No CRT -- only
 * kernel32 file APIs, so it loads on XP.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef HOST_LOG_H
#define HOST_LOG_H

#include <windows.h>

#include "../ntvdmex_bits.h"    /* defines only: NIBBLE_SHIFT/MASK */
#include "../ntvdmex_ascii.h"   /* defines only: DECIMAL_RADIX */

/* Append the ASCIIZ string s to p; return the new end (NUL-terminated). */
/* The digits these helpers write. */
#define LOG_HEX_DIGITS          8
#define LOG_HEX_BYTE_DIGITS     2
#define LOG_HEX_TOP_SHIFT       28      /* The first of a DWORD's eight digits */
#define LOG_DECIMAL_DIGITS      10
#define LOG_DUMP_ROW_MASK       0xF     /* LogDump: sixteen bytes a line */

/* THE LOG'S PATH LIVES WITH THE LOG. (session 56):
 * It used to be defined in main.c AFTER the headers that want it, so anything
 * included earlier -- wow32.h, for one -- could call LogAppend but had no
 * name for the file to pass it. Two copies of a path is how one of them goes
 * stale; one definition, beside the function that opens it.
 */
#ifndef LOG_PATH
#define LOG_PATH    "C:\\ntvdmex\\ntvdmhost.log"
#endif

#define LOG_MAX_BYTES (256u * 1024u * 1024u)  /* 4 MB -> 32 MB -> 256 MB. A client that RUNS
                                                 produces a long trace, and truncating it hides
                                                 exactly the part that matters. Measured: with
                                                 pmverbose.flag, Doom past the D/B fix fills 32 MB
                                                 BEFORE it dies, so the cap looked like the
                                                 stopping point -- an instrument lying by
                                                 omission. On a silent VDM teardown nothing runs
                                                 afterwards, so anything not already flushed is
                                                 gone: the ceiling has to clear the whole run. */

extern INT      g_LogIsQuiet;                           /* see log.c: what quiet stops */
extern LONGLONG g_LogQpc;                               /* time spent in LogAppend (QPC) */
/* LogAppend calls and bytes */
extern DWORD g_LogCalls;
extern DWORD g_LogBytes;

/* The log's writers and the state other files read; all defined in log.c. */
VOID LogWrite(PCSTR path, PCSTR buffer, PCSTR end);     /* truncate: a run's first line */
VOID LogAppend(PCSTR path, PCSTR buffer, PCSTR end);    /* append, under the size cap */

static inline PSTR LogPut(PSTR cursor, PCSTR text)
{
    while (*text)
        *cursor++ = *text++;

    *cursor = 0;
    return cursor;
}

/* Append v as 8 lowercase hex digits. */
static inline PSTR LogHex(PSTR cursor, UINT value)
{
    INT index;
    CHAR digits[LOG_HEX_DIGITS + 1];

    digits[LOG_HEX_DIGITS] = 0;

    for (index = LOG_HEX_DIGITS - 1; index >= 0; --index)
    {
        digits[index] = HEX_DIGITS_LOWER[value & NIBBLE_MASK];
        value >>= NIBBLE_SHIFT;
    }

    return LogPut(cursor, digits);
}

/* Append v as 2 lowercase hex digits. For byte-sized things -- interrupt numbers,
 * AH values, mode numbers -- where LogHex's 8 digits turn a list into a wall.
 */
static inline PSTR LogHexByte(PSTR cursor, UINT value)
{
    CHAR digits[LOG_HEX_BYTE_DIGITS + 1];

    digits[0] = HEX_DIGITS_LOWER[(value >> NIBBLE_SHIFT) & NIBBLE_MASK];
    digits[1] = HEX_DIGITS_LOWER[value & NIBBLE_MASK];
    digits[LOG_HEX_BYTE_DIGITS] = 0;
    return LogPut(cursor, digits);
}

/* Append a raw hex dump of n bytes at b (space-separated, newline every 16). */
/* Plain decimal. Everything else here is hex because it is describing machine state,
 * where hex is the readable form -- but a SCREEN RESOLUTION is not machine state, and
 * "0xa00 x 0x640" is not a thing anyone can check against their display settings.
 */
static inline PSTR LogDecimal(PSTR cursor, UINT value)
{
    CHAR digits[LOG_DECIMAL_DIGITS + 1];
    INT count = 0;

    if (!value)
    {
        *cursor++ = '0';
        *cursor = 0;
        return cursor;
    }

    while (value && count < LOG_DECIMAL_DIGITS)
    {
        digits[count++] = (CHAR)('0' + value % DECIMAL_RADIX);
        value /= DECIMAL_RADIX;
    }

    while (count) *cursor++ = digits[--count];

    *cursor = 0;
    return cursor;
}

static inline PSTR LogDump(PSTR cursor, LPCVOID bytes, UINT length)
{
    const BYTE *source = (const BYTE *)bytes;
    UINT index;

    for (index = 0; index < length; ++index)
    {
        *cursor++ = HEX_DIGITS_LOWER[source[index] >> NIBBLE_SHIFT];
        *cursor++ = HEX_DIGITS_LOWER[source[index] & NIBBLE_MASK];
        *cursor++ = ((index & LOG_DUMP_ROW_MASK) == LOG_DUMP_ROW_MASK) ? '\n' : ' ';
    }

    *cursor = 0;
    return cursor;
}

#endif /* HOST_LOG_H */
