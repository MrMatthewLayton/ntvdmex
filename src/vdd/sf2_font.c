/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * A SoundFont 2 file, read in place: see sf2_font.h. Section numbers are those of the
 * SoundFont Technical Specification 2.04.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "sf2_font.h"

#define SF2_CHUNK_HEADER_BYTES              8   /* Section 3.1: ckID + ckSize */
#define SF2_FOURCC_BYTES                    4
#define SF2_LIST_TYPE_BYTES                 4   /* A RIFF / LIST chunk's form type */
#define SF2_VERSION_BYTES                   4   /* Section 5.1: ifil is always four bytes */
#define SF2_SUPPORTED_MAJOR                 2   /* Section 5.1: within a wMajor the format only grows */
#define SF2_SAMPLE_BYTES                    2   /* Section 6.1: 16-bit sample data points */
#define SF2_PDTA_TABLES                     9   /* Section 7.1: the hydra */
#define SF2_MINIMUM_HEADER_RECORDS          2   /* Sections 7.2 / 7.6: one entry and the terminal */
#define SF2_MINIMUM_LIST_RECORDS            1   /* Bags, modulators, generators, samples: the terminal */
#define SF2_ALIGNMENT_MASK                  1   /* Section 3.1: chunks are padded to a word */

/* Offsets inside the records, section 7.2-7.10. */
#define SF2_PRESET_PROGRAM_OFFSET           20
#define SF2_PRESET_BANK_OFFSET              22
#define SF2_PRESET_BAG_OFFSET               24
#define SF2_BAG_MODULATOR_OFFSET            2
#define SF2_GENERATOR_AMOUNT_OFFSET         2
#define SF2_MODULATOR_DESTINATION_OFFSET    2
#define SF2_MODULATOR_AMOUNT_OFFSET         4
#define SF2_MODULATOR_AMOUNT_SOURCE_OFFSET  6
#define SF2_MODULATOR_TRANSFORM_OFFSET      8
#define SF2_INSTRUMENT_BAG_OFFSET           20
#define SF2_SAMPLE_START_OFFSET             20
#define SF2_SAMPLE_END_OFFSET               24
#define SF2_SAMPLE_START_LOOP_OFFSET        28
#define SF2_SAMPLE_END_LOOP_OFFSET          32
#define SF2_SAMPLE_RATE_OFFSET              36
#define SF2_SAMPLE_PITCH_OFFSET             40
#define SF2_SAMPLE_CORRECTION_OFFSET        41
#define SF2_SAMPLE_LINK_OFFSET              42
#define SF2_SAMPLE_TYPE_OFFSET              44

/* A chunk located inside a parent: its data and the data's length. */
typedef struct _SF2_CHUNK
{
    PCBYTE Data;
    DWORD  Length;
} SF2_CHUNK, *PSF2_CHUNK;

/* The nine hydra tables in their order (section 4.2: the order is fixed), with record sizes. */
static const CHAR g_Sf2PdtaNames[SF2_PDTA_TABLES][SF2_FOURCC_BYTES + 1] =
{
    "phdr", "pbag", "pmod", "pgen", "inst", "ibag", "imod", "igen", "shdr"
};

static const DWORD g_Sf2PdtaRecordBytes[SF2_PDTA_TABLES] =
{
    SF2_PRESET_RECORD_BYTES, SF2_BAG_RECORD_BYTES, SF2_MODULATOR_RECORD_BYTES,
    SF2_GENERATOR_RECORD_BYTES, SF2_INSTRUMENT_RECORD_BYTES, SF2_BAG_RECORD_BYTES,
    SF2_MODULATOR_RECORD_BYTES, SF2_GENERATOR_RECORD_BYTES, SF2_SAMPLE_RECORD_BYTES
};

static const DWORD g_Sf2PdtaMinimumRecords[SF2_PDTA_TABLES] =
{
    SF2_MINIMUM_HEADER_RECORDS, SF2_MINIMUM_LIST_RECORDS, SF2_MINIMUM_LIST_RECORDS,
    SF2_MINIMUM_LIST_RECORDS, SF2_MINIMUM_HEADER_RECORDS, SF2_MINIMUM_LIST_RECORDS,
    SF2_MINIMUM_LIST_RECORDS, SF2_MINIMUM_LIST_RECORDS, SF2_MINIMUM_LIST_RECORDS
};

