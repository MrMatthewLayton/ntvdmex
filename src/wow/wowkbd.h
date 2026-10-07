#ifndef NTVDMEX_WOWKBD_H
#define NTVDMEX_WOWKBD_H
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

/* ── ★★ CHARMAP's two, and they live HERE and not in wowuser.h. ──────────────
     VkKeyScan and MapVirtualKey are KEYBOARD.DRV exports in Win16, not USER's,
     and every thunk module has an id space ALL ITS OWN -- 0x81 is VkKeyScan here
     and something else entirely in USER. Implementing them in the wrong file
     compiles, dispatches from the wrong table, and answers a question nobody
     asked. (docs/STATE.md: gate on the BOP's own CS.) */
/* GetKeyNameText(lParam, lpszBuffer, nMaxCount) -- 4+4+2 = 10, reversed. */
#define WOWKBD_GETKEYNAMETEXT 0x0085
#define WOWKBD_GETKEYNAMETEXT_ARG_COUNT   0
#define WOWKBD_GETKEYNAMETEXT_ARG_BUFFER     2               /* far */
#define WOWKBD_GETKEYNAMETEXT_ARG_LPARAM  6               /* DWORD */

#define WOWKBD_VKKEYSCAN     0x0081
#define WOWKBD_MAPVIRTUALKEY 0x0083
#define WOWKBD_VKKEYSCAN_ARG_CHARACTER   0
#define WOWKBD_MAPVIRTUALKEY_ARG_TYPE 0
#define WOWKBD_MAPVIRTUALKEY_ARG_CODE 2

/* s90 (#297): GetKBCodePage() -- no arguments. Win32's GetKBCodePage is the OEM
   code page (GetOEMCP), which is what Win16's answered too: 437 on a US machine. */
#define WOWKBD_GETKBCODEPAGE 0x0084
#define WOWKBD_ANSITOOEM   0x0005
#define WOWKBD_OEMTOANSI   0x0006

#define WOWKBD_CONVERT_ARG_DESTINATION   0
#define WOWKBD_CONVERT_ARG_SOURCE   4
#define WOWKBD_KEY_NAME_MAX         64
#define WOWKBD_CHARACTER_MASK       0xFF

