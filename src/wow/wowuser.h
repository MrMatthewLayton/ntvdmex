#ifndef WOWUSER_H
#define WOWUSER_H
/*
 * wowuser.h -- USER.EXE's half of the WOW32 interface. GH #128, session 38.
 *
 * ── WHY THIS IS A SEPARATE FILE AND NOT MORE CASES IN wow32.h ────────────────
 * THE ID SPACE IS PER MODULE. Every id in wow32.h was read out of krnl386's own
 * stub table; USER has its own table of 457 stubs with its own numbering, and the
 * numbers COLLIDE. `0x39` is `GetProfileInt` in krnl386's table and
 * `RegisterClass` in USER's -- and for one run this host serviced the second with
 * the first and handed WOWEXEC the answer. Two id spaces in one switch is how that
 * happens; two files with two dispatchers, chosen by the stub's own segment, is how
 * it stops happening.
 *
 * The surface is enumerated in docs/research/wow-user-surface.md -- 441 ids, 262 of
 * them named by USER's own export table, regenerable with
 *     tools/ne/wowmap.py guest/ne/user.exe --md
 *
 * ── THE Win16 WNDCLASS, CONFIRMED FIELD BY FIELD AGAINST WOWEXEC's OWN CODE ──
 * Not taken from a header. WOWEXEC builds one on its stack and every store lands
 * where this layout says it should:
 *
 *     083b  lea ax,[bp-0x1a]        -> the struct is 0x1A = 26 bytes
 *     0819  lcall LoadCursor
 *     081e  mov [bp-0xc],ax          -> +0x0e  hCursor
 *     0823  lcall <stock object>
 *     0828  mov [bp-0xa],ax          -> +0x10  hbrBackground
 *     082b  mov word [bp-4],0x82     -> +0x16  lpszClassName offset
 *     0830  mov [bp-2],ds            -> +0x18  lpszClassName segment
 *     0833  sub ax,ax / mov [bp-6],ax / mov [bp-8],ax  -> +0x12/+0x14 lpszMenuName = NULL
 *
 *   +0x00 WORD  style          +0x0c WORD hIcon
 *   +0x02 DWORD lpfnWndProc    +0x0e WORD hCursor
 *   +0x06 WORD  cbClsExtra     +0x10 WORD hbrBackground
 *   +0x08 WORD  cbWndExtra     +0x12 DWORD lpszMenuName
 *   +0x0a WORD  hInstance      +0x16 DWORD lpszClassName
 *
 * ⚠ `mov word [bp-4],0x82` is the CLASS NAME POINTER, not a style. An earlier note
 *   read it as "style 0x82, hInstance=ds" off the same two instructions; the offsets
 *   above are what settle it, and they come from the struct size the program itself
 *   declares with that `lea`.
 */

#define WOWUSER_REGISTERCLASS   0x39
#define WOWUSER_CREATEWINDOW    0x29

/*
 * ── ★★★ CreateWindow's ARGUMENT BLOCK, READ OFF WOWEXEC'S OWN PUSHES ─────────
 * Not from a header, and not from the parameter order in the documentation: the
 * arg block grows the opposite way from the pushes, so "lpClassName is the first
 * parameter" says nothing about where it lands. The block base (bp+16) is the
 * LOWEST address, which holds the LAST word pushed -- so the parameter list is
 * reversed, and a DWORD's high word is at the LOWER offset because Pascal pushes
 * it first.
 *
 * WOWEXEC's call site is `wowexec seg1:0x08b2`, and the fifteen pushes ahead of
 * it are exactly 30 bytes -- the `push 0x1e` the USER stub at `seg1:0x038d`
 * declares. Import-table resolution names it outright: `USER.41 CREATEWINDOW`.
 *
 *   push ds      -> +28 |  push 0x8000 -> +14 (y)
 *   push 0xae    -> +26 |  push 0x8000 -> +12 (nWidth)
 *   push [0x16]  -> +24 |  push 0x8000 -> +10 (nHeight)
 *   push [0x14]  -> +22 |  push 0      -> +8  (hWndParent)
 *   push 0x2cf   -> +20 |  push 0      -> +6  (hMenu)
 *   push 0       -> +18 |  push [0x206]-> +4  (hInstance)
 *   push 0x8000  -> +16 |  push 0 / push 0 -> +2/+0 (lpParam)
 *
 * ★ AND THE DATA CROSS-VALIDATES THE LAYOUT, which is why it is trusted:
 *     +26/+28  ds:0x00ae is the string "WOWExecClass" -- the class WOWEXEC has
 *              just registered (a second copy of the literal; the WNDCLASS used
 *              ds:0x0082, and both decode to the same name).
 *     +18/+20  0x02CF0000 = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN. Read the other
 *              way round it would be 0x000002CF, which is not a window style.
 *     +10..+16 four copies of 0x8000 = CW_USEDEFAULT, exactly where x/y/w/h are.
 *     +4       [0x206], the SAME word WOWEXEC stored into WNDCLASS.hInstance at
 *              `seg1:0x0803`.
 *   Four independent agreements. A wrong offset assignment produces none of them.
 */
