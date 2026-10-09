/* wowsound.c -- ★ SOUND.DRV's OWN ID SPACE.  GH #299, session 90.
 *
 * The code of wowsound.h (#335): its functions and state, in their original order. Part of
 * the host's single translation unit: #included by main.c straight after wowsound.h. */

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
