#ifndef WOWRES_H
#define WOWRES_H
/*
 * wowres.h -- ★ THE GUEST'S OWN RESOURCES, AS REAL Win32 OBJECTS. GH #128, s.43.
 *
 * ── WHY THE HOST READS THE FILE ITSELF ──────────────────────────────────────
 * A Win16 program's menu, icons and accelerators live in its NE module, and the
 * 32-bit side cannot ask the 16-bit loader for them -- krnl386 hands out handles
 * into ITS address space, not bytes we can pass to Win32. But the file is right
 * there on disk and this host already knows its path (it is what it launched), so
 * the resources can simply be read.
 *
 * ⚠ THE APPLICATION'S OWN FILE, and only that. A class registered by a DLL would
 *   name a resource in the DLL, and answering it out of the EXE would be a wrong
 *   menu rather than a missing one. Every lookup says which file it searched.
 *
 * ── BOTH LAYOUTS WERE CONFIRMED AGAINST THE DATA BEFORE THIS EXISTED ────────
 * `tools/ne/neres.py` decodes the same two structures offline, and its output for
 * NOTEPAD.EXE's MENU #1 is:
 *
 *     &File -> &New / &Open... / &Save / Save &As... / &Print / Page Se&tup... /
 *              P&rint Setup... / --- / E&xit
 *     &Edit -> &Undo Ctrl+Z / --- / Cu&t Ctrl+X / &Copy Ctrl+C / ...
 *     &Search -> &Find... / Find &Next F3
 *     &Help -> &Contents / ... / &About Notepad...
 *
 * A wrong offset does not accidentally spell "&About Notepad...". That is the
 * whole reason the decoder was written as a tool first: the reading checks itself.
 *
 * ── THE TWO FORMATS ─────────────────────────────────────────────────────────
 * RESOURCE TABLE (at the NE header + 0x24): a WORD alignment shift, then TYPEINFO
 * records -- {WORD type, WORD count, DWORD reserved} followed by `count`
 * NAMEINFOs of {WORD offset, WORD length, WORD flags, WORD id, WORD, WORD} --
 * terminated by a zero type. Offsets and lengths are in alignment units. An id or
 * type with the high bit set is an integer; otherwise it is an offset to a
 * length-prefixed name.
 *
 * MENU TEMPLATE: WORD version, WORD headerSize, then items. An item is a WORD of
 * flags, then (unless it is a popup) a WORD id, then an ASCIIZ label. `MF_POPUP`
 * opens a submenu; `MF_END` closes the current level.
 */

#define WOWRES_MAX_FILE   (2u * 1024u * 1024u)
#define WOWRES_RT_MENU    4
#define WOWRES_MF_POPUP   0x0010
#define WOWRES_MF_END     0x0080

static BYTE  *g_wr_img  = NULL;      /* the application's file, verbatim */
static DWORD  g_wr_len  = 0;
static char   g_wr_path[512];
static int    g_wr_tried = 0;

static WORD wr_w(DWORD off)
{
    if (off + 2 > g_wr_len) return 0;
    return (WORD)(g_wr_img[off] | (g_wr_img[off + 1] << 8));
}

/* Read the application's file once. Returns 1 if there is an image to search. */
static int wowres_open(const char *path)
{
    HANDLE h;
    DWORD sz = 0, rd = 0;
    int i;
    if (g_wr_tried) return g_wr_img != NULL;
    g_wr_tried = 1;
    if (!path || !path[0]) return 0;
    for (i = 0; i < (int)sizeof g_wr_path - 1 && path[i]; ++i) g_wr_path[i] = path[i];
    g_wr_path[i] = 0;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz > WOWRES_MAX_FILE) { CloseHandle(h); return 0; }
    g_wr_img = (BYTE *)HeapAlloc(GetProcessHeap(), 0, sz);
    if (!g_wr_img) { CloseHandle(h); return 0; }
    ReadFile(h, g_wr_img, sz, &rd, NULL);
    CloseHandle(h);
    g_wr_len = rd;
    if (rd < 0x40 || g_wr_img[0] != 'M' || g_wr_img[1] != 'Z') {
        g_wr_img = NULL; g_wr_len = 0; return 0;
    }
    return 1;
}

/* Locate a resource by integer type and integer id. 0 = not found. */
static DWORD wowres_find(WORD type, WORD id, DWORD *len)
{
    DWORD h, rt, p;
    WORD shift;
    if (!g_wr_img) return 0;
    h = (DWORD)(g_wr_img[0x3C] | (g_wr_img[0x3D] << 8)
              | (g_wr_img[0x3E] << 16) | ((DWORD)g_wr_img[0x3F] << 24));
    if (h + 0x40 > g_wr_len || g_wr_img[h] != 'N' || g_wr_img[h + 1] != 'E') return 0;
    rt = h + wr_w(h + 0x24);
    if (rt + 2 > g_wr_len) return 0;
    shift = wr_w(rt);
    if (shift > 16) return 0;
    p = rt + 2;
    while (p + 8 <= g_wr_len) {
        WORD tid = wr_w(p), cnt = wr_w(p + 2), k;
        if (!tid) break;
        p += 8;
        for (k = 0; k < cnt && p + 12 <= g_wr_len; ++k, p += 12) {
            /* Integer type and id both carry the high bit; a named one is an
               offset into the string pool and is not what this looks up. */
            if (tid == (WORD)(0x8000 | type) && wr_w(p + 6) == (WORD)(0x8000 | id)) {
                DWORD off = (DWORD)wr_w(p) << shift;
                DWORD ln  = (DWORD)wr_w(p + 2) << shift;
                if (off + ln > g_wr_len) return 0;
                if (len) *len = ln;
                return off;
            }
        }
    }
    return 0;
}

