#ifndef NTVDMEX_WOWENUM_H
#define NTVDMEX_WOWENUM_H
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
 * `Action` says what to do next. Here, "next" is the next item.
 *
 *     EnumWindows BOP     park the caller; answer TRUE in advance
 *       -> proc(item 0)   WowCallEnter
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
 * ── s89/s90: FONTS AND OBJECTS ARE NOW HERE ─────────────────────────────────
 * EnumFontFamilies (s89), EnumFonts and EnumObjects (s90, #296) walk Win16
 * structures built up front in wowgdi.h. The note below is the original reason
 * they waited, kept because the rule it states still holds:
 * `EnumFonts`, `EnumFontFamilies` and `EnumObjects` are the same shape and were
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

typedef struct _WOWENUM {
    INT   Kind;
    DWORD Procedure;                         /* the guest's callback                                 */
    WORD  DataSelector;                      /* the DS it must be entered with                       */
    DWORD LParam;                            /* the caller's opaque value, passed to every call      */
    DWORD ReturnLinear;                      /* the caller's return hole -- revised only on a STOP   */
    WORD  Parent;                            /* WOWENUM_CHILDREN                                     */
    INT   Index;                             /* cursor: the next window slot, or the next point      */
    INT   StartX, StartY, EndX, EndY, Steps; /* WOWENUM_LINE                           */
    DWORD Calls;                             /* how many callbacks were made, for the log            */
} WOWENUM, *PWOWENUM;

/* The callbacks' argument blocks, in words, and the structures they point at. */
#define WOWENUM_MAX_ARGUMENTS       8
#define WOWENUM_LPARAM_WORDS        2
#define WOWENUM_LINE_ARGUMENTS      2     /* x, y -- lpData follows       */
#define WOWENUM_PAIR_ARGUMENTS      4     /* EnumObjects, EnumProps       */
#define WOWENUM_FONT_ARGUMENTS      7
#define WOWENUM_FONT_ARG_METRICS    2     /* lpntm: the second far pointer */
#define WOWENUM_METAFILE_ARGUMENTS  8
#define WOWENUM_METAFILE_ARG_RECORD 3     /* lpmr: the blob itself         */
#define WOWENUM_ELF_FACE_NAME       18    /* LOGFONT16.lfFaceName          */
#define WOWENUM_ELF_FACE_NAME_END   50
#define WOWENUM_LOGPEN16_SIZE       10
#define WOWENUM_LOGBRUSH16_SIZE     8
#define WOWENUM_PROP_NAME_MAX       31
#define WOWENUM_LINE_MAX_STEPS      4096
#define WOWENUM_INSTANCE_STACK_TOP  0x0A  /* INSTANCEDATA.pStackTop        */
#define WOWENUM_STACK_RESERVE       512
#define WOWENUM_METAFILE_MALFORMED  0xFFFF  /* wowgdi_mf_next's two refusals */
#define WOWENUM_METAFILE_NO_ROOM    0xFFFE
#define WOWENUM_HEX_RECORD_DIGITS   6


static WOWENUM g_WowEnum;

static INT WowEnumBusy(VOID) { return g_WowEnum.Kind != WOWENUM_NONE; }

/* Start one. Returns 0 if another is already running (see the nesting note). */
static INT WowEnumBegin(INT kind, DWORD procedure, WORD dataSelector, DWORD lParam,
                         DWORD returnLinear, WORD parent)
{
    if (g_WowEnum.Kind != WOWENUM_NONE) return 0;
    if (!procedure || !(procedure >> WOW_WORD_SHIFT)) return 0;
    g_WowEnum.Kind    = kind;
    g_WowEnum.Procedure    = procedure;
    g_WowEnum.DataSelector      = dataSelector;
    g_WowEnum.LParam  = lParam;
    g_WowEnum.ReturnLinear  = returnLinear;
    g_WowEnum.Parent  = parent;
    g_WowEnum.Index     = 0;
    g_WowEnum.Calls   = 0;
    return 1;
}

/* Bresenham's own step count: LineDDA visits max(|dx|,|dy|) + 1 points, which is
   what makes it a DDA rather than a plot -- the caller draws each one itself. */
static VOID WowEnumLine(INT startX, INT startY, INT endX, INT endY)
{
    INT deltaX = endX - startX, deltaY = endY - startY;
    if (deltaX < 0) deltaX = -deltaX;
    if (deltaY < 0) deltaY = -deltaY;
    g_WowEnum.StartX = startX; g_WowEnum.StartY = startY; g_WowEnum.EndX = endX; g_WowEnum.EndY = endY;
    /* s91: THE END POINT IS EXCLUDED, as GDI's LineDDA excludes it (w_ldda vs stock:
         (0,0)-(10,4) is 10 calls ending at (9,4); a zero-length line is none). */
    g_WowEnum.Steps = (deltaX > deltaY ? deltaX : deltaY);
    /* ⚠ AND IT IS BOUNDED. Every point is a 16-bit CALL, so a line across a large
         desktop is thousands of context switches -- correct, and slow enough to
         look like a hang. A diagonal of this display is ~2,000 points; 4,096 is
         generous and the log says when it clamps, so a guest that legitimately
         wants more is a line to grep for rather than a mystery stall. */
    if (g_WowEnum.Steps > WOWENUM_LINE_MAX_STEPS) g_WowEnum.Steps = WOWENUM_LINE_MAX_STEPS;
}

/* #295: an EnumMetaFile walk owns objects (the handle table) and a snapshot; both
   go here, so a stop, a refusal and a completion all release them. */
static VOID WowEnumEnd(VOID)
{
    if (g_WowEnum.Kind == WOWENUM_METAFILE) wowgdi_mf_end();
    g_WowEnum.Kind = WOWENUM_NONE; g_WowEnum.Procedure = 0;
}

/* ── ★ THE CALLER'S ANSWER, WHEN THE CALLBACK STOPPED IT. ────────────────────
     EnumWindows and friends return TRUE when the whole list was walked and FALSE
     when the callback broke out. The service writes TRUE up front -- so a
     refused or empty enumeration still answers something -- and this is the only
     thing that revises it. Same four-byte write into guest memory as the modal
     loop's unwind, and for the same reason: the hole outlives the context
     switches in between. */
static VOID WowEnumStopped(VOID)
{
    volatile BYTE *hole;
    if (!g_WowEnum.ReturnLinear) return;
    hole = (volatile BYTE *)(ULONG_PTR)g_WowEnum.ReturnLinear;
    hole[0] = 0; hole[1] = 0; hole[2] = 0; hole[3] = 0;
}

/*
 * One step. Returns 1 if a 16-bit call is now in flight, 0 if the enumeration is
 * over (and the guest should be resumed where it stands).
 *
 * `stop` is the callback's answer to the PREVIOUS item -- 0 means "stop" -- and
 * is ignored on the first step, where there is no previous item.
 */
static INT WowEnumStep(volatile BYTE *tib, DWORD stackBase, WORD returnSelector,
                        INT isFirst, DWORD result, PSTR note, INT noteCapacity)
{
    INT noteLength = 0;
    /* ⚠ FOUR, NOT THREE. A window callback takes (hwnd, lParam) = 3 words, but
         LineDDA's takes (x, y, lpData) = FOUR -- and the lParam words are written
         at [argumentCount] and [argumentCount+1] below, which for the line case is [2] and [3].
         Three would have written the last word off the end of this array. */
    WORD arguments[WOWENUM_MAX_ARGUMENTS];        /* s89: 7 for EnumFontFamilies; the window and line forms use 3-4 */
    INT  argumentCount = 0;
    WORD window16 = 0;

    if (g_WowEnum.Kind == WOWENUM_NONE) return 0;

    /* #295: the previous record's callback may have changed its handle table
         (through PlayMetaFileRecord, or by hand); take it back FIRST, before a stop
         below releases the table's objects -- a pen created by the record that
         said stop is still the enumeration's to delete. */
    if (!isFirst && g_WowEnum.Kind == WOWENUM_METAFILE && wowgdi_mf_readback()) {
        wu_puts(note, noteCapacity, &noteLength, "ENUM metafile: ★ the guest wrote a non-object value into"
                               " its handle table; those entries were NOT believed. ");
    }

    /* ★ THE CALLBACK'S VETO. Win16 says a callback returning 0 ends the
         enumeration, and the function then answers FALSE. */
    /* s91: NOT FOR LineDDA -- its callback is VOID, so AX is whatever the procedure
         left there; reading it as "stop" cut a line short at the first point whose y
         happened to be 0 (tests/probes/win16/w_ldda: 1 call where stock makes 10). */
    if (!isFirst && g_WowEnum.Kind != WOWENUM_LINE && !(result & WOW_WORD_MASK)) {
        wu_puts(note, noteCapacity, &noteLength, "ENUM stopped by the callback after 0x");
        wu_puthex(note, noteCapacity, &noteLength, g_WowEnum.Calls, WOW_HEX_WORD_DIGITS);
        wu_puts(note, noteCapacity, &noteLength, " call(s) -- the caller returns FALSE");
        WowEnumStopped();
        WowEnumEnd();
        return 0;
    }

    if (g_WowEnum.Kind == WOWENUM_FONTS) {
        /* EnumFontFamProc(LPENUMLOGFONT, LPNEWTEXTMETRIC, int FontType, LPARAM) */
        PCWOWENUM_FONT entry;
        if (g_WowEnum.Index >= g_WowEnumFontCount) {
            wu_puts(note, noteCapacity, &noteLength, "ENUM fonts complete: 0x");
            wu_puthex(note, noteCapacity, &noteLength, g_WowEnum.Calls, WOW_HEX_WORD_DIGITS);
            wu_puts(note, noteCapacity, &noteLength, " font(s)");
            WowEnumEnd();
            return 0;
        }
        entry = &g_WowEnumFonts[g_WowEnum.Index++];
        arguments[0] = 0; arguments[1] = 0;             /* lpelf: filled by WowCallEnter */
        arguments[2] = 0; arguments[3] = 0;             /* lpntm: ditto, +146 into the blob */
        arguments[4] = entry->FontType;
        arguments[5] = (WORD)(g_WowEnum.LParam >> WOW_WORD_SHIFT);
        arguments[6] = (WORD)(g_WowEnum.LParam & WOW_WORD_MASK);
        g_WowCallBlob2Argument = WOWENUM_FONT_ARG_METRICS; g_WowCallBlob2Offset = WOWENUM_ELF16;
        if (!returnSelector || !stackBase
            || !WowCallEnter(tib, stackBase, returnSelector, g_WowEnum.Procedure, g_WowEnum.DataSelector, arguments, WOWENUM_FONT_ARGUMENTS,
                              0, WOWCALL_RET_KEEP, NULL, 0, 0,
                              entry->Blob, (INT)sizeof entry->Blob, 0,
                              WowDlgIsSelectorAbsent((WORD)(g_WowEnum.Procedure >> WOW_WORD_SHIFT)))) {
            g_WowCallBlob2Argument = -1;
            wu_puts(note, noteCapacity, &noteLength, "ENUM fonts -- ★ THE CALL WAS REFUSED; the"
                                   " enumeration ends here");
            WowEnumEnd();
            return 0;
        }
        if (g_WowCallDepth > 0) {
            g_WowCallFrames[g_WowCallDepth - 1].Action = WOWCALL_ACT_ENUMNEXT;
            g_WowCallFrames[g_WowCallDepth - 1].ActionArgument = 0;
        }
        ++g_WowEnum.Calls;
        wu_puts(note, noteCapacity, &noteLength, "ENUM font -> \"");
        {   INT index; for (index = WOWENUM_ELF_FACE_NAME; index < WOWENUM_ELF_FACE_NAME_END && entry->Blob[index]; ++index) {
                CHAR character[2]; character[0] = (CHAR)entry->Blob[index]; character[1] = 0; wu_puts(note, noteCapacity, &noteLength, character); } }
        wu_puts(note, noteCapacity, &noteLength, "\" type=0x");
        wu_puthex(note, noteCapacity, &noteLength, entry->FontType, WOW_HEX_BYTE_DIGITS);
        return 1;
    }

    if (g_WowEnum.Kind == WOWENUM_OBJECTS) {
        /* EnumObjectsProc(LPVOID lpLogObject, LPARAM) -- s90, #296 */
        PCWOWENUM_FONT entry;
        if (g_WowEnum.Index >= g_WowEnumFontCount) {
            wu_puts(note, noteCapacity, &noteLength, "ENUM objects complete: 0x");
            wu_puthex(note, noteCapacity, &noteLength, g_WowEnum.Calls, WOW_HEX_WORD_DIGITS);
            wu_puts(note, noteCapacity, &noteLength, " object(s)");
            WowEnumEnd();
            return 0;
        }
        entry = &g_WowEnumFonts[g_WowEnum.Index++];
        arguments[0] = 0; arguments[1] = 0;             /* lpLogObject: filled by WowCallEnter */
        arguments[2] = (WORD)(g_WowEnum.LParam >> WOW_WORD_SHIFT);
        arguments[3] = (WORD)(g_WowEnum.LParam & WOW_WORD_MASK);
        if (!returnSelector || !stackBase
            || !WowCallEnter(tib, stackBase, returnSelector, g_WowEnum.Procedure, g_WowEnum.DataSelector, arguments, WOWENUM_PAIR_ARGUMENTS,
                              0, WOWCALL_RET_KEEP, NULL, 0, 0,
                              entry->Blob, entry->FontType == OBJ_PEN ? WOWENUM_LOGPEN16_SIZE : WOWENUM_LOGBRUSH16_SIZE, 0,
                              WowDlgIsSelectorAbsent((WORD)(g_WowEnum.Procedure >> WOW_WORD_SHIFT)))) {
            wu_puts(note, noteCapacity, &noteLength, "ENUM objects -- ★ THE CALL WAS REFUSED; the"
                                   " enumeration ends here");
            WowEnumEnd();
            return 0;
        }
        if (g_WowCallDepth > 0) {
            g_WowCallFrames[g_WowCallDepth - 1].Action = WOWCALL_ACT_ENUMNEXT;
            g_WowCallFrames[g_WowCallDepth - 1].ActionArgument = 0;
        }
        ++g_WowEnum.Calls;
        wu_puts(note, noteCapacity, &noteLength, "ENUM object -> style=0x");
        wu_puthex(note, noteCapacity, &noteLength, (DWORD)(entry->Blob[0] | (entry->Blob[1] << WOW_BYTE_SHIFT)), WOW_HEX_WORD_DIGITS);
        return 1;
    }

    if (g_WowEnum.Kind == WOWENUM_METAFILE) {
        /* EnumMetaFileProc(HDC, HANDLETABLE FAR*, METARECORD FAR*, int nObj, LPARAM)
           -- #295. One blob, two pointers: the record at +0, the table after it. */
        INT blobLength = 0, tableOffset = 0, room = WOWCALL_MAX_BLOB;
        UINT function = 0;
        WORD stackPointer = (WORD)(VDM_REG(tib, VTIB_ESP) & WOW_WORD_MASK);
        /* ⚠ THE BLOB IS ON THE GUEST'S STACK, so it must fit there. A Win16 task's
             DGROUP starts with INSTANCEDATA, whose word at +0x0A is pStackTop -- the
             lowest offset the stack may reach (what the C runtime's chkstk compares
             SP with; SDK convention, not measured on this host). Leave the callback
             512 bytes of its own below the blob. If +0x0A is not a plausible limit
             (above SP) it is not used; and SP itself bounds the blob so the 16-bit
             subtraction cannot wrap. A record that does not fit is TRUNCATED in the
             callback's copy (rdSize still says the real size; the callback reads
             what lies above the cut, i.e. the parked frame -- harmless to read, and
             a callback that WRITES a record's tail is not one we have seen), and
             PlayMetaFileRecord recognises it by address and plays the snapshot. */
        if (stackBase) {
            WORD stackTop = wowgdi_peek((const volatile BYTE *)(ULONG_PTR)stackBase, WOWENUM_INSTANCE_STACK_TOP);
            INT  limit = (stackTop && stackTop < stackPointer) ? (INT)(stackPointer - stackTop) : (INT)stackPointer;
            limit -= WOWENUM_STACK_RESERVE;
            if (limit < room) room = limit;
        }
        if (!wowgdi_mf_next(room, &blobLength, &tableOffset, &function)) {
            if (function == WOWENUM_METAFILE_MALFORMED)
                wu_puts(note, noteCapacity, &noteLength, "ENUM metafile: ★ A MALFORMED RECORD (rdSize < 3 or"
                                       " past the end) ENDS THE WALK; ");
            else if (function == WOWENUM_METAFILE_NO_ROOM) {
                wu_puts(note, noteCapacity, &noteLength, "ENUM metafile: ★ NO STACK ROOM FOR THE HANDLE TABLE;"
                                       " REFUSED -- the caller returns FALSE; ");
                WowEnumStopped();
            }
            wu_puts(note, noteCapacity, &noteLength, "ENUM metafile complete: 0x");
            wu_puthex(note, noteCapacity, &noteLength, g_WowEnum.Calls, WOW_HEX_WORD_DIGITS);
            wu_puts(note, noteCapacity, &noteLength, " record(s)");
            WowEnumEnd();
            return 0;
        }
        arguments[0] = g_WowEnum.Parent;                   /* the guest's own hdc, verbatim */
        arguments[1] = 0; arguments[2] = 0;                 /* lpht: blob2, +toff            */
        arguments[3] = 0; arguments[4] = 0;                 /* lpmr: the blob itself         */
        arguments[5] = (WORD)g_wmf.nobj;
        arguments[6] = (WORD)(g_WowEnum.LParam >> WOW_WORD_SHIFT);
        arguments[7] = (WORD)(g_WowEnum.LParam & WOW_WORD_MASK);
        g_WowCallBlob2Argument = 1; g_WowCallBlob2Offset = tableOffset;
        if (!returnSelector || !stackBase
            || !WowCallEnter(tib, stackBase, returnSelector, g_WowEnum.Procedure, g_WowEnum.DataSelector, arguments, WOWENUM_METAFILE_ARGUMENTS,
                              0, WOWCALL_RET_KEEP, NULL, 0, 0,
                              g_wmf_blob, blobLength, WOWENUM_METAFILE_ARG_RECORD,
                              WowDlgIsSelectorAbsent((WORD)(g_WowEnum.Procedure >> WOW_WORD_SHIFT)))) {
            g_WowCallBlob2Argument = -1;
            /* ⚠ FALSE, unlike the window forms: the guest has not seen the whole
                 picture, and TRUE would tell it that it had. */
            wu_puts(note, noteCapacity, &noteLength, "ENUM metafile -- ★ THE CALL WAS REFUSED; the"
                                   " enumeration ends here and the caller returns FALSE");
            WowEnumStopped();
            WowEnumEnd();
            return 0;
        }
        g_wmf.rec_lin = g_WowCallBlobLinear;
        g_wmf.tbl_lin = g_WowCallBlobLinear ? g_WowCallBlobLinear + (DWORD)tableOffset : 0;
        if (g_WowCallDepth > 0) {
            g_WowCallFrames[g_WowCallDepth - 1].Action = WOWCALL_ACT_ENUMNEXT;
            g_WowCallFrames[g_WowCallDepth - 1].ActionArgument = 0;
        }
        ++g_WowEnum.Calls;
        wu_puts(note, noteCapacity, &noteLength, "ENUM metarecord fn=0x");
        wu_puthex(note, noteCapacity, &noteLength, function, WOW_HEX_WORD_DIGITS);
        wu_puts(note, noteCapacity, &noteLength, " bytes=0x");
        wu_puthex(note, noteCapacity, &noteLength, g_wmf.rec_bytes, WOWENUM_HEX_RECORD_DIGITS);
        if (g_wmf.truncated)
            wu_puts(note, noteCapacity, &noteLength, " -- ★ TRUNCATED IN THE CALLBACK'S COPY (too big for the"
                                   " stack blob); PlayMetaFileRecord plays the full record");
        return 1;
    }

    if (g_WowEnum.Kind == WOWENUM_PROPS) {
        /* EnumPropProc(HWND, LPCSTR lpszName, HANDLE hData) -- s90, #296 */
        PCWOWENUM_FONT entry;
        INT isAtom, nameLength = 0;
        if (g_WowEnum.Index >= g_WowEnumFontCount) {
            wu_puts(note, noteCapacity, &noteLength, "ENUM props complete: 0x");
            wu_puthex(note, noteCapacity, &noteLength, g_WowEnum.Calls, WOW_HEX_WORD_DIGITS);
            wu_puts(note, noteCapacity, &noteLength, " propert(ies)");
            WowEnumEnd();
            return 0;
        }
        entry = &g_WowEnumFonts[g_WowEnum.Index++];
        isAtom = (entry->Blob[0] == 0);
        arguments[0] = g_WowEnum.Parent;
        arguments[1] = 0;
        arguments[2] = isAtom ? (WORD)(entry->Blob[1] | (entry->Blob[2] << WOW_BYTE_SHIFT)) : 0;   /* a string: filled */
        arguments[3] = entry->FontType;
        if (!isAtom) while (nameLength < WOWENUM_PROP_NAME_MAX && entry->Blob[nameLength]) ++nameLength;
        if (!returnSelector || !stackBase
            || !WowCallEnter(tib, stackBase, returnSelector, g_WowEnum.Procedure, g_WowEnum.DataSelector, arguments, WOWENUM_PAIR_ARGUMENTS,
                              0, WOWCALL_RET_KEEP, NULL, 0, 0,
                              isAtom ? NULL : entry->Blob, isAtom ? 0 : nameLength + 1, 1,
                              WowDlgIsSelectorAbsent((WORD)(g_WowEnum.Procedure >> WOW_WORD_SHIFT)))) {
            wu_puts(note, noteCapacity, &noteLength, "ENUM props -- ★ THE CALL WAS REFUSED; the"
                                   " enumeration ends here");
            WowEnumEnd();
            return 0;
        }
        if (g_WowCallDepth > 0) {
            g_WowCallFrames[g_WowCallDepth - 1].Action = WOWCALL_ACT_ENUMNEXT;
            g_WowCallFrames[g_WowCallDepth - 1].ActionArgument = 0;
        }
        ++g_WowEnum.Calls;
        wu_puts(note, noteCapacity, &noteLength, "ENUM prop -> ");
        if (isAtom) { wu_puts(note, noteCapacity, &noteLength, "atom 0x"); wu_puthex(note, noteCapacity, &noteLength, arguments[2], WOW_HEX_WORD_DIGITS); }
        else { wu_puts(note, noteCapacity, &noteLength, "\""); wu_puts(note, noteCapacity, &noteLength, (PCSTR)entry->Blob);
               wu_puts(note, noteCapacity, &noteLength, "\""); }
        return 1;
    }

    if (g_WowEnum.Kind == WOWENUM_LINE) {
        /* LineDDA(x1, y1, x2, y2, proc, data): proc(x, y, lpData) for each point
           on the line, in order. The points are computed the way a DDA does --
           one step along the major axis -- so the caller gets the same sequence
           it would plot itself. */
        INT index = g_WowEnum.Index;
        INT deltaX = g_WowEnum.EndX - g_WowEnum.StartX, deltaY = g_WowEnum.EndY - g_WowEnum.StartY;
        INT pointX, pointY;
        if (index >= g_WowEnum.Steps) {
            wu_puts(note, noteCapacity, &noteLength, "ENUM LineDDA complete: 0x");
            wu_puthex(note, noteCapacity, &noteLength, g_WowEnum.Calls, WOW_HEX_WORD_DIGITS);
            wu_puts(note, noteCapacity, &noteLength, " point(s)");
            WowEnumEnd();
            return 0;
        }
        pointX = g_WowEnum.StartX + MulDiv(deltaX, index, g_WowEnum.Steps);
        pointY = g_WowEnum.StartY + MulDiv(deltaY, index, g_WowEnum.Steps);
        g_WowEnum.Index = index + 1;
        arguments[0] = (WORD)(SHORT)pointX;
        arguments[1] = (WORD)(SHORT)pointY;
        argumentCount  = WOWENUM_LINE_ARGUMENTS;                       /* lpData is appended below */
    } else {
        /* A window source. Walk the table from the cursor, skipping slots that
           are free, are not real windows, or do not match the filter. */
        for (;;) {
            wowuser_win_t *window;
            if (g_WowEnum.Index >= WOWUSER_MAX_WIN) {
                wu_puts(note, noteCapacity, &noteLength, "ENUM complete: 0x");
                wu_puthex(note, noteCapacity, &noteLength, g_WowEnum.Calls, WOW_HEX_WORD_DIGITS);
                wu_puts(note, noteCapacity, &noteLength, " window(s) -- the caller returns TRUE");
                WowEnumEnd();
                return 0;
            }
            window = &g_wu_win[g_WowEnum.Index++];
            if (!window->hwnd || !window->hwnd32 || window->foreign) continue;
            if (g_WowEnum.Kind == WOWENUM_CHILDREN) {
                if (window->parent != g_WowEnum.Parent) continue;
            } else {
                /* ⚠ TOP-LEVEL ONLY, and "top level" here means "no parent we
                     issued": a control belongs to its dialog, not to the desktop.
                     EnumTaskWindows walks the same list because this host runs
                     ONE Win16 task -- so every window we have IS that task's.
                     Said here rather than left as a coincidence. */
                if (window->parent) continue;
                /* s92 (#306): ...no longer ONE task -- EnumTaskWindows keeps to
                     the hTask it was given (a window of unknown task still shows). */
                if (g_WowEnum.Kind == WOWENUM_TASK && g_wu_enumtask && window->task
                    && window->task != g_wu_enumtask) continue;
            }
            window16 = window->hwnd;
            break;
        }
        arguments[0] = window16;
        argumentCount  = 1;
    }

    /* lParam / lpData: one DWORD, high word first. */
    arguments[argumentCount]     = (WORD)(g_WowEnum.LParam >> WOW_WORD_SHIFT);
    arguments[argumentCount + 1] = (WORD)(g_WowEnum.LParam & WOW_WORD_MASK);
    argumentCount += WOWENUM_LPARAM_WORDS;

    if (!returnSelector || !stackBase
        || !WowCallEnter(tib, stackBase, returnSelector, g_WowEnum.Procedure, g_WowEnum.DataSelector, arguments, argumentCount,
                          /* returnLinear */ 0, WOWCALL_RET_KEEP, NULL,
                          window16, 0, NULL, 0, -1,
                          WowDlgIsSelectorAbsent((WORD)(g_WowEnum.Procedure >> WOW_WORD_SHIFT)))) {
        wu_puts(note, noteCapacity, &noteLength, "ENUM -- ★ THE CALL WAS REFUSED (depth, or no"
                               " return selector); the enumeration ends here and"
                               " the caller keeps the TRUE it was given");
        WowEnumEnd();
        return 0;
    }
    if (g_WowCallDepth > 0) {
        g_WowCallFrames[g_WowCallDepth - 1].Action = WOWCALL_ACT_ENUMNEXT;
        g_WowCallFrames[g_WowCallDepth - 1].ActionArgument = window16;
    }
    ++g_WowEnum.Calls;
    wu_puts(note, noteCapacity, &noteLength, "ENUM -> 0x");
    wu_puthex(note, noteCapacity, &noteLength, g_WowEnum.Procedure >> WOW_WORD_SHIFT, WOW_HEX_WORD_DIGITS);
    wu_puts(note, noteCapacity, &noteLength, ":0x");
    wu_puthex(note, noteCapacity, &noteLength, g_WowEnum.Procedure & WOW_WORD_MASK, WOW_HEX_WORD_DIGITS);
    wu_puts(note, noteCapacity, &noteLength, "(");
    {   INT index;
        for (index = 0; index < argumentCount; ++index) {
            if (index) wu_puts(note, noteCapacity, &noteLength, " ");
            wu_puthex(note, noteCapacity, &noteLength, arguments[index], WOW_HEX_WORD_DIGITS);
        }
    }
    wu_puts(note, noteCapacity, &noteLength, ")");
    return 1;
}

#endif /* NTVDMEX_WOWENUM_H */
