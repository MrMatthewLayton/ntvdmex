/* dos_ems.h -- LIM EMS 4.0 (Expanded Memory) core, host-testable.
 *
 * The second memory-extension layer for M4. EMS predates XMS and is reached via
 * INT 67h (the EMM driver, "EMMXXXX0"). Where XMS hands a real-mode program a
 * Move API, EMS gives it a 64 KB *page frame* in the upper-memory area split
 * into four 16 KB physical windows; the program maps any of its 16 KB logical
 * pages (drawn from a large expanded-memory pool) into those windows and then
 * reads/writes them directly through the frame.
 *
 * How this works under V86 without trapping (page-frame shadowing): expanded
 * memory lives in HOST buffers (one per handle, logical_pages * 16 KB). Mapping
 * logical page L into physical window P is a memcpy: first WRITE BACK whatever
 * 16 KB currently occupies window P to its backing logical page (the guest may
 * have written it directly), then READ IN page L. Between maps the guest's
 * direct accesses to the frame are just RAM accesses -- no fault, no trap. The
 * backing buffer is authoritative for every logical page EXCEPT the (<=4)
 * currently resident in a window, which is exactly the EMS access model.
 *
 * Same discipline as dos_mcb.h / dos_xms.h: no Windows calls, only Windows types
 * (src/ntvdmex_types.h), no globals; backing store via allocate/free hooks; the
 * page-frame window is a caller supplied pointer (host: the mapped 0xE0000 RAM;
 * tests: a 64 KB buffer).
 * Verified off-VM by tests/unit/ems_test.c.
 */
#ifndef NTVDMEX_DOS_EMS_H
#define NTVDMEX_DOS_EMS_H

#include "../ntvdmex_types.h"

#define DOS_EMS_PAGE_SIZE       0x4000u   /* 16 KB logical/physical page                */
#define DOS_EMS_PHYSICAL_PAGES  4         /* the page frame is four 16 KB windows        */
#define DOS_EMS_FRAME_SIZE      (DOS_EMS_PHYSICAL_PAGES * DOS_EMS_PAGE_SIZE)  /* 64 KB   */

/* EMS functions (AH), as main.c's dispatcher comments name them. */
#define DOS_EMS_FN_GET_STATUS           0x40
#define DOS_EMS_FN_GET_PAGE_FRAME       0x41
#define DOS_EMS_FN_GET_PAGE_COUNTS      0x42
#define DOS_EMS_FN_ALLOCATE             0x43
#define DOS_EMS_FN_MAP                  0x44
#define DOS_EMS_FN_DEALLOCATE           0x45
#define DOS_EMS_FN_GET_VERSION          0x46
#define DOS_EMS_FN_SAVE_PAGE_MAP        0x47
#define DOS_EMS_FN_RESTORE_PAGE_MAP     0x48
#define DOS_EMS_FN_GET_HANDLE_COUNT     0x4B
#define DOS_EMS_FN_GET_HANDLE_PAGES     0x4C
#define DOS_EMS_FN_GET_ALL_HANDLE_PAGES 0x4D
#define DOS_EMS_FN_REALLOCATE           0x51
#define DOS_EMS_FN_HANDLE_NAME          0x53
#define DOS_EMS_MAX_HANDLES     64
#define DOS_EMS_VERSION         0x40      /* LIM EMS 4.0, BCD in AL                       */

