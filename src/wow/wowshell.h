#ifndef NTVDMEX_WOWSHELL_H
#define NTVDMEX_WOWSHELL_H
/*
 * wowshell.h -- ★★★ SHELL.DLL's OWN ID SPACE. GH #128, session 44.
 *
 * ── WHY THERE IS A FOURTH DISPATCHER ────────────────────────────────────────
 * `C:\WINDOWS\SYSTEM32\SHELL.DLL` on XP is 5120 bytes and it is not an
 * implementation of anything -- `tools/ne/wowthunks.py` finds **34 WOW32 stubs**
 * in it and nothing else. So SHELL is a thunk module exactly as USER and GDI are,
 * with a numbering ALL ITS OWN, and this file exists for the same reason
 * wowuser.h does: `0x16` is `SetFocus` in USER's table and `ShellAbout` in this
 * one, and a single switch holding both id spaces is precisely how this host once
 * came to answer `RegisterClass` with `GetProfileInt`.
 *
 * ── ★★ WHY NOTEPAD NEEDS IT: Help > About IS NOT A DIALOG ───────────────────
 * Notepad has seven DIALOG resources and none of them is the About box. Its NE
 * import table (`tools/ne/neimports.py`) names `USER.174 LOADICON` and
 * `SHELL.22 SHELLABOUT`, and the call arrives at run time as
 * ShellAbout(hWnd, szApp, szOtherStuff, hIcon) -- hWnd Notepad's own main
 * window, hIcon what LoadIcon(hInstance, MAKEINTRESOURCE(1)) returned (ICON 1 is
 * Notepad's own).
 *
 * ⇒ the whole of Help > About is one API call, and answering it is the box.
 *
 * ── ★★ WHICH ID IT IS, MEASURED RATHER THAN ASSUMED FROM THE ORDINAL ────────
 * The ids in a thunk module are not required to be its export ordinals, so this
 * was measured: Help > About arrives as a SHELL-module BOP carrying id **0x16**,
 * 12 argument bytes, and return-stub offset `0x00c7` at OFF_FROM (all three as
 * logged at run time), and 0x16 == 22 is SHELLABOUT's ordinal in SHELL.DLL's
 * non-resident name table.
 *
 * ⇒ `ShellAbout` is SHELL id **0x16**, 12 argument bytes, return stub `0x00c7`,
 *   and those three fields are what `wow_shell_anchor()` in main.c identifies the
 *   whole table by -- the same shape as USER's anchor, and it cannot mis-fire
 *   quietly: all three have to agree or the dispatcher never engages at all.
 * ★ Three of the neighbours agree with the same reading, which is what makes it a
 *   reading and not a coincidence: `9 DRAGACCEPTFILES` -> id 0x09, 4 arg bytes
 *   (HWND + BOOL); `11 DRAGQUERYFILE` -> id 0x0b, 10 (HDROP + UINT + far + UINT);
 *   `12 DRAGFINISH` -> id 0x0c, 2 (HDROP). Wrong ids do not produce four
 *   signatures that match four documented parameter lists.
 *
 * ── THE ARGUMENT BLOCK ──────────────────────────────────────────────────────
 * Reversed as always -- the base is the LAST word pushed -- so the documented
 * parameter list lays out as, and it comes to exactly the 12 bytes the call carries:
 *
 *     +0x00 WORD  hIcon                  (pushed last)
 *     +0x02 DWORD szOtherStuff  16:16
 *     +0x06 DWORD szApp         16:16
 *     +0x0a WORD  hWnd                   (pushed first)
 */

/* SHELL's ids. Numbered in THEIR OWN space -- 0x16 here is not 0x16 in USER's. */
#define WOWSHELL_SHELLABOUT   0x0016
/* ── ★ DRAG AND DROP, from neneeds.py's list. Notepad accepts dropped files.
     `9 DRAGACCEPTFILES` -> id 0x09, 4 args (HWND, BOOL); `11 DRAGQUERYFILE` ->
     id 0x0b, 10 args (HDROP, UINT, LPSTR, UINT) = 2+2+4+2. Both add up to what
     their own stubs declare, and both were already named in the header above as
     part of what made "the ids are the ordinals" a reading rather than a guess. */
