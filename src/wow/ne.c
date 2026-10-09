/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The 16-bit New Executable loader: parse a module, resolve entry points and names, relocate segments, and keep the module registry.
 *
 * The function definitions of ne.h, which keeps their declarations and doc comments (#335).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "ne.h"

WORD NeRead16(PCBYTE bytes)
{
    return (WORD)(bytes[0] | (bytes[1] << BYTE_SHIFT));
}

static UINT32 NeRead32(PCBYTE bytes)
{
    return (UINT32)bytes[0] | ((UINT32)bytes[1] << BYTE_SHIFT)
         | ((UINT32)bytes[2] << WORD_SHIFT) | ((UINT32)bytes[3] << TOP_BYTE_SHIFT);
}

static VOID NeWrite16(PBYTE bytes, WORD value)
{
    bytes[0] = (BYTE)value;
    bytes[1] = (BYTE)(value >> BYTE_SHIFT);
}

static INT NeInBounds(PCNE_MODULE module, UINT32 offset, UINT32 length)
{
    return offset <= module->ImageLength && length <= module->ImageLength - offset;
}

INT NeParse(PNE_MODULE module, PCBYTE image, UINT32 length)
{
    UINT32 header;
    WORD index;
    INT byteIndex;
    for (byteIndex = 0; byteIndex < (INT)sizeof *module; ++byteIndex) ((PBYTE)module)[byteIndex] = 0;
    module->Image = image; module->ImageLength = length;

    if (length < NE_MZ_HEADER_SIZE || image[0] != 'M' || image[1] != 'Z')
    {
        module->Error = __LINE__;
        return -1;
    }
    header = NeRead32(image + NE_MZ_LFANEW);
    if (!NeInBounds(module, header, NE_HEADER_SIZE))
    {
        module->Error = __LINE__;
        return -1;
    }
    if (image[header] != 'N' || image[header + 1] != 'E')
    {
        module->Error = __LINE__;
        return -1;
    }
    module->Header = header;

    module->EntryTable    = NeRead16(image + header + NE_HDR_ENTRY_TABLE);
    module->EntryLength    = NeRead16(image + header + NE_HDR_ENTRY_LENGTH);
    module->ProgramFlags   = NeRead16(image + header + NE_HDR_PROGRAM_FLAGS);
    module->AutoData     = NeRead16(image + header + NE_HDR_AUTODATA);
    module->Heap         = NeRead16(image + header + NE_HDR_HEAP);
    module->Stack        = NeRead16(image + header + NE_HDR_STACK);
    module->CsIp         = NeRead32(image + header + NE_HDR_CSIP);
    module->SsSp         = NeRead32(image + header + NE_HDR_SSSP);
    module->SegmentCount        = NeRead16(image + header + NE_HDR_SEGMENT_COUNT);
    module->ModuleCount        = NeRead16(image + header + NE_HDR_MODULE_COUNT);
    module->NonResidentLength   = NeRead16(image + header + NE_HDR_NONRESIDENT_LENGTH);
    module->NonResidentOffset   = NeRead32(image + header + NE_HDR_NONRESIDENT_OFFSET);      /* ABSOLUTE, and a DWORD */
    module->SegmentTable      = NeRead16(image + header + NE_HDR_SEGMENT_TABLE);
    module->ResourceTable      = NeRead16(image + header + NE_HDR_RESOURCE_TABLE);
    module->ResidentTable = NeRead16(image + header + NE_HDR_RESIDENT_TABLE);
    module->ModuleTable      = NeRead16(image + header + NE_HDR_MODULE_TABLE);
    module->ImportTable      = NeRead16(image + header + NE_HDR_IMPORT_TABLE);
    module->MovableCount    = NeRead16(image + header + NE_HDR_MOVABLE_COUNT);
    module->AlignShift  = NeRead16(image + header + NE_HDR_ALIGN_SHIFT);
    module->TargetOs    = image[header + NE_HDR_TARGET_OS];
    module->OtherFlags  = image[header + NE_HDR_OTHER_FLAGS];
    module->ExpectedVersion   = NeRead16(image + header + NE_HDR_EXPECTED_VERSION);

    if (!module->AlignShift) module->AlignShift = NE_DEFAULT_ALIGN_SHIFT;          /* 0 means 512, not 1 */
    if (module->SegmentCount > NE_MAX_SEG)
    {
        module->Error = __LINE__;
        return -1;
    }
    if (!NeInBounds(module, header + module->SegmentTable, (UINT32)module->SegmentCount * NE_SEGENT_SIZE))
    {
        module->Error = __LINE__;
        return -1;
    }

    for (index = 0; index < module->SegmentCount; ++index)
    {
        PCBYTE entry = image + header + module->SegmentTable + index * NE_SEGENT_SIZE;
        PNE_SEGMENT segment = &module->Segments[index];
        segment->Sector   = NeRead16(entry);
        segment->Length   = NeRead16(entry + NE_SEGENT_LENGTH);
        segment->Flags    = NeRead16(entry + NE_SEGENT_FLAGS);
        segment->MinAlloc = NeRead16(entry + NE_SEGENT_MINALLOC);
        /* A zero length means 64K -- but only if the segment has file data at all. */
        if (!segment->Length && segment->Sector) segment->Length = NE_SEGMENT_64K;
        segment->FileOffset = (UINT32)segment->Sector << module->AlignShift;
        if (segment->Sector && !NeInBounds(module, segment->FileOffset, segment->Length))
        {
            module->Error = __LINE__;
            return -1;
        }
    }
    return 0;
}

/* entry table:
 * Bundles of entries, ordinals numbered sequentially from 1 across all bundles.
 * Each bundle: BYTE count, BYTE segment indicator.
 *   indicator 0     -> end of table
 *   indicator 0xFF  -> MOVEABLE: 6 bytes each (flags, INT 3Fh thunk, segno, offset)
 *   indicator 0xFE  -> ABSOLUTE: 3 bytes (flags, VALUE) -- see below
 *   otherwise       -> FIXED in that segment: 3 bytes each (flags, offset)
 * Needed because an INTERNALREF with a==0xFF targets an ORDINAL, not a segment.
 *
 * [INFO]: 0xFE IS NOT A SEGMENT NUMBER, AND READING IT AS ONE IS A REAL FAILURE, NOT A
 * THEORETICAL ONE. krnl386 has 30 such entries and they are the classic Win16
 * absolute exports -- __AHSHIFT=3, __AHINCR=8, __A000H=0xA000, __0040H=0x0040,
 * __WINFLAGS, and the __MOD_* module handles. Treated as "segment 254" they are
 * rejected against a 4-segment module, which is exactly how gdi.exe's relocation
 * pass first failed here: 366 of its records import __MOD_GDI.
 * `*segmentNumber` comes back 0 -- segments are numbered from 1, so 0 is free to mean
 * "no segment: *off is the whole answer".
 */
static INT NeEntryLookupEx(PCNE_MODULE module, WORD ordinal,
                              PWORD segmentNumber, PWORD offset, PBYTE entryFlags)
{
    UINT32 position = module->Header + module->EntryTable;
    UINT32 end = position + module->EntryLength;
    WORD current = 1;
    if (!ordinal || !NeInBounds(module, position, module->EntryLength)) return -1;
    while (position + NE_BUNDLE_HEADER_SIZE <= end)
    {
        BYTE count = module->Image[position], indicator = module->Image[position + 1];
        UINT32 record = position + NE_BUNDLE_HEADER_SIZE, step;
        if (!count) break;                       /* count 0 terminates */
        step = (indicator == NE_ENT_MOVEABLE) ? NE_ENTRY_MOVEABLE_SIZE : (indicator == 0 ? 0u : NE_ENTRY_FIXED_SIZE);
        if (!step) /* null bundle */
        {
            position = record;
            current = (WORD)(current + count);
            continue;
        }
        if (ordinal >= current && ordinal < current + count)
        {
            UINT32 entry = record + (UINT32)(ordinal - current) * step;
            if (!NeInBounds(module, entry, step)) return -1;
            if (entryFlags) *entryFlags = module->Image[entry];
            if (indicator == NE_ENT_MOVEABLE)      { *segmentNumber = module->Image[entry + NE_ENTRY_MOVEABLE_SEGMENT];
                                               *offset = NeRead16(module->Image + entry + NE_ENTRY_MOVEABLE_OFFSET); }
            else if (indicator == NE_ENT_ABSOLUTE) { *segmentNumber = 0;
                                               *offset = NeRead16(module->Image + entry + 1); }
            else                             { *segmentNumber = indicator;
                                               *offset = NeRead16(module->Image + entry + 1); }
            return 0;
        }
        current = (WORD)(current + count);
        position = record + (UINT32)count * step;
    }
    return -1;
}

INT NeEntryLookup(PCNE_MODULE module, WORD ordinal,
                           PWORD segmentNumber, PWORD offset)
{
    return NeEntryLookupEx(module, ordinal, segmentNumber, offset, 0);
}

/* names:
 * Four tables, all built from length-prefixed (Pascal) strings that are NOT
 * terminated, so every read here is by length and every result is terminated by us:
 *
 *   RESIDENT names   (hdr-relative)  entry 0 is the MODULE'S OWN NAME; the rest are
 *                                    exports that stay in memory.
 *   NON-RESIDENT     (ABSOLUTE)      entry 0 is the module DESCRIPTION, not an
 *                                    export; the rest are exports Windows is free to
 *                                    discard. Both entry-0s carry ordinal 0.
 *   MODULE REF       (hdr-relative)  n_mod WORDs, each an offset into...
 *   IMPORTED names   (hdr-relative)  ...which holds the names of modules imported
 *                                    FROM, and of functions imported BY NAME.
 *
 * [INFO]: MEASURED, AND IT DECIDES THE SEARCH ORDER: user.exe imports GETWOWCOMPATFLAGSEX
 * from KERNEL by NAME, and that export is in krnl386's NON-RESIDENT table (@521),
 * not its resident one. A by-name lookup that stops at the resident table finds 0 of
 * krnl386's 312 non-resident exports and fails on the very first real binary.
 */
static INT NeNameAt(PCNE_MODULE module, UINT32 offset, PSTR output, INT capacity)
{
    UINT32 length;
    UINT32 index;
    if (capacity <= 0) return -1;
    output[0] = 0;
    if (!NeInBounds(module, offset, 1)) return -1;
    length = module->Image[offset];
    if (!NeInBounds(module, offset + 1, length) || (INT)length >= capacity) return -1;
    for (index = 0; index < length; ++index) output[index] = (CHAR)module->Image[offset + 1 + index];
    output[length] = 0;
    return 0;
}

static CHAR NeUpper(CHAR character)
{
    return (character >= 'a' && character <= 'z') ? (CHAR)(character - ASCII_CASE_BIT) : character;
}

static INT NeEqualIgnoreCase(PCSTR left, PCSTR right)
{
    while (*left && *right)
    {
        if (NeUpper(*left) != NeUpper(*right)) return 0;
        ++left;
        ++right;
    }
    return *left == *right;
}

INT NeOwnName(PCNE_MODULE module, PSTR output, INT capacity)
{
    return NeNameAt(module, module->Header + module->ResidentTable, output, capacity);
}

INT NeRefName(PCNE_MODULE module, WORD reference, PSTR output, INT capacity)
{
    UINT32 entry;
    if (capacity > 0) output[0] = 0;
    if (!reference || reference > module->ModuleCount) return -1;
    entry = module->Header + module->ModuleTable + (UINT32)(reference - 1) * NE_WORD_BYTES;
    if (!NeInBounds(module, entry, NE_WORD_BYTES)) return -1;
    return NeNameAt(module, module->Header + module->ImportTable + NeRead16(module->Image + entry), output, capacity);
}

INT NeImportedName(PCNE_MODULE module, WORD nameOffset, PSTR output, INT capacity)
{
    return NeNameAt(module, module->Header + module->ImportTable + nameOffset, output, capacity);
}

/* Walk one name table. `offset`/`length` bound it; `skip_first` drops the module-name or
 * description entry. Returns 0 and sets *ordinal when `wanted` matches.
 */
static INT NeNamesFind(PCNE_MODULE module, UINT32 offset, UINT32 length,
                         PCSTR wanted, PWORD ordinal)
{
    UINT32 position = offset, end = offset + length;
    INT isFirst = 1;
    if (!length || !NeInBounds(module, offset, length)) return -1;
    while (position + NE_NAME_MIN_RECORD <= end)
    {
        CHAR name[NE_MAX_NAME];
        UINT32 nameLength = module->Image[position];
        if (!nameLength) break;                                  /* a zero length terminates */
        if (!NeInBounds(module, position + 1, nameLength + NE_ORDINAL_BYTES)) return -1;
        if (NeNameAt(module, position, name, sizeof name) == 0 && !isFirst && NeEqualIgnoreCase(name, wanted))
        {
            *ordinal = NeRead16(module->Image + position + 1 + nameLength);
            return 0;
        }
        isFirst = 0;
        position += 1 + nameLength + NE_ORDINAL_BYTES;
    }
    return -1;
}

INT NeExportByName(PCNE_MODULE module, PCSTR name, PWORD ordinal)
{
    /* The resident table has no length field -- it runs to its own zero terminator, so
     * bound it by the rest of the image and let the walk stop itself.
     */
    UINT32 residentTable = module->Header + module->ResidentTable;
    if (residentTable < module->ImageLength && NeNamesFind(module, residentTable, module->ImageLength - residentTable, name, ordinal) == 0)
        return 0;
    return NeNamesFind(module, module->NonResidentOffset, module->NonResidentLength, name, ordinal);
}

INT NeExportByOrdinal(PCNE_MODULE module, WORD ordinal,
                                PWORD segmentNumber, PWORD offset)
{
    BYTE flags = 0;
    if (NeEntryLookupEx(module, ordinal, segmentNumber, offset, &flags) != 0) return -1;
    if (!(flags & NE_ENT_EXPORTED)) return -1;
    return 0;
}

INT NeApplyRelocations(PNE_MODULE module, INT index, PNE_IMPORT importer, PVOID context)
{
    PNE_SEGMENT segment = &module->Segments[index];
    UINT32 position, count, recordIndex;
    if (!(segment->Flags & NE_SEG_RELOCS) || !segment->Sector) return 0;
    position = segment->FileOffset + segment->Length;
    if (!NeInBounds(module, position, NE_WORD_BYTES))
    {
        module->Error = __LINE__;
        return -1;
    }
    count = NeRead16(module->Image + position); position += NE_WORD_BYTES;
    if (!NeInBounds(module, position, count * NE_RELOC_SIZE))
    {
        module->Error = __LINE__;
        return -1;
    }

    for (recordIndex = 0; recordIndex < count; ++recordIndex)
    {
        PCBYTE record = module->Image + position + recordIndex * NE_RELOC_SIZE;
        BYTE  addressType = record[0], relocationType = record[1];
        WORD site = NeRead16(record + NE_RELOC_SITE), fieldA = NeRead16(record + NE_RELOC_TARGET_A), fieldB = NeRead16(record + NE_RELOC_TARGET_B);
        WORD targetSelector = 0, targetOffset = 0;

        switch (relocationType & NE_REL_TYPE_MASK)
        {
        case NE_REL_INTERNAL:
            if ((fieldA & BYTE_MASK) == NE_ENT_MOVEABLE)             /* target names an entry ordinal */
            {
                WORD segmentNumber;
                if (NeEntryLookup(module, fieldB, &segmentNumber, &targetOffset) != 0)
                {
                    module->Error = __LINE__;
                    return -1;
                }
                if (!segmentNumber || segmentNumber > module->SegmentCount)
                {
                    module->Error = __LINE__;
                    return -1;
                }
                targetSelector = module->Segments[segmentNumber - 1].Selector;
            }
            else
            {
                if (!fieldA || fieldA > module->SegmentCount)
                {
                    module->Error = __LINE__;
                    return -1;
                }
                targetSelector = module->Segments[fieldA - 1].Selector;
                targetOffset = fieldB;
            }
            break;
        case NE_REL_IMPORTORD:
        case NE_REL_IMPORTNAME:
            /* Not resolvable without the exporting module. LOUD, not silent: a
             * relocation left unapplied is a far call into nothing, and the guest
             * dies far away from here with no clue why.
             */
            if (!importer || importer(context, module, fieldA, fieldB, (relocationType & NE_REL_TYPE_MASK) == NE_REL_IMPORTNAME,
                            &targetSelector, &targetOffset) != 0)
            {
                module->Error = __LINE__;
                return -1;
            }
            break;
        default:
            module->Error = __LINE__; return -1;       /* OSFIXUP: none seen; refuse it */
        }

        /* Walk the chain (or patch once, if ADDITIVE).
         *
         * [INFO]: ADDITIVE MEANS *ADD*, NOT REPLACE, AND THE BINARIES SETTLE IT. Every
         * ADDITIVE record in the corpus is an OFFSET16 import of a __MOD_* absolute,
         * whose value is 0 -- so the fixup itself is indistinguishable either way.
         * What is NOT indistinguishable is the word already at each site: gdi.exe's
         * 366 sites hold 0x7b, 0x7c, 0x7d, 0x7e, 0x97, 0xaf... all different, and the
         * bytes around one read
         *       68 7e 00        push 0x007e        <- the site
         *       9a ff ff 00 00  call far <KERNEL>
         *       6a 06           push 6
         * i.e. a WOW thunk table whose pushed word is the API index. Replacing would
         * make every one `push 0` and send all 810 of gdi's and user's calls to
         * function zero -- a guest that starts and then behaves like nothing on
         * earth. Adding leaves them alone, which is what a zero addend should do.
         *
         * [CAUTION]: Only OFFSET16-additive occurs in the corpus. A segment/selector is not a
         * number you can add to, so the segment halves stay REPLACE; if a binary ever
         * turns up with an additive SEGMENT record, this is the decision to revisit.
         */
        for (;;)
        {
            WORD next;
            INT isAdditive = (relocationType & NE_REL_ADDITIVE) != 0;
            if (site + NE_WORD_BYTES_U > segment->Length) break;    /* a chain may run off the end */
            next = NeRead16(segment->Memory + site);
            switch (addressType)
            {
            case NE_ADDR_SEGMENT:  NeWrite16(segment->Memory + site, targetSelector); break;
            case NE_ADDR_OFFSET16:
                NeWrite16(segment->Memory + site, (WORD)(isAdditive ? next + targetOffset : targetOffset));
                break;
            case NE_ADDR_FARADDR:
                if (site + NE_FARADDR_BYTES > segment->Length) break;
                NeWrite16(segment->Memory + site, (WORD)(isAdditive ? next + targetOffset : targetOffset));
                NeWrite16(segment->Memory + site + NE_WORD_BYTES, targetSelector);
                break;
            case NE_ADDR_LOBYTE:
                segment->Memory[site] = (BYTE)(isAdditive ? segment->Memory[site] + targetOffset : targetOffset);
                break;
            default:               module->Error = __LINE__; return -1;
            }
            ++module->Sites;
            if (isAdditive) break;                      /* additive records are not chained */
            if (next == NE_CHAIN_END) break;
            site = next;
        }
    }
    return 0;
}

INT NeRegistryAdd(PNE_REGISTRY registry, PNE_MODULE module)
{
    if (registry->Count >= NE_MAX_MOD) return -1;
    if (NeOwnName(module, registry->Names[registry->Count], NE_MAX_NAME) != 0) return -1;
    registry->Modules[registry->Count] = module;
    ++registry->Count;
    return 0;
}

PNE_MODULE NeRegistryFind(PCNE_REGISTRY registry, PCSTR name)
{
    INT index;
    for (index = 0; index < registry->Count; ++index) if (NeEqualIgnoreCase(registry->Names[index], name)) return registry->Modules[index];
    return 0;
}

INT NeRegistryResolve(PVOID context, PCNE_MODULE module, WORD moduleReference,
                               WORD ordinalOrName, INT isByName,
                               PWORD selector, PWORD offset)
{
    PNE_REGISTRY registry = (PNE_REGISTRY)context;
    CHAR moduleName[NE_MAX_NAME], functionName[NE_MAX_NAME];
    PNE_MODULE target;
    WORD ordinal = ordinalOrName, segmentNumber = 0, segmentOffset = 0;
    INT index;

    functionName[0] = 0;
    registry->FailedModule[0] = 0; registry->FailedFunction[0] = 0; registry->FailedOrdinal = 0;
    if (NeRefName(module, moduleReference, moduleName, sizeof moduleName) != 0) return -1;
    for (index = 0; index < NE_MAX_NAME; ++index) registry->FailedModule[index] = moduleName[index] ? moduleName[index] : 0;

    target = NeRegistryFind(registry, moduleName);
    if (!target) return -1;                       /* module not loaded -- fail_mod names it */

    if (isByName)
    {
        if (NeImportedName(module, ordinalOrName, functionName, sizeof functionName) != 0) return -1;
        for (index = 0; index < NE_MAX_NAME; ++index) registry->FailedFunction[index] = functionName[index] ? functionName[index] : 0;
        if (NeExportByName(target, functionName, &ordinal) != 0) return -1;
    }
    registry->FailedOrdinal = ordinal;
    if (NeExportByOrdinal(target, ordinal, &segmentNumber, &segmentOffset) != 0) return -1;
    /* An ABSOLUTE export has no segment. Hand the constant back in BOTH halves so the
     * relocation gets it whichever field its addr_type patches: __A000H wants 0xA000
     * in the segment word, __AHINCR wants 8 in the offset word, and neither knows
     * which it is at this point.
     */
    if (!segmentNumber)
    {
        *selector = segmentOffset;
        *offset = segmentOffset;
        return 0;
    }
    if (segmentNumber > target->SegmentCount) return -1;
    /* A selector of 0 means step 2 above was skipped: the target module's segments
     * have no address yet, so this "resolved" import would be a call to 0000:xxxx.
     */
    if (!target->Segments[segmentNumber - 1].Selector) return -1;
    *selector = target->Segments[segmentNumber - 1].Selector;
    *offset = segmentOffset;
    return 0;
}

UINT32 NeSegmentAllocSize(PCNE_SEGMENT segment)
{
    UINT32 size = segment->Length;
    if (segment->MinAlloc && segment->MinAlloc > size) size = segment->MinAlloc;
    if (!size) size = 1;
    return size;
}
