/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for the SoundFont 2 synth (src/vdd/sf2_font.c, src/vdd/sf2_synth.c).
 *
 * A small SoundFont is built in memory, field by field from the SoundFont 2.04 specification's
 * record layouts (section 7), and then:
 *   - parsed, and refused when truncated at every length, garbled in each structural way
 *     section 7 names, or fuzzed (refused or played: never a crash);
 *   - played, checking zone selection by key and velocity, generator merging (section 8.5's own
 *     attackVolEnv example), playback rate for a known root key and tuning, pitch bend with
 *     RPN 0, each envelope stage against the clock, the three loop modes, release freeing the
 *     voice, percussion on channel 10, exclusive class, voice stealing, the pedals, saturation,
 *     stereo pairs and the GM / GS reset messages.
 * Expected values are literal (house rule): each comment says where the number comes from.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "sf2_synth.h"
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static INT g_Total = 0;
static INT g_Failures = 0;

/* the builder: */

static BYTE g_Phdr[1024];
static BYTE g_Pbag[1024];
static BYTE g_Pmod[1024];
static BYTE g_Pgen[1024];
static BYTE g_Inst[1024];
static BYTE g_Ibag[1024];
static BYTE g_Imod[1024];
static BYTE g_Igen[1024];
static BYTE g_Shdr[1024];
static DWORD g_PhdrLength;
static DWORD g_PbagLength;
static DWORD g_PmodLength;
static DWORD g_PgenLength;
static DWORD g_InstLength;
static DWORD g_IbagLength;
static DWORD g_ImodLength;
static DWORD g_IgenLength;
static DWORD g_ShdrLength;
static INT16 g_Samples[4096];
static DWORD g_SampleCount;

static BYTE g_Image[32768];
static DWORD g_ImageLength;
static DWORD g_PbagOffset;      /* where the pbag data landed in g_Image */
static DWORD g_PgenOffset;
static DWORD g_HydraOffset;     /* where the phdr chunk starts: the hydra runs to the end */
static DWORD g_IfilOffset;

/* playing: */

static SF2_FONT g_Font;
static SF2_SYNTH g_Synth;
static INT16 g_Output[44100 * 2];

static VOID Put16(PBYTE buffer, PDWORD length, DWORD value)
{
    buffer[(*length)++] = (BYTE)value;
    buffer[(*length)++] = (BYTE)(value >> 8);
}

static VOID Put32(PBYTE buffer, PDWORD length, DWORD value)
{
    Put16(buffer, length, value & 0xFFFF);
    Put16(buffer, length, value >> 16);
}

static VOID PutName(PBYTE buffer, PDWORD length, PCSTR name)
{
    DWORD character;

    for (character = 0; character < 20; ++character)
        buffer[(*length)++] = (BYTE)(character < strlen(name) ? name[character] : 0);
}

static VOID Preset(PCSTR name, WORD program, WORD bank)
{
    PutName(g_Phdr, &g_PhdrLength, name);
    Put16(g_Phdr, &g_PhdrLength, program);
    Put16(g_Phdr, &g_PhdrLength, bank);
    Put16(g_Phdr, &g_PhdrLength, g_PbagLength / 4);
    Put32(g_Phdr, &g_PhdrLength, 0);
    Put32(g_Phdr, &g_PhdrLength, 0);
    Put32(g_Phdr, &g_PhdrLength, 0);
}

static VOID PresetZone(VOID)
{
    Put16(g_Pbag, &g_PbagLength, g_PgenLength / 4);
    Put16(g_Pbag, &g_PbagLength, g_PmodLength / 10);
}

static VOID PresetGen(WORD generator, WORD amount)
{
    Put16(g_Pgen, &g_PgenLength, generator);
    Put16(g_Pgen, &g_PgenLength, amount);
}

static VOID Instrument(PCSTR name)
{
    PutName(g_Inst, &g_InstLength, name);
    Put16(g_Inst, &g_InstLength, g_IbagLength / 4);
}

static VOID InstrumentZone(VOID)
{
    Put16(g_Ibag, &g_IbagLength, g_IgenLength / 4);
    Put16(g_Ibag, &g_IbagLength, g_ImodLength / 10);
}

