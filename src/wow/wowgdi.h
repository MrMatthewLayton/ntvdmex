#ifndef WOWGDI_H
#define WOWGDI_H
/*
 * wowgdi.h -- ★★ GDI.EXE's OWN ID SPACE.  GH #128, session 44.
 *
 * A SEVENTH id space, and the first one opened for its own sake rather than
 * because a guest stopped on it: `tools/ne/neneeds.py` says NOTEPAD.EXE reaches
 * three of GDI's thunks directly, and MS PAINT reaches **41**. So this file is
 * where the north star's other half begins, and the three below are its first
 * three lines rather than the whole of it.
 *
 * ── THE IDS, FROM THE FILE ──────────────────────────────────────────────────
 * GDI's exports are tail-jumps, like USER's, so the stub is one hop past the
 * entry point (`neneeds.py` follows it; see the note there):
 *      68 DELETEDC       -> stub seg1:0x032d  id 0x44   2 args  retstub 0x033a
 *      69 DELETEOBJECT   -> stub seg1:0x0347  id 0x45   2 args  retstub 0x0354
 *      80 GETDEVICECAPS  -> stub seg1:0x05d1  id 0x50   4 args  retstub 0x05de
 * The ids are the export ordinals again -- checked here, as it is checked per
 * module, and never assumed: krnl386's are nothing like its ordinals.
 *
 * ── ★★ WHY THERE IS AN OBJECT MAP AND NOT A CAST ───────────────────────────
 * A Win32 `HDC`, `HBRUSH`, `HBITMAP` or `HFONT` is 32 bits and a Win16 program
 * has 16 to hold it in. Every one of them also travels back through 16-bit code
 * -- GDI's own `CreateDC` and `GetStockObject` are `native16` wrappers -- so the
 * value has to survive a round trip and still name the right object. Same answer
 * as the windows, the menus and the cursor/icon tokens, and for the same reason:
 * a truncated pointer would name the wrong object and would not fail loudly.
 *
 * ⚠⚠ AND THIS IS WHERE THE HONEST GAP IS TODAY. The calls that PRODUCE a DC or
 *   an object are not serviced yet (`CreateDC` and `GetStockObject` are native16
 *   wrappers whose internal stubs no run has named), so a handle arriving here
 *   will usually be one this host never issued. That is reported as exactly
 *   that -- "not one of our GDI tokens" -- and answered with GDI's own failure
 *   value, rather than guessed at. When the producers go in, these three already
 *   work; until then they say what is missing instead of pretending.
 */

#define WOWGDI_DELETEDC       0x0044
#define WOWGDI_DELETEOBJECT   0x0045
#define WOWGDI_GETDEVICECAPS  0x0050

#define GDC_ARG_INDEX   0
#define GDC_ARG_HDC     2
#define DOBJ_ARG_HANDLE 0

/* GDI tokens sit below the menu tokens (0x4000) and above the window handles,
   so a stray handle of any kind is recognisable on sight in a log. */
#define WOWGDI_BASE      0x2000
#define WOWGDI_STEP      0x0008
#define WOWGDI_MAX       256

typedef struct { WORD h; HGDIOBJ o; int isdc; } wowgdi_obj_t;
static wowgdi_obj_t g_wg_obj[WOWGDI_MAX];
static int          g_wg_nobj = 0;

/* One token per object. `isdc` is kept because DeleteDC and DeleteObject are
   different calls with different rules, and handing one to the other is a defect
   this map can catch instead of passing on to Win32. */
static WORD wowgdi_h16(HGDIOBJ o, int isdc)
{
    int i;
    if (!o) return 0;
    for (i = 0; i < g_wg_nobj; ++i)
        if (g_wg_obj[i].o == o) return g_wg_obj[i].h;
    if (g_wg_nobj >= WOWGDI_MAX) return 0;
    i = g_wg_nobj++;
    g_wg_obj[i].o = o;
    g_wg_obj[i].isdc = isdc;
    g_wg_obj[i].h = (WORD)(WOWGDI_BASE + i * WOWGDI_STEP);
    return g_wg_obj[i].h;
}

static HGDIOBJ wowgdi_h32(WORD h, int *isdc)
{
    int i;
    if (isdc) *isdc = 0;
    if (!h) return NULL;
    for (i = 0; i < g_wg_nobj; ++i)
        if (g_wg_obj[i].h == h) {
            if (isdc) *isdc = g_wg_obj[i].isdc;
            return g_wg_obj[i].o;
        }
    return NULL;
}

