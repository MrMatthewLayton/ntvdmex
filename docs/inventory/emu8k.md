# Inventory — AWE32 EMU8000 wavetable (#233)

**Spec:** *AWE32/EMU8000 Programmer's Guide*, revision 1.00, Dave Rossum (E-mu/Creative
Technology, 1994–96), the AWE32 Developer Information Pack. Archived at
[dosdays.co.uk](https://www.dosdays.co.uk/media/creative/emu8kpgm.pdf); **not mirrored** (its
licence forbids redistribution). `§n`/`p.n` below are the guide's.
**Our implementation:** `src/vdd/vdd_emu8k.c`, `src/vdd/vdd_emu8k.h`; mixer source in
`src/vdd/vdd_audio.c` (`vdd_audio_set_emu8k`); off-VM battery `tools/dostest/emu8k_test.c`
(58 checks). **Clean-room:** written from the guide's prose. No other emulator's EMU8000 was
read.
**Marked:** 2026-09-29, **from the code**. **Host wiring re-checked 2026-10-01:** the chip
*is* on the bus now (`e0d54b8`) — fitted when the SB model is AWE32 (`main.c:27952-27960`),
reset on the host's reset path (`main.c:8272`), mixed (`main.c:27975`), and advertised as
`BLASTER` `E` (`main.c:11036`, `dos_env.h:117`). Every mark below describes the device
model; the host steps still open are at the end.

**Verification:** everything is **untested** in the README's sense. It is exercised off-VM
against the guide's own numbers and has never been compared with an oracle. PCem and
DOSBox-X both carry an AWE32, but they are reimplementations, so a probe run against them
would only be *provisional*. Real AWE32 hardware is the oracle that is owed.

---

## The port interface (§2)

| Unit | ref | Status | Where / notes |
|---|---|---|---|
| Three port groups at `E`, `E+400h`, `E+800h`, four ports each | §2 | **IMPL** | `vdd_emu8k_init` `vdd_emu8k.c:720`. There are three `vdd_claim_ports` calls, and `E` defaults to `620h` (`EMU8K_DEFAULT_BASE`) |
| Pointer `E+802h`: bits 4–0 channel, 7–5 register | §2 | **IMPL** | `emu_word_out` `:563`. A read returns the low byte, and the high byte, which the guide calls "random (a VLSI test register)", reads **0** (`:578`) |
| Word transfers | §2 | **IMPL** | `emu_out`/`emu_in` `:583`/`:604` |
| Doubleword = LS word to the port, then MS word to port+2 | §2 | **IMPL** | Each half is applied on its own as a read-modify-write (`half_set` `:372`), so a driver that writes one half only still works. A 32-bit `OUT`/`IN` is split into the same two transfers (`:588`) |
| `E+402h` is **Data1's MS word or Data2**, depending on the register | p.6–7 | **IMPL** | `emu8k_data1_is_dw` `:365`. It is Data1's MS word for CCCA (r0) and for r1 channels 9, 10, 13 and 20–23; for everything else it is Data2 (`emu8k_test` T3) |
| Byte transfers | §2 | **N/A** | "Not allowed", and the guide does not say what they do. We latch the even byte and complete the word on the odd one (`:597`). Counted in `byte_io` |
| I/O WAIT (held-off transfers) | §2 | **N/A** | Transfers here are instantaneous, so nothing ever needs to wait |
| Interrupts | — | **N/A** | The guide gives the EMU8000 no interrupt line |

## Per-channel registers (p.7–18)

