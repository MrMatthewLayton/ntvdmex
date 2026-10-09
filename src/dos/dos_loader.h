/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Load a DOS program image (MZ .EXE or flat .COM) into
 * conventional memory. Pure logic over a `base` pointer (same convention as
 * dos_mcb.h): base=NULL for the host's absolute V86 addressing, a byte buffer
 * for off-VM tests. Ported from the M2.3 loader in tools/vdmhost/vdmhost.c.
 * Verified off-VM by tests/unit/mcb_test.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_DOS_LOADER_H
#define NTVDMEX_DOS_LOADER_H

#include "../ntvdmex_types.h"
#include "dos_mcb.h"    /* DosMcbSegmentAddress / DosMcbReadWord / DosMcbWriteWord paragraph addressing */

/* The MZ header's fields, as offsets into the file. */
#define DOS_MZ_LAST_PAGE_BYTES      2       /* e_cblp: bytes used in the last page */
#define DOS_MZ_PAGE_COUNT           4       /* e_cp */
#define DOS_MZ_RELOCATION_COUNT     6       /* e_crlc */
#define DOS_MZ_HEADER_PARAGRAPHS    8       /* e_cparhdr */
#define DOS_MZ_MIN_ALLOC            10      /* e_minalloc */
#define DOS_MZ_MAX_ALLOC            12      /* e_maxalloc */
#define DOS_MZ_INITIAL_SS           14      /* e_ss */
#define DOS_MZ_INITIAL_SP           16      /* e_sp */
#define DOS_MZ_INITIAL_IP           20      /* e_ip */
#define DOS_MZ_INITIAL_CS           22      /* e_cs */
#define DOS_MZ_RELOCATION_TABLE     24      /* e_lfarlc */
#define DOS_MZ_NEW_HEADER           0x3C    /* e_lfanew: a DWORD */
#define DOS_MZ_HEADER_MIN           0x1C    /* Shorter than this is not an MZ header */
#define DOS_MZ_NEW_HEADER_MIN       0x40    /* An e_lfanew below 40h is an old DOS .EXE */
#define DOS_EXE_SIGNATURE_SIZE      2       /* "MZ", and the new header's "NE" / "PE" */
#define DOS_MZ_PAGE_BYTES           512
#define DOS_MZ_PAGE_PARAGRAPHS      32u     /* 512 / 16 */
#define DOS_MZ_RELOCATION_ENTRY     4       /* Each: WORD offset, then WORD segment */
#define DOS_MZ_RELOCATION_SEGMENT   2

/* A flat .COM: loaded at PSP:100h, with the stack at the top of its segment. */
#define DOS_COM_MAX_BYTES           0xFE00
#define DOS_COM_ENTRY               0x100
#define DOS_COM_INITIAL_SP          0xFFFE

/* The new-header fields DosExeKind reads, as offsets from e_lfanew. */
#define DOS_PE_SIGNATURE_BYTES      4       /* "PE\0\0" */
#define DOS_PE_SUBSYSTEM            0x5C    /* The optional header's Subsystem WORD */
#define DOS_PE_SUBSYSTEM_END        0x5E
#define DOS_NE_TARGET_OS            0x36    /* ne_exetyp */
#define DOS_NE_TARGET_OS_END        0x37
#define DOS_NE_OS_UNSPECIFIED       0       /* Windows 1.x/2.x programs */
#define DOS_NE_OS_WINDOWS           2

/* IS THIS A DOS PROGRAM AT ALL? (GH #255) (Importance = 1):
 * A Windows program is an MZ file too: its MZ part is a STUB ("This program
 * cannot be run in DOS mode" / "requires Microsoft Windows"), and e_lfanew at 3Ch
 * points at the real header. MS-DOS runs the stub, because to DOS that is the
 * program. NTVDM does not: an NE goes to WOW and a PE to Win32, so on stock XP
 * typing NOTEPAD at COMMAND.COM starts Notepad. We are the NTVDM replacement, so
 * EXEC asks this before loading anything.
 *
 * [CAUTION]: ONLY WINDOWS'S OWN TWO SIGNATURES, AND ONLY A WINDOWS NE. The 3Ch slot is read
 * whatever the file is, so anything else must stay a DOS program:
 *   "LE"/"LX"  -- a DOS/4GW-bound game (Doom, Duke3D): the stub IS the extender;
 *   NE with target OS 1 (OS/2) -- a BIND'ed "family API" program, whose stub IS
 *              the DOS version of the program (XP has no OS/2 subsystem either);
 *   an e_lfanew below 40h or past the end of what was read -- an old DOS .EXE
 *              with whatever happens to sit at 3Ch.
 * NE target OS (ne_exetyp, NE+36h): 2 = Windows; 0 = unspecified, which is what
 * Windows 1.x/2.x programs carry (the field came later) -- both are WOW's.
 * Out (PE only): *subsystem = the PE optional header's Subsystem (2 GUI, 3 console),
 * which decides whether EXEC waits for it.
 */
