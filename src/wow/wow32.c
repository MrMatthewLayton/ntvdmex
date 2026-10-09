/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The 32-bit half of WOW: krnl386's calls out to Win32.  GH #128.
 *
 * The code of wow32.h (#335): its functions and state, in their original order;
 * its own translation unit, declared in wow32.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "wow32.h"
#include "host_state.h"
#include "log.h"
#include "ne.h"
#include "wowanchors.h"
#include "wowsched.h"
#include "wowcall.h"
#include "wowmsg.h"
#include "wowres.h"
#include "wowwin.h"
#include "wowgdi.h"
#include "wowuser.h"
#include "host_wow.h"

CHAR g_WowCommandProgram[WOW32_COMMAND_PROGRAM_MAX] = { 0 };   /* full path of the Win16 program */
CHAR g_WowCommandArguments[WOW32_COMMAND_ARGUMENTS_MAX] = { 0 };   /* its arguments, without a leading space */
CHAR g_WowCommandDirectory[MAX_PATH] = { 0 }; /* #164: the launch directory, 8.3; "" = none */

INT  g_WowCommandIsTaken     = 0;       /* delivered already -- deliver once */

static const WOW32_DECLINE_SITE g_Wow32DeclineSites[] = {
    { 0xb7,                0x52e5 },
    { WOW32_FILE_SEEK,     0x549e },   /* 0x98 -> AH=42h */
    { 0x77,                0x54d3 },
    { WOW32_FILE_OPEN,     0x5507 },   /* 0xc1 -> AH=3Dh */
    { WOW32_FILE_READ,     0x5573 },   /* 0x97 -> AH=3Fh */
    { WOW32_FILE_CLOSE,    0x5592 },   /* 0xc2 -> AH=3Eh */
    { WOW32_FILE_GETATTR,  0x55c7 },   /* 0xc7 -> AH=43h */
    { WOW32_FILE_7E,       0x55e2 },   /* 0x7e */
    { WOW32_FILE_GETDATE,  0x560c },   /* 0x89 -> AH=57h */
    { 0x76,                0x5634 },
    { 0x71,                0x565e },
    { WOW32_FILE_WRITE,    0x56bf },   /* 0x6f -> AH=40h */
};

/* Forward declarations, from when this file was part of main.c's unit (they were in wow32.h). */
VOID Wow32CurrentDirectorySet(PCSTR dir);  /* #164: main.c's per-task directory table */

/* ---- note building (shared by EVERY id space's dispatcher) --------------
 * These live here rather than in wowuser.h because five module files build
 * notes with them -- USER, SHELL, COMMDLG, KEYBOARD and GDI -- and their old
 * home forced an include order in which wowgdi.h had to come LAST. That order
 * is wrong now that USER's GetDC has to mint a GDI token, so the helpers moved
 * to the header everything already includes instead of the dependency being
 * worked around with forward declarations.
 */
VOID WowNotePut(PSTR buffer, INT capacity, PINT length, PCSTR text)
{
    while (*text && *length < capacity - 1)
        buffer[(*length)++] = *text++;
    buffer[*length] = 0;
}

VOID WowNoteHex(PSTR buffer, INT capacity, PINT length, DWORD value, INT digits)
{
    static const CHAR hexDigits[] = HEX_DIGITS_LOWER;
    INT index;

    for (index = digits - 1; index >= 0; --index)
    {
        if (*length >= capacity - 1)
            break;
        buffer[(*length)++] = hexDigits[(value >> (index * NIBBLE_SHIFT)) & NIBBLE_MASK];
    }
    buffer[*length] = 0;
}

/* A quoted string, with the quotes, truncated rather than dropped. */
VOID WowNoteQuoted(PSTR buffer, INT capacity, PINT length, PCSTR text)
{
    WowNotePut(buffer, capacity, length, "\"");
    WowNotePut(buffer, capacity, length, text);
    WowNotePut(buffer, capacity, length, "\"");
}

WORD Wow32PeekWord(volatile BYTE *bytes)
{
    return (WORD)(bytes[0] | (bytes[1] << BYTE_SHIFT));
}

VOID Wow32PokeWord(volatile BYTE *bytes, WORD value)
{
    bytes[0] = (BYTE)(value & BYTE_MASK);
    bytes[1] = (BYTE)(value >> BYTE_SHIFT);
}

/* Argument WORD at byte offset `off` into the argument block. */
WORD Wow32ArgWord(PCWOW32_FRAME frame, INT offset)
{
    if (offset < 0 || offset + WOW_WORD_BYTES > (INT)frame->ArgumentBytes)
        return 0;
    return Wow32PeekWord(frame->FrameBase + WOW32_OFF_ARGS + offset);
}

/* Argument DWORD at byte offset `off`. */
DWORD Wow32ArgDword(PCWOW32_FRAME frame, INT offset)
{
    return (DWORD)Wow32ArgWord(frame, offset) | ((DWORD)Wow32ArgWord(frame, offset + WOW_WORD_BYTES) << WORD_SHIFT);
}

/* A 16:16 far pointer argument, resolved to a host linear address.
 *
 * [CAUTION]: Returns 0 for a null selector rather than the LDT base, so a caller that
 * forgets to check cannot scribble at the bottom of the address space.
 */
volatile BYTE *Wow32ArgPointer(PCWOW32_FRAME frame, INT offset)
{
    DWORD farPointer = Wow32ArgDword(frame, offset);
    WORD  selector = (WORD)(farPointer >> WORD_SHIFT);
    DWORD base;

    if (!selector || !frame->SelectorToLinear)
        return NULL;
    base = frame->SelectorToLinear(selector, frame->Context);
    if (!base)
        return NULL;
    return (volatile BYTE *)(ULONG_PTR)(base + (farPointer & WORD_MASK));
}

/* Copy a NUL-terminated guest string at a far-pointer argument into a host buffer.
 * Returns 1 if there was a string to copy, 0 for a null/unreadable pointer -- and the
 * difference matters: the profile API gives `lpAppName == NULL` its own meaning
 * ("enumerate"), so "no pointer" must not arrive at Win32 as an empty string.
 */
INT Wow32ArgString(PCWOW32_FRAME frame, INT offset, PSTR output, INT capacity)
{
    volatile BYTE *source = Wow32ArgPointer(frame, offset);
    INT length = 0;

    if (!source)
    {
        if (capacity)
            output[0] = 0;
        return 0;
    }
    while (length < capacity - 1 && source[length])
    {
        output[length] = (CHAR)source[length];
        ++length;
    }
    output[length] = 0;
    return 1;
}

/* Resolve a 16:16 far pointer stored INSIDE a guest structure (rather than in
 * the argument block) to a host address. Same null-selector rule as
 * Wow32ArgPointer: 0 rather than the LDT base, so a missing check cannot scribble
 * at the bottom of the address space.
 */
volatile BYTE *Wow32FarAt(PCWOW32_FRAME frame, volatile BYTE *base, INT offset)
{
    DWORD farPointer  = (DWORD)Wow32PeekWord(base + offset)
              | ((DWORD)Wow32PeekWord(base + offset + WOW_WORD_BYTES) << WORD_SHIFT);
    WORD  selector = (WORD)(farPointer >> WORD_SHIFT);
    DWORD linear;

    if (!selector || !frame->SelectorToLinear)
        return NULL;
    linear = frame->SelectorToLinear(selector, frame->Context);
    if (!linear)
        return NULL;
    return (volatile BYTE *)(ULONG_PTR)(linear + (farPointer & WORD_MASK));
}

/* Copy a host string into a guest buffer described by a POINTER/CAPACITY PAIR,
 * and write the length actually stored back over the capacity.
 *
 * [CAUTION]: THE CAPACITY IS THE GUEST'S CLAIM ABOUT ITS OWN STACK, and it is the only
 * bound there is -- these buffers sit inside the caller's frame, a few bytes
 * below its return address, so overrunning one does not corrupt data, it
 * corrupts control flow. Never write more than the guest declared.
 * Returns the length written, or -1 if there was nowhere to write.
 */
static INT Wow32FarPut(
    PCWOW32_FRAME frame,
    volatile BYTE *base,
    INT pointerOffset,
    INT countOffset,
    PCSTR text)
{
    volatile BYTE *destination = Wow32FarAt(frame, base, pointerOffset);
    WORD capacity = Wow32PeekWord(base + countOffset);
    INT length = 0;

    if (!destination || !capacity)
        return -1;
    while (text[length] && length < (INT)capacity - 1)
    {
        destination[length] = (BYTE)text[length];
        ++length;
    }
    destination[length] = 0;
    Wow32PokeWord(base + countOffset, (WORD)length);
    return length;
}

/* [INFO]: The return value goes in the stack hole, NOT in AX/DX -- see the header note. */
VOID Wow32SetReturn(PWOW32_FRAME frame, DWORD value)
{
    Wow32PokeWord(frame->FrameBase + WOW32_OFF_RET,     (WORD)(value & WORD_MASK));
    Wow32PokeWord(frame->FrameBase + WOW32_OFF_RET + WOW_WORD_BYTES, (WORD)(value >> WORD_SHIFT));
    frame->Result = value;
}

/* [INFO]: What the guest WILL READ out of the return hole if nobody writes it.
 * A stepped-over call leaves the return slot holding whatever the stack last had
 * there, and krnl386 receives it as the call's answer and acts on it -- so an
 * unimplemented call is not inert, it answers at random. Two walls in this project
 * were that value and not the guest: the null-`ES` fault after `WowLoadModule`
 * (a stale value accepted as a module handle) and, upstream of it, a stale
 * non-zero taken as failure inside LoadModule. Reading the slot back and PRINTING
 * it is what lets a later reader tell "krnl386 decided this" from "our litter
 * decided this".
 */
DWORD Wow32PeekReturn(PCWOW32_FRAME frame)
{
    return (DWORD)Wow32PeekWord(frame->FrameBase + WOW32_OFF_RET)
         | ((DWORD)Wow32PeekWord(frame->FrameBase + WOW32_OFF_RET + WOW_WORD_BYTES) << WORD_SHIFT);
}

PCSTR Wow32Name(WORD id)
{
    switch (id)
    {
    case WOW32_FATALEXIT:
        return "FatalExit";

    case WOW32_EXITKERNELTHUNK:
        return "ExitKernelThunk";

    case WOW32_WRITEOUTPROFILES:
        return "WriteOutProfiles";

    case WOW32_GETVDMPOINTER32W:
        return "GetVDMPointer32W";

    case WOW32_CALLPROCEX32W:
        return "CallProcEx32W";

    case WOW32_YIELD:
        return "Yield";

    case WOW32_WAITEVENT:
        return "WaitEvent";

    case WOW32_POSTEVENT:
        return "PostEvent";

    case WOW32_SETPRIORITY:
        return "SetPriority";

    case WOW32_LOCKCURRENTTASK:
        return "LockCurrentTask";

    case WOW32_WOWLOADMODULE:
        return "WowLoadModule";

    case WOW32_WOWGETNEXTVDMCOMMAND:
        return "WowGetNextVDMCommand";

    case WOW32_OLDYIELD:
        return "OldYield";

    case WOW32_REGISTERDOSDATA:
        return "RegisterDosData?";

    case WOW32_GETSHORTPATHNAME:
        return "GetShortPathName";

    case WOW32_ACCEPTTASKSELECTOR:
        return "AcceptTaskSelector?";

    case WOW32_WOWWAITFORMSGANDEVENT:
        return "WowWaitForMsgAndEvent";

    case WOW32_WOWMSGBOX:
        return "WowMsgBox";

    case WOW32_GETDATETIME:
        return "GetDateTime?";

    case WOW32_GETDRIVETYPE:
        return "GetDriveType";

    case WOW32_WOWREGISTERSHELLWINDOW:
        return "WowRegisterShellWindowHandle";

    case WOW32_FREELIBRARY32W:
        return "FreeLibrary32W";

    case WOW32_GETPROCADDRESS32W:
        return "GetProcAddress32W";

    case WOW32_DIRECTEDYIELD:
        return "DirectedYield";

    case WOW32_LOADLIBRARYEX32W:
        return "LoadLibraryEx32W";

    case WOW32_WOWQUERYPERFCOUNTER:
        return "WowQueryPerformanceCounter";

    case WOW32_WOWCURSORICONOP:
        return "WowCursorIconOp";

    case WOW32_WOWFAILEDEXEC:
        return "WowFailedExec";

    case WOW32_WOWCLOSECOMPORT:
        return "WowCloseComPort";

    case WOW32_VIRTUALALLOC:
        return "VirtualAlloc";

    case WOW32_VIRTUALFREE:
        return "VirtualFree";

    case WOW32_GLOBALMEMORYSTATUS:
        return "GlobalMemoryStatus";

    case WOW32_WOWKILLREMOTETASK:
        return "WowKillRemoteTask";

    case WOW32_MESSAGEBOX:
        return "MessageBox?";

    case WOW32_RESOLVEMODULEPATH:
        return "ResolveModulePath";

    case WOW32_WOWSHUTDOWNTIMER:
        return "WowShutdownTimer";

    case WOW32_GETCURDIR:
        return "GetCurrentDirectory";

    case WOW32_GETSYSTEMDEFAULTLANGID:
        return "GetSystemDefaultLangID";

    case WOW32_GETWINDOWSDIRECTORY:
        return "GetWindowsDirectory";

    case WOW32_SETCURRENTDRIVE:
        return "SetCurrentDrive";

    case WOW32_GETPROFILEINT:
        return "GetProfileInt";

    case WOW32_GETPROFILESTRING:
        return "GetProfileString";

    case WOW32_SETCURRENTDIR:
        return "SetCurrentDirectory";

    case WOW32_GETPRIVATEPROFILESTRING:
        return "GetPrivateProfileString";

    case WOW32_GETPRIVATEPROFILEINT:
        return "GetPrivateProfileInt";

    case WOW32_WRITEPROFILESTRING:
        return "WriteProfileString";

    case WOW32_WRITEPRIVATEPROFILESTRING:
        return "WritePrivateProfileString";

    default:
        return NULL;
    }
}

/* WIN16 SEES 8.3 NAMES, AND ONLY 8.3 NAMES. (s73) (Importance = 1):
 * krnl386's loader opens the program through INT 21h, and a Win16 DOS world has no
 * long file names: "C:\Documents and Settings\...\notepad\notepad.EXE" fails at
 * the first space, WowFailedExec fires, and the user reads "Cannot find file ... (or
 * one of its components)" for a file that is right there. Stock WOW hands krnl386
 * the SHORT form (a double-click's AppName arrives that way). Every path we hand
 * the Win16 side -- the launch command, ResolveModulePath's answer -- goes through
 * here. A path with no short form (the API returns 0) is passed as given.
 */
VOID WowShorten(PSTR path, UINT capacity)
{
    CHAR shortPath[WOW32_SHORT_PATH_BUFFER];
    DWORD length = 0;

    if (path[0])
        length = GetShortPathNameA(path, shortPath, sizeof shortPath);
    if (length && length < sizeof shortPath && length < capacity)
    {
        UINT index;
        for (index = 0; index <= length; ++index)
            path[index] = shortPath[index];
    }
}

INT Wow32MayDecline(WORD id, WORD callSite)
{
    UINT index;

    for (index = 0; index < sizeof g_Wow32DeclineSites / sizeof g_Wow32DeclineSites[0]; ++index)
        if (g_Wow32DeclineSites[index].Id == id && g_Wow32DeclineSites[index].CallSite == callSite)
            return 1;
    return 0;
}

static WORD Wow32RawArgWord(PCWOW32_FRAME frame, INT offset)
{
    if (offset < 0 || offset > WOW32_RAW_ARGUMENTS_MAX)
        return 0;
    return Wow32PeekWord(frame->FrameBase + WOW32_OFF_ARGS + offset);
}

static DWORD Wow32RawArgDword(PCWOW32_FRAME frame, INT offset)
{
    return (DWORD)Wow32RawArgWord(frame, offset) | ((DWORD)Wow32RawArgWord(frame, offset + WOW_WORD_BYTES) << WORD_SHIFT);
}

/* A protected-mode 16:16 far pointer -> the host address it names (0 for NULL). */
DWORD Wow32Flat(PCWOW32_FRAME frame, DWORD farPointer)
{
    WORD selector = (WORD)(farPointer >> WORD_SHIFT);
    DWORD base;

    if (!selector || !frame->SelectorToLinear)
        return 0;
    base = frame->SelectorToLinear(selector, frame->Context);
    return base ? base + (farPointer & WORD_MASK) : 0;
}

/* Call a 32-bit function with n DWORDs, a[0] first. ESP is restored by hand, so a
 * STDCALL target (which pops) and a CDECL one (which does not) both come back
 * with the stack where it was.
 */
DWORD WowGenericThunkInvoke(DWORD procedure, PCDWORD arguments, INT count)
{
    DWORD result;

    __asm__ __volatile__ (
        "movl %%esp, %%edi\n\t"
        "movl %3, %%ecx\n\t"
        "1:\n\t"
        "testl %%ecx, %%ecx\n\t"
        "jz 2f\n\t"
        "pushl -4(%2,%%ecx,4)\n\t"
        "decl %%ecx\n\t"
        "jmp 1b\n\t"
        "2:\n\t"
        "call *%1\n\t"
        "movl %%edi, %%esp\n\t"
        : "=&a"(result)
        : "b"(procedure), "S"(arguments), "d"(count)
        : "ecx", "edi", "memory", "cc");
    return result;
}

INT Wow32Call(PWOW32_FRAME frame, PWOW32_DOSDATA dosData)
{
    /* [INFO]: NOT OUR ID SPACE, NOT OUR ANSWER. See `IsKernel` in WOW32_FRAME. */
    if (!frame->IsKernel)
        return 0;
    switch (frame->Id)
    {

    /* 0xb8 VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect):
     * krnl386 services DPMI 0501 ("allocate memory block") with this rather
     * than passing it to the DPMI host: it arrives when a guest issues 0501,
     * and its result comes back to that guest as 0501's address-and-handle
     * pair (BX:CX, SI:DI).
     *
     * [INFO]: A REAL VirtualAlloc IS THE RIGHT ANSWER, not a fake. The guest runs in
     * our own address space, so an address we allocate is one the guest can
     * reach once it puts a descriptor over it -- which is the next thing it
     * does. Handing back a plausible-looking number would fail later, further
     * from the cause.
     */
    /* -- 0x9a LoadLibraryEx32W(lpszLibFile, hFile, dwFlags) = 12, reversed. */
    case WOW32_LOADLIBRARYEX32W:
    {
        CHAR  path[MAX_PATH];
        DWORD flags = Wow32ArgDword(frame, WOW32_LOADLIBRARYEX32W_ARG_FLAGS);
        HMODULE module = NULL;
        if (Wow32ArgString(frame, WOW32_LOADLIBRARYEX32W_ARG_PATH, path, (INT)sizeof path) && path[0])
            module = LoadLibraryExA(path, NULL, flags);
        Wow32SetReturn(frame, (DWORD)(ULONG_PTR)module);
        return 1;
    }

    /* -- 0x8c FreeLibrary32W(hInst32) = 4. */
    case WOW32_FREELIBRARY32W:
    {
        DWORD module = Wow32ArgDword(frame, WOW32_FREELIBRARY32W_ARG_MODULE);
        Wow32SetReturn(frame, module ? (FreeLibrary((HMODULE)(ULONG_PTR)module) ? 1 : 0) : 0);
        return 1;
    }

    /* -- 0x8d GetProcAddress32W(hInst32, lpszProc) = 8: +0 the name (a null
     * selector = an ordinal in the offset), +4 the module.
     */
    case WOW32_GETPROCADDRESS32W:
    {
        DWORD module  = Wow32ArgDword(frame, WOW32_GETPROCADDRESS32W_ARG_MODULE);
        DWORD procedureName = Wow32ArgDword(frame, WOW32_GETPROCADDRESS32W_ARG_NAME);
        CHAR  name[WOW32_PROC_NAME_MAX];
        FARPROC pointer = NULL;
        if (module)
        {
            if (!(procedureName >> WORD_SHIFT)) pointer = GetProcAddress((HMODULE)(ULONG_PTR)module,
                                                (LPCSTR)(ULONG_PTR)(procedureName & WORD_MASK));
            else if (Wow32ArgString(frame, WOW32_GETPROCADDRESS32W_ARG_NAME, name, (INT)sizeof name))
                pointer = GetProcAddress((HMODULE)(ULONG_PTR)module, name);
        }
        Wow32SetReturn(frame, (DWORD)(ULONG_PTR)pointer);
        return 1;
    }

    /* -- 0x1a GetVDMPointer32W(lpAddress, fMode) = 6: +0 fMode (1 = protected
     * mode, 0 = real mode), +2 the 16:16 address. Real-mode memory sits at
     * linear 0 of this process, as in NTVDM.
     */
    case WOW32_GETVDMPOINTER32W:
    {
        WORD  mode = Wow32ArgWord(frame, WOW32_GETVDMPOINTER32W_ARG_MODE);
        DWORD farPointer   = Wow32ArgDword(frame, WOW32_GETVDMPOINTER32W_ARG_POINTER);
        DWORD result;
        if (mode)
            result = Wow32Flat(frame, farPointer);
        else
            result = ((farPointer >> WORD_SHIFT) << PARAGRAPH_SHIFT) + (farPointer & WORD_MASK);
        Wow32SetReturn(frame, result);
        return 1;
    }

    /* -- 0x1c CallProc32W / _CallProcEx32W. See the frame note above Wow32Call. */
    case WOW32_CALLPROCEX32W:
    {
        DWORD countAndFlags   = Wow32RawArgDword(frame, WOW32_CALLPROCEX32W_ARG_COUNT);
        DWORD mask = Wow32RawArgDword(frame, WOW32_CALLPROCEX32W_ARG_MASK);
        DWORD procedureAddress   = Wow32RawArgDword(frame, WOW32_CALLPROCEX32W_ARG_PROCEDURE);
        INT   isExtended   = (countAndFlags & WOW32_CALLPROCEX32W_EXTENDED) != 0;      /* krnl386's own mark: 518 */
        INT   number    = (INT)(countAndFlags & WORD_MASK);
        DWORD arguments[WOW_GT_MAX_PARAMETERS];
        INT   index;
        if (!procedureAddress || number < 0 || number > WOW_GT_MAX_PARAMETERS)
        {
            Wow32SetReturn(frame, 0);
            return 1;
        }
        for (index = 0; index < number; ++index)
        {
            /* a[i] = parameter i+1. Pascal: p(i+1) sits (n-1-i) DWORDs above +18. */
            INT   slot = isExtended ? index : (number - 1 - index);
            DWORD value    = Wow32RawArgDword(frame, WOW32_CALLPROCEX32W_ARG_FIRST + WOW32_CALLPROCEX32W_ARG_SIZE * slot);
            /* MASK, MEASURED (w_gthunk vs stock): bit 0 = the LAST parameter for
             * CallProc32W and the FIRST for _CallProcEx32W -- i.e. bit 0 is always
             * the parameter nearest the top of the 16-bit stack.
             */
            INT   bit  = isExtended ? index : number - 1 - index;
            if (mask & (1u << bit))
                value = Wow32Flat(frame, value);
            arguments[index] = value;
        }
        Wow32SetReturn(frame, WowGenericThunkInvoke(procedureAddress, arguments, number));
        return 1;
    }

    case WOW32_VIRTUALALLOC:
    {
        DWORD address  = Wow32ArgDword(frame, WOW32_VIRTUALALLOC_ARG_ADDRESS);
        DWORD size  = Wow32ArgDword(frame, WOW32_VIRTUALALLOC_ARG_SIZE);
        DWORD type  = Wow32ArgDword(frame, WOW32_VIRTUALALLOC_ARG_TYPE);
        DWORD protection  = Wow32ArgDword(frame, WOW32_VIRTUALALLOC_ARG_PROTECT);
        PVOID pointer = VirtualAlloc((LPVOID)(ULONG_PTR)address, size, type, protection);
        Wow32SetReturn(frame, (DWORD)(ULONG_PTR)pointer);
        return 1;
    }

    /* 0xb9 VirtualFree(lpAddress, dwSize, dwFreeType): */
    case WOW32_VIRTUALFREE:
    {
        DWORD address = Wow32ArgDword(frame, WOW32_VIRTUALFREE_ARG_ADDRESS);
        DWORD size = Wow32ArgDword(frame, WOW32_VIRTUALFREE_ARG_SIZE);
        DWORD type = Wow32ArgDword(frame, WOW32_VIRTUALFREE_ARG_TYPE);
        BOOL isOk = address ? VirtualFree((LPVOID)(ULONG_PTR)address, size, type) : FALSE;
        Wow32SetReturn(frame, (DWORD)isOk);
        return 1;
    }

    /* 0xbc GlobalMemoryStatus(LPMEMORYSTATUS):
     * The one argument is a 16:16 pointer to a 32-byte buffer whose dwLength
     * the guest has already set to 0x20. Fill it through the guest's own
     * selector -- we must not hand back a host pointer, because the guest
     * reads the fields out of its own stack buffer.
     */
    case WOW32_GLOBALMEMORYSTATUS:
    {
        volatile BYTE *destination = Wow32ArgPointer(frame, WOW32_GLOBALMEMORYSTATUS_ARG_BUFFER);
        if (destination)
        {
            MEMORYSTATUS memoryStatus;
            UINT byteIndex;
            PCBYTE source = (PCBYTE)&memoryStatus;
            memoryStatus.dwLength = sizeof(memoryStatus);
            GlobalMemoryStatus(&memoryStatus);
            for (byteIndex = 0; byteIndex < sizeof(memoryStatus); ++byteIndex)
                destination[byteIndex] = source[byteIndex];
        }
        Wow32SetReturn(frame, 0);            /* void -- but the slot is popped anyway */
        return 1;
    }

    /* 0xcf GetSystemDefaultLangID():
     * Named by krnl386's export table. What it is for is "am I on a DBCS
     * system" -- the Far-East LANGIDs 0x411/0x412/0x404/0x804/0x0c04. Answering
     * with the host's real LANGID is both correct and the whole point: a
     * Japanese XP should make krnl386 take the DBCS path.
     */
    case WOW32_GETSYSTEMDEFAULTLANGID:
        Wow32SetReturn(frame, (DWORD)GetSystemDefaultLangID());
        return 1;

    /* 0x7b GetShortPathName(lpszLong, lpszShort, cch). (s89, #270) (Importance = 2):
     * Named, never answered. At start-up krnl386 calls this with the value of
     * `SYSTEMROOT=` from its environment and a 0x79-byte buffer (seen in the
     * run log), and the system directory it later reports is the returned
     * length's worth of that answer + "\SYSTEM". Stepped over, the length was
     * 0 and KERNEL.135 GetSystemDirectory answered "\SYSTEM" -- 7 characters,
     * no drive (the Win16 test `kfile.sysdir.*`). Frame, reversed: +0 cch,
     * +2 lpszShort far, +6 lpszLong far. Written only if it fits, as Win32
     * does; otherwise 0, so krnl386 never copies a length it was not given.
     */
    case WOW32_GETSHORTPATHNAME:
    {
        volatile BYTE *destination = Wow32ArgPointer(frame, WOW32_GETSHORTPATHNAME_ARG_BUFFER);
        WORD capacity = Wow32ArgWord(frame, WOW32_GETSHORTPATHNAME_ARG_CAPACITY);
        CHAR source[MAX_PATH];
        CHAR output[MAX_PATH];
        DWORD number;
        if (!destination || !capacity || !Wow32ArgString(frame, WOW32_GETSHORTPATHNAME_ARG_PATH, source, sizeof source) || !source[0])
        {
            Wow32SetReturn(frame, 0);
            return 1;
        }
        number = GetShortPathNameA(source, output, sizeof output);
        if (!number || number >= sizeof output || number + 1 > (DWORD)capacity)
        {
            Wow32SetReturn(frame, 0);
            return 1;
        }
        {
            DWORD byteIndex;
            for (byteIndex = 0; byteIndex <= number; ++byteIndex)
                destination[byteIndex] = (BYTE)output[byteIndex];
        }
        Wow32SetReturn(frame, number);
        return 1;
    }

    /* -- 0xd0 GetWindowsDirectory(lpBuffer, uSize) -- see the note above.
     * The real Win32 call against the real directory, for the same reason
     * GetDriveType is a pass-through: our DOS layer opens real paths on the
     * real filesystem, so the host's Windows directory IS the guest's.
     *
     * [CAUTION]: uSize is the GUEST'S claim about its own buffer and the only bound
     * there is -- krnl386 declares 0x80 bytes. Never write more
     * than it declared.
     */
    case WOW32_GETWINDOWSDIRECTORY:
    {
        volatile BYTE *destination = Wow32ArgPointer(frame, WOW32_GETWINDOWSDIRECTORY_ARG_BUFFER);
        WORD capacity = Wow32ArgWord(frame, WOW32_GETWINDOWSDIRECTORY_ARG_CAPACITY);
        CHAR directory[MAX_PATH];
        UINT number;
        if (!destination || !capacity)
        {
            Wow32SetReturn(frame, 0);
            return 1;
        }
        number = GetWindowsDirectoryA(directory, sizeof directory);
        if (!number || number >= sizeof directory || number + 1 > (UINT)capacity)
        {
            Wow32SetReturn(frame, 0);
            return 1;
        }
        {
            UINT byteIndex;
            for (byteIndex = 0; byteIndex <= number; ++byteIndex)
                destination[byteIndex] = (BYTE)directory[byteIndex];
        }
        Wow32SetReturn(frame, number);
        return 1;
    }

    /* 0x80 GetPrivateProfileString, AND IT IS THE PROGRAM LAUNCH (Importance = 3):
     * Neither this ID nor 0x39 is self-named by krnl386's export table, so both
     * were pinned from the strings their arguments point at, as they arrive --
     * which turns out to name them outright. The first call at start-up:
     *
     *   lpAppName        "BOOT"
     *   lpKeyName        "WOWSHELL"
     *   lpDefault        "WOWEXEC.EXE"
     *   lpReturnedString an empty 0x50-byte buffer
     *   nSize            0x50
     *   lpFileName       "SYSTEM.INI"      ; 22 arg bytes = 4+4+4+4+2+4  OK
     *
     * and the very next thing krnl386 does is LoadModule the string we put in
     * that buffer. So this call is krnl386 asking **what Win16 program to
     * run**, and answering it is the launch itself.
     *
     * [INFO]: A second call has the same signature over `[DEBUG] OUTPUTTO` with an
     * empty default. Two independent calls agreeing on six arguments, with the
     * documented Win16 GetPrivateProfileString shape, is what makes this a
     * reading rather than a guess.
     *
     * [CAUTION]: Arguments are Pascal order: FIRST pushed is the HIGHEST offset, so
     * lpFileName (pushed last) is at 0, lpAppName at 18.
     *
     * [INFO]: Answer it with the REAL Win32 call against the REAL file. On this rig
     * `C:\WINDOWS\SYSTEM.INI` has no `[boot]` section at all (measured), so the
     * default is what comes back -- `WOWEXEC.EXE`, which is present. That is
     * the right answer for the right reason, and a box that DOES set WOWSHELL
     * gets its own shell rather than ours. A bare filename resolves against the
     * Windows directory, which is exactly the 16-bit convention.
     */
    case WOW32_GETPRIVATEPROFILESTRING:
    {
        CHAR application[WOW32_PROFILE_NAME_MAX];
        CHAR key[WOW32_PROFILE_NAME_MAX];
        CHAR defaultValue[MAX_PATH];
        CHAR fileName[MAX_PATH];
        CHAR buffer[WOW32_PROFILE_VALUE_MAX];
        volatile BYTE *destination = Wow32ArgPointer(frame, WOW32_GETPRIVATEPROFILESTRING_ARG_BUFFER);
        WORD   number   = Wow32ArgWord(frame, WOW32_GETPRIVATEPROFILESTRING_ARG_SIZE);
        INT    hasApplication  = Wow32ArgString(frame, WOW32_GETPRIVATEPROFILESTRING_ARG_APP, application,  sizeof application);
        INT    hasKey  = Wow32ArgString(frame, WOW32_GETPRIVATEPROFILESTRING_ARG_KEY, key,  sizeof key);
        DWORD  returned;
        UINT byteIndex;
        /* [CAUTION]: Same trap as 0x39 next door: `hd ? def : ""` would hand a READ-ONLY
         * literal to a call that writes to its arguments. NULL is documented and
         * safe for the two names; the default has to be a writable buffer.
         */
        if (!Wow32ArgString(frame, WOW32_GETPRIVATEPROFILESTRING_ARG_DEFAULT, defaultValue, sizeof defaultValue))
            defaultValue[0] = 0;
        Wow32ArgString(frame, WOW32_GETPRIVATEPROFILESTRING_ARG_FILE, fileName, sizeof fileName);
        if (number > sizeof buffer)
            number = sizeof buffer;
        returned = GetPrivateProfileStringA(hasApplication ? application : NULL, hasKey ? key : NULL,
                                       defaultValue, buffer, number, fileName);
        if (destination)
            for (byteIndex = 0; byteIndex <= returned && byteIndex < number; ++byteIndex)
                destination[byteIndex] = (BYTE)buffer[byteIndex];
        Wow32SetReturn(frame, returned);
        return 1;
    }

    /* Answered with the REAL Win32 call against the REAL file, like 0x80 above.
     *
     * [CAUTION]: nDefault IS THE WHOLE POINT: it is what the guest gets when the key is
     * absent, and it is what this call answered with 0 instead of. Passing it
     * through means a missing winmine.ini gives Minesweeper its OWN defaults,
     * which is what it would get on real Windows.
     *
     * [CAUTION]: `hd ? app : NULL` -- NULL is documented for the two names; never a
     * string literal, which is read-only and XP's profile code WRITES to these
     * buffers (session 38, and it killed the host).
     */
    case WOW32_GETPRIVATEPROFILEINT:
    {
        CHAR application[WOW32_PROFILE_NAME_MAX];
        CHAR key[WOW32_PROFILE_NAME_MAX];
        CHAR fileName[MAX_PATH];
        INT  hasApplication = Wow32ArgString(frame, WOW32_GETPRIVATEPROFILEINT_ARG_APP, application, sizeof application);
        INT  hasKey = Wow32ArgString(frame, WOW32_GETPRIVATEPROFILEINT_ARG_KEY, key, sizeof key);
        WORD defaultValue = Wow32ArgWord(frame, WOW32_GETPRIVATEPROFILEINT_ARG_DEFAULT);
        UINT returned;
        Wow32ArgString(frame, WOW32_GETPRIVATEPROFILEINT_ARG_FILE, fileName, sizeof fileName);
        returned = GetPrivateProfileIntA(hasApplication ? application : NULL, hasKey ? key : NULL,
                                    (INT)defaultValue, fileName);
        /* Win16 returns a UINT; the value is a WORD on the guest's side. */
        Wow32SetReturn(frame, (DWORD)(WORD)returned);
        return 1;
    }

    /* 0x39 GetProfileInt(lpAppName, lpKeyName, nDefault):
     * 10 arg bytes = 4 + 4 + 2, no filename -- so it is the SYSTEM profile
     * twin of 0x80 rather than a private one. The calls seen at run time read
     * as that, by their strings:
     *   ("KERNEL", "GPCONTINUE", <a default>)
     *   ("ModuleCompatibility", <the module's own name>, 0)
     * -- the second one per module just loaded, which is exactly what that
     * section is for.
     *
     * [CAUTION]: WHICH FILE IS UNPROVEN. Win16 `GetProfileInt` means WIN.INI, and
     * `[ModuleCompatibility]` is conventionally a SYSTEM.INI section; the two
     * readings are not distinguishable from krnl386's side because the call
     * carries no filename. It is recorded rather than hidden, and it costs
     * nothing today: this rig's SYSTEM.INI and WIN.INI have NEITHER section
     * (measured), so both readings return the caller's default. Revisit if a
     * module ever needs a compatibility flag.
     */
    /* [CAUTION]: NEVER HAND A STRING LITERAL TO THESE. (session 38) This read
     * `GetProfileIntA(ha ? app : "", hk ? key : "", def)`, and the `""` is in
     * .rdata -- a READ-ONLY page. XP's profile code writes to the name buffers
     * it is given, so the moment krnl386 asked with an empty section or key the
     * host died: `0xc0000005` in ntdll, a one-byte write to the literal's own
     * address. It survived ten calls in every earlier run
     * because every one of them had both names non-empty; the task scheduler
     * simply let the guest get as far as asking with one missing.
     *
     * The locals are writable and already the right size, so pass them always
     * and make "absent" an empty *buffer* rather than an empty literal.
     */
    /* 0xc8 -- AND IT IS WHY krnl386 THINKS IT IS ON DRIVE A (Importance = 4):
     * Named from when it arrives, not from an export table: a Win16 guest's
     * INT 21h AH=0Eh (SELECT DEFAULT DRIVE) for a drive other than the current
     * one produces this call with the requested drive, and the guest then sees
     * AL=26 drives, as DOS returns. And WHATEVER WE RETURN BECOMES krnl386's
     * ANSWER TO "what drive am I on": krnl386 keeps it as its cached current
     * drive.
     *
     * [CAUTION]: UNIMPLEMENTED, THIS RETURNED THE HARNESS SENTINEL 0 -- and 0 is a
     * perfectly good drive index, so krnl386 cached "the current drive is A:"
     * after every select. Measured on TERMINAL, which walks drives 0x19 down
     * to 0 and so calls this 25 times in one startup; WRITE does the same. It
     * is [[stepped-over-call-answers-at-random]] again: not a crash, not a log
     * line, just a number that means something.
     *
     * [CAUTION]: AND WE DO NOT SUPPORT CHANGING DRIVES. INT 21h AH=0Eh accepts a select
     * and ignores it, so returning the REQUESTED drive would claim a switch
     * that did not happen -- and the guest's very next AH=19h would contradict
     * it. Answer the one drive this machine is on, from the constant all three
     * routes now share. When a real per-process current drive exists, this
     * changes with it and not before.
     */
    /* -- 0x84 WowMsgBox -- THE ONE THAT MAKES A DEAD GUEST SAY WHY.
     * (session 56) --------------------------------------------------------
     * This is krnl386's own error reporter: when a launch fails it calls
     * WowFailedExec (0x9d) and then this, with the text, and then
     * ExitKernelThunk. Unimplemented, all three were stepped over -- so a
     * guest that could not start simply VANISHED, with the reason sitting in
     * a string nobody displayed.
     *
     * [INFO]: MEASURED on WINFILE, which is exactly this shape:
     *     FUNC=0x9d WowFailedExec        -> UNIMPLEMENTED
     *     FUNC=0x84 WowMsgBox  arg = "Can't run 16-bit Windows program"
     *     FUNC=0x02 ExitKernelThunk      -> shutting the VDM down
     * Its real cause is an ASSET gap, not a host defect: WINFILE.EXE's module
     * table is [VER, KERNEL, GDI, USER, KEYBOARD, COMMDLG, SHELL, SCONFIG,
     * COMMCTRL] -- it is the Windows for Workgroups build and imports
     * SCONFIG.DLL, which is not on the box (`open "SCONFIG.DLL" -> CF=1
     * gle=2`, and it is the ONE module the resolver could not give a path).
     * That replaces the recorded blocker for this guest, which named
     * ShellExecute and CreateWindowEx and had been stale since both were
     * implemented later in s55.
     *
     * THE GENERAL WIN IS NOT WINFILE. It is that this is the fifth time a
     * Win16 guest has diagnosed itself in English and the sixth is now free:
     * [[wow-real-hwnd-frontier]]'s standing advice is `implement MessageBox
     * FIRST on any new guest`, and krnl386's own message box is the one that
     * covers every guest that dies BEFORE it can put up its own.
     *
     * [CAUTION]: THE OFFSETS ARE READ OFF A RUN, not from a signature. The harness's own
     * `arg[2]` line resolves the text, and its `k` is a WORD index while
     * Wow32ArgDword takes a BYTE offset -- hence 4, not 2. `wType` at 0 was
     * 0x0030 = MB_ICONEXCLAMATION|MB_OK, which is what krnl386 would use for
     * a launch failure and is the reading that makes the other fields line up.
     * The caption slot is tried and falls back, because a wrong caption is
     * cosmetic and a missing message is not.
     *
     * [CAUTION]: IT IS MODAL, deliberately. The guest is being told its program cannot
     * run and stock does exactly this; a headless run that used to end
     * silently now ends with a box on screen saying why, which is strictly
     * more information for the same outcome.
     */
    case WOW32_WOWMSGBOX:
    {
        CHAR captionText[WOW32_MSGBOX_TEXT_MAX];
        CHAR bodyText[WOW32_MSGBOX_TEXT_MAX];
        CHAR logLine[WOW32_MSGBOX_LOG_MAX];
        PCSTR body;
        PCSTR caption;
        WORD type = Wow32ArgWord(frame, WOW32_WOWMSGBOX_ARG_TYPE);
        INT  number = 0;
        if (!Wow32ArgString(frame, WOW32_WOWMSGBOX_ARG_CAPTION, captionText, sizeof captionText))
            captionText[0] = 0;
        if (!Wow32ArgString(frame, WOW32_WOWMSGBOX_ARG_TEXT, bodyText, sizeof bodyText))
            bodyText[0] = 0;
        /* [INFO]: THE SLOTS ARE PINNED, and by data rather than by a signature --
         * the log line below was added first precisely so they could be. One
         * WINFILE run prints BOTH, and they are unambiguous:
         *     arg4 = "Can't run 16-bit Windows program"
         *     arg8 = "Cannot find file C:\WIN16\WINFILE.EXE (or one of its
         *             components). Check to ensure the path and filename are
         *             correct and that all required libraries are available"
         * The short one is the CAPTION and the long one is the TEXT.
         *
         * [CAUTION]: MY FIRST CUT GUESSED THE OTHER WAY ROUND -- "take whichever slot has
         * text as the body" -- and put the explanation in the title bar and
         * the summary in the body. It looked plausible and was backwards; the
         * instrument added one step earlier is what showed it. Keep both slots
         * on the log line so this stays checkable rather than remembered.
         */
        caption = captionText[0] ? captionText : "NTVDMEX -- 16-bit Windows";
        body = bodyText[0] ? bodyText : (captionText[0] ? captionText : "(krnl386 reported an error with no text)");
        /* RECORD BEFORE BLOCKING (Importance = 2):
         * MessageBoxA does not return until a human clicks, and this arm's
         * log block is not flushed until it does -- so the FIRST cut put the
         * box on screen and left NOTHING in the log, including the harness's
         * own arg lines. An instrument that blocks before it records is not an
         * instrument. Write the line here, then show the box.
         */
        {   PCSTR prefix = "  WOWMSGBOX: krnl386 is reporting an error to the "
                              "user. type=0x";
            PCSTR hexDigits = HEX_DIGITS_LOWER;
            PCSTR cursor;
            INT position;
            for (cursor = prefix; *cursor && number < (INT)sizeof logLine - WOW32_MSGBOX_LOG_LINE_END; ++cursor)
                logLine[number++] = *cursor;
            logLine[number++] = hexDigits[(type >> (3 * NIBBLE_SHIFT)) & NIBBLE_MASK];
            logLine[number++] = hexDigits[(type >> (2 * NIBBLE_SHIFT)) & NIBBLE_MASK];
            logLine[number++] = hexDigits[(type >> NIBBLE_SHIFT) & NIBBLE_MASK];
            logLine[number++] = hexDigits[type & NIBBLE_MASK];
            for (cursor = " arg4=\""; *cursor; ++cursor)
                logLine[number++] = *cursor;
            for (position = 0; captionText[position] && number < (INT)sizeof logLine - WOW32_MSGBOX_LOG_HEAD_ROOM; ++position)
                logLine[number++] = (captionText[position] == '\r' || captionText[position] == '\n') ? ' ' : captionText[position];
            for (cursor = "\" arg8=\""; *cursor; ++cursor)
                logLine[number++] = *cursor;
            for (position = 0; bodyText[position] && number < (INT)sizeof logLine - WOW32_MSGBOX_LOG_TAIL_ROOM; ++position)
                logLine[number++] = (bodyText[position] == '\r' || bodyText[position] == '\n') ? ' ' : bodyText[position];
            for (cursor = "\"\r\n"; *cursor; ++cursor)
                logLine[number++] = *cursor;
            LogAppend(LOG_PATH, logLine, logLine + number);
        }
        MessageBoxA(NULL, body, caption,
                    (UINT)((type & WOW32_MSGBOX_ICON_MASK) | MB_OK | MB_SETFOREGROUND));
        Wow32SetReturn(frame, 1);                       /* IDOK */
        return 1;
    }

    case WOW32_SETCURRENTDRIVE:
        Wow32SetReturn(frame, (DWORD)DOS_CURRENT_DRIVE);
        return 1;

    case WOW32_GETPROFILEINT:
    {
        CHAR application[WOW32_PROFILE_NAME_MAX];
        CHAR key[WOW32_PROFILE_NAME_MAX];
        WORD defaultValue;
        if (!Wow32ArgString(frame, WOW32_GETPROFILEINT_ARG_APP, application, sizeof application))
            application[0] = 0;
        if (!Wow32ArgString(frame, WOW32_GETPROFILEINT_ARG_KEY, key, sizeof key))
            key[0] = 0;
        defaultValue = Wow32ArgWord(frame, WOW32_GETPROFILEINT_ARG_DEFAULT);
        Wow32SetReturn(frame, (DWORD)GetProfileIntA(application, key, (INT)defaultValue));
        return 1;
    }

    /* -- 0x3a GetProfileString -- AND IT IS WHY MS PAINT WAS BLACK AND
     * WHITE. ----------------------------------------------------------------
     * The string twin of 0x39, 18 argument bytes = 4 + 4 + 4 + 4 + 2 and no
     * filename, so it is WIN.INI. Both the layout and the identity come from
     * one logged call (PBRUSH.EXE's import of KERNEL.58) rather than from
     * arithmetic on the id:
     *
     *   args as logged   (0009 | 6f0a 09c7 | 012a 09c7 | 090e 09c7 |
     *                                                    08f4 09c7)
     *   the strings: "Paintbrush", "clear", default "COLOR", nSize = 9
     *
     * nSize@0, lpReturnedString@2, lpDefault@6, lpKeyName@10, lpAppName@14.
     *
     * [INFO]: WHY IT MATTERS. Paint's `[Paintbrush] clear=` setting chooses between a
     * colour and a monochrome palette: "COLOR" (the default) means colour,
     * anything else -- including an empty answer -- means 28 greys. Every grey
     * brush a run has ever logged -- 0x00090909, 0x00121212, 0x00212121 -- is
     * one of those greys (the colour palette's entries as luminances), so Paint
     * was never losing colour in a blit. **It was told to be monochrome, by us,
     * by answering 0.** Answering with the default turned the palette to
     * colour on the next run.
     *
     * [CAUTION]: THE DEFAULT IS "COLOR", so a rig with no `[Paintbrush] clear` key gets
     * colour -- which is why searching WIN.INI for a colour key found nothing
     * and the key still turned out to be the answer. The bug was never in the
     * profile, it was that an unimplemented call cannot return a default.
     *
     * [CAUTION]: Same read-only-literal trap as its two neighbours: XP's profile code
     * writes into the name buffers it is given, so the locals are passed
     * always and "absent" is an empty *buffer*.
     *
     * [CAUTION]: Win32 truncates to nSize-1 and returns the character count without the
     * NUL, which is Win16's own convention -- and "COLOR" is 5.
     */
    case WOW32_GETPROFILESTRING:
    {
        CHAR application[WOW32_PROFILE_NAME_MAX];
        CHAR key[WOW32_PROFILE_NAME_MAX];
        CHAR defaultValue[MAX_PATH];
        CHAR buffer[WOW32_PROFILE_VALUE_MAX];
        volatile BYTE *destination = Wow32ArgPointer(frame, WOW32_GETPROFILESTRING_ARG_BUFFER);
        WORD  number = Wow32ArgWord(frame, WOW32_GETPROFILESTRING_ARG_SIZE);
        DWORD returned;
        UINT index;
        if (!Wow32ArgString(frame, WOW32_GETPROFILESTRING_ARG_APP, application, sizeof application))
            application[0] = 0;
        if (!Wow32ArgString(frame, WOW32_GETPROFILESTRING_ARG_KEY, key, sizeof key))
            key[0] = 0;
        if (!Wow32ArgString(frame, WOW32_GETPROFILESTRING_ARG_DEFAULT, defaultValue, sizeof defaultValue))
            defaultValue[0] = 0;
        if (number > sizeof buffer)
            number = (WORD)sizeof buffer;
        if (!number)
        {
            Wow32SetReturn(frame, 0);
            return 1;
        }
        returned = GetProfileStringA(application, key, defaultValue, buffer, number);
        if (destination)
            for (index = 0; index <= returned && index < number; ++index)
                destination[index] = (BYTE)buffer[index];
        Wow32SetReturn(frame, returned);
        return 1;
    }

    /* -- #293: THE WRITE HALF. 0x3b WriteProfileString(lpAppName, lpKeyName,
     * lpString) and 0x81 WritePrivateProfileString(..., lpFileName). ---------
     * Found by the s89 inventory (tools/ne/wowinventory.py), not by a run: 14
     * shelf programs import one or the other -- Calc's Scientific mode, Clock's
     * analog/digital, WinMine's best times, Solitaire's options -- and every
     * one was stepped over, so no Win16 program ever saved a setting. Nothing
     * looked broken, because the next launch simply read the defaults back.
     * 12 and 16 arg bytes = three and four far pointers, Pascal order like the
     * Get* twins above: FIRST pushed is the HIGHEST offset, so the file (if
     * any) is at 0, lpString next, lpAppName last.
     *
     * [INFO]: NULL HAS MEANING, so `Wow32ArgString`'s "no pointer" is passed as NULL:
     * a NULL lpString deletes the key, a NULL lpKeyName deletes the section,
     * and WritePrivateProfileString(NULL, NULL, NULL, file) flushes. Never a
     * string literal in their place (the read-only trap documented at 0x39).
     *
     * [INFO]: The REAL Win32 call against the REAL file, so XP's own IniFileMapping
     * applies exactly as it does for stock WOW, and a bare filename resolves
     * against the Windows directory -- the 16-bit convention.
     * Win16 returns BOOL in AX.
     */
    case WOW32_WRITEPROFILESTRING:
    {
        CHAR application[WOW32_PROFILE_WRITE_NAME_MAX];
        CHAR key[WOW32_PROFILE_WRITE_NAME_MAX];
        CHAR value[WOW32_PROFILE_WRITE_VALUE_MAX];
        INT  hasApplication = Wow32ArgString(frame, WOW32_WRITEPROFILESTRING_ARG_APP, application, sizeof application);
        INT  hasKey = Wow32ArgString(frame, WOW32_WRITEPROFILESTRING_ARG_KEY, key, sizeof key);
        INT  hasValue = Wow32ArgString(frame, WOW32_WRITEPROFILESTRING_ARG_VALUE, value, sizeof value);
        BOOL isOk = WriteProfileStringA(hasApplication ? application : NULL, hasKey ? key : NULL,
                                      hasValue ? value : NULL);
        Wow32SetReturn(frame, isOk ? 1 : 0);
        return 1;
    }

    case WOW32_WRITEPRIVATEPROFILESTRING:
    {
        CHAR application[WOW32_PROFILE_WRITE_NAME_MAX];
        CHAR key[WOW32_PROFILE_WRITE_NAME_MAX];
        CHAR value[WOW32_PROFILE_WRITE_VALUE_MAX];
        CHAR fileName[MAX_PATH];
        INT  hasApplication = Wow32ArgString(frame, WOW32_WRITEPRIVATEPROFILESTRING_ARG_APP, application, sizeof application);
        INT  hasKey = Wow32ArgString(frame, WOW32_WRITEPRIVATEPROFILESTRING_ARG_KEY,  key, sizeof key);
        INT  hasValue = Wow32ArgString(frame, WOW32_WRITEPRIVATEPROFILESTRING_ARG_VALUE,  value, sizeof value);
        BOOL isOk;
        if (!Wow32ArgString(frame, WOW32_WRITEPRIVATEPROFILESTRING_ARG_FILE, fileName, sizeof fileName) || !fileName[0])
        {
            Wow32SetReturn(frame, 0);                  /* no file: nothing to write to */
            return 1;
        }
        isOk = WritePrivateProfileStringA(hasApplication ? application : NULL, hasKey ? key : NULL,
                                        hasValue ? value : NULL, fileName);
        Wow32SetReturn(frame, isOk ? 1 : 0);
        return 1;
    }

    /* 0x03 WriteOutProfiles(): Win16's "flush the profile cache". Win32 flushes
     * WIN.INI with an all-NULL write; there is no return value.
     */
    case WOW32_WRITEOUTPROFILES:
        WriteProfileStringA(NULL, NULL, NULL);
        Wow32SetReturn(frame, 0);
        return 1;

    /* -- 0x82 SetCurrentDirectory -- AND IT IS WHERE `Save As` PUT THE
     * FILE. (session 49) --------------------------------------------------
     * With the LDT collision and OLESVR's null pointer fixed, MS Paint's save
     * RAN -- the log shows the whole .BMP being written: six `AH=40h` writes of
     * 0xF000 and 0xD7A0 bytes, a seek to end reporting **0x004AE7D6**
     * (4,908,502 = 1680x974x3 + headers), a seek back to 0, then 14 bytes and
     * 40 bytes (the BITMAPFILEHEADER and BITMAPINFOHEADER) and a close.
     * **It just wrote it in the wrong directory** -- `C:\Documents and
     * Settings\Matthew\TEST.BMP`, one level above the Desktop the user chose.
     *
     * Three calls of this id explain it, and the third names itself:
     *
     *   FUNC=0x82  arg = "C:\WINDOWS"
     *   FUNC=0x82  arg = "C:\DOCUME~1\Matthew\Desktop"
     *     -> UNIMPLEMENTED, STEPPED OVER ... ANSWERED 0
     *
     * [CAUTION]: AND THE SENTINEL IS "SUCCESS" HERE, WHICH IS WHY IT WAS SILENT. Only
     * DX=0xFFFF is an error to krnl386; our 0 is success, so it tells the
     * application the directory changed. It never did, so the subsequent
     * create resolved against the old current directory. **A stepped-over call
     * that answers "yes" is worse than one that answers "no".** Same family as
     * [[stepped-over-call-answers-at-random]].
     *
     * [CAUTION]: IT IS NOT DECLINABLE, and this file already said so before the bug:
     * 0x82 is among the three sites where 0xFFFF is a plain ERROR krnl386
     * reports to the app rather than chaining to DOS.
     * Declining was tried anyway in session 47 and moved the fault rather than
     * fixing it. So it is PERFORMED here.
     *
     * [INFO]: And the host's own current directory is the right place to perform it:
     * our DOS layer resolves relative paths against it (`SetCurrentDirectoryA
     * (g_cur)` in main.c), so one CWD serves the DOS side and this.
     *
     * [CAUTION]: A genuine failure returns 0xFFFFFFFF -- which at THIS site means "error",
     * and an error is the true answer when the directory does not exist.
     */
    case WOW32_SETCURRENTDIR:
    {
        CHAR directory[MAX_PATH];
        if (!Wow32ArgString(frame, WOW32_SETCURRENTDIR_ARG_PATH, directory, sizeof directory) || !directory[0])
        {
            Wow32SetReturn(frame, WOW32_RETURN_FAILED);
            return 1;
        }
        if (SetCurrentDirectoryA(directory))
        {
            Wow32CurrentDirectorySet(directory);              /* #164: per task, see main.c */
            Wow32SetReturn(frame, 0);
        }
        else
            Wow32SetReturn(frame, WOW32_RETURN_FAILED);
        return 1;
    }

    /* 0x88 GetDriveType(nDrive):
     * Named by krnl386's export table, and the Win16 GetDriveType it backs
     * uses the same codes as Win32 (documented: 2 = DRIVE_REMOVABLE).
     * So this is a straight pass-through of the Win32 call, not a WOW-private
     * encoding, and the host's answer is the guest's answer: our DOS layer
     * opens real paths on the real filesystem, so its drives ARE these drives.
     *
     * [INFO]: MEASURED, not assumed: krnl386 calls it 26 times in a row with nDrive
     * 0x00..0x19 -- A: through Z: -- which is what fixes 0 = A: rather than
     * 0 = "the default drive". Every one of those was previously stepped over
     * and answered with the harness sentinel, and a drive table built from 26
     * identical answers is what fed 0xf0 to the GetCurrentDirectory that
     * followed and #GP'd the run.
     *
     * [CAUTION]: GetDriveTypeA wants a ROOT PATH ("A:\"), not a letter.
     */
    /* -- 0x86 GetDateTime -- AND IT IS WHY THE CLOCK'S FACE IS BLANK.
     * (session 54) ----------------------------------------------------------
     * CLOCK.EXE asks DOS for the date the ordinary way -- `INT 21h AH=2Ah` in
     * protected mode. krnl386 owns that vector inside a WOW VDM (there is no DOS
     * underneath to answer it), and its handler thunks straight out to here. We
     * did not implement it, so it was STEPPED OVER and answered 0 -- and CLOCK
     * got no date, drew nothing, invalidated its window and asked again:
     * **10,317 times in a fourteen-second run.**
     *
     * [CAUTION]: AND IT CORRECTS A RECORDED CONCLUSION. The standing note said Clock's
     * time "comes from krnl386's own 16-bit code, so there is no thunk to
     * observe", and that both obvious hypotheses were dead (it never calls
     * GetCurrentTime; the BDA tick is correct). The first two were right. The
     * third was an inference, not a measurement -- there IS a thunk, this is it,
     * and one grep of the run for UNIMPLEMENTED found it in a minute.
     *
     * [INFO]: THE SIGNATURE BUG SHAPE, AGAIN: a stepped-over call whose sentinel answer
     * is read as data. Fifth instance in this project. When something is blank,
     * wrong or grey and the log shows no error, grep the run for the stepped-over
     * lines and ask what each answer is used for.
     *
     * [CAUTION]: THE PACKING IS THE FAT/MS-DOS ONE, and it is a JUDGEMENT, not a measurement:
     * date in the HIGH word, time in the LOW -- the order a DOS directory entry
     * stores them in, so a little-endian DWORD read gives exactly this.
     *     date: bits 15-9 year-1980, 8-5 month, 4-0 day
     *     time: bits 15-11 hour, 10-5 minute, 4-0 seconds/2
     * If that is wrong the clock shows a WRONG time rather than no time, which is
     * a different and much louder symptom than the one being fixed -- so the next
     * run distinguishes them without any further instrumentation.
     */
    /* [WARNING]: s88: THAT JUDGEMENT WAS WRONG, AND THE CLOCK SAID SO: "?5/00/2074" (Importance =
     * 1): The packing is pinned by how krnl386 hands it on in INT 21h AH=2Ah's own registers: CX =
     * the high word (the YEAR, in full), DL = bits 15-8 (the DAY), DH = bits 7-4 (the MONTH), AL =
     * bits 3-0 (the DAY OF THE WEEK). So DX:AX = year : (day << 8 | month << 4 | weekday) --
     * AH=2Ah's own registers, folded into one DWORD. Not a time at all: Clock's TIME comes from INT
     * 21h AH=2Ch, which krnl386 passes down to DOS.
     */
    case WOW32_GETDATETIME:
    {
        SYSTEMTIME systemTime;
        GetLocalTime(&systemTime);
        Wow32SetReturn(frame, ((DWORD)systemTime.wYear << WOW32_DATE_YEAR_SHIFT)
                      | ((DWORD)(systemTime.wDay & BYTE_MASK) << WOW32_DATE_DAY_SHIFT)
                      | ((DWORD)(systemTime.wMonth & WOW32_DATE_NIBBLE_MASK) << WOW32_DATE_MONTH_SHIFT)
                      | (DWORD)(systemTime.wDayOfWeek & WOW32_DATE_NIBBLE_MASK));
        return 1;
    }

    case WOW32_GETDRIVETYPE:
    {
        WORD number = Wow32ArgWord(frame, WOW32_GETDRIVETYPE_ARG_DRIVE);
        CHAR root[WOW32_ROOT_PATH_SIZE];
        if (number > WOW32_LAST_DRIVE)
        {
            Wow32SetReturn(frame, 1 /* DRIVE_NO_ROOT_DIR */);
            return 1;
        }
        root[0] = (CHAR)('A' + number);
        root[1] = ':';
        root[2] = '\\';
        root[3] = 0;
        Wow32SetReturn(frame, (DWORD)GetDriveTypeA(root));
        return 1;
    }

    /* -- s92 (#298): THE INTERNAL IDS, NAMED FROM WHEN THEY ARRIVE. No export maps to
     * these; krnl386 calls them from its own code and they were stepped over with
     * the sentinel. Each is answered on purpose now -- the evidence is in the
     * #298 issue thread; the two that CHANGE behaviour are marked .
     */

    /* [INFO]: 0x87: INT 21h AX=4408h -- "is drive BL removable?" -- arrives when a Win16
     * guest issues that call in protected mode, with (AX, BX). As the guest sees it:
     * DX = FFFFh is an ERROR (CF set, AX = the code), anything else clears CF with
     * AX = the answer. Stepped over it answered DX:AX = 0 -- "C: is REMOVABLE" -- to
     * every caller. Same rule as our DOS layer's 44/08 (dos_int21.c): 0 removable (a
     * CD too), 1 fixed, 0Fh invalid drive; BL 0 = the default drive.
     */
    case WOW32_ISDRIVEREMOVABLE:
    {
        BYTE drive = (BYTE)(Wow32ArgWord(frame, WOW32_ISDRIVEREMOVABLE_ARG_DRIVE) & BYTE_MASK);
        UINT driveType = 0;
        CHAR root[WOW32_ROOT_PATH_SIZE];
        if (!drive)
        {
            CHAR currentDirectory[MAX_PATH];
            drive = (GetCurrentDirectoryA(sizeof currentDirectory, currentDirectory) && currentDirectory[0] >= 'A' && currentDirectory[1] == ':')
                ? (BYTE)((currentDirectory[0] | WOW32_LOWERCASE_BIT) - 'a' + 1) : WOW32_DRIVE_C;
        }
        if (drive >= 1 && drive <= WOW32_DRIVE_COUNT && (GetLogicalDrives() & (1u << (drive - 1))))
        {
            root[0] = (CHAR)('A' + drive - 1);
            root[1] = ':';
            root[2] = '\\';
            root[3] = 0;
            driveType = GetDriveTypeA(root);
        }
        if (!driveType || driveType == DRIVE_NO_ROOT_DIR)
            Wow32SetReturn(frame, WOW32_ISDRIVEREMOVABLE_INVALID);
        else
            Wow32SetReturn(frame, (driveType == DRIVE_REMOVABLE || driveType == DRIVE_CDROM) ? 0u : 1u);
        return 1;
    }

    /* 0xc6: arrives during GlobalFree of certain blocks (ones the 32-bit side is
     * taken to own). Non-zero makes krnl386 go on and free the block; zero makes
     * GlobalFree report success -- and the block is kept.
     *
     * [WARNING]: MEASURED s92: answering 1 (free it) killed EVERY Win16 launch -- krnl386 freed
     * block 0x336 while loading KEYBOARD.DRV and then reported "Missing 16-bit
     * system module: KEYBOARD.DRV" and shut the VDM down (runs/s92/gate1). So these
     * blocks are the 32-bit side's to keep, and 0 -- "handled, do not free" -- is
     * the answer, as the sentinel always (accidentally) gave.
     */
    case WOW32_GLOBALFREE_OWNED:
        Wow32SetReturn(frame, 0);
        return 1;

    /* 0x8a: a new task's compatibility flags (argument: the new TDB; the answer is
     * what GetAppCompatFlags then returns). No application is on a list here: 0.
     */
    case WOW32_APPCOMPATFLAGS:
    /* 0x2f: arrives from GetModuleHandle when krnl386 does not know the name
     * (argument: the name). This host has no module krnl386 does not know: 0.
     */
    case WOW32_GETMODULEHANDLE_UNKNOWN:
    /* 0xbe: WOWGetTableOffsets -- a table of 15 per-module id bases. [CAUTION] MUST STAY
     * ZERO-FILLED: USER's and GDI's id spaces here are decoded with the bases at 0
     * ("a DIFFERENT id space"); real offsets would shift every id.
     */
    case WOW32_GETTABLEOFFSETS:
    /* 0xc0: arrives once at boot, carrying pointers into krnl386's data; no use of
     * the result has been observed.
     */
    case WOW32_BOOTPOINTERS:
    /* 0x9d WowFailedExec: WOWEXEC after every exec attempt; the result is ignored. */
    case WOW32_WOWFAILEDEXEC:
    /* 0x8b WowRegisterShellWindowHandle(hwnd, &wCmdShow, hmod): WOWEXEC reads > 0 as
     * "a SHARED WOW" and 0 as "a separate one" -- and a separate WOW exits with its
     * last application (it then calls WowSetExitOnLastApp(1)). One host per launch
     * IS a separate WOW, so 0 is the faithful answer (#280 would change it).
     */
    case WOW32_WOWREGISTERSHELLWINDOW:
        Wow32SetReturn(frame, 0);
        return 1;

    /* 0x7d: approve the selector about to become a TASK DATABASE (Importance = 1):
     * Not named by the export table, so it comes from when it arrives: during
     * task creation, with one argument -- the selector of a freshly allocated
     * 0x320-byte block, which then becomes the new task's database (it is
     * stamped "TD", the TDB signature). `0x320` is exactly limit+1 of every task
     * database in the stock panel (session-37), including the two of a live
     * stock WOW session.
     *
     * [INFO]: THE RETRY IS WHAT PINS THE SEMANTICS. Answered `0`, the guest asks again
     * with a DIFFERENT selector for the SAME bytes (same base and limit -- an
     * alias), so the only thing that differs between one
     * attempt and the next is the NUMERIC VALUE OF THE SELECTOR. The question
     * can only be "may this selector value be a task handle?".
     *
     * [INFO]: AND THE ANSWER IS THE SELECTOR, NOT A BOOLEAN: the value returned is the
     * selector the new TDB is then written through. A
     * 32-bit companion cannot conjure an LDT selector, so the only value it
     * can return is one it was offered: ECHO THE ARGUMENT. A bare `1` -- what
     * the `wow32ret.txt` experiment answered to get WOWEXEC.EXE running -- is
     * right only by luck; it would install `0x0001` as a task's selector.
     *
     * [INFO]: ACCEPTING IS THE RIGHT ANSWER FOR THIS HOST, and that is a reading, not
     * a shrug: whatever the real 32-bit side checks against, we keep no
     * parallel handle table, the LDT krnl386 allocates from is the one we gave
     * it, and each TDB gets a distinct selector by construction. Nothing here
     * could make one selector unacceptable and the next one acceptable.
     *
     * [CAUTION]: If a task ever does need rejecting, this is where it goes -- and the
     * guest's retry is a trap: every rejection costs two bytes of krnl386's
     * stack that are never given back; 1884 of them is the #SS that led here
     * in the first place.
     */
    case WOW32_ACCEPTTASKSELECTOR:
        Wow32SetReturn(frame, (DWORD)Wow32ArgWord(frame, WOW32_ACCEPTTASKSELECTOR_ARG_SELECTOR));
        return 1;

    /* 0x70 WowGetNextVDMCommand -- "WHICH 16-BIT PROGRAM DO I RUN?" (Importance = 4):
     * This is the call that launches a Win16 application, and answering it is
     * the difference between a WOW bootstrap and a running program.
     *
     * [INFO]: HOW IT WAS FOUND. Not by working down a list: the run said so. WOWEXEC
     * asks this once, we answered the harness sentinel `0`, and the VERY NEXT
     * call was `WowMsgBox("Can't run 16-bit Windows program", "Insufficient
     * memory to run this application...")`. The program it wanted is the one
     * the host was launched for -- on the rig, `SYSEDIT.EXE`.
     *
     * [INFO]: AND `0` IS A HARD ERROR, NOT "NOTHING TO DO". WOWEXEC distinguishes:
     *     ret == 0                  -> an error box
     *     ret != 0, cbCmdLine == 0  -> nothing visible; it carries on
     * So the sentinel was making WOWEXEC report a failure that had not
     * happened. "No command" is `1` with a zero length, and it is silent.
     *
     * THE STRUCTURE, AS WOWEXEC PASSES IT AND USES OUR ANSWER:
     * The one argument is a 16:16 pointer to 0x20 bytes on WOWEXEC's stack.
     * Every field below was seen either filled in on arrival (a capacity, a
     * buffer pointer) or carried by our answer into what WOWEXEC does next
     * (LoadModule's arguments, SetCurrentDirectory's argument) -- there is no
     * field here that was not observed being used.
     *
     *   +0x00 DWORD lpCmdLine   -> a 0x10d-byte buffer
     *   +0x04 DWORD lpAppName   -> a 0x10d-byte buffer
     *   +0x08 DWORD lpEnv       -> a GlobalAlloc'd, locked block
     *   +0x0c WORD  (zero on arrival; no use seen)
     *   +0x0e WORD  (zero on arrival; no use seen)
     *   +0x10 WORD  cbCmdLine   in 0x10d -- OUT MUST BE NON-ZERO OR NO LAUNCH
     *   +0x12 WORD  cbAppName   in 0x10d
     *   +0x14 WORD  cbEnv       in 0x1000 -- in/out, see the retry note below
     *   +0x16 WORD  CurDrive    0-based (0 = A:)
     *   +0x18 DWORD lpBufC      -> a third 0x10d-byte buffer
     *   +0x1c WORD  cbBufC      in 0x10d
     *   +0x1e WORD  nCmdShow    -> becomes lpCmdShow[1] of the LOADPARMS
     *
     * [INFO]: +0x04 IS THE MODULE NAME, AND THAT IS MEASURED, NOT ASSUMED: it is the
     * lpModuleName WOWEXEC passes to Win16 `LoadModule(lpModuleName,
     * lpParameterBlock)` next. The parameter block is the standard LOADPARMS,
     * and it is what identifies the other fields: `wEnvSeg` is the SEGMENT half
     * of lpEnv (+0x0a), and `nCmdShow` comes from +0x1e.
     *
     * [CAUTION]: THE COMMAND LINE IS A PASCAL TAIL, AND THE `-2` IS THE WHOLE PUZZLE.
     * The DOS command tail the launched program receives has a count byte of
     * (length of our lpCmdLine string - 2), followed by our string. So the
     * delivered string must be exactly two bytes longer than the tail text it
     * represents, and the only shape that makes every case come out right is
     *                    <tail text> CR LF
     * Check it: with no arguments the text is empty, we deliver "\r\n",
     * its length is 2, the count byte is 0, and the tail reads <0><CR><LF> --
     * a correct empty tail. With text " FOO" we deliver " FOO\r\n", the count
     * is 4, and the tail is <4>' ''F''O''O'<CR><LF>. Both the count and the
     * terminator land where DOS expects them.
     *
     * [CAUTION]: Deliver an EMPTY string and `0 - 2` makes the count byte 0xFE, and the
     * program reads 254 bytes of somebody's stack as its arguments. The two
     * trailing bytes are load-bearing.
     *
     * [CAUTION]: cbEnv (+0x14) IS DELIBERATELY NOT WRITTEN. It is an in/out "the buffer
     * was too small" field -- if the callee returns a LARGER value, the caller
     * reallocates and asks again (the documented shape of such fields). Leaving
     * it alone is the only value that cannot start that loop. WOWEXEC supplies
     * the environment itself (its own); we write a valid empty block so the
     * buffer is never uninitialised, and no more.
     *
     * [INFO]: DELIVER ONCE. In a shared WOW, WOWEXEC keeps asking for the next command
     * (that is what the call is FOR), so a host that answered every time would
     * relaunch the program forever. WOWEXEC only runs as a shared WOW when
     * WowRegisterShellWindowHandle succeeds, which it does not yet (see 0x8b) --
     * so this guard is not load-bearing
     * today, and it is here because the day it becomes load-bearing the symptom
     * is a fork bomb inside the VDM rather than a wrong answer in a log.
     */
    case WOW32_WOWGETNEXTVDMCOMMAND:
    {
        volatile BYTE *commandInfo = Wow32ArgPointer(frame, WOW32_WOWGETNEXTVDMCOMMAND_ARG_COMMANDINFO);
        PCSTR program = g_WowCommandProgram;
        CHAR tail[WOW32_COMMAND_ARGUMENTS_MAX];
        INT number;

        /* An unreachable structure is the one case that really is a hard error:
         * there is nowhere to put the answer, so `0` is the truth.
         */
        if (!commandInfo)
        {
            Wow32SetReturn(frame, 0);
            return 1;
        }

        if (!program[0] || g_WowCommandIsTaken)           /* nothing (more) to run */
        {
            Wow32PokeWord(commandInfo + WOWCMD_CBCMDLINE, 0);
            Wow32SetReturn(frame, 1);
            return 1;
        }

        /* The tail: <text> CR LF, per the note above. DOS tails conventionally
         * begin with the separating space, so the text is " " + args.
         */
        number = 0;
        if (g_WowCommandArguments[0])
        {
            INT argumentIndex;
            tail[number++] = ' ';
            for (argumentIndex = 0; g_WowCommandArguments[argumentIndex] && number < (INT)sizeof tail - WOW32_COMMAND_TAIL_ROOM; ++argumentIndex)
                tail[number++] = g_WowCommandArguments[argumentIndex];
        }
        tail[number++] = '\r';
        tail[number++] = '\n';
        tail[number] = 0;

        number = Wow32FarPut(frame, commandInfo, WOWCMD_LPAPPNAME, WOWCMD_CBAPPNAME, program);
        if (number <= 0) /* no name, no launch */
        {
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (Wow32FarPut(frame, commandInfo, WOWCMD_LPCMDLINE, WOWCMD_CBCMDLINE, tail) <= 0)
        {
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* A valid empty environment, so the caller never walks uninitialised
         * stack looking for its double NUL.
         */
        { volatile BYTE *environment = Wow32FarAt(frame, commandInfo, WOWCMD_LPENV);
          if (environment)
          {
              environment[0] = 0;
              environment[1] = 0;
          }
          }
        /* #164: THE THIRD BUFFER IS THE CURRENT DIRECTORY. (s85):
         * It was "uninitialised stack, passed on unconditionally" and we sent "".
         * Passed on to WHAT is in the log: WOWEXEC's next call is 0x82
         * SetCurrentDirectory with this buffer, which failed on the empty string,
         * so WOWEXEC stayed in C:\WINDOWS for LoadModule and the new task inherited
         * it -- a relative CreateFile landed in C:\WINDOWS. Stock (w_cwd under
         * stock.sh, the same rig) gives the task the folder it was launched from,
         * which is CSRSS's cur= for the launch. So that is what goes here.
         */
        if (g_WowCommandDirectory[0])
        {
            if (Wow32FarPut(frame, commandInfo, WOWCMD_LPBUFC, WOWCMD_CBBUFC, g_WowCommandDirectory) < 0)
                Wow32PokeWord(commandInfo + WOWCMD_CBBUFC, 0);
        }
        else
        {
            volatile BYTE *directoryBuffer = Wow32FarAt(frame, commandInfo, WOWCMD_LPBUFC);
            if (directoryBuffer)
                directoryBuffer[0] = 0;
            Wow32PokeWord(commandInfo + WOWCMD_CBBUFC, 0);
        }

        /* Drive letter of the program's own path, 0-based (0 = A:). Default to C: when the path is
         * not drive-qualified, because there is no "unknown" in a byte.
         */
        { CHAR driveLetter = (program[0] && program[1] == ':') ? program[0] : 'C';
          if (driveLetter >= 'a' && driveLetter <= 'z')
              driveLetter = (CHAR)(driveLetter - WOW32_LOWER_TO_UPPER);
          Wow32PokeWord(commandInfo + WOWCMD_CURDRIVE, (WORD)(driveLetter - 'A')); }
        Wow32PokeWord(commandInfo + WOWCMD_NCMDSHOW, 1);          /* SW_SHOWNORMAL */

        g_WowCommandIsTaken = 1;
        Wow32SetReturn(frame, 1);
        return 1;
    }

    /* 0x78: krnl386 hands us its view of the DOS data area:
     * The argument is a 16:16 pointer to the structure whose address the host
     * publishes at SysVars+0x6A (DOS_WOW_* in dos_layout.h) -- the same table
     * krnl386 takes its far pointers into DOS's data from.
     *
     * [INFO]: RECORD IT AND SUCCEED -- and that is not a stub dressed up. On real
     * Windows the 32-bit side wants this because ntvdm owns that memory from
     * another module; here the HOST already owns it, because the host is what
     * planted SysVars and that table in the first place (see DOS_WOW_* in
     * dos_layout.h). There is nothing to look up. What matters is that the
     * table krnl386 already read was valid, and that is the host's job, not
     * this call's. Logged so the two can be compared.
     */
    case WOW32_REGISTERDOSDATA:
        if (dosData)
        {
            dosData->IsSeen = 1;
            dosData->FarPointer = Wow32ArgDword(frame, WOW32_REGISTERDOSDATA_ARG_POINTER);
        }
        Wow32SetReturn(frame, 0);
        return 1;

    /* -- The INT 21h file family: decline, and let our own DOS layer serve it.
     * See the WOW32_DECLINE block above for why this is an answer rather than
     * a stub, and for the one behavioural difference it buys.
     *
     * [CAUTION]: ONLY AT A SITE THAT CHAINS. The same IDs are called from places where
     * 0xFFFF is returned to the app as a failure -- see Wow32MayDecline. At
     * one of those, fall through to "unimplemented", which is honest, and which
     * the log distinguishes.
     */
    default:
        if (Wow32MayDecline(frame->Id, frame->CallSite))
        {
            Wow32SetReturn(frame, WOW32_DECLINE);
            return 1;
        }
        return 0;
    }
}
