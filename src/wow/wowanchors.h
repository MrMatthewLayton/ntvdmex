/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * HOW THE HOST RECOGNISES A THUNK MODULE'S CODE SEGMENT.
 *               GH #128, session 45.  GENERATED -- see the regenerate line.
 *
 * THE PROBLEM:
 * Every thunk module (USER, GDI, SHELL, COMMDLG, KEYBOARD, krnl386's two
 * segments) has an id space ALL ITS OWN -- `0x44` is `ReleaseDC` in USER's
 * numbering and `DeleteDC` in GDI's -- so a call can only be dispatched once the
 * host knows which module its stub is in. It cannot be told: krnl386 loads these
 * modules itself and allocates their selectors at run time through INT 31h 0501,
 * so selector numbers are not even stable between runs. The host therefore
 * LEARNS a module's code segment by recognising a call that could only have come
 * from it, and dispatches that id space from then on.
 *
 * WHY THE ANCHOR IS THE WHOLE STUB TABLE AND NOT ONE CALL (Importance = 3):
 * Because a one-call anchor only identifies the module for the programs that
 * happen to make THAT call, and the cost of that was measured rather than
 * imagined. `wow_shell_anchor()` recognised SHELL from `ShellAbout` alone. MS
 * Paint never calls `ShellAbout` -- it calls `RegCreateKey`, `RegSetValue`,
 * `RegQueryValue` and `DragAcceptFiles` -- so SHELL was never identified at all,
 * every one of those was logged as "?'s table -- a DIFFERENT id space" and
 * answered by nobody, and `DragAcceptFiles` had been IMPLEMENTED since session
 * 44. A missing service and an unidentified module read identically in the log;
 * only the anchor tells them apart.
 *
 * WHY MATCHING ANY ROW IS STILL SAFE:
 * A WOW32 stub is 13 bytes and pushes both of the things it is matched on --
 * the argument byte count, a zero word and the id -- then far-calls the
 * module's common thunk, so the return address the call carries is the stub
 * + 13. (id, argbytes, retstub) is three independent fields read off the file:
 * a wrong segment would have to push this id and these argument bytes at
 * exactly the offset the call returns to. And the dispatcher only
 * consults a table after excluding every segment already identified as another
 * module's, so an ambiguity between two tables cannot silently pick one.
 *
 * [CAUTION]: THREE ROWS ARE PROBABLY NOT STUBS AT ALL -- `{ 0x2080, 0, ... }`, one in
 * GDI and two in USER. An id of 0x2080 taking no arguments is out of family
 * with every other row, and the likeliest reading is ordinary 16-bit code
 * that happens to match the stub's byte shape.
 * They are LEFT IN rather than filtered, because the filter would be a guess
 * too and these are harmless: a row only ever fires if a real call carries
 * that exact triple, and nothing does.
 *
 * REGENERATE:
 *    tools/ne/wowthunks.py --anchor guest/win16/shell.dll
 *    tools/ne/wowthunks.py --anchor guest/win16/gdi.exe
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_WOWANCHORS_H
#define NTVDMEX_WOWANCHORS_H

#include "host_state.h"

typedef struct _WOW_ANCHOR
{
    WORD Id;
    WORD ArgumentBytes;
    WORD ReturnStub;
} WOW_ANCHOR, *PWOW_ANCHOR;
typedef const WOW_ANCHOR *PCWOW_ANCHOR;

extern const WOW_ANCHOR g_WowShellAnchors[34];
extern const WOW_ANCHOR g_WowGdiAnchors[367];
extern const WOW_ANCHOR g_WowCommdlgAnchors[8];
extern const WOW_ANCHOR g_WowKeyboardAnchors[11];
extern const WOW_ANCHOR g_WowSoundAnchors[17];
extern const WOW_ANCHOR g_WowMmediaAnchors[2];

/* Defined in wowanchors.c (#335). */
INT WowAnchorHit(PCWOW_ANCHOR table, INT count, WORD id, WORD argumentBytes, WORD returnStub);

#endif /* NTVDMEX_WOWANCHORS_H */
