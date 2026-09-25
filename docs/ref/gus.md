# The Gravis UltraSound (GF1)

**Spec:** *UltraSound Software Development Kit v2.22* (Advanced Gravis / FORTE Technologies /
Ingenuity Software, 21 Dec 1994) — the manual `ULTRAWRD.DOC`, **Chapter 2 (Hardware
Information)**, cited below as **SDK §2.x**; and the SDK's own low-level driver source, cited by
file (`INIT.C`, `PEEKPOKE.C`, `VOL1.C`, `VARS.C`; the Pascal port `ULTRADRV.PAS` for the OS layer
the C kit ships only as a library). Publicly archived at
[github.com/RobertSundling/GUSDK222](https://github.com/RobertSundling/GUSDK222). **Not mirrored
here** — see [`SOURCES.md`](SOURCES.md).
**Companion:** [`../inventory/gus.md`](../inventory/gus.md) — what *we* do about it.

> **The thesis.** The GUS is not a DAC you stream to. It is **a synthesizer with its own
> memory**: the program copies samples into up to 1 MB of on-card DRAM once, then tells 14–32
> *voices* where in that DRAM to play from, how fast, how loud and where to pan — and the card
> mixes them itself. Almost everything that goes wrong in a GUS model is one of three things:
> the **indirect register file** (one select port, one data port, a voice page on top), the
> **self-modifying bits** the chip rewrites while the program is looking at them, or the
> **interrupt source FIFO** that is cleared by being read.

---

## 1. The ports — two blocks, and a sound-card base that is not one address  (SDK §2.1)

`X` is the jumper-selected digit: base `2X0`, so `X=4` is `240h`. The card decodes **two**
ranges from it — `2X0–2XF` and `3X0–3X7` — plus the joystick at `201h`.

| Port | R/W | What |
|---|---|---|
| `2X0` | W | **Mix control** (§5) — also the *gate* for the `2XB` latches |
| `2X6` | R | **IRQ status** — one bit per interrupt source (§6) ⚠ `2X6`, not `3X6` |
| `2X8` | R/W | Timer control — the AdLib-compatible timer's control register |
| `2X9` | W | Timer data |
| `2XB` | W | IRQ latch **or** DMA latch, chosen by `2X0` bit 6 (§5) |
| `2XF` | R/W | Register control: selects which of the `2XB` bank is live *(board rev 3.4+)* |
| `3X0` | R/W | MIDI control (W) / status (R) — a **6850 UART** |
| `3X1` | R/W | MIDI data |
| `3X2` | R/W | **Page** — the voice number the voice registers address |
| `3X3` | R/W | **Register select** |
| `3X4` | R/W | **Data low** — or a whole 16-bit register with a 16-bit I/O |
| `3X5` | R/W | **Data high** — or a whole 8-bit register |
| `3X7` | R/W | **DRAM data** — the byte at the DRAM I/O address (§3) |
| `7X6`, `3X6` | | ICS-2101 mixer / board revision *(rev 3.7+)*, UltraMax control — optional |

Software finds the card through the **`ULTRASND` environment variable**,
`ULTRASND=<base hex>,<DRAM DMA>,<record DMA>,<GF1 IRQ>,<MIDI IRQ>` — the SDK's defaults are
`220,1,1,11,5` (`ULTRADRV.PAS`, `UltraGetCfg`; SDK §7.2).
⚠ **The default base `220h` is the Sound Blaster's base.** A machine with both cards needs the GUS
elsewhere (`240h` is the common choice), and a model has to decide which card answers `2X0–2XF`.

## 2. The indirect register file  (SDK §2.5–§2.8)

Write the **voice number** to `3X2`, the **register number** to `3X3`, then read or write the
value at `3X4`/`3X5`. **8-bit registers live at `3X5`; 16-bit registers are either a 16-bit I/O
at `3X4` or low byte `3X4` + high byte `3X5`.** The SDK itself uses `OUTW` to `3X4` for every
16-bit register (`PEEKPOKE.C`, the Pascal reset in `ULTRADRV.PAS`), so a model that only handles
byte pairs misses the common case.

Voice registers are **written** at `00h–0Eh` and **read** at `80h–8Fh` — *add `80h` to read*.
The global registers are `41h–4Ch` (§2.1 below). `3X2` addresses the voice registers only.

### 2.1 Global registers  (SDK §2.6.1)

| Reg | R/W | Width | What |
|---|---|---|---|
| `41h` | R/W | 8 | **DRAM DMA control** — bit 0 go, 1 direction (1 = card→PC), 2 channel is 16-bit, 3–4 rate divisor (÷1…÷4), 5 TC IRQ enable, 6 **R: TC IRQ pending / W: data is 16-bit**, 7 invert MSB (signed↔unsigned) |
| `42h` | W | 16 | **DMA start address** — DRAM address bits 19–4 (so an 8-bit transfer starts on a 16-byte boundary, a 16-bit one on 32) |
| `43h` | W | 16 | **DRAM I/O address** bits 15–0 |
| `44h` | W | 8 | DRAM I/O address bits 19–16 |
| `45h` | R/W | 8 | Timer control — bit 2 timer 1 IRQ enable, bit 3 timer 2 IRQ enable |
| `46h`, `47h` | W | 8 | Timer 1 / timer 2 count — **counts up to FFh**; 80 µs and 320 µs per tick |
| `48h` | W | 8 | Sampling (record) frequency: `rate = 9878400 / (16 × (n + 2))` |
| `49h` | R/W | 8 | Sampling control — go, stereo, 16-bit channel, TC IRQ enable, **R: TC IRQ pending**, invert MSB |
| `4Bh` | W | 8 | Joystick trim DAC (default 29) |
| `4Ch` | R/W | 8 | **Reset** — bit 0 run (0 = held in reset), 1 DAC enable, 2 master IRQ enable. Normally `07h` |

**The 16-bit DMA address translation.** For a 16-bit DMA channel the start address is not the
DRAM address: the low 18 bits are halved and bits 18–19 kept —
`addr16 = ((addr >> 1) & 1FFFFh) | (addr & C0000h)` (`ULTRADRV.PAS`, `Convert_To_16Bit`; SDK
§2.6.1.2 calls it `convert_to_16()`). Voice addresses (§4) use the same translation for 16-bit
samples.

### 2.2 Voice registers  (SDK §2.6.2)

| W / R | Width | What |
|---|---|---|
| `00h`/`80h` | 8 | **Voice control** — 0 *stopped\**, 1 stop, 2 16-bit data, 3 loop, 4 bidirectional, 5 wavetable IRQ enable, 6 *direction\** (1 = decreasing), 7 *IRQ pending\** |
| `01h`/`81h` | 16 | **Frequency control** — bits 15–10 integer, 9–1 fraction: the amount added to the position per service |
| `02h`,`03h` | 16 | **Start** address: high word = address bits 19–7 (in bits 12–0); low word = bits 6–0 (in 15–9) + 4 fraction bits (8–5) |
| `04h`,`05h` | 16 | **End** address, same layout |
| `06h`/`86h` | 8 | Volume ramp **rate** — bits 5–0 step (1–63), bits 7–6 how often (§7) |
| `07h`,`08h` | 8 | Volume ramp **start / end** — `EEEEMMMM` |
| `09h`/`89h` | 16 | **Current volume\*** — bits 15–12 exponent, 11–4 mantissa, 3–0 extra ramp precision |
| `0Ah`,`0Bh` | 16 | **Current position\*** — high: bits 19–7; low: bits 6–0 (15–9) + **9 fraction bits** (8–0) |
| `0Ch`/`8Ch` | 8 | **Pan** — 0 full left … 15 full right |
| `0Dh`/`8Dh` | 8 | **Volume control** — 0 *ramp stopped\**, 1 stop ramp, **2 rollover**, 3 loop, 4 bidirectional, 5 ramp IRQ enable, 6 *direction\**, 7 *IRQ pending\** |
| `0Eh`/`8Eh` | 8 | **Active voices** (global despite being here): bits 7–6 set, bits 5–0 = voices − 1. **14 minimum** |
| `—`/`8Fh` | 8 | **IRQ source** (global, read-only) — see §6 |

**\* Self-modifying.** The GF1 is a pipeline that read-modify-writes each voice as it services
it; a bit it owns can change *between* the program's write and the chip's next pass. The
SDK's rule is **write those registers twice with a delay of at least 3 voice-times (≥ 4.8 µs)
between** (SDK §2.6.2). Its delay routine, `GF1_Delay`, is **seven reads of `3X7`**
(`ULTRADRV.PAS`) — so reading the DRAM data port must work and must take real bus time.

## 3. The DRAM  (SDK §2.6.1.3, §2.12, §2.6.1.1)

256 KB on the base card, up to 1 MB (20 address bits). Two ways in:

- **Programmed I/O:** set the address with `43h` (bits 15–0) and `44h` (19–16), then read or
  write the byte at `3X7`. Slow, byte-at-a-time — and it is **how software detects the card**
  (§8).
- **DMA:** program the PC's 8237 for the transfer, set `42h` to the DRAM address (bits 19–4, with
  the 16-bit translation on a 16-bit channel), then write `41h` with bit 0 set. Bit 5 asks for an
  interrupt at terminal count; bit 7 flips the sign bit on the way in (so unsigned PC data can
  be stored signed). **Reading `41h` returns TC-pending in bit 6 and clears it.**