| Register | Port / reg | Status | Where / notes |
|---|---|---|---|
| **CPF**: current pitch (31–16), fractional address (15–0) | D0 r0 | **PART** | Write `:400`, read live. `4000h` = one word per sample (`:663`). The guide says current pitch "slews" to the target at an unstated rate; here it **takes the target once per engine tick** (725.6 µs, `:343`) |
| **PTRX**: pitch target (31–16), reverb send (15–8), aux byte (7–0) | D0 r1 | **PART** | The pitch target is IMPL: the engine rewrites it every tick from IP and the modulators (`:321`). The reverb send and aux byte are **STORE**, because there is no effects engine |
| **CVCF**: current volume (31–16), current cutoff (15–0) | D0 r2 | **IMPL** | The volume slews to VT linearly across each tick (rounded so it arrives, `:350`), and a write takes effect at once (`:402`, which is §7's abrupt end). The cutoff takes FT every tick |
| **VTFT**: volume target (31–16), cutoff target (15–0) | D0 r3 | **IMPL** | Written by the engine when it is on (`:339`), and by the guest when it is off |
| D0 r4, r5 (not in the map) | D0 r4/r5 | **STORE** | Read back as written (`:406`) |
| **PSST**: pan (31–24), loop start (23–0) | D0 r6 | **IMPL** | The pan is a linear crossfade, **00h = right, FFh = left** (p.9 and the p.19 diagram: PAN to one side, its logical NOT to the other), `:354`. Loop start is `:667` |
| **CSL**: chorus send (31–24), loop end (23–0) | D0 r7 | **PART** | The loop end is IMPL: passing it returns to PSST, **always** (§5), `:667`. The chorus send is **STORE** |
| **CCCA**: Q (31–28), bit 27, DMA/WR/RIGHT (26–24), current address (23–0) | D1 r0 | **IMPL** | The address is one word below the audio, so the interpolator reads CA+1 and CA+2 (`:653`). The DMA bits allocate the channel to a stream (`emu_stream_alloc` `:122`), and a DMA channel is silent (`:638`). Q is the filter's resonance. Bit 27 is stored as written |
| **ENVVOL**: volume envelope delay | D1 r4 | **IMPL** | `8000h` = none; below that, 725 µs units, which is one engine tick each (`env_tick` `:202`) |
| **DCYSUSV**: bit 15 release, 14–8 sustain, 7 engine off, 6–0 rate | D1 r5 | **IMPL** | `emu_write_dcysusv` `:379`. **Engine off → on with bit 15 clear starts a note**: both envelopes and both LFOs restart, following §6's order. Bit 15 set means release. Sustain is 0.75 dB steps, and 0 = silence |
| **ENVVAL**: modulation envelope delay | D1 r6 | **IMPL** | As for ENVVOL |
| **DCYSUS**: the modulation envelope's sustain, decay and release | D1 r7 | **IMPL** | Bit 7 reads zero (`:442`), and bit 15 releases the modulation envelope (`:443`) |
| **ATKHLDV**: volume hold (14–8), attack (6–0) | D2 r4 | **PART** | Hold is 92 ms steps and attack is linear in amplitude. Bit 7 reads zero (`:456`). ⚠ **Bit 15 ("written as 0 to cause an attack") is not a trigger here.** The DCYSUSV write starts the note, and a re-attack on a note that is already sounding is not modelled |
| **LFO1VAL**: LFO 1 delay | D2 r5 | **IMPL** | `lfo_delay_ticks` `:252` |
| **ATKHLD**: modulation hold and attack | D2 r6 | **PART** | As for ATKHLDV |
| **LFO2VAL**: LFO 2 delay | D2 r7 | **IMPL** | |
| **IP**: initial pitch | D3 r0 | **IMPL** | `E000h` = unity, `1000h` per octave, `:321`. `emu8k_test` T5 measures 689, 1378 and 345 Hz |
| **IFATN**: initial cutoff (15–8), attenuation (7–0) | D3 r1 | **IMPL** | The attenuation is 0.375 dB steps (`:333`; T6 measures 12 dB). The cutoff scale is set by the **end points**, 125 Hz to 8 kHz. ⚠ The guide's own figures disagree: 255 quarter-semitones is 5.3 octaves, not 6. See `EMU8K_CUT_OCT` |
| **PEFE**: modulation-envelope depth to pitch (±1 oct) and cutoff (±6 oct) | D3 r2 | **IMPL** | `:322`, `:329` |
| **FMMOD**: LFO 1 depth to pitch (±1 oct) and cutoff (±3 oct) | D3 r3 | **IMPL** | `:323`, `:330` |
| **TREMFRQ**: LFO 1 tremolo (±12 dB) and frequency (0.042 Hz steps) | D3 r4 | **IMPL** | A triangle wave (`lfo_tick` `:241`). The guide does not name the LFO's shape |
| **FM2FRQ2**: LFO 2 vibrato (±1 oct) and frequency | D3 r5 | **IMPL** | |
| D3 r6, r7 (not in the map) | D3 r6/r7 | **STORE** | `:471` |

## Global registers

| Register | Port / reg / ch | Status | Where / notes |
|---|---|---|---|
| **HWCF1** (`0059h`) | D1 r1 ch 29 | **PART** | p.13 says it "will not be correctly read" and does not say *how*. We read back `value & 7Eh`, which is the shape the period drivers probe for (`:509`) |
| **HWCF2** (`0020h`) | D1 r1 ch 30 | **PART** | Reads back with bits 1–0 set, for the same reason |
| **HWCF3** (`0004h` = audio enable) | D1 r1 ch 31 | **IMPL** | Bit 2 gates the output (`:627`) and the engine keeps running. Other bits are stored |
| **HWCF4/5/6** | D1 r1 ch 9/10/13 | **STORE** | Doublewords, read back |
| **SMALR / SMARR**: read address, bit 31 EMPTY | D1 r1 ch 20/21 | **IMPL** | EMPTY is set only while no channel serves the stream (`emu_sm_read` `:164`) |
| **SMALW / SMARW**: write address, bit 31 FULL | D1 r1 ch 22/23 | **IMPL** | FULL is set while a word waits for a channel. Allocating one completes the transfer (`emu_stream_service` `:133`) |
| **SMLD / SMRD**: the stream data | D1 / D2 r1 ch 26 | **IMPL** | Writes go to SMAxW and increment it. Reads are a **prefetch**: the first word after a new SMAxR is the *old* stream's next word, which is the "stale" word §5 says to discard (T4) |
| **WC**: sample counter | D2 r1 ch 27 | **IMPL** | 44.1 kHz and never reset (p.13). It is driven by rendered samples, or by the host's clock if one is fitted (`emu_wc` `:175`) |
| **INIT1–4**: the effects initialisation arrays | D1/D2 r2/r3 | **STORE** | Each holds its own 32 words and reads them back. What they program (reverb, chorus, EQ) is not modelled |
| D1 r1 and D2 r1, channels the map does not name | — | **STORE** | `d1r1[]`/`d2r1[]` |

## Sound memory and synthesis

| Unit | ref | Status | Where / notes |
|---|---|---|---|
| DRAM from `200000h` | §5 | **IMPL** | Host-owned, `dram_words` long. The stock card is 512 KB (`EMU8K_DRAM_WORDS`), and any size up to the `FFFFDFh` ceiling works. Past the fitted size, reads are zero and writes are dropped, with **no aliasing** (T4) |
| **GM sound ROM** `000000h`–`1FFFFFh` | §5 | **MISS** | **We have no ROM image.** It reads zero and writes are dropped (`emu_mem` `:105`). A ROM can be fitted as `rom`/`rom_words`; see *The ROM and SoundFonts* below |
| The DMA-stream protocol: allocate, set the address, transfer, deallocate | §5 | **IMPL** | T4 runs the guide's seven allocation steps literally |
| Oscillator: 24.16 position, linear interpolation, always loops | §5, p.7–10 | **IMPL** | `vdd_emu8k_render_st` `:624` |
| Volume envelope DAHDSR | p.14–16 | **IMPL** | Delay and hold are timed. Attack is linear in amplitude. Decay and release are **dB-linear**, so the fall is exponential (T6 checks the shape). ⚠ The guide gives only the **end points** of the attack scale (11.88 s to 6 ms) and the decay/release scale (470 ms/dB to 240 µs/dB). **The curve between them is ours: logarithmic interpolation.** Its one cross-check is §7's example, where `5Ch` means "100 msec" and we get 1.97 ms/dB, about 50 dB in 100 ms |
| Modulation envelope, LFO 1, LFO 2 | p.15–18 | **IMPL** | Same engine. The modulation envelope's output is linear |
| Low-pass filter: cutoff and resonance | p.9, p.17 | **IMPL** | A 2-pole resonant low-pass (biquad, integer direct form I, `filt_run` `:293`). Q 0 is flat Butterworth and Q 15 peaks about 24 dB. **Q 0 with a cutoff of FFh is bit-exact transparent** (p.17; T7). The filter's exact topology is not in the guide |
| Pan | p.9, p.19 | **IMPL** | See PSST |
| Effects engine: reverb and chorus sends, INIT programs | p.19, §9 | **MISS** | The dry signal only. The sends are stored and read back |
| Output through the AWE32's CT1745 mixer | — | **MISS** | Summed into the mix at unity, as the GUS is (`vdd_audio.c`). The board-level route is not in the EMU8000 guide |
| Power-up state | §4 | **N/A (deliberate)** | Real hardware powers up with "random data" and output off, and AWEUTIL /S ran §4 from AUTOEXEC. **Reset leaves the chip in the post-§4 state**, because there is no AUTOEXEC to run it from (`vdd_emu8k_reset` `:687`). A guest that runs §4 itself gets the same state |
| AWEUTIL's NMI-driven MIDI emulation (`/EM`) | — | **N/A** | That is a TSR's behaviour, not the chip's |

## Detection: the sequence this passes

The guide says nothing about detection, so we model what the period drivers do. The model
passes each of these steps (`emu8k_test` T1–T4):

1. **The base comes from `BLASTER`'s `E`** (§2 note), or `A` + 400h on a legacy card.
2. **Write HWCF1 = `0059h`, HWCF2 = `0020h`, HWCF3 = `0000h`.** Then HWCF1 read back and
   masked with `7Eh` must give `58h`, and HWCF2 masked with `03h` must give `03h`. This is the
   check the open-source AWE drivers use. We are relying on their description of what the chip
   returns, because the guide says only "not correctly read".
3. **WC must advance at 44.1 kHz.** It is used as the §4 "wait 1024 sample periods" timer, and
   as a liveness check.
4. **Sizing DRAM:** write a pattern at `200000h + n×32K` words and read it back through the
   streams. Unfitted memory reads zero and nothing aliases, so a sizing loop stops at the
   fitted 512 KB.

## The ROM and SoundFonts: what a later step needs

On a real card the 1 MB ROM holds E-mu's General MIDI samples. Guests do not ask the chip
what is in it. They use **preset tables that already know the ROM's addresses**: Creative's
`SYNTHGM.SBK` and the drivers built into games. So:

- **An arbitrary SoundFont cannot be poured into the ROM region.** The guest would play
  whatever lies at the addresses it knows, which would be the wrong sounds. Only an image with
  the **same layout** as E-mu's ROM works, whether it is a dump of a real card (E-mu/Creative
  copyright) or a SoundFont rebuilt to that layout. The hook is already there: point `rom` at
  the words and set `rom_words`. It is read-only and addressed from word 0.
