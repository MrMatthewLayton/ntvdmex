# Inventory — Gravis UltraSound (GF1)

**Spec:** *UltraSound SDK v2.22* (Advanced Gravis / FORTE, 1994), Chapter 2 and the SDK's own
driver source. **▶ The hardware reference is [`../ref/gus.md`](../ref/gus.md)** — what the card
*does*.
**Our implementation:** `src/vdd/vdd_gus.c` (s80), `src/vdd/vdd_gus.h`; mixer source in `src/vdd/vdd_audio.c`; off-VM battery `tests/unit/gus_test.c` (76 checks; #190 added the latches, mix control, UART, record and card→PC rows).
**Acceptance guest:** heaven7 (its music is GUS-only; it renders already).
**Marked:** 2026-09-25 (s80), **from the code**; #190 rows re-marked 2026-09-29.

---

## Headline — heaven7 finds the card, fills it and plays it (s80)

Test machine, host `a0294462`: with `ULTRASND=240,3,3,11,11` in its environment, heaven7 resets the GF1
to `07h`, sizes and fills its DRAM by programmed I/O (66,272 pokes, 38,374 peeks), starts 132
voices and leaves 10 running; **96% of the 1.32 M samples rendered are non-zero, peak 17,871**.
It uses neither DMA nor interrupts — it polls. ⚠ Nobody has *heard* it yet: an unattended run
cannot. The by-hand test is owed.

⛔ Found on the way: the environment block is built **before** the devices, so the card must be
decided at startup — the first run had a GUS on the bus and no `ULTRASND=`, and heaven7 never
touched a port.

---

## The card

| Unit | ref | Status | Where / notes |
|---|---|---|---|
| `ULTRASND` environment string | §1, §8 | **IMPL** | `main.c` env build — the device's own numbers; a `dosenv.txt` `ULTRASND` wins. DOS path only: the Win16 block is left exactly as it was |
| Port claims `2X0–2XF`, `3X0–3X7` | §1 | **IMPL** | `vdd_gus_init` `vdd_gus.c:754`; `VDD_MAX_PORTS` 32 → 48 (31 were in use) |
| Register select / data, 8- and 16-bit access | §2 | **IMPL** | `gus_out`/`gus_in` `:284`/`:335`; a byte to `3X4` latches, `3X5` completes |
| Global registers `41h–4Ch` | §2.1 | **IMPL** | `gus_reg_write`/`gus_reg_read` `:180`/`:248` |
| Reset `4Ch` | §2.1 | **IMPL** | bit 0 = 0 runs `gus_chip_reset` `:161` |
| Voice registers `00h–0Eh` / `80h–8Eh` | §2.2 | **IMPL** | 32 banks via the page |
| Self-modifying bits | §2.2 | **IMPL** | the engine writes stopped/direction/pending itself; the double-write race is not modelled (it cannot lose a write here) |
| DRAM + PIO at `3X7` | §3 | **IMPL** | 1 MB (`GUS_DRAM_SIZE`); detection passes (`gus_test` T1) |
| DRAM DMA, 16-bit translation, invert-MSB | §3 | **IMPL** | `gus_dma_try` `:191` — instantaneous once the 8237 serves it; **a masked channel holds DRQ** and it is retried on each render / latch / 2X0 write (it used to 'complete' with no data, because `vdd_dma_remaining` is never 0) |
| DRAM DMA **card→PC** (41h bit 1 = 1) | §2.1, §3 | **IMPL** (#190) | `gus_dma_try` `:191` — DRAM bytes through `vdd_dma_write` (the guest's channel in *write* mode), TC + IRQ as for an upload; bit 7 inverts the MSB on the way out too (decision: a sign conversion undoes itself) |
| Voice engine: position, frequency, interpolation, end / loop / bidi / rollover | §4 | **IMPL** | `gus_voice_step` `:397`; rendered at the GF1's own rate (`vdd_gus_rate_hz`) and resampled by the mixer |
| Logarithmic volume, ramps | §7 | **IMPL** | `vdd_gus_vol_gain` `:372` — curve checked against the SDK table's ratios; `gus_ramp_step` `:424` |
| Pan | §7 | **IMPL** | #189 — `gus_pan_l`/`gus_pan_r` `:667`, a balance law, in `vdd_gus_render_st` |
| Latches `2XB`, the lock-out | §5 | **IMPL** (#190) | `gus_out` `2XB` `:435`, `gus_latch_decode` `:60` — the latch **drives** the lines: GF1 IRQ (bits 2–0), MIDI IRQ (5–3), bit 6 combine; DRAM DMA (2–0), record DMA (5–3), bit 6 combine. Code 0 = no line. Next-write lock-out (only GUS-port writes are seen, so a write to another card's port does not break the arm) |
| Latch state at power-on | §5 | **IMPL — decision** | `vdd_gus_reset` `:723`: the card **as ULTRINIT leaves it** — latches = ULTRASND's own numbers (combined when equal, as `UltraSetInterface` does), `2X0 = 09h`. Every DOS program meets a card its owner's AUTOEXEC already initialised; heaven7 never programs the board |
| `2XF` banks 5 and 6 | §5 | **IMPL** (#190) | bank 5: a write of 0 drops the asserted lines ("clear power-up IRQs"), so a still-pending source re-edges; bank 6 (jumper): bit 1 = MIDI port decode (off → `3X0/3X1` float `FFh`), bit 2 = joystick decode — **stored only**, the gameport at `201h` is its own device and is not gated. ⚠ bit positions as the SDK's rev-3.4 text; not re-read in this pass |
| Mix control `2X0` | §5 | **IMPL** (#190) | `gus_out` `:414`: bit 1 line out **off mutes the render** (voices keep running; `out_muted` counts it); bit 3 powers the IRQ/DMA drivers (off → no line, a DRQ waits); bit 4 combines the IRQs; bit 5 MIDI loopback; bit 6 latch select. Bits 0 (line in) and 2 (mic) are stored and reach nothing — **N/A**: there is no input device, the ADC hears silence either way |
| IRQ status `2X6` | §6 | **IMPL** | `gus_irq_status` `:67` |
| Voice IRQ FIFO `8Fh` | §6 | **IMPL** | `gus_irq_fifo` `:102`, cleared by the read, active-low bits |
| The card's interrupt lines | §6 | **IMPL** | `gus_irq_update` `:136`: edge per physical line on any enabled source, gated by 4Ch bit 2 **and** 2X0 bit 3; GF1 sources on the GF1 line, UART sources on the MIDI line (one line when combined) |
| Timers 1/2 and `2X8`/`2X9` | §9 | **IMPL** | `gus_timers` `:455`, advanced by rendered GF1 time |
| MIDI 6850 | §9 | **IMPL** (#190) | `gus_out`/`gus_in` `3X0`/`3X1`, `gus_midi_tx_irq`/`_rx_irq` `:82`: master reset (CR1–0 = 11), transmit IRQ only for CR6–5 = 01, receive IRQ CR7; status RDRF/TDRE/OVRN/IRQ; transmit is instant (each byte is a fresh TDRE edge) to **`gus_state.midi_sink`** (raw bytes); loopback fills RDRF (overrun on a second byte). 2X6 bits 0/1, on the latched MIDI IRQ. Receive from outside: **N/A** (no MIDI IN device). ⚠ **Host wiring owed** — see below |
| Record path (`48h`, `49h`) | §2.1 | **IMPL** (#190) | `gus_record` `:246`: a take runs card→PC on the **record** DMA channel at `9878400/(16·(48h+2))` Hz (stereo = 2 bytes/sample; a 16-bit channel moves pairs), paced by rendered GF1 time; bytes are midscale silence (`80h`, or `00h` with 49h bit 7). At TC the take stops (bit 0 dropped), 49h bit 6 + 2X6 bit 7 + IRQ if bit 5. Decision: stop at TC even on an auto-init channel |
| ICS-2101 mixer, CS4231 codec | §10 | **N/A** | later board options; `7X6` reads `FFh` = "pre-3.7 board" |

## Host wiring owed (#190)

The MIDI UART's `midi_sink` is **not connected** in `src/host/main.c` yet, so a GUS MIDI stream
is still silent on the test machine. The connection, reproduced by `gus_test` T11:

```c
static mpu_state g_gusmidi;                     /* PRIVATE assembler: never vdd_bus_add'ed */
static void gus_midi_to_synth(void *ctx, uint8_t b) { (void)ctx; vdd_mpu_feed(&g_gusmidi, b); }
...
g_gusmidi.sink = host_midi_sink;                /* the same synth as the MPU-401 */
g_gus.midi_sink = gus_midi_to_synth;            /* before vdd_bus_add(&g_gus_dev) */
```

A private `mpu_state`, not `g_mpu`: two byte streams sharing one assembler corrupt each other's
running status. `midi_sink` survives `vdd_gus_reset`. Worth adding to `gus_report`: `mix`,
`out_muted`, `midi_tx`, `samp_takes`, `dma_downloads` — **⚠ re-run heaven7 on the test machine**: if it
writes `2X0` with bit 1 set (line out off) it is now silent, as on a real card.

## What the card needs from the host — and what is not ready

| Host surface | State | Where | Consequence for a GUS |
|---|---|---|---|
| **Audio mixer** | ✅ ready | `src/vdd/vdd_audio.h:90,103` (`vdd_audio_init`, `vdd_audio_mix`) | the GF1's mixed stereo output becomes one more source summed here, as the SB and OPL are |
| **8237 DMA** | ✅ ready | `src/vdd/vdd_dma.c:22` — all 8 channels, 16-bit ones address words | DRAM uploads can use a free channel |
| **8259 slave PIC** | ✅ modelled | `src/vdd/vdd_pic.c:165` (IRQ 8–15, cascade on IRQ2) | the chip can raise 11/12/15… |
| **Host device-IRQ delivery** | ✅ **all 16 lines (s80)** | `g_irqn_pending[16]`, `g_irq_order`, `irq_pm_vec` in `src/host/main.c`; proved by `tests/probes/dos/p_irq8.com` vs three oracles | IRQ 11 reaches a guest. ⚠ The IF/VIF gate lets a handler that EOIs before `iret` be re-entered — see [`pic.md`](pic.md) |
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
