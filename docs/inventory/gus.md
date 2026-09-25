# Inventory — Gravis UltraSound (GF1)

**Spec:** *UltraSound SDK v2.22* (Advanced Gravis / FORTE, 1994), Chapter 2 and the SDK's own
driver source. **▶ The hardware reference is [`../ref/gus.md`](../ref/gus.md)** — what the card
*does*.
**Our implementation:** **none.** No `vdd_gus.c`; nothing in `src/` claims a GUS port.
**Acceptance guest:** heaven7 (its music is GUS-only; it renders already).
**Marked:** 2026-09-25 (s80), **from the code**.

---

## Headline — not a gap in a device, the device itself is absent

Every unit below is **MISS**. This file therefore does two jobs: it is the checklist the
implementation works through in the order [`ref/gus.md`](../ref/gus.md) sets out, and it records
the **host surfaces the card has to plug into** — because three of those are not ready for it
either, and one of them is the first thing a GUS program reads.

⛔ **heaven7 never touches a port.** Its run logs no unclaimed port at all
(`runs/s74_heaven7/`): the environment block carries `BLASTER=` but **no `ULTRASND=`**
(`src/dos/dos_env.h:98`), and a GUS program reads the variable *before* it probes (ref §1, §8).
So the first observable step is the environment string, not a register.

---

## The card

| Unit | ref | Status | Notes |
|---|---|---|---|
| `ULTRASND` environment string | §1, §8 | **MISS** | only `BLASTER=` is emitted (`dos_env.h:98`) |
| Port claims `2X0–2XF`, `3X0–3X7` | §1 | **MISS** | nothing registered on the bus (`src/vdd/vdd_bus.c`) |
| Register select / data (`3X2`–`3X5`), 8- and 16-bit access | §2 | **MISS** | the SDK uses 16-bit `OUT` to `3X4` for every word register |
| Global registers `41h–4Ch` | §2.1 | **MISS** | |
| Reset register `4Ch` (run / DAC / master IRQ) | §2.1 | **MISS** | |
| Voice registers `00h–0Eh`, reads at `80h–8Eh` | §2.2 | **MISS** | 32 voice banks, page at `3X2` |
| Self-modifying bits (stopped, direction, IRQ pending) | §2.2 | **MISS** | |
| DRAM (up to 1 MB) + programmed I/O at `3X7` | §3 | **MISS** | ⚠ also the SDK's *delay*: 7 reads of `3X7` |
| DRAM DMA (`41h`/`42h`), 16-bit address translation, invert-MSB | §3 | **MISS** | |
| Voice engine: position, frequency counter, interpolation, end/loop/bidirectional/rollover | §4 | **MISS** | output rate depends on the active-voice count |
| Logarithmic volume, ramps (rate/start/end, loop, IRQ) | §7 | **MISS** | curve pinned by the SDK's `_gf1_volumes` table |
| Pan (16 positions) | §7 | **MISS** | |
| Latches `2XB` via `2X0` bit 6, the next-write lock-out, `2XF` bank | §5 | **MISS** | |
| Mix control `2X0` (line out active-low, latch enable) | §5 | **MISS** | |
| IRQ status `2X6` | §6 | **MISS** | |
| Voice IRQ FIFO `8Fh`, cleared by reading | §6 | **MISS** | |
| Timers 1/2 (`45h–47h`) and the AdLib-compatible `2X8`/`2X9` | §9 | **MISS** | |
| MIDI 6850 at `3X0`/`3X1` | §9 | **MISS** | we have an MPU-401 (`src/vdd/vdd_mpu.c`); a 6850 is a different chip |
| Record path (`48h`, `49h`) | §2.1 | **MISS** | no input source exists; answer the registers, record silence |
| ICS-2101 mixer, CS4231 codec (UltraMax / daughter card) | §10 | **N/A** | later board options, out of the period base card |

---

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