static INT WowKeyboardCall(wow32_frame_t *frame, PSTR note, INT noteCapacity)
{
    if (noteCapacity) note[0] = 0;
    switch (frame->id) {

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
    /* ── ★ 0x85 GetKeyNameText(LONG lParam, LPSTR lpszBuffer, int cch) = 10 ──
         The name of a key, for a program showing an accelerator ("Ctrl+F4") --
         and the ONE thing that makes it a translation rather than a passthrough
         is that the name comes from the KEYBOARD LAYOUT, which is the OS's, not
         ours. Win32's GetKeyNameTextA takes the identical lParam bit field (the
         scan code at 16..23, the extended bit at 24, "do not care" at 25), so
         the value goes across unchanged and the OS answers in the user's own
         layout and language -- which is exactly what a Win16 program asking this
         question wanted and could not have got from a table we invented.
       ⚠ THE COUNT IS A BUFFER SIZE INCLUDING THE NUL in both, and the return is
         the length WITHOUT it. Same in both worlds; checked rather than assumed
         because a length convention off by one writes a NUL past a guest's
         buffer. */
    case WOWKBD_GETKBCODEPAGE: {
        UINT codePage = GetKBCodePage();
        INT  noteLength = 0;
        wu_puts(note, noteCapacity, &noteLength, "GetKBCodePage() -> ");
        wu_puthex(note, noteCapacity, &noteLength, codePage, WOW_HEX_WORD_DIGITS);
        wow32_setret(frame, (DWORD)(WORD)codePage);
        return 1;
    }
    case WOWKBD_GETKEYNAMETEXT: {
        DWORD keyParameter  = wow32_argd(frame, WOWKBD_GETKEYNAMETEXT_ARG_LPARAM);
        WORD  bufferSize = wow32_argw(frame, WOWKBD_GETKEYNAMETEXT_ARG_COUNT);
        volatile BYTE *destination = wow32_argptr(frame, WOWKBD_GETKEYNAMETEXT_ARG_BUFFER);
        CHAR keyName[WOWKBD_KEY_NAME_MAX];
        INT noteLength = 0, nameLength = 0, index;
        wu_puts(note, noteCapacity, &noteLength, "GetKeyNameText(lParam=0x");
        wu_puthex(note, noteCapacity, &noteLength, keyParameter, WOW_HEX_DWORD_DIGITS);
        wu_puts(note, noteCapacity, &noteLength, ", cch=");
        wu_puthex(note, noteCapacity, &noteLength, bufferSize, WOW_HEX_WORD_DIGITS);
        wu_puts(note, noteCapacity, &noteLength, ")");
        if (!destination || !bufferSize) {
            wu_puts(note, noteCapacity, &noteLength, " -- no buffer; answered 0");
            wow32_setret(frame, 0);
            return 1;
        }
        nameLength = GetKeyNameTextA((LONG)keyParameter, keyName, (INT)(bufferSize < sizeof keyName ? bufferSize
                                                                    : sizeof keyName));
        if (nameLength < 0) nameLength = 0;
        for (index = 0; index < nameLength && index < (INT)bufferSize - 1; ++index) destination[index] = (BYTE)keyName[index];
        destination[index] = 0;
        wu_puts(note, noteCapacity, &noteLength, " -> ");
        wu_putq(note, noteCapacity, &noteLength, keyName);
        wow32_setret(frame, (DWORD)index);
        return 1;
    }

    case WOWKBD_VKKEYSCAN: {
        WORD character = wow32_argw(frame, WOWKBD_VKKEYSCAN_ARG_CHARACTER);
        SHORT scan = VkKeyScanA((CHAR)(character & WOWKBD_CHARACTER_MASK));
        wow32_setret(frame, (DWORD)(WORD)scan);
        return 1;
    }
    case WOWKBD_MAPVIRTUALKEY: {
        WORD code = wow32_argw(frame, WOWKBD_MAPVIRTUALKEY_ARG_CODE);
        WORD mapType = wow32_argw(frame, WOWKBD_MAPVIRTUALKEY_ARG_TYPE);
        wow32_setret(frame, (DWORD)MapVirtualKeyA(code, mapType));
        return 1;
    }

    case WOWKBD_ANSITOOEM:
    case WOWKBD_OEMTOANSI: {
        INT   isToOem = (frame->id == WOWKBD_ANSITOOEM);
        volatile BYTE *destination = wow32_argptr(frame, WOWKBD_CONVERT_ARG_DESTINATION);
        volatile BYTE *source = wow32_argptr(frame, WOWKBD_CONVERT_ARG_SOURCE);
        INT noteLength = 0, isConverted;
        wu_puts(note, noteCapacity, &noteLength, isToOem ? "AnsiToOem " : "OemToAnsi ");
        if (!source || !destination) {
            wu_puts(note, noteCapacity, &noteLength, "-- ★ NULL pointer (src or dst); nothing"
                                       " converted");
            wow32_setret(frame, 0);
            return 1;
        }
        wu_putq(note, noteCapacity, &noteLength, (PCSTR)source);
        isConverted = isToOem ? CharToOemA((LPCSTR)source, (LPSTR)destination)
                   : OemToCharA((LPCSTR)source, (LPSTR)destination);
        wu_puts(note, noteCapacity, &noteLength, isConverted ? " -> " : " -- ★ CONVERSION FAILED, dst is"
                                                " whatever it was: ");
        wu_putq(note, noteCapacity, &noteLength, (PCSTR)destination);
        wow32_setret(frame, (DWORD)(isConverted ? 1 : 0));
        return 1;
    }

    default:
        return 0;
    }
}

#endif /* NTVDMEX_WOWKBD_H */