- **The `SoundFontPath` setting (`SET_STR_SOUNDFONT`) fits the *other* path better:** the
  host's MIDI synth for the MPU-401 (the `Midi` combo's `SoundFont` choice). Using the EMU8000
  as that synth would take four pieces: (1) a RIFF/SF2 parser, meaning the `sdta/smpl` 16-bit
  sample chunk plus `pdta`'s `phdr/pbag/pgen/inst/ibag/igen/shdr`; (2) the samples copied into
  a private sound-memory region; (3) a MIDI-to-voice allocator that turns note-on, CC and
  pitch-bend into the §6/§7/§8 register writes, with each generator converted to its register
  (attack, hold, decay, sustain, release, cutoff, Q, pan, attenuation, loop points); and
  (4) voice stealing across the 32 channels. That is a synth built on this chip, not the chip
  itself, so it is not implemented here.

## What the host must add — status 2026-10-01

✅ Steps 1, 2, 3, 5, 6 and 7 are done (`e0d54b8`; citations in the header). ⛔ **Still open:**
step 4 (no `g_emu8k.clock` is fitted, so WC counts in whole mix chunks) and step 8 (there is
no STAGE2 diagnostics line for the chip). The original list follows unchanged.


1. **Decide the card at startup**, alongside `g_gus_on` (`main.c` near `:25086`):
   `g_awe_on = (g_set.v[SET_SBMODEL] == 1)`. Index 1 is `"AWE32"` in `"SB16|AWE32|SB Pro"`
   (`settings.h:219`). It also needs the SB to be fitted (not `nosb.flag`). This makes
   `SbModel` stop being STORE-only (`sb.md` §6).