static VOID InstrumentGen(WORD generator, WORD amount)
{
    Put16(g_Igen, &g_IgenLength, generator);
    Put16(g_Igen, &g_IgenLength, amount);
}

static VOID InstrumentMod(
    WORD source,
    WORD destination,
    SHORT amount,
    WORD amountSource,
    WORD transform)
{
    Put16(g_Imod, &g_ImodLength, source);
    Put16(g_Imod, &g_ImodLength, destination);
    Put16(g_Imod, &g_ImodLength, (WORD)amount);
    Put16(g_Imod, &g_ImodLength, amountSource);
    Put16(g_Imod, &g_ImodLength, transform);
}

/* A sample of `points` copies of `value`, then the 46 zero points of section 6.1. */
static DWORD SampleData(INT16 value, DWORD points)
{
    DWORD start = g_SampleCount;
    DWORD point;

    for (point = 0; point < points; ++point)
        g_Samples[g_SampleCount++] = value;

    for (point = 0; point < 46; ++point)
        g_Samples[g_SampleCount++] = 0;

    return start;
}

static VOID Sample(
    PCSTR name,
    DWORD start,
    DWORD points,
    DWORD rate,
    BYTE pitch,
    WORD link,
    WORD type)
{
    PutName(g_Shdr, &g_ShdrLength, name);
    Put32(g_Shdr, &g_ShdrLength, start);
    Put32(g_Shdr, &g_ShdrLength, start + points);
    Put32(g_Shdr, &g_ShdrLength, start + 200);     /* loop 200 .. 800 */
    Put32(g_Shdr, &g_ShdrLength, start + 800);
    Put32(g_Shdr, &g_ShdrLength, rate);
    g_Shdr[g_ShdrLength++] = pitch;
    g_Shdr[g_ShdrLength++] = 0;
    Put16(g_Shdr, &g_ShdrLength, link);
    Put16(g_Shdr, &g_ShdrLength, type);
}

static VOID PutChunk(PCSTR id, PCBYTE data, DWORD length)
{
    memcpy(g_Image + g_ImageLength, id, 4);
    g_ImageLength += 4;
    Put32(g_Image, &g_ImageLength, length);
    memcpy(g_Image + g_ImageLength, data, length);
    g_ImageLength += length;

    if (length & 1)
        g_Image[g_ImageLength++] = 0;
}

static DWORD BeginList(PCSTR id, PCSTR type)
{
    DWORD sizeAt;

    memcpy(g_Image + g_ImageLength, id, 4);
    g_ImageLength += 4;
    sizeAt = g_ImageLength;
    g_ImageLength += 4;
    memcpy(g_Image + g_ImageLength, type, 4);
    g_ImageLength += 4;
    return sizeAt;
}

static VOID EndList(DWORD sizeAt)
{
    DWORD size = g_ImageLength - sizeAt - 4;
    DWORD at = sizeAt;

    Put32(g_Image, &at, size);
}

/* THE TEST FONT:
 * samples  0 "dc"     1000 points of 10000, 22050 Hz, root 60      (loop 200..800)
 *          1 "dc44"   1000 points of -8000, 44100 Hz, root 72
 *          2 "full"   1000 points of 32767, 44100 Hz, root 60
 *          3 "left"   = 0's data, 44100 Hz, left, linked to 4
 *          4 "right"  = 0's data, 22050 Hz, right, linked to 3
 * inst 0 "split"  global: attenuation 50 | 0-59 -> s0 loop | 60-127 vel 0-63 -> s1, attack
 *                 1200, attenuation 0 | 60-127 vel 64-127 -> s2
 * inst 1 "env"    s0 looped, attack 0 (1 s), decay 0 (1 s / 1000 cB), sustain 200 cB, release 0
 * inst 2 "tune"   s1, root overridden to 60, coarse -12
 * inst 3 "drums"  35-38 and 42-46 -> s0, one-shot, exclusive class 1
 * inst 4 "shot"   s0, no loop
 * inst 5 "mode3"  s0, loop until release, release 8000 tc
 * inst 6 "full"   s2 looped
 * inst 7 "stereo" s3 pan -500, s4 pan 500
 * presets: 0:0 "split" (global zone attack +2400, root key 70 -- illegal here), 0:1 "env",
 *          0:2 "tune" (fine +10), 0:3 "shot", 0:4 "mode3", 0:5 "full", 0:6 "stereo",
 *          128:0 "drums"
 */
