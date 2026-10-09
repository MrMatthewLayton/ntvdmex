/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * 16-bit New Executable loader (GH #128 / #4).
 *
 * The image format every 16-bit Windows program uses, and the first brick of the WOW
 * layer. Header-only and free of any Windows or VDM dependency, by the same convention
 * as src/dos/ -- so the whole thing is exercised off-VM by tests/unit/ne_test.c in
 * milliseconds instead of a round trip to the rig.
 *
 * WRITTEN AGAINST MEASURED BINARIES, NOT THE SPEC:
 * `tools/ne/nedump.py` was built first and pointed at the real files. What they
 * actually contain decided what is implemented here and in what order:
 *
 * krnl386.exe   4 segs, 13 relocs, ALL of one kind (INTERNALREF/SEGMENT, chained,
 *               targeting segment 1), and it IMPORTS FROM NOTHING -- it is the
 *               kernel. This is what `ntvdm -a ...\krnl386.exe` asks WOW to boot.
 * sysedit.exe   6 segs, 179 relocs over five kinds, importing from KERNEL, GDI,
 *               USER and SHELL. 147 are IMPORTORDINAL/FAR_ADDR -- ordinary API
 *               calls by ordinal, which is the gate to running real programs.
 *
 * So a loader that handles INTERNALREF is already enough to place krnl386 in memory,
 * and imports can arrive later without restructuring anything.
 *
 * [CAUTION]: TWO THINGS THE FORMAT DOES THAT ARE EASY TO GET WRONG, AND BOTH ARE HERE:
 * 1. RELOCATION CHAINS. A record does not name one site. Unless the ADDITIVE bit is
 *    set, the word AT the site holds the offset of the NEXT site to patch, and the
 *    chain ends at 0xFFFF. Treating each record as a single fixup silently leaves
 *    most of a segment unrelocated.
 * 2. MOVEABLE TARGETS. An INTERNALREF with a==0xFF does not name a segment; it names
 *    an ENTRY TABLE ORDINAL, which must be looked up to get the real segment:offset.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_NE_H
#define NTVDMEX_NE_H

#include "ne_format.h"

#define NE_MAX_SEG          96
#define NE_MAX_MOD          24      /* The whole XP WOW set is 15; leave room above it */
#define NE_MAX_NAME         32

/* NE_MODULE.prog_flags */
#define NE_PROG_LIBRARY     0x8000  /* ...so CS:IP is a DLL *init* entry, not a start */

/* entry-table flags byte */
#define NE_ENT_EXPORTED     0x01
#define NE_ENT_SHAREDDATA   0x02

/* entry-table bundle segment indicators */
#define NE_ENT_ABSOLUTE     0xFE    /* The "offset" IS the value; there is no segment */
#define NE_ENT_MOVEABLE     0xFF

/* NE_SEGMENT.flags */
#define NE_SEG_DATA         0x0001
#define NE_SEG_MOVEABLE     0x0010
#define NE_SEG_PRELOAD      0x0040
#define NE_SEG_RELOCS       0x0100

/* relocation record: low 2 bits of rel_type */
#define NE_REL_INTERNAL     0
#define NE_REL_IMPORTORD    1
#define NE_REL_IMPORTNAME   2
#define NE_REL_OSFIXUP      3
#define NE_REL_ADDITIVE     0x04    /* ...so do NOT walk a chain */

/* relocation record: addr_type -- what shape of thing is at the site */
#define NE_ADDR_LOBYTE      0
#define NE_ADDR_SEGMENT     2       /* 16-bit segment/selector */
#define NE_ADDR_FARADDR     3       /* 32-bit off:seg */
#define NE_ADDR_OFFSET16    5       /* 16-bit offset */
#define NE_ADDR_FARADDR48   11      /* 48-bit off32:seg (386) */
#define NE_ADDR_OFFSET32    13

typedef struct _NE_SEGMENT
{
    WORD Sector;
    WORD Flags;
    WORD MinAlloc;
    UINT32 FileOffset;
    UINT32 Length;
    WORD Selector;          /* runtime segment/selector -- filled by the caller */
    PBYTE Memory;          /* host pointer to this segment's loaded bytes */
} NE_SEGMENT, *PNE_SEGMENT; typedef const NE_SEGMENT *PCNE_SEGMENT;

typedef struct _NE_MODULE
{
    PCBYTE Image;
    UINT32 ImageLength;
    UINT32 Header;
    WORD ProgramFlags;
    WORD AutoData;
    WORD Heap;
    WORD Stack;
    UINT32 CsIp;
    UINT32 SsSp;
    WORD SegmentCount;
    WORD ModuleCount;
    WORD MovableCount;
    WORD AlignShift;
    WORD ExpectedVersion;
    WORD EntryTable;
    WORD EntryLength;
    WORD SegmentTable;
    WORD ResourceTable;
    WORD ResidentTable;
    WORD ModuleTable;
    WORD ImportTable;
    /* [CAUTION]: The NON-resident names table offset is an ABSOLUTE file offset and a DWORD,
     * unlike every other table offset in this header, which is a WORD relative to
     * the NE header. Reading it the same way as its neighbours lands in nothing.
     */
    UINT32 NonResidentOffset;
    WORD NonResidentLength;
    BYTE TargetOs;
    BYTE OtherFlags;
    NE_SEGMENT   Segments[NE_MAX_SEG];
    INT      Error;          /* 0 = ok; otherwise the __LINE__ that rejected it */
    /* [CAUTION]: How many SITES were actually patched, accumulated across segments. A
     * relocation pass that returns "success" having done nothing is
     * indistinguishable from one that worked, and the guest only finds out
     * later and somewhere else. Count it, print it, and assert on it.
     */
    UINT32 Sites;
} NE_MODULE, *PNE_MODULE; typedef const NE_MODULE *PCNE_MODULE;

/* Resolve an imported entry point. Returns 0 on success and fills seg:off.
 * `module` is the module DOING the importing -- the resolver needs it to turn `moduleReference` into a
 * module name and `ordinalOrName` into a string, both of which live in that module's own
 * tables. `moduleReference` is a 1-based index into the module reference table. When `isByName` is
 * 0, `ordinalOrName` is an ordinal; when 1, it is an offset into the imported-names
 * table. See NeRegistryResolve for the implementation WOW uses.
 */
typedef INT (*PNE_IMPORT)(PVOID context, const NE_MODULE *module, WORD moduleReference,
                            WORD ordinalOrName, INT isByName,
                            PWORD selector, PWORD offset);

/* THE REGISTRY: resolving imports BETWEEN loaded modules:
 * A relocation record names a module by a per-module reference index and an export
 * by ordinal (or by name). Turning that into an address needs the OTHER module, so
 * something has to hold them all. That is this.
 * Keyed on each module's OWN name from its resident table -- KERNEL, USER, GDI --
 * because that is the name importers use, and it is not the file name (krnl386.exe
 * is KERNEL).
 *
 * [CAUTION]: ORDERING, AND IT IS NOT OPTIONAL. A resolved import is written as
 * target-selector : offset, so every module's runtime `Selector` values must be FINAL
 * before ANY module is relocated. The sequence is:
 *    1. NeParse + copy segment bytes, for every module
 *    2. assign every segment its selector
 *    3. NeApplyRelocations, for every module
 * Relocating in step 1 and hoping to redo it later works only because relocation
 * records are read from the untouched file image, not from the patched segment --
 * but a chained record walks links THROUGH the segment, and the first pass
 * overwrites those links with addresses. So a second pass over an already-patched
 * segment follows garbage. Load once, select, then relocate once.
 *
 * [CAUTION]: MOVEABLE EXPORTS ARE RESOLVED DIRECT, NOT THROUGH THEIR THUNK. Every moveable
 * entry in the real binaries begins `CD 3F` (INT 3Fh) -- Windows hands importers
 * the address of that 3-byte thunk so the kernel can fault a discarded segment in
 * on first call, then rewrite the thunk as a direct jump. We do not move or discard
 * segments, so there is nothing to fault in and the indirection would buy only a
 * per-call interrupt. Resolving straight to segment:offset is therefore correct
 * HERE and would stop being correct the moment segment discarding is implemented.
 * Written down because the day that changes, this is the line that breaks.
 */
typedef struct _NE_REGISTRY
{
    PNE_MODULE Modules[NE_MAX_MOD];
    char       Names[NE_MAX_MOD][NE_MAX_NAME];  /* char, not CHAR: the spelling moves code (#333) */
    INT        Count;
    /* Diagnostics for the failure that actually happens: which import gave up. */
    char FailedModule[NE_MAX_NAME];
    char FailedFunction[NE_MAX_NAME];
    WORD   FailedOrdinal;
} NE_REGISTRY, *PNE_REGISTRY; typedef const NE_REGISTRY *PCNE_REGISTRY;

/* little-endian readers, bounds-checked: */
WORD NeRead16(PCBYTE bytes);

/* parse:
 * MZ at 0, a LONG at 0x3C giving the second header, and 'NE' there. Field offsets
 * are spelled out because getting one wrong shifts every table after it and the
 * failure looks like corrupt data rather than a bad constant -- which is exactly
 * what happened while writing nedump.py.
 */
INT NeParse(PNE_MODULE module, PCBYTE image, UINT32 length);

INT NeEntryLookup(PCNE_MODULE module, WORD ordinal, PWORD segmentNumber, PWORD offset);

/* The module's own name, from resident-names entry 0. This is the name OTHER modules
 * import it by -- and it is NOT the file name: krnl386.exe calls itself KERNEL.
 */
INT NeOwnName(PCNE_MODULE module, PSTR output, INT capacity);

/* Name of the `reference`th (1-based) module this one imports from. */
INT NeRefName(PCNE_MODULE module, WORD reference, PSTR output, INT capacity);

/* A function name from the imported-names table, given the offset a relocation
 * record carries in `fieldB` for an IMPORTNAME fixup.
 */
INT NeImportedName(PCNE_MODULE module, WORD nameOffset, PSTR output, INT capacity);

INT NeExportByName(PCNE_MODULE module, PCSTR name, PWORD ordinal);

/* Resolve an EXPORT of this module to segment number + offset. *seg_no == 0 on
 * return means the export is an ABSOLUTE constant and *off is its value.
 *
 * [CAUTION]: Refuses an ordinal whose entry lacks the EXPORTED bit. Measured: all 190 distinct
 * ordinals the four real importers ask for have it, and 14 of sysedit's 21 entries
 * do NOT -- so the bit is meaningful and checking it turns "imported a private
 * entry" from a far call into rubbish into a load-time error.
 */
INT NeExportByOrdinal(PCNE_MODULE module, WORD ordinal, PWORD segmentNumber, PWORD offset);

/* relocations:
 * Applied to segment `index` (0-based) after its bytes are in Segments[index].Memory and every
 * segment's runtime `Selector` value is known. The record table lives immediately AFTER
 * the segment's file data: a WORD count, then 8 bytes each.
 */
INT NeApplyRelocations(PNE_MODULE module, INT index, PNE_IMPORT importer, PVOID context);

INT NeRegistryAdd(PNE_REGISTRY registry, PNE_MODULE module);

PNE_MODULE NeRegistryFind(PCNE_REGISTRY registry, PCSTR name);

INT NeRegistryResolve(
    PVOID context,
    PCNE_MODULE module,
    WORD moduleReference,
    WORD ordinalOrName,
    INT isByName,
    PWORD selector,
    PWORD offset);

/* Convenience: how many bytes a segment needs in memory (minalloc can exceed the
 * file length -- BSS-style tail that must be present and zeroed).
 */
UINT32 NeSegmentAllocSize(PCNE_SEGMENT segment);

#endif /* NTVDMEX_NE_H */
