; w_sound.asm -- SOUND.DRV, the Windows 3.x voice-queue API (#299, s90).
;
; WinMine imports six of these (OpenSound, CloseSound, SetVoiceNote, SetVoiceAccent,
; StartSound, StopSound) and the host stepped over all six, so WinMine read whatever
; was in AX. This asks every export the questions whose answers stock can be compared
; on: the open/close pairing, a second open while open, the queue counts, and what the
; calls answer once the device is closed again.
; ⚠ AX IS POISONED (BEEF) before every call, so a call that leaves AX alone reads BEEF
;   rather than a plausible leftover.
; Ordinals: SOUND.DRV's entry table (19 FIXED exports); names from the Windows 3.1 SDK.

        org     0
%include "w16.inc"
W16_HEAD 'SOUND'
        IMP     X, 1,  OPENSOUND
        IMP     X, 2,  CLOSESOUND
        IMP     X, 3,  SETVOICEQUEUESIZE
        IMP     X, 4,  SETVOICENOTE
        IMP     X, 5,  SETVOICEACCENT
        IMP     X, 6,  SETVOICEENVELOPE
        IMP     X, 7,  SETSOUNDNOISE
        IMP     X, 8,  SETVOICESOUND
        IMP     X, 9,  STARTSOUND
        IMP     X, 10, STOPSOUND
        IMP     X, 11, WAITSOUNDSTATE
        IMP     X, 12, SYNCALLVOICES
        IMP     X, 13, COUNTVOICENOTES
        IMP     X, 15, GETTHRESHOLDSTATUS
        IMP     X, 16, SETVOICETHRESHOLD
W16_IAT

%macro P 1
        mov     ax, %1
        push    ax
%endmacro
%macro POISON 0
        mov     ax, 0BEEFh
%endmacro

cases:
        POISON
        API     OPENSOUND
        OUT     "snd.open"
        POISON
        API     OPENSOUND
        OUT     "snd.open.again"
        P 1
        P 256
        POISON
        API     SETVOICEQUEUESIZE
        OUT     "snd.qsize"
        P 1
        P 120                           ; tempo
        P 128                           ; volume
        P 0                             ; S_NORMAL
        P 0                             ; pitch
        POISON
        API     SETVOICEACCENT
        OUT     "snd.accent"
        P 1
        P 37                            ; note: middle C
        P 4                             ; quarter note
        P 0                             ; no dots
        POISON
        API     SETVOICENOTE
        OUT     "snd.note1"
        P 1
        P 0                             ; a rest
        P 16
        P 0
        POISON
        API     SETVOICENOTE
        OUT     "snd.note.rest"
        P 1
        POISON
        API     COUNTVOICENOTES
        OUT     "snd.count2"
        P 1
        P 85                            ; note out of range (1..84)
        P 4
        P 0
        POISON
        API     SETVOICENOTE
        OUT     "snd.note.bad"
        P 2                             ; voice 2: does not exist
        P 37
        P 4
        P 0
        POISON
        API     SETVOICENOTE
        OUT     "snd.note.voice2"
        P 1
        P 0                             ; dwFrequency = 440.0 (16.16) -> hi word
        P 440
        P 0
        P 2                             ; duration in clock ticks
        POISON
        API     SETVOICESOUND
        OUT     "snd.vsound"
        P 1
        P 0
        POISON
        API     SETVOICEENVELOPE
        OUT     "snd.envelope"
        P 0                             ; source S_PERIOD512
        P 0                             ; duration
        POISON
        API     SETSOUNDNOISE
        OUT     "snd.noise"
        P 1
        P 1
        POISON
        API     SETVOICETHRESHOLD
        OUT     "snd.thresh"
        POISON
        API     GETTHRESHOLDSTATUS
        OUT     "snd.thstatus"
        POISON
        API     SYNCALLVOICES
        OUT     "snd.sync"
        POISON
        API     STARTSOUND
        OUT     "snd.start"
        P 0                             ; S_QUEUEEMPTY
        POISON
        API     WAITSOUNDSTATE
        OUT     "snd.wait"
        P 1
        POISON
        API     COUNTVOICENOTES
        OUT     "snd.count.after"
        POISON
        API     STOPSOUND
        OUT     "snd.stop"
        POISON
        API     CLOSESOUND
        OUT     "snd.close"
        P 1
        P 37
        P 4
        P 0
        POISON
        API     SETVOICENOTE
        OUT     "snd.note.closed"
        P 1
        POISON
        API     COUNTVOICENOTES
        OUT     "snd.count.closed"
        POISON
        API     OPENSOUND
        OUT     "snd.reopen"
        POISON
        API     CLOSESOUND
        jmp     w16_fin

W16_TAIL "w16sound"
