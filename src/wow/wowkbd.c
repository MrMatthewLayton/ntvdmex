/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * KEYBOARD.DRV's OWN ID SPACE.  GH #128, session 44.
 *
 * The code of wowkbd.h (#335): its functions and state, in their original order;
 * its own translation unit, declared in wowkbd.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "wowkbd.h"
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
#include "wowcommdlg.h"

INT WowKeyboardCall(WOW32_FRAME *frame, PSTR note, INT noteCapacity)
{
    if (noteCapacity)
        note[0] = 0;
    switch (frame->Id)
    {

    /* 0x05 AnsiToOem / 0x06 OemToAnsi (Importance = 1):
     * The real Win32 conversions, against the real code pages. This is not a
     * pass-through for convenience: the OEM code page is a property of the
     * MACHINE, our DOS layer writes the guest's bytes to real files that other
     * programs on this box read, and inventing a table here would make Notepad
     * disagree with every other program about what byte means what character.
     *
     * [CAUTION]: BOTH POINTERS ARE THE GUEST'S. They resolve into guest memory, which is
     * our memory, so the conversion happens in place in the application's own
     * buffers -- there is nothing to copy back and nothing to bound beyond
     * what the guest already allocated.
     *
     * [CAUTION]: Win16 returns void. The thunk pops a return slot regardless, so this
     * writes one; a caller reading it would be reading something Win16 never
     * defined, and the log says which way the conversion went either way.
     */
    /* 0x85 GetKeyNameText(LONG lParam, LPSTR lpszBuffer, int cch) = 10 (Importance = 1):
     * The name of a key, for a program showing an accelerator ("Ctrl+F4") --
     * and the ONE thing that makes it a translation rather than a passthrough
     * is that the name comes from the KEYBOARD LAYOUT, which is the OS's, not
     * ours. Win32's GetKeyNameTextA takes the identical lParam bit field (the
     * scan code at 16..23, the extended bit at 24, "do not care" at 25), so
     * the value goes across unchanged and the OS answers in the user's own
     * layout and language -- which is exactly what a Win16 program asking this
     * question wanted and could not have got from a table we invented.
     *
     * [CAUTION]: THE COUNT IS A BUFFER SIZE INCLUDING THE NUL in both, and the return is
     * the length WITHOUT it. Same in both worlds; checked rather than assumed
     * because a length convention off by one writes a NUL past a guest's
     * buffer.
     */
    case WOWKBD_GETKBCODEPAGE:
    {
        UINT codePage = GetKBCodePage();
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetKBCodePage() -> ");
        WowNoteHex(note, noteCapacity, &noteLength, codePage, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)codePage);
        return 1;
    }

    case WOWKBD_GETKEYNAMETEXT:
    {
        DWORD keyParameter  = Wow32ArgDword(frame, WOWKBD_GETKEYNAMETEXT_ARG_LPARAM);
        WORD  bufferSize = Wow32ArgWord(frame, WOWKBD_GETKEYNAMETEXT_ARG_COUNT);
        volatile BYTE *destination = Wow32ArgPointer(frame, WOWKBD_GETKEYNAMETEXT_ARG_BUFFER);
        CHAR keyName[WOWKBD_KEY_NAME_MAX];
        INT noteLength = 0;
        INT nameLength = 0;
        INT index;
        WowNotePut(note, noteCapacity, &noteLength, "GetKeyNameText(lParam=0x");
        WowNoteHex(note, noteCapacity, &noteLength, keyParameter, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", cch=");
        WowNoteHex(note, noteCapacity, &noteLength, bufferSize, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!destination || !bufferSize)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- no buffer; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        nameLength = GetKeyNameTextA((LONG)keyParameter, keyName, (INT)(bufferSize < sizeof keyName ? bufferSize
                                                                    : sizeof keyName));
        if (nameLength < 0)
            nameLength = 0;
        for (index = 0; index < nameLength && index < (INT)bufferSize - 1; ++index)
            destination[index] = (BYTE)keyName[index];
        destination[index] = 0;
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteQuoted(note, noteCapacity, &noteLength, keyName);
        Wow32SetReturn(frame, (DWORD)index);
        return 1;
    }

    case WOWKBD_VKKEYSCAN:
    {
        WORD character = Wow32ArgWord(frame, WOWKBD_VKKEYSCAN_ARG_CHARACTER);
        SHORT scan = VkKeyScanA((CHAR)(character & WOWKBD_CHARACTER_MASK));
        Wow32SetReturn(frame, (DWORD)(WORD)scan);
        return 1;
    }

    case WOWKBD_MAPVIRTUALKEY:
    {
        WORD code = Wow32ArgWord(frame, WOWKBD_MAPVIRTUALKEY_ARG_CODE);
        WORD mapType = Wow32ArgWord(frame, WOWKBD_MAPVIRTUALKEY_ARG_TYPE);
        Wow32SetReturn(frame, (DWORD)MapVirtualKeyA(code, mapType));
        return 1;
    }

    case WOWKBD_ANSITOOEM:
    case WOWKBD_OEMTOANSI:
    {
        INT   isToOem = (frame->Id == WOWKBD_ANSITOOEM);
        volatile BYTE *destination = Wow32ArgPointer(frame, WOWKBD_CONVERT_ARG_DESTINATION);
        volatile BYTE *source = Wow32ArgPointer(frame, WOWKBD_CONVERT_ARG_SOURCE);
        INT noteLength = 0;
        INT isConverted;
        WowNotePut(note, noteCapacity, &noteLength, isToOem ? "AnsiToOem " : "OemToAnsi ");
        if (!source || !destination)
        {
            WowNotePut(note, noteCapacity, &noteLength, "-- ★ NULL pointer (src or dst); nothing"
                                       " converted");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNoteQuoted(note, noteCapacity, &noteLength, (PCSTR)source);
        isConverted = isToOem ? CharToOemA((LPCSTR)source, (LPSTR)destination)
                   : OemToCharA((LPCSTR)source, (LPSTR)destination);
        WowNotePut(note, noteCapacity, &noteLength, isConverted ? " -> " : " -- ★ CONVERSION FAILED, dst is"
                                                " whatever it was: ");
        WowNoteQuoted(note, noteCapacity, &noteLength, (PCSTR)destination);
        Wow32SetReturn(frame, (DWORD)(isConverted ? 1 : 0));
        return 1;
    }

    default:
        return 0;
    }
}