/* Forget a token whose object has been destroyed. ⚠ The slot is cleared rather
   than compacted: a token is its INDEX, so compacting would silently rename
   every object above it. */
static void wowgdi_forget(WORD h)
{
    int i;
    for (i = 0; i < g_wg_nobj; ++i)
        if (g_wg_obj[i].h == h) { g_wg_obj[i].o = NULL; g_wg_obj[i].h = 0; return; }
}

/*
 * ⚠ CALLED ONLY WHEN THE STUB IS GDI'S. The caller checks, as it does for every
 *   other table -- `0x45` is `SetWindowPos`-adjacent in USER's numbering and
 *   `DeleteObject` here.
 */
static int wowgdi_call(wow32_frame_t *f, char *note, int notecap)
{
    if (notecap) note[0] = 0;
    switch (f->id) {

    /* ── ★ 0x50 GetDeviceCaps(hDC, nIndex) ──────────────────────────────────
         Straight through to the OS on the same claim the WS_* bits and the SM_*
         indices travel on: Win32 inherited the DC capability indices from Win16
         unchanged. ⚠ As with GetSystemMetrics, a wrong answer here does not
         fail, it lays something out slightly wrong -- so the index AND the answer
         are logged on every call, and a guest whose arithmetic looks wrong can be
         checked against what it was actually told. */
    case WOWGDI_GETDEVICECAPS: {
        WORD hdc = wow32_argw(f, GDC_ARG_HDC);
        WORD idx = wow32_argw(f, GDC_ARG_INDEX);
        int  isdc = 0;
        HGDIOBJ o = wowgdi_h32(hdc, &isdc);
        int k = 0, v;
        wu_puts(note, notecap, &k, "GetDeviceCaps(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", 0x");
        wu_puthex(note, notecap, &k, idx, 4);
        wu_puts(note, notecap, &k, ")");
        if (!o || !isdc) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR GDI DC TOKENS (the"
                                       " calls that CREATE a DC are not serviced"
                                       " yet); answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        v = GetDeviceCaps((HDC)o, (int)idx);
        wu_puts(note, notecap, &k, " = 0x");
        wu_puthex(note, notecap, &k, (DWORD)v, 4);
        wow32_setret(f, (DWORD)(WORD)v);
        return 1;
    }

    /* ── ★ 0x44 DeleteDC / 0x45 DeleteObject ────────────────────────────────
       ⚠ THE TOKEN IS FORGOTTEN AS WELL AS THE OBJECT DELETED. A Win32 handle is
         reusable once freed, so a token left pointing at a dead HGDIOBJ would
         eventually name somebody else's object -- the same hazard DestroyWindow
         has, and the same answer.
       ⚠ AND THE TWO ARE NOT INTERCHANGEABLE: DeleteObject on a DC leaks it and
         DeleteDC on a brush fails, so the map records which it issued and this
         says so rather than passing the mistake to Win32. */
    case WOWGDI_DELETEDC:
    case WOWGDI_DELETEOBJECT: {
        int  wantdc = (f->id == WOWGDI_DELETEDC);
        WORD h = wow32_argw(f, DOBJ_ARG_HANDLE);
        int  isdc = 0;
        HGDIOBJ o = wowgdi_h32(h, &isdc);
        int k = 0, ok;
        wu_puts(note, notecap, &k, wantdc ? "DeleteDC 0x" : "DeleteObject 0x");
        wu_puthex(note, notecap, &k, h, 4);
        if (!o) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR GDI TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (wantdc != isdc) {
            wu_puts(note, notecap, &k, isdc ? " -- ★ THAT IS A DC AND THIS IS"
                                              " DeleteObject; refused"
                                            : " -- ★ THAT IS NOT A DC AND THIS IS"
                                              " DeleteDC; refused");
            wow32_setret(f, 0);
            return 1;
        }
        ok = wantdc ? (DeleteDC((HDC)o) ? 1 : 0) : (DeleteObject(o) ? 1 : 0);
        if (ok) wowgdi_forget(h);
        wu_puts(note, notecap, &k, ok ? " -> deleted, token released"
                                      : " -- ★ the OS refused the delete");
        wow32_setret(f, (DWORD)ok);
        return 1;
    }

    default:
        return 0;
    }
}

#endif /* WOWGDI_H */