/* EMM status codes (returned in AH). */
#define DOS_EMS_STATUS_OK                    0x00
#define DOS_EMS_ERROR_INTERNAL               0x80  /* internal driver error                 */
#define DOS_EMS_ERROR_HARDWARE               0x81  /* hardware malfunction                  */
#define DOS_EMS_ERROR_INVALID_HANDLE         0x83  /* invalid handle                        */
#define DOS_EMS_ERROR_UNDEFINED_FUNCTION     0x84  /* undefined function requested          */
#define DOS_EMS_ERROR_NO_HANDLES             0x85  /* no more handles available             */
#define DOS_EMS_ERROR_INVALID_SUBFUNCTION    0x8F  /* LIM: invalid subfunction              */
#define DOS_EMS_HANDLE_NAME_SET              1     /* AH=53h AL: 0 get, 1 set                */
#define DOS_EMS_HANDLE_PAGES_ENTRY           4     /* AH=4Dh: a handle WORD, a page-count WORD */
#define DOS_EMS_ERROR_SAVE_RESTORE           0x86  /* page-map save/restore error           */
#define DOS_EMS_ERROR_TOO_MANY_PAGES         0x87  /* more pages requested than physically exist */
#define DOS_EMS_ERROR_NOT_ENOUGH_PAGES       0x88  /* not enough free pages to satisfy request */
#define DOS_EMS_ERROR_ZERO_PAGES             0x89  /* zero pages requested (alloc)          */
#define DOS_EMS_ERROR_INVALID_LOGICAL_PAGE   0x8A  /* logical page out of range for the handle */
#define DOS_EMS_ERROR_INVALID_PHYSICAL_PAGE  0x8B  /* illegal physical-page (window) number */
#define DOS_EMS_ERROR_MAP_ALREADY_SAVED      0x8D  /* page map already saved for this handle */
#define DOS_EMS_ERROR_MAP_NOT_SAVED          0x8E  /* no saved page map for this handle     */

/* fn 44h: a logical page of 0xFFFF unmaps the window (LIM 4.0). */
#define DOS_EMS_UNMAP_LOGICAL_PAGE  0xFFFF

/* fn 53h: a handle's name is 8 bytes; all zero = unnamed (LIM 4.0). */
#define DOS_EMS_HANDLE_NAME_SIZE    8

/* fn 4Dh: each entry is a {handle, pages} word pair, 4 bytes, each word low byte first. */
#define DOS_EMS_HANDLE_PAGES_ENTRY_SIZE  4
#define DOS_EMS_ENTRY_HANDLE_LOW         0
#define DOS_EMS_ENTRY_HANDLE_HIGH        1
#define DOS_EMS_ENTRY_PAGES_LOW          2
#define DOS_EMS_ENTRY_PAGES_HIGH         3

/* What one physical window holds: which handle's which logical page, if any. */
typedef struct _DOS_EMS_WINDOW_MAPPING {
    WORD     Handle;
    WORD     LogicalPage;
    BYTE     IsMapped;
} DOS_EMS_WINDOW_MAPPING, *PDOS_EMS_WINDOW_MAPPING;

typedef const DOS_EMS_WINDOW_MAPPING *PCDOS_EMS_WINDOW_MAPPING;

typedef struct _DOS_EMS_HANDLE {
    BYTE     InUse;
    WORD     Pages;        /* logical 16 KB pages this handle owns (0 is legal)    */
    PVOID    Memory;       /* host buffer, pages * 16 KB                           */
    /* EMS 4.0 page-map save/restore (fn 47h/48h): one snapshot of the 4 windows. */
    BYTE     IsMapSaved;
    DOS_EMS_WINDOW_MAPPING SavedMap[DOS_EMS_PHYSICAL_PAGES];
    BYTE     Name[DOS_EMS_HANDLE_NAME_SIZE];  /* fn 53h handle name; all zero = unnamed (LIM 4.0) */
} DOS_EMS_HANDLE, *PDOS_EMS_HANDLE;

typedef const DOS_EMS_HANDLE *PCDOS_EMS_HANDLE;

/* Backing-store hooks: Allocate returns a buffer of pages*16KB (or 0); Free releases it. */
typedef PVOID (*PDOS_EMS_ALLOCATE_ROUTINE)(PVOID context, DWORD pages);
typedef VOID  (*PDOS_EMS_FREE_ROUTINE)(PVOID context, PVOID memory, DWORD pages);

typedef struct _DOS_EMS_STATE {
    WORD     FrameSegment; /* page-frame segment (e.g. 0xE000)                     */
    WORD     TotalPages;   /* size of the expanded-memory pool, in 16 KB pages     */
    WORD     UsedPages;    /* pages committed across all handles                   */
    /* current contents of the four physical windows */
    DOS_EMS_WINDOW_MAPPING PhysicalPages[DOS_EMS_PHYSICAL_PAGES];
    volatile BYTE *Frame;             /* the 64 KB page-frame window (RAM)         */
    DOS_EMS_HANDLE Handles[DOS_EMS_MAX_HANDLES];
    PDOS_EMS_ALLOCATE_ROUTINE Allocate;                 /* -> pages*16KB buffer    */
    PDOS_EMS_FREE_ROUTINE     Free;
    PVOID    Context;
} DOS_EMS_STATE, *PDOS_EMS_STATE;