#define DOS_EXE_DOS                 0       /* Load it ourselves: .COM, plain MZ, LE/LX, OS/2 NE */
#define DOS_EXE_NE                  1       /* A Windows NE: WOW's */
#define DOS_EXE_PE                  2       /* A PE: Win32's */

typedef struct _DOS_IMAGE
{
    /* entry CS:IP */
    WORD CodeSegment;
    WORD InstructionPointer;
    /* entry SS:SP */
    WORD StackSegment;
    WORD StackPointer;
    BOOL     IsExe;         /* 1 = MZ .EXE, 0 = flat .COM */
    DWORD    ImageSize;     /* bytes placed in conventional memory */
} DOS_IMAGE, *PDOS_IMAGE;

typedef const DOS_IMAGE *PCDOS_IMAGE;

static inline WORD DosLoaderReadWord(_In_reads_bytes_(2) PCBYTE field)
{
    return (WORD)((WORD)field[0] | ((WORD)field[1] << BYTE_SHIFT));
}

/* Place `file` (bytesRead bytes) into conventional memory at `base`:
 *  - MZ .EXE: load module copied to (pspSegment+0x10):0, relocations applied,
 *    CS:IP/SS:SP from the header (segment fields biased by the load segment).
 *  - flat .COM: copied to pspSegment:0x100; CS=SS=pspSegment, IP=0x100, SP=0xFFFE.
 * Returns the entry context. Mirrors vdmhost.c's M2.3 loader exactly.
 */
/* loadSegment: WHERE THE IMAGE GOES, which is not always just past the PSP:
 * An MZ image with e_minalloc = e_maxalloc = 0 is LOADED HIGH -- at the top of
 * the block EXEC gave it, with the PSP still at the bottom (GH #255; see
 * DosExecSize). 0 = the ordinary place, pspSegment + 10h. Ignored for a .COM.
 */
