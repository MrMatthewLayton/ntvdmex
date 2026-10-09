/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * SOUND.DRV's OWN ID SPACE.  GH #299, session 90.
 *
 * The eighth thunk table: the Windows 3.x voice-queue API (OpenSound, SetVoiceNote,
 * StartSound...), which WinMine imports six of. Its stubs are SOUND.DRV's seg1,
 * ids = export ordinals, recognised by the whole-table anchor in wowanchors.h.
 *
 * WHAT STOCK DOES, MEASURED, NOT REMEMBERED:
 * tests/probes/win16/w_sound asks all fifteen data-carrying exports, open and closed,
 * with AX poisoned to BEEF before each call. XP's own WOW answered **0 to every
 * one of the 23 cases** (build/wintest/w_sound.stock.txt, s90): OpenSound is 0,
 * a second OpenSound is 0, CountVoiceNotes after two queued notes is 0, an
 * out-of-range note is 0. NT never had the voice-queue hardware model behind this
 * API; its WOW accepts every call and plays nothing. So this table does the same,
 * on purpose, and SAYS SO in the log rather than leaving it to the step-over.
 *
 * [CAUTION]: BEFORE THIS TABLE the 23 cases already read 0 -- but as STEPPED-OVER calls of
 * "?'s table", whose 0 is the step-over's sentinel, not an answer. A probe that
 * agrees by accident is not a verified surface.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_WOWSOUND_H
#define NTVDMEX_WOWSOUND_H

#include "wow32.h"

#define WOWSND_OPENSOUND            0x0001
#define WOWSND_CLOSESOUND           0x0002
#define WOWSND_SETVOICEQUEUESIZE    0x0003
#define WOWSND_SETVOICENOTE         0x0004
#define WOWSND_SETVOICEACCENT       0x0005
#define WOWSND_SETVOICEENVELOPE     0x0006
#define WOWSND_SETSOUNDNOISE        0x0007
#define WOWSND_SETVOICESOUND        0x0008
#define WOWSND_STARTSOUND           0x0009
#define WOWSND_STOPSOUND            0x000a
#define WOWSND_WAITSOUNDSTATE       0x000b
#define WOWSND_SYNCALLVOICES        0x000c
#define WOWSND_COUNTVOICENOTES      0x000d
#define WOWSND_GETTHRESHOLDEVENT    0x000e
#define WOWSND_GETTHRESHOLDSTATUS   0x000f
#define WOWSND_SETVOICETHRESHOLD    0x0010
#define WOWSND_DOBEEP               0x0011

/* Defined in wowsound.c (#335). */
INT WowSoundCall(WOW32_FRAME *frame, PSTR note, INT noteCapacity);

#endif /* NTVDMEX_WOWSOUND_H */
