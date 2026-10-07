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
     name a lazy token carries, so it is minted as AD_KIND_REALICON.
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

static HKEY  g_WowShellKeys[WOWSHELL_KEYTOK_MAX];
static INT   g_WowShellKeyCount = 0;
static HKEY  g_WowShellRoot = NULL;

/* The private hive's root, created on first use. NULL means the registry itself
   refused, which is reported to the guest rather than papered over. */
static HKEY WowShellRoot(VOID)
{
    HKEY key;
    DWORD disposition = 0;
    if (g_WowShellRoot) return g_WowShellRoot;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, WOWSHELL_REG_PATH, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
                        NULL, &key, &disposition) != ERROR_SUCCESS)
        return NULL;
    g_WowShellRoot = key;
    return key;
}

/* A guest DWORD -> the real key it names, or NULL. */
static HKEY WowShellKey32(DWORD key16)
{
    DWORD index;
    if (key16 == WOWSHELL_HKCR16 || key16 == WOWSHELL_HKCR32) return WowShellRoot();
    if (key16 < WOWSHELL_KEYTOK_BASE) return NULL;
    index = key16 - WOWSHELL_KEYTOK_BASE;
    if (index >= (DWORD)g_WowShellKeyCount) return NULL;
    return g_WowShellKeys[index];
}

/* Mint a token for a key we just opened. 0 = the table is full. */
static DWORD WowShellKey16(HKEY key)
{
    INT index;
    if (!key) return 0;
    for (index = 0; index < g_WowShellKeyCount; ++index)
        if (!g_WowShellKeys[index]) break;                       /* reuse a closed slot */
    if (index == g_WowShellKeyCount) {
        if (g_WowShellKeyCount >= WOWSHELL_KEYTOK_MAX) return 0;
        index = g_WowShellKeyCount++;
    }
    g_WowShellKeys[index] = key;
    return WOWSHELL_KEYTOK_BASE + (DWORD)index;
}

/* Write a DWORD through a 16:16 far-pointer ARGUMENT (wow32_farput writes
   through a pointer inside a STRUCT, which is a different thing). */
static INT WowShellPutDword(const wow32_frame_t *frame, INT argumentOffset, DWORD value)
{
    volatile BYTE *bytes = wow32_argptr(frame, argumentOffset);
    if (!bytes) return 0;
    wow32_pokew(bytes,     (WORD)(value & WOW_WORD_MASK));
    wow32_pokew(bytes + WOW_WORD_BYTES, (WORD)(value >> WOW_WORD_SHIFT));
    return 1;
}

/* The subkey argument, or NULL -- and the difference is load-bearing: every one
   of these calls gives a null lpSubKey the meaning "the key itself". */
static PCSTR WowShellSubkey(const wow32_frame_t *frame, INT argumentOffset,
                                PSTR buffer, INT capacity)
{
    return wow32_argstr(frame, argumentOffset, buffer, capacity) && buffer[0] ? buffer : NULL;
}

/* Shared reporting for the four calls that take (hKey, lpSubKey). */
static VOID WowShellNoteKey(PSTR note, INT noteCapacity, PINT noteLength,
                              PCSTR name, DWORD key16, PCSTR subkey)
{
    wu_puts(note, noteCapacity, noteLength, name);
    wu_puts(note, noteCapacity, noteLength, " key=0x");
    wu_puthex(note, noteCapacity, noteLength, key16, WOW_HEX_DWORD_DIGITS);
    if (key16 == WOWSHELL_HKCR16 || key16 == WOWSHELL_HKCR32)
        wu_puts(note, noteCapacity, noteLength, "(HKEY_CLASSES_ROOT)");
    wu_puts(note, noteCapacity, noteLength, " sub=");
    wu_putq(note, noteCapacity, noteLength, subkey ? subkey : "(the key itself)");
}

/*
 * ⚠ CALLED ONLY WHEN THE STUB IS SHELL'S. The caller checks, exactly as it does
 *   for USER; this file must never be reachable from another module's numbering.
 * `note` receives a short description for the caller's log line.
 */
