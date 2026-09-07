#ifndef WOWENUM_H
#define WOWENUM_H
/*
 * wowenum.h -- ★★★ CALLING A GUEST'S CALLBACK ONCE PER ITEM. GH #128, session 57.
 *
 * ── THE SHAPE, AND WHY IT IS A THIRD USE OF ONE MECHANISM ───────────────────
 * `EnumWindows`, `EnumChildWindows`, `EnumTaskWindows` and `LineDDA` are one
 * function with four sources of items: the host has a list, and for each item it
 * must call SIXTEEN-BIT CODE and look at what comes back. That is the same
 * problem the EDIT chain solved in session 44 and the modal dialog loop solved
 * earlier today -- a service cannot call the guest, because entering guest code
 * means replacing the whole context, which only the BOP handler can do and undo.
 * So the service ASKS, the handler calls, and when the call RETURNS the frame's
 * `action` says what to do next. Here, "next" is the next item.
 *
 *     EnumWindows BOP     park the caller; answer TRUE in advance
 *       -> proc(item 0)   wowcall_enter
 *       <- returns TRUE   ACT_ENUMNEXT: more items? yes
 *       -> proc(item 1)
 *       <- returns FALSE  ★ THE CALLBACK SAID STOP. The caller's TRUE is revised
 *                           to FALSE and the enumeration ends -- which is the
 *                           whole contract of these functions.
 *
 * ⚠ ONE AT A TIME, AND NESTING IS REFUSED RATHER THAN TRUNCATED. A callback that
 *   starts a second enumeration would overwrite the first one's cursor, and the
 *   first would then walk the second's list -- a wrong answer that looks like a
 *   right one. A refusal is a FALSE the caller is entitled to read as "could not
 *   enumerate", and it is written to the log.
 * ⚠ THE LIST IS SNAPSHOT BY INDEX, NOT COPIED. The callback may create or destroy
 *   windows (that is legal, and TASKMAN's End Task does it), so the cursor is an
 *   index into the live table and each step re-validates the slot. A window that
 *   vanished mid-enumeration is skipped, not reported as a stale handle.
 *
 * ── WHAT IS NOT HERE, AND WHY ───────────────────────────────────────────────
 * `EnumFonts`, `EnumFontFamilies` and `EnumObjects` are the same shape and are
 * NOT implemented, because their callbacks receive POINTERS TO STRUCTURES --
 * LOGFONT, TEXTMETRIC, LOGPEN, LOGBRUSH -- whose Win16 layouts differ from
 * Win32's in exactly the way `ABC` and `RECT` do. Building those from memory is
 * the one thing this project does not do; they need a measurement (a guest's own
 * reads, or the 16-bit headers) first. The mechanism below is ready for them.
 */

/* ⚠ THE CONSTANTS AND THE THREE ENTRY POINTS THE SERVICES CALL LIVE IN
     wowcall.h, NOT HERE, and the reason is the include order: wowgdi.h and
     wowuser.h are compiled BEFORE this file and both arm an enumeration. Putting
     them in the header that already owns the callback mechanism keeps one
     declaration rather than a forward declaration in each dispatcher. */

typedef struct {
    int   kind;
    DWORD proc;        /* the guest's callback                                 */
    WORD  ds;          /* the DS it must be entered with                       */
    DWORD lparam;      /* the caller's opaque value, passed to every call      */
    DWORD retlin;      /* the caller's return hole -- revised only on a STOP   */
    WORD  parent;      /* WOWENUM_CHILDREN                                     */
    int   idx;         /* cursor: the next window slot, or the next point      */
    int   x0, y0, x1, y1, steps;     /* WOWENUM_LINE                           */
    DWORD calls;       /* how many callbacks were made, for the log            */
} wowenum_t;

static wowenum_t g_we;

static int wowenum_busy(void) { return g_we.kind != WOWENUM_NONE; }

