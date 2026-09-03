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

    default:
        return 0;
    }
}

#endif /* WOWSHELL_H */
