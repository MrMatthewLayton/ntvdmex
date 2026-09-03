#ifndef WOWSHELL_H
#define WOWSHELL_H
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
 * Notepad has seven DIALOG resources and none of them is the About box.
 * `tools/ne/neimports.py` walks its relocation chains and names the call outright:
 *
 *     seg1:call 0x0389      USER.174 LOADICON
 *     seg1:call 0x038f      SHELL.22 SHELLABOUT
 *
 * and the site reads straight off the disassembly, every push accounted for:
 *
 *     0374  push [0x10]        ; hWnd          -- Notepad's own main window
 *     0378  push ds / [0x4c]   ; szApp         -- far
 *     037d  push ds / 0xbe     ; szOtherStuff  -- far
 *     0381  push [0xaa0] / push 0 / push 1
 *     0389  lcall  <LoadIcon>  ;   ★ LoadIcon(hInstance, MAKEINTRESOURCE(1)),
 *                              ;     and ICON 1 is Notepad's own
 *     038e  push ax            ; hIcon
 *     038f  lcall  <ShellAbout>
 *
 * ⇒ the whole of Help > About is one API call, and answering it is the box.
 *
 * ── ★★ WHICH ID IT IS, FROM THE FILE AND NOT FROM THE ORDINAL ───────────────
 * The ids in a thunk module are not required to be its export ordinals, so this
 * was resolved rather than assumed. SHELL.DLL's non-resident name table gives
 * `22 SHELLABOUT`; its ENTRY table maps ordinal 22 to `MOVEABLE segment 1, offset
 * 186 = 0x00ba`; and the bytes at `seg1:0x00ba` are the stub:
 *
 *     00ba  6a 0c           push 12          ; ★ argument bytes
 *     00bc  68 00 00        push 0
 *     00bf  68 16 00        push 0x16        ; ★ THE ID
 *     00c2  9a b6 00 ....   lcall <the common thunk>
 *     00c7                  <- the return address a call carries at OFF_FROM
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
 * Reversed as always -- the base is the LAST word pushed -- so the call site
 * above lays out as, and it comes to exactly the 12 bytes the stub declares:
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
#define WOWSHELL_DRAGACCEPTFILES 0x0009
#define DAF_ARG_ACCEPT  0
#define DAF_ARG_HWND    2
#define WOWSHELL_DRAGQUERYFILE   0x000b
#define DQF_ARG_CCH     0
#define DQF_ARG_BUF     2
#define DQF_ARG_INDEX   6
#define DQF_ARG_HDROP   8

#define SA_ARG_HICON          0
#define SA_ARG_OTHER          2
#define SA_ARG_APP            6
#define SA_ARG_HWND          10

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

   ── THE IDS AND THE BLOCKS, FROM SHELL.DLL'S OWN ENTRY TABLE ────────────────
     `neneeds.py --stubs` resolves each ordinal to its stub and prints the return
     address that stub pushes; every retstub below matched the running guest to
     the digit, which is what makes this a reading rather than a parameter list
     copied out of a book:

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
     parameter list and the 12 bytes the stub declares agree. */
#define WOWSHELL_REGOPENKEY    0x0001
#define WOWSHELL_REGCREATEKEY  0x0002
#define RGK_ARG_RESULT    0        /* HKEY FAR*  -- pushed last               */
#define RGK_ARG_SUBKEY    4        /* LPCSTR                                  */
#define RGK_ARG_HKEY      8        /* HKEY       -- pushed first              */

#define WOWSHELL_REGCLOSEKEY   0x0003
#define RGC_ARG_HKEY      0

#define WOWSHELL_REGDELETEKEY  0x0004
#define RGD_ARG_SUBKEY    0
#define RGD_ARG_HKEY      4

#define WOWSHELL_REGSETVALUE   0x0005
#define RGS_ARG_CBDATA    0
#define RGS_ARG_DATA      4
#define RGS_ARG_TYPE      8
#define RGS_ARG_SUBKEY   12
#define RGS_ARG_HKEY     16

#define WOWSHELL_REGQUERYVALUE 0x0006
#define RGQ_ARG_CBVALUE   0        /* LONG FAR*  -- in: capacity, out: length */
#define RGQ_ARG_VALUE     4        /* LPSTR                                   */
#define RGQ_ARG_SUBKEY    8
#define RGQ_ARG_HKEY     12