static VOID BuildFont(VOID)
{
    DWORD dc;
    DWORD dc44;
    DWORD full;
    DWORD sizeAt;
    DWORD formAt;
    BYTE version[4] = { 2, 0, 4, 0 };

    g_PhdrLength = g_PbagLength = g_PmodLength = g_PgenLength = 0;
    g_InstLength = g_IbagLength = g_ImodLength = g_IgenLength = g_ShdrLength = 0;
    g_SampleCount = 0;
    g_ImageLength = 0;

    dc = SampleData(10000, 1000);
    dc44 = SampleData(-8000, 1000);
    full = SampleData(32767, 1000);
    Sample("dc", dc, 1000, 22050, 60, 0, 1);
    Sample("dc44", dc44, 1000, 44100, 72, 0, 1);
    Sample("full", full, 1000, 44100, 60, 0, 1);
    Sample("left", dc, 1000, 44100, 60, 4, 4);
    Sample("right", dc, 1000, 22050, 60, 3, 2);
    Sample("EOS", 0, 0, 0, 0, 0, 0);

    Instrument("split");
    InstrumentZone();
    InstrumentGen(48, 50);
    InstrumentZone();
    InstrumentGen(43, 0 | 59 << 8);
    InstrumentGen(54, 1);
    InstrumentGen(53, 0);
    InstrumentZone();
    InstrumentGen(43, 60 | 127 << 8);
    InstrumentGen(44, 0 | 63 << 8);
    InstrumentGen(34, 1200);
    InstrumentGen(48, 0);
    InstrumentGen(53, 1);
    InstrumentZone();
    InstrumentGen(43, 60 | 127 << 8);
    InstrumentGen(44, 64 | 127 << 8);
    InstrumentGen(54, 1);
    InstrumentGen(53, 2);

    /* env's zone also carries a modulator identical to the default CC1 -> vibrato depth
     * (section 9.5.1: same source, destination, amount source, transform) with amount 0, which
     * must replace the default.
     */
    Instrument("env");
    InstrumentZone();
    InstrumentMod(0x0081, 6, 0, 0, 0);
    InstrumentGen(54, 1);
    InstrumentGen(34, 0);
    InstrumentGen(36, 0);
    InstrumentGen(37, 200);
    InstrumentGen(38, 0);
    InstrumentGen(53, 0);

    Instrument("tune");
    InstrumentZone();
    InstrumentGen(58, 60);
    InstrumentGen(51, (WORD)-12);
    InstrumentGen(53, 1);

    Instrument("drums");
    InstrumentZone();
    InstrumentGen(43, 35 | 38 << 8);
    InstrumentGen(57, 1);
    InstrumentGen(53, 0);
    InstrumentZone();
    InstrumentGen(43, 42 | 46 << 8);
    InstrumentGen(57, 1);
    InstrumentGen(53, 0);

    Instrument("shot");
    InstrumentZone();
    InstrumentGen(53, 0);

    Instrument("mode3");
    InstrumentZone();
    InstrumentGen(54, 3);
    InstrumentGen(38, 8000);
    InstrumentGen(53, 0);

    Instrument("full");
    InstrumentZone();
    InstrumentGen(54, 1);
    InstrumentGen(53, 2);

    Instrument("stereo");
    InstrumentZone();
    InstrumentGen(17, (WORD)-500);
    InstrumentGen(54, 1);
    InstrumentGen(53, 3);
    InstrumentZone();
    InstrumentGen(17, 500);
    InstrumentGen(54, 1);
    InstrumentGen(53, 4);

    /* the terminal instrument, bag, generator and modulator (sections 7.6 - 7.9) */
    Instrument("EOI");
    InstrumentZone();
    InstrumentGen(0, 0);
    InstrumentMod(0, 0, 0, 0, 0);

    Preset("split", 0, 0);
    PresetZone();
    PresetGen(34, 2400);
    PresetGen(58, 70);
    PresetZone();
    PresetGen(41, 0);

    Preset("env", 1, 0);
    PresetZone();
    PresetGen(41, 1);

    Preset("tune", 2, 0);
    PresetZone();
    PresetGen(52, 10);
    PresetGen(41, 2);

    Preset("shot", 3, 0);
    PresetZone();
    PresetGen(41, 4);

    Preset("mode3", 4, 0);
    PresetZone();
    PresetGen(41, 5);

    Preset("full", 5, 0);
    PresetZone();
    PresetGen(41, 6);

    Preset("stereo", 6, 0);
    PresetZone();
    PresetGen(41, 7);

    Preset("drums", 0, 128);
    PresetZone();
    PresetGen(41, 3);

    Preset("EOP", 0, 0);
    PresetZone();
    PresetGen(0, 0);
    memset(g_Pmod, 0, 10);
    g_PmodLength = 10;              /* pmod: only its terminal record */

    formAt = BeginList("RIFF", "sfbk");
    sizeAt = BeginList("LIST", "INFO");
    g_IfilOffset = g_ImageLength + 8;
    PutChunk("ifil", version, 4);
    PutChunk("INAM", (PCBYTE)"Test\0\0", 6);
    EndList(sizeAt);
    sizeAt = BeginList("LIST", "sdta");
    PutChunk("smpl", (PCBYTE)g_Samples, g_SampleCount * 2);
    EndList(sizeAt);
    sizeAt = BeginList("LIST", "pdta");
    g_HydraOffset = g_ImageLength;
    PutChunk("phdr", g_Phdr, g_PhdrLength);
    g_PbagOffset = g_ImageLength + 8;
    PutChunk("pbag", g_Pbag, g_PbagLength);
    PutChunk("pmod", g_Pmod, g_PmodLength);
    g_PgenOffset = g_ImageLength + 8;
    PutChunk("pgen", g_Pgen, g_PgenLength);
    PutChunk("inst", g_Inst, g_InstLength);
    PutChunk("ibag", g_Ibag, g_IbagLength);
    PutChunk("imod", g_Imod, g_ImodLength);
    PutChunk("igen", g_Igen, g_IgenLength);
    PutChunk("shdr", g_Shdr, g_ShdrLength);
    EndList(sizeAt);
    EndList(formAt);
}

