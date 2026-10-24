/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * A SoundFont 2 file, read in place: the RIFF `sfbk` form, checked, and the hydra's nine
 * tables reached through it. (The SoundFont synth: sf2_synth.h.)
 *
 * WRITTEN FROM THE SPECIFICATION ONLY (Importance = 3):
 * Every rule here is from the public "SoundFont Technical Specification 2.04" (E-mu /
 * Creative, 2002), cited by section. No other synth's source was read. NTVDMEX is MIT and
 * clean-room (CLEAN-ROOM.md).
 *
 * THE FILE IS NEVER COPIED:
 * A SoundFont can be most of a 32-bit address space's free memory (the user's opl3-fm128.sf2 is
 * 135 MB), so the host maps it and hands this module a pointer and a length. The parse checks
 * the structure once (section 10.1: structural errors are found at load time and the file is
 * rejected), and from then on records are read straight out of the image, byte by byte: the
 * hydra's records are 38, 22 and 46 bytes long and are not aligned.
 *
 * Pure C, no <windows.h>: tests/unit/sf2_test.c builds fonts in memory and garbles them.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_SF2_FONT_H
#define NTVDMEX_SF2_FONT_H

#include "../ntvdmex_types.h"

/* Record sizes, section 7.2-7.10. */
#define SF2_PRESET_RECORD_BYTES         38
#define SF2_BAG_RECORD_BYTES            4
#define SF2_MODULATOR_RECORD_BYTES      10
#define SF2_GENERATOR_RECORD_BYTES      4
#define SF2_INSTRUMENT_RECORD_BYTES     22
#define SF2_SAMPLE_RECORD_BYTES         46
#define SF2_NAME_CHARS                  20              /* achPresetName / achInstName / achSampleName */
#define SF2_BANK_NAME_CHARS             64              /* How much of INAM the report keeps */

#define SF2_NONE                        0xFFFFFFFFu     /* No preset, no zone */
#define SF2_PERCUSSION_BANK             128             /* Section 7.2: the GM percussion bank */
#define SF2_MIDI_VALUES                 128             /* 0..127 */
#define SF2_COARSE_OFFSET_POINTS        32768           /* Section 8.1.2, gens 4/12/45/50 */

/* sfSampleType, section 7.10. */
#define SF2_SAMPLE_MONO                 0x0001
#define SF2_SAMPLE_RIGHT                0x0002
#define SF2_SAMPLE_LEFT                 0x0004
#define SF2_SAMPLE_LINKED               0x0008
#define SF2_SAMPLE_ROM                  0x8000

/* The generators, section 8.1.2. Numbered by the specification; the gaps are its unused ones. */
enum
{
    SF2_GEN_START_OFFSET = 0, SF2_GEN_END_OFFSET = 1, SF2_GEN_START_LOOP_OFFSET = 2,
    SF2_GEN_END_LOOP_OFFSET = 3, SF2_GEN_START_COARSE_OFFSET = 4, SF2_GEN_MOD_LFO_TO_PITCH = 5,
    SF2_GEN_VIB_LFO_TO_PITCH = 6, SF2_GEN_MOD_ENV_TO_PITCH = 7, SF2_GEN_FILTER_FC = 8,
    SF2_GEN_FILTER_Q = 9, SF2_GEN_MOD_LFO_TO_FILTER_FC = 10, SF2_GEN_MOD_ENV_TO_FILTER_FC = 11,
    SF2_GEN_END_COARSE_OFFSET = 12, SF2_GEN_MOD_LFO_TO_VOLUME = 13, SF2_GEN_CHORUS_SEND = 15,
    SF2_GEN_REVERB_SEND = 16, SF2_GEN_PAN = 17, SF2_GEN_MOD_LFO_DELAY = 21,
    SF2_GEN_MOD_LFO_FREQUENCY = 22, SF2_GEN_VIB_LFO_DELAY = 23, SF2_GEN_VIB_LFO_FREQUENCY = 24,
    SF2_GEN_MOD_ENV_DELAY = 25, SF2_GEN_MOD_ENV_ATTACK = 26, SF2_GEN_MOD_ENV_HOLD = 27,
    SF2_GEN_MOD_ENV_DECAY = 28, SF2_GEN_MOD_ENV_SUSTAIN = 29, SF2_GEN_MOD_ENV_RELEASE = 30,
    SF2_GEN_KEY_TO_MOD_ENV_HOLD = 31, SF2_GEN_KEY_TO_MOD_ENV_DECAY = 32,
    SF2_GEN_VOL_ENV_DELAY = 33, SF2_GEN_VOL_ENV_ATTACK = 34, SF2_GEN_VOL_ENV_HOLD = 35,
    SF2_GEN_VOL_ENV_DECAY = 36, SF2_GEN_VOL_ENV_SUSTAIN = 37, SF2_GEN_VOL_ENV_RELEASE = 38,
    SF2_GEN_KEY_TO_VOL_ENV_HOLD = 39, SF2_GEN_KEY_TO_VOL_ENV_DECAY = 40, SF2_GEN_INSTRUMENT = 41,
    SF2_GEN_KEY_RANGE = 43, SF2_GEN_VELOCITY_RANGE = 44, SF2_GEN_START_LOOP_COARSE_OFFSET = 45,
    SF2_GEN_KEYNUM = 46, SF2_GEN_VELOCITY = 47, SF2_GEN_INITIAL_ATTENUATION = 48,
    SF2_GEN_END_LOOP_COARSE_OFFSET = 50, SF2_GEN_COARSE_TUNE = 51, SF2_GEN_FINE_TUNE = 52,
    SF2_GEN_SAMPLE_ID = 53, SF2_GEN_SAMPLE_MODES = 54, SF2_GEN_SCALE_TUNING = 56,
    SF2_GEN_EXCLUSIVE_CLASS = 57, SF2_GEN_OVERRIDING_ROOT_KEY = 58, SF2_GEN_END_OPERATOR = 60,
    SF2_GENERATOR_COUNT = 61
};