/* Start one. Returns 0 if another is already running (see the nesting note). */
static int wowenum_begin(int kind, DWORD proc, WORD ds, DWORD lparam,
                         DWORD retlin, WORD parent)
{
    if (g_we.kind != WOWENUM_NONE) return 0;
    if (!proc || !(proc >> 16)) return 0;
    g_we.kind    = kind;
    g_we.proc    = proc;
    g_we.ds      = ds;
    g_we.lparam  = lparam;
    g_we.retlin  = retlin;
    g_we.parent  = parent;
    g_we.idx     = 0;
    g_we.calls   = 0;
    return 1;
}

/* Bresenham's own step count: LineDDA visits max(|dx|,|dy|) + 1 points, which is
   what makes it a DDA rather than a plot -- the caller draws each one itself. */
static void wowenum_line(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0, dy = y1 - y0;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    g_we.x0 = x0; g_we.y0 = y0; g_we.x1 = x1; g_we.y1 = y1;
    g_we.steps = (dx > dy ? dx : dy) + 1;
    /* ⚠ AND IT IS BOUNDED. Every point is a 16-bit CALL, so a line across a large
         desktop is thousands of context switches -- correct, and slow enough to
         look like a hang. A diagonal of this display is ~2,000 points; 4,096 is
         generous and the log says when it clamps, so a guest that legitimately
         wants more is a line to grep for rather than a mystery stall. */
    if (g_we.steps > 4096) g_we.steps = 4096;
}

static void wowenum_end(void) { g_we.kind = WOWENUM_NONE; g_we.proc = 0; }

/* ── ★ THE CALLER'S ANSWER, WHEN THE CALLBACK STOPPED IT. ────────────────────
     EnumWindows and friends return TRUE when the whole list was walked and FALSE
     when the callback broke out. The service writes TRUE up front -- so a
     refused or empty enumeration still answers something -- and this is the only
     thing that revises it. Same four-byte write into guest memory as the modal
     loop's unwind, and for the same reason: the hole outlives the context
     switches in between. */
static void wowenum_stopped(void)
{
    volatile BYTE *h;
    if (!g_we.retlin) return;
    h = (volatile BYTE *)(ULONG_PTR)g_we.retlin;
    h[0] = 0; h[1] = 0; h[2] = 0; h[3] = 0;
}

/*
 * One step. Returns 1 if a 16-bit call is now in flight, 0 if the enumeration is
 * over (and the guest should be resumed where it stands).
 *
 * `stop` is the callback's answer to the PREVIOUS item -- 0 means "stop" -- and
 * is ignored on the first step, where there is no previous item.
 */