static VOID Send(DWORD status, DWORD data1, DWORD data2)
{
    Sf2SynthShortMessage(&g_Synth, status | data1 << 8 | data2 << 16);
}

static VOID Play(DWORD frames)
{
    while (frames)
    {
        DWORD chunk = frames > 44100 ? 44100 : frames;

        Sf2SynthRender(&g_Synth, g_Output, chunk);
        frames -= chunk;
    }
}

static PSF2_VOICE OnlyVoice(VOID)
{
    INT voiceIndex;

    for (voiceIndex = 0; voiceIndex < SF2_SYNTH_VOICES; ++voiceIndex)
        if (g_Synth.Voices[voiceIndex].State != SF2_VOICE_FREE)
            return &g_Synth.Voices[voiceIndex];

    return NULL;
}

static BOOL Near(double value, double expected, double tolerance)
{
    return value >= expected - tolerance && value <= expected + tolerance;
}

static VOID Fresh(VOID)
{
    Sf2SynthInitialize(&g_Synth, &g_Font, 44100);
}

static VOID TestParse(VOID)
{
    SF2_FONT font;
    SF2_PARSE_RESULT result;
    DWORD length;
    DWORD failures = 0;
    static BYTE copy[32768];

    printf("-- parse --\n");
    result = Sf2FontParse(&g_Font, g_Image, g_ImageLength);
    CHECK(result == SF2_PARSE_OK, "the test font parses");
    CHECK(Sf2FontPresetCount(&g_Font) == 8, "8 presets (terminal not counted)");
    CHECK(Sf2FontInstrumentCount(&g_Font) == 8, "8 instruments");
    CHECK(Sf2FontSampleCount(&g_Font) == 5, "5 samples");
    CHECK(g_Font.SampleCount == 3138, "3138 sample points: 3 x (1000 + 46 zeros)");
    CHECK(g_Font.VersionMajor == 2 && g_Font.VersionMinor == 4, "ifil 2.04");
    CHECK(strcmp(g_Font.BankName, "Test") == 0, "INAM kept");
    CHECK(Sf2FontFindPreset(&g_Font, 128, 0) == 7, "bank 128 program 0 found");
    CHECK(Sf2FontFindPreset(&g_Font, 1, 0) == SF2_NONE, "an absent bank is SF2_NONE");

    for (length = 0; length < g_ImageLength; ++length)
        if (Sf2FontParse(&font, g_Image, length) == SF2_PARSE_OK)
            ++failures;

    CHECK(failures == 0, "every truncation of the file is refused");
    CHECK(Sf2FontParse(&font, NULL, 0) == SF2_PARSE_NOT_RIFF, "no image: not RIFF");

    memcpy(copy, g_Image, g_ImageLength);
    copy[0] = 'X';
    CHECK(Sf2FontParse(&font, copy, g_ImageLength) == SF2_PARSE_NOT_RIFF, "a damaged RIFF tag is refused");

    memcpy(copy, g_Image, g_ImageLength);
    copy[g_IfilOffset] = 3;
    CHECK(Sf2FontParse(&font, copy, g_ImageLength) == SF2_PARSE_BAD_VERSION, "ifil 3.x is refused (section 5.1)");

    memcpy(copy, g_Image, g_ImageLength);
    copy[g_PbagOffset + 4] = 50;            /* zone 1's wGenNdx far past zone 2's */
    CHECK(Sf2FontParse(&font, copy, g_ImageLength) == SF2_PARSE_BAD_INDEX, "a non-monotonic pbag is refused (section 7.3)");

    memcpy(copy, g_Image, g_ImageLength);
    copy[g_PgenOffset + 2 * 4 + 2] = 9;     /* split's Instrument generator -> 9: past the terminal 8 */
    CHECK(Sf2FontParse(&font, copy, g_ImageLength) == SF2_PARSE_BAD_INDEX, "an Instrument index past the terminal is refused (section 7.5)");

    memcpy(copy, g_Image, g_ImageLength);
    copy[g_PbagOffset - 4] = 37;            /* pbag's size: no longer a multiple of 4 */
    CHECK(Sf2FontParse(&font, copy, g_ImageLength) != SF2_PARSE_OK, "a broken pbag size is refused");

    memcpy(copy, g_Image, g_ImageLength);
    memcpy(copy + g_PbagOffset - 8, "pbog", 4);
    CHECK(Sf2FontParse(&font, copy, g_ImageLength) == SF2_PARSE_MISSING_CHUNK, "a renamed hydra table is refused (section 4.2: fixed order)");
}