static INT WowShellCall(wow32_frame_t *frame, PSTR note, INT noteCapacity)
{
    if (noteCapacity) note[0] = 0;
    switch (frame->id) {

    /* ── ★★★★★ 0x16 ShellAbout(hWnd, szApp, szOtherStuff, hIcon) ─────────────
         ★ THE REAL Win32 ONE IS THE RIGHT ANSWER, and for once that is not a
           shortcut -- it is what WOW itself does. `SHELL.DLL` is a thunk whose
           whole job is to reach the 32-bit shell, `ShellAboutA` in SHELL32 takes
           the same four parameters with the same meanings (including the `#`
           convention that splits szApp into a caption and a body), and the owner
           window we hand it is the guest's REAL window. Drawing our own About box
           would be inventing chrome, which is the answer this project threw away
           in session 42.
       ⚠ IT IS MODAL, AND ON THIS THREAD. ShellAboutA runs its own message loop
         and does not return until the box is dismissed, and it is called from the
         exec thread -- so the whole VDM is stopped for as long as the box is up.
         That is right for the CALLING task (a Win16 ShellAbout blocks it too) and
         wrong for every other one: real WOW gives each task a thread and the
         others keep running. Stated rather than discovered.
       ★ The dialog's own loop dispatches this thread's messages, so the guest's
         other windows keep painting and moving behind it. Nothing re-enters the
         guest, because the only thread that runs guest code is the one sitting
         inside this call.
       ⚠ hIcon IS A TOKEN, NOT AN ICON -- the guest got it from USER 0xad, which
         cannot know a cursor from an icon at the moment it is asked. Resolving it
         HERE is the point of use, where the guest has just said which it is by
         passing it as an icon. A NULL result is not a failure: ShellAbout with no
         icon shows the system's, which is what Windows does for a program with no
         icon of its own, and the note says so rather than leaving a silent blank.
       ⚠ AN OWNER WE DO NOT KNOW IS REPORTED, NOT REFUSED. The About box is still
         the correct answer to the call; what changes is that it comes up unowned,
         and a run that shows that line has found a window handle this host issued
         and then lost -- which is worth seeing. */
    /* ── ★★ 0x14 ShellExecute -- see the note by the ids. ────────────────────
       ⚠ A NULL lpOperation MEANS "open", and passing our empty buffer straight
         through would ask the shell to perform the verb "" -- which is not the
         same thing and fails. The distinction between "no string" and "an empty
         string" is exactly what wow32_argstr's return value is for. */
    case WOWSHELL_FINDENVSTRING:
    case WOWSHELL_INTERNALEXTRACTICON: {
        INT noteLength = 0;
        wu_puts(note, noteCapacity, &noteLength, frame->id == WOWSHELL_FINDENVSTRING
                ? "FindEnvironmentString -- NULL, as stock (XP's thunk has no arguments)"
                : "InternalExtractIcon -- 0, as stock (XP's thunk has no arguments)");
        wow32_setret(frame, 0);
        return 1;
    }

    case WOWSHELL_EXTRACTICON: {
        WORD instance = wow32_argw(frame, WOWSHELL_EXTRACTICON_ARG_HINST);
        INT  itemIndex   = (INT)(SHORT)wow32_argw(frame, WOWSHELL_EXTRACTICON_ARG_INDEX);
        CHAR fileName[MAX_PATH];
        INT  noteLength = 0;
        HICON icon;
        WORD token;
        (VOID)instance;
        wu_puts(note, noteCapacity, &noteLength, "ExtractIcon ");
        if (!wow32_argstr(frame, WOWSHELL_EXTRACTICON_ARG_FILE, fileName, sizeof fileName) || !fileName[0]) {
            wu_puts(note, noteCapacity, &noteLength, "-- ★ no file name; answered 0 (no such"
                                       " file), which is the documented answer");
            wow32_setret(frame, 0);
            return 1;
        }
        wu_putq(note, noteCapacity, &noteLength, fileName);
        wu_puts(note, noteCapacity, &noteLength, " index ");
        wu_puthex(note, noteCapacity, &noteLength, (DWORD)itemIndex, WOW_HEX_WORD_DIGITS);
        if (itemIndex == -1) {
            /* A COUNT QUERY. Win32 answers it the same way and it mints
               nothing -- the value is a number of icons, not a handle. */
            UINT count = (UINT)(ULONG_PTR)ExtractIconA(GetModuleHandleA(NULL), fileName,
                                                   (UINT)-1);
            wu_puts(note, noteCapacity, &noteLength, " -- a COUNT query -> ");
            wu_puthex(note, noteCapacity, &noteLength, count, WOW_HEX_WORD_DIGITS);
            wow32_setret(frame, (DWORD)(count & WOW_WORD_MASK));
            return 1;
        }
        icon = ExtractIconA(GetModuleHandleA(NULL), fileName, (UINT)itemIndex);
        if ((ULONG_PTR)icon == 1) {
            wu_puts(note, noteCapacity, &noteLength, " -- the file has NO icons (1), which is"
                                       " the documented in-band answer");
            wow32_setret(frame, 1);
            return 1;
        }
        if (!icon) {
            wu_puts(note, noteCapacity, &noteLength, " -- no such file or no such index -> 0");
            wow32_setret(frame, 0);
            return 1;
        }
        token = wowuser_sysres_mint_icon(icon);
        wu_puts(note, noteCapacity, &noteLength, " -> token 0x");
        wu_puthex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        if (!token) wu_puts(note, noteCapacity, &noteLength, " -- ★ THE TOKEN TABLE IS FULL, so"
                                             " the icon exists and the guest"
                                             " cannot be given it");
        wow32_setret(frame, token);
        return 1;
    }

    case WOWSHELL_EXTRACTASSOCIATEDICON: {
        volatile BYTE *indexPointer = wow32_argptr(frame, WOWSHELL_EXTRACTASSOCIATEDICON_ARG_LPIICON);
        volatile BYTE *pathPointer = wow32_argptr(frame, WOWSHELL_EXTRACTASSOCIATEDICON_ARG_PATH);
        CHAR path[MAX_PATH];
        INT  noteLength = 0, index;
        WORD itemIndex = 0, token;
        HICON icon;
        wu_puts(note, noteCapacity, &noteLength, "ExtractAssociatedIcon ");
        if (!wow32_argstr(frame, WOWSHELL_EXTRACTASSOCIATEDICON_ARG_PATH, path, sizeof path) || !path[0]) {
            wu_puts(note, noteCapacity, &noteLength, "-- ★ no path; answered 0");
            wow32_setret(frame, 0);
            return 1;
        }
        if (indexPointer) itemIndex = (WORD)(indexPointer[0] | (indexPointer[1] << WOW_BYTE_SHIFT));
        wu_putq(note, noteCapacity, &noteLength, path);
        wu_puts(note, noteCapacity, &noteLength, " index ");
        wu_puthex(note, noteCapacity, &noteLength, itemIndex, WOW_HEX_WORD_DIGITS);
        icon = ExtractAssociatedIconA(GetModuleHandleA(NULL), path, &itemIndex);
        if (!icon) {
            wu_puts(note, noteCapacity, &noteLength, " -- nothing associated -> 0");
            wow32_setret(frame, 0);
            return 1;
        }
        /* ★ BOTH OUT-PARAMETERS GO BACK, because the caller reads them: the path
             is now the file the icon came from (which may be a different file
             entirely) and the index is where in it. */
        if (pathPointer) {
            for (index = 0; index < MAX_PATH - 1 && path[index]; ++index) pathPointer[index] = (BYTE)path[index];
            pathPointer[index] = 0;
            if (index == MAX_PATH - 1)
                wu_puts(note, noteCapacity, &noteLength, " [★ path TRUNCATED at MAX_PATH]");
        }
        if (indexPointer) { indexPointer[0] = (BYTE)(itemIndex & WOW_BYTE_MASK); indexPointer[1] = (BYTE)(itemIndex >> WOW_BYTE_SHIFT); }
        token = wowuser_sysres_mint_icon(icon);
        wu_puts(note, noteCapacity, &noteLength, " -> ");
        wu_putq(note, noteCapacity, &noteLength, path);
        wu_puts(note, noteCapacity, &noteLength, " token 0x");
        wu_puthex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        wow32_setret(frame, token);
        return 1;
    }

    case WOWSHELL_REGISTERSHELLHOOK: {
        WORD window16 = wow32_argw(frame, WOWSHELL_REGISTERSHELLHOOK_ARG_HWND);
        WORD action  = wow32_argw(frame, WOWSHELL_REGISTERSHELLHOOK_ARG_ACTION);
        INT  noteLength = 0;
        wu_puts(note, noteCapacity, &noteLength, "RegisterShellHook(hwnd 0x");
        wu_puthex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        wu_puts(note, noteCapacity, &noteLength, ", action ");
        wu_puthex(note, noteCapacity, &noteLength, action, WOW_HEX_WORD_DIGITS);
        wu_puts(note, noteCapacity, &noteLength, ") -> registered."
                                   " ★ AND NO SHELL HOOK MESSAGE WILL EVER BE"
                                   " POSTED, said here rather than discovered:"
                                   " this host runs one Win16 task at a time, so"
                                   " there are no other Win16 windows to report,"
                                   " and the Win32 desktop's windows have no"
                                   " 16-bit handles to report them WITH. The"
                                   " subscription is true; the feed is empty.");
        wow32_setret(frame, 1);
        return 1;
    }

    case WOWSHELL_SHELLEXECUTE: {
        WORD window16 = wow32_argw(frame, WOWSHELL_SHELLEXECUTE_ARG_HWND);
        WORD showCommand = wow32_argw(frame, WOWSHELL_SHELLEXECUTE_ARG_SHOW);
        wowuser_win_t *window = wowuser_findwin(window16);
        CHAR operation[WOWSHELL_OPERATION_MAX], fileName[MAX_PATH], parameters[MAX_PATH], directory[MAX_PATH];
        INT  noteLength = 0, hasOperation, hasParameters, hasDirectory;
        DWORD result;
        hasOperation  = wow32_argstr(frame, WOWSHELL_SHELLEXECUTE_ARG_OP,     operation,     sizeof operation);
        hasParameters = wow32_argstr(frame, WOWSHELL_SHELLEXECUTE_ARG_PARAMS, parameters, sizeof parameters);
        hasDirectory = wow32_argstr(frame, WOWSHELL_SHELLEXECUTE_ARG_DIR,    directory,    sizeof directory);
        if (!wow32_argstr(frame, WOWSHELL_SHELLEXECUTE_ARG_FILE, fileName, sizeof fileName) || !fileName[0]) {
            wu_puts(note, noteCapacity, &noteLength, "ShellExecute -- ★ no lpFile; answered "
                                       "SE_ERR_FNF (2)");
            wow32_setret(frame, WOWSHELL_SE_ERR_FNF);
            return 1;
        }
        wu_puts(note, noteCapacity, &noteLength, "ShellExecute ");
        wu_putq(note, noteCapacity, &noteLength, hasOperation && operation[0] ? operation : "(open)");
        wu_puts(note, noteCapacity, &noteLength, " ");
        wu_putq(note, noteCapacity, &noteLength, fileName);
        if (hasParameters && parameters[0]) { wu_puts(note, noteCapacity, &noteLength, " args ");
                                    wu_putq(note, noteCapacity, &noteLength, parameters); }
        if (hasDirectory && directory[0])    { wu_puts(note, noteCapacity, &noteLength, " in ");
                                    wu_putq(note, noteCapacity, &noteLength, directory); }
        result = (DWORD)(ULONG_PTR)ShellExecuteA(window ? window->hwnd32 : NULL,
                                             (hasOperation && operation[0]) ? operation : NULL,
                                             fileName,
                                             (hasParameters && parameters[0]) ? parameters : NULL,
                                             (hasDirectory && directory[0]) ? directory : NULL,
                                             (INT)(SHORT)showCommand);
        wu_puts(note, noteCapacity, &noteLength, " -> ");
        wu_puthex(note, noteCapacity, &noteLength, result, WOW_HEX_WORD_DIGITS);
        wu_puts(note, noteCapacity, &noteLength, result > WOWSHELL_SE_ERR_LAST ? " (started)" : " (SE_ERR_*)");
        /* ⚠ TRUNCATED TO 16 BITS DELIBERATELY: the guest's variable is an
             HINSTANCE, which is a WORD here. A success value above 0xFFFF would
             wrap to something <= 32 and read as an error, so clamp instead. */
        if (result > WOW_WORD_MASK) result = WOW_WORD_MASK;
        wow32_setret(frame, result);
        return 1;
    }

    /* ── 0x15 FindExecutable(lpFile, lpDirectory, lpResult) -- which program
         opens this document. Same >32 convention as ShellExecute. */
    case WOWSHELL_FINDEXECUTABLE: {
        CHAR fileName[MAX_PATH], directory[MAX_PATH], output[MAX_PATH];
        volatile BYTE *resultPointer = wow32_argptr(frame, WOWSHELL_FINDEXECUTABLE_ARG_RESULT);
        INT noteLength = 0, hasDirectory, index;
        DWORD result;
        output[0] = 0;
        hasDirectory = wow32_argstr(frame, WOWSHELL_FINDEXECUTABLE_ARG_DIR, directory, sizeof directory);
        if (!wow32_argstr(frame, WOWSHELL_FINDEXECUTABLE_ARG_FILE, fileName, sizeof fileName) || !fileName[0] || !resultPointer) {
            wu_puts(note, noteCapacity, &noteLength, "FindExecutable -- ★ no lpFile or no "
                                       "result buffer; answered SE_ERR_FNF (2)");
            wow32_setret(frame, WOWSHELL_SE_ERR_FNF);
            return 1;
        }
        result = (DWORD)(ULONG_PTR)FindExecutableA(fileName,
                                               (hasDirectory && directory[0]) ? directory : NULL,
                                               output);
        wu_puts(note, noteCapacity, &noteLength, "FindExecutable ");
        wu_putq(note, noteCapacity, &noteLength, fileName);
        if (result > WOWSHELL_SE_ERR_LAST) {
            for (index = 0; index < (INT)sizeof output && output[index]; ++index) resultPointer[index] = (BYTE)output[index];
            resultPointer[index] = 0;
            wu_puts(note, noteCapacity, &noteLength, " -> ");
            wu_putq(note, noteCapacity, &noteLength, output);
        } else {
            resultPointer[0] = 0;
            wu_puts(note, noteCapacity, &noteLength, " -> none (SE_ERR_ ");
            wu_puthex(note, noteCapacity, &noteLength, result, WOW_HEX_WORD_DIGITS);
            wu_puts(note, noteCapacity, &noteLength, ")");
        }
        if (result > WOW_WORD_MASK) result = WOW_WORD_MASK;
        wow32_setret(frame, result);
        return 1;
    }

    /* ── 0x25 DoEnvironmentSubst(lpszString, cbString) -- expand %VAR% IN
         PLACE, inside the guest's own buffer.
       ⚠ THE RETURN IS A PACKED PAIR, not a status: the HIGH word is the length
         of the result and the LOW word is the size of the buffer. And the
         expansion must NOT be written back unless it FITS -- the buffer is the
         guest's and cbString is the only statement we have about its size. */
    case WOWSHELL_DOENVSUBST: {
        volatile BYTE *string = wow32_argptr(frame, WOWSHELL_DOENVSUBST_ARG_STR);
        WORD byteCount = wow32_argw(frame, WOWSHELL_DOENVSUBST_ARG_CB);
        CHAR input[WOWSHELL_ENV_BUFFER], output[WOWSHELL_ENV_BUFFER];
        INT noteLength = 0, index, count;
        DWORD expandedLength;
        if (!string || !byteCount) {
            wu_puts(note, noteCapacity, &noteLength, "DoEnvironmentSubst -- ★ no buffer");
            wow32_setret(frame, (DWORD)byteCount);
            return 1;
        }
        count = 0;
        while (count < (INT)sizeof input - 1 && count < (INT)byteCount && string[count]) { input[count] = (CHAR)string[count]; ++count; }
        input[count] = 0;
        expandedLength = ExpandEnvironmentStringsA(input, output, (DWORD)sizeof output);
        wu_puts(note, noteCapacity, &noteLength, "DoEnvironmentSubst ");
        wu_putq(note, noteCapacity, &noteLength, input);
        if (expandedLength && expandedLength <= (DWORD)byteCount) {
            for (index = 0; index < (INT)expandedLength && output[index]; ++index) string[index] = (BYTE)output[index];
            string[index] = 0;
            wu_puts(note, noteCapacity, &noteLength, " -> ");
            wu_putq(note, noteCapacity, &noteLength, output);
            wow32_setret(frame, ((DWORD)(WORD)index << WOW_WORD_SHIFT) | (DWORD)byteCount);
        } else {
            /* Too long, or nothing to do: leave the guest's buffer alone and
               report the original length. Truncating in place would hand the
               program a path that silently is not the path it asked about. */
            wu_puts(note, noteCapacity, &noteLength, expandedLength ? " -- ★ result does not fit; buffer"
                                             " left UNCHANGED"
                                           : " -- no substitution");
            wow32_setret(frame, ((DWORD)(WORD)count << WOW_WORD_SHIFT) | (DWORD)byteCount);
        }
        return 1;
    }

    case WOWSHELL_SHELLABOUT: {
        WORD window16 = wow32_argw(frame, WOWSHELL_SHELLABOUT_ARG_HWND);
        WORD iconToken = wow32_argw(frame, WOWSHELL_SHELLABOUT_ARG_HICON);
        wowuser_win_t *window = wowuser_findwin(window16);
        CHAR application[WOWSHELL_ABOUT_APP_MAX], otherText[WOWSHELL_ABOUT_OTHER_MAX];
        INT  noteLength = 0, bitCount = 0, result;
        /* The About box wants the full-size icon, so the size is the system's
           default -- the small-icon variant exists for the taskbar. */
        HICON icon = wowuser_sysres_hicon(iconToken, &bitCount, 0, 0);
        HWND  owner = window ? window->hwnd32 : NULL;

        wow32_argstr(frame, WOWSHELL_SHELLABOUT_ARG_APP,   application,   sizeof application);
        wow32_argstr(frame, WOWSHELL_SHELLABOUT_ARG_OTHER, otherText, sizeof otherText);

        wu_puts(note, noteCapacity, &noteLength, "ShellAbout ");
        wu_putq(note, noteCapacity, &noteLength, application);
        wu_puts(note, noteCapacity, &noteLength, " / ");
        wu_putq(note, noteCapacity, &noteLength, otherText);
        wu_puts(note, noteCapacity, &noteLength, " owner=0x");
        wu_puthex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window)          wu_puts(note, noteCapacity, &noteLength, " -- ★ NO SUCH WINDOW; the box"
                                                    " comes up UNOWNED");
        else if (!owner) wu_puts(note, noteCapacity, &noteLength, " -- no real window behind it;"
                                                    " the box comes up UNOWNED");
        wu_puts(note, noteCapacity, &noteLength, " icon=0x");
        wu_puthex(note, noteCapacity, &noteLength, iconToken, WOW_HEX_WORD_DIGITS);
        if (icon) {
            wu_puts(note, noteCapacity, &noteLength, " -> the app's own (");
            wu_puthex(note, noteCapacity, &noteLength, (DWORD)bitCount, WOW_HEX_BYTE_DIGITS);
            wu_puts(note, noteCapacity, &noteLength, " bpp)");
        } else {
            wu_puts(note, noteCapacity, &noteLength, " -- NOT RESOLVED; the system icon is used");
        }
        wu_puts(note, noteCapacity, &noteLength, " -- ★ MODAL: the VDM is stopped until it is"
                                   " dismissed");
        result = ShellAboutA(owner, application, otherText, icon);
        wu_puts(note, noteCapacity, &noteLength, "; dismissed, rc=0x");
        wu_puthex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_WORD_DIGITS);
        wow32_setret(frame, (DWORD)result);
        return 1;
    }

    /* ── ★ 0x09 DragAcceptFiles(hWnd, fAccept) ──────────────────────────────
         The real one, on the real window: accepting drops is a property the
         window manager enforces, and ours is the OS's. A guest that asks for it
         and then gets no WM_DROPFILES would be a lie one level down.
       ★ s92: AND THE DROP NOW ARRIVES -- WM_DROPFILES is relayed with a Win16 HDROP
         (a real global block; wowwin.h wowwin_drop16), read by DragQueryFile below
         and by SHELL.DLL's own 16-bit DragQueryPoint and DragFinish. */
    case WOWSHELL_DRAGACCEPTFILES: {
        WORD window16 = wow32_argw(frame, WOWSHELL_DRAGACCEPTFILES_ARG_HWND);
        WORD isAccept  = wow32_argw(frame, WOWSHELL_DRAGACCEPTFILES_ARG_ACCEPT);
        wowuser_win_t *window = wowuser_findwin(window16);
        INT noteLength = 0;
        wu_puts(note, noteCapacity, &noteLength, isAccept ? "DragAcceptFiles ACCEPT 0x"
                                       : "DragAcceptFiles REFUSE 0x");
        wu_puthex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->hwnd32) {
            wu_puts(note, noteCapacity, &noteLength, " -- no real window");
            wow32_setret(frame, 0);
            return 1;
        }
        DragAcceptFiles(window->hwnd32, isAccept ? TRUE : FALSE);
        wu_puts(note, noteCapacity, &noteLength, " -> the OS's (drops arrive as WM_DROPFILES, s92)");
        wow32_setret(frame, 0);
        return 1;
    }

    /* ── ★ 0x0b DragQueryFile(hDrop, iFile, lpszFile, cch) ──────────────────
       s92 (#305 M12): the HDROP is the global block wowwin_drop16 built (wowwin.h),
       read here the way SHELL.DLL's own DragQueryPoint reads it: locked through
       krnl386, pFiles at +0, the names from there. iFile 0xFFFF answers the count;
       a NULL buffer answers the length a name needs (without its NUL); otherwise at
       most cch-1 characters and a NUL are copied and the count copied is answered.
     ⚠ A handle that does not lock, or a block that does not parse, answers 0 --
       the honest "no files" this call always gave before drops were delivered. */
    case WOWSHELL_DRAGQUERYFILE: {
        WORD drop16 = wow32_argw(frame, WOWSHELL_DRAGQUERYFILE_ARG_HDROP);
        WORD itemIndex   = wow32_argw(frame, WOWSHELL_DRAGQUERYFILE_ARG_INDEX);
        WORD bufferSize   = wow32_argw(frame, WOWSHELL_DRAGQUERYFILE_ARG_CCH);
        volatile BYTE *output = wow32_argptr(frame, WOWSHELL_DRAGQUERYFILE_ARG_BUF);
        DWORD farPointer = g_ww_global16 ? g_ww_global16(WOWWIN_GLOBAL16_LOCK, drop16, 0) : 0;
        DWORD segmentBase = (farPointer >> WOW_WORD_SHIFT) ? dpmi_sel_base((WORD)(farPointer >> WOW_WORD_SHIFT)) : 0;
        volatile BYTE *dropBytes = segmentBase ? (volatile BYTE *)(ULONG_PTR)(segmentBase + (farPointer & WOW_WORD_MASK)) : NULL;
        DWORD result = 0;
        INT noteLength = 0;
        wu_puts(note, noteCapacity, &noteLength, "DragQueryFile drop 0x");
        wu_puthex(note, noteCapacity, &noteLength, drop16, WOW_HEX_WORD_DIGITS);
        wu_puts(note, noteCapacity, &noteLength, " index 0x");
        wu_puthex(note, noteCapacity, &noteLength, itemIndex, WOW_HEX_WORD_DIGITS);
        if (!dropBytes) {
            wu_puts(note, noteCapacity, &noteLength, " -- ★ the handle does not lock; 0");
            wow32_setret(frame, 0);
            return 1;
        }
        {   WORD offset = (WORD)(dropBytes[0] | (dropBytes[1] << WOW_BYTE_SHIFT)), count = 0;
            INT  guard = 0;
            while (offset < WOWSHELL_DROP_MAX_OFFSET && dropBytes[offset] && guard++ < WOWSHELL_DROP_MAX_FILES) {       /* walk to entry idx */
                WORD length = 0;
                while (length < MAX_PATH && dropBytes[offset + length]) ++length;
                if (itemIndex != WOWSHELL_DRAGQUERYFILE_COUNT && count == itemIndex) {
                    if (!output) result = length;
                    else if (bufferSize) {
                        WORD copied = (WORD)(length < bufferSize ? length : bufferSize - 1), cursor;
                        for (cursor = 0; cursor < copied; ++cursor) output[cursor] = dropBytes[offset + cursor];
                        output[copied] = 0;
                        result = copied;
                        wu_puts(note, noteCapacity, &noteLength, " -> \"");
                        {   CHAR name[WOWSHELL_LOG_NAME_MAX]; WORD nameIndex;
                            for (nameIndex = 0; nameIndex < copied && nameIndex < WOWSHELL_LOG_NAME_MAX - 1; ++nameIndex) name[nameIndex] = (CHAR)output[nameIndex];
                            name[nameIndex] = 0; wu_puts(note, noteCapacity, &noteLength, name); }
                        wu_puts(note, noteCapacity, &noteLength, "\"");
                    }
                    break;
                }
                ++count;
                offset = (WORD)(offset + length + 1);
            }
            if (itemIndex == WOWSHELL_DRAGQUERYFILE_COUNT) result = count;
        }
        g_ww_global16(WOWWIN_GLOBAL16_UNLOCK, drop16, 0);
        wu_puts(note, noteCapacity, &noteLength, " = 0x");
        wu_puthex(note, noteCapacity, &noteLength, result, WOW_HEX_WORD_DIGITS);
        wow32_setret(frame, result);
        return 1;
    }

    /* ── ★★★ 0x01 RegOpenKey / 0x02 RegCreateKey(hKey, lpSubKey, phkResult) ──
         Win32 still has `RegOpenKeyA`/`RegCreateKeyA` with exactly these
         semantics -- they are the same legacy calls Win16 had -- so the mapping
         is the identity once the key handle and the two far pointers are
         translated. What is NOT the identity is the root: see the header for why
         HKEY_CLASSES_ROOT lands in a private hive.
       ⚠ A NULL lpSubKey MEANS "DUPLICATE THIS KEY", so it is passed through as
         NULL rather than as "". RegOpenKeyA("") happens to work; RegCreateKeyA
         with an empty name does not mean the same thing everywhere, and the
         guest's intent is recoverable here and nowhere later.
       ⚠ phkResult IS WRITTEN OR THE CALL FAILS. The whole "Failed to register
         server" chain began with this hole being left unwritten: the guest read
         stack litter as a key handle and every call after it operated on
         nonsense. On any failure the slot is set to 0 as well as an error
         returned, so a guest that ignores the return code still gets a handle
         that fails honestly. */
    case WOWSHELL_REGOPENKEY:
    case WOWSHELL_REGCREATEKEY: {
        INT   isCreate = (frame->id == WOWSHELL_REGCREATEKEY);
        DWORD key16 = wow32_argd(frame, WOWSHELL_REGOPENKEY_ARG_HKEY);
        CHAR  subkeyBuffer[WOWSHELL_SUBKEY_MAX];
        PCSTR subkey = WowShellSubkey(frame, WOWSHELL_REGOPENKEY_ARG_SUBKEY, subkeyBuffer, sizeof subkeyBuffer);
        HKEY  parent = WowShellKey32(key16), output = NULL;
        DWORD token;
        LONG  result;
        INT   noteLength = 0;
        WowShellNoteKey(note, noteCapacity, &noteLength,
                          isCreate ? "RegCreateKey" : "RegOpenKey", key16, subkey);
        if (!parent) {
            wu_puts(note, noteCapacity, &noteLength, " -- ★ NOT A KEY THIS HOST ISSUED;"
                                       " ERROR_BADKEY");
            WowShellPutDword(frame, WOWSHELL_REGOPENKEY_ARG_RESULT, 0);
            wow32_setret(frame, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        result = isCreate ? RegCreateKeyA(parent, subkey, &output)
                    : RegOpenKeyA(parent, subkey, &output);
        if (result != ERROR_SUCCESS || !output) {
            wu_puts(note, noteCapacity, &noteLength, " -- the registry refused it, rc=0x");
            wu_puthex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_DWORD_DIGITS);
            WowShellPutDword(frame, WOWSHELL_REGOPENKEY_ARG_RESULT, 0);
            wow32_setret(frame, isCreate ? WOWSHELL_ERR_CANTWRITE
                                   : WOWSHELL_ERR_CANTOPEN);
            return 1;
        }
        token = WowShellKey16(output);
        if (!token) {
            RegCloseKey(output);
            wu_puts(note, noteCapacity, &noteLength, " -- ★ THE KEY TABLE IS FULL; the key was"
                                       " closed again and ERROR_OUTOFMEMORY"
                                       " answered");
            WowShellPutDword(frame, WOWSHELL_REGOPENKEY_ARG_RESULT, 0);
            wow32_setret(frame, WOWSHELL_ERR_OUTOFMEMORY);
            return 1;
        }
        wu_puts(note, noteCapacity, &noteLength, " -> key token 0x");
        wu_puthex(note, noteCapacity, &noteLength, token, WOW_HEX_DWORD_DIGITS);
        if (!WowShellPutDword(frame, WOWSHELL_REGOPENKEY_ARG_RESULT, token))
            wu_puts(note, noteCapacity, &noteLength, " -- ⚠ BUT phkResult WAS NOT WRITABLE");
        wow32_setret(frame, 0);
        return 1;
    }

    /* ── ★ 0x03 RegCloseKey(hKey) ────────────────────────────────────────────
       ⚠ CLOSING A ROOT IS A NO-OP, NOT AN ERROR. Guests close HKEY_CLASSES_ROOT
         routinely; closing our cached hive handle would leave every later call
         holding a dead HKEY. */
    case WOWSHELL_REGCLOSEKEY: {
        DWORD key16 = wow32_argd(frame, WOWSHELL_REGCLOSEKEY_ARG_HKEY);
        INT   noteLength = 0;
        wu_puts(note, noteCapacity, &noteLength, "RegCloseKey 0x");
        wu_puthex(note, noteCapacity, &noteLength, key16, WOW_HEX_DWORD_DIGITS);
        if (key16 == WOWSHELL_HKCR16 || key16 == WOWSHELL_HKCR32) {
            wu_puts(note, noteCapacity, &noteLength, " (HKEY_CLASSES_ROOT -- kept open)");
            wow32_setret(frame, 0);
            return 1;
        }
        if (key16 >= WOWSHELL_KEYTOK_BASE
            && key16 - WOWSHELL_KEYTOK_BASE < (DWORD)g_WowShellKeyCount) {
            DWORD index = key16 - WOWSHELL_KEYTOK_BASE;
            if (g_WowShellKeys[index]) {
                RegCloseKey(g_WowShellKeys[index]);
                g_WowShellKeys[index] = NULL;               /* the slot becomes reusable */
                wu_puts(note, noteCapacity, &noteLength, " -> closed, token freed");
                wow32_setret(frame, 0);
                return 1;
            }
        }
        wu_puts(note, noteCapacity, &noteLength, " -- ★ NOT AN OPEN KEY OF OURS; ERROR_BADKEY");
        wow32_setret(frame, WOWSHELL_ERR_BADKEY);
        return 1;
    }

    /* ── ★ 0x04 RegDeleteKey(hKey, lpSubKey) ────────────────────────────────
       ⚠ Win32's RegDeleteKeyA will not delete a key that still has subkeys, and
         Win16's would. That difference is REPORTED rather than worked around by
         recursing: a guest deleting a populated key is doing something this host
         has never seen one do, and inventing a recursive delete against the real
         registry on a guess is not a thing to do quietly. */
    case WOWSHELL_REGDELETEKEY: {
        DWORD key16 = wow32_argd(frame, WOWSHELL_REGDELETEKEY_ARG_HKEY);
        CHAR  subkeyBuffer[WOWSHELL_SUBKEY_MAX];
        PCSTR subkey = WowShellSubkey(frame, WOWSHELL_REGDELETEKEY_ARG_SUBKEY, subkeyBuffer, sizeof subkeyBuffer);
        HKEY  parent = WowShellKey32(key16);
        LONG  result;
        INT   noteLength = 0;
        WowShellNoteKey(note, noteCapacity, &noteLength, "RegDeleteKey", key16, subkey);
        if (!parent || !subkey) {
            wu_puts(note, noteCapacity, &noteLength, !parent
                        ? " -- ★ NOT A KEY THIS HOST ISSUED; ERROR_BADKEY"
                        : " -- ★ NO SUBKEY NAMED; ERROR_BADKEY");
            wow32_setret(frame, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        result = RegDeleteKeyA(parent, subkey);
        if (result != ERROR_SUCCESS) {
            wu_puts(note, noteCapacity, &noteLength, " -- refused, rc=0x");
            wu_puthex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_DWORD_DIGITS);
            wu_puts(note, noteCapacity, &noteLength, " (⚠ Win32 will not delete a key that"
                                       " still has subkeys; Win16 would)");
            wow32_setret(frame, WOWSHELL_ERR_CANTWRITE);
            return 1;
        }
        wu_puts(note, noteCapacity, &noteLength, " -> deleted");
        wow32_setret(frame, 0);
        return 1;
    }

    /* ── ★★★ 0x05 RegSetValue(hKey, lpSubKey, dwType, lpData, cbData) ────────
         Paint's registration is six of these. `RegSetValueA` is the same call on
         Win32, including the part that matters: with a subkey name it creates
         that subkey and sets ITS default value, which is how the whole
         `PBrush\protocol\StdFileEditing\server` tree gets built out of flat
         calls.
       ⚠ ONLY REG_SZ EXISTS HERE. Win16's RegSetValue accepted no other type --
         the parameter is there and is documented as "must be REG_SZ" -- so
         anything else is refused rather than passed on to a Win32 call that
         would take it and store something the guest can never read back through
         RegQueryValue.
       ⚠ cbData IS IGNORED BY BOTH, deliberately: the Win16 caller is entitled to
         pass 0 (Paint does, on every one of its six calls) and the string's
         length comes from its NUL. */
    case WOWSHELL_REGSETVALUE: {
        DWORD key16 = wow32_argd(frame, WOWSHELL_REGSETVALUE_ARG_HKEY);
        DWORD type = wow32_argd(frame, WOWSHELL_REGSETVALUE_ARG_TYPE);
        CHAR  subkeyBuffer[WOWSHELL_SUBKEY_MAX], dataBuffer[WOWSHELL_VALUE_MAX];
        PCSTR subkey = WowShellSubkey(frame, WOWSHELL_REGSETVALUE_ARG_SUBKEY, subkeyBuffer, sizeof subkeyBuffer);
        HKEY  parent = WowShellKey32(key16);
        LONG  result;
        INT   noteLength = 0;
        wow32_argstr(frame, WOWSHELL_REGSETVALUE_ARG_DATA, dataBuffer, sizeof dataBuffer);
        WowShellNoteKey(note, noteCapacity, &noteLength, "RegSetValue", key16, subkey);
        wu_puts(note, noteCapacity, &noteLength, " = ");
        wu_putq(note, noteCapacity, &noteLength, dataBuffer);
        if (!parent) {
            wu_puts(note, noteCapacity, &noteLength, " -- ★ NOT A KEY THIS HOST ISSUED;"
                                       " ERROR_BADKEY");
            wow32_setret(frame, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        if (type != REG_SZ) {
            wu_puts(note, noteCapacity, &noteLength, " -- ★ TYPE IS NOT REG_SZ (0x");
            wu_puthex(note, noteCapacity, &noteLength, type, WOW_HEX_DWORD_DIGITS);
            wu_puts(note, noteCapacity, &noteLength, "); Win16 RegSetValue has no other type,"
                                       " so this is refused rather than stored"
                                       " unreadably");
            wow32_setret(frame, WOWSHELL_ERR_INVALID);
            return 1;
        }
        result = RegSetValueA(parent, subkey, REG_SZ, dataBuffer, 0);
        if (result != ERROR_SUCCESS) {
            wu_puts(note, noteCapacity, &noteLength, " -- the registry refused it, rc=0x");
            wu_puthex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_DWORD_DIGITS);
            wow32_setret(frame, WOWSHELL_ERR_CANTWRITE);
            return 1;
        }
        wu_puts(note, noteCapacity, &noteLength, " -> stored");
        wow32_setret(frame, 0);
        return 1;
    }

    /* ── ★★★ 0x06 RegQueryValue(hKey, lpSubKey, lpValue, lpcbValue) ──────────
         The call Paint's verdict actually turns on: it registers itself and then
         reads `PBrush\protocol\StdFileEditing\server` back, and "Failed to
         register server" is what it says when that read does not return what it
         wrote.
       ⚠⚠ lpcbValue IS IN/OUT AND IT IS A **LONG**, NOT A WORD. On the way in it
         is the guest's own claim about the size of a buffer that is usually in
         its stack frame; on the way out it is the length stored. Writing more
         than it declared does not corrupt data, it corrupts the caller's return
         address -- so the capacity is honoured exactly, and a buffer too small
         is an error rather than a truncation, which is what Win32 does too.
       ⚠ A MISSING VALUE IS ERROR_BADKEY, NOT A CRASH AND NOT AN EMPTY STRING.
         Paint's FIRST call is a lookup of a key it has not created yet, and it
         is supposed to fail -- that failure is what makes it register. */
    case WOWSHELL_REGQUERYVALUE: {
        DWORD key16 = wow32_argd(frame, WOWSHELL_REGQUERYVALUE_ARG_HKEY);
        CHAR  subkeyBuffer[WOWSHELL_SUBKEY_MAX], valueBuffer[WOWSHELL_VALUE_MAX];
        PCSTR subkey = WowShellSubkey(frame, WOWSHELL_REGQUERYVALUE_ARG_SUBKEY, subkeyBuffer, sizeof subkeyBuffer);
        HKEY  parent = WowShellKey32(key16);
        volatile BYTE *byteCountPointer = wow32_argptr(frame, WOWSHELL_REGQUERYVALUE_ARG_CBVALUE);
        volatile BYTE *destination = wow32_argptr(frame, WOWSHELL_REGQUERYVALUE_ARG_VALUE);
        LONG  capacity = 0, byteCount = (LONG)sizeof valueBuffer;
        LONG  result;
        INT   noteLength = 0, index, cursor;
        WowShellNoteKey(note, noteCapacity, &noteLength, "RegQueryValue", key16, subkey);
        if (!parent) {
            wu_puts(note, noteCapacity, &noteLength, " -- ★ NOT A KEY THIS HOST ISSUED;"
                                       " ERROR_BADKEY");
            wow32_setret(frame, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        if (!byteCountPointer || !destination) {
            wu_puts(note, noteCapacity, &noteLength, " -- ★ NO BUFFER (lpValue or lpcbValue is"
                                       " a null far pointer); ERROR_INVALID");
            wow32_setret(frame, WOWSHELL_ERR_INVALID);
            return 1;
        }
        capacity = (LONG)((DWORD)wow32_peekw(byteCountPointer) | ((DWORD)wow32_peekw(byteCountPointer + WOW_WORD_BYTES) << WOW_WORD_SHIFT));
        result = RegQueryValueA(parent, subkey, valueBuffer, &byteCount);
        if (result != ERROR_SUCCESS) {
            wu_puts(note, noteCapacity, &noteLength, " -- ★ NOT PRESENT (rc=0x");
            wu_puthex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_DWORD_DIGITS);
            wu_puts(note, noteCapacity, &noteLength, "); ERROR_BADKEY -- which for a guest's"
                                       " FIRST lookup is the correct answer and"
                                       " is what makes it register");
            wow32_setret(frame, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        valueBuffer[sizeof valueBuffer - 1] = 0;
        for (index = 0; valueBuffer[index]; ++index) { }                    /* length, no CRT here */
        wu_puts(note, noteCapacity, &noteLength, " -> ");
        wu_putq(note, noteCapacity, &noteLength, valueBuffer);
        if (capacity <= index) {
            wu_puts(note, noteCapacity, &noteLength, " -- ★ BUT THE GUEST'S BUFFER IS 0x");
            wu_puthex(note, noteCapacity, &noteLength, (DWORD)capacity, WOW_HEX_WORD_DIGITS);
            wu_puts(note, noteCapacity, &noteLength, " BYTES AND THAT NEEDS MORE; nothing was"
                                       " written (ERROR_CANTREAD)");
            WowShellPutDword(frame, WOWSHELL_REGQUERYVALUE_ARG_CBVALUE, (DWORD)(index + 1));
            wow32_setret(frame, WOWSHELL_ERR_CANTREAD);
            return 1;
        }
        for (cursor = 0; cursor <= index; ++cursor) destination[cursor] = (BYTE)valueBuffer[cursor];   /* the NUL travels too */
        WowShellPutDword(frame, WOWSHELL_REGQUERYVALUE_ARG_CBVALUE, (DWORD)index);
        wow32_setret(frame, 0);
        return 1;
    }

    /* ── ★ 0x07 RegEnumKey(hKey, iSubkey, lpszBuffer, cbBuffer) ─────────────
         Not in Paint's list, but it is how a Win16 OLE CLIENT walks the database
         to find out what servers exist -- the other half of what Paint is
         registering itself into -- and it is four lines given the rest.
       ⚠ cbBuffer HERE IS A PLAIN VALUE, not a pointer, so the guest gets no
         length back; the buffer is NUL-terminated within its declared size and
         an over-long name is an error, as it is on Win32. */
    case WOWSHELL_REGENUMKEY: {
        DWORD key16 = wow32_argd(frame, WOWSHELL_REGENUMKEY_ARG_HKEY);
        DWORD itemIndex  = wow32_argd(frame, WOWSHELL_REGENUMKEY_ARG_INDEX);
        DWORD capacity  = wow32_argd(frame, WOWSHELL_REGENUMKEY_ARG_CBBUF);
        HKEY  parent = WowShellKey32(key16);
        volatile BYTE *destination = wow32_argptr(frame, WOWSHELL_REGENUMKEY_ARG_BUF);
        CHAR  nameBuffer[WOWSHELL_SUBKEY_MAX];
        LONG  result;
        INT   noteLength = 0, index, cursor;
        wu_puts(note, noteCapacity, &noteLength, "RegEnumKey key=0x");
        wu_puthex(note, noteCapacity, &noteLength, key16, WOW_HEX_DWORD_DIGITS);
        wu_puts(note, noteCapacity, &noteLength, " index=0x");
        wu_puthex(note, noteCapacity, &noteLength, itemIndex, WOW_HEX_WORD_DIGITS);
        if (!parent || !destination || !capacity) {
            wu_puts(note, noteCapacity, &noteLength, " -- ★ no key or no buffer; ERROR_BADKEY");
            wow32_setret(frame, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        result = RegEnumKeyA(parent, itemIndex, nameBuffer, (DWORD)sizeof nameBuffer);
        if (result != ERROR_SUCCESS) {
            wu_puts(note, noteCapacity, &noteLength, " -- no such subkey (the end of the"
                                       " enumeration); ERROR_BADKEY");
            wow32_setret(frame, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        nameBuffer[sizeof nameBuffer - 1] = 0;
        for (index = 0; nameBuffer[index]; ++index) { }
        if ((DWORD)index + 1 > capacity) {
            wu_puts(note, noteCapacity, &noteLength, " -- ★ the name does not fit the guest's"
                                       " buffer; nothing written"
                                       " (ERROR_CANTREAD)");
            wow32_setret(frame, WOWSHELL_ERR_CANTREAD);
            return 1;
        }
        wu_puts(note, noteCapacity, &noteLength, " -> ");
        wu_putq(note, noteCapacity, &noteLength, nameBuffer);
        for (cursor = 0; cursor <= index; ++cursor) destination[cursor] = (BYTE)nameBuffer[cursor];   /* the NUL travels too */
        wow32_setret(frame, 0);
        return 1;
    }

    default:
        return 0;
    }
}

#endif /* NTVDMEX_WOWSHELL_H */