static WORD Sf2Read16(PCBYTE bytes)
{
    return (WORD)(bytes[0] | (bytes[1] << 8));
}

static DWORD Sf2Read32(PCBYTE bytes)
{
    return (DWORD)bytes[0]
        | ((DWORD)bytes[1] << 8)
        | ((DWORD)bytes[2] << 16)
        | ((DWORD)bytes[3] << 24);
}

static BOOL Sf2IsFourCc(PCBYTE bytes, PCSTR name)
{
    return bytes[0] == (BYTE)name[0]
        && bytes[1] == (BYTE)name[1]
        && bytes[2] == (BYTE)name[2]
        && bytes[3] == (BYTE)name[3];
}

/* The chunk at `offset` in [data, data + length): FALSE if its header or data does not fit.
 * `next` is where the following chunk starts (the data plus its pad byte, section 3.1).
 */
static BOOL Sf2ChunkAt(PCBYTE data, DWORD length, DWORD offset, PSF2_CHUNK chunk, PDWORD next)
{
    DWORD chunkLength;

    if (offset > length || length - offset < SF2_CHUNK_HEADER_BYTES)
        return FALSE;

    chunkLength = Sf2Read32(data + offset + SF2_FOURCC_BYTES);

    if (chunkLength > length - offset - SF2_CHUNK_HEADER_BYTES)
        return FALSE;

    chunk->Data = data + offset + SF2_CHUNK_HEADER_BYTES;
    chunk->Length = chunkLength;
    *next = offset + SF2_CHUNK_HEADER_BYTES + chunkLength + (chunkLength & SF2_ALIGNMENT_MASK);
    return TRUE;
}

/* A LIST chunk of the given form type at `offset`. */
static SF2_PARSE_RESULT Sf2ListAt(
    PCBYTE data,
    DWORD length,
    DWORD offset,
    PCSTR formType,
    PSF2_CHUNK list,
    PDWORD next)
{
    SF2_CHUNK chunk;

    if (!Sf2ChunkAt(data, length, offset, &chunk, next))
        return offset >= length ? SF2_PARSE_MISSING_CHUNK : SF2_PARSE_TRUNCATED;

    if (!Sf2IsFourCc(data + offset, "LIST")
        || chunk.Length < SF2_LIST_TYPE_BYTES
        || !Sf2IsFourCc(chunk.Data, formType))
        return SF2_PARSE_MISSING_CHUNK;

    list->Data = chunk.Data + SF2_LIST_TYPE_BYTES;
    list->Length = chunk.Length - SF2_LIST_TYPE_BYTES;
    return SF2_PARSE_OK;
}

/* Section 5: the INFO list. ifil is mandatory; INAM is kept for the report; every other
 * sub-chunk, known or not, is ignored (section 3.2: unknown INFO sub-chunks are ignored).
 */
static SF2_PARSE_RESULT Sf2ParseInfoList(PSF2_FONT font, PSF2_CHUNK info)
{
    DWORD offset = 0;
    BOOL hasVersion = FALSE;

    while (offset < info->Length)
    {
        SF2_CHUNK chunk;
        DWORD next;
        PCBYTE header = info->Data + offset;

        if (!Sf2ChunkAt(info->Data, info->Length, offset, &chunk, &next))
            return SF2_PARSE_TRUNCATED;

        if (Sf2IsFourCc(header, "ifil"))
        {
            if (chunk.Length != SF2_VERSION_BYTES)
                return SF2_PARSE_NO_VERSION;

            font->VersionMajor = Sf2Read16(chunk.Data);
            font->VersionMinor = Sf2Read16(chunk.Data + sizeof(WORD));
            hasVersion = TRUE;
        }
        else if (Sf2IsFourCc(header, "INAM"))
        {
            DWORD character;
            DWORD characterLimit = chunk.Length < SF2_BANK_NAME_CHARS - 1
                ? chunk.Length
                : SF2_BANK_NAME_CHARS - 1;

            for (character = 0; character < characterLimit; ++character)
            {
                if (!chunk.Data[character])
                    break;

                font->BankName[character] = (CHAR)chunk.Data[character];
            }

            font->BankName[character] = 0;
        }

        offset = next;
    }

    if (!hasVersion)
        return SF2_PARSE_NO_VERSION;

    /* Section 5.1: a reader is upward compatible within one wMajor. 1.x is a different format
     * and 3.x is not this one (its samples need not be 16-bit PCM).
     */
    if (font->VersionMajor != SF2_SUPPORTED_MAJOR)
        return SF2_PARSE_BAD_VERSION;

    return SF2_PARSE_OK;
}

