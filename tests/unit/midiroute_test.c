/* midiroute_test.c -- off-VM battery for Settings > Audio > MIDI (src/vdd/midi_route.h,
 * GH #136).
 *
 * The setting picks a HOST midiOut device by name, so what is pinned here is the choice:
 * Host GM is device 0 whatever is installed (the default, unchanged), MT-32 and SoundFont
 * find their drivers by the names those drivers really register, and a machine with
 * neither says -1 so the host can fall back AND log it. The device names below are the
 * ones the drivers ship with; "Microsoft GS Wavetable SW Synth" is XP's own.
 */
#include <stdio.h>
#include "midi_route.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

int main(void)
{
    static const char *const XP[]   = { "Microsoft GS Wavetable SW Synth" };
    static const char *const MUNT[] = { "Microsoft GS Wavetable SW Synth", "MT-32 Synth Emulator" };
    static const char *const MIX[]  = { "Microsoft GS Wavetable SW Synth", "Roland SC-55 (USB)",
                                        "VirtualMIDISynth #1", "munt mt32emu" };
    static const char *const BASS[] = { "BASSMIDI Driver (port A)", "Microsoft GS Wavetable SW Synth" };

    printf("== #136: MIDI routing battery ==\n");

    CHECK(MidiRoutePick(MIDI_ROUTE_GM, XP, 1) == 0,   "Host GM: device 0");
    CHECK(MidiRoutePick(MIDI_ROUTE_GM, MUNT, 2) == 0, "Host GM: device 0 even with Munt installed");
    CHECK(MidiRoutePick(MIDI_ROUTE_GM, BASS, 2) == 0, "Host GM: device 0 even if it is not the GS synth");
    CHECK(MidiRoutePick(MIDI_ROUTE_GM, 0, 0) == 0,    "Host GM: device 0 with no names at all (as before)");
    CHECK(MidiRoutePick(7, XP, 1) == 0,               "an out-of-range choice behaves as Host GM");

    CHECK(MidiRoutePick(MIDI_ROUTE_MT32, MUNT, 2) == 1, "MT-32: finds Munt's \"MT-32 Synth Emulator\"");
    CHECK(MidiRoutePick(MIDI_ROUTE_MT32, MIX, 4) == 3,  "MT-32: case-insensitive (\"munt mt32emu\")");
    CHECK(MidiRoutePick(MIDI_ROUTE_MT32, XP, 1) == -1,  "MT-32: none installed -> -1 (fall back, and say so)");
    CHECK(MidiRoutePick(MIDI_ROUTE_MT32, BASS, 2) == -1,"MT-32: a SoundFont driver is not an MT-32");

    CHECK(MidiRoutePick(MIDI_ROUTE_SF2, BASS, 2) == 0,  "SoundFont: finds BASSMIDI");
    CHECK(MidiRoutePick(MIDI_ROUTE_SF2, MIX, 4) == 2,   "SoundFont: finds VirtualMIDISynth");
    CHECK(MidiRoutePick(MIDI_ROUTE_SF2, MUNT, 2) == -1, "SoundFont: Munt is not a SoundFont synth");
    CHECK(MidiRoutePick(MIDI_ROUTE_SF2, XP, 1) == -1,   "SoundFont: XP's GS synth alone -> -1");

    CHECK(MidiRouteHas("abc", "") == 0 && MidiRouteHas(0, "X") == 0, "has: empty / NULL never match");
    CHECK(MidiRouteHas("MT-3", "MT-32") == 0, "has: a needle longer than the tail does not match");

    printf("-- %d checks, %d failures --\n", total, fails);
    return fails ? 1 : 0;
}
