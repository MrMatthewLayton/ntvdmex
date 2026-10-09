/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * CALLING A GUEST'S CALLBACK ONCE PER ITEM. GH #128, session 57.
 *
 * THE SHAPE, AND WHY IT IS A THIRD USE OF ONE MECHANISM:
 * `EnumWindows`, `EnumChildWindows`, `EnumTaskWindows` and `LineDDA` are one
 * function with four sources of items: the host has a list, and for each item it
 * must call SIXTEEN-BIT CODE and look at what comes back. That is the same
 * problem the EDIT chain solved in session 44 and the modal dialog loop solved
 * earlier today -- a service cannot call the guest, because entering guest code
 * means replacing the whole context, which only the BOP handler can do and undo.
 * So the service ASKS, the handler calls, and when the call RETURNS the frame's
 * `Action` says what to do next. Here, "next" is the next item.
 *
 *   EnumWindows BOP     park the caller; answer TRUE in advance
 *     -> proc(item 0)   WowCallEnter
 *     <- returns TRUE   ACT_ENUMNEXT: more items? yes
 *     -> proc(item 1)
 *     <- returns FALSE  THE CALLBACK SAID STOP. The caller's TRUE is revised
 *                         to FALSE and the enumeration ends -- which is the
 *                         whole contract of these functions.
 *
 * [CAUTION]: ONE AT A TIME, AND NESTING IS REFUSED RATHER THAN TRUNCATED. A callback that
 * starts a second enumeration would overwrite the first one's cursor, and the
 * first would then walk the second's list -- a wrong answer that looks like a
 * right one. A refusal is a FALSE the caller is entitled to read as "could not
 * enumerate", and it is written to the log.
 *
 * [CAUTION]: THE LIST IS SNAPSHOT BY INDEX, NOT COPIED. The callback may create or destroy
 * windows (that is legal, and TASKMAN's End Task does it), so the cursor is an
 * index into the live table and each step re-validates the slot. A window that
 * vanished mid-enumeration is skipped, not reported as a stale handle.
 *
 * s89/s90: FONTS AND OBJECTS ARE NOW HERE:
 * EnumFontFamilies (s89), EnumFonts and EnumObjects (s90, #296) walk Win16
 * structures built up front in wowgdi.h. The note below is the original reason
 * they waited, kept because the rule it states still holds:
 * `EnumFonts`, `EnumFontFamilies` and `EnumObjects` are the same shape and were
 * NOT implemented, because their callbacks receive POINTERS TO STRUCTURES --
 * LOGFONT, TEXTMETRIC, LOGPEN, LOGBRUSH -- whose Win16 layouts differ from
 * Win32's in exactly the way `ABC` and `RECT` do. Building those from memory is
 * the one thing this project does not do; they need a measurement (a guest's own
 * reads, or the 16-bit headers) first. The mechanism below is ready for them.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_WOWENUM_H
#define NTVDMEX_WOWENUM_H

/* WowEnumStep: the first call of an enumeration, or the next. */
#define WOWENUM_NEXT                    0
#define WOWENUM_FIRST                   1

/* The callbacks' argument blocks, in words, and the structures they point at. */
#define WOWENUM_MAX_ARGUMENTS           8
#define WOWENUM_LPARAM_WORDS            2
#define WOWENUM_LINE_ARGUMENTS          2       /* X, y -- lpData follows */
#define WOWENUM_PAIR_ARGUMENTS          4       /* EnumObjects, EnumProps */
#define WOWENUM_FONT_ARGUMENTS          7
#define WOWENUM_FONT_ARG_METRICS        2       /* Lpntm: the second far pointer */
#define WOWENUM_METAFILE_ARGUMENTS      8
#define WOWENUM_METAFILE_ARG_RECORD     3       /* Lpmr: the blob itself */
#define WOWENUM_ELF_FACE_NAME           18      /* LOGFONT16.lfFaceName */
#define WOWENUM_ELF_FACE_NAME_END       50
#define WOWENUM_LOGPEN16_SIZE           10
#define WOWENUM_LOGBRUSH16_SIZE         8
#define WOWENUM_PROP_NAME_MAX           31
#define WOWENUM_LINE_MAX_STEPS          4096
#define WOWENUM_INSTANCE_STACK_TOP      0x0A    /* INSTANCEDATA.pStackTop */
#define WOWENUM_STACK_RESERVE           512
#define WOWENUM_HEX_RECORD_DIGITS       6

/* [CAUTION]: THE CONSTANTS AND THE THREE ENTRY POINTS THE SERVICES CALL LIVE IN
 * wowcall.h, NOT HERE, and the reason is the include order: wowgdi.h and
 * wowuser.h are compiled BEFORE this file and both arm an enumeration. Putting
 * them in the header that already owns the callback mechanism keeps one
 * declaration rather than a forward declaration in each dispatcher.
 */

typedef struct _WOWENUM
{
    INT   Kind;
    DWORD Procedure;                         /* the guest's callback */
    WORD  DataSelector;                      /* the DS it must be entered with */
    DWORD LParam;                            /* the caller's opaque value, passed to every call */
    DWORD ReturnLinear;                      /* the caller's return hole -- revised only on a STOP */
    WORD  Parent;                            /* WOWENUM_CHILDREN */
    INT   Index;                             /* cursor: the next window slot, or the next point */
    /* WOWENUM_LINE */
    INT StartX;
    INT StartY;
    INT EndX;
    INT EndY;
    INT Steps;
    DWORD Calls;                             /* how many callbacks were made, for the log */
} WOWENUM, *PWOWENUM;

/* Defined in wowenum.c (#335). */
INT WowEnumBusy(VOID);
INT WowEnumBegin(
    INT kind,
    DWORD procedure,
    WORD dataSelector,
    DWORD lParam,
    DWORD returnLinear,
    WORD parent);
VOID WowEnumLine(INT startX, INT startY, INT endX, INT endY);
INT WowEnumStep(
    volatile BYTE *tib,
    DWORD stackBase,
    WORD returnSelector,
    INT isFirst,
    DWORD result,
    PSTR note,
    INT noteCapacity);

#endif /* NTVDMEX_WOWENUM_H */
