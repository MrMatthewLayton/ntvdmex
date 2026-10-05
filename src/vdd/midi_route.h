/* midi_route.h -- which host MIDI device the MPU-401 plays through. (GH #136)
 *
 * Settings > Audio > MIDI offers "Host GM | MT-32 | SoundFont", and for as long as the row
 * existed nothing read it: the MPU-401 always opened midiOut device 0 -- on XP, the
 * Microsoft GS Wavetable synth. That stays the default, exactly.
 *
 * ── ★ THIS HOST HAS NO SYNTH OF ITS OWN, SO A CHOICE IS A HOST DEVICE, BY NAME. ──────
 *   There is no MT-32 emulator and no SoundFont player in NTVDMEX, and writing either is
 *   not what this row is for. What a period PC owner did -- and what XP users still do --
 *   is install one as a Windows MIDI DRIVER: Munt's "MT-32 Synth Emulator", a BASSMIDI
 *   or VirtualMIDISynth SoundFont driver, a real MT-32 on a USB MIDI cable. Each shows up
 *   as a midiOut device with a recognisable name, so the choice is honoured by FINDING
 *   that device:
 *       Host GM    device 0, without looking (the old behaviour, byte for byte)
 *       MT-32      the first device whose name contains MT-32 / MT32 / MUNT
 *       SoundFont  the first whose name contains SOUNDFONT / BASSMIDI / VIRTUALMIDISYNTH /
 *                  FLUID / SF2
 *   (case-insensitive). Nothing matching = -1: the host falls back to device 0 and SAYS
 *   so in the log -- a GM synth playing MT-32 music sounds wrong, but silence is worse
 *   and the line names the cause.
 *   An external synth also gets SysEx (vdd_mpu.h): an MT-32 without its timbre uploads
 *   is the wrong instrument.
 * ⚠ SoundFontPath is NOT passed to the SoundFont driver: those drivers keep their own
 *   SF2 list in their own configuration and offer no API to hand them one. The path row
 *   stays stored-only, and the log says so.
 *
 * Pure C, no <windows.h>: the device NAMES are injected, so the choice is checked off-VM
 * by tests/unit/midiroute_test.c.
 */
#ifndef NTVDMEX_MIDI_ROUTE_H
#define NTVDMEX_MIDI_ROUTE_H

enum { MIDI_ROUTE_GM = 0, MIDI_ROUTE_MT32, MIDI_ROUTE_SF2, MIDI_ROUTE_COUNT };

/* Case-insensitive "does hay contain needle" (needle is upper-case ASCII). */
static inline int midi_route_has(const char *hay, const char *needle)
{
    int i, k;
    if (!hay || !needle || !needle[0]) return 0;
    for (i = 0; hay[i]; ++i) {
        for (k = 0; needle[k]; ++k) {
            char c = hay[i + k];
            if (c >= 'a' && c <= 'z') c = (char)(c - 0x20);
            if (c != needle[k]) break;
        }
        if (!needle[k]) return 1;
    }
    return 0;
}

/* The device index `choice` asks for among `n` named devices, or -1 for "none of them".
   Host GM is device 0 whatever the names say (and even with n == 0, as before: opening
   it is what tells us whether it exists). */
static inline int midi_route_pick(int choice, const char *const *names, int n)
{
    static const char *const MT32[] = { "MT-32", "MT32", "MUNT", 0 };
    static const char *const SF2[]  = { "SOUNDFONT", "BASSMIDI", "VIRTUALMIDISYNTH",
                                        "FLUID", "SF2", 0 };
    const char *const *want;
    int i, j;
    if (choice == MIDI_ROUTE_MT32)     want = MT32;
    else if (choice == MIDI_ROUTE_SF2) want = SF2;
    else return 0;
    for (i = 0; i < n; ++i)
        for (j = 0; want[j]; ++j)
            if (midi_route_has(names[i], want[j])) return i;
    return -1;
}

#endif /* NTVDMEX_MIDI_ROUTE_H */
