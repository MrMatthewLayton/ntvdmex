/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for the NE loader (GH #128 / #4).
 *
 * Two halves, deliberately:
 *
 *  1. SYNTHETIC. A hand-built NE image covering every relocation shape the loader
 *     claims to handle, including the two that are easy to get wrong -- chained
 *     records and moveable (entry-ordinal) targets. These ALWAYS run, so a fresh
 *     clone gets real coverage with nothing to download.
 *
 *  2. REAL BINARIES, IF PRESENT. guest/ne/krnl386.exe and sysedit.exe are
 *     Microsoft's and are NOT in this repository, so these cases SKIP when absent
 *     rather than fail. The expected numbers come from tools/ne/nedump.py run
 *     against the real files -- see the commit that added it.
 *
 * Build+run via tests/probes/dos/run.sh.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../src/wow/ne.h"

/* build a synthetic NE in memory: */
#define HDR         0x40
#define SEGTAB      0x40    /* Relative to HDR */
#define ENTTAB      0x60
#define SECSHIFT    4

#define LRES        0x100   /* Resident names, relative to HDR */
#define LNRES       0x300   /* Non-resident, ABSOLUTE */

/* Two segments. Seg1 = code with relocations, seg2 = data.
 * Relocation records exercise: SEGMENT/chained, FARADDR/chained, OFFSET16/additive,
 * and an INTERNALREF whose target is a MOVEABLE entry ordinal. With `imports`, two
 * more records arrive: an IMPORTORDINAL and an IMPORTNAME, both against TESTLIB.
 */
#define MODTAB      0x80    /* Relative to HDR */
#define IMPTAB      0x90

static INT g_Passes;
static INT g_Failures;
static INT g_Skips;

static BYTE g_Image[0x4000];

/* a synthetic LIBRARY to import from:
 * Exports ordinal 1 (fixed, exported), 2 (moveable, exported, reachable by the name
 * BAR from the NON-resident table) and 3 (present but NOT exported). Its own name is
 * TESTLIB, which is what an importer refers to it by.
 */
static BYTE g_LibraryImage[0x1000];
static BYTE g_LibrarySegmentMemory[0x200];

static BYTE g_SegmentMemory[2][0x200];

static VOID NeTestCheck(INT condition, PCSTR description)
{
    if (condition)
    {
        ++g_Passes;
        printf("  PASS  %s\n", description);
    }
    else
    {
        ++g_Failures;
        printf("  FAIL  %s\n", description);
    }
}

static VOID NeTestSkip(PCSTR description)
{
    ++g_Skips;
    printf("  SKIP  %s\n", description);
}

static VOID NeTestWrite16(UINT32 offset, WORD value)
{
    g_Image[offset] = (BYTE)value;
    g_Image[offset+1] = (BYTE)(value>>8);
}

static VOID NeTestWrite32(UINT32 offset, UINT32 value)
{
    NeTestWrite16(offset, (WORD)value);
    NeTestWrite16(offset+2, (WORD)(value>>16));
}

static VOID NeTestLibraryWrite16(UINT32 offset, WORD value)
{
    g_LibraryImage[offset] = (BYTE)value;
    g_LibraryImage[offset+1] = (BYTE)(value>>8);
}

static VOID NeTestLibraryWrite32(UINT32 offset, UINT32 value)
{
    NeTestLibraryWrite16(offset, (WORD)value);
    NeTestLibraryWrite16(offset+2, (WORD)(value>>16));
}

static UINT32 NeTestLibraryString(UINT32 offset, PCSTR text, WORD ordinal)
{
    UINT32 length = (UINT32)strlen(text);
    UINT32 index;

    g_LibraryImage[offset] = (BYTE)length;
    for (index = 0; index < length; ++index)
        g_LibraryImage[offset+1+index] = (BYTE)text[index];
    NeTestLibraryWrite16(offset + 1 + length, ordinal);
    return offset + 1 + length + 2;
}

static VOID NeTestBuildLibrary(VOID)
{
    UINT32 segment1Offset = 0x200;
    UINT32 entryOffset;
    UINT32 offset;

    memset(g_LibraryImage, 0, sizeof g_LibraryImage);
    g_LibraryImage[0] = 'M';
    g_LibraryImage[1] = 'Z';
    NeTestLibraryWrite32(0x3C, HDR);
    g_LibraryImage[HDR] = 'N';
    g_LibraryImage[HDR+1] = 'E';
    NeTestLibraryWrite16(HDR+0x04, ENTTAB);
    NeTestLibraryWrite16(HDR+0x0C, 0x8001);         /* LIBRARY | SINGLEDATA */
    NeTestLibraryWrite16(HDR+0x0E, 0);
    NeTestLibraryWrite32(HDR+0x14, (1u<<16) | 0x00);
    NeTestLibraryWrite16(HDR+0x1C, 1);              /* one segment */
    NeTestLibraryWrite16(HDR+0x22, SEGTAB);
    NeTestLibraryWrite16(HDR+0x26, LRES);
    NeTestLibraryWrite16(HDR+0x30, 1);
    NeTestLibraryWrite16(HDR+0x32, SECSHIFT);
    g_LibraryImage[HDR+0x36] = 2;

    NeTestLibraryWrite16(HDR+SEGTAB+0, (WORD)(segment1Offset >> SECSHIFT));
    NeTestLibraryWrite16(HDR+SEGTAB+2, 0x40);
    NeTestLibraryWrite16(HDR+SEGTAB+4, 0);
    NeTestLibraryWrite16(HDR+SEGTAB+6, 0x40);

    entryOffset = HDR + ENTTAB;
    g_LibraryImage[entryOffset++] = 1;
    g_LibraryImage[entryOffset++] = 1;                      /* bundle: 1 FIXED in seg 1 */
    g_LibraryImage[entryOffset++] = 0x01;
    NeTestLibraryWrite16(entryOffset, 0x0010);
    entryOffset += 2;         /* ord 1, EXPORTED */
    g_LibraryImage[entryOffset++] = 1;
    g_LibraryImage[entryOffset++] = 0xFF;                   /* bundle: 1 MOVEABLE */
    g_LibraryImage[entryOffset++] = 0x01;
    g_LibraryImage[entryOffset++] = 0xCD;
    g_LibraryImage[entryOffset++] = 0x3F;
    g_LibraryImage[entryOffset++] = 1;
    NeTestLibraryWrite16(entryOffset, 0x0020);
    entryOffset += 2;            /* ord 2 -> seg 1 : 0x0020 */
    g_LibraryImage[entryOffset++] = 1;
    g_LibraryImage[entryOffset++] = 1;
    g_LibraryImage[entryOffset++] = 0x00;
    NeTestLibraryWrite16(entryOffset, 0x0030);
    entryOffset += 2;         /* ord 3, NOT exported */
    g_LibraryImage[entryOffset++] = 1;
    g_LibraryImage[entryOffset++] = NE_ENT_ABSOLUTE;        /* bundle: 1 ABSOLUTE */
    g_LibraryImage[entryOffset++] = 0x01;
    NeTestLibraryWrite16(entryOffset, 0xA000);
    entryOffset += 2;         /* ord 4 = the CONSTANT */
    g_LibraryImage[entryOffset++] = 0;                                     /* terminator */
    NeTestLibraryWrite16(HDR+0x06, (WORD)(entryOffset - (HDR + ENTTAB)));    /* entry table length */

    offset = NeTestLibraryString(HDR + LRES, "TESTLIB", 0);               /* entry 0 = module name */
    offset = NeTestLibraryString(offset, "FOO", 1);
    g_LibraryImage[offset] = 0;

    offset = NeTestLibraryString(LNRES, "a description", 0);              /* entry 0 = description */
    offset = NeTestLibraryString(offset, "BAR", 2);
    g_LibraryImage[offset] = 0;
    NeTestLibraryWrite32(HDR+0x2C, LNRES);                             /* ABSOLUTE, and a DWORD */
    NeTestLibraryWrite16(HDR+0x20, (WORD)(offset + 1 - LNRES));
}

static VOID NeTestBuild(INT imports)
{
    UINT32 segment1Offset = 0x200;
    UINT32 segment2Offset = 0x400;
    UINT32 relocations;

    memset(g_Image, 0, sizeof g_Image);
    g_Image[0] = 'M';
    g_Image[1] = 'Z';
    NeTestWrite32(0x3C, HDR);
    g_Image[HDR] = 'N';
    g_Image[HDR+1] = 'E';
    NeTestWrite16(HDR+0x04, ENTTAB);          /* entry table offset */
    NeTestWrite16(HDR+0x06, 16);              /* entry table length */
    NeTestWrite16(HDR+0x0C, 0x0001);          /* prog flags */
    NeTestWrite16(HDR+0x0E, 2);               /* autodata = seg 2 */
    NeTestWrite32(HDR+0x14, (1u<<16) | 0x10); /* CS:IP = seg1:0x10 */
    NeTestWrite32(HDR+0x18, (2u<<16) | 0x00);
    NeTestWrite16(HDR+0x1C, 2);               /* 2 segments */
    NeTestWrite16(HDR+0x22, SEGTAB);
    NeTestWrite16(HDR+0x30, 1);               /* 1 moveable entry */
    NeTestWrite16(HDR+0x32, SECSHIFT);
    g_Image[HDR+0x36] = 2;              /* target = Windows */
    NeTestWrite16(HDR+0x3E, 0x030A);

    /* segment table: sector, length, flags, minalloc */
    NeTestWrite16(HDR+SEGTAB+0, (WORD)(segment1Offset >> SECSHIFT));
    NeTestWrite16(HDR+SEGTAB+2, 0x40);
    NeTestWrite16(HDR+SEGTAB+4, NE_SEG_RELOCS);
    NeTestWrite16(HDR+SEGTAB+6, 0x80);
    NeTestWrite16(HDR+SEGTAB+8, (WORD)(segment2Offset >> SECSHIFT));
    NeTestWrite16(HDR+SEGTAB+10, 0x20);
    NeTestWrite16(HDR+SEGTAB+12, NE_SEG_DATA);
    NeTestWrite16(HDR+SEGTAB+14, 0x20);

    /* entry table: one bundle, 1 moveable entry -> ordinal 1 = seg 2, offset 0x1234 */
    g_Image[HDR+ENTTAB+0] = 1;      /* count */
    g_Image[HDR+ENTTAB+1] = 0xFF;   /* moveable */
    g_Image[HDR+ENTTAB+2] = 0;      /* flags */
    g_Image[HDR+ENTTAB+3] = 0xCD;
    g_Image[HDR+ENTTAB+4] = 0x3F;   /* INT 3Fh thunk */
    g_Image[HDR+ENTTAB+5] = 2;      /* segment 2 */
    NeTestWrite16(HDR+ENTTAB+6, 0x1234);  /* offset */

    /* module reference + imported names tables, so an importer can name TESTLIB */
    NeTestWrite16(HDR+0x1E, (WORD)(imports ? 1 : 0));    /* n_mod */
    NeTestWrite16(HDR+0x28, MODTAB);
    NeTestWrite16(HDR+0x2A, IMPTAB);
    NeTestWrite16(HDR+MODTAB, 0);                            /* ref 1 -> imp name at +0 */
    g_Image[HDR+IMPTAB+0] = 7;
    memcpy(g_Image+HDR+IMPTAB+1, "TESTLIB", 7);
    g_Image[HDR+IMPTAB+8] = 3;
    memcpy(g_Image+HDR+IMPTAB+9, "BAR", 3);

    /* seg1 relocation table sits right after its 0x40 bytes of data */
    relocations = segment1Offset + 0x40;
    NeTestWrite16(relocations, (WORD)(imports ? 6 : 4));

    /* (a) SEGMENT, chained: site 0x00 -> 0x02 -> end. target = seg 2 */
    g_Image[relocations+2+0] = NE_ADDR_SEGMENT;
    g_Image[relocations+2+1] = NE_REL_INTERNAL;
    NeTestWrite16(relocations+2+2, 0x0000);
    NeTestWrite16(relocations+2+4, 2);
    NeTestWrite16(relocations+2+6, 0);
    NeTestWrite16(segment1Offset + 0x00, 0x0002);      /* chain link -> next site 0x02 */
    NeTestWrite16(segment1Offset + 0x02, 0xFFFF);      /* end of chain */

    /* (b) FARADDR, single: site 0x10, target = seg 2 : 0x0040 */
    g_Image[relocations+10+0] = NE_ADDR_FARADDR;
    g_Image[relocations+10+1] = NE_REL_INTERNAL;
    NeTestWrite16(relocations+10+2, 0x0010);
    NeTestWrite16(relocations+10+4, 2);
    NeTestWrite16(relocations+10+6, 0x0040);
    NeTestWrite16(segment1Offset + 0x10, 0xFFFF);

    /* (c) OFFSET16, ADDITIVE (must NOT walk a chain): site 0x20 */
    g_Image[relocations+18+0] = NE_ADDR_OFFSET16;
    g_Image[relocations+18+1] = NE_REL_INTERNAL | NE_REL_ADDITIVE;
    NeTestWrite16(relocations+18+2, 0x0020);
    NeTestWrite16(relocations+18+4, 2);
    NeTestWrite16(relocations+18+6, 0x00AA);
    NeTestWrite16(segment1Offset + 0x20, 0x0030);      /* looks like a chain link; must be ignored */
    NeTestWrite16(segment1Offset + 0x30, 0xBEEF);      /* sentinel: must survive untouched */

    /* (d) INTERNALREF to a MOVEABLE target: a=0xFF, b=ordinal 1 */
    g_Image[relocations+26+0] = NE_ADDR_FARADDR;
    g_Image[relocations+26+1] = NE_REL_INTERNAL;
    NeTestWrite16(relocations+26+2, 0x0028);
    NeTestWrite16(relocations+26+4, 0x00FF);
    NeTestWrite16(relocations+26+6, 1);
    NeTestWrite16(segment1Offset + 0x28, 0xFFFF);

    if (!imports)
        return;
    /* (e) IMPORTORDINAL: TESTLIB ordinal 1 -> site 0x34 (0x30 is (c)'s sentinel) */
    g_Image[relocations+34+0] = NE_ADDR_FARADDR;
    g_Image[relocations+34+1] = NE_REL_IMPORTORD;
    NeTestWrite16(relocations+34+2, 0x0034);
    NeTestWrite16(relocations+34+4, 1);
    NeTestWrite16(relocations+34+6, 1);
    NeTestWrite16(segment1Offset + 0x34, 0xFFFF);
    /* (f) IMPORTNAME: TESTLIB "BAR" (imported-names offset 8) -> site 0x3A */
    g_Image[relocations+42+0] = NE_ADDR_FARADDR;
    g_Image[relocations+42+1] = NE_REL_IMPORTNAME;
    NeTestWrite16(relocations+42+2, 0x003A);
    NeTestWrite16(relocations+42+4, 1);
    NeTestWrite16(relocations+42+6, 8);
    NeTestWrite16(segment1Offset + 0x3A, 0xFFFF);
}

INT main(VOID)
{
    NE_MODULE module;

    printf("== WOW: NE loader battery (GH #128/#4) ==\n");

    NeTestBuild(0);
    NeTestCheck(NeParse(&module, g_Image, sizeof g_Image) == 0, "synthetic image parses");
    NeTestCheck(module.SegmentCount == 2, "2 segments");
    NeTestCheck(module.AlignShift == SECSHIFT, "align shift honoured");
    NeTestCheck(module.AutoData == 2, "autodata segment = 2");
    NeTestCheck((module.CsIp >> 16) == 1 && (module.CsIp & 0xFFFF) == 0x10, "CS:IP = seg1:0x0010");
    NeTestCheck(module.TargetOs == 2, "target OS = Windows");
    NeTestCheck(module.Segments[0].FileOffset == 0x200, "seg1 file offset from sector<<shift");
    NeTestCheck(NeSegmentAllocSize(&module.Segments[0]) == 0x80, "seg1 alloc size uses minalloc, not length");

    /* entry table */
    {   WORD segmentNumber = 0, offset = 0;
        NeTestCheck(NeEntryLookup(&module, 1, &segmentNumber, &offset) == 0 && segmentNumber == 2 && offset == 0x1234,
           "moveable entry ordinal 1 -> seg 2:0x1234");
        NeTestCheck(NeEntryLookup(&module, 2, &segmentNumber, &offset) != 0, "ordinal past the end is rejected");
        NeTestCheck(NeEntryLookup(&module, 0, &segmentNumber, &offset) != 0, "ordinal 0 is rejected");
    }

    /* load + relocate */
    memcpy(g_SegmentMemory[0], g_Image + module.Segments[0].FileOffset, module.Segments[0].Length);
    memcpy(g_SegmentMemory[1], g_Image + module.Segments[1].FileOffset, module.Segments[1].Length);
    module.Segments[0].Memory = g_SegmentMemory[0];
    module.Segments[0].Selector = 0x1000;
    module.Segments[1].Memory = g_SegmentMemory[1];
    module.Segments[1].Selector = 0x2000;

    NeTestCheck(NeApplyRelocations(&module, 0, NULL, NULL) == 0, "relocations apply");
    /* 4 records, one of which is a 2-site chain -> 5 sites. "Success" with 0 sites
     * patched would otherwise be indistinguishable from success.
     */
    NeTestCheck(module.Sites == 5, "5 sites patched (4 records, one a 2-link chain)");
    NeTestCheck(NeRead16(g_SegmentMemory[0] + 0x00) == 0x2000, "chained SEGMENT: first site patched");
    NeTestCheck(NeRead16(g_SegmentMemory[0] + 0x02) == 0x2000, "chained SEGMENT: SECOND site patched too");
    NeTestCheck(NeRead16(g_SegmentMemory[0] + 0x10) == 0x0040 && NeRead16(g_SegmentMemory[0] + 0x12) == 0x2000,
       "FARADDR writes off then seg");
    /* [INFO]: 0x0030 was already at the site and 0x00AA is the fixup: ADDITIVE ADDS them.
     * This expectation used to read `== 0x00AA` -- written from memory, and wrong.
     * gdi.exe refuted it: 366 of its records are additive against a zero-valued
     * __MOD_GDI, over sites holding a thunk's API index.
     */
    NeTestCheck(NeRead16(g_SegmentMemory[0] + 0x20) == 0x0030 + 0x00AA,
       "ADDITIVE OFFSET16 ADDS to the value at the site, it does not replace it");
    NeTestCheck(NeRead16(g_SegmentMemory[0] + 0x30) == 0xBEEF,
       "ADDITIVE record did NOT follow the value as a chain link");
    NeTestCheck(NeRead16(g_SegmentMemory[0] + 0x28) == 0x1234 && NeRead16(g_SegmentMemory[0] + 0x2A) == 0x2000,
       "moveable INTERNALREF resolved through the entry table");

    /* [INFO]: RELOCATION IS NOT IDEMPOTENT, and this is the check that says so. A chained
     * record finds its next site by reading the word AT the current site -- which
     * the first pass has just overwritten with an address. Running the pass twice
     * over the same memory therefore follows garbage. Anything that wants to
     * relocate "again later" (against real selectors, say) must reload the segment
     * bytes from the file image first.
     */
    {   UINT32 first = module.Sites;
        module.Sites = 0;
        NeApplyRelocations(&module, 0, NULL, NULL);
        NeTestCheck(module.Sites != first,
           "re-relocating an already-patched segment does NOT reproduce the first pass");
    }

    /* an unresolvable import must FAIL, not quietly leave a far call to nowhere */
    {   NE_MODULE unresolved;
    BYTE segmentCopy[0x200];
        NeTestBuild(0);
        g_Image[0x200 + 0x40 + 2 + 1] = NE_REL_IMPORTORD;   /* record (a) -> import */
        NeParse(&unresolved, g_Image, sizeof g_Image);
        memcpy(segmentCopy, g_Image + unresolved.Segments[0].FileOffset, unresolved.Segments[0].Length);
        unresolved.Segments[0].Memory = segmentCopy;
        unresolved.Segments[0].Selector = 0x1000;
        unresolved.Segments[1].Memory = g_SegmentMemory[1];
        unresolved.Segments[1].Selector = 0x2000;
        NeTestCheck(NeApplyRelocations(&unresolved, 0, NULL, NULL) != 0,
           "unresolved IMPORTORDINAL is refused loudly");
        NeTestCheck(unresolved.Error != 0, "...and records where it gave up");
    }

    /* names, exports, and cross-module imports (synthetic): */
    {   NE_MODULE library, application;
    NE_REGISTRY registry;
    BYTE applicationSegments[2][0x200];
        WORD ordinal = 0;
        WORD segmentNumber = 0;
        WORD segmentOffset = 0;
        CHAR name[NE_MAX_NAME];

        NeTestBuildLibrary();
        NeTestCheck(NeParse(&library, g_LibraryImage, sizeof g_LibraryImage) == 0, "synthetic library parses");
        NeTestCheck((library.ProgramFlags & NE_PROG_LIBRARY) != 0, "...and reports itself a LIBRARY");
        NeTestCheck(NeOwnName(&library, name, sizeof name) == 0 && !strcmp(name, "TESTLIB"),
           "own name comes from resident-names entry 0");
        NeTestCheck(NeExportByName(&library, "FOO", &ordinal) == 0 && ordinal == 1,
           "export by name, RESIDENT table");
        NeTestCheck(NeExportByName(&library, "BAR", &ordinal) == 0 && ordinal == 2,
           "export by name, NON-RESIDENT table (krnl386 puts 312 exports there)");
        NeTestCheck(NeExportByName(&library, "bar", &ordinal) == 0 && ordinal == 2, "...case-insensitive");
        NeTestCheck(NeExportByName(&library, "TESTLIB", &ordinal) != 0,
           "the module's OWN name is not an export");
        NeTestCheck(NeExportByName(&library, "a description", &ordinal) != 0,
           "the non-resident DESCRIPTION is not an export either");
        NeTestCheck(NeExportByName(&library, "NOPE", &ordinal) != 0, "an absent name fails");
        NeTestCheck(NeExportByOrdinal(&library, 1, &segmentNumber, &segmentOffset) == 0 && segmentNumber == 1 && segmentOffset == 0x10,
           "export by ordinal, FIXED entry");
        NeTestCheck(NeExportByOrdinal(&library, 2, &segmentNumber, &segmentOffset) == 0 && segmentNumber == 1 && segmentOffset == 0x20,
           "export by ordinal, MOVEABLE entry resolves past its INT 3Fh thunk");
        NeTestCheck(NeExportByOrdinal(&library, 3, &segmentNumber, &segmentOffset) != 0,
           "an entry WITHOUT the EXPORTED bit is refused");
        NeTestCheck(NeExportByOrdinal(&library, 4, &segmentNumber, &segmentOffset) == 0 && segmentNumber == 0 && segmentOffset == 0xA000,
           "an ABSOLUTE entry (indicator 0xFE) yields seg_no 0 and its constant");

        /* load the library's one segment and give it a selector */
        library.Segments[0].Memory = g_LibrarySegmentMemory;
        library.Segments[0].Selector = 0x3000;
        memcpy(g_LibrarySegmentMemory, g_LibraryImage + library.Segments[0].FileOffset, library.Segments[0].Length);

        NeTestBuild(1);
        NeTestCheck(NeParse(&application, g_Image, sizeof g_Image) == 0, "importer parses");
        NeTestCheck(application.ModuleCount == 1, "importer references 1 module");
        NeTestCheck(NeRefName(&application, 1, name, sizeof name) == 0 && !strcmp(name, "TESTLIB"),
           "module reference 1 names TESTLIB");
        NeTestCheck(NeRefName(&application, 2, name, sizeof name) != 0, "a reference past the end fails");
        NeTestCheck(NeImportedName(&application, 8, name, sizeof name) == 0 && !strcmp(name, "BAR"),
           "imported-names offset 8 reads BAR");

        memcpy(applicationSegments[0], g_Image + application.Segments[0].FileOffset, application.Segments[0].Length);
        memcpy(applicationSegments[1], g_Image + application.Segments[1].FileOffset, application.Segments[1].Length);
        application.Segments[0].Memory = applicationSegments[0];
        application.Segments[0].Selector = 0x1000;
        application.Segments[1].Memory = applicationSegments[1];
        application.Segments[1].Selector = 0x2000;

        memset(&registry, 0, sizeof registry);
        NeTestCheck(NeRegistryAdd(&registry, &library) == 0, "library registers");
        NeTestCheck(NeRegistryFind(&registry, "testlib") == &library, "registry lookup is case-insensitive");
        NeTestCheck(NeRegistryFind(&registry, "KERNEL") == NULL, "an unregistered module is not found");

        NeTestCheck(NeApplyRelocations(&application, 0, NeRegistryResolve, &registry) == 0,
           "imports resolve through the registry");
        NeTestCheck(NeRead16(applicationSegments[0] + 0x34) == 0x0010 && NeRead16(applicationSegments[0] + 0x36) == 0x3000,
           "IMPORTORDINAL patched to TESTLIB's selector:offset");
        NeTestCheck(NeRead16(applicationSegments[0] + 0x3A) == 0x0020 && NeRead16(applicationSegments[0] + 0x3C) == 0x3000,
           "IMPORTNAME resolved via the non-resident table and patched");

        /* the ordering rule, enforced rather than merely documented */
        {   NE_MODULE unassignedLibrary = library;
        NE_MODULE application2;
        BYTE segmentCopy[2][0x200];
        NE_REGISTRY registry2;
            unassignedLibrary.Segments[0].Selector = 0;                       /* selectors not assigned yet */
            NeParse(&application2, g_Image, sizeof g_Image);
            memcpy(segmentCopy[0], g_Image + application2.Segments[0].FileOffset, application2.Segments[0].Length);
            memcpy(segmentCopy[1], g_Image + application2.Segments[1].FileOffset, application2.Segments[1].Length);
            application2.Segments[0].Memory = segmentCopy[0];
            application2.Segments[0].Selector = 0x1000;
            application2.Segments[1].Memory = segmentCopy[1];
            application2.Segments[1].Selector = 0x2000;
            memset(&registry2, 0, sizeof registry2);
            NeRegistryAdd(&registry2, &unassignedLibrary);
            NeTestCheck(NeApplyRelocations(&application2, 0, NeRegistryResolve, &registry2) != 0,
               "relocating BEFORE the target has selectors is refused, not silently 0000:xxxx");
        }

        /* a missing module must name itself -- "KEYBOARD" is exactly what wowexec
         * will hit, and a failure that does not say which module is a dead end
         */
        {   NE_MODULE application3;
        BYTE segmentCopy[2][0x200];
        NE_REGISTRY registry3;
            NeParse(&application3, g_Image, sizeof g_Image);
            memcpy(segmentCopy[0], g_Image + application3.Segments[0].FileOffset, application3.Segments[0].Length);
            memcpy(segmentCopy[1], g_Image + application3.Segments[1].FileOffset, application3.Segments[1].Length);
            application3.Segments[0].Memory = segmentCopy[0];
            application3.Segments[0].Selector = 0x1000;
            application3.Segments[1].Memory = segmentCopy[1];
            application3.Segments[1].Selector = 0x2000;
            memset(&registry3, 0, sizeof registry3);
            NeTestCheck(NeApplyRelocations(&application3, 0, NeRegistryResolve, &registry3) != 0,
               "an import from an unloaded module fails");
            NeTestCheck(!strcmp(registry3.FailedModule, "TESTLIB"), "...and the registry names which module");
        }
    }

    /* malformed input */
    {   NE_MODULE bad;
    BYTE junk[64];
        memset(junk, 0, sizeof junk);
        NeTestCheck(NeParse(&bad, junk, sizeof junk) != 0, "no MZ -> rejected");
        junk[0] = 'M';
        junk[1] = 'Z';
        NeTestCheck(NeParse(&bad, junk, sizeof junk) != 0, "MZ but no NE -> rejected");
    }

    /* real binaries, if the user supplied them:
     * This is the half that matters. Everything above proves the loader does what
     * the loader was written to do; this proves the real WOW binaries agree.
     *
     * The whole set is loaded, given selectors, registered, and relocated in the
     * order the registry documents -- which is the exact sequence the host will
     * run.
     *
     * [INFO]: THIS IS THE ENTIRE XP WOW MODULE GRAPH, and it CLOSES: 15 modules, every
     * import resolved, nothing missing. Earlier the set was five and USER,
     * WOWEXEC and SYSEDIT stopped at SYSTEM, KEYBOARD and SHELL -- so the loader
     * named the three files to go and fetch, and fetching them (off the rig, out
     * of %SystemRoot%\System32) closed every stop with no code change at all.
     * That is the payoff for making a failed import name its module instead of
     * just failing.
     */
    {
        static const struct
        {
            PCSTR Path;
            PCSTR OwnName;
            INT SegmentCount;
            INT MovableCount;
            INT ModuleCount;
            PCSTR MissingModule;
        } realModules[] = {
            { "guest/ne/krnl386.exe",   "KERNEL",    4, 164, 0, NULL },
            { "guest/ne/system.drv",    "SYSTEM",    2,   0, 1, NULL },
            { "guest/ne/keyboard.drv",  "KEYBOARD",  2,   0, 1, NULL },
            { "guest/ne/mouse.drv",     "MOUSE",     2,   5, 0, NULL },
            { "guest/ne/sound.drv",     "SOUND",     2,   0, 1, NULL },
            { "guest/ne/comm.drv",      "COMM",      4,  21, 2, NULL },
            { "guest/ne/gdi.exe",       "GDI",       2, 355, 1, NULL },
            { "guest/ne/user.exe",      "USER",      3,   0, 2, NULL },
            { "guest/ne/shell.dll",     "SHELL",     2,  36, 1, NULL },
            { "guest/ne/toolhelp.dll",  "TOOLHELP",  2,   0, 2, NULL },
            { "guest/ne/winnls.dll",    "WINNLS",    2,  38, 1, NULL },
            { "guest/ne/wifeman.dll",   "WIFEMAN",   2,  86, 1, NULL },
            { "guest/ne/commdlg.dll",   "COMMDLG",   6,  13, 3, NULL },
            { "guest/ne/wowexec.exe",   "WOWEXEC",   2,   2, 4, NULL },
            { "guest/ne/sysedit.exe",   "SYSEDIT",   6,  21, 4, NULL },
        };
        enum
        {
            NREAL = sizeof realModules / sizeof realModules[0]
        };
        static NE_MODULE modules[NREAL];
        INT present[NREAL];
        NE_REGISTRY registry;
        size_t moduleIndex;
        INT index;

        memset(&registry, 0, sizeof registry);
        memset(present, 0, sizeof present);

        /* step 1: parse and load every module's segments */
        for (moduleIndex = 0; moduleIndex < NREAL; ++moduleIndex)
        {
            /* run.sh may invoke us from the repo root or from tests/unit --
             * try both rather than silently SKIPping the most valuable cases.
             */
            FILE *file = fopen(realModules[moduleIndex].Path, "rb");
            PBYTE buffer;
            long size;
            CHAR name[NE_MAX_NAME];
            if (!file)
            {
                CHAR alternatePath[256];
                snprintf(alternatePath, sizeof alternatePath, "../../%s", realModules[moduleIndex].Path);
                file = fopen(alternatePath, "rb");
            }
            if (!file)
            {
                NeTestSkip(realModules[moduleIndex].OwnName);
                continue;
            }
            fseek(file, 0, SEEK_END);
            size = ftell(file);
            fseek(file, 0, SEEK_SET);
            buffer = malloc((size_t)size);
            if (!buffer || fread(buffer, 1, (size_t)size, file) != (size_t)size)
            {
                fclose(file);
                free(buffer);
                NeTestSkip(realModules[moduleIndex].OwnName);
                continue;
            }
            fclose(file);
            printf("  -- %s\n", realModules[moduleIndex].Path);
            NeTestCheck(NeParse(&modules[moduleIndex], buffer, (UINT32)size) == 0, "  parses");
            NeTestCheck(modules[moduleIndex].SegmentCount == realModules[moduleIndex].SegmentCount, "  segment count matches nedump");
            NeTestCheck(modules[moduleIndex].MovableCount == realModules[moduleIndex].MovableCount, "  moveable entry count matches nedump");
            NeTestCheck(modules[moduleIndex].TargetOs == 2, "  targets Windows");
            NeTestCheck(modules[moduleIndex].ModuleCount == realModules[moduleIndex].ModuleCount, "  module reference count matches nedump");
            NeTestCheck(NeOwnName(&modules[moduleIndex], name, sizeof name) == 0 && !strcmp(name, realModules[moduleIndex].OwnName),
               "  own name (NOT the file name)");
            /* LIBRARY vs PROGRAM decides whether its CS:IP may be jumped to at all.
             * Across the whole set exactly two are PROGRAMs -- wowexec (the one WOW
             * actually runs) and sysedit (an ordinary app). Everything else, the
             * kernel included, is a library with SS:SP = 0:0.
             */
            NeTestCheck(((modules[moduleIndex].ProgramFlags & NE_PROG_LIBRARY) != 0) ==
               (strcmp(realModules[moduleIndex].OwnName, "WOWEXEC") != 0 && strcmp(realModules[moduleIndex].OwnName, "SYSEDIT") != 0),
               "  LIBRARY bit agrees with the bootstrap plan");

            for (index = 0; index < (INT)modules[moduleIndex].SegmentCount; ++index)
            {
                PNE_SEGMENT segment = &modules[moduleIndex].Segments[index];
                UINT32 needed = NeSegmentAllocSize(segment);
                segment->Memory = (PBYTE)calloc(1, needed);
                if (segment->Sector)
                    memcpy(segment->Memory, buffer + segment->FileOffset, segment->Length);
                /* step 2: a selector, distinct per module and segment. Real values
                 * come from the LDT on the host; only their distinctness matters.
                 */
                segment->Selector = (WORD)(((moduleIndex + 1) << 8) | ((index + 1) << 3) | 7);
            }
            NeTestCheck(NeRegistryAdd(&registry, &modules[moduleIndex]) == 0, "  registers under its own name");
            present[moduleIndex] = 1;
        }

        if (present[0])
        {
            /* The measured facts that made krnl386 the cheap first milestone. */
            NeTestCheck(modules[0].ModuleCount == 0, "krnl386 imports from NOTHING");
            NeTestCheck((modules[0].CsIp >> 16) == 1 && (modules[0].CsIp & 0xFFFF) == 0xc02b,
               "krnl386 CS:IP = seg1:0xc02b");
            NeTestCheck(modules[0].AlignShift == 4, "krnl386 align shift 4");
            /* ...and the one export every by-name lookup was built for. */
            {   WORD exportOrdinal = 0;
                NeTestCheck(NeExportByName(&modules[0], "GETWOWCOMPATFLAGSEX", &exportOrdinal) == 0 && exportOrdinal == 521,
                   "KERNEL.GETWOWCOMPATFLAGSEX @521 -- and ONLY the non-resident table has it");
                NeTestCheck(NeExportByName(&modules[0], "GLOBALALLOC", &exportOrdinal) == 0,
                   "KERNEL.GLOBALALLOC resolves by name");
            }
            /* The 30 absolute exports, which is what indicator 0xFE turned out to be.
             * Values, not addresses -- and gdi/user import 810 sites' worth.
             */
            {   WORD exportOrdinal = 0, segmentNumber = 0, segmentOffset = 0;
                NeTestCheck(NeExportByName(&modules[0], "__AHINCR", &exportOrdinal) == 0 &&
                   NeExportByOrdinal(&modules[0], exportOrdinal, &segmentNumber, &segmentOffset) == 0 && segmentNumber == 0 && segmentOffset == 8,
                   "KERNEL.__AHINCR is an ABSOLUTE whose value is 8");
                NeTestCheck(NeExportByName(&modules[0], "__A000H", &exportOrdinal) == 0 &&
                   NeExportByOrdinal(&modules[0], exportOrdinal, &segmentNumber, &segmentOffset) == 0 && segmentNumber == 0 && segmentOffset == 0xA000,
                   "KERNEL.__A000H is an ABSOLUTE whose value is 0xA000");
                NeTestCheck(NeExportByName(&modules[0], "__MOD_GDI", &exportOrdinal) == 0 && exportOrdinal == 574,
                   "KERNEL.__MOD_GDI @574 -- the export that made gdi.exe fail as 'segment 254'");
            }
        }

        /* step 3: relocate. Every import that can be satisfied, must be. */
        for (moduleIndex = 0; moduleIndex < NREAL; ++moduleIndex)
        {
            CHAR description[128];
            INT status = 0;
            if (!present[moduleIndex])
                continue;
            modules[moduleIndex].Sites = 0;
            for (index = 0; index < (INT)modules[moduleIndex].SegmentCount && status == 0; ++index)
                status = NeApplyRelocations(&modules[moduleIndex], index, NeRegistryResolve, &registry);
            if (realModules[moduleIndex].MissingModule)
            {
                snprintf(description, sizeof description, "%s: relocation stops at %s, the module we "
                         "did not load", realModules[moduleIndex].OwnName, realModules[moduleIndex].MissingModule);
                NeTestCheck(status != 0 && !strcmp(registry.FailedModule, realModules[moduleIndex].MissingModule), description);
            }
            else
            {
                /* "Resolved everything" is indistinguishable from "did nothing"
                 * unless the sites are counted -- but a module with no relocation
                 * records at all is legitimately zero. mouse.drv is exactly that:
                 * 2 segments, no relocs, imports nothing. So ask the segments what
                 * to expect rather than assuming every module has fixups.
                 */
                INT hasRelocations = 0;
                for (index = 0; index < (INT)modules[moduleIndex].SegmentCount; ++index)
                    if ((modules[moduleIndex].Segments[index].Flags & NE_SEG_RELOCS) && modules[moduleIndex].Segments[index].Sector)
                        hasRelocations = 1;
                snprintf(description, sizeof description,
                         "%s: EVERY relocation resolved (%u sites)", realModules[moduleIndex].OwnName, modules[moduleIndex].Sites);
                NeTestCheck(status == 0, description);
                NeTestCheck(hasRelocations ? modules[moduleIndex].Sites > 0 : modules[moduleIndex].Sites == 0,
                   hasRelocations ? "  ...and it patched something"
                              : "  ...and it has no relocation records, so zero is right");
            }
        }
    }

    printf("\n%d checks, %d failed, %d skipped\n", g_Passes + g_Failures, g_Failures, g_Skips);
    return g_Failures ? 1 : 0;
}