/* Section 6: smpl is the only sample data we read; sm24 (section 6.2) is optional and its low
 * byte is ignored, which the section allows. Anything else in sdta is an unknown chunk.
 */
static SF2_PARSE_RESULT Sf2ParseSampleList(PSF2_FONT font, PSF2_CHUNK sdta)
{
    DWORD offset = 0;

    while (offset < sdta->Length)
    {
        SF2_CHUNK chunk;
        DWORD next;
        PCBYTE header = sdta->Data + offset;

        if (!Sf2ChunkAt(sdta->Data, sdta->Length, offset, &chunk, &next))
            return SF2_PARSE_TRUNCATED;

        if (Sf2IsFourCc(header, "smpl"))
        {
            /* The image is mapped at a page boundary and every chunk is word-padded, so a
             * well-formed file puts smpl on an even offset: INT16 reads are aligned.
             */
            if ((DWORD)(chunk.Data - font->Image) & SF2_ALIGNMENT_MASK)
                return SF2_PARSE_TRUNCATED;

            font->Samples = (const INT16 *)(const VOID *)chunk.Data;
            font->SampleCount = chunk.Length / SF2_SAMPLE_BYTES;
        }
        else if (!Sf2IsFourCc(header, "sm24"))
            return SF2_PARSE_UNEXPECTED_CHUNK;

        offset = next;
    }

    return font->Samples ? SF2_PARSE_OK : SF2_PARSE_NO_SAMPLES;
}

/* Section 7: the nine tables, in their fixed order, each a whole number of records and at
 * least its terminal.
 */
static SF2_PARSE_RESULT Sf2ParseHydraTables(PSF2_FONT font, PSF2_CHUNK pdta)
{
    PSF2_TABLE tables[SF2_PDTA_TABLES];
    DWORD offset = 0;
    DWORD tableIndex;

    tables[0] = &font->Presets;
    tables[1] = &font->PresetBags;
    tables[2] = &font->PresetModulators;
    tables[3] = &font->PresetGenerators;
    tables[4] = &font->Instruments;
    tables[5] = &font->InstrumentBags;
    tables[6] = &font->InstrumentModulators;
    tables[7] = &font->InstrumentGenerators;
    tables[8] = &font->SampleHeaders;

    for (tableIndex = 0; tableIndex < SF2_PDTA_TABLES; ++tableIndex)
    {
        SF2_CHUNK chunk;
        DWORD next;
        DWORD recordBytes;

        if (offset >= pdta->Length)
            return SF2_PARSE_MISSING_CHUNK;

        if (!Sf2ChunkAt(pdta->Data, pdta->Length, offset, &chunk, &next))
            return SF2_PARSE_TRUNCATED;

        if (!Sf2IsFourCc(pdta->Data + offset, g_Sf2PdtaNames[tableIndex]))
            return SF2_PARSE_MISSING_CHUNK;

        recordBytes = g_Sf2PdtaRecordBytes[tableIndex];

        if (chunk.Length % recordBytes
            || chunk.Length / recordBytes < g_Sf2PdtaMinimumRecords[tableIndex])
            return SF2_PARSE_BAD_TABLE_SIZE;

        tables[tableIndex]->Records = chunk.Data;
        tables[tableIndex]->Count = chunk.Length / recordBytes;
        offset = next;
    }

    return SF2_PARSE_OK;
}

/* Sections 7.2 / 7.6: header bag indices rise monotonically and the terminal's names the
 * terminal bag. Sections 7.3 / 7.7: each bag's generator and modulator indices rise and the
 * terminal bag's name the terminal records. Anything else is structurally defective.
 */
