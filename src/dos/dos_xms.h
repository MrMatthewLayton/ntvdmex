/* dos_xms.h -- XMS 3.0 (eXtended Memory Specification) core, host-testable.
 *
 * The memory-extension layer for M4.  XMS is how real-mode DOS programs reach
 * memory above 1 MB without leaving real mode: they obtain the driver entry
 * point via INT 2Fh AX=4310h and then FAR-CALL it with the function in AH.
 *
 * Architecture note (why this is clean under V86 + NtVdmControl): extended
 * memory is *above* the 1 MB the V86 map covers, so we do NOT place it in the
 * guest address space at all.  Each Extended Memory Block (EMB) is a buffer on
 * the HOST heap.  A pure-real-mode client never addresses an EMB directly --
 * it moves data in and out with the Move function (0Bh), which we implement as
 * a memcpy between the host EMB buffer and the guest's conventional window.
 * (Lock (0Ch) hands back a 32-bit linear address for protected-mode/DPMI
 * clients; see the note on DosXmsLock.)
 *
 * Same discipline as dos_mcb.h: no Windows calls, only Windows types
 * (src/ntvdmex_types.h), no globals. The backing store is supplied through
 * allocate/free hooks (the host passes VirtualAlloc-based ones; the off-VM
 * battery passes malloc/free), so the allocator logic is identical in V86 and
 * under the native test cc.
 * Verified off-VM by tests/unit/xms_test.c.
 */
#ifndef NTVDMEX_DOS_XMS_H
#define NTVDMEX_DOS_XMS_H

#include "../ntvdmex_types.h"

#define DOS_XMS_MAX_HANDLES   64       /* EMB handles 1..DOS_XMS_MAX_HANDLES        */
#define DOS_XMS_VERSION       0x0300   /* XMS spec version, BCD (3.0)              */
/* Driver internal revision. ⚠ 0x0310 (what HIMEM.SYS reports, measured by
   p_xms.asm on 6.22) WAS TRIED AND REFUTED for GH #47: MEM's own version
   wrapper returns BX rather than AX --
       3CBB: xor ah,ah / call far [XMS entry] / mov dx,bx / ret
   -- so the revision is a value it actually propagates, which made it a
   well-founded suspect. It changed nothing. Fifth refuted hypothesis. */
#define DOS_XMS_REVISION      0x0300

/* XMS error codes (returned in BL when AX=0). */
#define DOS_XMS_ERROR_NOT_IMPLEMENTED             0x80  /* function not implemented      */
#define DOS_XMS_ERROR_DRIVER                      0x8E  /* general driver error          */
#define DOS_XMS_ERROR_NO_HMA                      0x90  /* HMA does not exist            */
#define DOS_XMS_ERROR_HMA_IN_USE                  0x91  /* HMA already in use            */
#define DOS_XMS_ERROR_HMA_NOT_ALLOCATED           0x93  /* HMA not allocated             */
#define DOS_XMS_ERROR_OUT_OF_MEMORY               0xA0  /* all extended memory is allocated */
#define DOS_XMS_ERROR_NO_HANDLES                  0xA1  /* all handles are in use        */
#define DOS_XMS_ERROR_INVALID_HANDLE              0xA2  /* invalid handle                */
#define DOS_XMS_ERROR_INVALID_SOURCE_HANDLE       0xA3  /* invalid source handle         */
#define DOS_XMS_ERROR_INVALID_SOURCE_OFFSET       0xA4  /* invalid source offset         */
#define DOS_XMS_ERROR_INVALID_DESTINATION_HANDLE  0xA5  /* invalid destination handle    */
#define DOS_XMS_ERROR_INVALID_DESTINATION_OFFSET  0xA6  /* invalid destination offset    */
#define DOS_XMS_ERROR_INVALID_LENGTH              0xA7  /* invalid length                */
#define DOS_XMS_ERROR_NOT_LOCKED                  0xAA  /* block is not locked           */
#define DOS_XMS_ERROR_LOCKED                      0xAB  /* block is locked               */
#define DOS_XMS_ERROR_LOCK_OVERFLOW               0xAC  /* lock count overflow           */

