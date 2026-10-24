/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The SoundFont 2 synth: MIDI in, 16-bit stereo out, voices built from a parsed font
 * (sf2_font.h). Settings > Audio > MIDI = SoundFont with a file chosen plays through it, in our
 * mixer (audio_sf2.h is the host side).
 *
 * WRITTEN FROM THE SPECIFICATION ONLY (Importance = 3):
 * The synthesis model is section 9 of the public "SoundFont Technical Specification 2.04"
 * (E-mu / Creative, 2002) and every rule below cites its section. No other synth's source was
 * read (CLEAN-ROOM.md). Where the specification leaves a choice open (section 9.7 expects
 * "a best practical reproduction"), the comment says so and says what we chose.
 *
 * WHAT IT DOES:
 * - Section 9.4: preset and instrument zones, global zones, key / velocity ranges,
 *   instrument generators absolute and preset generators added.
 * - Section 9.5: modulators -- the ten defaults of section 8.4 and the file's own, with the
 *   section's replace / add rules. Linked modulators (section 9.5.4) are ignored.
 * - Section 9.1: the oscillator (linear interpolation, the three loop modes), the 2-pole
 *   resonant low-pass, both envelopes, both LFOs, attenuation and pan. No reverb or chorus:
 *   their sends are computed and go nowhere.
 * - Section 9.2: bank select, data entry for RPN 0-2, sustain, all sound / notes off, reset all
 *   controllers; the GM, GS and XG reset messages and GS's drum-part message.
 * - A fixed pool of voices; a note that finds none takes the quietest one, released ones first.
 *
 * COST, ON A PERIOD MACHINE:
 * Everything that is not per sample runs once per SF2_SYNTH_BLOCK_FRAMES (the control rate):
 * envelopes, LFOs, pitch, filter coefficients, gain. The per-sample loop is one interpolation,
 * an optional biquad and two multiply-adds, with no division and no allocation; gain is ramped
 * across each block so the steps are not heard. There is no heap at all: the synth is one
 * structure the caller owns.
 *
 * Single-threaded: the caller serialises messages and Render (audio_sf2.c does).
 * Pure C, no <windows.h>: tests/unit/sf2_test.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_SF2_SYNTH_H
#define NTVDMEX_SF2_SYNTH_H

#include "sf2_font.h"

#define SF2_SYNTH_VOICES                64
#define SF2_SYNTH_CHANNELS              16
#define SF2_SYNTH_BLOCK_FRAMES          32  /* The control rate: ~0.7 ms at 44.1 kHz */
#define SF2_SYNTH_VOICE_MODULATORS      32  /* The 10 defaults and the file's own, per voice */
#define SF2_SYNTH_CONTROLLERS           128
#define SF2_SYNTH_STEREO                2
#define SF2_SYNTH_PERCUSSION_CHANNEL    9   /* GM: MIDI channel 10 */

/* Section 8.4.10's destination "Initial Pitch" has no generator number, so it gets an index of
 * its own past the generators. No file modulator can reach it: its destination would be an
 * unknown generator, which section 10.3 ignores.
 */
#define SF2_DESTINATION_INITIAL_PITCH   SF2_GENERATOR_COUNT
#define SF2_DESTINATION_COUNT           (SF2_GENERATOR_COUNT + 1)

enum
{
    SF2_VOICE_FREE = 0, SF2_VOICE_ON, SF2_VOICE_RELEASED
};

/* Section 9.1.7: the six phases, and the end. */
enum
{
    SF2_STAGE_DELAY = 0, SF2_STAGE_ATTACK, SF2_STAGE_HOLD, SF2_STAGE_DECAY, SF2_STAGE_SUSTAIN,
    SF2_STAGE_RELEASE, SF2_STAGE_DONE
};

/* One envelope. Level is centibels of attenuation for the volume envelope (section 8.1.2,
 * gens 36-38: it moves in constant dB steps) and the 0..1 value for the modulation envelope.
 */
