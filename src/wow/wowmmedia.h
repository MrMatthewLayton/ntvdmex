#ifndef WOWMMEDIA_H
#define WOWMMEDIA_H
/*
 * wowmmedia.h -- ★ MMSYSTEM.DLL's OWN ID SPACE (WOW32's "MMEDIA" table).  #278, s90.
 *
 * XP's 16-bit MMSYSTEM reaches 32-bit WINMM almost entirely through the GENERIC
 * thunks (LoadLibraryEx32W("winmm.dll"), GetProcAddress32W for a dozen entry points)
 * and then calls those entry points through exactly TWO WOW ids of its own, read out
 * of the binary (mmsystem.dll seg1, ids additive on KERNEL.581 __MOD_MMEDIA = 0):
 *   seg1:0x611  `6a 1c / 68 0000 / 68 0002 / call WOW16Call`  -> id 2, 28 bytes
 *   seg1:0x61e  `6a 00 / 68 0000 / 68 0001 / call WOW16Call`  -> id 1, 0 bytes
 *
 * ── id 2: mmCallProc32(uDevId, uMsg, dwInst, dwP1, dwP2, lpProc32, fDirChange) ────
 * Seven DWORDs, PASCAL, so reversed in the frame: +0 fDirChange, +4 the 32-bit
 * procedure, +8 dwP2, +12 dwP1, +16 dwInst, +20 uMsg, +24 uDevId. MMSYSTEM's own
 * call site (seg8:0x6a9) pushes exactly that shape for NotifyCallbackData. The
 * procedure is a WINMM stdcall entry taking the first five; fDirChange asks for the
 * 32-bit current directory to follow the 16-bit task's first (MCI opens files by
 * relative name) -- this host runs the guest in its own directory already, so it is
 * logged and not acted on.
 * ── id 1: no arguments, called from inside timeGetTime when the 32-bit clock has
 * not moved since the last read (seg1:0x16b-0x176) -- a yield so that it can. ──────
 */

#define WOWMM_YIELD       0x0001
#define WOWMM_CALLPROC32  0x0002

static int wowmmedia_call(wow32_frame_t *f, char *note, int notecap)
{
    int k = 0;
    if (notecap) note[0] = 0;
    switch (f->id) {
    case WOWMM_CALLPROC32: {
        DWORD a[5];
        DWORD fn  = wow32_argd(f, 4);
        DWORD dir = wow32_argd(f, 0);
        DWORD r;
        a[0] = wow32_argd(f, 24); a[1] = wow32_argd(f, 20); a[2] = wow32_argd(f, 16);
        a[3] = wow32_argd(f, 12); a[4] = wow32_argd(f, 8);
        wu_puts(note, notecap, &k, "mmCallProc32(fn=0x");
        wu_puthex(note, notecap, &k, fn, 8);
        wu_puts(note, notecap, &k, ", dev=0x"); wu_puthex(note, notecap, &k, a[0], 4);
        wu_puts(note, notecap, &k, ", msg=0x"); wu_puthex(note, notecap, &k, a[1], 4);
        if (dir) wu_puts(note, notecap, &k, ", dirchange");
        wu_puts(note, notecap, &k, ")");
        if (!fn) { wu_puts(note, notecap, &k, " -- NULL procedure; 0");
                   wow32_setret(f, 0); return 1; }
        r = wow_gt_invoke(fn, a, 5);
        wu_puts(note, notecap, &k, " -> 0x"); wu_puthex(note, notecap, &k, r, 8);
        wow32_setret(f, r);
        return 1;
    }
    case WOWMM_YIELD:
        Sleep(1);
        wu_puts(note, notecap, &k, "mm yield (timeGetTime had not moved)");
        wow32_setret(f, 0);
        return 1;
    }
    return 0;
}

#endif /* WOWMMEDIA_H */