#define CW_ARG_LPPARAM      0
#define CW_ARG_HINSTANCE    4
#define CW_ARG_HMENU        6
#define CW_ARG_HWNDPARENT   8
#define CW_ARG_HEIGHT       10
#define CW_ARG_WIDTH        12
#define CW_ARG_Y            14
#define CW_ARG_X            16
#define CW_ARG_STYLE        18
#define CW_ARG_WINDOWNAME   22
#define CW_ARG_CLASSNAME    26

/* Win16's CW_USEDEFAULT, and what this host resolves it to. ⚠ THE SIZE IS
   NOMINAL: there is no desktop yet, so there is no honest answer -- it is a
   placeholder that must become the real metric the day windows have pixels, and
   it is a constant here so that day is a one-line change rather than a hunt. */
#define CW_USEDEFAULT16     0x8000
#define WOWUSER_DESK_CX     640
#define WOWUSER_DESK_CY     480

/* Win16 WNDCLASS field offsets -- see the note above. */
#define WNDCLASS_STYLE          0x00
#define WNDCLASS_WNDPROC        0x02
#define WNDCLASS_CLSEXTRA       0x06
#define WNDCLASS_WNDEXTRA       0x08
#define WNDCLASS_HINSTANCE      0x0a
#define WNDCLASS_HICON          0x0c
#define WNDCLASS_HCURSOR        0x0e
#define WNDCLASS_HBRBACKGROUND  0x10
#define WNDCLASS_MENUNAME       0x12
#define WNDCLASS_CLASSNAME      0x16
#define WNDCLASS_SIZE           0x1a

#define WOWUSER_MAX_CLASS 32

typedef struct {
    char  name[64];
    WORD  atom;
    WORD  style;
    DWORD wndproc;                   /* 16:16 far pointer into the guest */
    WORD  hinst;
    WORD  hicon, hcursor, hbrback;
    WORD  clsextra, wndextra;
} wowuser_class_t;

static wowuser_class_t g_wu_class[WOWUSER_MAX_CLASS];
static int             g_wu_nclass = 0;

/*
 * ── ★★ A WINDOW, AS AN OBJECT, WITH NO PIXELS BEHIND IT ─────────────────────
 * DELIBERATELY NOT A REAL HWND. A host window would drag in a real message
 * queue, a real WM_CREATE and the 16:16 thunk back into the class's wndproc --
 * none of which exists, and all of which would be half-built and lying by the
 * time the first CreateWindow returned. What the guest can actually observe at
 * this point is a handle that is non-zero, stable, and answers questions about
 * itself, so that is exactly what this is: the class it was made from, its
 * rectangle, its style, its text. WOWEXEC's own window is the WOW shell's and is
 * normally hidden, so for this guest there is nothing to draw in any case.
 * ⇒ When windows do get pixels, this struct is the thing that grows a host
 *   window handle; nothing above it has to move.
 *
 * ⚠ THE HANDLE SPACE IS SYNTHETIC AND SAYS SO. A real Win16 HWND is an offset
 *   into USER's local heap; ours is a counter. Nothing may infer memory from it.
 */
