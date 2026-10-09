/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Which host MIDI device the MPU-401 plays through. (GH #136)
 *
 * Settings > Audio > MIDI offers "Host GM | MT-32 | SoundFont", and for as long as the row
 * existed nothing read it: the MPU-401 always opened midiOut device 0 -- on XP, the
 * Microsoft GS Wavetable synth. That stays the default, exactly.
 *
 * THIS HOST HAS NO SYNTH OF ITS OWN, SO A CHOICE IS A HOST DEVICE, BY NAME (Importance = 1):
 * There is no MT-32 emulator and no SoundFont player in NTVDMEX, and writing either is
 * not what this row is for. What a period PC owner did -- and what XP users still do --
 * is install one as a Windows MIDI DRIVER: Munt's "MT-32 Synth Emulator", a BASSMIDI
 * or VirtualMIDISynth SoundFont driver, a real MT-32 on a USB MIDI cable. Each shows up
 * as a midiOut device with a recognisable name, so the choice is honoured by FINDING
 * that device:
 *     Host GM    device 0, without looking (the old behaviour, byte for byte)
 *     MT-32      the first device whose name contains MT-32 / MT32 / MUNT
 *     SoundFont  the first whose name contains SOUNDFONT / BASSMIDI / VIRTUALMIDISYNTH /
 *                FLUID / SF2
 * (case-insensitive). Nothing matching = -1: the host falls back to device 0 and SAYS
 * so in the log -- a GM synth playing MT-32 music sounds wrong, but silence is worse
 * and the line names the cause.
 * An external synth also gets SysEx (vdd_mpu.h): an MT-32 without its timbre uploads
 * is the wrong instrument.
 *
 * [CAUTION]: SoundFontPath is NOT passed to the SoundFont driver: those drivers keep their own
 * SF2 list in their own configuration and offer no API to hand them one. The path row
 * stays stored-only, and the log says so.
 *
 * Pure C, no <windows.h>: the device NAMES are injected, so the choice is checked off-VM
 * by tests/unit/midiroute_test.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_MIDI_ROUTE_H
#define NTVDMEX_MIDI_ROUTE_H

#include "../ntvdmex_types.h"

#define MIDI_ROUTE_UPPER_CASE_OFFSET    0x20    /* 'a' - 'A' */

enum
{
    MIDI_ROUTE_GM = 0, MIDI_ROUTE_MT32, MIDI_ROUTE_SF2, MIDI_ROUTE_COUNT
};

/* Case-insensitive "does haystack contain needle" (needle is upper-case ASCII). */
static inline INT MidiRouteHas(_In_opt_ PCSTR haystack, _In_opt_ PCSTR needle)
{
    INT start, offset;
    if (!haystack || !needle || !needle[0]) return 0;
    for (start = 0; haystack[start]; ++start)
    {
        for (offset = 0; needle[offset]; ++offset)
        {
            CHAR character = haystack[start + offset];
            if (character >= 'a' && character <= 'z') character = (CHAR)(character - MIDI_ROUTE_UPPER_CASE_OFFSET);
            if (character != needle[offset]) break;
        }
        if (!needle[offset]) return 1;
    }
    return 0;
}

/* The device index `choice` asks for among `deviceCount` named devices, or -1 for "none of them".
 * Host GM is device 0 whatever the names say (and even with deviceCount == 0, as before: opening
 * it is what tells us whether it exists).
 */
static inline INT MidiRoutePick(_In_ INT choice, _In_reads_(deviceCount) const PCSTR *names, _In_ INT deviceCount)
{
    static const PCSTR mt32Names[] = { "MT-32", "MT32", "MUNT", 0 };
    static const PCSTR soundFontNames[]  = { "SOUNDFONT", "BASSMIDI", "VIRTUALMIDISYNTH",
                                        "FLUID", "SF2", 0 };
    const PCSTR *wanted;
    INT deviceIndex, nameIndex;
    if (choice == MIDI_ROUTE_MT32)     wanted = mt32Names;
    else if (choice == MIDI_ROUTE_SF2) wanted = soundFontNames;
    else return 0;
    for (deviceIndex = 0; deviceIndex < deviceCount; ++deviceIndex)
        for (nameIndex = 0; wanted[nameIndex]; ++nameIndex)
            if (MidiRouteHas(names[deviceIndex], wanted[nameIndex])) return deviceIndex;
    return -1;
}

#endif /* NTVDMEX_MIDI_ROUTE_H */
