#ifndef NTVDMEX_WOWSOUND_H
#define NTVDMEX_WOWSOUND_H
/*
 * wowsound.h -- ★ SOUND.DRV's OWN ID SPACE.  GH #299, session 90.
 *
 * The eighth thunk table: the Windows 3.x voice-queue API (OpenSound, SetVoiceNote,
 * StartSound...), which WinMine imports six of. Its stubs are SOUND.DRV's seg1,
 * ids = export ordinals, recognised by the whole-table anchor in wowanchors.h.
 *
 * ── WHAT STOCK DOES, MEASURED, NOT REMEMBERED ───────────────────────────────
 * tests/probes/win16/w_sound asks all fifteen data-carrying exports, open and closed,
 * with AX poisoned to BEEF before each call. XP's own WOW answered **0 to every
 * one of the 23 cases** (build/wintest/w_sound.stock.txt, s90): OpenSound is 0,
 * a second OpenSound is 0, CountVoiceNotes after two queued notes is 0, an
 * out-of-range note is 0. NT never had the voice-queue hardware model behind this
 * API; its WOW accepts every call and plays nothing. So this table does the same,
 * on purpose, and SAYS SO in the log rather than leaving it to the step-over.
 * ⚠ BEFORE THIS TABLE the 23 cases already read 0 -- but as STEPPED-OVER calls of
 *   "?'s table", whose 0 is the step-over's sentinel, not an answer. A probe that
 *   agrees by accident is not a verified surface.
 */

#define WOWSND_OPENSOUND          0x0001
#define WOWSND_CLOSESOUND         0x0002
#define WOWSND_SETVOICEQUEUESIZE  0x0003
#define WOWSND_SETVOICENOTE       0x0004
#define WOWSND_SETVOICEACCENT     0x0005
#define WOWSND_SETVOICEENVELOPE   0x0006
#define WOWSND_SETSOUNDNOISE      0x0007
#define WOWSND_SETVOICESOUND      0x0008
#define WOWSND_STARTSOUND         0x0009
#define WOWSND_STOPSOUND          0x000a
#define WOWSND_WAITSOUNDSTATE     0x000b
#define WOWSND_SYNCALLVOICES      0x000c
#define WOWSND_COUNTVOICENOTES    0x000d
#define WOWSND_GETTHRESHOLDEVENT  0x000e
#define WOWSND_GETTHRESHOLDSTATUS 0x000f
#define WOWSND_SETVOICETHRESHOLD  0x0010
#define WOWSND_DOBEEP             0x0011

static INT WowSoundCall(WOW32_FRAME *frame, PSTR note, INT noteCapacity)
{
    static const PCSTR functionNames[] = {
        "?", "OpenSound", "CloseSound", "SetVoiceQueueSize", "SetVoiceNote",
        "SetVoiceAccent", "SetVoiceEnvelope", "SetSoundNoise", "SetVoiceSound",
        "StartSound", "StopSound", "WaitSoundState", "SyncAllVoices",
        "CountVoiceNotes", "GetThresholdEvent", "GetThresholdStatus",
        "SetVoiceThreshold", "DoBeep",
    };
    INT noteLength = 0;
    if (noteCapacity) note[0] = 0;
    switch (frame->Id) {
    case WOWSND_OPENSOUND:
    case WOWSND_CLOSESOUND:
    case WOWSND_SETVOICEQUEUESIZE:
    case WOWSND_SETVOICENOTE:
    case WOWSND_SETVOICEACCENT:
    case WOWSND_SETVOICEENVELOPE:
    case WOWSND_SETSOUNDNOISE:
    case WOWSND_SETVOICESOUND:
    case WOWSND_STARTSOUND:
    case WOWSND_STOPSOUND:
    case WOWSND_WAITSOUNDSTATE:
    case WOWSND_SYNCALLVOICES:
    case WOWSND_COUNTVOICENOTES:
    case WOWSND_GETTHRESHOLDSTATUS:
    case WOWSND_SETVOICETHRESHOLD:
    case WOWSND_DOBEEP:
        WowNotePut(note, noteCapacity, &noteLength, functionNames[frame->Id]);
        WowNotePut(note, noteCapacity, &noteLength, " -- 0, as stock's WOW answers every SOUND.DRV call"
                                   " (w_sound, 23/23)");
        Wow32SetReturn(frame, 0);
        return 1;
    /* ⚠ GetThresholdEvent returns an LPINT (DX:AX). Stock's answer for it is
         UNMEASURED -- the probe's OUT shows AX only -- so it is a NULL far pointer
         here, which is what a 0-everything WOW gives, and it is flagged. */
    case WOWSND_GETTHRESHOLDEVENT:
        WowNotePut(note, noteCapacity, &noteLength, "GetThresholdEvent -- NULL (DX not measured on stock)");
        Wow32SetReturn(frame, 0);
        return 1;
    }
    return 0;
}

#endif /* NTVDMEX_WOWSOUND_H */
