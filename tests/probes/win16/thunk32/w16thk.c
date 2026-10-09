/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * W16THK.DLL, the 32-bit half of tests/probes/win16/w_wcb (s91, #309).
 *
 * A 32-bit thunk DLL of the kind wownt32.h is for: 16-bit code reaches it through
 * the generic thunks (LoadLibraryEx32W / CallProc32W), and it calls BACK into the
 * VDM through WOW32.DLL. It finds WOW32.DLL the way XP's own winmm.dll does --
 * GetModuleHandle + GetProcAddress, never an import -- so the same file runs
 * under stock (the real WOW32.DLL) and under NTVDMEX (bin\wowshim\WOW32.DLL)
 * without either loader having to resolve a static import against the other.
 *
 *   T_Cb(callback16, parameter)  WOWCallback16(callback16, parameter)  -> its DX:AX
 *   T_CbEx(callback16)           WOWCallback16Ex, PASCAL, three WORDs 1,2,3  -> its DX:AX
 *                                (the callee answers a*100+b*10+c: 123 = the order is right)
 *   T_Glob()                     Alloc/Lock/write/LockSize/Unlock/Free, then AllocLock +
 *                                UnlockFree -- a bit per step that answered as documented
 *
 * Build: tests/probes/win16/thunk32/build.sh  (i686-w64-mingw32, no CRT)
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <windows.h>

#define WCB16_PASCAL                    0               /* wownt32.h */
#define THUNK_NOT_FOUND                 0xDEAD0001u     /* WOW32.DLL or the export is missing */
#define THUNK_CALL_FAILED               0xDEAD0002u
#define THUNK_ARGUMENT_COUNT            3
#define THUNK_ARGUMENT_A                1               /* The callee answers a*100+b*10+c */
#define THUNK_ARGUMENT_B                2
#define THUNK_ARGUMENT_C                3
#define THUNK_BLOCK_SIZE                100
#define THUNK_FIXED_SIZE                32
#define THUNK_MARK_LENGTH               2
#define THUNK_MARK_FIRST                'O'
#define THUNK_MARK_SECOND               'K'
#define THUNK_NO_HANDLE                 0xFFFF
#define THUNK_LOW_BYTE                  0xFF
#define THUNK_SELECTOR_SHIFT            16              /* A 16:16 pointer's selector */

/* T_Glob's answer: a bit per step that answered as documented... */
#define THUNK_ALLOCATED                 1
#define THUNK_LOCKED                    2
#define THUNK_ZEROED                    4
#define THUNK_SIZED                     8
#define THUNK_WRITTEN                   0x10
#define THUNK_FREED                     0x20
#define THUNK_ALLOC_LOCKED              0x40
#define THUNK_UNLOCK_FREED              0x80

/* ...and, where one did not, what it said instead. */
#define THUNK_FREE_CODE_SHIFT           8
#define THUNK_UNLOCK_FREE_CODE_SHIFT    10
#define THUNK_FREE_BYTE_SHIFT           16
#define THUNK_UNLOCK_FREE_BYTE_SHIFT    24
#define THUNK_ANSWER_ZERO               0
#define THUNK_ANSWER_HANDLE             1
#define THUNK_ANSWER_OTHER              2               /* GlobalFree16 */
#define THUNK_ANSWER_SELECTOR           2               /* GlobalUnlockFree16: the selector */
#define THUNK_ANSWER_UNKNOWN            3               /* GlobalUnlockFree16: anything else */

typedef BOOL   (WINAPI *PWOW_CALLBACK16_EX)(DWORD, DWORD, DWORD, PVOID, PDWORD);

typedef DWORD  (WINAPI *PWOW_CALLBACK16)(DWORD, DWORD);

typedef WORD   (WINAPI *PWOW_GLOBAL_ALLOC16)(WORD, DWORD);

typedef WORD   (WINAPI *PWOW_GLOBAL_FREE16)(WORD);

typedef DWORD  (WINAPI *PWOW_GLOBAL_LOCK16)(WORD);

typedef BOOL   (WINAPI *PWOW_GLOBAL_UNLOCK16)(WORD);

typedef DWORD  (WINAPI *PWOW_GLOBAL_ALLOC_LOCK16)(WORD, DWORD, WORD *);

typedef WORD   (WINAPI *PWOW_GLOBAL_UNLOCK_FREE16)(DWORD);

typedef DWORD  (WINAPI *PWOW_GLOBAL_LOCK_SIZE16)(WORD, PDWORD);

typedef LPVOID (WINAPI *PWOW_GET_VDM_POINTER)(DWORD, DWORD, BOOL);

static FARPROC ThunkWow32Procedure(const char *name)
{
    HMODULE wow32 = GetModuleHandleA("WOW32.DLL");

    return wow32 ? GetProcAddress(wow32, name) : NULL;
}

__declspec(dllexport) DWORD WINAPI T_Cb(DWORD callback16, DWORD parameter)
{
    PWOW_CALLBACK16 callback = (PWOW_CALLBACK16)ThunkWow32Procedure("WOWCallback16");

    return callback ? callback(callback16, parameter) : THUNK_NOT_FOUND;
}

__declspec(dllexport) DWORD WINAPI T_CbEx(DWORD callback16)
{
    PWOW_CALLBACK16_EX callback = (PWOW_CALLBACK16_EX)ThunkWow32Procedure("WOWCallback16Ex");
    WORD arguments[THUNK_ARGUMENT_COUNT];
    DWORD result = 0;

    /* PASCAL: the stack image, lowest address = the LAST argument */
    arguments[0] = THUNK_ARGUMENT_C;
    arguments[1] = THUNK_ARGUMENT_B;
    arguments[2] = THUNK_ARGUMENT_A;
    if (!callback)
        return THUNK_NOT_FOUND;
    if (!callback(callback16, WCB16_PASCAL, sizeof arguments, arguments, &result))
        return THUNK_CALL_FAILED;
    return result;
}

__declspec(dllexport) DWORD WINAPI T_Glob(void)
{
    PWOW_GLOBAL_ALLOC16       globalAlloc      = (PWOW_GLOBAL_ALLOC16)ThunkWow32Procedure("WOWGlobalAlloc16");
    PWOW_GLOBAL_FREE16        globalFree       = (PWOW_GLOBAL_FREE16)ThunkWow32Procedure("WOWGlobalFree16");
    PWOW_GLOBAL_LOCK16        globalLock       = (PWOW_GLOBAL_LOCK16)ThunkWow32Procedure("WOWGlobalLock16");
    PWOW_GLOBAL_UNLOCK16      globalUnlock     = (PWOW_GLOBAL_UNLOCK16)ThunkWow32Procedure("WOWGlobalUnlock16");
    PWOW_GLOBAL_ALLOC_LOCK16  globalAllocLock  = (PWOW_GLOBAL_ALLOC_LOCK16)ThunkWow32Procedure("WOWGlobalAllocLock16");
    PWOW_GLOBAL_UNLOCK_FREE16 globalUnlockFree = (PWOW_GLOBAL_UNLOCK_FREE16)ThunkWow32Procedure("WOWGlobalUnlockFree16");
    PWOW_GLOBAL_LOCK_SIZE16   globalLockSize   = (PWOW_GLOBAL_LOCK_SIZE16)ThunkWow32Procedure("WOWGlobalLockSize16");
    PWOW_GET_VDM_POINTER      getVdmPointer    = (PWOW_GET_VDM_POINTER)ThunkWow32Procedure("WOWGetVDMPointer");
    DWORD answers = 0;
    DWORD pointer16;
    DWORD size = 0;
    WORD handle;
    WORD fixedHandle = 0;
    BYTE *bytes;

    if (!globalAlloc || !globalFree || !globalLock || !globalUnlock || !globalAllocLock || !globalUnlockFree || !globalLockSize || !getVdmPointer)
        return THUNK_NOT_FOUND;
    handle = globalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, THUNK_BLOCK_SIZE);
    if (handle)
        answers |= THUNK_ALLOCATED;
    pointer16 = handle ? globalLock(handle) : 0;
    if (pointer16)
        answers |= THUNK_LOCKED;
    bytes = pointer16 ? (BYTE *)getVdmPointer(pointer16, THUNK_BLOCK_SIZE, TRUE) : NULL;
    if (bytes && bytes[0] == 0 && bytes[THUNK_BLOCK_SIZE - 1] == 0)
        answers |= THUNK_ZEROED;                                                                        /* ZEROINIT honoured */
    if (bytes)
    {
        bytes[0] = THUNK_MARK_FIRST;
        bytes[1] = THUNK_MARK_SECOND;
    }
    if (handle && globalLockSize(handle, &size) == pointer16 && size >= THUNK_BLOCK_SIZE)
        answers |= THUNK_SIZED;                                                                                      /* same block, its size */
    if (bytes && ((BYTE *)getVdmPointer(globalLock(handle), THUNK_MARK_LENGTH, TRUE))[1] == THUNK_MARK_SECOND)
        answers |= THUNK_WRITTEN;
    if (handle) /* three locks taken */
    {
        globalUnlock(handle);
        globalUnlock(handle);
        globalUnlock(handle);
    }
    {   WORD freeResult = handle ? globalFree(handle) : THUNK_NO_HANDLE;
        if (freeResult == 0)
            answers |= THUNK_FREED;                                                   /* GlobalFree: 0 = freed */
        answers |= (DWORD)(freeResult == 0 ? THUNK_ANSWER_ZERO : freeResult == handle ? THUNK_ANSWER_HANDLE : THUNK_ANSWER_OTHER) << THUNK_FREE_CODE_SHIFT;  /* what it said instead */
        if (freeResult != 0 && freeResult != handle)
            answers |= (DWORD)(freeResult & THUNK_LOW_BYTE) << THUNK_FREE_BYTE_SHIFT;                                           /* ...and its low byte */
    }
    pointer16 = globalAllocLock(GMEM_FIXED, THUNK_FIXED_SIZE, &fixedHandle);
    if (pointer16 && fixedHandle)
        answers |= THUNK_ALLOC_LOCKED;
    if (pointer16)
    {
        WORD freeResult = globalUnlockFree(pointer16);
        if (freeResult == 0)
            answers |= THUNK_UNLOCK_FREED;
        answers |= (DWORD)(freeResult == 0 ? THUNK_ANSWER_ZERO : freeResult == fixedHandle ? THUNK_ANSWER_HANDLE
                           : freeResult == (WORD)(pointer16 >> THUNK_SELECTOR_SHIFT) ? THUNK_ANSWER_SELECTOR : THUNK_ANSWER_UNKNOWN) << THUNK_UNLOCK_FREE_CODE_SHIFT;
        if (freeResult != 0 && freeResult != fixedHandle && freeResult != (WORD)(pointer16 >> THUNK_SELECTOR_SHIFT))
            answers |= (DWORD)(freeResult & THUNK_LOW_BYTE) << THUNK_UNLOCK_FREE_BYTE_SHIFT;
    }
    return answers;     /* low byte: a bit per documented answer; bits 8-11: the free codes */
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    (void)module;
    (void)reason;
    (void)reserved;
    return TRUE;
}