The 20-bit DRAM is split into **256 KB banks** for 16-bit playback: a 16-bit voice cannot cross
one — that is what keeping bits 18–19 in the translation encodes.

## 4. How a voice plays  (SDK §1.4, §2.6.2.1–§2.6.2.12)

The GF1 services voice 0, 1, …, *(active − 1)*, 0, … and spends **1.6 µs per voice**. So each
voice is updated once every `1.6 µs × active` — with 14 voices that is **44.1 kHz**, with 32 it
is 19.3 kHz. The output rate is a function of the active-voice count; the frequency counter
has to be recomputed if it changes. The SDK's own formula: the service rate is
`1 000 000 / (1.619695497 × active)` Hz, and for a desired sample rate
`fc = ((rate << 9) + divisor/2) / divisor` with that service rate as the divisor (SDK §1.4).

On each service the voice's **position** (20.9 fixed point) moves by the frequency counter,
up or down per the direction bit. The sample is **linearly interpolated** between the two
DRAM samples the position lies between, to 16-bit precision even for 8-bit data. On reaching
**end** (or start, going down):

| Voice control | Volume control bit 2 (rollover) | Result |
|---|---|---|
| loop off | off | **stop** (sets *stopped*); IRQ if enabled — *and keeps re-raising it until stopped* |
| loop on | — | **wrap** to the other boundary (unidirectional) or **reverse** (bidirectional); IRQ if enabled |
| — | **on** | **carry on in the same direction**, IRQ if enabled — the ping-pong-buffer feature. Rollover beats loop |

