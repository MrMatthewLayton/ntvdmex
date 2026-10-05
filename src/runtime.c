/*
 * runtime.c - Freestanding runtime shim (no C runtime library).
 *
 * NTVDMEX links with -nostdlib so the produced binary depends on nothing beyond
 * the Win32 system DLLs that ship with Windows XP itself. The toolchain used to
 * build it (Homebrew mingw-w64) is UCRT-default, and UCRT (api-ms-win-crt-*.dll)
 * does NOT exist on XP -- a CRT-linked binary fails to load there with missing
 * DLL errors. Dropping the CRT sidesteps that entirely and also suits a project
 * that will ultimately manage its own process image (low memory, VDM_TIB, ...).
 *
 * The cost is small: we supply our own entry point and the handful of mem*
 * primitives the compiler may emit calls to. Everything else is Win32.
 *
 * NOTE: this file is compiled with -fno-builtin / -fno-tree-loop-distribute-patterns
 * (see CMakeLists.txt) so GCC does not "optimise" these loops back into a call to
 * the very function being defined (infinite recursion).
 */
#include "ntvdmex_types.h"

/* The three mem* functions keep the C library's names and exact signatures (void *,
   int, size_t): the compiler emits calls to them by those names and checks the
   declarations against its built-in ones. Only their insides follow the style. */

void *memset(void *buffer, int fillValue, size_t byteCount)
{
    PBYTE destination = (PBYTE)buffer;
    while (byteCount--)
        *destination++ = (BYTE)fillValue;
    return buffer;
}

void *memmove(void *destinationBuffer, const void *sourceBuffer, size_t byteCount)
{
    PBYTE  destination = (PBYTE)destinationBuffer;
    PCBYTE source = (PCBYTE)sourceBuffer;
    if (destination < source) {
        while (byteCount--)
            *destination++ = *source++;
    } else {
        destination += byteCount;
        source += byteCount;
        while (byteCount--)
            *--destination = *--source;
    }
    return destinationBuffer;
}

void *memcpy(void *destinationBuffer, const void *sourceBuffer, size_t byteCount)
{
    return memmove(destinationBuffer, sourceBuffer, byteCount);
}

/* Real-mode-DOS host, but first a window: hand off to WinMain, then exit.
   Named WinMainCRTStartup so the linker picks it as the default GUI entry. */
extern int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int);

VOID WinMainCRTStartup(VOID)
{
    INT exitCode = WinMain(GetModuleHandleA(NULL), NULL, GetCommandLineA(), SW_SHOWDEFAULT);
    ExitProcess((UINT)exitCode);
}