/* ── ★★ 0x14 ShellExecute -- ONE OF THE FOUR SINGLE CALLS THAT EACH BLOCK A
     GUEST. WINFILE and PACKAGER both import it, and WINFILE is a file manager:
     "open the thing I double-clicked" is most of what it exists to do.
     ShellExecute(hwnd, lpOperation, lpFile, lpParameters, lpDirectory, nShow)
     = 2 + 4 + 4 + 4 + 4 + 2 = 20 bytes, which is what the stub declares.
   ⚠ ITS RETURN IS NOT A BOOLEAN AND NOT A HANDLE. Win16 returns an HINSTANCE
     that is really a status: > 32 means success, <= 32 is an error code, and
     Win32's ShellExecuteA kept the same convention -- so the value passes
     straight through and must NOT be normalised to 0/1. */
#define WOWSHELL_SHELLEXECUTE 0x0014
#define WOWSHELL_SHELLEXECUTE_ARG_SHOW   0
#define WOWSHELL_SHELLEXECUTE_ARG_DIR    2
#define WOWSHELL_SHELLEXECUTE_ARG_PARAMS 6
#define WOWSHELL_SHELLEXECUTE_ARG_FILE   10
#define WOWSHELL_SHELLEXECUTE_ARG_OP     14
#define WOWSHELL_SHELLEXECUTE_ARG_HWND   18

/* 0x15 FindExecutable(lpFile, lpDirectory, lpResult) -- 4+4+4 = 12. */
#define WOWSHELL_FINDEXECUTABLE 0x0015
#define WOWSHELL_FINDEXECUTABLE_ARG_RESULT 0
#define WOWSHELL_FINDEXECUTABLE_ARG_DIR    4
#define WOWSHELL_FINDEXECUTABLE_ARG_FILE   8

/* 0x25 DoEnvironmentSubst(lpszString, cbString) -- 4+2 = 6. Expands %VAR% IN
   PLACE, and the buffer it is given is the only one it may use. */
#define WOWSHELL_DOENVSUBST   0x0025
#define WOWSHELL_DOENVSUBST_ARG_CB  0
#define WOWSHELL_DOENVSUBST_ARG_STR 2

/* ── ★★ 0x22 ExtractIcon(hInst, lpszExeFileName, nIconIndex) = 8 ────────────
     "Give me icon N out of that file", and it is how PROGMAN draws a program
     item and how PACKAGER shows what it has packaged. The Win32 call has the
     same three arguments and the same three answers -- an HICON, 1 for "the file
     has none", 0 for "no such file" -- so what this host has to add is only the
     handle: an HICON from ANOTHER MODULE cannot be described by the ordinal or
     name a lazy token carries, so it is minted as WOWUSER_AD_KIND_REALICON.
   ⚠ nIconIndex == -1 IS A COUNT QUERY, not an extraction, and it must not mint
     anything: the answer is a number, not a handle. */
#define WOWSHELL_EXTRACTICON  0x0022
/* s90 (#297): XP's shell.dll thunks for FindEnvironmentString (ord 38) and
   InternalExtractIcon (ord 39) arrive with NO argument bytes (ids 0x26/0x27, as
   logged) -- XP's WOW never implemented them. Stock answers 0 (and DX 0) to both, measured
   in w_misc; so does this, on purpose rather than through the step-over. */
#define WOWSHELL_FINDENVSTRING      0x0026
#define WOWSHELL_INTERNALEXTRACTICON 0x0027
#define WOWSHELL_EXTRACTICON_ARG_INDEX 0
#define WOWSHELL_EXTRACTICON_ARG_FILE  2   /* far */
#define WOWSHELL_EXTRACTICON_ARG_HINST 6

