/* ne.h -- 16-bit New Executable loader (GH #128 / #4).
 *
 * The image format every 16-bit Windows program uses, and the first brick of the WOW
 * layer. Header-only and free of any Windows or VDM dependency, by the same convention
 * as src/dos/ -- so the whole thing is exercised off-VM by tools/dostest/ne_test.c in
 * milliseconds instead of a round trip to the rig.
 *
 * ── WRITTEN AGAINST MEASURED BINARIES, NOT THE SPEC. ────────────────────────────────
 * `tools/ne/nedump.py` was built first and pointed at the real files. What they
 * actually contain decided what is implemented here and in what order:
 *
 *   krnl386.exe   4 segs, 13 relocs, ALL of one kind (INTERNALREF/SEGMENT, chained,
 *                 targeting segment 1), and it IMPORTS FROM NOTHING -- it is the
 *                 kernel. This is what `ntvdm -a ...\krnl386.exe` asks WOW to boot.
 *   sysedit.exe   6 segs, 179 relocs over five kinds, importing from KERNEL, GDI,
 *                 USER and SHELL. 147 are IMPORTORDINAL/FAR_ADDR -- ordinary API
 *                 calls by ordinal, which is the gate to running real programs.
 *
 * So a loader that handles INTERNALREF is already enough to place krnl386 in memory,
 * and imports can arrive later without restructuring anything.
 *
 * ⚠ TWO THINGS THE FORMAT DOES THAT ARE EASY TO GET WRONG, AND BOTH ARE HERE:
 *   1. RELOCATION CHAINS. A record does not name one site. Unless the ADDITIVE bit is
 *      set, the word AT the site holds the offset of the NEXT site to patch, and the
 *      chain ends at 0xFFFF. Treating each record as a single fixup silently leaves
 *      most of a segment unrelocated.
 *   2. MOVEABLE TARGETS. An INTERNALREF with a==0xFF does not name a segment; it names
 *      an ENTRY TABLE ORDINAL, which must be looked up to get the real segment:offset.
 */
#ifndef NTVDMEX_NE_H
#define NTVDMEX_NE_H

#include <stdint.h>

#define NE_MAX_SEG 96
#define NE_MAX_MOD 16        /* modules held in one registry -- WOW needs about six  */
#define NE_MAX_NAME 32

/* ne_module.prog_flags */
#define NE_PROG_LIBRARY    0x8000    /* ...so CS:IP is a DLL *init* entry, not a start */

/* entry-table flags byte */
#define NE_ENT_EXPORTED    0x01
#define NE_ENT_SHAREDDATA  0x02

/* entry-table bundle segment indicators */
#define NE_ENT_ABSOLUTE    0xFE      /* the "offset" IS the value; there is no segment */
#define NE_ENT_MOVEABLE    0xFF

/* ne_seg.flags */
#define NE_SEG_DATA        0x0001
#define NE_SEG_MOVEABLE    0x0010
#define NE_SEG_PRELOAD     0x0040
#define NE_SEG_RELOCS      0x0100

/* relocation record: low 2 bits of rel_type */
#define NE_REL_INTERNAL    0
#define NE_REL_IMPORTORD   1
#define NE_REL_IMPORTNAME  2
#define NE_REL_OSFIXUP     3
#define NE_REL_ADDITIVE    0x04      /* ...so do NOT walk a chain */

/* relocation record: addr_type -- what shape of thing is at the site */
#define NE_ADDR_LOBYTE     0
#define NE_ADDR_SEGMENT    2         /* 16-bit segment/selector          */
#define NE_ADDR_FARADDR    3         /* 32-bit off:seg                   */
#define NE_ADDR_OFFSET16   5         /* 16-bit offset                    */
#define NE_ADDR_FARADDR48  11        /* 48-bit off32:seg (386)           */
#define NE_ADDR_OFFSET32   13

typedef struct {
    uint16_t sector, flags, minalloc;
    uint32_t file_off, length;
    uint16_t seg;          /* runtime segment/selector -- filled by the caller  */
    uint8_t *mem;          /* host pointer to this segment's loaded bytes       */
} ne_seg;