/* A garbled font is refused or played; either way, nothing reads outside the image. */
static VOID TestFuzz(VOID)
{
    static BYTE copy[32768];
    static SF2_FONT font;
    static SF2_SYNTH synth;
    static INT16 output[512 * 2];
    DWORD seed = 12345;
    DWORD round;
    DWORD parsed = 0;

    printf("-- fuzz --\n");

    for (round = 0; round < 3000; ++round)
    {
        DWORD flip;

        memcpy(copy, g_Image, g_ImageLength);

        for (flip = 0; flip < 1 + round % 8; ++flip)
        {
            seed = seed * 1103515245 + 12345;
            /* odd rounds hit only the hydra: indices, generators, sample headers */
            if (round & 1)
                copy[g_HydraOffset + (seed >> 8) % (g_ImageLength - g_HydraOffset)] ^= (BYTE)(seed >> 24);
            else
                copy[(seed >> 8) % g_ImageLength] ^= (BYTE)(seed >> 24);
        }

        if (Sf2FontParse(&font, copy, g_ImageLength) == SF2_PARSE_OK)
        {
            DWORD program;

            ++parsed;
            Sf2SynthInitialize(&synth, &font, 44100);

            for (program = 0; program < 8; ++program)
            {
                Sf2SynthShortMessage(&synth, 0xC0 | program << 8);
                Sf2SynthShortMessage(&synth, 0x90 | (30 + program * 9) << 8 | 100 << 16);
                Sf2SynthShortMessage(&synth, 0x99 | 36 << 8 | 100 << 16);
            }

            Sf2SynthRender(&synth, output, 512);
        }
    }

    printf("  (%u of 3000 garbled fonts still parsed, and played)\n", parsed);
    CHECK(TRUE, "3000 garbled fonts: refused or played, no crash");
}