static inline DOS_IMAGE DosLoadImageAt(
    _In_opt_ volatile BYTE *base,
    _In_reads_bytes_(bytesRead) PCBYTE file,
    _In_ DWORD bytesRead,
    _In_ WORD pspSegment,
    _In_ WORD requestedSegment)
{
    DOS_IMAGE image;
    DWORD index;

    if (bytesRead >= DOS_MZ_HEADER_MIN && file[0] == 'M' && file[1] == 'Z')
    {
        DWORD headerSize  = (DWORD)DosLoaderReadWord(file + DOS_MZ_HEADER_PARAGRAPHS) * PARAGRAPH_SIZE;  /* e_cparhdr */
        DWORD relocationCount   = DosLoaderReadWord(file + DOS_MZ_RELOCATION_COUNT);                 /* reloc count */
        DWORD relocationTable = DosLoaderReadWord(file + DOS_MZ_RELOCATION_TABLE);               /* reloc tbl off */
        WORD loadSegment = requestedSegment ? requestedSegment : (WORD)(pspSegment + DOS_PSP_PARAGRAPHS);
        volatile BYTE *imageBytes = DosMcbSegmentAddress(base, loadSegment);
        WORD lastPageBytes = DosLoaderReadWord(file + DOS_MZ_LAST_PAGE_BYTES);                  /* bytes, last pg */
        WORD pageCount   = DosLoaderReadWord(file + DOS_MZ_PAGE_COUNT);                  /* page count */
        DWORD totalUsed = pageCount ? ((DWORD)(pageCount - 1) * DOS_MZ_PAGE_BYTES + (lastPageBytes ? lastPageBytes : DOS_MZ_PAGE_BYTES))
                                  : bytesRead;
        DWORD imageSize;

        if (totalUsed > bytesRead)
            totalUsed = bytesRead;

        imageSize = totalUsed > headerSize ? totalUsed - headerSize : 0;

        for (index = 0; index < imageSize; ++index)
            imageBytes[index] = file[headerSize + index];

        for (index = 0; index < relocationCount; ++index)                              /* apply relocations */
        {
            DWORD fixupOffset = DosLoaderReadWord(file + relocationTable + index * DOS_MZ_RELOCATION_ENTRY);
            DWORD fixupSegment = DosLoaderReadWord(file + relocationTable + index * DOS_MZ_RELOCATION_ENTRY + DOS_MZ_RELOCATION_SEGMENT);
            volatile BYTE *fixup = (volatile BYTE *)((ULONG_PTR)base
                                    + (((DWORD)(loadSegment + fixupSegment)) << PARAGRAPH_SHIFT) + fixupOffset);
            DosMcbWriteWord(fixup, (WORD)(DosMcbReadWord(fixup) + loadSegment));
        }

        image.CodeSegment = (WORD)(loadSegment + DosLoaderReadWord(file + DOS_MZ_INITIAL_CS));     /* e_cs */
        image.InstructionPointer = DosLoaderReadWord(file + DOS_MZ_INITIAL_IP);                            /* e_ip */
        image.StackSegment = (WORD)(loadSegment + DosLoaderReadWord(file + DOS_MZ_INITIAL_SS));     /* e_ss */
        image.StackPointer = DosLoaderReadWord(file + DOS_MZ_INITIAL_SP);                            /* e_sp */
        image.IsExe = TRUE;
        image.ImageSize = imageSize;
    }
    else                                                        /* flat .COM */
    {
        DWORD comSize = bytesRead > DOS_COM_MAX_BYTES ? DOS_COM_MAX_BYTES : bytesRead;
        volatile BYTE *code = DosMcbSegmentAddress(base, pspSegment) + DOS_COM_ENTRY;   /* pspSegment:0x100 */

        for (index = 0; index < comSize; ++index)
            code[index] = file[index];

        image.CodeSegment = pspSegment;
        image.InstructionPointer = DOS_COM_ENTRY;
        image.StackSegment = pspSegment;
        image.StackPointer = DOS_COM_INITIAL_SP;
        image.IsExe = FALSE;
        image.ImageSize = comSize;
    }

    return image;
}

static inline DOS_IMAGE DosLoadImage(
    _In_opt_ volatile BYTE *base,
    _In_reads_bytes_(bytesRead) PCBYTE file,
    _In_ DWORD bytesRead,
    _In_ WORD pspSegment)
{
    return DosLoadImageAt(base, file, bytesRead, pspSegment, 0);
}

/* THE IMAGE'S SIZE IN PARAGRAPHS, AS DOS COUNTS IT FOR MEMORY: WHOLE PAGES:
 * e_cp * 32 - e_cparhdr -- the 512-byte page count, NOT trimmed by e_cblp (the
 * bytes used in the last page). MEASURED (p_exmem.asm, 6.22 under QEMU and
 * DOSBox-X agree): the child's file is 150h bytes, a 20h-byte header and 130h of
 * image -- 13h paragraphs of actual bytes -- and DOS sizes its block as if the
 * image were 1Eh (one page, 20h paragraphs, less the 2-paragraph header). The
 * loader still COPIES only the e_cblp-trimmed bytes (DosLoadImageAt); this is the
 * size DOS RESERVES for it, and the distance below the block's end a load-high
 * image is put at.
 */
static inline WORD DosImageParagraphs(_In_reads_bytes_(bytesRead) PCBYTE file, _In_ DWORD bytesRead)
{
    DWORD pageParagraphs;
    DWORD headerParagraphs;

    if (bytesRead < DOS_MZ_HEADER_MIN || file[0] != 'M' || file[1] != 'Z')
        return 0;

    pageParagraphs = (DWORD)DosLoaderReadWord(file + DOS_MZ_PAGE_COUNT) * DOS_MZ_PAGE_PARAGRAPHS;
    headerParagraphs   = DosLoaderReadWord(file + DOS_MZ_HEADER_PARAGRAPHS);
    return (WORD)(pageParagraphs > headerParagraphs ? pageParagraphs - headerParagraphs : 0);
}