⚠ Moving the end address below the current position of a playing voice raises the interrupt
at once, and because the high and low words are written separately the voice can see a
half-written end (SDK §2.6.2). Programs that stream through rollover depend on exactly that.

## 5. The latches: which IRQ and DMA the card drives  (SDK §2.13–§2.16; `INIT.C`)

The card's interrupt and DMA lines are selected by **write-only latches at `2XB`**, and **`2X0`
bit 6 decides which one the next `2XB` write reaches**: 1 = the IRQ latch, 0 = the DMA latch.
⚠ **The `2XB` write must be the very next I/O write after `2X0`**, or it is locked out — a
guard against probing software corrupting the latches.

| Latch | bits 2–0 | bits 5–3 | bit 6 |
|---|---|---|---|
| IRQ | GF1 IRQ: 1=IRQ2 2=5 3=3 4=7 5=11 6=12 7=15 | MIDI IRQ, same table, 0 = none | both on the GF1 line |
| DMA | DRAM DMA: 1=DMA1 2=3 3=5 4=6 5=7, 0 = none | record DMA, same table | both on one channel |

**Mix control `2X0`:** bit 0 **0 = line in enabled**, bit 1 **0 = line out enabled** (both
active-low), bit 2 mic in, **bit 3 enable the latches** (power to the IRQ/DMA drivers — never
turn it off again), bit 4 combine the GF1 and MIDI IRQs, bit 5 MIDI loopback, bit 6 latch select.

**Board rev 3.4+:** `2XF` selects a bank behind `2XB` — `0` the classic latches, `5` "write 0
to clear power-up IRQs", `6` the jumper register (MIDI/joystick decode enables).

The SDK's order (`INIT.C`, `UltraSetInterface`): `2XF←5; 2X0←mix; 2XB←0; 2XF←0;` then DMA latch
with bit 7 set, IRQ latch, DMA latch, IRQ latch, a write to `3X2` "to lock out writes", and
finally `2X0←mix|09h` (output on, latches on). **Model the lock-out and a program that follows
this exact sequence still has to work** — that is the whole test.

## 6. Interrupts  (SDK §2.9, §2.6.2.16; `ULTRADRV.PAS` reset)

`2X6` says **which unit** wants attention: bit 0 MIDI transmit, 1 MIDI receive, 2 timer 1,
3 timer 2, 5 **wavetable** (any voice), 6 **volume ramp** (any voice), 7 **DMA TC** (DRAM or
record). Nothing is delivered at all unless `4Ch` bit 2 (master IRQ enable) is set.