typedef struct {
    const uint8_t *img;
    uint32_t img_len, hdr;
    uint16_t prog_flags, autodata, heap, stack;
    uint32_t csip, sssp;
    uint16_t n_seg, n_mod, n_movable, align_shift, expect_ver;
    uint16_t entry_tab, entry_len, seg_tab, res_tab, resident_tab, mod_tab, imp_tab;
    /* ⚠ The NON-resident names table offset is an ABSOLUTE file offset and a DWORD,
         unlike every other table offset in this header, which is a WORD relative to
         the NE header. Reading it the same way as its neighbours lands in nothing. */
    uint32_t nonres_off;
    uint16_t nonres_len;
    uint8_t  target_os, other_flags;
    ne_seg   seg[NE_MAX_SEG];
    int      err;          /* 0 = ok; otherwise the __LINE__ that rejected it   */
    /* ⚠ How many SITES were actually patched, accumulated across segments. A
         relocation pass that returns "success" having done nothing is
         indistinguishable from one that worked, and the guest only finds out
         later and somewhere else. Count it, print it, and assert on it. */
    uint32_t sites;
} ne_module;

/* Resolve an imported entry point. Returns 0 on success and fills seg:off.
   `m` is the module DOING the importing -- the resolver needs it to turn `mod` into a
   module name and `ord_or_name` into a string, both of which live in that module's own
   tables. `mod` is a 1-based index into the module reference table. When `by_name` is
   0, `ord_or_name` is an ordinal; when 1, it is an offset into the imported-names
   table. See ne_registry_resolve for the implementation WOW uses. */
typedef int (*ne_import_fn)(void *ctx, const ne_module *m, uint16_t mod,
                            uint16_t ord_or_name, int by_name,
                            uint16_t *seg, uint16_t *off);

/* ── little-endian readers, bounds-checked ──────────────────────────────────────── */
static uint16_t ne_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t ne_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void ne_wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static int ne_ok(const ne_module *m, uint32_t off, uint32_t n)
{
    return off <= m->img_len && n <= m->img_len - off;
}

/* ── parse ──────────────────────────────────────────────────────────────────────────
     MZ at 0, a LONG at 0x3C giving the second header, and 'NE' there. Field offsets
     are spelled out because getting one wrong shifts every table after it and the
     failure looks like corrupt data rather than a bad constant -- which is exactly
     what happened while writing nedump.py. */