/* Handle 0 names conventional memory in a Move; EMB handles are 1-based. */
#define DOS_XMS_CONVENTIONAL_HANDLE  0
#define DOS_XMS_FIRST_HANDLE         1
#define DOS_XMS_BYTES_PER_KB         1024u
#define DOS_XMS_MAX_LOCK_COUNT       0xFF   /* the lock count is a byte               */
#define DOS_XMS_MAX_FREE_HANDLES     255    /* fn 0Eh reports the free count in BL    */
#define DOS_XMS_ODD_LENGTH_MASK      1      /* a move length must be even             */

/* A real-mode far pointer as a Move offset: low word = offset, high word = segment. */
#define DOS_XMS_FAR_SEGMENT_SHIFT    16

/* One Extended Memory Block. */
typedef struct _DOS_XMS_HANDLE {
    BYTE     InUse;       /* 1 = allocated (handle in use)                     */
    BYTE     LockCount;   /* lock count (0 = unlocked)                         */
    DWORD    SizeKb;      /* block size in KB (0 is a legal, empty block)      */
    PVOID    Memory;      /* host backing buffer (SizeKb*1024 bytes; 0 if KB=0)*/
} DOS_XMS_HANDLE, *PDOS_XMS_HANDLE;

typedef const DOS_XMS_HANDLE *PCDOS_XMS_HANDLE;

/* Backing-store hooks. Allocate returns a zeroed buffer of `kilobytes` KB (or NULL on
   failure); Free releases it. The context is passed through (host state / unused). */
typedef PVOID (*PDOS_XMS_ALLOCATE_ROUTINE)(PVOID context, DWORD kilobytes);
typedef VOID  (*PDOS_XMS_FREE_ROUTINE)(PVOID context, PVOID memory, DWORD kilobytes);

typedef struct _DOS_XMS_STATE {
    BOOL       IsA20Enabled;    /* A20 gate state (0 = masked, 1 = enabled)     */
    BOOL       IsHmaAllocated;  /* HMA (the 64KB-16 above 1MB) allocated?       */
    DWORD      TotalKb;         /* size of the extended-memory pool we advertise */
    DWORD      UsedKb;          /* KB currently committed across all handles    */
    DOS_XMS_HANDLE Handles[DOS_XMS_MAX_HANDLES];
    PDOS_XMS_ALLOCATE_ROUTINE Allocate;
    PDOS_XMS_FREE_ROUTINE     Free;
    PVOID      Context;
} DOS_XMS_STATE, *PDOS_XMS_STATE;

typedef const DOS_XMS_STATE *PCDOS_XMS_STATE;

/* The 16-byte Move structure (XMS fn 0Bh), pointed at by DS:SI in the guest.
   For a conventional endpoint Handle==0 and Offset is a real-mode far pointer
   (low word = offset, high word = segment); for an EMB, Offset is a 32-bit
   byte offset into that handle's block. */
typedef struct _DOS_XMS_MOVE {
    DWORD    Length;             /* bytes to move (must be even; 0 is a no-op)        */
    WORD     SourceHandle;       /* 0 = conventional                                  */
    DWORD    SourceOffset;
    WORD     DestinationHandle;  /* 0 = conventional                                  */
    DWORD    DestinationOffset;
} DOS_XMS_MOVE, *PDOS_XMS_MOVE;

typedef const DOS_XMS_MOVE *PCDOS_XMS_MOVE;

/* --- bring-up -------------------------------------------------------------- */

