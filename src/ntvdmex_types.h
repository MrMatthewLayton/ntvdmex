/* ntvdmex_types.h -- the Windows types, on every platform this code is built for (#333).
 *
 * NTVDMEX is written with the Windows types (docs/STYLE.md, section 1). On Windows they come
 * from <windows.h>. The off-VM tests build the device models on macOS, where there is no
 * <windows.h>, so this header supplies the same names there -- with EXACTLY the Windows
 * widths. That is the whole point of it, and the one trap: on the Mac `unsigned long` is 64
 * bits, while DWORD, LONG and ULONG are 32 bits on Windows. Every width is checked at compile
 * time below, on both platforms, and again by tests/unit/types_test.c.
 *
 * Include this instead of <stdint.h> / <windows.h> for the base types.
 */
#ifndef NTVDMEX_TYPES_H
#define NTVDMEX_TYPES_H

#ifdef _WIN32

#include <windows.h>

#else /* the off-VM build: the Windows names, at the Windows widths */

#include <stddef.h>

typedef unsigned char       BYTE;
typedef unsigned short      WORD;
typedef unsigned int        DWORD;        /* NOT unsigned long: that is 64 bits here */
typedef signed char         INT8;
typedef signed short        INT16;
typedef signed int          INT32;
typedef signed long long    INT64;
typedef unsigned char       UINT8;
typedef unsigned short      UINT16;
typedef unsigned int        UINT32;
typedef unsigned long long  UINT64;
typedef int                 INT;
typedef unsigned int        UINT;
typedef int                 LONG;         /* 32 bits, as on Windows -- avoid it anyway */
typedef unsigned int        ULONG;
typedef long long           LONGLONG;
typedef unsigned long long  ULONGLONG;
typedef short               SHORT;
typedef unsigned short      USHORT;
typedef char                CHAR;
typedef unsigned char       UCHAR;
typedef unsigned short      WCHAR;
typedef int                 BOOL;
typedef unsigned char       BOOLEAN;
typedef float               FLOAT;
typedef size_t              SIZE_T;
typedef __UINTPTR_TYPE__    UINT_PTR;     /* an integer as wide as a pointer: 64 bits here */
typedef unsigned long       ULONG_PTR;    /* pointer-sized, as on Windows: 64 bits here */
typedef void               *PVOID;
typedef void               *HANDLE;

typedef BYTE   *PBYTE;
typedef WORD   *PWORD;
typedef DWORD  *PDWORD;
typedef INT8   *PINT8;
typedef INT16  *PINT16;
typedef INT32  *PINT32;
typedef INT64  *PINT64;
typedef UINT8  *PUINT8;
typedef UINT16 *PUINT16;
typedef UINT32 *PUINT32;
typedef UINT64 *PUINT64;
typedef INT    *PINT;
typedef UINT   *PUINT;
typedef BOOL   *PBOOL;
typedef CHAR   *PSTR;
typedef const CHAR *PCSTR;
typedef WCHAR  *PWSTR;
typedef const WCHAR *PCWSTR;
typedef SIZE_T *PSIZE_T;

#define VOID  void
#define CONST const
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#define LOBYTE(w)        ((BYTE)((w) & 0xFF))
#define HIBYTE(w)        ((BYTE)(((w) >> 8) & 0xFF))
#define LOWORD(l)        ((WORD)((l) & 0xFFFF))
#define HIWORD(l)        ((WORD)(((l) >> 16) & 0xFFFF))
#define MAKEWORD(lo, hi) ((WORD)(((BYTE)(lo)) | ((WORD)((BYTE)(hi))) << 8))
#define MAKELONG(lo, hi) ((LONG)(((WORD)(lo)) | ((DWORD)((WORD)(hi))) << 16))

#define FORCEINLINE static inline __attribute__((always_inline))

/* SAL annotations document a function's contract to Microsoft's analyser; here they are
   documentation only, and expand to nothing. */
#define _In_
#define _In_opt_
#define _Out_
#define _Out_opt_
#define _Inout_
#define _Inout_opt_
#define _In_reads_(count)
#define _In_reads_opt_(count)
#define _In_reads_bytes_(size)
#define _In_reads_bytes_opt_(size)
#define _Out_writes_(count)
#define _Out_writes_opt_(count)
#define _Out_writes_bytes_(size)
#define _Out_writes_bytes_opt_(size)
#define _Inout_updates_(count)
#define _Inout_updates_bytes_(size)
#define _Ret_maybenull_
#define _Success_(expression)

#endif /* _WIN32 */

/* ── THE POINTER-TO-CONST FORMS THE SDK DOES NOT HAVE. Windows defines PCSTR and PCWSTR but
     no PC form for the basic integer types or for void (only LPCVOID, and the LP prefix is
     not used here). Added on both platforms, so `PC…` means pointer-to-const everywhere. */
typedef const VOID  *PCVOID;
typedef const BYTE  *PCBYTE;
typedef const WORD  *PCWORD;
typedef const DWORD *PCDWORD;

/* ── EVERY WIDTH, CHECKED WHERE IT IS COMPILED. A wrong width does not fail loudly at run
     time: it silently truncates a register or misreads guest memory. So a wrong one fails
     the BUILD, on whichever platform got it wrong. */
#define NTVDMEX_TYPES_ASSERT_SIZE(type, bytes) \
    typedef char NTVDMEX_TYPES_SIZE_OF_##type[(sizeof(type) == (bytes)) ? 1 : -1]
NTVDMEX_TYPES_ASSERT_SIZE(BYTE, 1);
NTVDMEX_TYPES_ASSERT_SIZE(WORD, 2);
NTVDMEX_TYPES_ASSERT_SIZE(DWORD, 4);
NTVDMEX_TYPES_ASSERT_SIZE(INT8, 1);
NTVDMEX_TYPES_ASSERT_SIZE(INT16, 2);
NTVDMEX_TYPES_ASSERT_SIZE(INT32, 4);
NTVDMEX_TYPES_ASSERT_SIZE(INT64, 8);
NTVDMEX_TYPES_ASSERT_SIZE(UINT64, 8);
NTVDMEX_TYPES_ASSERT_SIZE(INT, 4);
NTVDMEX_TYPES_ASSERT_SIZE(UINT, 4);
NTVDMEX_TYPES_ASSERT_SIZE(LONG, 4);
NTVDMEX_TYPES_ASSERT_SIZE(ULONG, 4);
NTVDMEX_TYPES_ASSERT_SIZE(BOOL, 4);
NTVDMEX_TYPES_ASSERT_SIZE(UINT_PTR, sizeof(PVOID));

#include "ntvdmex_bits.h"       /* the byte/word/dword masks and shifts */
#include "ntvdmex_x86.h"        /* the x86 layout: paragraphs, pages, the IVT */
#include "ntvdmex_units.h"      /* time and frequency units */
#include "ntvdmex_ascii.h"      /* control characters */

#endif /* NTVDMEX_TYPES_H */
