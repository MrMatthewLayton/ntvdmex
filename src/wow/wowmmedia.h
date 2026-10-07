#ifndef NTVDMEX_WOWMMEDIA_H
#define NTVDMEX_WOWMMEDIA_H
/*
 * wowmmedia.h -- ★ MMSYSTEM.DLL's OWN ID SPACE (WOW32's "MMEDIA" table).  #278, s90.
 *
 * XP's 16-bit MMSYSTEM reaches 32-bit WINMM almost entirely through the GENERIC
 * thunks (LoadLibraryEx32W("winmm.dll"), GetProcAddress32W for a dozen entry points)
 * and then calls those entry points through exactly TWO WOW ids of its own (its
 * thunk table has two stubs; ids additive on KERNEL.581 __MOD_MMEDIA = 0):
 *   id 2, 28 argument bytes      id 1, no arguments
 *
 * ── id 2: mmCallProc32(uDevId, uMsg, dwInst, dwP1, dwP2, lpProc32, fDirChange) ────
 * Seven DWORDs, PASCAL, so reversed in the frame: +0 fDirChange, +4 the 32-bit
 * procedure, +8 dwP2, +12 dwP1, +16 dwInst, +20 uMsg, +24 uDevId -- the shape
 * the frames for NotifyCallbackData arrive in (s90). The procedure is a WINMM
 * stdcall entry taking the first five; fDirChange asks for the 32-bit current
 * directory to follow the 16-bit task's first (MCI opens files by relative name)
 * -- this host runs the guest in its own directory already, so it is logged and
 * not acted on.
 * ── id 1: no arguments; it arrives from timeGetTime when the 32-bit clock has not
 * moved since the last read -- a yield so that it can. ────────────────────────────
 */

#define WOWMM_YIELD       0x0001
#define WOWMM_CALLPROC32  0x0002

/* mmCallProc32's frame, reversed (PASCAL), and the five arguments the procedure takes. */
#define WOWMM_CALLPROC32_ARG_DIRCHANGE  0
#define WOWMM_CALLPROC32_ARG_PROCEDURE  4
#define WOWMM_CALLPROC32_ARG_PARAM2     8
#define WOWMM_CALLPROC32_ARG_PARAM1     12
#define WOWMM_CALLPROC32_ARG_INSTANCE   16
#define WOWMM_CALLPROC32_ARG_MESSAGE    20
#define WOWMM_CALLPROC32_ARG_DEVICE     24
#define WOWMM_CALLPROC32_ARGUMENTS      5

static INT WowMultimediaCall(wow32_frame_t *frame, PSTR note, INT noteCapacity)
{
    INT noteLength = 0;
    if (noteCapacity) note[0] = 0;
    switch (frame->id) {
    case WOWMM_CALLPROC32: {
        DWORD arguments[WOWMM_CALLPROC32_ARGUMENTS];
        DWORD procedure  = wow32_argd(frame, WOWMM_CALLPROC32_ARG_PROCEDURE);
        DWORD directoryChange = wow32_argd(frame, WOWMM_CALLPROC32_ARG_DIRCHANGE);
        DWORD result;
        arguments[0] = wow32_argd(frame, WOWMM_CALLPROC32_ARG_DEVICE); arguments[1] = wow32_argd(frame, WOWMM_CALLPROC32_ARG_MESSAGE); arguments[2] = wow32_argd(frame, WOWMM_CALLPROC32_ARG_INSTANCE);
        arguments[3] = wow32_argd(frame, WOWMM_CALLPROC32_ARG_PARAM1); arguments[4] = wow32_argd(frame, WOWMM_CALLPROC32_ARG_PARAM2);
        wu_puts(note, noteCapacity, &noteLength, "mmCallProc32(fn=0x");
        wu_puthex(note, noteCapacity, &noteLength, procedure, WOW_HEX_DWORD_DIGITS);
        wu_puts(note, noteCapacity, &noteLength, ", dev=0x"); wu_puthex(note, noteCapacity, &noteLength, arguments[0], WOW_HEX_WORD_DIGITS);
        wu_puts(note, noteCapacity, &noteLength, ", msg=0x"); wu_puthex(note, noteCapacity, &noteLength, arguments[1], WOW_HEX_WORD_DIGITS);
        if (directoryChange) wu_puts(note, noteCapacity, &noteLength, ", dirchange");
        wu_puts(note, noteCapacity, &noteLength, ")");
        if (!procedure) { wu_puts(note, noteCapacity, &noteLength, " -- NULL procedure; 0");
                   wow32_setret(frame, 0); return 1; }
        result = wow_gt_invoke(procedure, arguments, WOWMM_CALLPROC32_ARGUMENTS);
        wu_puts(note, noteCapacity, &noteLength, " -> 0x"); wu_puthex(note, noteCapacity, &noteLength, result, WOW_HEX_DWORD_DIGITS);
        wow32_setret(frame, result);
        return 1;
    }
    case WOWMM_YIELD:
        Sleep(1);
        wu_puts(note, noteCapacity, &noteLength, "mm yield (timeGetTime had not moved)");
        wow32_setret(frame, 0);
        return 1;
    }
    return 0;
}

#endif /* NTVDMEX_WOWMMEDIA_H */