typedef const DOS_EMS_STATE *PCDOS_EMS_STATE;

static inline VOID DosEmsClearHandleName(_Out_ PDOS_EMS_HANDLE handleEntry)
{
    INT nameIndex;
    for (nameIndex = 0; nameIndex < DOS_EMS_HANDLE_NAME_SIZE; ++nameIndex)
        handleEntry->Name[nameIndex] = 0;
}

/* --- bring-up -------------------------------------------------------------- */

static inline VOID DosEmsInitialize(_Out_ PDOS_EMS_STATE state, _In_ WORD frameSegment,
                                    _In_ WORD totalPages,
                                    _In_ volatile BYTE *frame,
                                    _In_opt_ PDOS_EMS_ALLOCATE_ROUTINE allocate,
                                    _In_opt_ PDOS_EMS_FREE_ROUTINE free, _In_opt_ PVOID context)
{
    INT handleIndex, windowIndex;
    state->FrameSegment = frameSegment;
    state->TotalPages = totalPages;
    state->UsedPages = 0;
    state->Frame = frame;
    for (windowIndex = 0; windowIndex < DOS_EMS_PHYSICAL_PAGES; ++windowIndex) {
        state->PhysicalPages[windowIndex].Handle = 0;
        state->PhysicalPages[windowIndex].LogicalPage = 0;
        state->PhysicalPages[windowIndex].IsMapped = 0;
    }
    for (handleIndex = 0; handleIndex < DOS_EMS_MAX_HANDLES; ++handleIndex) {
        state->Handles[handleIndex].InUse = 0;
        state->Handles[handleIndex].Pages = 0;
        state->Handles[handleIndex].Memory = 0;
        state->Handles[handleIndex].IsMapSaved = 0;
        DosEmsClearHandleName(&state->Handles[handleIndex]);
    }
    state->Allocate = allocate; state->Free = free; state->Context = context;
}

static inline PDOS_EMS_HANDLE DosEmsGetHandle(_In_ PDOS_EMS_STATE state, _In_ WORD handle)
{
    if (handle >= DOS_EMS_MAX_HANDLES) return 0;
    if (!state->Handles[handle].InUse) return 0;
    return &state->Handles[handle];
}

/* Copy helper: 16 KB between the page-frame window and a handle's backing page. */
static inline VOID DosEmsCopyPage(
    _Out_writes_bytes_(DOS_EMS_PAGE_SIZE) volatile BYTE *destination,
    _In_reads_bytes_(DOS_EMS_PAGE_SIZE) volatile BYTE *source)
{
    DWORD byteIndex;
    for (byteIndex = 0; byteIndex < DOS_EMS_PAGE_SIZE; ++byteIndex)
        destination[byteIndex] = source[byteIndex];
}

/* Write back whatever currently sits in physical window `windowIndex` to its backing page. */
static inline VOID DosEmsWriteBackWindow(_Inout_ PDOS_EMS_STATE state, _In_ INT windowIndex)
{
    PDOS_EMS_HANDLE handleEntry;
    if (!state->PhysicalPages[windowIndex].IsMapped) return;
    handleEntry = DosEmsGetHandle(state, state->PhysicalPages[windowIndex].Handle);
    if (handleEntry && handleEntry->Memory
        && state->PhysicalPages[windowIndex].LogicalPage < handleEntry->Pages)
        DosEmsCopyPage((volatile BYTE *)((PBYTE)handleEntry->Memory
                           + (DWORD)state->PhysicalPages[windowIndex].LogicalPage
                             * DOS_EMS_PAGE_SIZE),
                       state->Frame + (DWORD)windowIndex * DOS_EMS_PAGE_SIZE);
}