static int ne_parse(ne_module *m, const uint8_t *img, uint32_t len)
{
    uint32_t h;
    uint16_t i;
    int k;
    for (k = 0; k < (int)sizeof *m; ++k) ((uint8_t *)m)[k] = 0;
    m->img = img; m->img_len = len;

    if (len < 0x40 || img[0] != 'M' || img[1] != 'Z') { m->err = __LINE__; return -1; }
    h = ne_rd32(img + 0x3C);
    if (!ne_ok(m, h, 0x40)) { m->err = __LINE__; return -1; }
    if (img[h] != 'N' || img[h + 1] != 'E') { m->err = __LINE__; return -1; }
    m->hdr = h;

    m->entry_tab    = ne_rd16(img + h + 0x04);
    m->entry_len    = ne_rd16(img + h + 0x06);
    m->prog_flags   = ne_rd16(img + h + 0x0C);
    m->autodata     = ne_rd16(img + h + 0x0E);
    m->heap         = ne_rd16(img + h + 0x10);
    m->stack        = ne_rd16(img + h + 0x12);
    m->csip         = ne_rd32(img + h + 0x14);
    m->sssp         = ne_rd32(img + h + 0x18);
    m->n_seg        = ne_rd16(img + h + 0x1C);
    m->n_mod        = ne_rd16(img + h + 0x1E);
    m->nonres_len   = ne_rd16(img + h + 0x20);
    m->nonres_off   = ne_rd32(img + h + 0x2C);      /* ABSOLUTE, and a DWORD */
    m->seg_tab      = ne_rd16(img + h + 0x22);
    m->res_tab      = ne_rd16(img + h + 0x24);
    m->resident_tab = ne_rd16(img + h + 0x26);
    m->mod_tab      = ne_rd16(img + h + 0x28);
    m->imp_tab      = ne_rd16(img + h + 0x2A);
    m->n_movable    = ne_rd16(img + h + 0x30);
    m->align_shift  = ne_rd16(img + h + 0x32);
    m->target_os    = img[h + 0x36];
    m->other_flags  = img[h + 0x37];
    m->expect_ver   = ne_rd16(img + h + 0x3E);

    if (!m->align_shift) m->align_shift = 9;          /* 0 means 512, not 1 */
    if (m->n_seg > NE_MAX_SEG) { m->err = __LINE__; return -1; }
    if (!ne_ok(m, h + m->seg_tab, (uint32_t)m->n_seg * 8)) { m->err = __LINE__; return -1; }

    for (i = 0; i < m->n_seg; ++i) {
        const uint8_t *e = img + h + m->seg_tab + i * 8;
        ne_seg *s = &m->seg[i];
        s->sector   = ne_rd16(e);
        s->length   = ne_rd16(e + 2);
        s->flags    = ne_rd16(e + 4);
        s->minalloc = ne_rd16(e + 6);
        /* A zero length means 64K -- but only if the segment has file data at all. */
        if (!s->length && s->sector) s->length = 0x10000;
        s->file_off = (uint32_t)s->sector << m->align_shift;
        if (s->sector && !ne_ok(m, s->file_off, s->length)) { m->err = __LINE__; return -1; }
    }
    return 0;
}

/* ── entry table ────────────────────────────────────────────────────────────────────
     Bundles of entries, ordinals numbered sequentially from 1 across all bundles.
     Each bundle: BYTE count, BYTE segment indicator.
       indicator 0     -> end of table
       indicator 0xFF  -> MOVEABLE: 6 bytes each (flags, INT 3Fh thunk, segno, offset)
       indicator 0xFE  -> ABSOLUTE: 3 bytes (flags, VALUE) -- see below
       otherwise       -> FIXED in that segment: 3 bytes each (flags, offset)
     Needed because an INTERNALREF with a==0xFF targets an ORDINAL, not a segment.

   ★ 0xFE IS NOT A SEGMENT NUMBER, AND READING IT AS ONE IS A REAL FAILURE, NOT A
     THEORETICAL ONE. krnl386 has 30 such entries and they are the classic Win16
     absolute exports -- __AHSHIFT=3, __AHINCR=8, __A000H=0xA000, __0040H=0x0040,
     __WINFLAGS, and the __MOD_* module handles. Treated as "segment 254" they are
     rejected against a 4-segment module, which is exactly how gdi.exe's relocation
     pass first failed here: 366 of its records import __MOD_GDI.
     `*seg_no` comes back 0 -- segments are numbered from 1, so 0 is free to mean
     "no segment: *off is the whole answer". */
static int ne_entry_lookup_ex(const ne_module *m, uint16_t ordinal,
                              uint16_t *seg_no, uint16_t *off, uint8_t *ent_flags)
{
    uint32_t o = m->hdr + m->entry_tab;
    uint32_t end = o + m->entry_len;
    uint16_t cur = 1;
    if (!ordinal || !ne_ok(m, o, m->entry_len)) return -1;
    while (o + 2 <= end) {
        uint8_t cnt = m->img[o], ind = m->img[o + 1];
        uint32_t rec = o + 2, step;
        if (!cnt) break;                       /* count 0 terminates */
        step = (ind == NE_ENT_MOVEABLE) ? 6u : (ind == 0 ? 0u : 3u);
        if (!step) { o = rec; cur = (uint16_t)(cur + cnt); continue; }  /* null bundle */
        if (ordinal >= cur && ordinal < cur + cnt) {
            uint32_t e = rec + (uint32_t)(ordinal - cur) * step;
            if (!ne_ok(m, e, step)) return -1;
            if (ent_flags) *ent_flags = m->img[e];
            if (ind == NE_ENT_MOVEABLE)      { *seg_no = m->img[e + 3];
                                               *off = ne_rd16(m->img + e + 4); }
            else if (ind == NE_ENT_ABSOLUTE) { *seg_no = 0;
                                               *off = ne_rd16(m->img + e + 1); }
            else                             { *seg_no = ind;
                                               *off = ne_rd16(m->img + e + 1); }
            return 0;
        }
        cur = (uint16_t)(cur + cnt);
        o = rec + (uint32_t)cnt * step;
    }
    return -1;
}