#define WOWSHELL_REGENUMKEY    0x0007
#define RGE_ARG_CBBUF     0
#define RGE_ARG_BUF       4
#define RGE_ARG_INDEX     8
#define RGE_ARG_HKEY     12

/* Win3.1 SHELL.DLL's own error numbers -- NOT Win32's. 0 is success in both. */
#define WOWSHELL_ERR_BADKEY      2
#define WOWSHELL_ERR_CANTOPEN    3
#define WOWSHELL_ERR_CANTREAD    4
#define WOWSHELL_ERR_CANTWRITE   5
#define WOWSHELL_ERR_OUTOFMEMORY 6
#define WOWSHELL_ERR_INVALID     7

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

static HKEY  g_ws_key[WOWSHELL_KEYTOK_MAX];
static int   g_ws_nkey = 0;
static HKEY  g_ws_root = NULL;

/* The private hive's root, created on first use. NULL means the registry itself
   refused, which is reported to the guest rather than papered over. */
static HKEY wowshell_root(void)
{
    HKEY k;
    DWORD disp = 0;
    if (g_ws_root) return g_ws_root;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, WOWSHELL_REG_PATH, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
                        NULL, &k, &disp) != ERROR_SUCCESS)
        return NULL;
    g_ws_root = k;
    return k;
}

/* A guest DWORD -> the real key it names, or NULL. */
static HKEY wowshell_key32(DWORD h)
{
    DWORD i;
    if (h == WOWSHELL_HKCR16 || h == WOWSHELL_HKCR32) return wowshell_root();
    if (h < WOWSHELL_KEYTOK_BASE) return NULL;
    i = h - WOWSHELL_KEYTOK_BASE;
    if (i >= (DWORD)g_ws_nkey) return NULL;
    return g_ws_key[i];
}

/* Mint a token for a key we just opened. 0 = the table is full. */
static DWORD wowshell_key16(HKEY k)
{
    int i;
    if (!k) return 0;
    for (i = 0; i < g_ws_nkey; ++i)
        if (!g_ws_key[i]) break;                       /* reuse a closed slot */
    if (i == g_ws_nkey) {
        if (g_ws_nkey >= WOWSHELL_KEYTOK_MAX) return 0;
        i = g_ws_nkey++;
    }
    g_ws_key[i] = k;
    return WOWSHELL_KEYTOK_BASE + (DWORD)i;
}

/* Write a DWORD through a 16:16 far-pointer ARGUMENT (wow32_farput writes
   through a pointer inside a STRUCT, which is a different thing). */
static int wowshell_putd(const wow32_frame_t *f, int argoff, DWORD v)
{
    volatile BYTE *p = wow32_argptr(f, argoff);
    if (!p) return 0;
    wow32_pokew(p,     (WORD)(v & 0xFFFF));
    wow32_pokew(p + 2, (WORD)(v >> 16));
    return 1;
}

/* The subkey argument, or NULL -- and the difference is load-bearing: every one
   of these calls gives a null lpSubKey the meaning "the key itself". */
static const char *wowshell_sub(const wow32_frame_t *f, int argoff,
                                char *buf, int cap)
{
    return wow32_argstr(f, argoff, buf, cap) && buf[0] ? buf : NULL;
}

/* Shared reporting for the four calls that take (hKey, lpSubKey). */
static void wowshell_note_key(char *note, int notecap, int *k,
                              const char *name, DWORD hkey, const char *sub)
{
    wu_puts(note, notecap, k, name);
    wu_puts(note, notecap, k, " key=0x");
    wu_puthex(note, notecap, k, hkey, 8);
    if (hkey == WOWSHELL_HKCR16 || hkey == WOWSHELL_HKCR32)
        wu_puts(note, notecap, k, "(HKEY_CLASSES_ROOT)");
    wu_puts(note, notecap, k, " sub=");
    wu_putq(note, notecap, k, sub ? sub : "(the key itself)");
}

/*
 * ⚠ CALLED ONLY WHEN THE STUB IS SHELL'S. The caller checks, exactly as it does
 *   for USER; this file must never be reachable from another module's numbering.
 * `note` receives a short description for the caller's log line.
 */