For the voices, **`8Fh` is a FIFO**: each read returns one pending voice event — bits 4–0 the
voice, **bit 7 = 0 means wavetable IRQ, bit 6 = 0 means volume IRQ** (active-low), bit 5
always 1 — and **reading it clears that voice's pending bits** in its control registers. A
handler reads `8Fh` repeatedly until both bits 6 and 7 read 1 (SDK §2.6.2.16).
⚠ A stopped-at-end voice with its IRQ enabled **re-raises** after being read; the SDK tells
the handler to ignore the repeat until it has stopped the voice or moved the end.

The SDK's reset clears everything by **reading**: `2X6`, then `41h`, `49h` and `8Fh`
(`ULTRADRV.PAS`, `UltraReset`). A model whose reads have no side effect leaves those pending.

## 7. Volume  (SDK §2.22, §3.7; `VOL1.C`)

Volume is **logarithmic**: a 4-bit exponent and an 8-bit mantissa, `EEEE MMMMMMMM` in the
current-volume register. The SDK's linear-to-log table (`VOL1.C`, `_gf1_volumes[512]`) pins
the curve: linear index 1 is `700h`, 2 is `7FFh`, 3 is `880h`, 5 is `940h` — **each doubling
of amplitude is one exponent step (6 dB) and the mantissa is linear within the octave**, i.e.
amplitude ∝ 2^E × (256 + M) / 256. Zero is silence. The SDK warns never to leave an unused
voice at a non-zero volume: every voice is summed.

**Ramps.** Program the current volume, the start and end (`EEEEMMMM`, start < end — direction
comes from the volume-control direction bit), and the rate: the step (bits 5–0, added to the
12-bit current volume) is applied every `FUR`, `FUR/8`, `FUR/64` or `FUR/512` voice-loops by
rate bits 7–6, where FUR is one pass over the active voices. At the end the ramp stops, loops or
reverses exactly like a voice, and can raise the volume IRQ. The SDK's table: a full-scale ramp
takes 1.4 ms (fastest, 14 voices) to 107 s (slowest, 32 voices). Ramping to the rails with a big
step overshoots and oscillates; the SDK keeps clear of the ends.

**Pan** is 16 positions, 0 left to 15 right, per voice.

## 8. How software finds the card  (`INIT.C`: `UltraProbe`, `UltraPing`)

1. Read `ULTRASND` for the base.
2. Hold the GF1 in reset (`4Ch ← 0`), wait, release it (`4Ch ← 1`), wait.
3. **Poke `AAh` into DRAM byte 0 and `55h` into byte 1 through `43h`/`44h`/`3X7`, read both
   back**, restore them. Both match ⇒ a GUS is there.

That is the whole contract a model must meet to be *found*: the reset register and
programmed-I/O DRAM. Everything else is needed to be *heard*.

## 9. Timers and MIDI  (SDK §2.2–§2.4, §2.10–§2.11, §2.6.1.4–5)

**Timers.** Two up-counters, 80 µs and 320 µs a tick, loaded via `46h`/`47h`, raising IRQs
(enabled in `45h`) when they pass `FFh`. `2X8`/`2X9` are the **AdLib-compatible** timer
interface — write `4` to `2X8` to select it; `2X9` bits 0/1 start timers 1/2, bits 6/5 mask
them, bit 7 clears the timer IRQ. That compatibility is how AdLib *detection* code finds a
timer at the GUS base.

**MIDI.** A 6850 ACIA at `3X0`/`3X1` — control bits 0–1 master reset, bits 5–6 transmit-IRQ
enable, bit 7 receive-IRQ enable; status bit 0 receive full, 1 transmit empty, 4 framing error,
5 overrun, 7 IRQ pending. Reading or writing data clears the IRQ.

## 10. What this document deliberately leaves out

- The **ICS-2101 mixer** (rev 3.7+, `3X6`/`7X6`, SDK Ch. 5) and the **CS4231 codec** of the
  UltraMax and the 16-bit daughter card (SDK Ch. 6). They are later options, not the GF1.
- The **analogue** output stage — gain, filtering, the line/amp outputs. The digital contract
  ends at the mixed sample.
- The **joystick** at `201h` — it is the standard gameport, already its own surface.
- The SDK's *software* (patch format, MIDI mapping, ULTRAMID). That is a driver, not the card.