static int ne_entry_lookup(const ne_module *m, uint16_t ordinal,
                           uint16_t *seg_no, uint16_t *off)
{
    return ne_entry_lookup_ex(m, ordinal, seg_no, off, 0);
}

/* ── names ──────────────────────────────────────────────────────────────────────────
     Four tables, all built from length-prefixed (Pascal) strings that are NOT
     terminated, so every read here is by length and every result is terminated by us:

       RESIDENT names   (hdr-relative)  entry 0 is the MODULE'S OWN NAME; the rest are
                                        exports that stay in memory.
       NON-RESIDENT     (ABSOLUTE)      entry 0 is the module DESCRIPTION, not an
                                        export; the rest are exports Windows is free to
                                        discard. Both entry-0s carry ordinal 0.
       MODULE REF       (hdr-relative)  n_mod WORDs, each an offset into...
       IMPORTED names   (hdr-relative)  ...which holds the names of modules imported
                                        FROM, and of functions imported BY NAME.

   ★ MEASURED, AND IT DECIDES THE SEARCH ORDER: user.exe imports GETWOWCOMPATFLAGSEX
     from KERNEL by NAME, and that export is in krnl386's NON-RESIDENT table (@521),
     not its resident one. A by-name lookup that stops at the resident table finds 0 of
     krnl386's 312 non-resident exports and fails on the very first real binary. */
static int ne_name_at(const ne_module *m, uint32_t off, char *out, int cap)
{
    uint32_t n;
    uint32_t i;
    if (cap <= 0) return -1;
    out[0] = 0;
    if (!ne_ok(m, off, 1)) return -1;
    n = m->img[off];
    if (!ne_ok(m, off + 1, n) || (int)n >= cap) return -1;
    for (i = 0; i < n; ++i) out[i] = (char)m->img[off + 1 + i];
    out[n] = 0;
    return 0;
}

static char ne_upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static int ne_ieq(const char *a, const char *b)
{
    while (*a && *b) { if (ne_upper(*a) != ne_upper(*b)) return 0; ++a; ++b; }
    return *a == *b;
}

/* The module's own name, from resident-names entry 0. This is the name OTHER modules
   import it by -- and it is NOT the file name: krnl386.exe calls itself KERNEL. */
static int ne_own_name(const ne_module *m, char *out, int cap)
{
    return ne_name_at(m, m->hdr + m->resident_tab, out, cap);
}

/* Name of the `ref`th (1-based) module this one imports from. */
static int ne_ref_name(const ne_module *m, uint16_t ref, char *out, int cap)
{
    uint32_t e;
    if (cap > 0) out[0] = 0;
    if (!ref || ref > m->n_mod) return -1;
    e = m->hdr + m->mod_tab + (uint32_t)(ref - 1) * 2;
    if (!ne_ok(m, e, 2)) return -1;
    return ne_name_at(m, m->hdr + m->imp_tab + ne_rd16(m->img + e), out, cap);
}

/* A function name from the imported-names table, given the offset a relocation
   record carries in `b` for an IMPORTNAME fixup. */
static int ne_imported_name(const ne_module *m, uint16_t name_off, char *out, int cap)
{
    return ne_name_at(m, m->hdr + m->imp_tab + name_off, out, cap);
}

