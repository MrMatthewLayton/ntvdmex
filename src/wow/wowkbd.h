#ifndef WOWKBD_H
#define WOWKBD_H
/*
 * wowkbd.h -- ★ KEYBOARD.DRV's OWN ID SPACE.  GH #128, session 44.
 *
 * A SIXTH id space, and the smallest one yet: `tools/ne/neneeds.py` says
 * NOTEPAD.EXE imports exactly two things from KEYBOARD that reach the 32-bit
 * side, `5 ANSITOOEM` and `6 OEMTOANSI`, and both are character-set conversion
 * rather than anything to do with keys.
 *
 * ── WHY A TEXT EDITOR IMPORTS THEM, AND WHY IT MATTERS HERE ─────────────────
 * A Windows 3.x program holds text in the ANSI (Windows) code page and a DOS
 * file holds it in the OEM one, so a Win16 editor converts on the way in and out.
 * That puts these two directly in the path this session could not finish: Notepad
 * opened, seeked and READ its file, and nothing appeared. An unimplemented call
 * here returns the harness sentinel and touches neither buffer -- so the
 * destination stays whatever it was, which for a freshly allocated block is
 * zeros, and a zero-length string is exactly what "nothing appeared" looks like.
 * ⚠ THAT IS A CANDIDATE, NOT A DIAGNOSIS. It is written down as the reason these
 *   two went in first, so that if the file still does not appear the next reader
 *   knows this was tried and can stop suspecting it.
 *
 * ── THE IDS, FROM THE FILE ──────────────────────────────────────────────────
 *   ordinal 5 ANSITOOEM -> entry table FIXED seg 1 offset 108 (0x6c), and the
 *     bytes there are `6a 08 68 00 00 68 05 00 9a` -- 8 argument bytes, id 0x05.
 *   ordinal 6 OEMTOANSI -> offset 121 (0x79), `6a 08 68 00 00 68 06 00 9a`.
 * A stub is 13 bytes, so the return addresses are 0x0079 and 0x0086. As with
 * SHELL and COMMDLG the ids are the export ordinals; as always that is checked
 * per module and never assumed (krnl386's are nothing like its ordinals).
 *
 * ── THE ARGUMENTS ───────────────────────────────────────────────────────────
 * 8 bytes is two far pointers, and the block is reversed as always, so the
 * DESTINATION -- the last parameter and therefore the last push -- is at +0:
 *     AnsiToOem(lpAnsiStr, lpOemStr)   ->  +0 lpOemStr   +4 lpAnsiStr
 *     OemToAnsi(lpOemStr, lpAnsiStr)   ->  +0 lpAnsiStr  +4 lpOemStr
 * ⚠ Getting that round the wrong way would convert in place over the source and
 *   leave the destination untouched -- which looks exactly like doing nothing,
 *   i.e. like the bug this is here to fix. The direction is taken from the
 *   parameter order plus the reversal rule, both of which this host has
 *   confirmed on every other block it reads.
 *
 * ⚠ NUL-TERMINATED, NO COUNT. These are the counted versions' siblings
 *   (`AnsiToOemBuff` takes a length and is a different ordinal), so the length
 *   is the source string's and the destination must be at least as large. That
 *   is the caller's contract in Win16 and it is unchanged here; Win32's
 *   CharToOemA/OemToCharA have exactly the same one.
 */

#define WOWKBD_ANSITOOEM   0x0005
#define WOWKBD_OEMTOANSI   0x0006

#define A2O_ARG_DST   0
#define A2O_ARG_SRC   4

static int wowkbd_call(wow32_frame_t *f, char *note, int notecap)
{
    if (notecap) note[0] = 0;
    switch (f->id) {

    /* ── ★ 0x05 AnsiToOem / 0x06 OemToAnsi ──────────────────────────────────
         The real Win32 conversions, against the real code pages. This is not a
         pass-through for convenience: the OEM code page is a property of the
         MACHINE, our DOS layer writes the guest's bytes to real files that other
         programs on this box read, and inventing a table here would make Notepad
         disagree with every other program about what byte means what character.
       ⚠ BOTH POINTERS ARE THE GUEST'S. They resolve into guest memory, which is
         our memory, so the conversion happens in place in the application's own
         buffers -- there is nothing to copy back and nothing to bound beyond
         what the guest already allocated.
       ⚠ Win16 returns void. The thunk pops a return slot regardless, so this
         writes one; a caller reading it would be reading something Win16 never
         defined, and the log says which way the conversion went either way. */
    case WOWKBD_ANSITOOEM:
    case WOWKBD_OEMTOANSI: {
        int   toOem = (f->id == WOWKBD_ANSITOOEM);
        volatile BYTE *dst = wow32_argptr(f, A2O_ARG_DST);
        volatile BYTE *src = wow32_argptr(f, A2O_ARG_SRC);
        int k = 0, ok;
        wu_puts(note, notecap, &k, toOem ? "AnsiToOem " : "OemToAnsi ");
        if (!src || !dst) {
            wu_puts(note, notecap, &k, "-- ★ NULL pointer (src or dst); nothing"
                                       " converted");
            wow32_setret(f, 0);
            return 1;
        }
        wu_putq(note, notecap, &k, (const char *)src);
        ok = toOem ? CharToOemA((LPCSTR)src, (LPSTR)dst)
                   : OemToCharA((LPCSTR)src, (LPSTR)dst);
        wu_puts(note, notecap, &k, ok ? " -> " : " -- ★ CONVERSION FAILED, dst is"
                                                " whatever it was: ");
        wu_putq(note, notecap, &k, (const char *)dst);
        wow32_setret(f, (DWORD)(ok ? 1 : 0));
        return 1;
    }

    default:
        return 0;
    }
}

#endif /* WOWKBD_H */