/* Why a file was refused. Sf2FontErrorText names each for the log. */
typedef enum _SF2_PARSE_RESULT
{
    SF2_PARSE_OK = 0,
    SF2_PARSE_NOT_RIFF,         /* no "RIFF" .. "sfbk" form at the start */
    SF2_PARSE_TRUNCATED,        /* a chunk runs past the end of the file or of its parent */
    SF2_PARSE_MISSING_CHUNK,    /* INFO / sdta / pdta, or one of the nine hydra tables */
    SF2_PARSE_NO_VERSION,       /* section 5.1: no ifil, or not four bytes */
    SF2_PARSE_BAD_VERSION,      /* not a 2.x file */
    SF2_PARSE_NO_SAMPLES,       /* no smpl: the samples would be ROM, which no PC has */
    SF2_PARSE_BAD_TABLE_SIZE,   /* not whole records, or no terminal */
    SF2_PARSE_BAD_INDEX,        /* an index out of order or range */
    SF2_PARSE_UNEXPECTED_CHUNK  /* section 10.2: an unknown chunk outside INFO */
} SF2_PARSE_RESULT;

/* One hydra table: its records, terminal record included (section 7.2-7.10). */
typedef struct _SF2_TABLE
{
    PCBYTE Records;
    DWORD  Count;
} SF2_TABLE, *PSF2_TABLE;

typedef const SF2_TABLE *PCSF2_TABLE;

typedef struct _SF2_FONT
{
    PCBYTE   Image;
    DWORD    ImageLength;
    WORD     VersionMajor;          /* ifil */
    WORD     VersionMinor;
    CHAR     BankName[SF2_BANK_NAME_CHARS];     /* INAM, or empty */
    const INT16 *Samples;           /* smpl: 16-bit little-endian, in the image itself */
    DWORD    SampleCount;           /* sample data points */
    SF2_TABLE Presets;              /* phdr */
    SF2_TABLE PresetBags;           /* pbag */
    SF2_TABLE PresetModulators;     /* pmod */
    SF2_TABLE PresetGenerators;     /* pgen */
    SF2_TABLE Instruments;          /* inst */
    SF2_TABLE InstrumentBags;       /* ibag */
    SF2_TABLE InstrumentModulators; /* imod */
    SF2_TABLE InstrumentGenerators; /* igen */
    SF2_TABLE SampleHeaders;        /* shdr */
} SF2_FONT, *PSF2_FONT;

typedef const SF2_FONT *PCSF2_FONT;

