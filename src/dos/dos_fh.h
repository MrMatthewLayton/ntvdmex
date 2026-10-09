/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The DOS file-handle table's TWO RULES, in one place.
 *
 * WHY THIS FILE EXISTS:
 * DOS pre-opens stdin/stdout/stderr/aux/prn as ordinary slots in the same handle
 * table as everything else.  That is not a detail; it is the entire mechanism by
 * which `>` works, and it produces exactly two rules:
 *
 * 1. ALLOCATION: a new handle is the LOWEST FREE SLOT.  So when a shell closes
 *    handle 1 and creates the redirect target, the target BECOMES handle 1 and
 *    every subsequent write to stdout is already a write to the file.
 * 2. CLASSIFICATION: a BOUND handle is a file, whatever its number.  Only an
 *    UNBOUND low handle is a console device.
 *
 * Both rules are MEASURED, not remembered -- tests/probes/dos/p_redir.asm run on the
 * MS-DOS 6.22 oracle (GH #133):
 *
 *   CASE=int21.3c.baseline     AX=0005    five standard handles open -> 5
 *   CASE=int21.3c.after.close1 AX=0001    handle 1 closed first      -> 1
 *   CASE=int21.42.end.on.h1    AX=0004 DX=0000 CF=0   lseek works on handle 1
 *   BUF=redir.contents 41424344           AH=02h AND AH=40h both reached it
 *
 * WHY IT IS A HEADER AND NOT FOUR COPIES:
 * It WAS four copies.  AH=3Ch/3Dh learned rule 1 when #133 was chased; AH=0Fh/16h
 * (FCB), AH=5Bh (create-new) and AH=6Ch (extended open) each kept their own
 * `for (slot = 5; ...)` loop and so could never hand back a redirected handle,
 * and AH=42h (lseek) kept its own `h >= 5` guard and so refused to seek on one --
 * which is precisely how `>>` finds end-of-file.  One rule, five spellings, four
 * of them wrong: the same shape as the duplicate *_ARG_* macros the WOW battery
 * now fails on (src/wow/wowconv.h).
 *
 * Kept pure -- no Windows calls, no dos_machine_t, only Windows types
 * (src/ntvdmex_types.h) -- so the off-VM battery can test it directly
 * (tests/unit/fh_test.c).  HANDLE is a PVOID, so the table is passed as PVOID const *.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_DOS_FH_H
#define NTVDMEX_DOS_FH_H

#include "../ntvdmex_types.h"
#include "dos_layout.h"     /* DOS_MAX_FILES */

/* The five slots DOS pre-opens: 0 stdin, 1 stdout, 2 stderr, 3 aux, 4 prn. */

/* A DEVICE HANDLE IS NOT CONFINED TO SLOTS 0-4:
 * It was, and that made the classic redirection idiom impossible:
 *
 *   AH=45h dup(1)  -> save the console          <- FAILED, error 6
 *   AH=3Eh close(1)
 *   AH=3Ch create  -> the target becomes handle 1
 *   ...run the command...
 *   AH=46h dup2(saved, 1)                       <- so this could not work
 *
 * Measured on the 6.22 oracle (tests/probes/dos/p_redir.asm):
 *   CASE=int21.45.dup.stdout SIG=CF AX=0005 CF=0
 * i.e. real DOS duplicates stdout into slot 5, which is then ALSO the console.
 * Ours returned AX=0006 CF=1 on the rig, because a device was only ever
 * representable in a slot below 5.
 *
 * So the "is a device" bit spans the low DOS_DEV_SLOTS entries, not five. 32
 * is a whole word and far past any real program's nesting; a duplicate that
 * would land beyond it is refused LOUDLY (DOS error 4) rather than silently
 * demoted to a file handle, which is the "runs but lies" class.
 */
#define DOS_DEV_SLOTS   32

/* Is slot `handle` bound to a real file?  A bound handle is a file WHATEVER ITS
 * NUMBER: that is rule 2, and it is what lets a redirected handle 1 read, write,
 * seek and close like the file it is.  Guards the caller's bounds too, so a
 * caller can hand this a raw guest BX.
 */
static BOOL DosHandleIsFile(_In_reads_(DOS_MAX_FILES) PVOID const *fileHandles, _In_ UINT handle)
{
    return handle < DOS_MAX_FILES && fileHandles[handle] != 0;
}

/* Is slot `handle` an unbound standard handle -- i.e. a console/AUX/PRN device?
 * This is the ONLY case that should reach the console sink.
 *
 * [CAUTION]: `deviceMask` is spelled `unsigned int`, not UINT, on purpose (#333): with the typedef,
 * GCC 14 -O3 allocates registers differently across dos_int21(), and the style pass is
 * proven by an unchanged binary. Same type, different spelling -- do not "tidy" it.
 */
static BOOL DosHandleIsDevice(_In_reads_(DOS_MAX_FILES) PVOID const *fileHandles,
                              _In_ unsigned int deviceMask, _In_ UINT handle)
{
    return handle < DOS_DEV_SLOTS && fileHandles[handle] == 0 && (deviceMask & (1u << handle)) != 0;
}

/* Mark slot `handle` as a character device, or clear it. Returns 0 when the slot is
 * past the mask -- see DOS_DEV_SLOTS for why that is refused and not rounded.
 */
static BOOL DosHandleSetDevice(_Inout_ PUINT deviceMask, _In_ UINT handle, _In_ BOOL isDevice)
{
    if (handle >= DOS_DEV_SLOTS) return FALSE;
    if (isDevice) *deviceMask |= (1u << handle);
    else          *deviceMask &= ~(1u << handle);
    return TRUE;
}

/* Rule 1: the lowest free slot, skipping standard slots that are still open as
 * devices.  Returns DOS_MAX_FILES when the table is full (caller reports DOS
 * error 4, "too many open files").
 *
 * [CAUTION]: It MUST start at 0, not at 5.  Starting at 5 is the defect this header was
 * extracted to delete: the redirect target then lands in slot 5, handle 1 is
 * still the console, and the text goes to the screen while the file stays 0
 * bytes -- measured, three times, in GH #133.
 */
static UINT DosHandleAllocate(_In_reads_(DOS_MAX_FILES) PVOID const *fileHandles, _In_ UINT deviceMask)
{
    UINT slot;
    for (slot = 0; slot < DOS_MAX_FILES; ++slot)
    {
        if (fileHandles[slot]) continue;                                   /* bound */
        if (DosHandleIsDevice(fileHandles, deviceMask, slot)) continue;    /* device */
        break;
    }
    return slot;
}

#endif /* NTVDMEX_DOS_FH_H */