/* --- fn 42h: page counts --------------------------------------------------- */
static inline VOID DosEmsGetPageCounts(_In_ PCDOS_EMS_STATE state, _Out_opt_ PWORD freePages,
                                       _Out_opt_ PWORD totalPages)
{
    if (totalPages) *totalPages = state->TotalPages;
    if (freePages)  *freePages  = (WORD)(state->TotalPages - state->UsedPages);
}

/* --- fn 43h: allocate `pages` logical pages, returns a handle ------------- *
 * EMS forbids a zero-page allocation here (fn 43h); fn 5Ah allows it. */
static inline BOOL DosEmsAllocatePages(_Inout_ PDOS_EMS_STATE state, _In_ WORD pages,
                                       _Out_opt_ PWORD newHandle, _Out_opt_ PBYTE errorCode)
{
    INT handleIndex; PVOID buffer = 0;
    if (pages == 0) {
        if (errorCode) *errorCode = DOS_EMS_ERROR_ZERO_PAGES;
        return FALSE;
    }
    if (pages > state->TotalPages) {
        if (errorCode) *errorCode = DOS_EMS_ERROR_TOO_MANY_PAGES;
        return FALSE;
    }
    if (state->UsedPages + pages > state->TotalPages) {
        if (errorCode) *errorCode = DOS_EMS_ERROR_NOT_ENOUGH_PAGES;
        return FALSE;
    }
    for (handleIndex = 0; handleIndex < DOS_EMS_MAX_HANDLES; ++handleIndex)
        if (!state->Handles[handleIndex].InUse) break;
    if (handleIndex == DOS_EMS_MAX_HANDLES) {
        if (errorCode) *errorCode = DOS_EMS_ERROR_NO_HANDLES;
        return FALSE;
    }
    buffer = state->Allocate ? state->Allocate(state->Context, pages) : 0;
    if (!buffer) {
        if (errorCode) *errorCode = DOS_EMS_ERROR_NOT_ENOUGH_PAGES;
        return FALSE;
    }
    state->Handles[handleIndex].InUse = 1;
    state->Handles[handleIndex].Pages = pages;
    state->Handles[handleIndex].Memory = buffer;
    state->Handles[handleIndex].IsMapSaved = 0;
    DosEmsClearHandleName(&state->Handles[handleIndex]);
    state->UsedPages += pages;
    if (newHandle) *newHandle = (WORD)handleIndex;
    if (errorCode) *errorCode = DOS_EMS_STATUS_OK;
    return TRUE;
}

/* --- fn 44h: map logical page `logicalPage` of `handle` into window `physicalPage` *
 * logicalPage == DOS_EMS_UNMAP_LOGICAL_PAGE (0xFFFF) unmaps the window (LIM 4.0). */
static inline BOOL DosEmsMapPage(_Inout_ PDOS_EMS_STATE state, _In_ BYTE physicalPage,
                                 _In_ WORD logicalPage, _In_ WORD handle,
                                 _Out_opt_ PBYTE errorCode)
{
    PDOS_EMS_HANDLE handleEntry;
    if (physicalPage >= DOS_EMS_PHYSICAL_PAGES) {
        if (errorCode) *errorCode = DOS_EMS_ERROR_INVALID_PHYSICAL_PAGE;
        return FALSE;
    }
    handleEntry = DosEmsGetHandle(state, handle);
    if (!handleEntry) {
        if (errorCode) *errorCode = DOS_EMS_ERROR_INVALID_HANDLE;
        return FALSE;
    }
    if (logicalPage == DOS_EMS_UNMAP_LOGICAL_PAGE) {      /* unmap this window         */
        DosEmsWriteBackWindow(state, physicalPage);
        state->PhysicalPages[physicalPage].IsMapped = 0;
        if (errorCode) *errorCode = DOS_EMS_STATUS_OK;
        return TRUE;
    }
    if (logicalPage >= handleEntry->Pages) {
        if (errorCode) *errorCode = DOS_EMS_ERROR_INVALID_LOGICAL_PAGE;
        return FALSE;
    }
    DosEmsWriteBackWindow(state, physicalPage);           /* save the outgoing page    */
    DosEmsCopyPage(state->Frame + (DWORD)physicalPage * DOS_EMS_PAGE_SIZE,
                   (volatile BYTE *)((PBYTE)handleEntry->Memory
                                     + (DWORD)logicalPage * DOS_EMS_PAGE_SIZE));
    state->PhysicalPages[physicalPage].Handle = handle;
    state->PhysicalPages[physicalPage].LogicalPage = logicalPage;
    state->PhysicalPages[physicalPage].IsMapped = 1;
    if (errorCode) *errorCode = DOS_EMS_STATUS_OK;
    return TRUE;
}

