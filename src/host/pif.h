/* pif.h -- read a Program Information File: which program, which arguments, which
 * directory. Header-only and free of Win32 so the off-VM battery can hold it to a
 * real PIF's bytes (tests/unit/pif_test.c).
 *
 * WHY. Explorer launches a .PIF by queuing the PIF ITSELF as the VDM's program
 * (measured on the rig, s85: `command fetch ... app=[C:\QB45\QB.PIF] args=[]`). Stock
 * NTVDM reads it and runs the program it names. We handed the PIF's bytes to
 * COMMAND.COM as a program, which is the "sidescrolling cursor" the user saw, and
 * QuickBASIC's `/L` (needed for CALL ABSOLUTE) never arrived.
 *
 * THE FORMAT (Windows 3.x PIF, 0x171-byte basic section, then extension sections):
 *   0x02  title[30]           0x24  program[63]        0x65  start directory[64]
 *   0xA5  parameters[64]
 *   0x171 the first extension header, "MICROSOFT PIFEX"; each header is
 *         name[16], next-header offset (word, 0xFFFF = last), data offset, data length.
 *   "WINDOWS 386 3.0" data +0x28: parameters[64] -- what Windows 3.x and NT use when
 *   present, so it wins over the basic section's copy.
 * Strings are space- or NUL-padded; both are trimmed.
 */
#ifndef NTVDMEX_PIF_H
#define NTVDMEX_PIF_H
#include "../ntvdmex_types.h"
#define PIF_SECTION_WINDOWS_386 "WINDOWS 386 3.0"   /* the 386-enhanced extension block */

#define PIF_BASIC_LEN   0x171
#define PIF_PROG_OFF    0x24
#define PIF_PROG_LEN    63
#define PIF_DIR_OFF     0x65
#define PIF_DIR_LEN     64
#define PIF_PARAMS_OFF  0xA5
#define PIF_PARAMS_LEN  64
#define PIF_W386_PARAMS 0x28
/* An extension header: name[16], then three words. */
#define PIF_EXT_NAME_LENGTH   16
#define PIF_EXT_NEXT          16    /* the next header's offset; PIF_EXT_LAST = none */
#define PIF_EXT_DATA_OFFSET   18
#define PIF_EXT_DATA_LENGTH   20
#define PIF_EXT_HEADER_SIZE   22
#define PIF_EXT_LAST          0xFFFF
#define PIF_EXT_MAX_SECTIONS  16    /* a malformed chain cannot loop past this */

typedef struct _PIF_INFO {
    char Program[PIF_PROG_LEN + 1];       /* char, not CHAR: the spelling moves code (#333) */
    char Directory[PIF_DIR_LEN + 1];
    char Parameters[PIF_PARAMS_LEN + 1];
    INT  IsParametersFrom386;          /* 1 = the WINDOWS 386 3.0 section supplied them */
} PIF_INFO, *PPIF_INFO;

/* `unsigned`, not UINT, in this file: the spelling moved code in main.c (#333). */

/* Copy a fixed field, stopping at NUL, then trim trailing (and leading) blanks. */
static VOID PifCopyField(PSTR destination, PCBYTE source, unsigned length)
{
    unsigned end = 0, start = 0;
    while (end < length && source[end]) ++end;
    while (start < end && (source[start] == ' ' || source[start] == '\t')) ++start;
    while (end > start && (source[end - 1] == ' ' || source[end - 1] == '\t')) --end;
    { unsigned index; for (index = 0; index < end - start; ++index) destination[index] = (CHAR)source[start + index]; destination[end - start] = 0; }
}

static INT PifIsSectionName(PCBYTE header, PCSTR name)
{
    unsigned index;
    for (index = 0; name[index]; ++index) if (header[index] != (BYTE)name[index]) return 0;
    return header[index] == 0;
}

/* 1 if `bytes` looks like a PIF and `out` was filled; 0 otherwise. An MZ image, or
   anything shorter than the basic section, is not a PIF. */
static INT PifParse(PCBYTE bytes, unsigned long length, PPIF_INFO out)
{
    unsigned long offset;
    INT guard;
    out->Program[0] = out->Directory[0] = out->Parameters[0] = 0;
    out->IsParametersFrom386 = 0;
    if (length < PIF_BASIC_LEN || (bytes[0] == 'M' && bytes[1] == 'Z')) return 0;
    PifCopyField(out->Program,    bytes + PIF_PROG_OFF,   PIF_PROG_LEN);
    PifCopyField(out->Directory,  bytes + PIF_DIR_OFF,    PIF_DIR_LEN);
    PifCopyField(out->Parameters, bytes + PIF_PARAMS_OFF, PIF_PARAMS_LEN);
    if (!out->Program[0]) return 0;
    /* Extension sections, if the file has them. Bounded both by the file and by a
       count, so a malformed chain cannot loop. */
    offset = PIF_BASIC_LEN;
    for (guard = 0; guard < PIF_EXT_MAX_SECTIONS && offset + PIF_EXT_HEADER_SIZE <= length; ++guard) {
        PCBYTE header = bytes + offset;
        unsigned next = (unsigned)(header[PIF_EXT_NEXT] | (header[PIF_EXT_NEXT + 1] << BYTE_SHIFT));
        unsigned dataOffset = (unsigned)(header[PIF_EXT_DATA_OFFSET] | (header[PIF_EXT_DATA_OFFSET + 1] << BYTE_SHIFT));
        unsigned dataLength = (unsigned)(header[PIF_EXT_DATA_LENGTH] | (header[PIF_EXT_DATA_LENGTH + 1] << BYTE_SHIFT));
        if (PifIsSectionName(header, PIF_SECTION_WINDOWS_386) && dataLength >= PIF_W386_PARAMS + PIF_PARAMS_LEN
            && (unsigned long)dataOffset + dataLength <= length) {
            CHAR parameters386[PIF_PARAMS_LEN + 1];
            PifCopyField(parameters386, bytes + dataOffset + PIF_W386_PARAMS, PIF_PARAMS_LEN);
            if (parameters386[0]) {
                unsigned index;
                for (index = 0; parameters386[index]; ++index) out->Parameters[index] = parameters386[index];
                out->Parameters[index] = 0;
                out->IsParametersFrom386 = 1;
            }
        }
        if (next == PIF_EXT_LAST || next <= offset || next >= length) break;
        offset = next;
    }
    return 1;
}

#endif