static VOID TestZones(VOID)
{
    PSF2_VOICE voice;

    printf("-- zones --\n");
    Fresh();
    Send(0x90, 40, 100);
    voice = OnlyVoice();
    CHECK(g_Synth.ActiveVoices == 1 && voice && voice->SampleIndex == 0, "key 40: the 0-59 zone, sample 0");
    CHECK(voice && voice->Generators[48] == 50, "the local zone inherits the global zone's attenuation 50");
    CHECK(voice && voice->Generators[34] == -9600, "section 8.5's example: default -12000 + preset 2400 = -9600");
    CHECK(voice && voice->Generators[58] == -1, "overridingRootKey at the preset level is ignored (section 8.5)");

    Fresh();
    Send(0x90, 72, 30);
    voice = OnlyVoice();
    CHECK(g_Synth.ActiveVoices == 1 && voice && voice->SampleIndex == 1, "key 72 velocity 30: the vel 0-63 zone, sample 1");
    CHECK(voice && voice->Generators[34] == 3600, "section 8.5's example: instrument 1200 + preset 2400 = 3600");
    CHECK(voice && voice->Generators[48] == 0, "a local zone's attenuation replaces the global zone's");

    Fresh();
    Send(0x90, 72, 100);
    voice = OnlyVoice();
    CHECK(g_Synth.ActiveVoices == 1 && voice && voice->SampleIndex == 2, "key 72 velocity 100: the vel 64-127 zone, sample 2");
}

static VOID TestPitch(VOID)
{
    PSF2_VOICE voice;

    printf("-- pitch --\n");
    Fresh();
    Send(0xC0, 1, 0);
    Send(0x90, 60, 100);
    voice = OnlyVoice();
    CHECK(voice && Near(Sf2VoicePlaybackRate(voice), 0.5, 0.00001), "root key, 22050 Hz sample at 44100: rate 0.5");

    Fresh();
    Send(0x90, 48, 100);
    voice = OnlyVoice();
    CHECK(voice && Near(Sf2VoicePlaybackRate(voice), 0.25, 0.00001), "an octave below the root: 0.25");

    Fresh();
    Send(0x90, 59, 100);
    voice = OnlyVoice();
    CHECK(voice && Near(Sf2VoicePlaybackRate(voice), 0.47194, 0.00001), "a semitone below: 0.5 x 2^(-1/12) = 0.47194");

    /* tune: 44100 Hz sample, root overridden 72 -> 60, coarse -12, preset fine +10 */
    Fresh();
    Send(0xC0, 2, 0);
    Send(0x90, 60, 100);
    voice = OnlyVoice();
    CHECK(voice && Near(Sf2VoicePlaybackRate(voice), 0.50289, 0.00001), "overridden root, coarse -12, fine +10: 2^(-1190/1200) = 0.50289");
    Send(0x90, 72, 100);
    voice = &g_Synth.Voices[1];
    CHECK(Near(Sf2VoicePlaybackRate(voice), 1.00579, 0.00001), "and an octave up: 2^(10/1200) = 1.00579");

    /* RPN 0 = 12 semitones, then the wheel fully up: +12 x 8191/8192 semitones */
    Fresh();
    Send(0xC0, 1, 0);
    Send(0xB0, 101, 0);
    Send(0xB0, 100, 0);
    Send(0xB0, 6, 12);
    Send(0x90, 60, 100);
    Send(0xE0, 127, 127);
    Play(64);
    voice = OnlyVoice();
    CHECK(voice && Near(Sf2VoicePlaybackRate(voice), 0.99992, 0.00002), "bend range 12, wheel up: 0.5 x 2^(8191/8192) = 0.99992");
    Send(0xE0, 0, 0);
    Play(64);
    CHECK(voice && Near(Sf2VoicePlaybackRate(voice), 0.25, 0.00001), "wheel fully down: an octave below, 0.25");
}