/* HOW MUCH MEMORY EXEC GIVES AN MZ PROGRAM. (GH #255) (Importance = 1):
 * EXEC used to hand every child ALL of the largest free block, whatever the
 * header said. The header says two things about memory beyond the image:
 * e_minalloc  paragraphs it CANNOT run without -- not there => error 8, and the
 *             program is never loaded;
 * e_maxalloc  paragraphs it would LIKE -- the block is cut down to that, which is
 *             how a program linked /CPARMAXALLOC leaves memory for its children.
 * And one combination is special: minalloc = maxalloc = 0 means LOAD HIGH -- the
 * whole block, with the image at its TOP.
 *
 * [INFO]: MEASURED, NOT DERIVED (tests/probes/dos/p_exmem.asm; 6.22 under QEMU and
 * DOSBox-X agree): a child whose image DOS counts as 1Eh paragraphs (see
 * DosImageParagraphs -- whole pages) with min/max = 100h/200h gets a block of
 * 10h (PSP) + 1Eh + 200h = 22Eh; max 10h gives 3Eh; min F000h is refused with
 * AX=0008 and never runs; min/max 0/0 puts CS 1Eh paragraphs below the block's
 * end; max FFFFh takes the whole largest block. Every child used to get 8AE2h
 * paragraphs -- all of it -- on the rig.
 * Inputs: the file, and `largest` = the largest free block in paragraphs. Out:
 * *allocation (paragraphs to allocate, PSP included) and *loadHigh (1 = load high).
 * Returns 0, or 8 (DOS's "insufficient memory") when even minalloc will not fit.
 * A .COM (or anything not MZ) is not sized here: it takes the largest block.
 */
static inline INT DosExecSize(
    _In_reads_bytes_(bytesRead) PCBYTE file,
    _In_ DWORD bytesRead,
    _In_ WORD largest,
    _Out_ PWORD allocation,
    _Out_ PBOOL loadHigh)
{
    DWORD imageParagraphs;
    DWORD needed;
    DWORD wanted;
    WORD minimumAlloc;
    WORD maximumAlloc;

    *loadHigh = FALSE;

    if (bytesRead < DOS_MZ_HEADER_MIN || file[0] != 'M' || file[1] != 'Z')
    {
        *allocation = largest;
        return DOS_MCB_SUCCESS;
    }

    imageParagraphs = DosImageParagraphs(file, bytesRead);
    minimumAlloc = DosLoaderReadWord(file + DOS_MZ_MIN_ALLOC);
    maximumAlloc = DosLoaderReadWord(file + DOS_MZ_MAX_ALLOC);
    needed = DOS_PSP_PARAGRAPHS + imageParagraphs + minimumAlloc;

    if (needed > largest)
        return DOS_MCB_ERROR_INSUFFICIENT_MEMORY;

    if (minimumAlloc == 0 && maximumAlloc == 0)
    {
        *loadHigh = TRUE;
        *allocation = largest;
        return DOS_MCB_SUCCESS;
    }

    wanted = DOS_PSP_PARAGRAPHS + imageParagraphs + maximumAlloc;

    if (wanted < needed)
        wanted = needed;                               /* a maxalloc below minalloc */

    *allocation = (WORD)(wanted < largest ? wanted : largest);
    return DOS_MCB_SUCCESS;
}

static inline INT DosExeKind(
    _In_reads_bytes_(bytesRead) PCBYTE file,
    _In_ DWORD bytesRead,
    _Out_ PUINT subsystem)
{
    DWORD newHeader;

    *subsystem = 0;

    if (bytesRead < DOS_MZ_NEW_HEADER_MIN || file[0] != 'M' || file[1] != 'Z')
        return DOS_EXE_DOS;

    newHeader = (DWORD)DosLoaderReadWord(file + DOS_MZ_NEW_HEADER) | ((DWORD)DosLoaderReadWord(file + DOS_MZ_NEW_HEADER + 2) << WORD_SHIFT);

    if (newHeader < DOS_MZ_NEW_HEADER_MIN || newHeader > bytesRead - DOS_PE_SIGNATURE_BYTES)
        return DOS_EXE_DOS;

    if (file[newHeader] == 'P' && file[newHeader + 1] == 'E' && file[newHeader + 2] == 0 && file[newHeader + 3] == 0)
    {
        if (newHeader + DOS_PE_SUBSYSTEM_END <= bytesRead)
            *subsystem = DosLoaderReadWord(file + newHeader + DOS_PE_SUBSYSTEM);

        return DOS_EXE_PE;
    }

    if (file[newHeader] == 'N' && file[newHeader + 1] == 'E' && newHeader + DOS_NE_TARGET_OS_END <= bytesRead)
    {
        BYTE targetOs = file[newHeader + DOS_NE_TARGET_OS];

        if (targetOs == DOS_NE_OS_WINDOWS || targetOs == DOS_NE_OS_UNSPECIFIED)
            return DOS_EXE_NE;
    }

    return DOS_EXE_DOS;
}