/* --- fn 45h: deallocate a handle ------------------------------------------- */
static inline BOOL DosEmsDeallocatePages(_Inout_ PDOS_EMS_STATE state, _In_ WORD handle,
                                         _Out_opt_ PBYTE errorCode)
{
    PDOS_EMS_HANDLE handleEntry = DosEmsGetHandle(state, handle);
    INT windowIndex;
    if (!handleEntry) {
        if (errorCode) *errorCode = DOS_EMS_ERROR_INVALID_HANDLE;
        return FALSE;
    }
    /* drop any live windows */
    for (windowIndex = 0; windowIndex < DOS_EMS_PHYSICAL_PAGES; ++windowIndex)
        if (state->PhysicalPages[windowIndex].IsMapped
            && state->PhysicalPages[windowIndex].Handle == handle)
            state->PhysicalPages[windowIndex].IsMapped = 0;
    if (handleEntry->Memory && state->Free)
        state->Free(state->Context, handleEntry->Memory, handleEntry->Pages);
    state->UsedPages -= handleEntry->Pages;
    handleEntry->InUse = 0; handleEntry->Pages = 0; handleEntry->Memory = 0;
    handleEntry->IsMapSaved = 0; DosEmsClearHandleName(handleEntry);
    if (errorCode) *errorCode = DOS_EMS_STATUS_OK;
    return TRUE;
}

/* --- fn 4Ch: pages owned by a handle --------------------------------------- */
static inline BOOL DosEmsGetHandlePages(_In_ PDOS_EMS_STATE state, _In_ WORD handle,
                                        _Out_opt_ PWORD pages, _Out_opt_ PBYTE errorCode)
{
    PDOS_EMS_HANDLE handleEntry = DosEmsGetHandle(state, handle);
    if (!handleEntry) {
        if (errorCode) *errorCode = DOS_EMS_ERROR_INVALID_HANDLE;
        return FALSE;
    }
    if (pages) *pages = handleEntry->Pages;
    if (errorCode) *errorCode = DOS_EMS_STATUS_OK;
    return TRUE;
}

/* --- fn 4Bh: number of open handles ---------------------------------------- */
static inline INT DosEmsGetHandleCount(_In_ PCDOS_EMS_STATE state)
{
    INT handleIndex, handleCount = 0;
    for (handleIndex = 0; handleIndex < DOS_EMS_MAX_HANDLES; ++handleIndex)
        if (state->Handles[handleIndex].InUse) ++handleCount;
    return handleCount;
}

/* --- fn 4Dh: get all handle pages ------------------------------------------- *
 * LIM 4.0: ES:DI receives one {handle, pages} word pair per ACTIVE handle, BX the
 * count. Missing until s81 (#47): MEM /D calls it and, with AH=84h and BX left as
 * whatever it held, listed 256 handles of 4000h pages each. `entries` may be 0 to
 * count only; otherwise it must hold DOS_EMS_MAX_HANDLES pairs (4 bytes each). */
