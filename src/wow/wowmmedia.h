/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * MMSYSTEM.DLL's OWN ID SPACE (WOW32's "MMEDIA" table).  #278, s90.
 *
 * XP's 16-bit MMSYSTEM reaches 32-bit WINMM almost entirely through the GENERIC
 * thunks (LoadLibraryEx32W("winmm.dll"), GetProcAddress32W for a dozen entry points)
 * and then calls those entry points through exactly TWO WOW ids of its own (its
 * thunk table has two stubs; ids additive on KERNEL.581 __MOD_MMEDIA = 0):
 * id 2, 28 argument bytes      id 1, no arguments
 *
 * id 2: mmCallProc32(uDevId, uMsg, dwInst, dwP1, dwP2, lpProc32, fDirChange):
 * Seven DWORDs, PASCAL, so reversed in the frame: +0 fDirChange, +4 the 32-bit
 * procedure, +8 dwP2, +12 dwP1, +16 dwInst, +20 uMsg, +24 uDevId -- the shape
 * the frames for NotifyCallbackData arrive in (s90). The procedure is a WINMM
 * stdcall entry taking the first five; fDirChange asks for the 32-bit current
 * directory to follow the 16-bit task's first (MCI opens files by relative name)
 * -- this host runs the guest in its own directory already, so it is logged and
 * not acted on.
 * -- id 1: no arguments; it arrives from timeGetTime when the 32-bit clock has not
 * moved since the last read -- a yield so that it can. ----------------------------
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_WOWMMEDIA_H
#define NTVDMEX_WOWMMEDIA_H

#include "wow32.h"

#define WOWMM_YIELD                     0x0001
#define WOWMM_CALLPROC32                0x0002

/* mmCallProc32's frame, reversed (PASCAL), and the five arguments the procedure takes. */
#define WOWMM_CALLPROC32_ARG_DIRCHANGE  0
#define WOWMM_CALLPROC32_ARG_PROCEDURE  4
#define WOWMM_CALLPROC32_ARG_PARAM2     8
#define WOWMM_CALLPROC32_ARG_PARAM1     12
#define WOWMM_CALLPROC32_ARG_INSTANCE   16
#define WOWMM_CALLPROC32_ARG_MESSAGE    20
#define WOWMM_CALLPROC32_ARG_DEVICE     24
#define WOWMM_CALLPROC32_ARGUMENTS      5

/* Defined in wowmmedia.c (#335). */
INT WowMultimediaCall(WOW32_FRAME *frame, PSTR note, INT noteCapacity);

#endif /* NTVDMEX_WOWMMEDIA_H */