static VOID TestEnvelope(VOID)
{
    PSF2_VOICE voice;

    printf("-- envelope --\n");
    Fresh();
    Send(0xC0, 1, 0);
    Send(0x90, 60, 127);
    voice = OnlyVoice();
    CHECK(voice && Near(voice->Modulation[48], 41.2, 0.2), "velocity 127 + CC7 100: 960 x concave(27/128) = 41.2 cB");
    Play(22050);
    CHECK(voice && voice->VolumeEnvelope.Stage == SF2_STAGE_ATTACK, "0.5 s into a 1 s attack: still attacking");
    CHECK(voice && Near(voice->VolumeEnvelope.Progress, 0.5, 0.01), "...half way, linear in amplitude");
    Play(22050 + 4410);
    CHECK(voice && voice->VolumeEnvelope.Stage == SF2_STAGE_DECAY, "1.1 s: decaying");
    CHECK(voice && Near(voice->VolumeEnvelope.Level, 100, 5), "...0.1 s of 1000 cB per second: 100 cB");
    Play(8820);
    CHECK(voice && voice->VolumeEnvelope.Stage == SF2_STAGE_SUSTAIN && voice->VolumeEnvelope.Level == 200, "1.3 s: holding the 200 cB sustain");
    CHECK(voice && voice->Position >= voice->LoopStart && voice->Position < voice->LoopEnd, "a looped sample stays inside its loop");
    Send(0x80, 60, 0);
    Play(22050);
    CHECK(voice && voice->State == SF2_VOICE_RELEASED && Near(voice->VolumeEnvelope.Level, 700, 5), "0.5 s into the release: 200 + 500 cB");
    Play(13230);
    CHECK(g_Synth.ActiveVoices == 0, "0.8 s: past 960 cB at 0.76 s, the voice is freed");
    CHECK(!Sf2SynthRender(&g_Synth, g_Output, 64), "no voices: Render says there is nothing to mix");

    Fresh();
    Send(0xC0, 1, 0);
    Send(0x90, 60, 64);
    voice = OnlyVoice();
    CHECK(voice && Near(voice->Modulation[48], 158.9, 0.3), "velocity 64: 41.2 + 960 x concave(63/128) = 158.9 cB");
    Send(0xB0, 1, 127);
    Play(64);
    CHECK(voice && Near(voice->Modulation[6], 0, 0.001), "CC1 127: no vibrato depth -- env's own amount-0 modulator replaced the default");

    Fresh();
    Send(0xB0, 1, 127);
    Send(0x90, 40, 100);
    voice = OnlyVoice();
    CHECK(voice && Near(voice->Modulation[6], 49.6, 0.1), "...where the default stands, CC1 127 gives 50 x 127/128 = 49.6 cents");
}

static VOID TestLoops(VOID)
{
    PSF2_VOICE voice;

    printf("-- loops --\n");
    Fresh();
    Send(0xC0, 3, 0);
    Send(0x90, 60, 100);
    Play(1900);
    CHECK(g_Synth.ActiveVoices == 1, "no loop: 1000 points at rate 0.5 still playing at 1900 frames");
    Play(200);
    CHECK(g_Synth.ActiveVoices == 0, "...and freed by 2100: the sample ran out");

    Fresh();
    Send(0xC0, 4, 0);
    Send(0x90, 60, 100);
    Play(4000);
    voice = OnlyVoice();
    CHECK(voice && voice->Position >= voice->LoopStart && voice->Position < voice->LoopEnd, "loop until release: looping while held");
    Send(0x80, 60, 0);
    Play(2100);
    CHECK(g_Synth.ActiveVoices == 0, "...released, it plays out to the end and stops (its 80 s release never finished)");
}

