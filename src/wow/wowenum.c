/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * CALLING A GUEST'S CALLBACK ONCE PER ITEM. GH #128, session 57.
 *
 * The code of wowenum.h (#335): its functions and state, in their original order;
 * its own translation unit, declared in wowenum.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "host_state.h"
#include "log.h"
#include "ne.h"
#include "wow32.h"
#include "wowanchors.h"
#include "wowsched.h"
#include "wowcall.h"
#include "wowmsg.h"
#include "wowres.h"
#include "wowwin.h"
#include "wowgdi.h"
#include "wowuser.h"
#include "wowdlg.h"
#include "wowenum.h"
#include "host_wow.h"

static WOWENUM g_WowEnum;

INT WowEnumBusy(VOID)
{
    return g_WowEnum.Kind != WOWENUM_NONE;
}

/* Start one. Returns 0 if another is already running (see the nesting note). */
INT WowEnumBegin(
    INT kind,
    DWORD procedure,
    WORD dataSelector,
    DWORD lParam,
    DWORD returnLinear,
    WORD parent)
{
    if (g_WowEnum.Kind != WOWENUM_NONE)
        return 0;
    if (!procedure || !(procedure >> WORD_SHIFT))
        return 0;
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
 * what makes it a DDA rather than a plot -- the caller draws each one itself.
 */
VOID WowEnumLine(INT startX, INT startY, INT endX, INT endY)
{
    INT deltaX = endX - startX, deltaY = endY - startY;

    if (deltaX < 0)
        deltaX = -deltaX;
    if (deltaY < 0)
        deltaY = -deltaY;
    g_WowEnum.StartX = startX;
    g_WowEnum.StartY = startY;
    g_WowEnum.EndX = endX;
    g_WowEnum.EndY = endY;
    /* s91: THE END POINT IS EXCLUDED, as GDI's LineDDA excludes it (w_ldda vs stock:
     * (0,0)-(10,4) is 10 calls ending at (9,4); a zero-length line is none).
     */
    g_WowEnum.Steps = (deltaX > deltaY ? deltaX : deltaY);
    /* [CAUTION]: AND IT IS BOUNDED. Every point is a 16-bit CALL, so a line across a large
     * desktop is thousands of context switches -- correct, and slow enough to
     * look like a hang. A diagonal of this display is ~2,000 points; 4,096 is
     * generous and the log says when it clamps, so a guest that legitimately
     * wants more is a line to grep for rather than a mystery stall.
     */
    if (g_WowEnum.Steps > WOWENUM_LINE_MAX_STEPS)
        g_WowEnum.Steps = WOWENUM_LINE_MAX_STEPS;
}

/* #295: an EnumMetaFile walk owns objects (the handle table) and a snapshot; both
 * go here, so a stop, a refusal and a completion all release them.
 */
static VOID WowEnumEnd(VOID)
{
    if (g_WowEnum.Kind == WOWENUM_METAFILE)
        WowGdiMetafileEnd();
    g_WowEnum.Kind = WOWENUM_NONE;
    g_WowEnum.Procedure = 0;
}

/* THE CALLER'S ANSWER, WHEN THE CALLBACK STOPPED IT (Importance = 1):
 * EnumWindows and friends return TRUE when the whole list was walked and FALSE
 * when the callback broke out. The service writes TRUE up front -- so a
 * refused or empty enumeration still answers something -- and this is the only
 * thing that revises it. Same four-byte write into guest memory as the modal
 * loop's unwind, and for the same reason: the hole outlives the context
 * switches in between.
 */
static VOID WowEnumStopped(VOID)
{
    volatile BYTE *hole;

    if (!g_WowEnum.ReturnLinear)
        return;
    hole = (volatile BYTE *)(ULONG_PTR)g_WowEnum.ReturnLinear;
    hole[0] = 0;
    hole[1] = 0;
    hole[2] = 0;
    hole[3] = 0;
}

/* One step. Returns 1 if a 16-bit call is now in flight, 0 if the enumeration is
 * over (and the guest should be resumed where it stands).
 *
 * `stop` is the callback's answer to the PREVIOUS item -- 0 means "stop" -- and
 * is ignored on the first step, where there is no previous item.
 */
INT WowEnumStep(
    volatile BYTE *tib,
    DWORD stackBase,
    WORD returnSelector,
    INT isFirst,
    DWORD result,
    PSTR note,
    INT noteCapacity)
{
    INT noteLength = 0;
    /* [CAUTION]: FOUR, NOT THREE. A window callback takes (hwnd, lParam) = 3 words, but
     * LineDDA's takes (x, y, lpData) = FOUR -- and the lParam words are written
     * at [argumentCount] and [argumentCount+1] below, which for the line case is [2] and [3].
     * Three would have written the last word off the end of this array.
     */
    WORD arguments[WOWENUM_MAX_ARGUMENTS];        /* s89: 7 for EnumFontFamilies; the window and line forms use 3-4 */
    INT  argumentCount = 0;
    WORD window16 = 0;

    if (g_WowEnum.Kind == WOWENUM_NONE)
        return 0;

    /* #295: the previous record's callback may have changed its handle table
     * (through PlayMetaFileRecord, or by hand); take it back FIRST, before a stop
     * below releases the table's objects -- a pen created by the record that
     * said stop is still the enumeration's to delete.
     */
    if (!isFirst && g_WowEnum.Kind == WOWENUM_METAFILE && WowGdiMetafileReadBack())
    {
        WowNotePut(note, noteCapacity, &noteLength, "ENUM metafile: ★ the guest wrote a non-object value into"
                               " its handle table; those entries were NOT believed. ");
    }

    /* [INFO]: THE CALLBACK'S VETO. Win16 says a callback returning 0 ends the
     * enumeration, and the function then answers FALSE.
     */
    /* s91: NOT FOR LineDDA -- its callback is VOID, so AX is whatever the procedure
     * left there; reading it as "stop" cut a line short at the first point whose y
     * happened to be 0 (tests/probes/win16/w_ldda: 1 call where stock makes 10).
     */
    if (!isFirst && g_WowEnum.Kind != WOWENUM_LINE && !(result & WORD_MASK))
    {
        WowNotePut(note, noteCapacity, &noteLength, "ENUM stopped by the callback after 0x");
        WowNoteHex(note, noteCapacity, &noteLength, g_WowEnum.Calls, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " call(s) -- the caller returns FALSE");
        WowEnumStopped();
        WowEnumEnd();
        return 0;
    }

    if (g_WowEnum.Kind == WOWENUM_FONTS)
    {
        /* EnumFontFamProc(LPENUMLOGFONT, LPNEWTEXTMETRIC, int FontType, LPARAM) */
        PCWOWENUM_FONT entry;
        if (g_WowEnum.Index >= g_WowEnumFontCount)
        {
            WowNotePut(note, noteCapacity, &noteLength, "ENUM fonts complete: 0x");
            WowNoteHex(note, noteCapacity, &noteLength, g_WowEnum.Calls, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " font(s)");
            WowEnumEnd();
            return 0;
        }
        entry = &g_WowEnumFonts[g_WowEnum.Index++];
        arguments[0] = 0;
        arguments[1] = 0;             /* lpelf: filled by WowCallEnter */
        arguments[2] = 0;
        arguments[3] = 0;             /* lpntm: ditto, +146 into the blob */
        arguments[4] = entry->FontType;
        arguments[5] = (WORD)(g_WowEnum.LParam >> WORD_SHIFT);
        arguments[6] = (WORD)(g_WowEnum.LParam & WORD_MASK);
        g_WowCallBlob2Argument = WOWENUM_FONT_ARG_METRICS;
        g_WowCallBlob2Offset = WOWENUM_ELF16;
        if (!returnSelector || !stackBase
            || !WowCallEnter(tib, stackBase, returnSelector, g_WowEnum.Procedure, g_WowEnum.DataSelector, arguments, WOWENUM_FONT_ARGUMENTS,
                              0, WOWCALL_RET_KEEP, NULL, 0, 0,
                              entry->Blob, (INT)sizeof entry->Blob, 0,
                              WowDlgIsSelectorAbsent((WORD)(g_WowEnum.Procedure >> WORD_SHIFT))))
        {
            g_WowCallBlob2Argument = -1;
            WowNotePut(note, noteCapacity, &noteLength, "ENUM fonts -- ★ THE CALL WAS REFUSED; the"
                                   " enumeration ends here");
            WowEnumEnd();
            return 0;
        }
        if (g_WowCallDepth > 0)
        {
            g_WowCallFrames[g_WowCallDepth - 1].Action = WOWCALL_ACT_ENUMNEXT;
            g_WowCallFrames[g_WowCallDepth - 1].ActionArgument = 0;
        }
        ++g_WowEnum.Calls;
        WowNotePut(note, noteCapacity, &noteLength, "ENUM font -> \"");
        {   INT index;
        for (index = WOWENUM_ELF_FACE_NAME; index < WOWENUM_ELF_FACE_NAME_END && entry->Blob[index]; ++index)
        {
                CHAR character[2];
                character[0] = (CHAR)entry->Blob[index];
                character[1] = 0;
                WowNotePut(note, noteCapacity, &noteLength, character); } }
        WowNotePut(note, noteCapacity, &noteLength, "\" type=0x");
        WowNoteHex(note, noteCapacity, &noteLength, entry->FontType, WOW_HEX_BYTE_DIGITS);
        return 1;
    }

    if (g_WowEnum.Kind == WOWENUM_OBJECTS)
    {
        /* EnumObjectsProc(LPVOID lpLogObject, LPARAM) -- s90, #296 */
        PCWOWENUM_FONT entry;
        if (g_WowEnum.Index >= g_WowEnumFontCount)
        {
            WowNotePut(note, noteCapacity, &noteLength, "ENUM objects complete: 0x");
            WowNoteHex(note, noteCapacity, &noteLength, g_WowEnum.Calls, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " object(s)");
            WowEnumEnd();
            return 0;
        }
        entry = &g_WowEnumFonts[g_WowEnum.Index++];
        arguments[0] = 0;
        arguments[1] = 0;             /* lpLogObject: filled by WowCallEnter */
        arguments[2] = (WORD)(g_WowEnum.LParam >> WORD_SHIFT);
        arguments[3] = (WORD)(g_WowEnum.LParam & WORD_MASK);
        if (!returnSelector || !stackBase
            || !WowCallEnter(tib, stackBase, returnSelector, g_WowEnum.Procedure, g_WowEnum.DataSelector, arguments, WOWENUM_PAIR_ARGUMENTS,
                              0, WOWCALL_RET_KEEP, NULL, 0, 0,
                              entry->Blob, entry->FontType == OBJ_PEN ? WOWENUM_LOGPEN16_SIZE : WOWENUM_LOGBRUSH16_SIZE, 0,
                              WowDlgIsSelectorAbsent((WORD)(g_WowEnum.Procedure >> WORD_SHIFT))))
        {
            WowNotePut(note, noteCapacity, &noteLength, "ENUM objects -- ★ THE CALL WAS REFUSED; the"
                                   " enumeration ends here");
            WowEnumEnd();
            return 0;
        }
        if (g_WowCallDepth > 0)
        {
            g_WowCallFrames[g_WowCallDepth - 1].Action = WOWCALL_ACT_ENUMNEXT;
            g_WowCallFrames[g_WowCallDepth - 1].ActionArgument = 0;
        }
        ++g_WowEnum.Calls;
        WowNotePut(note, noteCapacity, &noteLength, "ENUM object -> style=0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(entry->Blob[0] | (entry->Blob[1] << BYTE_SHIFT)), WOW_HEX_WORD_DIGITS);
        return 1;
    }

    if (g_WowEnum.Kind == WOWENUM_METAFILE)
    {
        /* EnumMetaFileProc(HDC, HANDLETABLE FAR*, METARECORD FAR*, int nObj, LPARAM)
         * -- #295. One blob, two pointers: the record at +0, the table after it.
         */
        INT blobLength = 0, tableOffset = 0, room = WOWCALL_MAX_BLOB;
        UINT function = 0;
        WORD stackPointer = (WORD)(VDM_REG(tib, VTIB_ESP) & WORD_MASK);
        /* [CAUTION]: THE BLOB IS ON THE GUEST'S STACK, so it must fit there. A Win16 task's
         * DGROUP starts with INSTANCEDATA, whose word at +0x0A is pStackTop -- the
         * lowest offset the stack may reach (what the C runtime's chkstk compares
         * SP with; SDK convention, not measured on this host). Leave the callback
         * 512 bytes of its own below the blob. If +0x0A is not a plausible limit
         * (above SP) it is not used; and SP itself bounds the blob so the 16-bit
         * subtraction cannot wrap. A record that does not fit is TRUNCATED in the
         * callback's copy (rdSize still says the real size; the callback reads
         * what lies above the cut, i.e. the parked frame -- harmless to read, and
         * a callback that WRITES a record's tail is not one we have seen), and
         * PlayMetaFileRecord recognises it by address and plays the snapshot.
         */
        if (stackBase)
        {
            WORD stackTop = WowGdiPeek((const volatile BYTE *)(ULONG_PTR)stackBase, WOWENUM_INSTANCE_STACK_TOP);
            INT  limit = (stackTop && stackTop < stackPointer) ? (INT)(stackPointer - stackTop) : (INT)stackPointer;
            limit -= WOWENUM_STACK_RESERVE;
            if (limit < room)
                room = limit;
        }
        if (!WowGdiMetafileNext(room, &blobLength, &tableOffset, &function))
        {
            if (function == WOWGDI_MF_MALFORMED)
                WowNotePut(note, noteCapacity, &noteLength, "ENUM metafile: ★ A MALFORMED RECORD (rdSize < 3 or"
                                       " past the end) ENDS THE WALK; ");
            else if (function == WOWGDI_MF_NO_ROOM)
            {
                WowNotePut(note, noteCapacity, &noteLength, "ENUM metafile: ★ NO STACK ROOM FOR THE HANDLE TABLE;"
                                       " REFUSED -- the caller returns FALSE; ");
                WowEnumStopped();
            }
            WowNotePut(note, noteCapacity, &noteLength, "ENUM metafile complete: 0x");
            WowNoteHex(note, noteCapacity, &noteLength, g_WowEnum.Calls, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " record(s)");
            WowEnumEnd();
            return 0;
        }
        arguments[0] = g_WowEnum.Parent;                   /* the guest's own hdc, verbatim */
        arguments[1] = 0;
        arguments[2] = 0;                 /* lpht: blob2, +toff */
        arguments[3] = 0;
        arguments[4] = 0;                 /* lpmr: the blob itself */
        arguments[5] = (WORD)g_WowGdiMetafile.ObjectCount;
        arguments[6] = (WORD)(g_WowEnum.LParam >> WORD_SHIFT);
        arguments[7] = (WORD)(g_WowEnum.LParam & WORD_MASK);
        g_WowCallBlob2Argument = 1;
        g_WowCallBlob2Offset = tableOffset;
        if (!returnSelector || !stackBase
            || !WowCallEnter(tib, stackBase, returnSelector, g_WowEnum.Procedure, g_WowEnum.DataSelector, arguments, WOWENUM_METAFILE_ARGUMENTS,
                              0, WOWCALL_RET_KEEP, NULL, 0, 0,
                              g_WowGdiMetafileBlob, blobLength, WOWENUM_METAFILE_ARG_RECORD,
                              WowDlgIsSelectorAbsent((WORD)(g_WowEnum.Procedure >> WORD_SHIFT))))
        {
            g_WowCallBlob2Argument = -1;
            /* [CAUTION]: FALSE, unlike the window forms: the guest has not seen the whole
             * picture, and TRUE would tell it that it had.
             */
            WowNotePut(note, noteCapacity, &noteLength, "ENUM metafile -- ★ THE CALL WAS REFUSED; the"
                                   " enumeration ends here and the caller returns FALSE");
            WowEnumStopped();
            WowEnumEnd();
            return 0;
        }
        g_WowGdiMetafile.RecordLinear = g_WowCallBlobLinear;
        g_WowGdiMetafile.TableLinear = g_WowCallBlobLinear ? g_WowCallBlobLinear + (DWORD)tableOffset : 0;
        if (g_WowCallDepth > 0)
        {
            g_WowCallFrames[g_WowCallDepth - 1].Action = WOWCALL_ACT_ENUMNEXT;
            g_WowCallFrames[g_WowCallDepth - 1].ActionArgument = 0;
        }
        ++g_WowEnum.Calls;
        WowNotePut(note, noteCapacity, &noteLength, "ENUM metarecord fn=0x");
        WowNoteHex(note, noteCapacity, &noteLength, function, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " bytes=0x");
        WowNoteHex(note, noteCapacity, &noteLength, g_WowGdiMetafile.RecordBytes, WOWENUM_HEX_RECORD_DIGITS);
        if (g_WowGdiMetafile.IsTruncated)
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ TRUNCATED IN THE CALLBACK'S COPY (too big for the"
                                   " stack blob); PlayMetaFileRecord plays the full record");
        return 1;
    }

    if (g_WowEnum.Kind == WOWENUM_PROPS)
    {
        /* EnumPropProc(HWND, LPCSTR lpszName, HANDLE hData) -- s90, #296 */
        PCWOWENUM_FONT entry;
        INT isAtom, nameLength = 0;
        if (g_WowEnum.Index >= g_WowEnumFontCount)
        {
            WowNotePut(note, noteCapacity, &noteLength, "ENUM props complete: 0x");
            WowNoteHex(note, noteCapacity, &noteLength, g_WowEnum.Calls, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " propert(ies)");
            WowEnumEnd();
            return 0;
        }
        entry = &g_WowEnumFonts[g_WowEnum.Index++];
        isAtom = (entry->Blob[0] == 0);
        arguments[0] = g_WowEnum.Parent;
        arguments[1] = 0;
        arguments[2] = isAtom ? (WORD)(entry->Blob[1] | (entry->Blob[2] << BYTE_SHIFT)) : 0;   /* a string: filled */
        arguments[3] = entry->FontType;
        if (!isAtom)
            while (nameLength < WOWENUM_PROP_NAME_MAX && entry->Blob[nameLength])
                ++nameLength;
        if (!returnSelector || !stackBase
            || !WowCallEnter(tib, stackBase, returnSelector, g_WowEnum.Procedure, g_WowEnum.DataSelector, arguments, WOWENUM_PAIR_ARGUMENTS,
                              0, WOWCALL_RET_KEEP, NULL, 0, 0,
                              isAtom ? NULL : entry->Blob, isAtom ? 0 : nameLength + 1, 1,
                              WowDlgIsSelectorAbsent((WORD)(g_WowEnum.Procedure >> WORD_SHIFT))))
        {
            WowNotePut(note, noteCapacity, &noteLength, "ENUM props -- ★ THE CALL WAS REFUSED; the"
                                   " enumeration ends here");
            WowEnumEnd();
            return 0;
        }
        if (g_WowCallDepth > 0)
        {
            g_WowCallFrames[g_WowCallDepth - 1].Action = WOWCALL_ACT_ENUMNEXT;
            g_WowCallFrames[g_WowCallDepth - 1].ActionArgument = 0;
        }
        ++g_WowEnum.Calls;
        WowNotePut(note, noteCapacity, &noteLength, "ENUM prop -> ");
        if (isAtom)
        {
            WowNotePut(note, noteCapacity, &noteLength, "atom 0x");
            WowNoteHex(note, noteCapacity, &noteLength, arguments[2], WOW_HEX_WORD_DIGITS);
        }
        else { WowNotePut(note, noteCapacity, &noteLength, "\"");
        WowNotePut(note, noteCapacity, &noteLength, (PCSTR)entry->Blob);
               WowNotePut(note, noteCapacity, &noteLength, "\""); }
        return 1;
    }

    if (g_WowEnum.Kind == WOWENUM_LINE)
    {
        /* LineDDA(x1, y1, x2, y2, proc, data): proc(x, y, lpData) for each point
         * on the line, in order. The points are computed the way a DDA does --
         * one step along the major axis -- so the caller gets the same sequence
         * it would plot itself.
         */
        INT index = g_WowEnum.Index;
        INT deltaX = g_WowEnum.EndX - g_WowEnum.StartX, deltaY = g_WowEnum.EndY - g_WowEnum.StartY;
        INT pointX, pointY;
        if (index >= g_WowEnum.Steps)
        {
            WowNotePut(note, noteCapacity, &noteLength, "ENUM LineDDA complete: 0x");
            WowNoteHex(note, noteCapacity, &noteLength, g_WowEnum.Calls, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " point(s)");
            WowEnumEnd();
            return 0;
        }
        pointX = g_WowEnum.StartX + MulDiv(deltaX, index, g_WowEnum.Steps);
        pointY = g_WowEnum.StartY + MulDiv(deltaY, index, g_WowEnum.Steps);
        g_WowEnum.Index = index + 1;
        arguments[0] = (WORD)(SHORT)pointX;
        arguments[1] = (WORD)(SHORT)pointY;
        argumentCount  = WOWENUM_LINE_ARGUMENTS;                       /* lpData is appended below */
    }
    else
    {
        /* A window source. Walk the table from the cursor, skipping slots that
         * are free, are not real windows, or do not match the filter.
         */
        for (;;)
        {
            PWOWUSER_WINDOW window;
            if (g_WowEnum.Index >= WOWUSER_MAX_WIN)
            {
                WowNotePut(note, noteCapacity, &noteLength, "ENUM complete: 0x");
                WowNoteHex(note, noteCapacity, &noteLength, g_WowEnum.Calls, WOW_HEX_WORD_DIGITS);
                WowNotePut(note, noteCapacity, &noteLength, " window(s) -- the caller returns TRUE");
                WowEnumEnd();
                return 0;
            }
            window = &g_WowUserWindows[g_WowEnum.Index++];
            if (!window->Window16 || !window->Window32 || window->IsForeign)
                continue;
            if (g_WowEnum.Kind == WOWENUM_CHILDREN)
            {
                if (window->Parent != g_WowEnum.Parent)
                    continue;
            }
            else
            {
                /* [CAUTION]: TOP-LEVEL ONLY, and "top level" here means "no parent we
                 * issued": a control belongs to its dialog, not to the desktop.
                 * EnumTaskWindows walks the same list because this host runs
                 * ONE Win16 task -- so every window we have IS that task's.
                 * Said here rather than left as a coincidence.
                 */
                if (window->Parent)
                    continue;
                /* s92 (#306): ...no longer ONE task -- EnumTaskWindows keeps to
                 * the hTask it was given (a window of unknown task still shows).
                 */
                if (g_WowEnum.Kind == WOWENUM_TASK && g_WowUserEnumTask && window->Task
                    && window->Task != g_WowUserEnumTask)
                    continue;
            }
            window16 = window->Window16;
            break;
        }
        arguments[0] = window16;
        argumentCount  = 1;
    }

    /* lParam / lpData: one DWORD, high word first. */
    arguments[argumentCount]     = (WORD)(g_WowEnum.LParam >> WORD_SHIFT);
    arguments[argumentCount + 1] = (WORD)(g_WowEnum.LParam & WORD_MASK);
    argumentCount += WOWENUM_LPARAM_WORDS;

    if (!returnSelector || !stackBase
        || !WowCallEnter(tib, stackBase, returnSelector, g_WowEnum.Procedure, g_WowEnum.DataSelector, arguments, argumentCount,
                          /* returnLinear */ 0, WOWCALL_RET_KEEP, NULL,
                          window16, 0, NULL, 0, -1,
                          WowDlgIsSelectorAbsent((WORD)(g_WowEnum.Procedure >> WORD_SHIFT))))
    {
        WowNotePut(note, noteCapacity, &noteLength, "ENUM -- ★ THE CALL WAS REFUSED (depth, or no"
                               " return selector); the enumeration ends here and"
                               " the caller keeps the TRUE it was given");
        WowEnumEnd();
        return 0;
    }
    if (g_WowCallDepth > 0)
    {
        g_WowCallFrames[g_WowCallDepth - 1].Action = WOWCALL_ACT_ENUMNEXT;
        g_WowCallFrames[g_WowCallDepth - 1].ActionArgument = window16;
    }
    ++g_WowEnum.Calls;
    WowNotePut(note, noteCapacity, &noteLength, "ENUM -> 0x");
    WowNoteHex(note, noteCapacity, &noteLength, g_WowEnum.Procedure >> WORD_SHIFT, WOW_HEX_WORD_DIGITS);
    WowNotePut(note, noteCapacity, &noteLength, ":0x");
    WowNoteHex(note, noteCapacity, &noteLength, g_WowEnum.Procedure & WORD_MASK, WOW_HEX_WORD_DIGITS);
    WowNotePut(note, noteCapacity, &noteLength, "(");
    {   INT index;
        for (index = 0; index < argumentCount; ++index)
        {
            if (index)
                WowNotePut(note, noteCapacity, &noteLength, " ");
            WowNoteHex(note, noteCapacity, &noteLength, arguments[index], WOW_HEX_WORD_DIGITS);
        }
    }
    WowNotePut(note, noteCapacity, &noteLength, ")");
    return 1;
}