#define WOWUSER_MAX_WIN   32
#define WOWUSER_HWND_BASE 0x0100        /* first synthetic handle */
#define WOWUSER_HWND_STEP 0x0020        /* spaced so a stray +n is not a hit    */

typedef struct {
    WORD  hwnd;                      /* 0 = free slot */
    WORD  cls;                       /* index into g_wu_class */
    DWORD style;
    DWORD wndproc;                   /* copied from the class AT CREATION -- Win16
                                        keeps it per window, so a later
                                        RegisterClass cannot retarget this one */
    int   x, y, cx, cy;
    WORD  parent, menu, hinst;
    char  text[64];
} wowuser_win_t;

static wowuser_win_t g_wu_win[WOWUSER_MAX_WIN];
static int           g_wu_nwin = 0;

/* Read a NUL-terminated guest string through a 16:16 far pointer. */
static int wowuser_farstr(const wow32_frame_t *f, DWORD fp, char *out, int cap)
{
    WORD sel = (WORD)(fp >> 16);
    DWORD base;
    const volatile BYTE *s;
    int k = 0;
    if (cap) out[0] = 0;
    if (!sel || !f->sel2lin) return 0;
    base = f->sel2lin(sel, f->ctx);
    if (!base) return 0;
    s = (const volatile BYTE *)(ULONG_PTR)(base + (fp & 0xFFFF));
    while (k < cap - 1 && s[k]) { out[k] = (char)s[k]; ++k; }
    out[k] = 0;
    return 1;
}

static WORD wowuser_peek(const volatile BYTE *p, int off)
{
    return (WORD)(p[off] | (p[off + 1] << 8));
}

/* The class a name is registered under, or NULL. Win16 class names are
   case-insensitive, and a lookup that is not would silently register duplicates. */
static wowuser_class_t *wowuser_find(const char *name)
{
    int i, k;
    for (i = 0; i < g_wu_nclass; ++i) {
        const char *a = g_wu_class[i].name, *b = name;
        for (k = 0; ; ++k) {
            char ca = a[k], cb = b[k];
            if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
            if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
            if (ca != cb) break;
            if (!ca) return &g_wu_class[i];
        }
    }
    return NULL;
}

/* The class registered under an ATOM, or NULL. ⚠ Win16 lets lpClassName be an
   atom rather than a string -- a null selector with the atom in the offset -- and
   a host that only understood strings would fail every CreateWindow made from
   RegisterClass's own return value, which is the idiomatic way to write it. */
static wowuser_class_t *wowuser_find_atom(WORD atom)
{
    int i;
    if (!atom) return NULL;
    for (i = 0; i < g_wu_nclass; ++i)
        if (g_wu_class[i].atom == atom) return &g_wu_class[i];
    return NULL;
}

/* ---- note building ------------------------------------------------------
   The note is what the run log says a call DID, and it is the only channel this
   project has for reading a Win16 program's intent. It is built with a running
   cursor rather than a formatter because there is no CRT here (see runtime.c). */
static void wu_puts(char *b, int cap, int *k, const char *s)
{
    while (*s && *k < cap - 1) b[(*k)++] = *s++;
    b[*k] = 0;
}

static void wu_puthex(char *b, int cap, int *k, DWORD v, int digits)
{
    static const char hx[] = "0123456789abcdef";
    int i;
    for (i = digits - 1; i >= 0; --i) {
        if (*k >= cap - 1) break;
        b[(*k)++] = hx[(v >> (i * 4)) & 0xF];
    }
    b[*k] = 0;
}

/* A quoted string, with the quotes, truncated rather than dropped. */
static void wu_putq(char *b, int cap, int *k, const char *s)
{
    wu_puts(b, cap, k, "\"");
    wu_puts(b, cap, k, s);
    wu_puts(b, cap, k, "\"");
}