/* AH=4Bh AL=03: LOAD AN OVERLAY. (GH #50):
 * Not a process: no PSP, no memory allocated, no transfer of control. The caller
 * says WHERE to put it (loadSegment) and, separately, WHAT TO RELOCATE BY
 * (relocationFactor) -- and those two are not the same number. A large program swaps
 * overlays into one buffer it already owns, so the relocation factor is the
 * buffer's segment while the load segment may differ.
 *
 * [INFO]: THE FACTOR, NOT THE LOAD SEGMENT. tests/probes/dos/p_ovl.asm passes a factor of
 * 0x1234 that is deliberately NOT the load segment, and MS-DOS 6.22 relocates by
 * the factor:
 *     CASE=ovl.relocated.word SIG=AX AX=1234
 * which separates the three ways to be wrong -- 0 (never relocated), the load
 * segment (relocated by the wrong value), or their sum (relocated twice).
 */
static inline DWORD DosLoadOverlay(
    _In_opt_ volatile BYTE *base,
    _In_reads_bytes_(bytesRead) PCBYTE file,
    _In_ DWORD bytesRead,
    _In_ WORD loadSegment,
    _In_ WORD relocationFactor)
{
    DWORD index;
    DWORD headerSize;
    DWORD relocationCount;
    DWORD relocationTable;
    DWORD totalUsed;
    DWORD imageSize;
    WORD lastPageBytes;
    WORD pageCount;
    volatile BYTE *imageBytes;

    if (bytesRead < DOS_MZ_HEADER_MIN || file[0] != 'M' || file[1] != 'Z')
    {
        /* A .COM overlay is the file, verbatim, with nothing to relocate. */
        imageBytes = DosMcbSegmentAddress(base, loadSegment);

        for (index = 0; index < bytesRead; ++index)
            imageBytes[index] = file[index];

        return bytesRead;
    }

    headerSize  = (DWORD)DosLoaderReadWord(file + DOS_MZ_HEADER_PARAGRAPHS) * PARAGRAPH_SIZE;
    relocationCount   = DosLoaderReadWord(file + DOS_MZ_RELOCATION_COUNT);
    relocationTable = DosLoaderReadWord(file + DOS_MZ_RELOCATION_TABLE);
    lastPageBytes   = DosLoaderReadWord(file + DOS_MZ_LAST_PAGE_BYTES);
    pageCount     = DosLoaderReadWord(file + DOS_MZ_PAGE_COUNT);
    totalUsed = pageCount ? ((DWORD)(pageCount - 1) * DOS_MZ_PAGE_BYTES + (lastPageBytes ? lastPageBytes : DOS_MZ_PAGE_BYTES)) : bytesRead;

    if (totalUsed > bytesRead)
        totalUsed = bytesRead;

    imageSize = totalUsed > headerSize ? totalUsed - headerSize : 0;
    imageBytes = DosMcbSegmentAddress(base, loadSegment);

    for (index = 0; index < imageSize; ++index)
        imageBytes[index] = file[headerSize + index];

    for (index = 0; index < relocationCount; ++index)
    {
        DWORD fixupOffset = DosLoaderReadWord(file + relocationTable + index * DOS_MZ_RELOCATION_ENTRY);
        DWORD fixupSegment = DosLoaderReadWord(file + relocationTable + index * DOS_MZ_RELOCATION_ENTRY + DOS_MZ_RELOCATION_SEGMENT);
        volatile BYTE *fixup = (volatile BYTE *)((ULONG_PTR)base
                                + (((DWORD)(loadSegment + fixupSegment)) << PARAGRAPH_SHIFT) + fixupOffset);
        DosMcbWriteWord(fixup, (WORD)(DosMcbReadWord(fixup) + relocationFactor));
    }

    return imageSize;
}

#endif /* NTVDMEX_DOS_LOADER_H */