/* Walk one name table. `off`/`len` bound it; `skip_first` drops the module-name or
   description entry. Returns 0 and sets *ordinal when `want` matches. */
static int ne_names_find(const ne_module *m, uint32_t off, uint32_t len,
                         const char *want, uint16_t *ordinal)
{
    uint32_t o = off, end = off + len;
    int first = 1;
    if (!len || !ne_ok(m, off, len)) return -1;
    while (o + 3 <= end) {
        char nm[NE_MAX_NAME];
        uint32_t n = m->img[o];
        if (!n) break;                                  /* a zero length terminates */
        if (!ne_ok(m, o + 1, n + 2u)) return -1;
        if (ne_name_at(m, o, nm, sizeof nm) == 0 && !first && ne_ieq(nm, want)) {
            *ordinal = ne_rd16(m->img + o + 1 + n);
            return 0;
        }
        first = 0;
        o += 1 + n + 2;
    }
    return -1;
}

static int ne_export_by_name(const ne_module *m, const char *name, uint16_t *ordinal)
{
    /* The resident table has no length field -- it runs to its own zero terminator, so
       bound it by the rest of the image and let the walk stop itself. */
    uint32_t r = m->hdr + m->resident_tab;
    if (r < m->img_len && ne_names_find(m, r, m->img_len - r, name, ordinal) == 0)
        return 0;
    return ne_names_find(m, m->nonres_off, m->nonres_len, name, ordinal);
}

/* Resolve an EXPORT of this module to segment number + offset. *seg_no == 0 on
   return means the export is an ABSOLUTE constant and *off is its value.
   ⚠ Refuses an ordinal whose entry lacks the EXPORTED bit. Measured: all 190 distinct
     ordinals the four real importers ask for have it, and 14 of sysedit's 21 entries
     do NOT -- so the bit is meaningful and checking it turns "imported a private
     entry" from a far call into rubbish into a load-time error. */
static int ne_export_by_ordinal(const ne_module *m, uint16_t ordinal,
                                uint16_t *seg_no, uint16_t *off)
{
    uint8_t fl = 0;
    if (ne_entry_lookup_ex(m, ordinal, seg_no, off, &fl) != 0) return -1;
    if (!(fl & NE_ENT_EXPORTED)) return -1;
    return 0;
}

/* ── relocations ────────────────────────────────────────────────────────────────────
     Applied to segment `idx` (0-based) after its bytes are in seg[idx].mem and every
     segment's runtime `seg` value is known. The record table lives immediately AFTER
     the segment's file data: a WORD count, then 8 bytes each. */
