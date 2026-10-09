/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The DOS handle table's two rules, pinned off-VM.  GH #133, #131.
 *
 * Every expectation here is a line from tests/probes/dos/p_redir.asm run on the
 * genuine MS-DOS 6.22 oracle, quoted in the check's name.  None of it is written
 * from memory of what DOS does -- that is the cardinal rule of epic #24, and the
 * reason this file cites the CASE= line it came from.
 *
 *   cc -std=c99 -I src -I src/dos -o fh_test tests/unit/fh_test.c && ./fh_test
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "dos_fh.h"

/* The standard handles, by slot. */
#define FH_TEST_STDIN               0
#define FH_TEST_STDOUT              1
#define FH_TEST_STDERR              2
#define FH_TEST_ALL_STD_OPEN        0x1F    /* Bits 0-4: stdin..prn */
#define FH_TEST_FIRST_FREE          5       /* The slot past the five standard handles */
#define FH_TEST_SIXTH               6
#define FH_TEST_SEVENTH             7
#define FH_TEST_EIGHTH              8
#define FH_TEST_LAST_DEVICE_SLOT    31      /* The last slot the device mask can mark */
#define FH_TEST_PAST_DEVICE_MASK    32
#define FH_TEST_RAW_GUEST_BX        0xFFFF
#define FH_TEST_BOUND_VALUE         0xF11E  /* Any non-NULL stands for a Win32 handle */
#define FH_TEST_NONE                0
#define FH_TEST_MARK                TRUE

static INT g_Checks, g_Failures;

static VOID FhTestExpect(PCSTR description, LONG actual, LONG expected)
{
    ++g_Checks;
    if (actual == expected) return;
    ++g_Failures;
    printf("  FAIL %-52s got %ld, want %ld\n", description, (long)actual, (long)expected);
}

/* A table in the state dos_int21_init leaves it: nothing bound, all five
 * standard handles open as devices.
 */
static VOID FhTestStandardTable(PVOID fileHandles[DOS_MAX_FILES], PUINT deviceMask)
{
    memset(fileHandles, 0, sizeof(PVOID) * DOS_MAX_FILES);
    *deviceMask = FH_TEST_ALL_STD_OPEN;                       /* bits 0-4: stdin..prn */
}

/* Any non-NULL value stands for "bound to a Win32 handle". */
static PVOID const g_Bound = (PVOID)(SIZE_T)FH_TEST_BOUND_VALUE;