static inline INT DosEmsGetAllHandlePages(
    _In_ PCDOS_EMS_STATE state,
    _Out_writes_bytes_opt_(DOS_EMS_MAX_HANDLES * DOS_EMS_HANDLE_PAGES_ENTRY_SIZE) PBYTE entries)
{
    INT handleIndex, entryCount = 0;
    for (handleIndex = 0; handleIndex < DOS_EMS_MAX_HANDLES; ++handleIndex) {
        if (!state->Handles[handleIndex].InUse) continue;
        if (entries) {
            entries[entryCount * DOS_EMS_HANDLE_PAGES_ENTRY_SIZE + DOS_EMS_ENTRY_HANDLE_LOW]
                = (BYTE)(handleIndex & BYTE_MASK);
            entries[entryCount * DOS_EMS_HANDLE_PAGES_ENTRY_SIZE + DOS_EMS_ENTRY_HANDLE_HIGH]
                = (BYTE)(handleIndex >> BYTE_SHIFT);
            entries[entryCount * DOS_EMS_HANDLE_PAGES_ENTRY_SIZE + DOS_EMS_ENTRY_PAGES_LOW]
                = (BYTE)(state->Handles[handleIndex].Pages & BYTE_MASK);
            entries[entryCount * DOS_EMS_HANDLE_PAGES_ENTRY_SIZE + DOS_EMS_ENTRY_PAGES_HIGH]
                = (BYTE)(state->Handles[handleIndex].Pages >> BYTE_SHIFT);
        }
        ++entryCount;
    }
    return entryCount;
}

/* --- fn 53h: get (AL=0) / set (AL=1) a handle's 8-byte name ---------------- *
 * LIM 4.0. A bad handle is 83h -- and that is what matters most: MEM /D walks
 * handles 0-255 with 4Ch and 53h and lists every one not refused as BAD HANDLE, so
 * while 53h answered 84h (undefined function) it listed all 256 (s81, #47). */
static inline BOOL DosEmsGetSetHandleName(
    _In_ PDOS_EMS_STATE state, _In_ WORD handle, _In_ BOOL isSetRequest,
    _Inout_updates_bytes_(DOS_EMS_HANDLE_NAME_SIZE) volatile BYTE *name,
    _Out_opt_ PBYTE errorCode)
{
    PDOS_EMS_HANDLE handleEntry = DosEmsGetHandle(state, handle);
    INT nameIndex;
    if (!handleEntry) {
        if (errorCode) *errorCode = DOS_EMS_ERROR_INVALID_HANDLE;
        return FALSE;
    }
    for (nameIndex = 0; nameIndex < DOS_EMS_HANDLE_NAME_SIZE; ++nameIndex) {
        if (isSetRequest) handleEntry->Name[nameIndex] = name[nameIndex];
        else name[nameIndex] = handleEntry->Name[nameIndex];
    }
    if (errorCode) *errorCode = DOS_EMS_STATUS_OK;
    return TRUE;
}

/* --- fn 51h: reallocate a handle's page count ------------------------------ *
 * Preserves min(old,new) pages of content. Active mappings of pages that no
 * longer exist after a shrink are dropped. */
static inline BOOL DosEmsReallocatePages(_Inout_ PDOS_EMS_STATE state, _In_ WORD handle,
                                         _In_ WORD newPages, _Out_opt_ PBYTE errorCode)
{
    PDOS_EMS_HANDLE handleEntry = DosEmsGetHandle(state, handle);
    PVOID newBuffer = 0; DWORD bytesToKeep, byteIndex; INT windowIndex;
    if (!handleEntry) {
        if (errorCode) *errorCode = DOS_EMS_ERROR_INVALID_HANDLE;
        return FALSE;
    }
    if (newPages == handleEntry->Pages) {
        if (errorCode) *errorCode = DOS_EMS_STATUS_OK;
        return TRUE;
    }
    if (newPages > handleEntry->Pages &&
        state->UsedPages - handleEntry->Pages + newPages > state->TotalPages) {
        if (errorCode) *errorCode = DOS_EMS_ERROR_NOT_ENOUGH_PAGES;
        return FALSE;
    }
    /* flush any of this handle's live windows so the backing buffer is current  */
    for (windowIndex = 0; windowIndex < DOS_EMS_PHYSICAL_PAGES; ++windowIndex)
        if (state->PhysicalPages[windowIndex].IsMapped
            && state->PhysicalPages[windowIndex].Handle == handle)
            DosEmsWriteBackWindow(state, windowIndex);
    if (newPages > 0) {
        newBuffer = state->Allocate ? state->Allocate(state->Context, newPages) : 0;
        if (!newBuffer) {
            if (errorCode) *errorCode = DOS_EMS_ERROR_NOT_ENOUGH_PAGES;
            return FALSE;
        }
        bytesToKeep = (newPages < handleEntry->Pages ? newPages : handleEntry->Pages)
                    * DOS_EMS_PAGE_SIZE;
        for (byteIndex = 0; byteIndex < bytesToKeep; ++byteIndex)
            ((PBYTE)newBuffer)[byteIndex] = ((PBYTE)handleEntry->Memory)[byteIndex];
    }
    /* drop now-invalid mappings */
    for (windowIndex = 0; windowIndex < DOS_EMS_PHYSICAL_PAGES; ++windowIndex)
        if (state->PhysicalPages[windowIndex].IsMapped
            && state->PhysicalPages[windowIndex].Handle == handle
            && state->PhysicalPages[windowIndex].LogicalPage >= newPages)
            state->PhysicalPages[windowIndex].IsMapped = 0;
    if (handleEntry->Memory && state->Free)
        state->Free(state->Context, handleEntry->Memory, handleEntry->Pages);
    state->UsedPages = (WORD)(state->UsedPages - handleEntry->Pages + newPages);
    handleEntry->Memory = newBuffer; handleEntry->Pages = newPages;
    if (errorCode) *errorCode = DOS_EMS_STATUS_OK;
    return TRUE;
}