static inline VOID DosXmsInitialize(_Out_ PDOS_XMS_STATE state, _In_ DWORD totalKb,
                                    _In_opt_ PDOS_XMS_ALLOCATE_ROUTINE allocate,
                                    _In_opt_ PDOS_XMS_FREE_ROUTINE free,
                                    _In_opt_ PVOID context)
{
    INT handleIndex;
    /* ── A20 IS ON, AND SAYING OTHERWISE WAS A LIE ABOUT OUR OWN MACHINE. ─────
         This started masked, so AH=07h (query A20) answered "disabled" until a
         guest happened to call AH=03h. But an NT VDM does not wrap at 1 MB --
         the line is effectively always open -- and extended memory is
         unreachable with A20 masked, so a memory reporter that asks first and
         allocates second is told the pool it can see is unusable.
       Oracle, tests/probes/dos/p_xms.asm on MS-DOS 6.22 with HIMEM.SYS loaded:
         CASE=xms.07.query.a20 SIG=AX,BX AX=0001 BX=B100
       i.e. enabled. Ours answered AX=0000. (GH #47) */
    state->IsA20Enabled = TRUE;
    state->IsHmaAllocated = FALSE;
    state->TotalKb = totalKb;
    state->UsedKb = 0;
    for (handleIndex = 0; handleIndex < DOS_XMS_MAX_HANDLES; ++handleIndex) {
        state->Handles[handleIndex].InUse = 0;
        state->Handles[handleIndex].LockCount = 0;
        state->Handles[handleIndex].SizeKb = 0;
        state->Handles[handleIndex].Memory = 0;
    }
    state->Allocate = allocate; state->Free = free; state->Context = context;
}

/* Resolve a handle number (1-based) to its slot, or NULL if invalid/free. */
static inline PDOS_XMS_HANDLE DosXmsGetHandle(_In_ PDOS_XMS_STATE state, _In_ WORD handle)
{
    if (handle == DOS_XMS_CONVENTIONAL_HANDLE || handle > DOS_XMS_MAX_HANDLES) return 0;
    if (!state->Handles[handle - DOS_XMS_FIRST_HANDLE].InUse) return 0;
    return &state->Handles[handle - DOS_XMS_FIRST_HANDLE];
}

/* --- fn 08h: query free extended memory ------------------------------------ *
 * Returns the largest free block (KB) and the total free (KB). With a single
 * pool the largest free block is the whole remaining pool. */
static inline VOID DosXmsQueryFreeMemory(_In_ PCDOS_XMS_STATE state,
                                         _Out_opt_ PDWORD largestKb, _Out_opt_ PDWORD totalKb)
{
    DWORD freeKb = (state->TotalKb > state->UsedKb) ? (state->TotalKb - state->UsedKb) : 0;
    if (largestKb) *largestKb = freeKb;
    if (totalKb)   *totalKb   = freeKb;
}

/* --- fn 09h: allocate an EMB of `kilobytes` KB ----------------------------- *
 * On success returns TRUE and *newHandle = the new handle (1-based). On failure
 * returns FALSE and *errorCode = an XMS error code. A 0-KB request is legal (an
 * empty block to be grown later by Reallocate). */
static inline BOOL DosXmsAllocate(_Inout_ PDOS_XMS_STATE state, _In_ DWORD kilobytes,
                                  _Out_opt_ PWORD newHandle, _Out_opt_ PBYTE errorCode)
{
    INT handleIndex; PVOID buffer = 0;
    if (kilobytes > 0 && state->UsedKb + kilobytes > state->TotalKb) {
        if (errorCode) *errorCode = DOS_XMS_ERROR_OUT_OF_MEMORY;
        return FALSE;
    }
    for (handleIndex = 0; handleIndex < DOS_XMS_MAX_HANDLES; ++handleIndex)
        if (!state->Handles[handleIndex].InUse) break;
    if (handleIndex == DOS_XMS_MAX_HANDLES) {
        if (errorCode) *errorCode = DOS_XMS_ERROR_NO_HANDLES;
        return FALSE;
    }
    if (kilobytes > 0) {
        buffer = state->Allocate ? state->Allocate(state->Context, kilobytes) : 0;
        if (!buffer) {
            if (errorCode) *errorCode = DOS_XMS_ERROR_OUT_OF_MEMORY;
            return FALSE;
        }
    }
    state->Handles[handleIndex].InUse = 1;
    state->Handles[handleIndex].LockCount = 0;
    state->Handles[handleIndex].SizeKb = kilobytes;
    state->Handles[handleIndex].Memory = buffer;
    state->UsedKb += kilobytes;
    if (newHandle) *newHandle = (WORD)(handleIndex + DOS_XMS_FIRST_HANDLE);
    return TRUE;
}