static int ne_apply_relocs(ne_module *m, int idx, ne_import_fn imp, void *ctx)
{
    ne_seg *s = &m->seg[idx];
    uint32_t o, n, i;
    if (!(s->flags & NE_SEG_RELOCS) || !s->sector) return 0;
    o = s->file_off + s->length;
    if (!ne_ok(m, o, 2)) { m->err = __LINE__; return -1; }
    n = ne_rd16(m->img + o); o += 2;
    if (!ne_ok(m, o, n * 8)) { m->err = __LINE__; return -1; }

    for (i = 0; i < n; ++i) {
        const uint8_t *r = m->img + o + i * 8;
        uint8_t  at = r[0], rt = r[1];
        uint16_t site = ne_rd16(r + 2), a = ne_rd16(r + 4), b = ne_rd16(r + 6);
        uint16_t tseg = 0, toff = 0;

        switch (rt & 3) {
        case NE_REL_INTERNAL:
            if ((a & 0xFF) == 0xFF) {           /* target names an entry ordinal */
                uint16_t sn;
                if (ne_entry_lookup(m, b, &sn, &toff) != 0) { m->err = __LINE__; return -1; }
                if (!sn || sn > m->n_seg) { m->err = __LINE__; return -1; }
                tseg = m->seg[sn - 1].seg;
            } else {
                if (!a || a > m->n_seg) { m->err = __LINE__; return -1; }
                tseg = m->seg[a - 1].seg;
                toff = b;
            }
            break;
        case NE_REL_IMPORTORD:
        case NE_REL_IMPORTNAME:
            /* Not resolvable without the exporting module. LOUD, not silent: a
               relocation left unapplied is a far call into nothing, and the guest
               dies far away from here with no clue why. */
            if (!imp || imp(ctx, m, a, b, (rt & 3) == NE_REL_IMPORTNAME,
                            &tseg, &toff) != 0) { m->err = __LINE__; return -1; }
            break;
        default:
            m->err = __LINE__; return -1;       /* OSFIXUP: none seen; refuse it */
        }

        /* Walk the chain (or patch once, if ADDITIVE).

         ★ ADDITIVE MEANS *ADD*, NOT REPLACE, AND THE BINARIES SETTLE IT. Every
           ADDITIVE record in the corpus is an OFFSET16 import of a __MOD_* absolute,
           whose value is 0 -- so the fixup itself is indistinguishable either way.
           What is NOT indistinguishable is the word already at each site: gdi.exe's
           366 sites hold 0x7b, 0x7c, 0x7d, 0x7e, 0x97, 0xaf... all different, and the
           bytes around one read
                 68 7e 00        push 0x007e        <- the site
                 9a ff ff 00 00  call far <KERNEL>
                 6a 06           push 6
           i.e. a WOW thunk table whose pushed word is the API index. Replacing would
           make every one `push 0` and send all 810 of gdi's and user's calls to
           function zero -- a guest that starts and then behaves like nothing on
           earth. Adding leaves them alone, which is what a zero addend should do.
         ⚠ Only OFFSET16-additive occurs in the corpus. A segment/selector is not a
           number you can add to, so the segment halves stay REPLACE; if a binary ever
           turns up with an additive SEGMENT record, this is the decision to revisit. */
        for (;;) {
            uint16_t next;
            int add = (rt & NE_REL_ADDITIVE) != 0;
            if (site + 2u > s->length) break;    /* a chain may run off the end */
            next = ne_rd16(s->mem + site);
            switch (at) {
            case NE_ADDR_SEGMENT:  ne_wr16(s->mem + site, tseg); break;
            case NE_ADDR_OFFSET16:
                ne_wr16(s->mem + site, (uint16_t)(add ? next + toff : toff));
                break;
            case NE_ADDR_FARADDR:
                if (site + 4u > s->length) break;
                ne_wr16(s->mem + site, (uint16_t)(add ? next + toff : toff));
                ne_wr16(s->mem + site + 2, tseg);
                break;
            case NE_ADDR_LOBYTE:
                s->mem[site] = (uint8_t)(add ? s->mem[site] + toff : toff);
                break;
            default:               m->err = __LINE__; return -1;
            }
            ++m->sites;
            if (add) break;                      /* additive records are not chained */
            if (next == 0xFFFF) break;
            site = next;
        }
    }
    return 0;
}

/* ── THE REGISTRY: resolving imports BETWEEN loaded modules. ────────────────────────
     A relocation record names a module by a per-module reference index and an export
     by ordinal (or by name). Turning that into an address needs the OTHER module, so
     something has to hold them all. That is this.
     Keyed on each module's OWN name from its resident table -- KERNEL, USER, GDI --
     because that is the name importers use, and it is not the file name (krnl386.exe
     is KERNEL).

   ⚠ ORDERING, AND IT IS NOT OPTIONAL. A resolved import is written as
     target-selector : offset, so every module's runtime `seg` values must be FINAL
     before ANY module is relocated. The sequence is:
        1. ne_parse + copy segment bytes, for every module
        2. assign every segment its selector
        3. ne_apply_relocs, for every module
     Relocating in step 1 and hoping to redo it later works only because relocation
     records are read from the untouched file image, not from the patched segment --
     but a chained record walks links THROUGH the segment, and the first pass
     overwrites those links with addresses. So a second pass over an already-patched
     segment follows garbage. Load once, select, then relocate once.

   ⚠ MOVEABLE EXPORTS ARE RESOLVED DIRECT, NOT THROUGH THEIR THUNK. Every moveable
     entry in the real binaries begins `CD 3F` (INT 3Fh) -- Windows hands importers
     the address of that 3-byte thunk so the kernel can fault a discarded segment in
     on first call, then rewrite the thunk as a direct jump. We do not move or discard
     segments, so there is nothing to fault in and the indirection would buy only a
     per-call interrupt. Resolving straight to segment:offset is therefore correct
     HERE and would stop being correct the moment segment discarding is implemented.
     Written down because the day that changes, this is the line that breaks. */
