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

static INT WowMultimediaCall(wow32_frame_t *frame, PSTR note, INT noteCapacity)
{
    INT noteLength = 0;
    if (noteCapacity) note[0] = 0;
    switch (frame->id) {
    case WOWMM_CALLPROC32: {
        DWORD arguments[5];
        DWORD procedure  = wow32_argd(frame, 4);
        DWORD directoryChange = wow32_argd(frame, 0);
        DWORD result;
        arguments[0] = wow32_argd(frame, 24); arguments[1] = wow32_argd(frame, 20); arguments[2] = wow32_argd(frame, 16);
        arguments[3] = wow32_argd(frame, 12); arguments[4] = wow32_argd(frame, 8);
        wu_puts(note, noteCapacity, &noteLength, "mmCallProc32(fn=0x");
        wu_puthex(note, noteCapacity, &noteLength, procedure, 8);
        wu_puts(note, noteCapacity, &noteLength, ", dev=0x"); wu_puthex(note, noteCapacity, &noteLength, arguments[0], 4);
        wu_puts(note, noteCapacity, &noteLength, ", msg=0x"); wu_puthex(note, noteCapacity, &noteLength, arguments[1], 4);
        if (directoryChange) wu_puts(note, noteCapacity, &noteLength, ", dirchange");
        wu_puts(note, noteCapacity, &noteLength, ")");
        if (!procedure) { wu_puts(note, noteCapacity, &noteLength, " -- NULL procedure; 0");
                   wow32_setret(frame, 0); return 1; }
        result = wow_gt_invoke(procedure, arguments, 5);
        wu_puts(note, noteCapacity, &noteLength, " -> 0x"); wu_puthex(note, noteCapacity, &noteLength, result, 8);
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