/* --- fn 0Ah: free an EMB --------------------------------------------------- *
 * Fails if the block is still locked. */
static inline BOOL DosXmsFree(_Inout_ PDOS_XMS_STATE state, _In_ WORD handle,
                              _Out_opt_ PBYTE errorCode)
{
    PDOS_XMS_HANDLE handleEntry = DosXmsGetHandle(state, handle);
    if (!handleEntry) {
        if (errorCode) *errorCode = DOS_XMS_ERROR_INVALID_HANDLE;
        return FALSE;
    }
    if (handleEntry->LockCount) {
        if (errorCode) *errorCode = DOS_XMS_ERROR_LOCKED;
        return FALSE;
    }
    if (handleEntry->Memory && state->Free)
        state->Free(state->Context, handleEntry->Memory, handleEntry->SizeKb);
    state->UsedKb -= handleEntry->SizeKb;
    handleEntry->InUse = 0; handleEntry->LockCount = 0;
    handleEntry->SizeKb = 0; handleEntry->Memory = 0;
    return TRUE;
}

/* --- fn 0Fh: reallocate an EMB to `newKb` KB ------------------------------- *
 * Preserves min(old,new) bytes of content. Fails if the block is locked. */
static inline BOOL DosXmsReallocate(_Inout_ PDOS_XMS_STATE state, _In_ WORD handle,
                                    _In_ DWORD newKb, _Out_opt_ PBYTE errorCode)
{
    PDOS_XMS_HANDLE handleEntry = DosXmsGetHandle(state, handle);
    PVOID newBuffer = 0; DWORD byteIndex, bytesToKeep;
    if (!handleEntry) {
        if (errorCode) *errorCode = DOS_XMS_ERROR_INVALID_HANDLE;
        return FALSE;
    }
    if (handleEntry->LockCount) {
        if (errorCode) *errorCode = DOS_XMS_ERROR_LOCKED;
        return FALSE;
    }
    if (newKb == handleEntry->SizeKb) return TRUE;
    if (newKb > handleEntry->SizeKb &&
        state->UsedKb - handleEntry->SizeKb + newKb > state->TotalKb) {
        if (errorCode) *errorCode = DOS_XMS_ERROR_OUT_OF_MEMORY;
        return FALSE;
    }
    if (newKb > 0) {
        newBuffer = state->Allocate ? state->Allocate(state->Context, newKb) : 0;
        if (!newBuffer) {
            if (errorCode) *errorCode = DOS_XMS_ERROR_OUT_OF_MEMORY;
            return FALSE;
        }
        bytesToKeep = (newKb < handleEntry->SizeKb ? newKb : handleEntry->SizeKb)
                    * DOS_XMS_BYTES_PER_KB;
        for (byteIndex = 0; byteIndex < bytesToKeep; ++byteIndex)
            ((PBYTE)newBuffer)[byteIndex] = ((PBYTE)handleEntry->Memory)[byteIndex];
    }
    if (handleEntry->Memory && state->Free)
        state->Free(state->Context, handleEntry->Memory, handleEntry->SizeKb);
    state->UsedKb = state->UsedKb - handleEntry->SizeKb + newKb;
    handleEntry->Memory = newBuffer; handleEntry->SizeKb = newKb;
    return TRUE;
}

