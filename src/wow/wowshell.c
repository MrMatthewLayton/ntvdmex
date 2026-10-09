/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * SHELL.DLL's OWN ID SPACE. GH #128, session 44.
 *
 * The code of wowshell.h (#335): its functions and state, in their original order;
 * its own translation unit, declared in wowshell.h.
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
#include "wowshell.h"
#include "host_dpmi.h"

static HKEY  g_WowShellKeys[WOWSHELL_KEYTOK_MAX];
static INT   g_WowShellKeyCount = 0;
static HKEY  g_WowShellRoot = NULL;

/* The private hive's root, created on first use. NULL means the registry itself
 * refused, which is reported to the guest rather than papered over.
 */
static HKEY WowShellRoot(VOID)
{
    HKEY key;
    DWORD disposition = 0;

    if (g_WowShellRoot)
        return g_WowShellRoot;
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

    if (key16 == WOWSHELL_HKCR16 || key16 == WOWSHELL_HKCR32)
        return WowShellRoot();
    if (key16 < WOWSHELL_KEYTOK_BASE)
        return NULL;
    index = key16 - WOWSHELL_KEYTOK_BASE;
    if (index >= (DWORD)g_WowShellKeyCount)
        return NULL;
    return g_WowShellKeys[index];
}

/* Mint a token for a key we just opened. 0 = the table is full. */
static DWORD WowShellKey16(HKEY key)
{
    INT index;

    if (!key)
        return 0;
    for (index = 0; index < g_WowShellKeyCount; ++index)
        if (!g_WowShellKeys[index])
            break;                                               /* reuse a closed slot */
    if (index == g_WowShellKeyCount)
    {
        if (g_WowShellKeyCount >= WOWSHELL_KEYTOK_MAX)
            return 0;
        index = g_WowShellKeyCount++;
    }
    g_WowShellKeys[index] = key;
    return WOWSHELL_KEYTOK_BASE + (DWORD)index;
}

/* Write a DWORD through a 16:16 far-pointer ARGUMENT (Wow32FarPut writes
 * through a pointer inside a STRUCT, which is a different thing).
 */
static INT WowShellPutDword(PCWOW32_FRAME frame, INT argumentOffset, DWORD value)
{
    volatile BYTE *bytes = Wow32ArgPointer(frame, argumentOffset);

    if (!bytes)
        return 0;
    Wow32PokeWord(bytes,     (WORD)(value & WORD_MASK));
    Wow32PokeWord(bytes + WOW_WORD_BYTES, (WORD)(value >> WORD_SHIFT));
    return 1;
}

/* The subkey argument, or NULL -- and the difference is load-bearing: every one
 * of these calls gives a null lpSubKey the meaning "the key itself".
 */
static PCSTR WowShellSubkey(PCWOW32_FRAME frame, INT argumentOffset, PSTR buffer, INT capacity)
{
    return Wow32ArgString(frame, argumentOffset, buffer, capacity) && buffer[0] ? buffer : NULL;
}

/* Shared reporting for the four calls that take (hKey, lpSubKey). */
static VOID WowShellNoteKey(
    PSTR note,
    INT noteCapacity,
    PINT noteLength,
    PCSTR name,
    DWORD key16,
    PCSTR subkey)
{
    WowNotePut(note, noteCapacity, noteLength, name);
    WowNotePut(note, noteCapacity, noteLength, " key=0x");
    WowNoteHex(note, noteCapacity, noteLength, key16, WOW_HEX_DWORD_DIGITS);
    if (key16 == WOWSHELL_HKCR16 || key16 == WOWSHELL_HKCR32)
        WowNotePut(note, noteCapacity, noteLength, "(HKEY_CLASSES_ROOT)");
    WowNotePut(note, noteCapacity, noteLength, " sub=");
    WowNoteQuoted(note, noteCapacity, noteLength, subkey ? subkey : "(the key itself)");
}

/* [CAUTION]: CALLED ONLY WHEN THE STUB IS SHELL'S. The caller checks, exactly as it does
 * for USER; this file must never be reachable from another module's numbering.
 * `note` receives a short description for the caller's log line.
 */
INT WowShellCall(PWOW32_FRAME frame, PSTR note, INT noteCapacity)
{
    if (noteCapacity)
        note[0] = 0;
    switch (frame->Id)
    {

    /* 0x16 ShellAbout(hWnd, szApp, szOtherStuff, hIcon) (Importance = 5):
     *
     * [INFO]: THE REAL Win32 ONE IS THE RIGHT ANSWER, and for once that is not a
     * shortcut -- it is what WOW itself does. `SHELL.DLL` is a thunk whose
     * whole job is to reach the 32-bit shell, `ShellAboutA` in SHELL32 takes
     * the same four parameters with the same meanings (including the `#`
     * convention that splits szApp into a caption and a body), and the owner
     * window we hand it is the guest's REAL window. Drawing our own About box
     * would be inventing chrome, which is the answer this project threw away
     * in session 42.
     *
     * [CAUTION]: IT IS MODAL, AND ON THIS THREAD. ShellAboutA runs its own message loop
     * and does not return until the box is dismissed, and it is called from the
     * exec thread -- so the whole VDM is stopped for as long as the box is up.
     * That is right for the CALLING task (a Win16 ShellAbout blocks it too) and
     * wrong for every other one: real WOW gives each task a thread and the
     * others keep running. Stated rather than discovered.
     *
     * [INFO]: The dialog's own loop dispatches this thread's messages, so the guest's
     * other windows keep painting and moving behind it. Nothing re-enters the
     * guest, because the only thread that runs guest code is the one sitting
     * inside this call.
     *
     * [CAUTION]: hIcon IS A TOKEN, NOT AN ICON -- the guest got it from USER 0xad, which
     * cannot know a cursor from an icon at the moment it is asked. Resolving it
     * HERE is the point of use, where the guest has just said which it is by
     * passing it as an icon. A NULL result is not a failure: ShellAbout with no
     * icon shows the system's, which is what Windows does for a program with no
     * icon of its own, and the note says so rather than leaving a silent blank.
     *
     * [CAUTION]: AN OWNER WE DO NOT KNOW IS REPORTED, NOT REFUSED. The About box is still
     * the correct answer to the call; what changes is that it comes up unowned,
     * and a run that shows that line has found a window handle this host issued
     * and then lost -- which is worth seeing.
     */
    /* 0x14 ShellExecute -- see the note by the ids (Importance = 2):
     *
     * [CAUTION]: A NULL lpOperation MEANS "open", and passing our empty buffer straight
     * through would ask the shell to perform the verb "" -- which is not the
     * same thing and fails. The distinction between "no string" and "an empty
     * string" is exactly what Wow32ArgString's return value is for.
     */
    case WOWSHELL_FINDENVSTRING:
    case WOWSHELL_INTERNALEXTRACTICON:
    {
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, frame->Id == WOWSHELL_FINDENVSTRING
                ? "FindEnvironmentString -- NULL, as stock (XP's thunk has no arguments)"
                : "InternalExtractIcon -- 0, as stock (XP's thunk has no arguments)");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    case WOWSHELL_EXTRACTICON:
    {
        WORD instance = Wow32ArgWord(frame, WOWSHELL_EXTRACTICON_ARG_HINST);
        INT  itemIndex   = (INT)(SHORT)Wow32ArgWord(frame, WOWSHELL_EXTRACTICON_ARG_INDEX);
        CHAR fileName[MAX_PATH];
        INT  noteLength = 0;
        HICON icon;
        WORD token;
        (VOID)instance;
        WowNotePut(note, noteCapacity, &noteLength, "ExtractIcon ");
        if (!Wow32ArgString(frame, WOWSHELL_EXTRACTICON_ARG_FILE, fileName, sizeof fileName) || !fileName[0])
        {
            WowNotePut(note, noteCapacity, &noteLength, "-- ★ no file name; answered 0 (no such"
                                       " file), which is the documented answer");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNoteQuoted(note, noteCapacity, &noteLength, fileName);
        WowNotePut(note, noteCapacity, &noteLength, " index ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)itemIndex, WOW_HEX_WORD_DIGITS);
        if (itemIndex == -1)
        {
            /* A COUNT QUERY. Win32 answers it the same way and it mints
             * nothing -- the value is a number of icons, not a handle.
             */
            UINT count = (UINT)(ULONG_PTR)ExtractIconA(GetModuleHandleA(NULL), fileName,
                                                   (UINT)-1);
            WowNotePut(note, noteCapacity, &noteLength, " -- a COUNT query -> ");
            WowNoteHex(note, noteCapacity, &noteLength, count, WOW_HEX_WORD_DIGITS);
            Wow32SetReturn(frame, (DWORD)(count & WORD_MASK));
            return 1;
        }
        icon = ExtractIconA(GetModuleHandleA(NULL), fileName, (UINT)itemIndex);
        if ((ULONG_PTR)icon == 1)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- the file has NO icons (1), which is"
                                       " the documented in-band answer");
            Wow32SetReturn(frame, 1);
            return 1;
        }
        if (!icon)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- no such file or no such index -> 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        token = WowUserSystemResourceMintIcon(icon);
        WowNotePut(note, noteCapacity, &noteLength, " -> token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        if (!token) WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE TOKEN TABLE IS FULL, so"
                                             " the icon exists and the guest"
                                             " cannot be given it");
        Wow32SetReturn(frame, token);
        return 1;
    }

    case WOWSHELL_EXTRACTASSOCIATEDICON:
    {
        volatile BYTE *indexPointer = Wow32ArgPointer(frame, WOWSHELL_EXTRACTASSOCIATEDICON_ARG_LPIICON);
        volatile BYTE *pathPointer = Wow32ArgPointer(frame, WOWSHELL_EXTRACTASSOCIATEDICON_ARG_PATH);
        CHAR path[MAX_PATH];
        INT noteLength = 0;
        INT index;
        WORD itemIndex = 0;
        WORD token;
        HICON icon;
        WowNotePut(note, noteCapacity, &noteLength, "ExtractAssociatedIcon ");
        if (!Wow32ArgString(frame, WOWSHELL_EXTRACTASSOCIATEDICON_ARG_PATH, path, sizeof path) || !path[0])
        {
            WowNotePut(note, noteCapacity, &noteLength, "-- ★ no path; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (indexPointer)
            itemIndex = (WORD)(indexPointer[0] | (indexPointer[1] << BYTE_SHIFT));
        WowNoteQuoted(note, noteCapacity, &noteLength, path);
        WowNotePut(note, noteCapacity, &noteLength, " index ");
        WowNoteHex(note, noteCapacity, &noteLength, itemIndex, WOW_HEX_WORD_DIGITS);
        icon = ExtractAssociatedIconA(GetModuleHandleA(NULL), path, &itemIndex);
        if (!icon)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- nothing associated -> 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* [INFO]: BOTH OUT-PARAMETERS GO BACK, because the caller reads them: the path
         * is now the file the icon came from (which may be a different file
         * entirely) and the index is where in it.
         */
        if (pathPointer)
        {
            for (index = 0; index < MAX_PATH - 1 && path[index]; ++index)
                pathPointer[index] = (BYTE)path[index];
            pathPointer[index] = 0;
            if (index == MAX_PATH - 1)
                WowNotePut(note, noteCapacity, &noteLength, " [★ path TRUNCATED at MAX_PATH]");
        }
        if (indexPointer)
        {
            indexPointer[0] = (BYTE)(itemIndex & BYTE_MASK);
            indexPointer[1] = (BYTE)(itemIndex >> BYTE_SHIFT);
        }
        token = WowUserSystemResourceMintIcon(icon);
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteQuoted(note, noteCapacity, &noteLength, path);
        WowNotePut(note, noteCapacity, &noteLength, " token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, token);
        return 1;
    }

    case WOWSHELL_REGISTERSHELLHOOK:
    {
        WORD window16 = Wow32ArgWord(frame, WOWSHELL_REGISTERSHELLHOOK_ARG_HWND);
        WORD action  = Wow32ArgWord(frame, WOWSHELL_REGISTERSHELLHOOK_ARG_ACTION);
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "RegisterShellHook(hwnd 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", action ");
        WowNoteHex(note, noteCapacity, &noteLength, action, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ") -> registered."
                                   " ★ AND NO SHELL HOOK MESSAGE WILL EVER BE"
                                   " POSTED, said here rather than discovered:"
                                   " this host runs one Win16 task at a time, so"
                                   " there are no other Win16 windows to report,"
                                   " and the Win32 desktop's windows have no"
                                   " 16-bit handles to report them WITH. The"
                                   " subscription is true; the feed is empty.");
        Wow32SetReturn(frame, 1);
        return 1;
    }

    case WOWSHELL_SHELLEXECUTE:
    {
        WORD window16 = Wow32ArgWord(frame, WOWSHELL_SHELLEXECUTE_ARG_HWND);
        WORD showCommand = Wow32ArgWord(frame, WOWSHELL_SHELLEXECUTE_ARG_SHOW);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        CHAR operation[WOWSHELL_OPERATION_MAX];
        CHAR fileName[MAX_PATH];
        CHAR parameters[MAX_PATH];
        CHAR directory[MAX_PATH];
        INT noteLength = 0;
        INT hasOperation;
        INT hasParameters;
        INT hasDirectory;
        DWORD result;
        hasOperation  = Wow32ArgString(frame, WOWSHELL_SHELLEXECUTE_ARG_OP,     operation,     sizeof operation);
        hasParameters = Wow32ArgString(frame, WOWSHELL_SHELLEXECUTE_ARG_PARAMS, parameters, sizeof parameters);
        hasDirectory = Wow32ArgString(frame, WOWSHELL_SHELLEXECUTE_ARG_DIR,    directory,    sizeof directory);
        if (!Wow32ArgString(frame, WOWSHELL_SHELLEXECUTE_ARG_FILE, fileName, sizeof fileName) || !fileName[0])
        {
            WowNotePut(note, noteCapacity, &noteLength, "ShellExecute -- ★ no lpFile; answered "
                                       "SE_ERR_FNF (2)");
            Wow32SetReturn(frame, WOWSHELL_SE_ERR_FNF);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, "ShellExecute ");
        WowNoteQuoted(note, noteCapacity, &noteLength, hasOperation && operation[0] ? operation : "(open)");
        WowNotePut(note, noteCapacity, &noteLength, " ");
        WowNoteQuoted(note, noteCapacity, &noteLength, fileName);
        if (hasParameters && parameters[0]) { WowNotePut(note, noteCapacity, &noteLength, " args ");
                                    WowNoteQuoted(note, noteCapacity, &noteLength, parameters); }
        if (hasDirectory && directory[0])    { WowNotePut(note, noteCapacity, &noteLength, " in ");
                                    WowNoteQuoted(note, noteCapacity, &noteLength, directory); }
        result = (DWORD)(ULONG_PTR)ShellExecuteA(window ? window->Window32 : NULL,
                                             (hasOperation && operation[0]) ? operation : NULL,
                                             fileName,
                                             (hasParameters && parameters[0]) ? parameters : NULL,
                                             (hasDirectory && directory[0]) ? directory : NULL,
                                             (INT)(SHORT)showCommand);
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteHex(note, noteCapacity, &noteLength, result, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, result > WOWSHELL_SE_ERR_LAST ? " (started)" : " (SE_ERR_*)");
        /* [CAUTION]: TRUNCATED TO 16 BITS DELIBERATELY: the guest's variable is an
         * HINSTANCE, which is a WORD here. A success value above 0xFFFF would
         * wrap to something <= 32 and read as an error, so clamp instead.
         */
        if (result > WORD_MASK)
            result = WORD_MASK;
        Wow32SetReturn(frame, result);
        return 1;
    }

    /* -- 0x15 FindExecutable(lpFile, lpDirectory, lpResult) -- which program
     * opens this document. Same >32 convention as ShellExecute.
     */
    case WOWSHELL_FINDEXECUTABLE:
    {
        CHAR fileName[MAX_PATH];
        CHAR directory[MAX_PATH];
        CHAR output[MAX_PATH];
        volatile BYTE *resultPointer = Wow32ArgPointer(frame, WOWSHELL_FINDEXECUTABLE_ARG_RESULT);
        INT noteLength = 0;
        INT hasDirectory;
        INT index;
        DWORD result;
        output[0] = 0;
        hasDirectory = Wow32ArgString(frame, WOWSHELL_FINDEXECUTABLE_ARG_DIR, directory, sizeof directory);
        if (!Wow32ArgString(frame, WOWSHELL_FINDEXECUTABLE_ARG_FILE, fileName, sizeof fileName) || !fileName[0] || !resultPointer)
        {
            WowNotePut(note, noteCapacity, &noteLength, "FindExecutable -- ★ no lpFile or no "
                                       "result buffer; answered SE_ERR_FNF (2)");
            Wow32SetReturn(frame, WOWSHELL_SE_ERR_FNF);
            return 1;
        }
        result = (DWORD)(ULONG_PTR)FindExecutableA(fileName,
                                               (hasDirectory && directory[0]) ? directory : NULL,
                                               output);
        WowNotePut(note, noteCapacity, &noteLength, "FindExecutable ");
        WowNoteQuoted(note, noteCapacity, &noteLength, fileName);
        if (result > WOWSHELL_SE_ERR_LAST)
        {
            for (index = 0; index < (INT)sizeof output && output[index]; ++index)
                resultPointer[index] = (BYTE)output[index];
            resultPointer[index] = 0;
            WowNotePut(note, noteCapacity, &noteLength, " -> ");
            WowNoteQuoted(note, noteCapacity, &noteLength, output);
        }
        else
        {
            resultPointer[0] = 0;
            WowNotePut(note, noteCapacity, &noteLength, " -> none (SE_ERR_ ");
            WowNoteHex(note, noteCapacity, &noteLength, result, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ")");
        }
        if (result > WORD_MASK)
            result = WORD_MASK;
        Wow32SetReturn(frame, result);
        return 1;
    }

    /* -- 0x25 DoEnvironmentSubst(lpszString, cbString) -- expand %VAR% IN
     * PLACE, inside the guest's own buffer.
     *
     * [CAUTION]: THE RETURN IS A PACKED PAIR, not a status: the HIGH word is the length
     * of the result and the LOW word is the size of the buffer. And the
     * expansion must NOT be written back unless it FITS -- the buffer is the
     * guest's and cbString is the only statement we have about its size.
     */
    case WOWSHELL_DOENVSUBST:
    {
        volatile BYTE *string = Wow32ArgPointer(frame, WOWSHELL_DOENVSUBST_ARG_STR);
        WORD byteCount = Wow32ArgWord(frame, WOWSHELL_DOENVSUBST_ARG_CB);
        CHAR input[WOWSHELL_ENV_BUFFER];
        CHAR output[WOWSHELL_ENV_BUFFER];
        INT noteLength = 0;
        INT index;
        INT count;
        DWORD expandedLength;
        if (!string || !byteCount)
        {
            WowNotePut(note, noteCapacity, &noteLength, "DoEnvironmentSubst -- ★ no buffer");
            Wow32SetReturn(frame, (DWORD)byteCount);
            return 1;
        }
        count = 0;
        while (count < (INT)sizeof input - 1 && count < (INT)byteCount && string[count])
        {
            input[count] = (CHAR)string[count];
            ++count;
        }
        input[count] = 0;
        expandedLength = ExpandEnvironmentStringsA(input, output, (DWORD)sizeof output);
        WowNotePut(note, noteCapacity, &noteLength, "DoEnvironmentSubst ");
        WowNoteQuoted(note, noteCapacity, &noteLength, input);
        if (expandedLength && expandedLength <= (DWORD)byteCount)
        {
            for (index = 0; index < (INT)expandedLength && output[index]; ++index)
                string[index] = (BYTE)output[index];
            string[index] = 0;
            WowNotePut(note, noteCapacity, &noteLength, " -> ");
            WowNoteQuoted(note, noteCapacity, &noteLength, output);
            Wow32SetReturn(frame, ((DWORD)(WORD)index << WORD_SHIFT) | (DWORD)byteCount);
        }
        else
        {
            /* Too long, or nothing to do: leave the guest's buffer alone and
             * report the original length. Truncating in place would hand the
             * program a path that silently is not the path it asked about.
             */
            WowNotePut(note, noteCapacity, &noteLength, expandedLength ? " -- ★ result does not fit; buffer"
                                             " left UNCHANGED"
                                           : " -- no substitution");
            Wow32SetReturn(frame, ((DWORD)(WORD)count << WORD_SHIFT) | (DWORD)byteCount);
        }
        return 1;
    }

    case WOWSHELL_SHELLABOUT:
    {
        WORD window16 = Wow32ArgWord(frame, WOWSHELL_SHELLABOUT_ARG_HWND);
        WORD iconToken = Wow32ArgWord(frame, WOWSHELL_SHELLABOUT_ARG_HICON);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        CHAR application[WOWSHELL_ABOUT_APP_MAX];
        CHAR otherText[WOWSHELL_ABOUT_OTHER_MAX];
        INT noteLength = 0;
        INT bitCount = 0;
        INT result;
        /* The About box wants the full-size icon, so the size is the system's
         * default -- the small-icon variant exists for the taskbar.
         */
        HICON icon = WowUserSystemResourceIcon(iconToken, &bitCount, 0, 0);
        HWND  owner = window ? window->Window32 : NULL;

        Wow32ArgString(frame, WOWSHELL_SHELLABOUT_ARG_APP,   application,   sizeof application);
        Wow32ArgString(frame, WOWSHELL_SHELLABOUT_ARG_OTHER, otherText, sizeof otherText);

        WowNotePut(note, noteCapacity, &noteLength, "ShellAbout ");
        WowNoteQuoted(note, noteCapacity, &noteLength, application);
        WowNotePut(note, noteCapacity, &noteLength, " / ");
        WowNoteQuoted(note, noteCapacity, &noteLength, otherText);
        WowNotePut(note, noteCapacity, &noteLength, " owner=0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window)          WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO SUCH WINDOW; the box"
                                                    " comes up UNOWNED");
        else if (!owner) WowNotePut(note, noteCapacity, &noteLength, " -- no real window behind it;"
                                                    " the box comes up UNOWNED");
        WowNotePut(note, noteCapacity, &noteLength, " icon=0x");
        WowNoteHex(note, noteCapacity, &noteLength, iconToken, WOW_HEX_WORD_DIGITS);
        if (icon)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -> the app's own (");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)bitCount, WOW_HEX_BYTE_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " bpp)");
        }
        else
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- NOT RESOLVED; the system icon is used");
        }
        WowNotePut(note, noteCapacity, &noteLength, " -- ★ MODAL: the VDM is stopped until it is"
                                   " dismissed");
        result = ShellAboutA(owner, application, otherText, icon);
        WowNotePut(note, noteCapacity, &noteLength, "; dismissed, rc=0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    /* 0x09 DragAcceptFiles(hWnd, fAccept) (Importance = 1):
     * The real one, on the real window: accepting drops is a property the
     * window manager enforces, and ours is the OS's. A guest that asks for it
     * and then gets no WM_DROPFILES would be a lie one level down.
     *
     * [INFO]: s92: AND THE DROP NOW ARRIVES -- WM_DROPFILES is relayed with a Win16 HDROP
     * (a real global block; wowwin.h WowWinDrop16), read by DragQueryFile below
     * and by SHELL.DLL's own 16-bit DragQueryPoint and DragFinish.
     */
    case WOWSHELL_DRAGACCEPTFILES:
    {
        WORD window16 = Wow32ArgWord(frame, WOWSHELL_DRAGACCEPTFILES_ARG_HWND);
        WORD isAccept  = Wow32ArgWord(frame, WOWSHELL_DRAGACCEPTFILES_ARG_ACCEPT);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, isAccept ? "DragAcceptFiles ACCEPT 0x"
                                       : "DragAcceptFiles REFUSE 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        DragAcceptFiles(window->Window32, isAccept ? TRUE : FALSE);
        WowNotePut(note, noteCapacity, &noteLength, " -> the OS's (drops arrive as WM_DROPFILES, s92)");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* 0x0b DragQueryFile(hDrop, iFile, lpszFile, cch) (Importance = 1):
     * s92 (#305 M12): the HDROP is the global block WowWinDrop16 built (wowwin.h),
     * read here the way SHELL.DLL's own DragQueryPoint reads it: locked through
     * krnl386, pFiles at +0, the names from there. iFile 0xFFFF answers the count;
     * a NULL buffer answers the length a name needs (without its NUL); otherwise at
     * most cch-1 characters and a NUL are copied and the count copied is answered.
     *
     * [CAUTION]: A handle that does not lock, or a block that does not parse, answers 0 --
     * the honest "no files" this call always gave before drops were delivered.
     */
    case WOWSHELL_DRAGQUERYFILE:
    {
        WORD drop16 = Wow32ArgWord(frame, WOWSHELL_DRAGQUERYFILE_ARG_HDROP);
        WORD itemIndex   = Wow32ArgWord(frame, WOWSHELL_DRAGQUERYFILE_ARG_INDEX);
        WORD bufferSize   = Wow32ArgWord(frame, WOWSHELL_DRAGQUERYFILE_ARG_CCH);
        volatile BYTE *output = Wow32ArgPointer(frame, WOWSHELL_DRAGQUERYFILE_ARG_BUF);
        DWORD farPointer = g_WowWinGlobal16 ? g_WowWinGlobal16(WOWWIN_GLOBAL16_LOCK, drop16, 0) : 0;
        DWORD segmentBase = (farPointer >> WORD_SHIFT) ? DpmiSelectorBase((WORD)(farPointer >> WORD_SHIFT)) : 0;
        volatile BYTE *dropBytes = segmentBase ? (volatile BYTE *)(ULONG_PTR)(segmentBase + (farPointer & WORD_MASK)) : NULL;
        DWORD result = 0;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "DragQueryFile drop 0x");
        WowNoteHex(note, noteCapacity, &noteLength, drop16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " index 0x");
        WowNoteHex(note, noteCapacity, &noteLength, itemIndex, WOW_HEX_WORD_DIGITS);
        if (!dropBytes)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ the handle does not lock; 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        {   WORD offset = (WORD)(dropBytes[0] | (dropBytes[1] << BYTE_SHIFT)), count = 0;
            INT  guard = 0;
            while (offset < WOWSHELL_DROP_MAX_OFFSET && dropBytes[offset] && guard++ < WOWSHELL_DROP_MAX_FILES)         /* walk to entry idx */
            {
                WORD length = 0;
                while (length < MAX_PATH && dropBytes[offset + length])
                    ++length;
                if (itemIndex != WOWSHELL_DRAGQUERYFILE_COUNT && count == itemIndex)
                {
                    if (!output)
                        result = length;
                    else if (bufferSize)
                    {
                        WORD copied = (WORD)(length < bufferSize ? length : bufferSize - 1);
                        WORD cursor;
                        for (cursor = 0; cursor < copied; ++cursor)
                            output[cursor] = dropBytes[offset + cursor];
                        output[copied] = 0;
                        result = copied;
                        WowNotePut(note, noteCapacity, &noteLength, " -> \"");
                        {   CHAR name[WOWSHELL_LOG_NAME_MAX];
                        WORD nameIndex;
                            for (nameIndex = 0; nameIndex < copied && nameIndex < WOWSHELL_LOG_NAME_MAX - 1; ++nameIndex)
                                name[nameIndex] = (CHAR)output[nameIndex];
                            name[nameIndex] = 0;
                            WowNotePut(note, noteCapacity, &noteLength, name); }
                        WowNotePut(note, noteCapacity, &noteLength, "\"");
                    }
                    break;
                }
                ++count;
                offset = (WORD)(offset + length + 1);
            }
            if (itemIndex == WOWSHELL_DRAGQUERYFILE_COUNT)
                result = count;
        }
        g_WowWinGlobal16(WOWWIN_GLOBAL16_UNLOCK, drop16, 0);
        WowNotePut(note, noteCapacity, &noteLength, " = 0x");
        WowNoteHex(note, noteCapacity, &noteLength, result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, result);
        return 1;
    }

    /* 0x01 RegOpenKey / 0x02 RegCreateKey(hKey, lpSubKey, phkResult) (Importance = 3):
     * Win32 still has `RegOpenKeyA`/`RegCreateKeyA` with exactly these
     * semantics -- they are the same legacy calls Win16 had -- so the mapping
     * is the identity once the key handle and the two far pointers are
     * translated. What is NOT the identity is the root: see the header for why
     * HKEY_CLASSES_ROOT lands in a private hive.
     *
     * [CAUTION]: A NULL lpSubKey MEANS "DUPLICATE THIS KEY", so it is passed through as
     * NULL rather than as "". RegOpenKeyA("") happens to work; RegCreateKeyA
     * with an empty name does not mean the same thing everywhere, and the
     * guest's intent is recoverable here and nowhere later.
     *
     * [CAUTION]: phkResult IS WRITTEN OR THE CALL FAILS. The whole "Failed to register
     * server" chain began with this hole being left unwritten: the guest read
     * stack litter as a key handle and every call after it operated on
     * nonsense. On any failure the slot is set to 0 as well as an error
     * returned, so a guest that ignores the return code still gets a handle
     * that fails honestly.
     */
    case WOWSHELL_REGOPENKEY:
    case WOWSHELL_REGCREATEKEY:
    {
        INT   isCreate = (frame->Id == WOWSHELL_REGCREATEKEY);
        DWORD key16 = Wow32ArgDword(frame, WOWSHELL_REGOPENKEY_ARG_HKEY);
        CHAR  subkeyBuffer[WOWSHELL_SUBKEY_MAX];
        PCSTR subkey = WowShellSubkey(frame, WOWSHELL_REGOPENKEY_ARG_SUBKEY, subkeyBuffer, sizeof subkeyBuffer);
        HKEY parent = WowShellKey32(key16);
        HKEY output = NULL;
        DWORD token;
        LONG  result;
        INT   noteLength = 0;
        WowShellNoteKey(note, noteCapacity, &noteLength,
                          isCreate ? "RegCreateKey" : "RegOpenKey", key16, subkey);
        if (!parent)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT A KEY THIS HOST ISSUED;"
                                       " ERROR_BADKEY");
            WowShellPutDword(frame, WOWSHELL_REGOPENKEY_ARG_RESULT, 0);
            Wow32SetReturn(frame, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        result = isCreate ? RegCreateKeyA(parent, subkey, &output)
                    : RegOpenKeyA(parent, subkey, &output);
        if (result != ERROR_SUCCESS || !output)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- the registry refused it, rc=0x");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_DWORD_DIGITS);
            WowShellPutDword(frame, WOWSHELL_REGOPENKEY_ARG_RESULT, 0);
            Wow32SetReturn(frame, isCreate ? WOWSHELL_ERR_CANTWRITE
                                   : WOWSHELL_ERR_CANTOPEN);
            return 1;
        }
        token = WowShellKey16(output);
        if (!token)
        {
            RegCloseKey(output);
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE KEY TABLE IS FULL; the key was"
                                       " closed again and ERROR_OUTOFMEMORY"
                                       " answered");
            WowShellPutDword(frame, WOWSHELL_REGOPENKEY_ARG_RESULT, 0);
            Wow32SetReturn(frame, WOWSHELL_ERR_OUTOFMEMORY);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> key token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_DWORD_DIGITS);
        if (!WowShellPutDword(frame, WOWSHELL_REGOPENKEY_ARG_RESULT, token))
            WowNotePut(note, noteCapacity, &noteLength, " -- ⚠ BUT phkResult WAS NOT WRITABLE");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* 0x03 RegCloseKey(hKey) (Importance = 1):
     *
     * [CAUTION]: CLOSING A ROOT IS A NO-OP, NOT AN ERROR. Guests close HKEY_CLASSES_ROOT
     * routinely; closing our cached hive handle would leave every later call
     * holding a dead HKEY.
     */
    case WOWSHELL_REGCLOSEKEY:
    {
        DWORD key16 = Wow32ArgDword(frame, WOWSHELL_REGCLOSEKEY_ARG_HKEY);
        INT   noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "RegCloseKey 0x");
        WowNoteHex(note, noteCapacity, &noteLength, key16, WOW_HEX_DWORD_DIGITS);
        if (key16 == WOWSHELL_HKCR16 || key16 == WOWSHELL_HKCR32)
        {
            WowNotePut(note, noteCapacity, &noteLength, " (HKEY_CLASSES_ROOT -- kept open)");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (key16 >= WOWSHELL_KEYTOK_BASE
            && key16 - WOWSHELL_KEYTOK_BASE < (DWORD)g_WowShellKeyCount)
        {
            DWORD index = key16 - WOWSHELL_KEYTOK_BASE;
            if (g_WowShellKeys[index])
            {
                RegCloseKey(g_WowShellKeys[index]);
                g_WowShellKeys[index] = NULL;               /* the slot becomes reusable */
                WowNotePut(note, noteCapacity, &noteLength, " -> closed, token freed");
                Wow32SetReturn(frame, 0);
                return 1;
            }
        }
        WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT AN OPEN KEY OF OURS; ERROR_BADKEY");
        Wow32SetReturn(frame, WOWSHELL_ERR_BADKEY);
        return 1;
    }

    /* 0x04 RegDeleteKey(hKey, lpSubKey) (Importance = 1):
     *
     * [CAUTION]: Win32's RegDeleteKeyA will not delete a key that still has subkeys, and
     * Win16's would. That difference is REPORTED rather than worked around by
     * recursing: a guest deleting a populated key is doing something this host
     * has never seen one do, and inventing a recursive delete against the real
     * registry on a guess is not a thing to do quietly.
     */
    case WOWSHELL_REGDELETEKEY:
    {
        DWORD key16 = Wow32ArgDword(frame, WOWSHELL_REGDELETEKEY_ARG_HKEY);
        CHAR  subkeyBuffer[WOWSHELL_SUBKEY_MAX];
        PCSTR subkey = WowShellSubkey(frame, WOWSHELL_REGDELETEKEY_ARG_SUBKEY, subkeyBuffer, sizeof subkeyBuffer);
        HKEY  parent = WowShellKey32(key16);
        LONG  result;
        INT   noteLength = 0;
        WowShellNoteKey(note, noteCapacity, &noteLength, "RegDeleteKey", key16, subkey);
        if (!parent || !subkey)
        {
            WowNotePut(note, noteCapacity, &noteLength, !parent
                        ? " -- ★ NOT A KEY THIS HOST ISSUED; ERROR_BADKEY"
                        : " -- ★ NO SUBKEY NAMED; ERROR_BADKEY");
            Wow32SetReturn(frame, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        result = RegDeleteKeyA(parent, subkey);
        if (result != ERROR_SUCCESS)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- refused, rc=0x");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_DWORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " (⚠ Win32 will not delete a key that"
                                       " still has subkeys; Win16 would)");
            Wow32SetReturn(frame, WOWSHELL_ERR_CANTWRITE);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> deleted");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* 0x05 RegSetValue(hKey, lpSubKey, dwType, lpData, cbData) (Importance = 3):
     * Paint's registration is six of these. `RegSetValueA` is the same call on
     * Win32, including the part that matters: with a subkey name it creates
     * that subkey and sets ITS default value, which is how the whole
     * `PBrush\protocol\StdFileEditing\server` tree gets built out of flat
     * calls.
     *
     * [CAUTION]: ONLY REG_SZ EXISTS HERE. Win16's RegSetValue accepted no other type --
     * the parameter is there and is documented as "must be REG_SZ" -- so
     * anything else is refused rather than passed on to a Win32 call that
     * would take it and store something the guest can never read back through
     * RegQueryValue.
     *
     * [CAUTION]: cbData IS IGNORED BY BOTH, deliberately: the Win16 caller is entitled to
     * pass 0 (Paint does, on every one of its six calls) and the string's
     * length comes from its NUL.
     */
    case WOWSHELL_REGSETVALUE:
    {
        DWORD key16 = Wow32ArgDword(frame, WOWSHELL_REGSETVALUE_ARG_HKEY);
        DWORD type = Wow32ArgDword(frame, WOWSHELL_REGSETVALUE_ARG_TYPE);
        CHAR subkeyBuffer[WOWSHELL_SUBKEY_MAX];
        CHAR dataBuffer[WOWSHELL_VALUE_MAX];
        PCSTR subkey = WowShellSubkey(frame, WOWSHELL_REGSETVALUE_ARG_SUBKEY, subkeyBuffer, sizeof subkeyBuffer);
        HKEY  parent = WowShellKey32(key16);
        LONG  result;
        INT   noteLength = 0;
        Wow32ArgString(frame, WOWSHELL_REGSETVALUE_ARG_DATA, dataBuffer, sizeof dataBuffer);
        WowShellNoteKey(note, noteCapacity, &noteLength, "RegSetValue", key16, subkey);
        WowNotePut(note, noteCapacity, &noteLength, " = ");
        WowNoteQuoted(note, noteCapacity, &noteLength, dataBuffer);
        if (!parent)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT A KEY THIS HOST ISSUED;"
                                       " ERROR_BADKEY");
            Wow32SetReturn(frame, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        if (type != REG_SZ)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ TYPE IS NOT REG_SZ (0x");
            WowNoteHex(note, noteCapacity, &noteLength, type, WOW_HEX_DWORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, "); Win16 RegSetValue has no other type,"
                                       " so this is refused rather than stored"
                                       " unreadably");
            Wow32SetReturn(frame, WOWSHELL_ERR_INVALID);
            return 1;
        }
        result = RegSetValueA(parent, subkey, REG_SZ, dataBuffer, 0);
        if (result != ERROR_SUCCESS)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- the registry refused it, rc=0x");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_DWORD_DIGITS);
            Wow32SetReturn(frame, WOWSHELL_ERR_CANTWRITE);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> stored");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* 0x06 RegQueryValue(hKey, lpSubKey, lpValue, lpcbValue) (Importance = 3):
     * The call Paint's verdict actually turns on: it registers itself and then
     * reads `PBrush\protocol\StdFileEditing\server` back, and "Failed to
     * register server" is what it says when that read does not return what it
     * wrote.
     *
     * [CAUTION]: lpcbValue IS IN/OUT AND IT IS A **LONG**, NOT A WORD. On the way in it
     * is the guest's own claim about the size of a buffer that is usually in
     * its stack frame; on the way out it is the length stored. Writing more
     * than it declared does not corrupt data, it corrupts the caller's return
     * address -- so the capacity is honoured exactly, and a buffer too small
     * is an error rather than a truncation, which is what Win32 does too.
     *
     * [CAUTION]: A MISSING VALUE IS ERROR_BADKEY, NOT A CRASH AND NOT AN EMPTY STRING.
     * Paint's FIRST call is a lookup of a key it has not created yet, and it
     * is supposed to fail -- that failure is what makes it register.
     */
    case WOWSHELL_REGQUERYVALUE:
    {
        DWORD key16 = Wow32ArgDword(frame, WOWSHELL_REGQUERYVALUE_ARG_HKEY);
        CHAR subkeyBuffer[WOWSHELL_SUBKEY_MAX];
        CHAR valueBuffer[WOWSHELL_VALUE_MAX];
        PCSTR subkey = WowShellSubkey(frame, WOWSHELL_REGQUERYVALUE_ARG_SUBKEY, subkeyBuffer, sizeof subkeyBuffer);
        HKEY  parent = WowShellKey32(key16);
        volatile BYTE *byteCountPointer = Wow32ArgPointer(frame, WOWSHELL_REGQUERYVALUE_ARG_CBVALUE);
        volatile BYTE *destination = Wow32ArgPointer(frame, WOWSHELL_REGQUERYVALUE_ARG_VALUE);
        LONG capacity = 0;
        LONG byteCount = (LONG)sizeof valueBuffer;
        LONG  result;
        INT noteLength = 0;
        INT index;
        INT cursor;
        WowShellNoteKey(note, noteCapacity, &noteLength, "RegQueryValue", key16, subkey);
        if (!parent)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT A KEY THIS HOST ISSUED;"
                                       " ERROR_BADKEY");
            Wow32SetReturn(frame, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        if (!byteCountPointer || !destination)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO BUFFER (lpValue or lpcbValue is"
                                       " a null far pointer); ERROR_INVALID");
            Wow32SetReturn(frame, WOWSHELL_ERR_INVALID);
            return 1;
        }
        capacity = (LONG)((DWORD)Wow32PeekWord(byteCountPointer) | ((DWORD)Wow32PeekWord(byteCountPointer + WOW_WORD_BYTES) << WORD_SHIFT));
        result = RegQueryValueA(parent, subkey, valueBuffer, &byteCount);
        if (result != ERROR_SUCCESS)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT PRESENT (rc=0x");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_DWORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, "); ERROR_BADKEY -- which for a guest's"
                                       " FIRST lookup is the correct answer and"
                                       " is what makes it register");
            Wow32SetReturn(frame, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        valueBuffer[sizeof valueBuffer - 1] = 0;
        for (index = 0; valueBuffer[index]; ++index) /* length, no CRT here */
        {
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteQuoted(note, noteCapacity, &noteLength, valueBuffer);
        if (capacity <= index)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ BUT THE GUEST'S BUFFER IS 0x");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)capacity, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " BYTES AND THAT NEEDS MORE; nothing was"
                                       " written (ERROR_CANTREAD)");
            WowShellPutDword(frame, WOWSHELL_REGQUERYVALUE_ARG_CBVALUE, (DWORD)(index + 1));
            Wow32SetReturn(frame, WOWSHELL_ERR_CANTREAD);
            return 1;
        }
        for (cursor = 0; cursor <= index; ++cursor)
            destination[cursor] = (BYTE)valueBuffer[cursor];                                           /* the NUL travels too */
        WowShellPutDword(frame, WOWSHELL_REGQUERYVALUE_ARG_CBVALUE, (DWORD)index);
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* 0x07 RegEnumKey(hKey, iSubkey, lpszBuffer, cbBuffer) (Importance = 1):
     * Not in Paint's list, but it is how a Win16 OLE CLIENT walks the database
     * to find out what servers exist -- the other half of what Paint is
     * registering itself into -- and it is four lines given the rest.
     *
     * [CAUTION]: cbBuffer HERE IS A PLAIN VALUE, not a pointer, so the guest gets no
     * length back; the buffer is NUL-terminated within its declared size and
     * an over-long name is an error, as it is on Win32.
     */
    case WOWSHELL_REGENUMKEY:
    {
        DWORD key16 = Wow32ArgDword(frame, WOWSHELL_REGENUMKEY_ARG_HKEY);
        DWORD itemIndex  = Wow32ArgDword(frame, WOWSHELL_REGENUMKEY_ARG_INDEX);
        DWORD capacity  = Wow32ArgDword(frame, WOWSHELL_REGENUMKEY_ARG_CBBUF);
        HKEY  parent = WowShellKey32(key16);
        volatile BYTE *destination = Wow32ArgPointer(frame, WOWSHELL_REGENUMKEY_ARG_BUF);
        CHAR  nameBuffer[WOWSHELL_SUBKEY_MAX];
        LONG  result;
        INT noteLength = 0;
        INT index;
        INT cursor;
        WowNotePut(note, noteCapacity, &noteLength, "RegEnumKey key=0x");
        WowNoteHex(note, noteCapacity, &noteLength, key16, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " index=0x");
        WowNoteHex(note, noteCapacity, &noteLength, itemIndex, WOW_HEX_WORD_DIGITS);
        if (!parent || !destination || !capacity)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ no key or no buffer; ERROR_BADKEY");
            Wow32SetReturn(frame, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        result = RegEnumKeyA(parent, itemIndex, nameBuffer, (DWORD)sizeof nameBuffer);
        if (result != ERROR_SUCCESS)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- no such subkey (the end of the"
                                       " enumeration); ERROR_BADKEY");
            Wow32SetReturn(frame, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        nameBuffer[sizeof nameBuffer - 1] = 0;
        for (index = 0; nameBuffer[index]; ++index)
        {
        }
        if ((DWORD)index + 1 > capacity)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ the name does not fit the guest's"
                                       " buffer; nothing written"
                                       " (ERROR_CANTREAD)");
            Wow32SetReturn(frame, WOWSHELL_ERR_CANTREAD);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteQuoted(note, noteCapacity, &noteLength, nameBuffer);
        for (cursor = 0; cursor <= index; ++cursor)
            destination[cursor] = (BYTE)nameBuffer[cursor];                                           /* the NUL travels too */
        Wow32SetReturn(frame, 0);
        return 1;
    }

    default:
        return 0;
    }
}
