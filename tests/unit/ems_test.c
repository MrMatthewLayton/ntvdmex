/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM unit battery for the EMS core (src/dos/dos_ems.h).
 *
 * Layer-1 test for M4 slice 2, in the style of xms_test.c. A 64 KB buffer
 * stands in for the page-frame window (the host maps real 0xE0000 RAM there);
 * handle backing pages are malloc'd. The key thing under test is page-frame
 * *shadowing*: mapping a logical page memcpys it into a physical window, the
 * "guest" writes the window directly, and remapping must write those changes
 * back to the logical page so they survive -- this is how EMS works without a
 * memory trap. Also covers counts/alloc/dealloc, realloc grow/shrink, the
 * logical/physical range errors, and save/restore page maps.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dos_ems.h"

/* The pool the checks build, and the handles' sizes. */
#define EMS_TEST_FRAME_SEGMENT          0xE000
#define EMS_TEST_POOL_PAGES             256     /* 4 MB */
#define EMS_TEST_HANDLE_PAGES           8
#define EMS_TEST_OVERSIZED_PAGES        1000    /* More than the whole pool */
#define EMS_TEST_TOO_MANY_FREE_PAGES    250     /* Within the pool, beyond what is free */
#define EMS_TEST_GROWN_PAGES            16
#define EMS_TEST_SHRUNK_PAGES           4
#define EMS_TEST_UNKNOWN_HANDLE         99
#define EMS_TEST_UNUSED_HANDLE          200
#define EMS_TEST_FIRST_LISTED_PAGES     3
#define EMS_TEST_SECOND_LISTED_PAGES    5
#define EMS_TEST_HANDLE_NAME            "GAME"

/* The windows and logical pages the shadowing checks use. */
#define EMS_TEST_SECOND_WINDOW          1
#define EMS_TEST_THIRD_WINDOW           2
#define EMS_TEST_SECOND_WINDOW_PAGE     2
#define EMS_TEST_THIRD_WINDOW_PAGE      3
#define EMS_TEST_PERTURB_PAGE           4
#define EMS_TEST_PERTURB_PAGE_TOO       5

/* Where in a window the guest writes, and what. */
#define EMS_TEST_FIRST_BYTE             0x0000
#define EMS_TEST_LAST_BYTE              (DOS_EMS_PAGE_SIZE - 1)
#define EMS_TEST_MARK_OFFSET            0x10
#define EMS_TEST_DIRTY_OFFSET           0x20
#define EMS_TEST_PAGE0_FIRST_MARK       0xA1
#define EMS_TEST_PAGE0_LAST_MARK        0xA2
#define EMS_TEST_PAGE1_MARK             0xB1
#define EMS_TEST_SECOND_WINDOW_MARK     0xC3
#define EMS_TEST_THIRD_WINDOW_MARK      0xD4
#define EMS_TEST_DIRTY_MARK             0x5A

static INT g_Checks = 0;
static INT g_Failures = 0;

static VOID EmsTestCheck(BOOL passed, PCSTR description)
{
    g_Checks++;
    if (passed)
    {
        printf("  PASS  %s\n", description);
    }
    else
    {
        printf("  FAIL  %s\n", description);
        g_Failures++;
    }
}

static PVOID EmsTestAllocate(PVOID context, DWORD pages)
{
    (VOID)context;
    return calloc((SIZE_T)pages, DOS_EMS_PAGE_SIZE);
}

static VOID EmsTestFree(PVOID context, PVOID memory, DWORD pages)
{
    (VOID)context;
    (VOID)pages;
    free(memory);
}

/* a byte the guest would write straight into a physical window */
static VOID EmsTestPokeWindow(PDOS_EMS_STATE state, INT windowIndex, DWORD offset, BYTE value)
{
    state->Frame[(DWORD)windowIndex * DOS_EMS_PAGE_SIZE + offset] = value;
}