/* --- fn 0Eh: get EMB handle information ------------------------------------ */
static inline BOOL DosXmsGetHandleInformation(_In_ PCDOS_XMS_STATE state, _In_ WORD handle,
                                              _Out_opt_ PBYTE lockCount,
                                              _Out_opt_ PBYTE freeHandles,
                                              _Out_opt_ PDWORD sizeKb, _Out_opt_ PBYTE errorCode)
{
    INT handleIndex, freeHandleCount = 0;
    PCDOS_XMS_HANDLE handleEntry;
    if (handle == DOS_XMS_CONVENTIONAL_HANDLE || handle > DOS_XMS_MAX_HANDLES
        || !state->Handles[handle - DOS_XMS_FIRST_HANDLE].InUse) {
        if (errorCode) *errorCode = DOS_XMS_ERROR_INVALID_HANDLE;
        return FALSE;
    }
    handleEntry = &state->Handles[handle - DOS_XMS_FIRST_HANDLE];
    for (handleIndex = 0; handleIndex < DOS_XMS_MAX_HANDLES; ++handleIndex)
        if (!state->Handles[handleIndex].InUse) ++freeHandleCount;
    if (lockCount)   *lockCount   = handleEntry->LockCount;
    if (freeHandles) *freeHandles = (BYTE)(freeHandleCount > DOS_XMS_MAX_FREE_HANDLES
                                           ? DOS_XMS_MAX_FREE_HANDLES : freeHandleCount);
    if (sizeKb)      *sizeKb      = handleEntry->SizeKb;
    return TRUE;
}

/* --- fn 0Ch / 0Dh: lock / unlock an EMB ------------------------------------ *
 * Lock pins the block and returns its 32-bit linear address. Under our model
 * the EMB lives on the host heap, so the "linear address" is the host pointer
 * truncated to 32 bits -- meaningful only to a protected-mode/DPMI client that
 * shares our flat address space (M4 DPMI work); a pure real-mode client uses
 * Move instead and never dereferences this. */
static inline BOOL DosXmsLock(_Inout_ PDOS_XMS_STATE state, _In_ WORD handle,
                              _Out_opt_ PDWORD linearAddress, _Out_opt_ PBYTE errorCode)
{
    PDOS_XMS_HANDLE handleEntry = DosXmsGetHandle(state, handle);
    if (!handleEntry) {
        if (errorCode) *errorCode = DOS_XMS_ERROR_INVALID_HANDLE;
        return FALSE;
    }
    if (handleEntry->LockCount == DOS_XMS_MAX_LOCK_COUNT) {
        if (errorCode) *errorCode = DOS_XMS_ERROR_LOCK_OVERFLOW;
        return FALSE;
    }
    ++handleEntry->LockCount;
    if (linearAddress) *linearAddress = (DWORD)(UINT_PTR)handleEntry->Memory;
    return TRUE;
}

static inline BOOL DosXmsUnlock(_Inout_ PDOS_XMS_STATE state, _In_ WORD handle,
                                _Out_opt_ PBYTE errorCode)
{
    PDOS_XMS_HANDLE handleEntry = DosXmsGetHandle(state, handle);
    if (!handleEntry) {
        if (errorCode) *errorCode = DOS_XMS_ERROR_INVALID_HANDLE;
        return FALSE;
    }
    if (handleEntry->LockCount == 0) {
        if (errorCode) *errorCode = DOS_XMS_ERROR_NOT_LOCKED;
        return FALSE;
    }
    --handleEntry->LockCount;
    return TRUE;
}

/* --- fn 0Bh: move a block -------------------------------------------------- *
 * Copies move->Length bytes from the source endpoint to the destination. An
 * endpoint with handle 0 is conventional memory: its offset is a real-mode far
 * pointer (low word = offset, high word = segment) resolved against
 * `conventionalBase` (NULL => absolute V86 addressing: linear = (seg<<4)+off). An
 * endpoint with a nonzero handle is an EMB: its offset is a byte offset into that
 * block. Returns TRUE on success; FALSE with *errorCode set on a bad
 * handle/offset/length. */
/* s84: THE MOVE MUST STAY INSIDE WHAT IT NAMES. `offset + len > size` wrapped at 32
   bits (offset FFFFF000h, length 2000h passed), and a conventional endpoint had no
   limit at all -- either one let a DOS program read or write the HOST's memory past
   the block or past the first megabyte. A real-mode far pointer reaches at most
   FFFF:FFFF = 10FFEFh, so that is the conventional ceiling. */
