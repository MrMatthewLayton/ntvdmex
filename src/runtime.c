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
#include <windows.h>

void *memset(void *dst, int c, size_t n)
{
    unsigned char *p = (unsigned char *)dst;
    while (n--)
        *p++ = (unsigned char)c;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    return memmove(dst, src, n);
}

/* Real-mode-DOS host, but first a window: hand off to WinMain, then exit.
   Named WinMainCRTStartup so the linker picks it as the default GUI entry. */
extern int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int);

void WinMainCRTStartup(void)
{
    int rc = WinMain(GetModuleHandleA(NULL), NULL, GetCommandLineA(), SW_SHOWDEFAULT);
    ExitProcess((UINT)rc);
}