/* ── 0x24 ExtractAssociatedIcon(hInst, lpIconPath, lpiIcon) = 10 ─────────────
     The same, for a DOCUMENT: follow the association, and REWRITE the caller's
     path buffer with the file the icon actually came from. Both the path and the
     index are in/out, which is why they are far pointers rather than values.
   ⚠ THE BUFFER IS THE GUEST'S AND ITS SIZE IS NOT PASSED. Win32's own contract
     is the same (it assumes MAX_PATH), so the copy back is bounded at MAX_PATH
     and the log says if the result was longer -- a silent overrun into a guest's
     data segment is not a trade this host makes. */
#define WOWSHELL_EXTRACTASSOCIATEDICON 0x0024
#define WOWSHELL_EXTRACTASSOCIATEDICON_ARG_LPIICON 0   /* far -- WORD in/out */
#define WOWSHELL_EXTRACTASSOCIATEDICON_ARG_PATH    4   /* far -- char[] in/out */
#define WOWSHELL_EXTRACTASSOCIATEDICON_ARG_HINST   8

/* ── ★ 0x2b RegisterShellHook(hWnd, fAction) ─────────────────────────────────
     PROGMAN calls it because PROGMAN IS THE SHELL: it is asking to be told when
     top-level windows appear, vanish or want activating. We record the window
     and answer TRUE, and THE LOG SAYS PLAINLY THAT NO HOOK MESSAGE IS EVER
     POSTED -- because this host runs one Win16 task at a time, so there are no
     other Win16 windows to report, and reporting the Win32 desktop's would mean
     handing the guest 16-bit handles for windows it cannot own.
   ⚠ THAT IS AN ANSWER, NOT A LIE, AND THE DIFFERENCE IS THE SUBSCRIPTION: the
     call's contract is "you are registered", which is true. A shell that is told
     nothing happened is in the same position as a shell on an idle desktop. */
#define WOWSHELL_REGISTERSHELLHOOK 0x002b
#define WOWSHELL_REGISTERSHELLHOOK_ARG_ACTION 0
#define WOWSHELL_REGISTERSHELLHOOK_ARG_HWND   2

#define WOWSHELL_DRAGACCEPTFILES 0x0009
#define WOWSHELL_DRAGACCEPTFILES_ARG_ACCEPT 0
#define WOWSHELL_DRAGACCEPTFILES_ARG_HWND   2
#define WOWSHELL_DRAGQUERYFILE   0x000b
#define WOWSHELL_DRAGQUERYFILE_ARG_CCH   0
#define WOWSHELL_DRAGQUERYFILE_ARG_BUF   2
#define WOWSHELL_DRAGQUERYFILE_ARG_INDEX 6
#define WOWSHELL_DRAGQUERYFILE_ARG_HDROP 8

#define WOWSHELL_SHELLABOUT_ARG_HICON 0
#define WOWSHELL_SHELLABOUT_ARG_OTHER 2
#define WOWSHELL_SHELLABOUT_ARG_APP   6
#define WOWSHELL_SHELLABOUT_ARG_HWND  10

