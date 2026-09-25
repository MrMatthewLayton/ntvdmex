# Inventory — Gravis UltraSound (GF1)

**Spec:** *UltraSound SDK v2.22* (Advanced Gravis / FORTE, 1994), Chapter 2 and the SDK's own
driver source. **▶ The hardware reference is [`../ref/gus.md`](../ref/gus.md)** — what the card
*does*.
**Our implementation:** `src/vdd/vdd_gus.c` (s80), `src/vdd/vdd_gus.h`; mixer source in `src/vdd/vdd_audio.c`; off-VM battery `tools/dostest/gus_test.c` (29 checks).
**Acceptance guest:** heaven7 (its music is GUS-only; it renders already).
**Marked:** 2026-09-25 (s80), **from the code**.

---

## Headline — heaven7 finds the card, fills it and plays it (s80)

Rig, host `a0294462`: with `ULTRASND=240,3,3,11,11` in its environment, heaven7 resets the GF1
to `07h`, sizes and fills its DRAM by programmed I/O (66,272 pokes, 38,374 peeks), starts 132
voices and leaves 10 running; **96% of the 1.32 M samples rendered are non-zero, peak 17,871**.
It uses neither DMA nor interrupts — it polls. ⚠ Nobody has *heard* it yet: a headless run
cannot. The by-hand test is owed.

⛔ Found on the way: the environment block is built **before** the devices, so the card must be
decided at startup — the first run had a GUS on the bus and no `ULTRASND=`, and heaven7 never
touched a port.

---

## The card

| Unit | ref | Status | Where / notes |
|---|---|---|---|
| `ULTRASND` environment string | §1, §8 | **IMPL** | `main.c` env build — the device's own numbers; a `dosenv.txt` `ULTRASND` wins. DOS path only: the Win16 block is left exactly as it was |
| Port claims `2X0–2XF`, `3X0–3X7` | §1 | **IMPL** | `vdd_gus.c:527`; `VDD_MAX_PORTS` 32 → 48 (31 were in use) |
| Register select / data, 8- and 16-bit access | §2 | **IMPL** | `gus_out`/`gus_in` `:284`/`:335`; a byte to `3X4` latches, `3X5` completes |
| Global registers `41h–4Ch` | §2.1 | **IMPL** | `gus_reg_write`/`gus_reg_read` `:180`/`:248` |
| Reset `4Ch` | §2.1 | **IMPL** | bit 0 = 0 runs `gus_chip_reset` `:161` |
| Voice registers `00h–0Eh` / `80h–8Eh` | §2.2 | **IMPL** | 32 banks via the page |
| Self-modifying bits | §2.2 | **IMPL** | the engine writes stopped/direction/pending itself; the double-write race is not modelled (it cannot lose a write here) |
| DRAM + PIO at `3X7` | §3 | **IMPL** | 1 MB (`GUS_DRAM_SIZE`); detection passes (`gus_test` T1) |
| DRAM DMA, 16-bit translation, invert-MSB | §3 | **IMPL** | `gus_dma_try` `:126` — instantaneous; retried each render if the 8237 is not ready. Card→PC direction not modelled |
| Voice engine: position, frequency, interpolation, end / loop / bidi / rollover | §4 | **IMPL** | `gus_voice_step` `:397`; rendered at the GF1's own rate (`vdd_gus_rate_hz`) and resampled by the mixer |
| Logarithmic volume, ramps | §7 | **IMPL** | `vdd_gus_vol_gain` `:372` — curve checked against the SDK table's ratios; `gus_ramp_step` `:424` |
| Pan | §7 | **PART** | stored and read back; **the mixer is mono**, so pan does not move the sound |
| Latches `2XB`, the lock-out, `2XF` | §5 | **PART** | IRQ/DMA latches and the next-write lock-out IMPL (only GUS-port writes are seen, so a write to another card's port does not break the arm); `2XF` banks 5/6 accepted and not stored |
| Mix control `2X0` | §5 | **PART** | stored; line/mic inputs and output enable do not gate anything |
| IRQ status `2X6` | §6 | **IMPL** | `gus_irq_status` `:67` |
| Voice IRQ FIFO `8Fh` | §6 | **IMPL** | `gus_irq_fifo` `:102`, cleared by the read, active-low bits |
| The card's interrupt line | §6 | **IMPL** | edge on any enabled source with 4Ch bit 2; on the latched line or IRQ 11 |
| Timers 1/2 and `2X8`/`2X9` | §9 | **IMPL** | `gus_timers` `:455`, advanced by rendered GF1 time |
| MIDI 6850 | §9 | **PART** | status reads "transmitter empty"; data goes nowhere, nothing is ever received |
| Record path (`48h`, `49h`) | §2.1 | **PART** | registers answer; starting a take completes at once with no data |
| ICS-2101 mixer, CS4231 codec | §10 | **N/A** | later board options; `7X6` reads `FFh` = "pre-3.7 board" |

## What the card needs from the host — and what is not ready

| Host surface | State | Where | Consequence for a GUS |
|---|---|---|---|
| **Audio mixer** | ✅ ready | `src/vdd/vdd_audio.h:90,103` (`vdd_audio_init`, `vdd_audio_mix`) | the GF1's mixed stereo output becomes one more source summed here, as the SB and OPL are |
| **8237 DMA** | ✅ ready | `src/vdd/vdd_dma.c:22` — all 8 channels, 16-bit ones address words | DRAM uploads can use a free channel |
| **8259 slave PIC** | ✅ modelled | `src/vdd/vdd_pic.c:165` (IRQ 8–15, cascade on IRQ2) | the chip can raise 11/12/15… |
| **Host device-IRQ delivery** | ✅ **all 16 lines (s80)** | `g_irqn_pending[16]`, `g_irq_order`, `irq_pm_vec` in `src/host/main.c`; proved by `tools/dostest/p_irq8.com` vs three oracles | IRQ 11 reaches a guest. ⚠ The IF/VIF gate lets a handler that EOIs before `iret` be re-entered — see [`pic.md`](pic.md) |
| **Port base** | ⚠ decision | SB at `220h` (`dos_env.h:98`, `BLASTER=A220 I5 D1 T3`) | the SDK default base `220` collides with the SB; the GUS needs another base (`240h` is the period's common choice) |

## Decisions to take before the first line of the device

1. **Resources — decided (user, 2026-09-25):** base `240h`, **IRQ 11** (slave delivery built for
   it, s80), a free DMA channel (3, or 5 for 16-bit). `ULTRASND=240,<dma>,<dma>,11,11`.
2. **Voice engine timing.** The GF1 produces one output sample per pass over the active voices
   (44.1 kHz at 14 voices, 19.3 kHz at 32). The model renders at the mixer's rate and must
   advance voices by *GF1* time, or pitch shifts with the voice count incorrectly.
3. **Order of work**, each step observable on heaven7:
   `ULTRASND` → reset + DRAM PIO (the SDK's detection passes) → register file + voices + volume
   (sound) → DMA uploads → IRQs (wavetable/volume/DMA TC, the FIFO) → timers → MIDI.
