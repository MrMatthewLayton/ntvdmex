/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for Settings > Audio > MIDI (src/vdd/midi_route.h,
 * GH #136).
 *
 * The setting picks a HOST midiOut device by name, so what is pinned here is the choice:
 * Host GM is device 0 whatever is installed (the default, unchanged), MT-32 and SoundFont
 * find their drivers by the names those drivers really register, and a machine with
 * neither says -1 so the host can fall back AND log it. The device names below are the
 * ones the drivers ship with; "Microsoft GS Wavetable SW Synth" is XP's own.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include "midi_route.h"

static INT g_Total = 0;
static INT g_Failures = 0;
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

INT main(VOID)
{
    static PCSTR const xpDevices[]   = { "Microsoft GS Wavetable SW Synth" };
    static PCSTR const muntDevices[] = { "Microsoft GS Wavetable SW Synth", "MT-32 Synth Emulator" };
    static PCSTR const mixedDevices[]  = { "Microsoft GS Wavetable SW Synth", "Roland SC-55 (USB)",
                                        "VirtualMIDISynth #1", "munt mt32emu" };
    static PCSTR const bassDevices[] = { "BASSMIDI Driver (port A)", "Microsoft GS Wavetable SW Synth" };

    printf("== #136: MIDI routing battery ==\n");

    CHECK(MidiRoutePick(MIDI_ROUTE_GM, xpDevices, 1) == 0,   "Host GM: device 0");
    CHECK(MidiRoutePick(MIDI_ROUTE_GM, muntDevices, 2) == 0, "Host GM: device 0 even with Munt installed");
    CHECK(MidiRoutePick(MIDI_ROUTE_GM, bassDevices, 2) == 0, "Host GM: device 0 even if it is not the GS synth");
    CHECK(MidiRoutePick(MIDI_ROUTE_GM, 0, 0) == 0,    "Host GM: device 0 with no names at all (as before)");
    CHECK(MidiRoutePick(7, xpDevices, 1) == 0,               "an out-of-range choice behaves as Host GM");

    CHECK(MidiRoutePick(MIDI_ROUTE_MT32, muntDevices, 2) == 1, "MT-32: finds Munt's \"MT-32 Synth Emulator\"");
    CHECK(MidiRoutePick(MIDI_ROUTE_MT32, mixedDevices, 4) == 3,  "MT-32: case-insensitive (\"munt mt32emu\")");
    CHECK(MidiRoutePick(MIDI_ROUTE_MT32, xpDevices, 1) == -1,  "MT-32: none installed -> -1 (fall back, and say so)");
    CHECK(MidiRoutePick(MIDI_ROUTE_MT32, bassDevices, 2) == -1,"MT-32: a SoundFont driver is not an MT-32");

    CHECK(MidiRoutePick(MIDI_ROUTE_SF2, bassDevices, 2) == 0,  "SoundFont: finds BASSMIDI");
    CHECK(MidiRoutePick(MIDI_ROUTE_SF2, mixedDevices, 4) == 2,   "SoundFont: finds VirtualMIDISynth");
    CHECK(MidiRoutePick(MIDI_ROUTE_SF2, muntDevices, 2) == -1, "SoundFont: Munt is not a SoundFont synth");
    CHECK(MidiRoutePick(MIDI_ROUTE_SF2, xpDevices, 1) == -1,   "SoundFont: XP's GS synth alone -> -1");

    CHECK(MidiRouteHas("abc", "") == 0 && MidiRouteHas(0, "X") == 0, "has: empty / NULL never match");
    CHECK(MidiRouteHas("MT-3", "MT-32") == 0, "has: a needle longer than the tail does not match");

    printf("-- %d checks, %d failures --\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