/* ── ★★★★★ THE REGISTRATION DATABASE -- MS PAINT'S FIRST WALL. ───────────────
     A run of PBRUSH.EXE put up, in its own words:

         Paintbrush: "Failed to register server."

     and the calls behind it name themselves through their own arguments:
     `HKEY_CLASSES_ROOT` + "PBrush", then "Paintbrush Picture", "pbrush.exe",
     "protocol\StdFileEditing", "server", "verb\0" -- Paint registering itself as
     an OLE server in Windows 3.1's REG.DAT. These are SHELL.DLL ordinals 1-7,
     and they are exactly the four `tools/ne/neneeds.py --todo` listed as
     PBRUSH's outstanding SHELL work. Two independent methods, one answer.

   ★★★ AND THE REAL BUG WAS THE ANCHOR, NOT THE MISSING CALLS. Every one of them
     was logged as "?'s table -- a DIFFERENT id space" and answered by nobody,
     because `wow_shell_anchor()` recognised SHELL's code segment from
     `ShellAbout` ALONE -- and Paint never calls ShellAbout. `DragAcceptFiles`,
     which this file has implemented since session 44, went unanswered in that
     run for the same reason. An anchor that matches one call identifies a module
     only for the programs that happen to make that call; see main.c, where it
     now matches the whole stub table computed from the file.

   ── THE IDS AND THE BLOCKS, AS THE CALLS ARRIVE ────────────────────────────
     id, argument bytes and the return-stub offset each call carries at
     OFF_FROM, as logged from the running guest -- a measurement, not a
     parameter list copied out of a book:

       ord 1 REGOPENKEY    id 0x01  12 args  retstub 0x002b
       ord 2 REGCREATEKEY  id 0x02  12 args  retstub 0x0038
       ord 3 REGCLOSEKEY   id 0x03   4 args  retstub 0x0045
       ord 4 REGDELETEKEY  id 0x04   8 args  retstub 0x0052
       ord 5 REGSETVALUE   id 0x05  20 args  retstub 0x005f
       ord 6 REGQUERYVALUE id 0x06  16 args  retstub 0x006c
       ord 7 REGENUMKEY    id 0x07  16 args  retstub 0x0079

     Reversed as always -- the base is the LAST word pushed -- and cross-checked
     against the observed call. PBRUSH's RegCreateKey carried
     `(6e9a 09c7 0ae6 09c7 0001 0000)`: +0x00 is the far `phkResult` pushed last,
     +0x04 the far "PBrush", and +0x08 the DWORD `1` -- HKEY_CLASSES_ROOT. The
     parameter list and the 12 bytes the call carries agree. */
#define WOWSHELL_REGOPENKEY    0x0001
#define WOWSHELL_REGCREATEKEY  0x0002
#define WOWSHELL_REGOPENKEY_ARG_RESULT 0   /* HKEY FAR*  -- pushed last               */
#define WOWSHELL_REGOPENKEY_ARG_SUBKEY 4   /* LPCSTR                                  */
#define WOWSHELL_REGOPENKEY_ARG_HKEY   8   /* HKEY       -- pushed first              */

#define WOWSHELL_REGCLOSEKEY   0x0003
#define WOWSHELL_REGCLOSEKEY_ARG_HKEY 0

#define WOWSHELL_REGDELETEKEY  0x0004
#define WOWSHELL_REGDELETEKEY_ARG_SUBKEY 0
#define WOWSHELL_REGDELETEKEY_ARG_HKEY   4

#define WOWSHELL_REGSETVALUE   0x0005
#define WOWSHELL_REGSETVALUE_ARG_CBDATA 0
#define WOWSHELL_REGSETVALUE_ARG_DATA   4
#define WOWSHELL_REGSETVALUE_ARG_TYPE   8
#define WOWSHELL_REGSETVALUE_ARG_SUBKEY 12
#define WOWSHELL_REGSETVALUE_ARG_HKEY   16

#define WOWSHELL_REGQUERYVALUE 0x0006
#define WOWSHELL_REGQUERYVALUE_ARG_CBVALUE 0   /* LONG FAR*  -- in: capacity, out: length */
#define WOWSHELL_REGQUERYVALUE_ARG_VALUE   4   /* LPSTR                                   */
#define WOWSHELL_REGQUERYVALUE_ARG_SUBKEY  8
#define WOWSHELL_REGQUERYVALUE_ARG_HKEY    12

#define WOWSHELL_REGENUMKEY    0x0007
#define WOWSHELL_REGENUMKEY_ARG_CBBUF 0
#define WOWSHELL_REGENUMKEY_ARG_BUF   4
#define WOWSHELL_REGENUMKEY_ARG_INDEX 8
#define WOWSHELL_REGENUMKEY_ARG_HKEY  12