/* --- fn 47h/48h: save / restore the page-map context for a handle ---------- */
static inline BOOL DosEmsSavePageMap(_In_ PDOS_EMS_STATE state, _In_ WORD handle,
                                     _Out_opt_ PBYTE errorCode)
{
    PDOS_EMS_HANDLE handleEntry = DosEmsGetHandle(state, handle);
    INT windowIndex;
    if (!handleEntry) {
        if (errorCode) *errorCode = DOS_EMS_ERROR_INVALID_HANDLE;
        return FALSE;
    }
    if (handleEntry->IsMapSaved) {
        if (errorCode) *errorCode = DOS_EMS_ERROR_MAP_ALREADY_SAVED;
        return FALSE;
    }
    for (windowIndex = 0; windowIndex < DOS_EMS_PHYSICAL_PAGES; ++windowIndex) {
        handleEntry->SavedMap[windowIndex].Handle      = state->PhysicalPages[windowIndex].Handle;
        handleEntry->SavedMap[windowIndex].LogicalPage =
            state->PhysicalPages[windowIndex].LogicalPage;
        handleEntry->SavedMap[windowIndex].IsMapped    = state->PhysicalPages[windowIndex].IsMapped;
    }
    handleEntry->IsMapSaved = 1;
    if (errorCode) *errorCode = DOS_EMS_STATUS_OK;
    return TRUE;
}

static inline BOOL DosEmsRestorePageMap(_Inout_ PDOS_EMS_STATE state, _In_ WORD handle,
                                        _Out_opt_ PBYTE errorCode)
{
    PDOS_EMS_HANDLE handleEntry = DosEmsGetHandle(state, handle);
    INT windowIndex;
    if (!handleEntry) {
        if (errorCode) *errorCode = DOS_EMS_ERROR_INVALID_HANDLE;
        return FALSE;
    }
    if (!handleEntry->IsMapSaved) {
        if (errorCode) *errorCode = DOS_EMS_ERROR_MAP_NOT_SAVED;
        return FALSE;
    }
    for (windowIndex = 0; windowIndex < DOS_EMS_PHYSICAL_PAGES; ++windowIndex) {
        if (handleEntry->SavedMap[windowIndex].IsMapped)
            DosEmsMapPage(state, (BYTE)windowIndex, handleEntry->SavedMap[windowIndex].LogicalPage,
                          handleEntry->SavedMap[windowIndex].Handle, errorCode);
        else {
            DosEmsWriteBackWindow(state, windowIndex);
            state->PhysicalPages[windowIndex].IsMapped = 0;
        }
    }
    handleEntry->IsMapSaved = 0;
    if (errorCode) *errorCode = DOS_EMS_STATUS_OK;
    return TRUE;
}

#endif /* NTVDMEX_DOS_EMS_H */