static BOOL Sf2AreBagsSound(
    PCSF2_TABLE headers,
    DWORD headerRecordBytes,
    DWORD headerBagOffset,
    PCSF2_TABLE bags,
    PCSF2_TABLE modulators,
    PCSF2_TABLE generators)
{
    DWORD previous = 0;
    DWORD recordIndex;

    for (recordIndex = 0; recordIndex < headers->Count; ++recordIndex)
    {
        DWORD bag = Sf2Read16(headers->Records + recordIndex * headerRecordBytes + headerBagOffset);

        if (bag < previous)
            return FALSE;

        previous = bag;
    }

    if (previous != bags->Count - 1)
        return FALSE;

    previous = 0;

    for (recordIndex = 0; recordIndex < bags->Count; ++recordIndex)
    {
        DWORD generator = Sf2Read16(bags->Records + recordIndex * SF2_BAG_RECORD_BYTES);

        if (generator < previous)
            return FALSE;

        previous = generator;
    }

    if (previous != generators->Count - 1)
        return FALSE;

    previous = 0;

    for (recordIndex = 0; recordIndex < bags->Count; ++recordIndex)
    {
        PCBYTE bag = bags->Records + recordIndex * SF2_BAG_RECORD_BYTES;
        DWORD modulator = Sf2Read16(bag + SF2_BAG_MODULATOR_OFFSET);

        if (modulator < previous)
            return FALSE;

        previous = modulator;
    }

    return previous == modulators->Count - 1;
}

/* Sections 7.5 / 7.9: an Instrument generator at or past the terminal instrument, or a sampleID
 * at or past the terminal sample, makes the file structurally unsound. Checked here once, so a
 * note never follows an index out of its table.
 */
static BOOL Sf2AreIndexGeneratorsSound(
    PCSF2_TABLE generators,
    WORD indexOperator,
    DWORD terminalIndex)
{
    DWORD recordIndex;

    for (recordIndex = 0; recordIndex < generators->Count; ++recordIndex)
    {
        PCBYTE record = generators->Records + recordIndex * SF2_GENERATOR_RECORD_BYTES;

        if (Sf2Read16(record) == indexOperator
            && Sf2Read16(record + SF2_GENERATOR_AMOUNT_OFFSET) >= terminalIndex)
            return FALSE;
    }

    return TRUE;
}

SF2_PARSE_RESULT Sf2FontParse(PSF2_FONT font, PCBYTE image, DWORD imageLength)
{
    SF2_CHUNK form;
    SF2_CHUNK info;
    SF2_CHUNK sdta;
    SF2_CHUNK pdta;
    SF2_PARSE_RESULT result;
    DWORD next;
    DWORD byteIndex;

    for (byteIndex = 0; byteIndex < sizeof(*font); ++byteIndex)
        ((PBYTE)font)[byteIndex] = 0;

    font->Image = image;
    font->ImageLength = imageLength;

    /* Section 4.1: RIFF('sfbk' INFO sdta pdta). Bytes after the form are not ours to judge. */
    if (!image
        || imageLength < SF2_CHUNK_HEADER_BYTES + SF2_LIST_TYPE_BYTES
        || !Sf2IsFourCc(image, "RIFF"))
        return SF2_PARSE_NOT_RIFF;

    if (!Sf2ChunkAt(image, imageLength, 0, &form, &next))
        return SF2_PARSE_TRUNCATED;

    if (form.Length < SF2_LIST_TYPE_BYTES || !Sf2IsFourCc(form.Data, "sfbk"))
        return SF2_PARSE_NOT_RIFF;

    form.Data += SF2_LIST_TYPE_BYTES;
    form.Length -= SF2_LIST_TYPE_BYTES;

    result = Sf2ListAt(form.Data, form.Length, 0, "INFO", &info, &next);

    if (result != SF2_PARSE_OK)
        return result;

    result = Sf2ListAt(form.Data, form.Length, next, "sdta", &sdta, &next);

    if (result != SF2_PARSE_OK)
        return result;

    result = Sf2ListAt(form.Data, form.Length, next, "pdta", &pdta, &next);

    if (result != SF2_PARSE_OK)
        return result;

    result = Sf2ParseInfoList(font, &info);

    if (result != SF2_PARSE_OK)
        return result;

    result = Sf2ParseSampleList(font, &sdta);

    if (result != SF2_PARSE_OK)
        return result;

    result = Sf2ParseHydraTables(font, &pdta);

    if (result != SF2_PARSE_OK)
        return result;

    if (!Sf2AreBagsSound(
            &font->Presets,
            SF2_PRESET_RECORD_BYTES,
            SF2_PRESET_BAG_OFFSET,
            &font->PresetBags,
            &font->PresetModulators,
            &font->PresetGenerators))
        return SF2_PARSE_BAD_INDEX;

    if (!Sf2AreBagsSound(
            &font->Instruments,
            SF2_INSTRUMENT_RECORD_BYTES,
            SF2_INSTRUMENT_BAG_OFFSET,
            &font->InstrumentBags,
            &font->InstrumentModulators,
            &font->InstrumentGenerators))
        return SF2_PARSE_BAD_INDEX;

    if (!Sf2AreIndexGeneratorsSound(
            &font->PresetGenerators,
            SF2_GEN_INSTRUMENT,
            font->Instruments.Count - 1)
        || !Sf2AreIndexGeneratorsSound(
            &font->InstrumentGenerators,
            SF2_GEN_SAMPLE_ID,
            font->SampleHeaders.Count - 1))
        return SF2_PARSE_BAD_INDEX;

    return SF2_PARSE_OK;
}