typedef struct _SF2_ENVELOPE
{
    INT   Stage;                    /* SF2_STAGE_* */
    DWORD BlocksLeft;               /* in delay or hold */
    FLOAT Progress;                 /* 0..1 through the attack */
    FLOAT Level;
    FLOAT AttackStep;               /* progress per block */
    DWORD HoldBlocks;
    FLOAT DecayStep;                /* level per block */
    FLOAT SustainLevel;
    FLOAT ReleaseStep;
} SF2_ENVELOPE, *PSF2_ENVELOPE;

typedef const SF2_ENVELOPE *PCSF2_ENVELOPE;

/* Section 9.1.6: a delay at zero, then a triangle from zero up to +1, down to -1, and up. */
typedef struct _SF2_LFO
{
    DWORD DelayBlocks;
    FLOAT Phase;                    /* 0..1 of a period */
    FLOAT PhaseStep;                /* per block */
    FLOAT Value;
} SF2_LFO, *PSF2_LFO;

typedef struct _SF2_VOICE
{
    BYTE  State;                    /* SF2_VOICE_* */
    BYTE  Channel;
    BYTE  Key;                      /* as the note-on sent it: what a note-off matches */
    BYTE  Velocity;
    BYTE  EffectiveKey;             /* after the keynum generator (section 8.1.2, gen 46) */
    BYTE  EffectiveVelocity;        /* after the velocity generator (gen 47) */
    BYTE  LoopMode;                 /* sampleModes & 3 (gen 54) */
    BOOL  IsSustained;              /* its note-off arrived while CC64 was down */
    DWORD NoteId;                   /* shared by every voice one note-on started */
    DWORD SampleIndex;
    WORD  SampleType;               /* SF2_SAMPLE_* */
    WORD  SampleLink;               /* a stereo sample's partner (section 7.10) */
    INT   ExclusiveClass;
    INT   PitchLeader;              /* a left sample's right partner (section 7.10), or -1 */
    SHORT Generators[SF2_GENERATOR_COUNT];      /* instrument + preset, section 9.4 */
    SF2_MODULATOR Modulators[SF2_SYNTH_VOICE_MODULATORS];
    DWORD ModulatorCount;
    FLOAT Modulation[SF2_DESTINATION_COUNT];    /* each destination's summing node */
    DWORD ModulationSerial;         /* the channel serial Modulation was computed at */

    /* the oscillator: positions are absolute sample data points (section 9.3) */
    DWORD Start;
    DWORD End;
    DWORD LoopStart;
    DWORD LoopEnd;
    DWORD Position;
    DWORD Fraction;                 /* 0.32 fixed point between Position and the next point */
    DWORD StepWhole;
    DWORD StepFraction;
    double KeyPitchCents;           /* scaleTuning x (key - root) + the sample's correction */
    double RateRatio;               /* sample rate / output rate */

    SF2_ENVELOPE VolumeEnvelope;
    SF2_ENVELOPE ModulationEnvelope;
    SF2_LFO ModulationLfo;
    SF2_LFO VibratoLfo;

    /* the amplifier: ramped across each control block */
    FLOAT GainLeft;
    FLOAT GainRight;
    FLOAT GainStepLeft;
    FLOAT GainStepRight;
    FLOAT Amplitude;                /* the last block's target, for choosing a voice to steal */

    /* the filter (section 9.1.3): a biquad, direct form I */
    BOOL  IsFiltered;
    FLOAT FilterCutoffCents;
    FLOAT FilterQ;
    FLOAT FilterB0;
    FLOAT FilterB1;
    FLOAT FilterB2;
    FLOAT FilterA1;
    FLOAT FilterA2;
    FLOAT FilterX1;
    FLOAT FilterX2;
    FLOAT FilterY1;
    FLOAT FilterY2;
} SF2_VOICE, *PSF2_VOICE;

typedef const SF2_VOICE *PCSF2_VOICE;

