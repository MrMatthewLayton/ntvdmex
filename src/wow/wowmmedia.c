/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * MMSYSTEM.DLL's OWN ID SPACE (WOW32's "MMEDIA" table).  #278, s90.
 *
 * The code of wowmmedia.h (#335): its functions and state, in their original order;
 * its own translation unit, declared in wowmmedia.h.
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
#include "wowcommdlg.h"
#include "wowkbd.h"
#include "wowsound.h"
#include "wowmmedia.h"

INT WowMultimediaCall(WOW32_FRAME *frame, PSTR note, INT noteCapacity)
{
    INT noteLength = 0;

    if (noteCapacity)
        note[0] = 0;
    switch (frame->Id)
    {
    case WOWMM_CALLPROC32:
    {
        DWORD arguments[WOWMM_CALLPROC32_ARGUMENTS];
        DWORD procedure  = Wow32ArgDword(frame, WOWMM_CALLPROC32_ARG_PROCEDURE);
        DWORD directoryChange = Wow32ArgDword(frame, WOWMM_CALLPROC32_ARG_DIRCHANGE);
        DWORD result;
        arguments[0] = Wow32ArgDword(frame, WOWMM_CALLPROC32_ARG_DEVICE);
        arguments[1] = Wow32ArgDword(frame, WOWMM_CALLPROC32_ARG_MESSAGE);
        arguments[2] = Wow32ArgDword(frame, WOWMM_CALLPROC32_ARG_INSTANCE);
        arguments[3] = Wow32ArgDword(frame, WOWMM_CALLPROC32_ARG_PARAM1);
        arguments[4] = Wow32ArgDword(frame, WOWMM_CALLPROC32_ARG_PARAM2);
        WowNotePut(note, noteCapacity, &noteLength, "mmCallProc32(fn=0x");
        WowNoteHex(note, noteCapacity, &noteLength, procedure, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", dev=0x");
        WowNoteHex(note, noteCapacity, &noteLength, arguments[0], WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", msg=0x");
        WowNoteHex(note, noteCapacity, &noteLength, arguments[1], WOW_HEX_WORD_DIGITS);
        if (directoryChange)
            WowNotePut(note, noteCapacity, &noteLength, ", dirchange");
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!procedure) { WowNotePut(note, noteCapacity, &noteLength, " -- NULL procedure; 0");
                   Wow32SetReturn(frame, 0);
                   return 1; }
        result = WowGenericThunkInvoke(procedure, arguments, WOWMM_CALLPROC32_ARGUMENTS);
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, result, WOW_HEX_DWORD_DIGITS);
        Wow32SetReturn(frame, result);
        return 1;
    }

    case WOWMM_YIELD:
        Sleep(1);
        WowNotePut(note, noteCapacity, &noteLength, "mm yield (timeGetTime had not moved)");
        Wow32SetReturn(frame, 0);
        return 1;
    }
    return 0;
}