#define DOS_XMS_CONVENTIONAL_LIMIT 0x10FFF0u
static inline BOOL DosXmsMove(_Inout_ PDOS_XMS_STATE state,
                              _In_opt_ volatile BYTE *conventionalBase,
                              _In_ PCDOS_XMS_MOVE move, _Out_opt_ PBYTE errorCode)
{
    volatile BYTE *source, *destination;
    DWORD length = move->Length, byteIndex;
    if (length == 0) return TRUE;           /* a 0-length move is a legal no-op  */
    if (length & DOS_XMS_ODD_LENGTH_MASK) {
        if (errorCode) *errorCode = DOS_XMS_ERROR_INVALID_LENGTH;
        return FALSE;
    }

    if (move->SourceHandle == DOS_XMS_CONVENTIONAL_HANDLE) {
        DWORD segment = (move->SourceOffset >> DOS_XMS_FAR_SEGMENT_SHIFT) & WORD_MASK,
              offset = move->SourceOffset & WORD_MASK;
        if ((segment << PARAGRAPH_SHIFT) + offset + length > DOS_XMS_CONVENTIONAL_LIMIT) {
            if (errorCode) *errorCode = DOS_XMS_ERROR_INVALID_SOURCE_OFFSET;
            return FALSE;
        }
        source = (volatile BYTE *)((UINT_PTR)conventionalBase
                                   + (segment << PARAGRAPH_SHIFT) + offset);
    } else {
        PDOS_XMS_HANDLE handleEntry = DosXmsGetHandle(state, move->SourceHandle);
        if (!handleEntry) {
            if (errorCode) *errorCode = DOS_XMS_ERROR_INVALID_SOURCE_HANDLE;
            return FALSE;
        }
        if (move->SourceOffset > handleEntry->SizeKb * DOS_XMS_BYTES_PER_KB
            || length > handleEntry->SizeKb * DOS_XMS_BYTES_PER_KB - move->SourceOffset) {
            if (errorCode) *errorCode = DOS_XMS_ERROR_INVALID_SOURCE_OFFSET;
            return FALSE;
        }   /* no 32-bit wrap */
        source = (volatile BYTE *)((PBYTE)handleEntry->Memory + move->SourceOffset);
    }
    if (move->DestinationHandle == DOS_XMS_CONVENTIONAL_HANDLE) {
        DWORD segment = (move->DestinationOffset >> DOS_XMS_FAR_SEGMENT_SHIFT)
                      & WORD_MASK,
              offset = move->DestinationOffset & WORD_MASK;
        if ((segment << PARAGRAPH_SHIFT) + offset + length > DOS_XMS_CONVENTIONAL_LIMIT) {
            if (errorCode) *errorCode = DOS_XMS_ERROR_INVALID_DESTINATION_OFFSET;
            return FALSE;
        }
        destination = (volatile BYTE *)((UINT_PTR)conventionalBase
                                        + (segment << PARAGRAPH_SHIFT) + offset);
    } else {
        PDOS_XMS_HANDLE handleEntry = DosXmsGetHandle(state, move->DestinationHandle);
        if (!handleEntry) {
            if (errorCode) *errorCode = DOS_XMS_ERROR_INVALID_DESTINATION_HANDLE;
            return FALSE;
        }
        if (move->DestinationOffset > handleEntry->SizeKb * DOS_XMS_BYTES_PER_KB
            || length > handleEntry->SizeKb * DOS_XMS_BYTES_PER_KB - move->DestinationOffset) {
            if (errorCode) *errorCode = DOS_XMS_ERROR_INVALID_DESTINATION_OFFSET;
            return FALSE;
        }   /* no 32-bit wrap */
        destination = (volatile BYTE *)((PBYTE)handleEntry->Memory + move->DestinationOffset);
    }
    /* Overlap-safe copy (real HIMEM permits an overlapping move within a block). */
    if (destination <= source)
        for (byteIndex = 0; byteIndex < length; ++byteIndex)
            destination[byteIndex] = source[byteIndex];
    else
        for (byteIndex = length; byteIndex-- > 0; )
            destination[byteIndex] = source[byteIndex];
    return TRUE;
}

#endif /* NTVDMEX_DOS_XMS_H */
