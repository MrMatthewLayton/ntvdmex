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

#define PIF_BASIC_LEN   0x171
#define PIF_PROG_OFF    0x24
#define PIF_PROG_LEN    63
#define PIF_DIR_OFF     0x65
#define PIF_DIR_LEN     64
#define PIF_PARAMS_OFF  0xA5
#define PIF_PARAMS_LEN  64
#define PIF_W386_PARAMS 0x28

typedef struct {
    char prog[PIF_PROG_LEN + 1];
    char dir[PIF_DIR_LEN + 1];
    char params[PIF_PARAMS_LEN + 1];
    int  params_from_386;          /* 1 = the WINDOWS 386 3.0 section supplied them */
} pif_info;

/* Copy a fixed field, stopping at NUL, then trim trailing (and leading) blanks. */
static void pif_field(char *dst, const unsigned char *src, unsigned len)
{
    unsigned n = 0, s = 0;
    while (n < len && src[n]) ++n;
    while (s < n && (src[s] == ' ' || src[s] == '\t')) ++s;
    while (n > s && (src[n - 1] == ' ' || src[n - 1] == '\t')) --n;
    { unsigned i; for (i = 0; i < n - s; ++i) dst[i] = (char)src[s + i]; dst[n - s] = 0; }
}

static int pif_name_is(const unsigned char *b, const char *name)
{
    unsigned i;
    for (i = 0; name[i]; ++i) if (b[i] != (unsigned char)name[i]) return 0;
    return b[i] == 0;
}

/* 1 if `b` looks like a PIF and `out` was filled; 0 otherwise. An MZ image, or
   anything shorter than the basic section, is not a PIF. */
static int pif_parse(const unsigned char *b, unsigned long n, pif_info *out)
{
    unsigned long off;
    int guard;
    out->prog[0] = out->dir[0] = out->params[0] = 0;
    out->params_from_386 = 0;
    if (n < PIF_BASIC_LEN || (b[0] == 'M' && b[1] == 'Z')) return 0;
    pif_field(out->prog,   b + PIF_PROG_OFF,   PIF_PROG_LEN);
    pif_field(out->dir,    b + PIF_DIR_OFF,    PIF_DIR_LEN);
    pif_field(out->params, b + PIF_PARAMS_OFF, PIF_PARAMS_LEN);
    if (!out->prog[0]) return 0;
    /* Extension sections, if the file has them. Bounded both by the file and by a
       count, so a malformed chain cannot loop. */
    off = PIF_BASIC_LEN;
    for (guard = 0; guard < 16 && off + 22 <= n; ++guard) {
        const unsigned char *h = b + off;
        unsigned next = (unsigned)(h[16] | (h[17] << 8));
        unsigned doff = (unsigned)(h[18] | (h[19] << 8));
        unsigned dlen = (unsigned)(h[20] | (h[21] << 8));
        if (pif_name_is(h, "WINDOWS 386 3.0") && dlen >= PIF_W386_PARAMS + PIF_PARAMS_LEN
            && (unsigned long)doff + dlen <= n) {
            char p386[PIF_PARAMS_LEN + 1];
            pif_field(p386, b + doff + PIF_W386_PARAMS, PIF_PARAMS_LEN);
            if (p386[0]) {
                unsigned i;
                for (i = 0; p386[i]; ++i) out->params[i] = p386[i];
                out->params[i] = 0;
                out->params_from_386 = 1;
            }
        }
        if (next == 0xFFFF || next <= off || next >= n) break;
        off = next;
    }
    return 1;
}

#endif