static int wowenum_step(volatile BYTE *tib, DWORD ssbase, WORD rsel,
                        int first, DWORD result, char *note, int cap)
{
    int k = 0;
    /* ⚠ FOUR, NOT THREE. A window callback takes (hwnd, lParam) = 3 words, but
         LineDDA's takes (x, y, lpData) = FOUR -- and the lParam words are written
         at [nargw] and [nargw+1] below, which for the line case is [2] and [3].
         Three would have written the last word off the end of this array. */
    WORD arg[4];
    int  nargw = 0;
    WORD hwnd16 = 0;

    if (g_we.kind == WOWENUM_NONE) return 0;

    /* ★ THE CALLBACK'S VETO. Win16 says a callback returning 0 ends the
         enumeration, and the function then answers FALSE. */
    if (!first && !(result & 0xFFFF)) {
        wu_puts(note, cap, &k, "ENUM stopped by the callback after 0x");
        wu_puthex(note, cap, &k, g_we.calls, 4);
        wu_puts(note, cap, &k, " call(s) -- the caller returns FALSE");
        wowenum_stopped();
        wowenum_end();
        return 0;
    }

    if (g_we.kind == WOWENUM_LINE) {
        /* LineDDA(x1, y1, x2, y2, proc, data): proc(x, y, lpData) for each point
           on the line, in order. The points are computed the way a DDA does --
           one step along the major axis -- so the caller gets the same sequence
           it would plot itself. */
        int i = g_we.idx;
        int dx = g_we.x1 - g_we.x0, dy = g_we.y1 - g_we.y0;
        int x, y;
        if (i >= g_we.steps) {
            wu_puts(note, cap, &k, "ENUM LineDDA complete: 0x");
            wu_puthex(note, cap, &k, g_we.calls, 4);
            wu_puts(note, cap, &k, " point(s)");
            wowenum_end();
            return 0;
        }
        x = g_we.x0 + (g_we.steps > 1 ? MulDiv(dx, i, g_we.steps - 1) : 0);
        y = g_we.y0 + (g_we.steps > 1 ? MulDiv(dy, i, g_we.steps - 1) : 0);
        g_we.idx = i + 1;
        arg[0] = (WORD)(short)x;
        arg[1] = (WORD)(short)y;
        nargw  = 2;                       /* lpData is appended below */
    } else {
        /* A window source. Walk the table from the cursor, skipping slots that
           are free, are not real windows, or do not match the filter. */
        for (;;) {
            wowuser_win_t *w;
            if (g_we.idx >= WOWUSER_MAX_WIN) {
                wu_puts(note, cap, &k, "ENUM complete: 0x");
                wu_puthex(note, cap, &k, g_we.calls, 4);
                wu_puts(note, cap, &k, " window(s) -- the caller returns TRUE");
                wowenum_end();
                return 0;
            }
            w = &g_wu_win[g_we.idx++];
            if (!w->hwnd || !w->hwnd32) continue;
            if (g_we.kind == WOWENUM_CHILDREN) {
                if (w->parent != g_we.parent) continue;
            } else {
                /* ⚠ TOP-LEVEL ONLY, and "top level" here means "no parent we
                     issued": a control belongs to its dialog, not to the desktop.
                     EnumTaskWindows walks the same list because this host runs
                     ONE Win16 task -- so every window we have IS that task's.
                     Said here rather than left as a coincidence. */
                if (w->parent) continue;
            }
            hwnd16 = w->hwnd;
            break;
        }
        arg[0] = hwnd16;
        nargw  = 1;
    }

    /* lParam / lpData: one DWORD, high word first. */
    arg[nargw]     = (WORD)(g_we.lparam >> 16);
    arg[nargw + 1] = (WORD)(g_we.lparam & 0xFFFF);
    nargw += 2;

    if (!rsel || !ssbase
        || !wowcall_enter(tib, ssbase, rsel, g_we.proc, g_we.ds, arg, nargw,
                          /* retlin */ 0, WOWCALL_RET_KEEP, NULL,
                          hwnd16, 0, NULL, 0, -1,
                          wowdlg_sel_absent((WORD)(g_we.proc >> 16)))) {
        wu_puts(note, cap, &k, "ENUM -- ★ THE CALL WAS REFUSED (depth, or no"
                               " return selector); the enumeration ends here and"
                               " the caller keeps the TRUE it was given");
        wowenum_end();
        return 0;
    }
    if (g_wc_depth > 0) {
        g_wc[g_wc_depth - 1].action = WOWCALL_ACT_ENUMNEXT;
        g_wc[g_wc_depth - 1].actarg = hwnd16;
    }
    ++g_we.calls;
    wu_puts(note, cap, &k, "ENUM -> 0x");
    wu_puthex(note, cap, &k, g_we.proc >> 16, 4);
    wu_puts(note, cap, &k, ":0x");
    wu_puthex(note, cap, &k, g_we.proc & 0xFFFF, 4);
    wu_puts(note, cap, &k, "(");
    {   int i;
        for (i = 0; i < nargw; ++i) {
            if (i) wu_puts(note, cap, &k, " ");
            wu_puthex(note, cap, &k, arg[i], 4);
        }
    }
    wu_puts(note, cap, &k, ")");
    return 1;
}

#endif /* WOWENUM_H */