typedef struct {
    ne_module *mod[NE_MAX_MOD];
    char       name[NE_MAX_MOD][NE_MAX_NAME];
    int        n;
    /* Diagnostics for the failure that actually happens: which import gave up. */
    char       fail_mod[NE_MAX_NAME], fail_fn[NE_MAX_NAME];
    uint16_t   fail_ord;
} ne_registry;

static int ne_registry_add(ne_registry *r, ne_module *m)
{
    if (r->n >= NE_MAX_MOD) return -1;
    if (ne_own_name(m, r->name[r->n], NE_MAX_NAME) != 0) return -1;
    r->mod[r->n] = m;
    ++r->n;
    return 0;
}

static ne_module *ne_registry_find(const ne_registry *r, const char *name)
{
    int i;
    for (i = 0; i < r->n; ++i) if (ne_ieq(r->name[i], name)) return r->mod[i];
    return 0;
}

static int ne_registry_resolve(void *ctx, const ne_module *m, uint16_t mod,
                               uint16_t ord_or_name, int by_name,
                               uint16_t *seg, uint16_t *off)
{
    ne_registry *r = (ne_registry *)ctx;
    char mname[NE_MAX_NAME], fname[NE_MAX_NAME];
    ne_module *t;
    uint16_t ord = ord_or_name, sn = 0, so = 0;
    int i;

    fname[0] = 0;
    r->fail_mod[0] = 0; r->fail_fn[0] = 0; r->fail_ord = 0;
    if (ne_ref_name(m, mod, mname, sizeof mname) != 0) return -1;
    for (i = 0; i < NE_MAX_NAME; ++i) r->fail_mod[i] = mname[i] ? mname[i] : 0;

    t = ne_registry_find(r, mname);
    if (!t) return -1;                       /* module not loaded -- fail_mod names it */

    if (by_name) {
        if (ne_imported_name(m, ord_or_name, fname, sizeof fname) != 0) return -1;
        for (i = 0; i < NE_MAX_NAME; ++i) r->fail_fn[i] = fname[i] ? fname[i] : 0;
        if (ne_export_by_name(t, fname, &ord) != 0) return -1;
    }
    r->fail_ord = ord;
    if (ne_export_by_ordinal(t, ord, &sn, &so) != 0) return -1;
    /* An ABSOLUTE export has no segment. Hand the constant back in BOTH halves so the
       relocation gets it whichever field its addr_type patches: __A000H wants 0xA000
       in the segment word, __AHINCR wants 8 in the offset word, and neither knows
       which it is at this point. */
    if (!sn) { *seg = so; *off = so; return 0; }
    if (sn > t->n_seg) return -1;
    /* A selector of 0 means step 2 above was skipped: the target module's segments
       have no address yet, so this "resolved" import would be a call to 0000:xxxx. */
    if (!t->seg[sn - 1].seg) return -1;
    *seg = t->seg[sn - 1].seg;
    *off = so;
    return 0;
}

/* Convenience: how many bytes a segment needs in memory (minalloc can exceed the
   file length -- BSS-style tail that must be present and zeroed). */
static uint32_t ne_seg_alloc_size(const ne_seg *s)
{
    uint32_t want = s->length;
    if (s->minalloc && s->minalloc > want) want = s->minalloc;
    if (!want) want = 1;
    return want;
}

#endif /* NTVDMEX_NE_H */
