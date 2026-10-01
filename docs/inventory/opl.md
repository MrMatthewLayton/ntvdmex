# Inventory — Yamaha OPL2 (YM3812) / OPL3 (YMF262) FM synthesiser (`388h`–`38Bh`)

**Spec:** Yamaha *YM3812 Application Manual* (register map, WSE, timers, rhythm); Yamaha
*YMF262 (OPL3) datasheet* (second register array, NEW, 4-operator connection figure,
waveforms 4–7, CHA–CHD outputs); Creative *Sound Blaster 16 Hardware Programming Reference*
(OPL3 at `2x0`–`2x3`, the `(status & 06h) == 0` OPL3 detect). None is held in the repository.
**Oracle:** Nuked-OPL3, used strictly as a black box through `tools/oplref/oplprobe.c`
(fetched on demand into `build/oplref/`, never linked into the host, its source never read for
values). No OPL3 experiment has been run against it yet — see "Owed" below.
**Our implementation:** `src/vdd/vdd_opl.h`, `vdd_opl.c` (register file, ports, timers),
`vdd_opl_synth.c` (synthesis), `vdd_audio.c` (mixing), `vdd_sb.c` (the SB's FM mirrors).
Off-VM: `opl_test.c` (64 checks), `opl_synth_test.c` (41), `sb_test.c` T11/T11b,
`audio_test.c` (the #232 block).
**Marked:** 2026-09-29 (#232), **from the code**.

**Chip select:** `opl_state.opl3` — 0 = YM3812, 1 = YMF262. Survives `vdd_opl_reset`
(`vdd_opl.c` `vdd_opl_reset`). ✅ **The host sets it** (re-checked 2026-10-01, `588409a`): the
`Opl` setting (`OPL2|OPL3`, default OPL3, `settings.h:235`) is applied live,
`g_opl.opl3 = (SET_OPL == 1)` (`main.c:10983`).

---

## Headline — the OPL3 exists; three of its details are inferences owed a measurement

The second array, the single 9-bit latch, NEW gating, 4-operator pairing and algorithms,
waveforms 4–7, and CHA/CHB stereo are all modelled, and an OPL2 (or an OPL3 with NEW clear)
renders **bit-identically** to the build before any of it existed — held by golden checksums
(`opl_synth_test.c` T7, `audio_test.c`). What is not yet oracle-checked: the slope of waveform 7,
whether `38Ah` reads status on a real SB16, and whether an array-1 voice keyed with NEW clear is
really silent on silicon (the model follows the datasheet's reading: array 1 is an OPL3
extension, and the extensions are gated on NEW).

---

## 1. Ports

| Unit | Spec | Status | Where | Verification |
|---|---|---|---|---|
| `388h` W — address, array 0 | A1=0 A0=0 | **IMPL** | `vdd_opl.c` `opl_out` → `vdd_opl_write_addr` | `opl_test.c` (every `wr`) |
| `389h` W — data | A0=1; writes the latched address | **IMPL** | `vdd_opl_write_data` | ″ |
| `38Ah` W — address, **array 1** | OPL3 only (A1=1) | **IMPL** | one 9-bit latch, A1 is bit 8 | `opl_test.c` T13 |
| `38Bh` W — data | OPL3 only | **IMPL** | same data path as `389h` (latch decides the array) | `opl_test.c` T13 ("one 9-bit latch") |
| `388h` R — status | IRQ/T1/T2 in bits 7–5; ID in bits 2–1 | **IMPL** | `vdd_opl_read_status`: OPL2 `06h`-idle, OPL3 `00h`-idle | `opl_test.c` T4, T11, T12 |
| `38Ah` R | OPL3: status (A1 not decoded on a read) | **PART** | ⚠ inference — the datasheet's read cycle names only A0 | `opl_test.c` T12 |
| `389h`/`38Bh` R | write-only | **IMPL** | read `FFh` | `opl_test.c` T9, T12 |
| `38Ah`/`38Bh` on an **OPL2** | not decoded (an AdLib stops at `389h`) | **IMPL** | writes dropped, reads `FFh` | `opl_test.c` T11 |
| SB `2x0`/`2x1`, `2x8`/`2x9` | array 0 | **IMPL** | `vdd_sb.c` `sb_out`/`sb_in` | `sb_test.c` T11, T11b |
| SB `2x2`/`2x3` | OPL3 array 1; SB Pro 1 right OPL2 | **PART** | OPL3: array 1 + status. OPL2: `2x2` is an array-0 mirror; the SB Pro 1's second OPL2 is not modelled | `sb_test.c` T11b |
| no chip (`nosb.flag`) | bus floats | **IMPL** | `g_opl_absent` → status `FFh` (also through the SB mirrors now) | — |

## 2. Array-0 globals

| Reg | Unit | Status | Where | Verification |
|---|---|---|---|---|
| `01h` b5 | WSE — waveform select enable (**OPL2 only**; YMF262 `01h` is LSI test) | **IMPL** | `vdd_opl_synth.c` `opl_eff_wave` — OPL2 with WSE clear plays sine | `opl_synth_test.c` T11 |
| `02h`/`03h` | timer 1 / 2 preset | **IMPL** | `vdd_opl_write_reg` | `opl_test.c` T5, T6 |
| `04h` | timer control / IRQ reset | **IMPL** | ″ | `opl_test.c` T4–T8 |
| `08h` | CSM, NTS (key-split select) | **STORE** | NTS not honoured: KSR always uses F-num bit 9 (`opl_eff_rate`) | — |
| `BDh` | AM/VIB depth, rhythm, drum keys | **PART** | bass drum + tom-tom synthesised; snare/hi-hat/cymbal counted, silent (`opl_rhythm_sample`) | `opl_synth_test.c` T7 (golden) |

## 3. Array-1 globals (OPL3)

| Reg | Unit | Status | Where | Verification |
|---|---|---|---|---|
| `104h` b0–5 | 4-op pairs 0+3, 1+4, 2+5, 9+12, 10+13, 11+14 | **IMPL** | `opl_4op_role`; not timer control | `opl_test.c` T13–T15 |
| `105h` b0 | NEW — OPL3 extensions live | **IMPL** | `vdd_opl_new_mode`; cleared by reset | `opl_test.c` T14, T17; `opl_synth_test.c` T8 |
| `101h`–`103h`, `108h`, `1BDh` | no function in array 1 | **N/A** | stored, ignored | — |

## 4. Per-operator (`20h`–`F5h`, and `120h`–`1F5h` on an OPL3)

| Reg | Unit | Status | Where | Verification |
|---|---|---|---|---|
| `20h` | AM, VIB, EGT, KSR, MULT | **IMPL** | `vdd_opl_write_reg` | `opl_test.c` T2, T13 |
| `40h` | KSL, TL | **IMPL** | ″ | `opl_test.c` T2 |
| `60h` / `80h` | AR/DR, SL/RR | **IMPL** | ″ | `opl_test.c` T2 |
| `E0h` | waveform: 2 bits (OPL2 w/ WSE; OPL3 NEW=0), 3 bits (OPL3 NEW=1) | **IMPL** | 3 bits latched, masked at render (`opl_eff_wave`) | `opl_synth_test.c` T11 |
| waveforms 4–7 | double-rate sine, camel, square, derived square | **PART** | `opl_wave`; ⚠ wave 7's slope (×2 per 32 phase steps) is not in the datasheet and is owed a measurement | `opl_synth_test.c` T11 (4 and 6 by shape) |

## 5. Per-channel (`A0h`–`C8h`, and `1A0h`–`1C8h`)

| Reg | Unit | Status | Where | Verification |
|---|---|---|---|---|
| `A0h`/`B0h` | F-number, block, key-on | **IMPL** | `opl_key_channel`; a 4-op pair keys from its first channel, the second's key is ignored | `opl_test.c` T3, T15 |
| `C0h` b0 | CNT; with the partner's CNT picks the 4-op algorithm | **IMPL** | `opl_voice2`, `opl_voice4` | `opl_synth_test.c` T10 (all four algorithms) |
| `C0h` b1–3 | feedback (op 1 only in a 4-op voice) | **IMPL** | ″ | `opl_synth_test.c` T4 |
| `C0h` b4/b5 | CHA → left, CHB → right (NEW only) | **IMPL** | `opl_route`; a 4-op voice routes by its first channel | `opl_synth_test.c` T9, `audio_test.c` |
| `C0h` b6/b7 | CHC/CHD — not wired on an SB16/AWE32 | **N/A** | dropped: a voice routed only there is silent | `opl_synth_test.c` T9 |

## 6. Output

| Unit | Status | Where | Verification |
|---|---|---|---|
| mono render (`vdd_opl_render`) | **IMPL** | OPL2 / NEW=0 unchanged; NEW=1 folds (L+R)/2 | `opl_synth_test.c` T7, T9 |
| stereo render (`vdd_opl_render_st`) | **IMPL** | interleaved L/R at 49716 Hz | `opl_synth_test.c` T7–T12 |
| mixer | **IMPL** | `vdd_audio.c` takes the OPL as a stereo source | `audio_test.c` (golden `6CA12225h`, left-only voice) |

---

## Owed

- **Wave 7's slope**, **`38Ah` read**, **array-1 voice with NEW clear**: an `oplprobe` experiment
  each (black-box, one variable), then a real SB16 if one is available.
- The register-trace hook (`opl_state.trace`) is 8-bit: array-1 writes are not captured.
- ~~Host: set `opl3` from `SET_OPL`; call `vdd_opl_all_notes_off` on program exit.~~ ✅ Done in
  `588409a` (`main.c:10983`; program exit `main.c:4142`, both banks and the rhythm drums).