static VOID TestChannels(VOID)
{
    PSF2_VOICE voice;
    DWORD key;
    static const BYTE gmReset[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };
    static const BYTE gsReset[] = { 0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7 };
    static const BYTE gsDrums[] = { 0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x11, 0x15, 0x01, 0x19, 0xF7 };

    printf("-- channels --\n");
    Fresh();
    Send(0x99, 36, 100);
    voice = OnlyVoice();
    CHECK(g_Synth.Channels[9].PresetIndex == 7 && voice && voice->SampleIndex == 0, "channel 10 plays bank 128");
    Send(0x99, 42, 100);
    CHECK(voice && voice->State == SF2_VOICE_RELEASED, "exclusive class 1: the second drum cuts the first");

    Fresh();
    Send(0xB0, 0, 5);
    Send(0xC0, 1, 0);
    CHECK(g_Synth.Channels[0].PresetIndex == 1, "bank 5 lacks program 1: falls back to bank 0");
    Send(0xC0, 100, 0);
    CHECK(g_Synth.Channels[0].PresetIndex == 0, "no program 100 anywhere: bank 0 program 0");

    Fresh();
    Send(0xC0, 1, 0);

    for (key = 0; key < 70; ++key)
        Send(0x90, key, 100);

    CHECK(g_Synth.ActiveVoices == 64, "70 notes: the pool stays at 64");
    CHECK(g_Synth.Stats.VoicesStolen == 6, "...6 voices stolen");
    Send(0xB0, 120, 0);
    CHECK(g_Synth.ActiveVoices == 0, "CC120: every voice stopped at once");

    Fresh();
    Send(0xC0, 1, 0);
    Send(0xB0, 64, 127);
    Send(0x90, 60, 100);
    Send(0x80, 60, 0);
    voice = OnlyVoice();
    CHECK(voice && voice->State == SF2_VOICE_ON && voice->IsSustained, "CC64 down: the key-off is stored");
    Send(0xB0, 64, 0);
    CHECK(voice && voice->State == SF2_VOICE_RELEASED, "CC64 up: it is executed");
    Send(0x90, 62, 100);
    Send(0xB0, 123, 0);
    CHECK(g_Synth.Voices[1].State == SF2_VOICE_RELEASED, "CC123: all notes enter release");

    /* "full": an instant attack, so the gains are settled after one block */
    Fresh();
    Send(0xC0, 5, 0);
    Send(0xB0, 10, 0);
    Send(0x90, 60, 100);
    Play(64);
    voice = OnlyVoice();
    CHECK(voice && voice->GainRight == 0 && Near(voice->GainLeft, 0.387, 0.002), "CC10 0: hard left, 82.4 cB down (section 8.4.6, pan clamped at -500)");
    Send(0xB0, 11, 0);
    Play(64);
    CHECK(voice && voice->GainLeft < 0.0001f, "CC11 0: 960 x concave(127/128) = 843 cB more, near silence");
    Send(0xB0, 121, 0);
    Play(64);
    CHECK(voice && Near(voice->GainLeft, 0.387, 0.002) && g_Synth.Channels[0].Controllers[10] == 0, "CC121 restores expression, keeps pan");

    Fresh();
    Send(0xC0, 5, 0);
    Send(0xB0, 7, 127);

    for (key = 60; key < 68; ++key)
        Send(0x90, key, 127);

    Play(2000);
    CHECK(g_Output[3998] == 32767 && g_Output[3999] == 32767, "8 full-scale voices saturate at +32767, never wrap");

    Fresh();
    Send(0xC0, 6, 0);
    Send(0x90, 60, 100);
    Play(64);
    CHECK(g_Synth.ActiveVoices == 2
        && Near(Sf2VoicePlaybackRate(&g_Synth.Voices[0]), 0.5, 0.00001)
        && Near(Sf2VoicePlaybackRate(&g_Synth.Voices[1]), 0.5, 0.00001),
        "a stereo pair: the 44100 Hz left sample follows its 22050 Hz right partner (section 7.10)");

    Fresh();
    Send(0xC0, 5, 0);
    Send(0xB0, 7, 20);
    Send(0x90, 60, 100);
    Sf2SynthSysEx(&g_Synth, gmReset, sizeof(gmReset));
    CHECK(g_Synth.ActiveVoices == 0
        && g_Synth.Channels[0].Program == 0
        && g_Synth.Channels[0].Controllers[7] == 100
        && g_Synth.Stats.Resets == 1,
        "GM System On: voices stopped, program 0, volume 100");
    Send(0xC0, 5, 0);
    Sf2SynthSysEx(&g_Synth, gsReset, sizeof(gsReset));
    CHECK(g_Synth.Channels[0].Program == 0 && g_Synth.Stats.Resets == 2, "GS reset: the same");
    Sf2SynthSysEx(&g_Synth, gsDrums, sizeof(gsDrums));
    CHECK(g_Synth.Channels[0].IsPercussion && g_Synth.Channels[0].PresetIndex == 7, "GS 40 11 15 01: part 1 (channel 1) becomes a drum part");
}

INT main(VOID)
{
    printf("== SoundFont 2 synth battery ==\n");
    BuildFont();
    TestParse();
    TestFuzz();
    TestZones();
    TestPitch();
    TestEnvelope();
    TestLoops();
    TestChannels();
    printf("-- %d checks, %d failures --\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