static BYTE EmsTestPeekWindow(PDOS_EMS_STATE state, INT windowIndex, DWORD offset)
{
    return state->Frame[(DWORD)windowIndex * DOS_EMS_PAGE_SIZE + offset];
}

/* The two words of a fn 4Dh entry. */
static WORD EmsTestEntryHandle(PCBYTE entry)
{
    return MAKEWORD(entry[DOS_EMS_ENTRY_HANDLE_LOW], entry[DOS_EMS_ENTRY_HANDLE_HIGH]);
}

static WORD EmsTestEntryPages(PCBYTE entry)
{
    return MAKEWORD(entry[DOS_EMS_ENTRY_PAGES_LOW], entry[DOS_EMS_ENTRY_PAGES_HIGH]);
}

INT main(VOID)
{
    static BYTE frame[DOS_EMS_FRAME_SIZE];   /* the 64 KB page-frame window */
    DOS_EMS_STATE state;
    WORD firstHandle;
    WORD secondHandle;
    WORD freePages;
    WORD totalPages;
    WORD pages;
    BYTE errorCode;
    BOOL succeeded;

    printf("== M4 EMS core battery ==\n");

    /* 256-page pool (4 MB) framed at E000:0. */
    memset(frame, 0, sizeof frame);
    DosEmsInitialize(&state, EMS_TEST_FRAME_SEGMENT, EMS_TEST_POOL_PAGES, frame,
                     EmsTestAllocate, EmsTestFree, NULL);

    /* T1: bring-up + counts ---------------------------------------------- */
    EmsTestCheck(state.FrameSegment == EMS_TEST_FRAME_SEGMENT, "init: page frame at E000");
    DosEmsGetPageCounts(&state, &freePages, &totalPages);
    EmsTestCheck(freePages == EMS_TEST_POOL_PAGES && totalPages == EMS_TEST_POOL_PAGES,
                 "fn42: 256 free / 256 total initially");

    /* T2: zero-page alloc is rejected (fn 43h) --------------------------- */
    EmsTestCheck(!DosEmsAllocatePages(&state, 0, &firstHandle, &errorCode)
                 && errorCode == DOS_EMS_ERROR_ZERO_PAGES, "fn43: zero pages rejected (89h)");

    /* T3: allocate 8 pages ----------------------------------------------- */
    succeeded = DosEmsAllocatePages(&state, EMS_TEST_HANDLE_PAGES, &firstHandle, &errorCode);
    EmsTestCheck(succeeded && errorCode == DOS_EMS_STATUS_OK, "fn43: allocate 8 pages -> handle");
    DosEmsGetPageCounts(&state, &freePages, &totalPages);
    EmsTestCheck(freePages == EMS_TEST_POOL_PAGES - EMS_TEST_HANDLE_PAGES,
                 "fn42: 248 free after 8-page alloc");
    succeeded = DosEmsGetHandlePages(&state, firstHandle, &pages, &errorCode);
    EmsTestCheck(succeeded && pages == EMS_TEST_HANDLE_PAGES, "fn4C: handle owns 8 pages");
    EmsTestCheck(DosEmsGetHandleCount(&state) == 1, "fn4B: 1 open handle");

    /* T4: over-pool alloc fails ------------------------------------------ */
    EmsTestCheck(!DosEmsAllocatePages(&state, EMS_TEST_OVERSIZED_PAGES, &secondHandle, &errorCode)
                 && errorCode == DOS_EMS_ERROR_TOO_MANY_PAGES, "fn43: > pool rejected (87h)");
    succeeded = DosEmsAllocatePages(&state, EMS_TEST_TOO_MANY_FREE_PAGES, &secondHandle,
                                    &errorCode);
    EmsTestCheck(!succeeded && errorCode == DOS_EMS_ERROR_NOT_ENOUGH_PAGES,
                 "fn43: not-enough-free rejected (88h)");

    /* T5: map errors ----------------------------------------------------- */
    EmsTestCheck(!DosEmsMapPage(&state, DOS_EMS_PHYSICAL_PAGES, 0, firstHandle, &errorCode)
                 && errorCode == DOS_EMS_ERROR_INVALID_PHYSICAL_PAGE,
                 "fn44: physical window >3 rejected (8Bh)");
    EmsTestCheck(!DosEmsMapPage(&state, 0, EMS_TEST_HANDLE_PAGES, firstHandle, &errorCode)
                 && errorCode == DOS_EMS_ERROR_INVALID_LOGICAL_PAGE,
                 "fn44: logical page >= owned rejected (8Ah)");
    EmsTestCheck(!DosEmsMapPage(&state, 0, 0, EMS_TEST_UNKNOWN_HANDLE, &errorCode)
                 && errorCode == DOS_EMS_ERROR_INVALID_HANDLE, "fn44: bad handle rejected (83h)");

    /* T6: SHADOWING -- map page 0 into window 0, write it, remap, verify it  *
     * was written back to the logical page (the heart of EMS).
     */
    succeeded = DosEmsMapPage(&state, 0, 0, firstHandle, &errorCode);
    EmsTestCheck(succeeded, "fn44: map logical 0 -> window 0");
    /* guest writes the window */
    EmsTestPokeWindow(&state, 0, EMS_TEST_FIRST_BYTE, EMS_TEST_PAGE0_FIRST_MARK);
    EmsTestPokeWindow(&state, 0, EMS_TEST_LAST_BYTE, EMS_TEST_PAGE0_LAST_MARK);
    succeeded = DosEmsMapPage(&state, 0, 1, firstHandle, &errorCode);  /* swap in logical page 1 */
    EmsTestCheck(succeeded, "fn44: map logical 1 -> window 0 (writes back page 0)");
    EmsTestPokeWindow(&state, 0, EMS_TEST_FIRST_BYTE, EMS_TEST_PAGE1_MARK);  /* mark page 1 */
    succeeded = DosEmsMapPage(&state, 0, 0, firstHandle, &errorCode);  /* bring page 0 back */
    EmsTestCheck(succeeded
                 && EmsTestPeekWindow(&state, 0, EMS_TEST_FIRST_BYTE) == EMS_TEST_PAGE0_FIRST_MARK
                 && EmsTestPeekWindow(&state, 0, EMS_TEST_LAST_BYTE) == EMS_TEST_PAGE0_LAST_MARK,
                 "fn44: page 0 content survived the swap (shadow write-back)");
    succeeded = DosEmsMapPage(&state, 0, 1, firstHandle, &errorCode);  /* page 1's mark survived */
    EmsTestCheck(succeeded
                 && EmsTestPeekWindow(&state, 0, EMS_TEST_FIRST_BYTE) == EMS_TEST_PAGE1_MARK,
                 "fn44: page 1 content also survived");

    /* T7: two pages live in two windows at once -------------------------- */
    DosEmsMapPage(&state, EMS_TEST_SECOND_WINDOW, EMS_TEST_SECOND_WINDOW_PAGE, firstHandle,
                  &errorCode);
    EmsTestPokeWindow(&state, EMS_TEST_SECOND_WINDOW, EMS_TEST_MARK_OFFSET,
                      EMS_TEST_SECOND_WINDOW_MARK);
    DosEmsMapPage(&state, EMS_TEST_THIRD_WINDOW, EMS_TEST_THIRD_WINDOW_PAGE, firstHandle,
                  &errorCode);
    EmsTestPokeWindow(&state, EMS_TEST_THIRD_WINDOW, EMS_TEST_MARK_OFFSET,
                      EMS_TEST_THIRD_WINDOW_MARK);
    EmsTestCheck(EmsTestPeekWindow(&state, EMS_TEST_SECOND_WINDOW, EMS_TEST_MARK_OFFSET)
                     == EMS_TEST_SECOND_WINDOW_MARK
                 && EmsTestPeekWindow(&state, EMS_TEST_THIRD_WINDOW, EMS_TEST_MARK_OFFSET)
                     == EMS_TEST_THIRD_WINDOW_MARK,
                 "fn44: independent windows hold independent pages");

    /* T8: unmap a window (logical 0xFFFF) writes back + clears ----------- */
    succeeded = DosEmsMapPage(&state, EMS_TEST_SECOND_WINDOW, DOS_EMS_UNMAP_LOGICAL_PAGE,
                              firstHandle, &errorCode);
    EmsTestCheck(succeeded && !state.PhysicalPages[EMS_TEST_SECOND_WINDOW].IsMapped,
                 "fn44: logical 0xFFFF unmaps the window");

    /* T9: save / restore the page map ------------------------------------ */
    succeeded = DosEmsSavePageMap(&state, firstHandle, &errorCode);
    EmsTestCheck(succeeded, "fn47: save page map");
    EmsTestCheck(!DosEmsSavePageMap(&state, firstHandle, &errorCode)
                 && errorCode == DOS_EMS_ERROR_MAP_ALREADY_SAVED,
                 "fn47: double-save rejected (8Dh)");
    /* perturb the mapping */
    DosEmsMapPage(&state, 0, EMS_TEST_PERTURB_PAGE, firstHandle, &errorCode);
    DosEmsMapPage(&state, EMS_TEST_THIRD_WINDOW, EMS_TEST_PERTURB_PAGE_TOO, firstHandle,
                  &errorCode);
    succeeded = DosEmsRestorePageMap(&state, firstHandle, &errorCode);
    EmsTestCheck(succeeded && state.PhysicalPages[0].LogicalPage == 1
                 && state.PhysicalPages[EMS_TEST_THIRD_WINDOW].LogicalPage
                        == EMS_TEST_THIRD_WINDOW_PAGE,
                 "fn48: restore brings back the saved windows");
    EmsTestCheck(!DosEmsRestorePageMap(&state, firstHandle, &errorCode)
                 && errorCode == DOS_EMS_ERROR_MAP_NOT_SAVED,
                 "fn48: restore-without-save rejected (8Eh)");

    /* T10: realloc grow preserves content -------------------------------- */
    {
        PBYTE memory;
        DosEmsMapPage(&state, 0, 0, firstHandle, &errorCode);
        /* dirty page 0 via the window */
        EmsTestPokeWindow(&state, 0, EMS_TEST_DIRTY_OFFSET, EMS_TEST_DIRTY_MARK);
        /* 8 -> 16 pages (flush+grow) */
        succeeded = DosEmsReallocatePages(&state, firstHandle, EMS_TEST_GROWN_PAGES, &errorCode);
        EmsTestCheck(succeeded && state.Handles[firstHandle].Pages == EMS_TEST_GROWN_PAGES,
                     "fn51: grow 8->16 pages");
        memory = (PBYTE)state.Handles[firstHandle].Memory;
        EmsTestCheck(memory[EMS_TEST_DIRTY_OFFSET] == EMS_TEST_DIRTY_MARK,
                     "fn51: grow preserved the written-back page 0");
        DosEmsGetPageCounts(&state, &freePages, &totalPages);
        EmsTestCheck(freePages == EMS_TEST_POOL_PAGES - EMS_TEST_GROWN_PAGES,
                     "fn42: 240 free after grow to 16");
        /* shrink to 4 pages */
        succeeded = DosEmsReallocatePages(&state, firstHandle, EMS_TEST_SHRUNK_PAGES, &errorCode);
        EmsTestCheck(succeeded && state.Handles[firstHandle].Pages == EMS_TEST_SHRUNK_PAGES,
                     "fn51: shrink 16->4 pages");
    }

    /* T11: free the handle, pool restored, windows dropped --------------- */
    DosEmsMapPage(&state, 0, 0, firstHandle, &errorCode);  /* a live window before free */
    succeeded = DosEmsDeallocatePages(&state, firstHandle, &errorCode);
    EmsTestCheck(succeeded && !state.PhysicalPages[0].IsMapped,
                 "fn45: dealloc drops the handle's live windows");
    DosEmsGetPageCounts(&state, &freePages, &totalPages);
    EmsTestCheck(freePages == EMS_TEST_POOL_PAGES, "fn42: whole pool free again after dealloc");
    EmsTestCheck(!DosEmsDeallocatePages(&state, firstHandle, &errorCode)
                 && errorCode == DOS_EMS_ERROR_INVALID_HANDLE, "fn45: double-free rejected (83h)");
    EmsTestCheck(DosEmsGetHandleCount(&state) == 0, "fn4B: 0 open handles at end");

    /* T12: fn 4Dh lists exactly the active handles, {handle, pages} each --- */
    {   WORD listedHandle, otherListedHandle;
        BYTE entries[DOS_EMS_MAX_HANDLES * DOS_EMS_HANDLE_PAGES_ENTRY_SIZE];
        PCBYTE secondEntry = entries + DOS_EMS_HANDLE_PAGES_ENTRY_SIZE;
        EmsTestCheck(DosEmsGetAllHandlePages(&state, entries) == 0, "fn4D: no handles -> count 0");
        DosEmsAllocatePages(&state, EMS_TEST_FIRST_LISTED_PAGES, &listedHandle, &errorCode);
        DosEmsAllocatePages(&state, EMS_TEST_SECOND_LISTED_PAGES, &otherListedHandle, &errorCode);
        EmsTestCheck(DosEmsGetAllHandlePages(&state, entries) == 2, "fn4D: two handles -> count 2");
        EmsTestCheck(EmsTestEntryHandle(entries) == listedHandle
                     && EmsTestEntryPages(entries) == EMS_TEST_FIRST_LISTED_PAGES,
                     "fn4D: first pair = {handle, 3 pages}");
        EmsTestCheck(EmsTestEntryHandle(secondEntry) == otherListedHandle
                     && EmsTestEntryPages(secondEntry) == EMS_TEST_SECOND_LISTED_PAGES,
                     "fn4D: second pair = {handle, 5 pages}");
        {   BYTE name[DOS_EMS_HANDLE_NAME_SIZE] = EMS_TEST_HANDLE_NAME;
            BYTE nameRead[DOS_EMS_HANDLE_NAME_SIZE];
            EmsTestCheck(DosEmsGetSetHandleName(&state, listedHandle, TRUE, name, &errorCode),
                         "fn53 AL=1: set name");
            EmsTestCheck(DosEmsGetSetHandleName(&state, listedHandle, FALSE, nameRead, &errorCode)
                         && memcmp(nameRead, name, DOS_EMS_HANDLE_NAME_SIZE) == 0,
                         "fn53 AL=0: reads back the name");
            EmsTestCheck(!DosEmsGetSetHandleName(&state, EMS_TEST_UNUSED_HANDLE, FALSE, nameRead,
                                                 &errorCode)
                         && errorCode == DOS_EMS_ERROR_INVALID_HANDLE,
                         "fn53: unused handle -> 83h (what MEM /D relies on)"); }
        DosEmsDeallocatePages(&state, listedHandle, &errorCode);
        DosEmsDeallocatePages(&state, otherListedHandle, &errorCode);
        {   WORD reusedHandle;
        BYTE nameRead[DOS_EMS_HANDLE_NAME_SIZE];
            DosEmsAllocatePages(&state, 1, &reusedHandle, &errorCode);
            EmsTestCheck(DosEmsGetSetHandleName(&state, reusedHandle, FALSE, nameRead, &errorCode)
                         && nameRead[0] == 0,
                         "fn53: a reused handle starts unnamed");
            DosEmsDeallocatePages(&state, reusedHandle, &errorCode); } }

    printf("\n%d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