/* Win3.1 SHELL.DLL's own error numbers -- NOT Win32's. 0 is success in both. */
#define WOWSHELL_ERR_BADKEY      2
#define WOWSHELL_ERR_CANTOPEN    3
#define WOWSHELL_ERR_CANTREAD    4
#define WOWSHELL_ERR_CANTWRITE   5
#define WOWSHELL_ERR_OUTOFMEMORY 6
#define WOWSHELL_ERR_INVALID     7

/* ShellExecute/FindExecutable: SE_ERR_FNF, and the highest value that is an error
   rather than an instance handle (anything above 32 succeeded). */
#define WOWSHELL_SE_ERR_FNF       2
#define WOWSHELL_SE_ERR_LAST      32
#define WOWSHELL_OPERATION_MAX    64
#define WOWSHELL_ENV_BUFFER       (MAX_PATH * 2)
#define WOWSHELL_ABOUT_APP_MAX    160
#define WOWSHELL_ABOUT_OTHER_MAX  320
#define WOWSHELL_SUBKEY_MAX       256
#define WOWSHELL_VALUE_MAX        512
#define WOWSHELL_LOG_NAME_MAX     64
/* A Win16 HDROP: a word offset to the first name, then NUL-terminated names. */
#define WOWSHELL_DROP_MAX_OFFSET  0x0800
#define WOWSHELL_DROP_MAX_FILES   512
#define WOWSHELL_DRAGQUERYFILE_COUNT 0xFFFF   /* iFile -1: how many files */

/*
 * ── ★★★ WHERE THE WIN16 REGISTRATION DATABASE ACTUALLY LIVES ────────────────
 * Under `HKEY_CURRENT_USER\Software\NTVDMEX\Win16Reg`, and NOT under the real
 * `HKEY_CLASSES_ROOT`, which is what a literal reading of the API would do.
 *
 * ⚠ A LITERAL READING WOULD PUBLISH THE GUEST INTO THE HOST. Paint's very first
 *   act is to register `PBrush`, `pbrush.exe` and a `StdFileEditing` verb as a
 *   system-wide document handler. Under real HKCR that is a change to the user's
 *   Windows installation -- made by a program they only meant to LOOK at, on
 *   every launch, and not undone when it exits. This host routes a guest; it
 *   does not get to re-register the desktop's file associations as a side
 *   effect.
 * ★ AND NOTHING NEEDS IT THERE. The database exists so Win16 OLE clients can
 *   find Win16 servers, and they look it up through these same seven calls, so a
 *   private hive answers every question the real one would. What is given up is
 *   cross-bitness OLE with 32-bit XP programs, which this host does not do.
 * ★ HKEY_CURRENT_USER for the same reason `src/host/settings.h` uses it: the VDM
 *   runs as the logged-in user and must not need administrator rights.
 *
 * ⚠ THE ROOT ARRIVES AS TWO DIFFERENT NUMBERS AND BOTH ARE REAL. Win16 defines
 *   HKEY_CLASSES_ROOT as 1, and PBRUSH's own call passes 1 -- but the call
 *   OLESVR makes passes 0x80000000, Win32's value. Both were seen in one run of
 *   one program, so both are accepted rather than one being declared correct.
 */
#define WOWSHELL_HKCR16     0x00000001ul
#define WOWSHELL_HKCR32     0x80000000ul
#define WOWSHELL_REG_PATH   "Software\\NTVDMEX\\Win16Reg"

/* Guest-visible key handles. A Win16 HKEY is a full DWORD, so the token can be
   made unmistakable rather than squeezed: anything in this range is ours, and
   anything else that is not a root is a handle we never issued. */
#define WOWSHELL_KEYTOK_BASE 0x57160000ul
#define WOWSHELL_KEYTOK_MAX  64

#endif /* NTVDMEX_WOWSHELL_H */