typedef struct _SF2_CHANNEL
{
    BYTE  Program;
    BYTE  BankSelect;               /* CC0, applied at the next program change (section 9.2) */
    BYTE  Bank;                     /* the bank the current program was chosen in */
    BYTE  ChannelPressure;
    BYTE  Controllers[SF2_SYNTH_CONTROLLERS];
    BYTE  PolyPressure[SF2_MIDI_VALUES];
    WORD  PitchBend;                /* 14-bit, 8192 = centre */
    WORD  RegisteredParameter;      /* CC101 << 7 | CC100 */
    BOOL  IsNonRegistered;          /* CC98/99 came last: data entry is not for an RPN */
    BOOL  IsPercussion;
    SHORT BendRangeCents;           /* RPN 0 */
    SHORT FineTuneCents;            /* RPN 1 */
    SHORT CoarseTuneSemitones;      /* RPN 2 */
    DWORD PresetIndex;              /* SF2_NONE = no preset for this bank / program */
    DWORD Serial;                   /* bumped by every change a modulator can see */
} SF2_CHANNEL, *PSF2_CHANNEL;

typedef const SF2_CHANNEL *PCSF2_CHANNEL;

typedef struct _SF2_SYNTH_STATS
{
    DWORD NotesOn;                  /* note-ons received */
    DWORD NotesSilent;              /* ...that found no preset or no zone */
    DWORD VoicesStarted;
    DWORD VoicesStolen;
    DWORD VoicesPeak;
    DWORD Resets;                   /* GM / GS / XG reset messages */
} SF2_SYNTH_STATS, *PSF2_SYNTH_STATS;

typedef const SF2_SYNTH_STATS *PCSF2_SYNTH_STATS;

typedef struct _SF2_SYNTH
{
    PCSF2_FONT Font;
    DWORD OutputHz;
    double BlockSeconds;
    FLOAT MasterGain;               /* universal SysEx master volume */
    DWORD FramesUntilControl;
    DWORD NextNoteId;
    DWORD ActiveVoices;
    SF2_CHANNEL Channels[SF2_SYNTH_CHANNELS];
    SF2_VOICE Voices[SF2_SYNTH_VOICES];
    FLOAT Mix[SF2_SYNTH_BLOCK_FRAMES * SF2_SYNTH_STEREO];
    SF2_SYNTH_STATS Stats;
} SF2_SYNTH, *PSF2_SYNTH;

typedef const SF2_SYNTH *PCSF2_SYNTH;

/* Ready to play `font` at `outputHz`: every channel at its power-on state, no voices. */
VOID Sf2SynthInitialize(_Out_ PSF2_SYNTH synth, _In_ PCSF2_FONT font, _In_ DWORD outputHz);

/* One short message: status | data1 << 8 | data2 << 16 (as the MPU-401 packs them). */
VOID Sf2SynthShortMessage(_Inout_ PSF2_SYNTH synth, _In_ DWORD message);

/* One whole SysEx message, F0 .. F7. */
VOID Sf2SynthSysEx(_Inout_ PSF2_SYNTH synth, _In_reads_(length) PCBYTE message, _In_ DWORD length);

/* GM System On: every voice stopped, every channel at its power-on state. */
VOID Sf2SynthReset(_Inout_ PSF2_SYNTH synth);

/* Render `frames` interleaved stereo frames into `output` (overwritten). FALSE = no voice is
 * sounding: nothing written, and nothing to mix.
 */
BOOL Sf2SynthRender(
    _Inout_ PSF2_SYNTH synth,
    _Out_writes_(frames * 2) PINT16 output,
    _In_ DWORD frames);

/* The playback rate a voice steps through its sample at, in sample points per output frame. */
double Sf2VoicePlaybackRate(_In_ PCSF2_VOICE voice);

/* A generator's value for a voice: instrument + preset + modulators, clamped to the range of
 * section 8.1.3.
 */
double Sf2VoiceParameter(_In_ PCSF2_VOICE voice, _In_ INT generator);

/* The volume envelope's gain, 0..1 (attack is linear in amplitude, the rest moves in dB). */
double Sf2EnvelopeVolumeGain(_In_ PCSF2_ENVELOPE envelope);

#endif /* NTVDMEX_SF2_SYNTH_H */
