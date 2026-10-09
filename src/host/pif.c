/* pif.c -- a .PIF's program, directory and parameters.
 *
 * The function definitions of pif.h, which keeps their declarations and doc comments (#335). */
#include "pif.h"

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

INT PifParse(PCBYTE bytes, unsigned long length, PPIF_INFO out)
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