/* Build one level of a menu, returning the offset just past it. `into` may be
   NULL, which walks the template without building -- used to count items so an
   empty or unreadable menu is never attached to a window. */
static DWORD wowres_menu_level(HMENU into, DWORD p, DWORD end, int depth, int *n)
{
    while (p + 2 <= end) {
        WORD flags = wr_w(p);
        WORD id = 0;
        char text[128];
        int t = 0;
        p += 2;
        if (!(flags & WOWRES_MF_POPUP)) { id = wr_w(p); p += 2; }
        while (p < end && g_wr_img[p] && t < (int)sizeof text - 1)
            text[t++] = (char)g_wr_img[p++];
        text[t] = 0;
        while (p < end && g_wr_img[p]) ++p;          /* an over-long label */
        ++p;                                          /* the NUL */
        if (depth > 8) return p;                      /* a bounded tree, always */
        if (flags & WOWRES_MF_POPUP) {
            HMENU sub = into ? CreatePopupMenu() : NULL;
            p = wowres_menu_level(sub, p, end, depth + 1, n);
            if (into && sub) AppendMenuA(into, MF_POPUP | MF_STRING,
                                         (UINT_PTR)sub, text);
        } else if (!text[0] && !id) {
            if (into) AppendMenuA(into, MF_SEPARATOR, 0, NULL);
        } else {
            if (into) AppendMenuA(into, MF_STRING, id, text);
        }
        if (n) ++*n;
        if (flags & WOWRES_MF_END) return p;
    }
    return p;
}

/*
 * Build a real HMENU from the application's MENU resource `id`, or NULL.
 * ⚠ ONE MENU PER WINDOW. Win32 will not let two windows share an HMENU, so this
 *   builds a fresh one every time rather than caching -- a cached menu attached
 *   twice is a menu that vanishes from the first window.
 */
static HMENU wowres_menu(WORD id, int *items)
{
    DWORD len = 0, off = wowres_find(WOWRES_RT_MENU, id, &len);
    DWORD p;
    HMENU m;
    int n = 0;
    if (items) *items = 0;
    if (!off || len < 4) return NULL;
    p = off + 4 + wr_w(off + 2);                 /* version, then headerSize */
    m = CreateMenu();
    if (!m) return NULL;
    wowres_menu_level(m, p, off + len, 0, &n);
    if (items) *items = n;
    if (!n) { DestroyMenu(m); return NULL; }
    return m;
}

/*
 * ── ★★ THE APPLICATION'S OWN ICON ───────────────────────────────────────────
 * A GROUP_ICON resource is a directory: {WORD reserved, WORD type, WORD count}
 * then `count` 14-byte entries {BYTE w, h, colours, reserved; WORD planes, bits;
 * DWORD bytes; WORD id}, each naming an ICON resource by id.
 *
 * ★ CONFIRMED AGAINST NOTEPAD BEFORE THIS WAS WRITTEN. Its GROUP_ICON #1 decodes
 *   to type=1, count=2, and the two entries are 32x32 1bpp/304 bytes id=1 and
 *   32x32 4bpp/744 bytes id=2 -- and the resource table independently lists ICON 1
 *   at 304 bytes and ICON 2 at 752 (744 rounded up to the alignment unit). A wrong
 *   layout does not produce byte counts that match a different table.
 *
 * The ICON resource itself is a DIB -- header, palette, XOR bits, AND mask -- and
 * `CreateIconFromResourceEx` takes exactly that, so the OS does the decoding. The
 * `0x00030000` is the icon-resource version that API is documented to want; it is
 * a Win32 contract, not a claim about Win16.
 */
#define WOWRES_RT_ICON        3
#define WOWRES_RT_GROUP_ICON  14

static HICON wowres_icon(WORD groupid, int *picked)
{
    DWORD glen = 0, goff = wowres_find(WOWRES_RT_GROUP_ICON, groupid, &glen);
    DWORD ilen = 0, ioff;
    WORD cnt, i, bestid = 0;
    int bestbits = -1;
    if (picked) *picked = 0;
    if (!goff || glen < 6) return NULL;
    if (wr_w(goff + 2) != 1) return NULL;              /* type 1 = icons */
    cnt = wr_w(goff + 4);
    if (!cnt || 6u + 14u * cnt > glen) return NULL;
    /* Richest colour depth wins -- the OS scales, so the only thing worth
       choosing between these is how much colour information there is. */
    for (i = 0; i < cnt; ++i) {
        DWORD e = goff + 6 + (DWORD)i * 14;
        int bits = wr_w(e + 6);
        if (bits > bestbits) { bestbits = bits; bestid = wr_w(e + 12); }
    }
    if (!bestid) return NULL;
    ioff = wowres_find(WOWRES_RT_ICON, bestid, &ilen);
    if (!ioff || !ilen) return NULL;
    if (picked) *picked = bestbits;
    return CreateIconFromResourceEx(g_wr_img + ioff, ilen, TRUE, 0x00030000,
                                    0, 0, LR_DEFAULTCOLOR);
}

#endif /* WOWRES_H */