2. **Statics:** `static emu8k_state g_emu8k; static ntvdd g_emu8k_dev;` and
   `static uint16_t g_emu8k_dram[EMU8K_DRAM_WORDS];` (512 KB), plus
   `#include "vdd_emu8k.h"`.
3. **Register it on the bus** next to the GUS (`main.c` near `:26920`):
   `g_emu8k.base = g_sbcfg.base + 0x400; g_emu8k.dram = g_emu8k_dram;
   g_emu8k.dram_words = EMU8K_DRAM_WORDS; g_emu8k_dev = vdd_emu8k_device(&g_emu8k);
   vdd_bus_add(&g_bus, &g_emu8k_dev);`. This adds **three port ranges**, so check the STAGE2
   bus line (`claim_fail`) against `VDD_MAX_PORTS` (48). The ranges `620–623`, `A20–A23` and
   `E20–E23` collide with nothing else we fit.
4. **Optionally fit the clock** so WC counts at 22.7 µs granularity instead of in whole mix
   chunks. Set `g_emu8k.clock` to a QueryPerformanceCounter-based function that returns µs.
   ⚠ While the host is paused (#219) that clock keeps running and the engine does not. A
   guest's WC wait would still finish, which a paused guest cannot notice.
5. **The mixer:** after `vdd_audio_set_gus(&g_audio, ...)` (`main.c` near `:26938`), add
   `vdd_audio_set_emu8k(&g_audio, g_awe_on ? &g_emu8k : NULL);`. Rendering already runs
   under `HOST_LOCK` in `host_audio_fill`, and the port handlers run under the same lock as
   every other device.
6. **Reset:** add `vdd_emu8k_reset(&g_emu8k);` beside `vdd_gus_reset(&g_gus)` (`main.c`
   near `:7737`). That is the reset path the host actually calls, not one it never reaches.
7. **`BLASTER`:** the guest finds the chip through `E`. `dos_sbcfg` (`dos_env.h`) needs an
   `emu` field (0 = do not advertise), and `dos_env_blaster` needs to emit ` E` plus three hex
   digits before ` T`. A real AWE32's string is `A220 I5 D1 H5 P330 E620 T6`. ⚠ **`T` and `H`
   are deliberately left as they are** (`dos_env.h:59-77`, Doom is confirmed against the
   current string). Whether AWE-aware drivers need `T6`/`H5` is a measurement to make on the
   rig, not something to change on the way past.
8. **Diagnostics:** a STAGE2 line in the style of the GUS one (`main.c` near `:886`) should
   print `io_writes`, `io_reads`, `byte_io`, `sm_words_written`, `sm_words_read`, `sm_held`,
   `notes_started`, `releases`, `samples_out`, `out_nonzero` and `out_peak`. With those, "no
   sound" separates into four cases: not found, never uploaded, never started, or started but
   silent.