PCSTR Sf2FontErrorText(SF2_PARSE_RESULT result)
{
    switch (result)
    {
    case SF2_PARSE_OK:
        return "ok";

    case SF2_PARSE_NOT_RIFF:
        return "not a RIFF sfbk file";

    case SF2_PARSE_TRUNCATED:
        return "truncated (a chunk runs past its parent)";

    case SF2_PARSE_MISSING_CHUNK:
        return "a required chunk is missing or out of order";

    case SF2_PARSE_NO_VERSION:
        return "no ifil version chunk";

    case SF2_PARSE_BAD_VERSION:
        return "not a SoundFont 2.x file";

    case SF2_PARSE_NO_SAMPLES:
        return "no smpl sample data";

    case SF2_PARSE_BAD_TABLE_SIZE:
        return "a hydra table is not a whole number of records";

    case SF2_PARSE_BAD_INDEX:
        return "a bag, generator or modulator index is out of order or range";

    case SF2_PARSE_UNEXPECTED_CHUNK:
        return "an unknown chunk where none is allowed";

    default:
        return "unknown error";
    }
}

DWORD Sf2FontPresetCount(PCSF2_FONT font)
{
    return font->Presets.Count ? font->Presets.Count - 1 : 0;
}

DWORD Sf2FontInstrumentCount(PCSF2_FONT font)
{
    return font->Instruments.Count ? font->Instruments.Count - 1 : 0;
}

DWORD Sf2FontSampleCount(PCSF2_FONT font)
{
    return font->SampleHeaders.Count ? font->SampleHeaders.Count - 1 : 0;
}

VOID Sf2FontReadPreset(PCSF2_FONT font, DWORD presetIndex, PSF2_PRESET_HEADER preset)
{
    PCBYTE record = font->Presets.Records + presetIndex * SF2_PRESET_RECORD_BYTES;

    preset->Program = Sf2Read16(record + SF2_PRESET_PROGRAM_OFFSET);
    preset->Bank = Sf2Read16(record + SF2_PRESET_BANK_OFFSET);
    preset->FirstBag = Sf2Read16(record + SF2_PRESET_BAG_OFFSET);
    preset->EndBag = Sf2Read16(record + SF2_PRESET_RECORD_BYTES + SF2_PRESET_BAG_OFFSET);
}

DWORD Sf2FontFindPreset(PCSF2_FONT font, WORD bank, WORD program)
{
    DWORD presetIndex;
    DWORD presetCount = Sf2FontPresetCount(font);

    for (presetIndex = 0; presetIndex < presetCount; ++presetIndex)
    {
        SF2_PRESET_HEADER preset;

        Sf2FontReadPreset(font, presetIndex, &preset);

        /* Section 7.2: a preset with no zones is ignored. */
        if (preset.Bank == bank && preset.Program == program && preset.EndBag > preset.FirstBag)
            return presetIndex;
    }

    return SF2_NONE;
}

VOID Sf2FontInstrumentBags(PCSF2_FONT font, DWORD instrumentIndex, PDWORD firstBag, PDWORD endBag)
{
    PCBYTE record = font->Instruments.Records + instrumentIndex * SF2_INSTRUMENT_RECORD_BYTES;

    *firstBag = Sf2Read16(record + SF2_INSTRUMENT_BAG_OFFSET);
    *endBag = Sf2Read16(record + SF2_INSTRUMENT_RECORD_BYTES + SF2_INSTRUMENT_BAG_OFFSET);
}