/*
 * Returns 1 if serviced (the caller advances EIP past the BOP), 0 if not.
 * `note` receives a short human-readable description of what happened, so the
 * caller's log line can say what was registered rather than just that something was.
 *
 * ⚠ CALLED ONLY WHEN THE STUB IS USER'S. The caller checks; this file must never
 *   be reachable from krnl386's id space or the whole point of splitting it is lost.
 */
static int wowuser_call(wow32_frame_t *f, char *note, int notecap)
{
    if (notecap) note[0] = 0;
    switch (f->id) {

    /* ── ★★★ 0x39 RegisterClass(const WNDCLASS FAR*) ──────────────────────────
         Named by USER's own export table: ordinal 57 `REGISTERCLASS` is a wrapper
         at seg1:0x1dbd that TAIL-JUMPS to the stub at seg1:0x0c18, and that stub
         pushes id 0x39 with 4 argument bytes -- one far pointer. The caller tests
         `or ax,ax`, so 0 is failure and any non-zero value is the class ATOM.
       ★ REGISTERING IS REAL WORK, NOT A STUB. The window procedure, the class
         styles and the extra-bytes counts are what CreateWindow and DefWindowProc
         will need, and they are only available here -- the guest hands them over
         once and then refers to the class by name. So keep them.
       ⚠ THE ATOM MUST BE STABLE AND NON-ZERO. Win16 atoms live at 0xC000 and up;
         a second RegisterClass of the same name returns the SAME atom rather than
         allocating another, because a program that re-registers (WOWEXEC does, once
         per relaunch) must not exhaust the table. */
    case WOWUSER_REGISTERCLASS: {
        volatile BYTE *wc = wow32_argptr(f, 0);
        char cname[64];
        wowuser_class_t *c;
        int n;
        if (!wc) { wow32_setret(f, 0); return 1; }      /* an unreadable WNDCLASS fails */
        wowuser_farstr(f, (DWORD)wowuser_peek(wc, WNDCLASS_CLASSNAME)
                          | ((DWORD)wowuser_peek(wc, WNDCLASS_CLASSNAME + 2) << 16),
                       cname, sizeof cname);
        if (!cname[0]) { wow32_setret(f, 0); return 1; } /* no name, no class */
        c = wowuser_find(cname);
        if (!c) {
            if (g_wu_nclass >= WOWUSER_MAX_CLASS) { wow32_setret(f, 0); return 1; }
            c = &g_wu_class[g_wu_nclass++];
            for (n = 0; n < (int)sizeof c->name; ++n) c->name[n] = cname[n];
            c->atom = (WORD)(0xC000 + g_wu_nclass);
        }
        c->style    = wowuser_peek(wc, WNDCLASS_STYLE);
        c->wndproc  = (DWORD)wowuser_peek(wc, WNDCLASS_WNDPROC)
                    | ((DWORD)wowuser_peek(wc, WNDCLASS_WNDPROC + 2) << 16);
        c->clsextra = wowuser_peek(wc, WNDCLASS_CLSEXTRA);
        c->wndextra = wowuser_peek(wc, WNDCLASS_WNDEXTRA);
        c->hinst    = wowuser_peek(wc, WNDCLASS_HINSTANCE);
        c->hicon    = wowuser_peek(wc, WNDCLASS_HICON);
        c->hcursor  = wowuser_peek(wc, WNDCLASS_HCURSOR);
        c->hbrback  = wowuser_peek(wc, WNDCLASS_HBRBACKGROUND);
        /* The note is the whole point of servicing this: it is the first time this
           project can say WHAT a Win16 program is trying to put on the screen. */
        {   int k = 0;
            wu_puts(note, notecap, &k, "RegisterClass ");
            wu_putq(note, notecap, &k, cname);
        }
        wow32_setret(f, c->atom);
        return 1;
    }

    /* ── ★★★ 0x29 CreateWindow(...) -- 30 argument bytes ──────────────────────
         `USER.41 CREATEWINDOW`, named by resolving WOWEXEC's own import chain at
         its call site rather than inferred: the stub at USER `seg1:0x038d`
         pushes id 0x29 and `0x1e` argument bytes, and Win16's eleven parameters
         come to exactly 30. The block layout is derived and cross-validated in
         the comment on CW_ARG_* above.
       ★ AN UNREGISTERED CLASS MUST FAIL. Real Windows returns NULL, and a host
         that made a window for any name at all would hide a broken RegisterClass
         behind a working CreateWindow -- the "runs but lies" class.
       ⚠ The caller tests `or ax,ax / je`, so 0 is the failure the guest expects
         and any non-zero value is taken as the window handle. */
    case WOWUSER_CREATEWINDOW: {
        DWORD clsfp = wow32_argd(f, CW_ARG_CLASSNAME);
        char  cname[64], wname[64];
        wowuser_class_t *c;
        wowuser_win_t *w;
        int i, k = 0;

        if ((WORD)(clsfp >> 16) == 0)                    /* an ATOM, not a string */
            c = wowuser_find_atom((WORD)clsfp);
        else {
            wowuser_farstr(f, clsfp, cname, sizeof cname);
            c = cname[0] ? wowuser_find(cname) : NULL;
        }
        if (!c) { wow32_setret(f, 0); wu_puts(note, notecap, &k,
                                              "CreateWindow: no such class"); return 1; }

        w = NULL;
        for (i = 0; i < g_wu_nwin; ++i) if (!g_wu_win[i].hwnd) { w = &g_wu_win[i]; break; }
        if (!w) {
            if (g_wu_nwin >= WOWUSER_MAX_WIN) { wow32_setret(f, 0); return 1; }
            w = &g_wu_win[g_wu_nwin++];
        }
        w->hwnd    = (WORD)(WOWUSER_HWND_BASE + (w - g_wu_win) * WOWUSER_HWND_STEP);
        w->cls     = (WORD)(c - g_wu_class);
        w->style   = wow32_argd(f, CW_ARG_STYLE);
        w->wndproc = c->wndproc;
        w->parent  = wow32_argw(f, CW_ARG_HWNDPARENT);
        w->menu    = wow32_argw(f, CW_ARG_HMENU);
        w->hinst   = wow32_argw(f, CW_ARG_HINSTANCE);
        {   WORD x  = wow32_argw(f, CW_ARG_X),  y  = wow32_argw(f, CW_ARG_Y);
            WORD cx = wow32_argw(f, CW_ARG_WIDTH), cy = wow32_argw(f, CW_ARG_HEIGHT);
            /* CW_USEDEFAULT is only meaningful before there is a rectangle; every
               later reader wants numbers, so resolve it once, here. */
            w->x  = (x  == CW_USEDEFAULT16) ? 0 : (int)(short)x;
            w->y  = (y  == CW_USEDEFAULT16) ? 0 : (int)(short)y;
            w->cx = (cx == CW_USEDEFAULT16) ? WOWUSER_DESK_CX : (int)(short)cx;
            w->cy = (cy == CW_USEDEFAULT16) ? WOWUSER_DESK_CY : (int)(short)cy;
        }
        w->text[0] = 0;
        if (wowuser_farstr(f, wow32_argd(f, CW_ARG_WINDOWNAME), wname, sizeof wname))
            for (i = 0; i < (int)sizeof w->text; ++i) w->text[i] = wname[i];

        wu_puts(note, notecap, &k, "CreateWindow ");
        wu_putq(note, notecap, &k, c->name);
        wu_puts(note, notecap, &k, " ");
        wu_putq(note, notecap, &k, w->text);
        wu_puts(note, notecap, &k, " style=0x");
        wu_puthex(note, notecap, &k, w->style, 8);
        wu_puts(note, notecap, &k, " -> hwnd=0x");
        wu_puthex(note, notecap, &k, w->hwnd, 4);
        wow32_setret(f, w->hwnd);
        return 1;
    }

    default:
        return 0;
    }
}

#endif /* WOWUSER_H */