typedef struct _SF2_PRESET_HEADER
{
    WORD  Program;                  /* wPreset */
    WORD  Bank;                     /* wBank */
    DWORD FirstBag;                 /* wPresetBagNdx */
    DWORD EndBag;                   /* the next preset's wPresetBagNdx */
} SF2_PRESET_HEADER, *PSF2_PRESET_HEADER;

typedef struct _SF2_GENERATOR
{
    WORD  Operator;                 /* sfGenOper */
    SHORT Amount;                   /* genAmount, as shAmount; ranges are byLo | byHi << 8 */
} SF2_GENERATOR, *PSF2_GENERATOR;

typedef struct _SF2_MODULATOR
{
    WORD  Source;                   /* sfModSrcOper */
    WORD  Destination;              /* sfModDestOper */
    SHORT Amount;                   /* modAmount */
    WORD  AmountSource;             /* sfModAmtSrcOper */
    WORD  Transform;                /* sfModTransOper */
} SF2_MODULATOR, *PSF2_MODULATOR;

typedef const SF2_MODULATOR *PCSF2_MODULATOR;

typedef struct _SF2_SAMPLE_HEADER
{
    DWORD Start;
    DWORD End;
    DWORD StartLoop;
    DWORD EndLoop;
    DWORD SampleRate;
    BYTE  OriginalPitch;
    INT8  PitchCorrection;          /* cents */
    WORD  SampleLink;
    WORD  SampleType;               /* SF2_SAMPLE_* */
} SF2_SAMPLE_HEADER, *PSF2_SAMPLE_HEADER;

/* One preset or instrument zone, as section 7.3 / 7.7 define it. */
typedef struct _SF2_ZONE
{
    DWORD GeneratorFirst;           /* the zone's generators, the terminal one excluded */
    DWORD GeneratorEnd;
    DWORD ModulatorFirst;
    DWORD ModulatorEnd;
    BOOL  HasTerminal;              /* ends in Instrument (preset) / sampleID (instrument) */
    WORD  TerminalIndex;            /* that generator's instrument or sample index */
} SF2_ZONE, *PSF2_ZONE;

/* Check the whole file and fill `font`; nothing is copied, so `image` must outlive it. */
SF2_PARSE_RESULT Sf2FontParse(
    _Out_ PSF2_FONT font,
    _In_reads_bytes_(imageLength) PCBYTE image,
    _In_ DWORD imageLength);

/* The parse result as a short phrase for the log. */
PCSTR Sf2FontErrorText(_In_ SF2_PARSE_RESULT result);

/* Presets / instruments / samples, terminal records not counted. */
DWORD Sf2FontPresetCount(_In_ PCSF2_FONT font);
DWORD Sf2FontInstrumentCount(_In_ PCSF2_FONT font);
DWORD Sf2FontSampleCount(_In_ PCSF2_FONT font);

VOID Sf2FontReadPreset(
    _In_ PCSF2_FONT font,
    _In_ DWORD presetIndex,
    _Out_ PSF2_PRESET_HEADER preset);

/* The first preset with this bank and program (section 7.2: the first is the active one), or
 * SF2_NONE.
 */
DWORD Sf2FontFindPreset(_In_ PCSF2_FONT font, _In_ WORD bank, _In_ WORD program);

/* An instrument's bag range [first, end). */
VOID Sf2FontInstrumentBags(
    _In_ PCSF2_FONT font,
    _In_ DWORD instrumentIndex,
    _Out_ PDWORD firstBag,
    _Out_ PDWORD endBag);

/* The zone at a preset bag (isPreset) or an instrument bag. */
VOID Sf2FontReadZone(
    _In_ PCSF2_FONT font,
    _In_ BOOL isPreset,
    _In_ DWORD bagIndex,
    _Out_ PSF2_ZONE zone);

VOID Sf2FontReadGenerator(
    _In_ PCSF2_TABLE generators,
    _In_ DWORD generatorIndex,
    _Out_ PSF2_GENERATOR generator);

VOID Sf2FontReadModulator(
    _In_ PCSF2_TABLE modulators,
    _In_ DWORD modulatorIndex,
    _Out_ PSF2_MODULATOR modulator);

VOID Sf2FontReadSampleHeader(
    _In_ PCSF2_FONT font,
    _In_ DWORD sampleIndex,
    _Out_ PSF2_SAMPLE_HEADER header);

#endif /* NTVDMEX_SF2_FONT_H */