INT main(VOID)
{
    PVOID fileHandles[DOS_MAX_FILES];
    UINT deviceMask;
    UINT slot;

    printf("== DOS handle table (dos_fh.h) -- rules measured on MS-DOS 6.22\n");

    /* RULE 1: ALLOCATION IS THE LOWEST FREE SLOT:
     * Oracle: CASE=int21.3c.baseline SIG=AX,CF AX=0005 CF=0
     * With stdin/stdout/stderr/aux/prn open, a create gets 5.
     */
    FhTestStandardTable(fileHandles, &deviceMask);
    FhTestExpect("3Ch baseline, five devices open -> 5", DosHandleAllocate(fileHandles, deviceMask), FH_TEST_FIRST_FREE);

    /* Oracle: CASE=int21.3c.after.close1 SIG=AX,CF AX=0001 CF=0
     *
     * [INFO]: THE FACT #133 was missing. Close handle 1 and the next create lands in
     * the slot stdout just vacated -- which is the whole of `>`. A loop that
     * starts at 5 cannot produce this, and that is what made the text go to the
     * screen while the file stayed 0 bytes.
     */
    FhTestStandardTable(fileHandles, &deviceMask);
    deviceMask &= ~(1u << FH_TEST_STDOUT);                 /* the shell closed stdout */
    FhTestExpect("3Ch after close(1) -> 1  [the redirect]", DosHandleAllocate(fileHandles, deviceMask), FH_TEST_STDOUT);

    /* Input redirection is the same rule on the other end. */
    FhTestStandardTable(fileHandles, &deviceMask);
    deviceMask &= ~(1u << FH_TEST_STDIN);
    FhTestExpect("3Dh after close(0) -> 0  [`< file`]", DosHandleAllocate(fileHandles, deviceMask), FH_TEST_STDIN);

    /* A closed-and-refilled slot is not free any more: the SECOND create must
     * move on to 5 rather than handing out handle 1 twice. Getting this wrong
     * would alias the redirect target and whatever the command opens next.
     */
    FhTestStandardTable(fileHandles, &deviceMask);
    deviceMask &= ~(1u << FH_TEST_STDOUT);
    fileHandles[FH_TEST_STDOUT] = g_Bound;                          /* the target took slot 1 */
    FhTestExpect("second create with 1 taken -> 5", DosHandleAllocate(fileHandles, deviceMask), FH_TEST_FIRST_FREE);

    /* Ordinary allocation above the standard handles still packs downwards into
     * any hole a close left, which is what DOS does and what programs that
     * count on handle numbers rely on.
     */
    FhTestStandardTable(fileHandles, &deviceMask);
    fileHandles[FH_TEST_FIRST_FREE] = g_Bound; fileHandles[FH_TEST_SIXTH] = g_Bound; fileHandles[FH_TEST_SEVENTH] = g_Bound;
    FhTestExpect("5,6,7 bound -> 8", DosHandleAllocate(fileHandles, deviceMask), FH_TEST_EIGHTH);
    fileHandles[FH_TEST_SIXTH] = NULL;                              /* close the middle one */
    FhTestExpect("...then close 6 -> 6 (fills the hole)", DosHandleAllocate(fileHandles, deviceMask), FH_TEST_SIXTH);

    /* Full table reports DOS_MAX_FILES so the caller can raise error 4 rather
     * than index off the end.
     */
    FhTestStandardTable(fileHandles, &deviceMask);
    deviceMask = FH_TEST_NONE;
    for (slot = 0; slot < DOS_MAX_FILES; ++slot) fileHandles[slot] = g_Bound;
    FhTestExpect("full table -> DOS_MAX_FILES", DosHandleAllocate(fileHandles, deviceMask), DOS_MAX_FILES);

    /* RULE 2: A BOUND HANDLE IS A FILE, WHATEVER ITS NUMBER:
     * Oracle: CASE=int21.42.end.on.h1 SIG=AX,DX,CF AX=0004 DX=0000 CF=0
     * Real DOS seeks handle 1 to end-of-file and reports 4 bytes. AH=42h used
     * to guard with `h >= 5` and refused this with error 6 -- the last
     * survivor of #133, and the reason `>>` could not find end-of-file.
     */
    FhTestStandardTable(fileHandles, &deviceMask);
    deviceMask &= ~(1u << FH_TEST_STDOUT);
    fileHandles[FH_TEST_STDOUT] = g_Bound;
    FhTestExpect("bound handle 1 IS a file (42h/40h/3Fh/3Eh)", DosHandleIsFile(fileHandles, FH_TEST_STDOUT), TRUE);
    FhTestExpect("bound handle 1 is NOT a device", DosHandleIsDevice(fileHandles, deviceMask, FH_TEST_STDOUT), FALSE);

    /* ...and the ordinary case is unchanged: an UNBOUND low handle is the
     * console, which is what makes a non-redirected program print.
     */
    FhTestStandardTable(fileHandles, &deviceMask);
    FhTestExpect("unbound handle 1 is a device", DosHandleIsDevice(fileHandles, deviceMask, FH_TEST_STDOUT), TRUE);
    FhTestExpect("unbound handle 1 is NOT a file", DosHandleIsFile(fileHandles, FH_TEST_STDOUT), FALSE);
    FhTestExpect("unbound handle 2 (stderr) is a device", DosHandleIsDevice(fileHandles, deviceMask, FH_TEST_STDERR), TRUE);

    /* A closed standard handle is neither: not a device any more, not yet a
     * file. It is simply free, which is what makes rule 1 hand it out.
     */
    FhTestStandardTable(fileHandles, &deviceMask);
    deviceMask &= ~(1u << FH_TEST_STDOUT);
    FhTestExpect("closed handle 1 is not a device", DosHandleIsDevice(fileHandles, deviceMask, FH_TEST_STDOUT), FALSE);
    FhTestExpect("closed handle 1 is not a file", DosHandleIsFile(fileHandles, FH_TEST_STDOUT), FALSE);

    /* RULE 3: A DEVICE IS DUPLICABLE, AND NOT ONLY INTO SLOTS 0-4:
     * Oracle: CASE=int21.45.dup.stdout SIG=CF AX=0005 CF=0
     * Real DOS duplicates the console into slot 5, which is then ALSO the
     * console. NTVDMEX returned AX=0006 CF=1 on the rig because a device could
     * only be represented below slot 5 -- so `dup(1)`, the first step of every
     * save-redirect-restore a shell performs, was impossible.
     *
     * [INFO]: The wrong answer ATE ITS OWN EVIDENCE: with stdout never restored, the
     * probe's remaining output went into the file under test instead of the
     * dump. Hence the rule gets its own checks here.
     */
    FhTestStandardTable(fileHandles, &deviceMask);
    FhTestExpect("dup(1): the copy lands in slot 5", DosHandleAllocate(fileHandles, deviceMask), FH_TEST_FIRST_FREE);
    FhTestExpect("...and slot 5 can be marked a device", DosHandleSetDevice(&deviceMask, FH_TEST_FIRST_FREE, FH_TEST_MARK), TRUE);
    FhTestExpect("...so slot 5 IS a device", DosHandleIsDevice(fileHandles, deviceMask, FH_TEST_FIRST_FREE), TRUE);
    FhTestExpect("...and is NOT a file (nothing to WriteFile to)", DosHandleIsFile(fileHandles, FH_TEST_FIRST_FREE), FALSE);
    FhTestExpect("...and is not handed out again", DosHandleAllocate(fileHandles, deviceMask), FH_TEST_SIXTH);

    /* The restore: dup2(saved, 1) puts the device back on handle 1. */
    deviceMask &= ~(1u << FH_TEST_STDOUT);                 /* the shell had closed stdout */
    FhTestExpect("before restore, 1 is free", DosHandleAllocate(fileHandles, deviceMask), FH_TEST_STDOUT);
    DosHandleSetDevice(&deviceMask, FH_TEST_STDOUT, FH_TEST_MARK);     /* dup2(5, 1) */
    FhTestExpect("after dup2(5,1), handle 1 is the console again",
                 DosHandleIsDevice(fileHandles, deviceMask, FH_TEST_STDOUT), TRUE);

    /* Past the mask the answer must be REFUSED, not rounded: a device slot that
     * silently read as a file is the "runs but lies" class.
     */
    FhTestStandardTable(fileHandles, &deviceMask);
    FhTestExpect("device mark at slot 31 succeeds", DosHandleSetDevice(&deviceMask, FH_TEST_LAST_DEVICE_SLOT, FH_TEST_MARK), TRUE);
    FhTestExpect("device mark at slot 32 is REFUSED", DosHandleSetDevice(&deviceMask, FH_TEST_PAST_DEVICE_MASK, FH_TEST_MARK), FALSE);
    FhTestExpect("...and slot 32 is therefore not a device", DosHandleIsDevice(fileHandles, deviceMask, FH_TEST_PAST_DEVICE_MASK), FALSE);

    /* Bounds: a guest can put anything in BX, so both predicates take a raw
     * value and must not index off the end.
     */
    FhTestStandardTable(fileHandles, &deviceMask);
    FhTestExpect("h = DOS_MAX_FILES is not a file", DosHandleIsFile(fileHandles, DOS_MAX_FILES), FALSE);
    FhTestExpect("h = 0xFFFF is not a file", DosHandleIsFile(fileHandles, FH_TEST_RAW_GUEST_BX), FALSE);
    FhTestExpect("h = 0xFFFF is not a device", DosHandleIsDevice(fileHandles, deviceMask, FH_TEST_RAW_GUEST_BX), FALSE);

    printf("== %d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