static int wowshell_call(wow32_frame_t *f, char *note, int notecap)
{
    if (notecap) note[0] = 0;
    switch (f->id) {

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
    case WOWSHELL_SHELLABOUT: {
        WORD hwnd = wow32_argw(f, SA_ARG_HWND);
        WORD htok = wow32_argw(f, SA_ARG_HICON);
        wowuser_win_t *w = wowuser_findwin(hwnd);
        char app[160], other[320];
        int  k = 0, bits = 0, rc;
        HICON hico = wowuser_sysres_hicon(htok, &bits);
        HWND  owner = w ? w->hwnd32 : NULL;

        wow32_argstr(f, SA_ARG_APP,   app,   sizeof app);
        wow32_argstr(f, SA_ARG_OTHER, other, sizeof other);

        wu_puts(note, notecap, &k, "ShellAbout ");
        wu_putq(note, notecap, &k, app);
        wu_puts(note, notecap, &k, " / ");
        wu_putq(note, notecap, &k, other);
        wu_puts(note, notecap, &k, " owner=0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        if (!w)          wu_puts(note, notecap, &k, " -- ★ NO SUCH WINDOW; the box"
                                                    " comes up UNOWNED");
        else if (!owner) wu_puts(note, notecap, &k, " -- no real window behind it;"
                                                    " the box comes up UNOWNED");
        wu_puts(note, notecap, &k, " icon=0x");
        wu_puthex(note, notecap, &k, htok, 4);
        if (hico) {
            wu_puts(note, notecap, &k, " -> the app's own (");
            wu_puthex(note, notecap, &k, (DWORD)bits, 2);
            wu_puts(note, notecap, &k, " bpp)");
        } else {
            wu_puts(note, notecap, &k, " -- NOT RESOLVED; the system icon is used");
        }
        wu_puts(note, notecap, &k, " -- ★ MODAL: the VDM is stopped until it is"
                                   " dismissed");
        rc = ShellAboutA(owner, app, other, hico);
        wu_puts(note, notecap, &k, "; dismissed, rc=0x");
        wu_puthex(note, notecap, &k, (DWORD)rc, 4);
        wow32_setret(f, (DWORD)rc);
        return 1;
    }

    /* ── ★ 0x09 DragAcceptFiles(hWnd, fAccept) ──────────────────────────────
         The real one, on the real window: accepting drops is a property the
         window manager enforces, and ours is the OS's. A guest that asks for it
         and then gets no WM_DROPFILES would be a lie one level down.
       ⚠ WM_DROPFILES IS NOT FORWARDED YET, and this says so rather than leaving
         it to be discovered: the message carries an HDROP, which needs a token
         of its own, and no run has produced one. What this call does today is
         make the window accept a drop that nothing yet delivers. */
    case WOWSHELL_DRAGACCEPTFILES: {
        WORD hwnd = wow32_argw(f, DAF_ARG_HWND);
        WORD acc  = wow32_argw(f, DAF_ARG_ACCEPT);
        wowuser_win_t *w = wowuser_findwin(hwnd);
        int k = 0;
        wu_puts(note, notecap, &k, acc ? "DragAcceptFiles ACCEPT 0x"
                                       : "DragAcceptFiles REFUSE 0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        if (!w || !w->hwnd32) {
            wu_puts(note, notecap, &k, " -- no real window");
            wow32_setret(f, 0);
            return 1;
        }
        DragAcceptFiles(w->hwnd32, acc ? TRUE : FALSE);
        wu_puts(note, notecap, &k, " -> the OS's -- ⚠ but WM_DROPFILES is not"
                                   " forwarded yet, so nothing will arrive");
        wow32_setret(f, 0);
        return 1;
    }

    /* ── ★ 0x0b DragQueryFile(hDrop, iFile, lpszFile, cch) ──────────────────
       ⚠ REFUSED, NOT GUESSED. An HDROP can only have come from a WM_DROPFILES
         this host has never delivered, so any handle arriving here is one we did
         not issue. Answering 0 is the honest "no files", and the alternative --
         handing an arbitrary 16-bit number to Win32 as an HDROP -- would read
         somebody else's memory. */
    case WOWSHELL_DRAGQUERYFILE: {
        WORD hdrop = wow32_argw(f, DQF_ARG_HDROP);
        WORD idx   = wow32_argw(f, DQF_ARG_INDEX);
        int k = 0;
        wu_puts(note, notecap, &k, "DragQueryFile drop 0x");
        wu_puthex(note, notecap, &k, hdrop, 4);
        wu_puts(note, notecap, &k, " index 0x");
        wu_puthex(note, notecap, &k, idx, 4);
        wu_puts(note, notecap, &k, " -- ★ no HDROP has ever been issued by this"
                                   " host (WM_DROPFILES is not forwarded);"
                                   " answered 0 rather than handing Win32 a"
                                   " handle we did not make");
        wow32_setret(f, 0);
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
        int   create = (f->id == WOWSHELL_REGCREATEKEY);
        DWORD hkey = wow32_argd(f, RGK_ARG_HKEY);
        char  sbuf[256];
        const char *sub = wowshell_sub(f, RGK_ARG_SUBKEY, sbuf, sizeof sbuf);
        HKEY  parent = wowshell_key32(hkey), out = NULL;
        DWORD tok;
        LONG  rc;
        int   k = 0;
        wowshell_note_key(note, notecap, &k,
                          create ? "RegCreateKey" : "RegOpenKey", hkey, sub);
        if (!parent) {
            wu_puts(note, notecap, &k, " -- ★ NOT A KEY THIS HOST ISSUED;"
                                       " ERROR_BADKEY");
            wowshell_putd(f, RGK_ARG_RESULT, 0);
            wow32_setret(f, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        rc = create ? RegCreateKeyA(parent, sub, &out)
                    : RegOpenKeyA(parent, sub, &out);
        if (rc != ERROR_SUCCESS || !out) {
            wu_puts(note, notecap, &k, " -- the registry refused it, rc=0x");
            wu_puthex(note, notecap, &k, (DWORD)rc, 8);
            wowshell_putd(f, RGK_ARG_RESULT, 0);
            wow32_setret(f, create ? WOWSHELL_ERR_CANTWRITE
                                   : WOWSHELL_ERR_CANTOPEN);
            return 1;
        }
        tok = wowshell_key16(out);
        if (!tok) {
            RegCloseKey(out);
            wu_puts(note, notecap, &k, " -- ★ THE KEY TABLE IS FULL; the key was"
                                       " closed again and ERROR_OUTOFMEMORY"
                                       " answered");
            wowshell_putd(f, RGK_ARG_RESULT, 0);
            wow32_setret(f, WOWSHELL_ERR_OUTOFMEMORY);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> key token 0x");
        wu_puthex(note, notecap, &k, tok, 8);
        if (!wowshell_putd(f, RGK_ARG_RESULT, tok))
            wu_puts(note, notecap, &k, " -- ⚠ BUT phkResult WAS NOT WRITABLE");
        wow32_setret(f, 0);
        return 1;
    }

    /* ── ★ 0x03 RegCloseKey(hKey) ────────────────────────────────────────────
       ⚠ CLOSING A ROOT IS A NO-OP, NOT AN ERROR. Guests close HKEY_CLASSES_ROOT
         routinely; closing our cached hive handle would leave every later call
         holding a dead HKEY. */
    case WOWSHELL_REGCLOSEKEY: {
        DWORD hkey = wow32_argd(f, RGC_ARG_HKEY);
        int   k = 0;
        wu_puts(note, notecap, &k, "RegCloseKey 0x");
        wu_puthex(note, notecap, &k, hkey, 8);
        if (hkey == WOWSHELL_HKCR16 || hkey == WOWSHELL_HKCR32) {
            wu_puts(note, notecap, &k, " (HKEY_CLASSES_ROOT -- kept open)");
            wow32_setret(f, 0);
            return 1;
        }
        if (hkey >= WOWSHELL_KEYTOK_BASE
            && hkey - WOWSHELL_KEYTOK_BASE < (DWORD)g_ws_nkey) {
            DWORD i = hkey - WOWSHELL_KEYTOK_BASE;
            if (g_ws_key[i]) {
                RegCloseKey(g_ws_key[i]);
                g_ws_key[i] = NULL;               /* the slot becomes reusable */
                wu_puts(note, notecap, &k, " -> closed, token freed");
                wow32_setret(f, 0);
                return 1;
            }
        }
        wu_puts(note, notecap, &k, " -- ★ NOT AN OPEN KEY OF OURS; ERROR_BADKEY");
        wow32_setret(f, WOWSHELL_ERR_BADKEY);
        return 1;
    }

    /* ── ★ 0x04 RegDeleteKey(hKey, lpSubKey) ────────────────────────────────
       ⚠ Win32's RegDeleteKeyA will not delete a key that still has subkeys, and
         Win16's would. That difference is REPORTED rather than worked around by
         recursing: a guest deleting a populated key is doing something this host
         has never seen one do, and inventing a recursive delete against the real
         registry on a guess is not a thing to do quietly. */
    case WOWSHELL_REGDELETEKEY: {
        DWORD hkey = wow32_argd(f, RGD_ARG_HKEY);
        char  sbuf[256];
        const char *sub = wowshell_sub(f, RGD_ARG_SUBKEY, sbuf, sizeof sbuf);
        HKEY  parent = wowshell_key32(hkey);
        LONG  rc;
        int   k = 0;
        wowshell_note_key(note, notecap, &k, "RegDeleteKey", hkey, sub);
        if (!parent || !sub) {
            wu_puts(note, notecap, &k, !parent
                        ? " -- ★ NOT A KEY THIS HOST ISSUED; ERROR_BADKEY"
                        : " -- ★ NO SUBKEY NAMED; ERROR_BADKEY");
            wow32_setret(f, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        rc = RegDeleteKeyA(parent, sub);
        if (rc != ERROR_SUCCESS) {
            wu_puts(note, notecap, &k, " -- refused, rc=0x");
            wu_puthex(note, notecap, &k, (DWORD)rc, 8);
            wu_puts(note, notecap, &k, " (⚠ Win32 will not delete a key that"
                                       " still has subkeys; Win16 would)");
            wow32_setret(f, WOWSHELL_ERR_CANTWRITE);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> deleted");
        wow32_setret(f, 0);
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
        DWORD hkey = wow32_argd(f, RGS_ARG_HKEY);
        DWORD type = wow32_argd(f, RGS_ARG_TYPE);
        char  sbuf[256], dbuf[512];
        const char *sub = wowshell_sub(f, RGS_ARG_SUBKEY, sbuf, sizeof sbuf);
        HKEY  parent = wowshell_key32(hkey);
        LONG  rc;
        int   k = 0;
        wow32_argstr(f, RGS_ARG_DATA, dbuf, sizeof dbuf);
        wowshell_note_key(note, notecap, &k, "RegSetValue", hkey, sub);
        wu_puts(note, notecap, &k, " = ");
        wu_putq(note, notecap, &k, dbuf);
        if (!parent) {
            wu_puts(note, notecap, &k, " -- ★ NOT A KEY THIS HOST ISSUED;"
                                       " ERROR_BADKEY");
            wow32_setret(f, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        if (type != REG_SZ) {
            wu_puts(note, notecap, &k, " -- ★ TYPE IS NOT REG_SZ (0x");
            wu_puthex(note, notecap, &k, type, 8);
            wu_puts(note, notecap, &k, "); Win16 RegSetValue has no other type,"
                                       " so this is refused rather than stored"
                                       " unreadably");
            wow32_setret(f, WOWSHELL_ERR_INVALID);
            return 1;
        }
        rc = RegSetValueA(parent, sub, REG_SZ, dbuf, 0);
        if (rc != ERROR_SUCCESS) {
            wu_puts(note, notecap, &k, " -- the registry refused it, rc=0x");
            wu_puthex(note, notecap, &k, (DWORD)rc, 8);
            wow32_setret(f, WOWSHELL_ERR_CANTWRITE);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> stored");
        wow32_setret(f, 0);
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
        DWORD hkey = wow32_argd(f, RGQ_ARG_HKEY);
        char  sbuf[256], vbuf[512];
        const char *sub = wowshell_sub(f, RGQ_ARG_SUBKEY, sbuf, sizeof sbuf);
        HKEY  parent = wowshell_key32(hkey);
        volatile BYTE *cbp = wow32_argptr(f, RGQ_ARG_CBVALUE);
        volatile BYTE *dst = wow32_argptr(f, RGQ_ARG_VALUE);
        LONG  cap = 0, cb = (LONG)sizeof vbuf;
        LONG  rc;
        int   k = 0, i, j;
        wowshell_note_key(note, notecap, &k, "RegQueryValue", hkey, sub);
        if (!parent) {
            wu_puts(note, notecap, &k, " -- ★ NOT A KEY THIS HOST ISSUED;"
                                       " ERROR_BADKEY");
            wow32_setret(f, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        if (!cbp || !dst) {
            wu_puts(note, notecap, &k, " -- ★ NO BUFFER (lpValue or lpcbValue is"
                                       " a null far pointer); ERROR_INVALID");
            wow32_setret(f, WOWSHELL_ERR_INVALID);
            return 1;
        }
        cap = (LONG)((DWORD)wow32_peekw(cbp) | ((DWORD)wow32_peekw(cbp + 2) << 16));
        rc = RegQueryValueA(parent, sub, vbuf, &cb);
        if (rc != ERROR_SUCCESS) {
            wu_puts(note, notecap, &k, " -- ★ NOT PRESENT (rc=0x");
            wu_puthex(note, notecap, &k, (DWORD)rc, 8);
            wu_puts(note, notecap, &k, "); ERROR_BADKEY -- which for a guest's"
                                       " FIRST lookup is the correct answer and"
                                       " is what makes it register");
            wow32_setret(f, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        vbuf[sizeof vbuf - 1] = 0;
        for (i = 0; vbuf[i]; ++i) { }                    /* length, no CRT here */
        wu_puts(note, notecap, &k, " -> ");
        wu_putq(note, notecap, &k, vbuf);
        if (cap <= i) {
            wu_puts(note, notecap, &k, " -- ★ BUT THE GUEST'S BUFFER IS 0x");
            wu_puthex(note, notecap, &k, (DWORD)cap, 4);
            wu_puts(note, notecap, &k, " BYTES AND THAT NEEDS MORE; nothing was"
                                       " written (ERROR_CANTREAD)");
            wowshell_putd(f, RGQ_ARG_CBVALUE, (DWORD)(i + 1));
            wow32_setret(f, WOWSHELL_ERR_CANTREAD);
            return 1;
        }
        for (j = 0; j <= i; ++j) dst[j] = (BYTE)vbuf[j];   /* the NUL travels too */
        wowshell_putd(f, RGQ_ARG_CBVALUE, (DWORD)i);
        wow32_setret(f, 0);
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
        DWORD hkey = wow32_argd(f, RGE_ARG_HKEY);
        DWORD idx  = wow32_argd(f, RGE_ARG_INDEX);
        DWORD cap  = wow32_argd(f, RGE_ARG_CBBUF);
        HKEY  parent = wowshell_key32(hkey);
        volatile BYTE *dst = wow32_argptr(f, RGE_ARG_BUF);
        char  nbuf[256];
        LONG  rc;
        int   k = 0, i, j;
        wu_puts(note, notecap, &k, "RegEnumKey key=0x");
        wu_puthex(note, notecap, &k, hkey, 8);
        wu_puts(note, notecap, &k, " index=0x");
        wu_puthex(note, notecap, &k, idx, 4);
        if (!parent || !dst || !cap) {
            wu_puts(note, notecap, &k, " -- ★ no key or no buffer; ERROR_BADKEY");
            wow32_setret(f, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        rc = RegEnumKeyA(parent, idx, nbuf, (DWORD)sizeof nbuf);
        if (rc != ERROR_SUCCESS) {
            wu_puts(note, notecap, &k, " -- no such subkey (the end of the"
                                       " enumeration); ERROR_BADKEY");
            wow32_setret(f, WOWSHELL_ERR_BADKEY);
            return 1;
        }
        nbuf[sizeof nbuf - 1] = 0;
        for (i = 0; nbuf[i]; ++i) { }
        if ((DWORD)i + 1 > cap) {
            wu_puts(note, notecap, &k, " -- ★ the name does not fit the guest's"
                                       " buffer; nothing written"
                                       " (ERROR_CANTREAD)");
            wow32_setret(f, WOWSHELL_ERR_CANTREAD);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> ");
        wu_putq(note, notecap, &k, nbuf);
        for (j = 0; j <= i; ++j) dst[j] = (BYTE)nbuf[j];   /* the NUL travels too */
        wow32_setret(f, 0);
        return 1;
    }

    default:
        return 0;
    }
}

#endif /* WOWSHELL_H */