VOID Sf2FontReadZone(PCSF2_FONT font, BOOL isPreset, DWORD bagIndex, PSF2_ZONE zone)
{
    PCSF2_TABLE bags = isPreset ? &font->PresetBags : &font->InstrumentBags;
    PCSF2_TABLE generators = isPreset ? &font->PresetGenerators : &font->InstrumentGenerators;
    WORD terminalOperator = isPreset ? SF2_GEN_INSTRUMENT : SF2_GEN_SAMPLE_ID;
    PCBYTE bag = bags->Records + bagIndex * SF2_BAG_RECORD_BYTES;
    DWORD generatorEnd;
    DWORD generatorIndex;

    zone->GeneratorFirst = Sf2Read16(bag);
    zone->GeneratorEnd = Sf2Read16(bag + SF2_BAG_RECORD_BYTES);
    zone->ModulatorFirst = Sf2Read16(bag + SF2_BAG_MODULATOR_OFFSET);
    zone->ModulatorEnd = Sf2Read16(bag + SF2_BAG_RECORD_BYTES + SF2_BAG_MODULATOR_OFFSET);
    zone->HasTerminal = FALSE;
    zone->TerminalIndex = 0;

    /* Sections 7.5 / 7.9: generators after the terminal one are ignored, so the zone ends at
     * the first Instrument / sampleID. A zone without one is global (if it is the first) or
     * ignored -- the caller decides.
     */
    generatorEnd = zone->GeneratorEnd;

    for (generatorIndex = zone->GeneratorFirst; generatorIndex < generatorEnd; ++generatorIndex)
    {
        PCBYTE record = generators->Records + generatorIndex * SF2_GENERATOR_RECORD_BYTES;

        if (Sf2Read16(record) == terminalOperator)
        {
            zone->HasTerminal = TRUE;
            zone->TerminalIndex = Sf2Read16(record + SF2_GENERATOR_AMOUNT_OFFSET);
            zone->GeneratorEnd = generatorIndex;
            break;
        }
    }
}

VOID Sf2FontReadGenerator(PCSF2_TABLE generators, DWORD generatorIndex, PSF2_GENERATOR generator)
{
    PCBYTE record = generators->Records + generatorIndex * SF2_GENERATOR_RECORD_BYTES;

    generator->Operator = Sf2Read16(record);
    generator->Amount = (SHORT)Sf2Read16(record + SF2_GENERATOR_AMOUNT_OFFSET);
}

VOID Sf2FontReadModulator(PCSF2_TABLE modulators, DWORD modulatorIndex, PSF2_MODULATOR modulator)
{
    PCBYTE record = modulators->Records + modulatorIndex * SF2_MODULATOR_RECORD_BYTES;

    modulator->Source = Sf2Read16(record);
    modulator->Destination = Sf2Read16(record + SF2_MODULATOR_DESTINATION_OFFSET);
    modulator->Amount = (SHORT)Sf2Read16(record + SF2_MODULATOR_AMOUNT_OFFSET);
    modulator->AmountSource = Sf2Read16(record + SF2_MODULATOR_AMOUNT_SOURCE_OFFSET);
    modulator->Transform = Sf2Read16(record + SF2_MODULATOR_TRANSFORM_OFFSET);
}

VOID Sf2FontReadSampleHeader(PCSF2_FONT font, DWORD sampleIndex, PSF2_SAMPLE_HEADER header)
{
    PCBYTE record = font->SampleHeaders.Records + sampleIndex * SF2_SAMPLE_RECORD_BYTES;

    header->Start = Sf2Read32(record + SF2_SAMPLE_START_OFFSET);
    header->End = Sf2Read32(record + SF2_SAMPLE_END_OFFSET);
    header->StartLoop = Sf2Read32(record + SF2_SAMPLE_START_LOOP_OFFSET);
    header->EndLoop = Sf2Read32(record + SF2_SAMPLE_END_LOOP_OFFSET);
    header->SampleRate = Sf2Read32(record + SF2_SAMPLE_RATE_OFFSET);
    header->OriginalPitch = record[SF2_SAMPLE_PITCH_OFFSET];
    header->PitchCorrection = (INT8)record[SF2_SAMPLE_CORRECTION_OFFSET];
    header->SampleLink = Sf2Read16(record + SF2_SAMPLE_LINK_OFFSET);
    header->SampleType = Sf2Read16(record + SF2_SAMPLE_TYPE_OFFSET);
}
