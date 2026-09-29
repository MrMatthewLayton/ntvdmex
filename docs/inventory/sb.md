# Inventory — Sound Blaster (DSP + mixer)

**Spec:** Creative Labs, *Sound Blaster Series Hardware Programming Guide* — the DSP command
table, the SB Pro (CT1345) and SB16 (CT1745) mixer maps, the DMA/IRQ contract. ⚠ **Not held
in the repo**: [`../ref/SOURCES.md`](../ref/SOURCES.md) names it and links no copy, and there is
no `docs/ref/sb.md` yet. Commands marked *undoc.* below are not in that guide; for them an
oracle (PCem's SB16, dosbox-x) is the only spec.
**Our implementation:** `src/vdd/vdd_sb.c` (612 lines), `src/vdd/vdd_sb.h`; gain and output in
`src/vdd/vdd_audio.c`; transport through `src/vdd/vdd_dma.c`; card resources, knobs and
`BLASTER` in `src/host/main.c` and `src/dos/dos_env.h`.
**Off-VM:** `tools/dostest/sb_test.c` (T1–T12), `tools/dostest/audio_test.c` (T3–T6, the #189
stereo blocks). **DOS probe: none** — there is no `p_sb*` in `tools/dostest/`.
**Marked:** 2026-09-29, **from the code**, with citations. ⚠ `src/host/main.c` was being edited
in the working tree while this was written; its line numbers are given with the symbol and
will drift — search for the symbol.

⚠ **Every row below is at most *untested* on the verification axis.** Both batteries were
written against our own model, so they pin what the code does, not what a card does — the
trap [`dma.md`](dma.md) records for `dma_test.c`. Where a test pins a unit it is named; that
is a regression guard, not an oracle.

---

## Headline

**The playback transport is solid and the rest of the card is a subset shaped by three
guests.** Everything Doom's DMX, ZAR's Miles driver and Skyroads use — the reset handshake,
the version, 8-bit single-cycle and auto-init, the SB16 `Bx`/`Cx` programmed transfers, SB16
and SB Pro stereo, mixer `80h`–`82h` — is implemented and measured on the rig (below). What is
missing is the rest of the DSP command table, and the way it is missing is dangerous:
**`sb_cmd_args()` returns 0 for every command it does not know** (`vdd_sb.c:80`), so an
unmodelled command that takes argument bytes has those bytes **executed as commands**. And
the mixer is a 256-byte RAM of which three registers are read, by their high nibble only.

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 Ports | 13 | 7 | 4 | — | — | 2 |
| §2 Reset, detection, the command parser | 3 | 2 | 1 | — | — | — |
| §3 DSP commands | 43 | 8 | 16 | 1 | 17 | 1 |
| §4 Mixer registers | 30 | — | 8 | 19 | 2 | 1 |
| §5 Transfer engine, IRQ, DMA | 16 | 10 | 3 | — | 3 | — |
| §6 Model variants and configuration | 7 | 2 | 2 | 1 | 1 | 1 |
| **Total** | **112** | **29** | **34** | **21** | **23** | **5** |

### What the rig has measured on this surface

| Measurement | Where |
|---|---|
| Doom (DMX, DSP 4.05 path): SB16 `C6` 8-bit auto-init, mode byte `20h` (**stereo**), 256-byte blocks = 128 frames; ~3600 blocks / 45 s at 11025 Hz, underruns 0 | `docs/log/sessions/session-22.md:32`, `:272` |
| Doom: the DMX ISR asks mixer `82h` and refills only if bit 0/1 is set; ~28% of delivered IRQs were once turned away there | `vdd_sb.c:233-244`, `vdd_sb.h:43-53` |
| Doom: 32% of non-flat blocks replayed one ring lap earlier (`REPLAYED_LOUD=933` of 3662 checked) — a refill-margin race in the host, not a DSP-register defect | `session-24.md:188-196` |
| ZAR (Miles `SBLASTER.DIG`): reads mixer `80h`/`81h`, then a 16-byte `14h` single-cycle self-test at `40 D3`; answered `00h` it waited forever. Fixed by deriving both from the card's configuration | `session-59.md:103-122` |
| ZAR: 349 blocks, 268 with signal, `REPLAYED_LOUD=0`, 270 IRQ-5 reflections, none failed | `session-81.md:131-137` |
| #189 stereo: Doom L/R correlation 0.932; SB Pro stereo via mixer `0Eh` bit 1 and `90h`/`91h` | `session-82.md:21`, `:27` |
| #213 (ZAR silent) was the PIC acknowledge order, **not** the SB — recorded so it is not re-blamed here | `session-83.md:16-26` |

---

## 1. Ports

The card claims `base`..`base+0Fh` only (`vdd_sb.c:609`). Base, IRQ and DMA come from one
struct, `g_sbcfg`, which also writes `BLASTER` (`main.c` `g_sbcfg` ≈`:10322`; `dos_env.h:101`).

| Port | Access | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|---|
| `2x0`/`2x1` | R/W | FM address/data — SB Pro left OPL2, SB16 OPL3 bank 0 | **IMPL** | array-0 address / data (`vdd_sb.c` `sb_out`, #232); `2x0` read is OPL status incl. the chip-ID bits (`sb_in` → `vdd_opl_read_status`); `2x1` read returns `FFh`. The chip itself: [opl.md](opl.md) | `sb_test.c` T11b |
| `2x2`/`2x3` | R/W | FM — SB Pro **right** OPL2, SB16 OPL3 **bank 1** | **PART** | #232: with an OPL3 fitted, `2x2` addresses **array 1** and reads status; with an OPL2, `2x2` stays an array-0 mirror and reads `FFh` — the SB Pro 1's second (right) OPL2 is not modelled | `sb_test.c` T11b |
| `2x4` | W / R | mixer index | **PART** | write latches (`:197`); read falls to `FFh` (`:304`). Whether the real index reads back is an oracle question | untested |
| `2x5` | R/W | mixer data | **IMPL** | transport only; each register is marked in §4 (`:198`, `:230-293`) | `sb_test.c:125-126` |
| `2x6` | W | DSP reset | **IMPL** | 1 then 0; the falling edge queues `AAh` (`:199-215`) | `sb_test.c:40-46`, `:83` |
| `2x7`, `2xB`, `2xD` | — | unassigned | **N/A** | not decoded by the card; read `FFh` (`:304`) | — |
| `2x8`/`2x9` | R/W | FM, AdLib-compatible mirror of `388h`/`389h` | **IMPL** | `:191-196`, status read `:227-229` | `sb_test.c:171-176` (T11) |
| `2xA` | R | DSP read data | **IMPL** | 8-byte queue (`SB_OUTQ_MAX`, `vdd_sb.h:57`); `FFh` when empty (`vdd_sb.c:18`) | `sb_test.c:86` |
| `2xC` | W | DSP command / data | **IMPL** | `sb_dsp_write` `:170-182` | all of `sb_test.c` |
| `2xC` | R | write-buffer status, bit 7 = busy | **IMPL** | always `00h`, never busy (`:295`) — a permitted state of the part; the busy period after each byte is not modelled | untested |
| `2xE` | R | read-buffer status, bit 7 = data; **8-bit IRQ acknowledge** | **IMPL** | `FFh`/`7Fh` (`:296-299`); acks only when the last transfer was 8-bit | `sb_test.c:84`, `:127-128` (T6) |
| `2xF` | R | **16-bit IRQ acknowledge** (DSP 4.xx) | **PART** | clears the one pending flag whatever raised it (`:300-303`), so it also acknowledges an **8-bit** IRQ; reads `FFh` | untested |
| `2x10`–`2x13` | R/W | CD-ROM interface (SB Pro / SB16 with an on-board CD port) | **N/A** | not claimed (`:609`); no CD drive is modelled | — |

## 2. Reset, detection, and the command parser

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| Reset handshake: `1` → `0` → `2xE` bit 7 set → `2xA` = `AAh` | **IMPL** | `vdd_sb.c:199-215`. Answered instantly; the guide's ~100 µs is a maximum a guest waits, so this is inside it | `sb_test.c:83-86` (T1); Skyroads sweeps `210h`–`260h` for it (`sb_test.c:3-6`) |
| What a reset clears: pending command, queue, transfer, pause, pending IRQ | **IMPL** | `sb_dsp_soft_reset` `:36-44`. (High-speed and MIDI UART modes would also end here; neither exists — §3.) Mixer untouched, as on the card | `sb_test.c:179-183` (T12) |
| The command/argument parser | **PART** | ⛔ `sb_cmd_args` (`:68-82`) knows the argument count of 12 commands and returns **0 for every other**. An unmodelled command that takes arguments — `24h`, `38h`, `74h`–`77h`, `E2h`, the ASP set — leaves its argument bytes to be **decoded as fresh commands**: a MIDI note-on byte sent through `38h` is `90h`, which starts a high-speed DMA transfer (`:123`). Nothing replies for a command that should, so a guest polling `2xE` for the reply waits on its own timeout | untested |

## 3. DSP commands

*Gen* is the DSP generation that introduced the command (1.xx SB, 2.0x SB 2.0, 3.xx SB Pro,
4.xx SB16). ⚠ **No command is refused by generation**: with `dspver.txt` claiming 2.01, every
4.xx command below is still accepted (§6).

| Cmd | Gen | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|---|
| `04h` `05h` `0Eh` `0Fh` | 4.xx ASP | ASP/CSP status and register access | **N/A** | only on SB16 ASP/CSP boards; we claim a plain SB16. ⚠ Their argument bytes still fall through the parser (§2) | — |
| `10h` | 1.xx | direct DAC, one 8-bit sample | **MISS** | argument consumed (`:72`), sample **discarded** (`:109-110`); timer-driven direct-DAC playback is silent | — |
| `14h` | 1.xx | 8-bit single-cycle DMA output | **IMPL** | `:118-122`; stereo follows mixer `0Eh` bit 1 | `sb_test.c:104-120` (T5); `audio_test.c:99-111` |
| `16h` | 1.xx | 2-bit ADPCM single-cycle output | **PART** | length parsed (`:73`), then **played as 8-bit PCM** (`:118`) — noise, at 4× the byte rate's worth of samples | untested |
| `17h` | 1.xx | 2-bit ADPCM single-cycle, with reference byte | **PART** | as `16h`; the reference byte is played as a sample | untested |
| `1Ch` | 2.0x | 8-bit auto-init DMA output (length from `48h`) | **IMPL** | `:123-127` | `sb_test.c:130-141` (T7); `audio_test.c:113-131` |
| `1Fh` | 2.0x | 2-bit ADPCM auto-init, with reference | **MISS** | `default:` `:165` | — |
| `20h` | 1.xx | direct ADC, one sample | **MISS** | no reply is queued | — |
| `24h` | 1.xx | 8-bit single-cycle DMA **input** | **MISS** | its two length bytes are not consumed (§2) | — |
| `2Ch` | 2.0x | 8-bit auto-init DMA **input** | **PART** | ⛔ **wrong direction**: grouped with `1Ch` as an OUTPUT (`:123`), so a recording program's ring is read and played instead of written | untested |
| `30h`–`33h` | 1.xx / 2.0x | MIDI read — polling, interrupt, with timestamp | **MISS** | | — |
| `34h`–`37h` | 2.0x | MIDI UART mode (poll / interrupt / timestamp) | **MISS** | ⛔ after this, every `2xC` byte is MIDI data until a reset; we decode each as a DSP command | — |
| `38h` | 1.xx | MIDI write, one byte | **MISS** | argument not consumed (§2) | — |
| `40h` | 1.xx | time constant, `256 − 1000000/rate` | **IMPL** | `sb_rate_from_tc` `:61-65`, `:133-135`; for SB Pro stereo the rate is halved into frames (`:575-576`) | `sb_test.c:99-100` (T4); `audio_test.c:339-366` |
| `41h` | 4.xx | output sample rate, big-endian | **IMPL** | `:136-138` | `sb_test.c:101-102`; `audio_test.c:134-136` (T6) |
| `42h` | 4.xx | input sample rate | **PART** | written into the one `rate_hz` the **output** uses (`:136-138`); there is no input path to consume it | untested |
| `45h` / `47h` | 4.xx | continue 8-bit / 16-bit auto-init (undo `DAh`/`D9h`) | **MISS** | `default:` | — |
| `48h` | 2.0x | DMA block size for auto-init and high-speed | **IMPL** | `:139-141` | `sb_test.c:134` (T7) |
| `74h` / `75h` | 1.xx | 4-bit ADPCM single-cycle (without / with reference) | **MISS** | length bytes not consumed (§2) | — |
| `76h` / `77h` | 1.xx | 2.6-bit ADPCM single-cycle | **MISS** | length bytes not consumed | — |
| `7Dh` / `7Fh` | 2.0x | 4-bit / 2.6-bit ADPCM auto-init, with reference | **MISS** | | — |
| `80h` | 1.xx | silence for *n* samples, then IRQ | **PART** | length consumed (`:77`); nothing is timed and **no IRQ is ever raised** — a driver that uses a silent block to test its IRQ waits forever | untested |
| `90h` | 2.01–3.xx | high-speed 8-bit auto-init | **PART** | plays (`:123-127`); the high-speed **lockout** — the DSP accepts no command until a reset — is not modelled | `audio_test.c:339-366` |
| `91h` | 2.01–3.xx | high-speed 8-bit single-cycle | **PART** | `:128-132`; same missing lockout | untested |
| `98h` / `99h` | 2.01–3.xx | high-speed auto-init / single-cycle **input** | **MISS** | | — |
| `A0h` / `A8h` | 3.xx | mono / stereo input mode | **MISS** | no input path | — |
| `B0h`–`BFh` | 4.xx | 16-bit programmed DMA, mode byte + length | **PART** | output single/auto-init, signed (bit 4) and stereo (bit 5) IMPL (`:95-107`). **Input (bit 3) silently goes idle with no IRQ** (`:104`); the FIFO bit is ignored; undefined opcodes in the range (bit 0 set) are accepted | `sb_test.c:151-163` (T9) |
| `C0h`–`CFh` | 4.xx | 8-bit programmed DMA, mode byte + length | **PART** | as `Bx` | `audio_test.c:318-337` (`C6h`, stereo) |
| `D0h` | 1.xx | halt 8-bit DMA | **PART** | one `paused` flag for both widths (`:142`) — it also halts a 16-bit transfer | `sb_test.c:143-147` (T8) |
| `D1h` / `D3h` | 1.xx | speaker on / off | **STORE** | `st->speaker` (`:143-144`) is read by nothing (`vdd_sb.h:82`). Right for the SB16 we claim (the guide gives these no effect on 4.xx output); wrong under `dspver.txt` < 4, where speaker-off mutes the DAC | untested |
| `D4h` | 1.xx | continue 8-bit DMA | **PART** | same shared flag (`:145`) | `sb_test.c:148-149` (T8) |
| `D5h` / `D6h` | 4.xx | halt / continue 16-bit DMA | **PART** | same shared flag (`:146-147`) — `D5h` halts an 8-bit transfer | untested |
| `D8h` | 2.0x | speaker status (`00h` off, `FFh` on) | **MISS** | no reply | — |
| `D9h` / `DAh` | 4.xx / 2.0x | exit 16-bit / 8-bit auto-init after this block | **PART** | either one ends whichever transfer is running (`:148-150`); the end-of-block behaviour itself is right | `audio_test.c:335`, `:363` (used, not asserted) |
| `E0h` | 2.0x | DSP identification: reply `~arg` | **IMPL** | `:151-153` | `sb_test.c:94-96` (T3) |
| `E1h` | 1.xx | DSP version, two bytes | **IMPL** | `:154-157`; 4.05 by default, `dspver.txt` overrides (§6) | `sb_test.c:88-92` (T2) |
| `E2h` | undoc. | DMA identification: a computed byte written by DMA | **MISS** | argument not consumed (§2); Creative's own drivers use it to test DMA wiring | — |
| `E3h` | undoc. | copyright string | **PART** | a single `00h` (`:158-160`); the real DSP returns Creative's copyright text, NUL-terminated | untested |
| `E4h` | undoc. | write test register | **PART** | argument consumed (`:79`), value **discarded** | untested |
| `E8h` | undoc. | read test register | **MISS** | no reply, so the `E4h`/`E8h` loop-back test fails | — |
| `F1h`, `F8h`, `FBh`–`FDh` | undoc. | auxiliary / DSP status reads | **MISS** | no reply; what each answers is an oracle question | — |
| `F2h` | 1.xx | force 8-bit IRQ | **IMPL** | `:161-164` | `sb_test.c:165-168` (T10) |
| `F3h` | 4.xx | force **16-bit** IRQ | **PART** | shares `F2h`'s arm; mixer `82h` then reports bit 0 or bit 1 according to the **last transfer's** width (`:245`), not this command's | untested |

## 4. Mixer registers

The mixer is `uint8_t mix[256]` (`vdd_sb.h:132`): every index latches and reads back
(`vdd_sb.c:198`, `:292`). Only `22h`, `04h` and `26h` (gain, `vdd_audio.c:10-19`) and `0Eh`
bit 1 (stereo, `vdd_sb.c:120`) are consumed; `80h`–`82h` are answered from state.

| Reg | Card | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|---|
| `00h` | Pro, 16 | mixer reset (any write) | **MISS** | stored like any other byte; nothing is reset | — |
| `02h` `06h` `08h` | SB 2.0 CD (CT1335) | master / FM / CD | **N/A** | the SB 2.0 CD-interface mixer; not a card we claim | — |
| `04h` | Pro, 16 | voice (DAC) volume, L/R nibbles | **PART** | gain from the **high (left) nibble only**, applied to both channels (`vdd_audio.c:15`, `:156`). ⛔ A value of `00h` is taken as "never programmed" and plays at 12/15 (`:17`), so a guest cannot mute by writing zero | `audio_test.c:113-125` (level, not the register) |
| `0Ah` | Pro, 16 | mic volume | **STORE** | no input path | — |
| `0Ch` | Pro | input source and input filter | **STORE** | | — |
| `0Eh` | Pro | output filter bypass (bit 5), **stereo switch** (bit 1) | **PART** | bit 1 selects stereo for `14h`/`1Ch`/`90h`/`91h` (`vdd_sb.c:120`, `:125`, `:130`); bit 5 — no output filter is modelled | `audio_test.c:339-366` |
| `22h` | Pro, 16 | master volume, L/R nibbles | **PART** | high nibble only (`vdd_audio.c:14`); `00h` = "never programmed", as `04h` (`:16`). Power-up `CCh` (`vdd_sb.c:595`) | untested |
| `26h` | Pro, 16 | FM volume | **PART** | high nibble only (`vdd_audio.c:15`, `:140`); `00h` plays at 12/15; power-up value **not** set, reads `00h` | untested |
| `28h` | Pro, 16 | CD volume | **STORE** | no CD audio source | — |
| `2Eh` | Pro, 16 | line volume | **STORE** | no line input | — |
| `30h`/`31h` | 16 | master L/R (5-bit) | **STORE** | ⛔ read by nothing — a driver that sets volume through the SB16 registers changes nothing we play | — |
| `32h`/`33h` | 16 | voice L/R | **STORE** | as `30h` | — |
| `34h`/`35h` | 16 | MIDI (FM) L/R | **STORE** | as `30h` | — |
| `36h`/`37h` | 16 | CD L/R | **STORE** | | — |
| `38h`/`39h` | 16 | line L/R | **STORE** | | — |
| `3Ah` | 16 | mic | **STORE** | | — |
| `3Bh` | 16 | PC speaker level | **STORE** | the speaker has its own fixed level (`vdd_audio.c:83-87`) | — |
| `3Ch` | 16 | output mixer switches | **STORE** | | — |
| `3Dh`/`3Eh` | 16 | input mixer switches L/R | **STORE** | | — |
| `3Fh`/`40h` | 16 | input gain L/R | **STORE** | | — |
| `41h`/`42h` | 16 | output gain L/R | **STORE** | | — |
| `43h` | 16 | mic AGC | **STORE** | | — |
| `44h`/`45h` | 16 | treble L/R | **STORE** | | — |
| `46h`/`47h` | 16 | bass L/R | **STORE** | | — |
| `80h` | 16 | IRQ select | **PART** | read derived from `st->irq` (`vdd_sb.c:277-284`) — right; a **write** is stored and ignored, so a guest cannot move the IRQ. IRQ 11, a dialog choice (`main.c` `settings_apply` ≈`:10379`), has no encoding and reads `00h` | ZAR (`session-59.md:103-122`) |
| `81h` | 16 | DMA select | **PART** | read derived (`:285-290`); write ignored. Reports DMA 5 (`22h`) while `BLASTER` advertises no `H` by default (`dos_env.h:59-63`) | ZAR (same) |
| `82h` | 16 | IRQ status: bit 0 8-bit, bit 1 16-bit, bit 2 MPU-401 | **PART** | bits 0/1 from **one** pending flag and the last transfer's width (`:245`) — both cannot be set at once; bit 2 never set. Board-revision bits not modelled | `sb_test.c:125-126` (T6); Doom (`vdd_sb.c:233-244`) |
| SB Pro ↔ SB16 aliasing | 16 | `22h`/`04h`/`26h`/`28h`/`2Eh` map onto `30h`–`39h` and back | **MISS** | two independent bytes each | — |
| Power-up values | Pro, 16 | the guide's reset values for every register | **PART** | only `22h` and `04h` = `CCh` (`vdd_sb.c:595-596`); all others read `00h` | untested |
| Unassigned indices | — | what a read of an undefined register returns | **STORE** | reads back whatever was written (`:292`); the real answer is an oracle question | — |

## 5. Transfer engine, IRQ and DMA

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| 8-bit data on the `D` channel | **IMPL** | `sb_fetch_sample` `vdd_sb.c:315` | `sb_test.c` T5, T7 |
| 16-bit data on the `H` channel, in words | **IMPL** | `:315`, `:362-364`; word addressing in `vdd_dma_cur_phys` `vdd_dma.c:24-29` | `sb_test.c:151-163` (T9) |
| 16-bit data through the 8-bit channel (SB16 with no separate 16-bit channel) | **MISS** | a 16-bit transfer always uses `dma16` (default 5, `vdd_sb.h:37`) | — |
| DSP block count independent of the 8237's terminal count | **IMPL** | `block_left` (`vdd_sb.c:375`, `:477`) | `sb_test.c` T7 |
| Single-cycle: one IRQ, then idle | **IMPL** | `:566` | `sb_test.c:111-120` (T5); `audio_test.c:108-111` |
| Auto-init: IRQ per block, keep streaming | **IMPL** | `:565` | `sb_test.c:137-141`; `audio_test.c:128-131` |
| Sample formats: unsigned 8-bit (legacy), signed 8-bit (mode bit 4), signed 16-bit LE | **IMPL** | `:362-370` | `sb_test.c:113-117`, `:162` |
| SB16 stereo (mode byte bit 5), L then R | **IMPL** | `:102`, `:364-369` | `audio_test.c:318-337` |
| SB Pro stereo: mixer `0Eh` bit 1, time constant counts both channels | **IMPL** | `:111-131`, `vdd_sb_frame_hz` `:575-576` | `audio_test.c:339-366` |
| Pacing: bytes leave the ring at the programmed rate | **IMPL** | drained by the host mixer at `vdd_sb_frame_hz` (`vdd_audio.c:157-159`), i.e. on the host audio clock, not guest CPU time | `audio_test.c:99-111`, `:360-362` |
| Completion IRQ raised on the configured line | **IMPL** | `:561-564` | `sb_test.c:118-119` |
| DMA not being serviced (channel masked, or 8237 stopped at TC before the DSP block ends) | **PART** | ⛔ a short fetch is treated as the **end of the block** (`:342`, `:477`) and **raises the IRQ at once**; the real DSP holds DREQ and waits, with no interrupt. A self-test that starts the DSP before unmasking the channel gets a completion it should not | untested |
| One pending interrupt per width | **PART** | a single `irq_pending` flag (`vdd_sb.h:94`) serves 8- and 16-bit; see `2xF`, `F3h`, `82h` | untested |
| A block that ends while the last IRQ is unacknowledged | **PART** | a fresh `vdd_raise_irq` every block regardless (`:564`), and `sb_test.c:141` asserts it. ⚠ The comment at `:27-31` says a real SB16 does exactly this; VDMSound's SB16 logic (quoted at `:398-400`) stops processing bytes instead. Which is right is an **oracle question**, and the opt-in ACK gate (§6) is the other answer | `sb_test.c:141` (our model) |
| Recording: ADC → memory through `vdd_dma_write` | **MISS** | no input path at all; `24h`/`2Ch`/`98h`/`99h`/`Bx`/`Cx` input are MISS or wrong (§3). Even a silent take that completes with its IRQs, as the GUS record path does ([`gus.md`](gus.md)), is absent | — |
| Rate limits per DSP generation | **MISS** | `41h`/`40h` accept any value (`:134`, `:137`); nothing is clamped to the generation's range | — |

## 6. Model variants and configuration

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| Resources: base `220/240/260/280`, IRQ `5/7/10/11`, DMA `1/3/5`, one struct for the card and `BLASTER` | **IMPL** | `main.c` `settings_apply` ≈`:10379`; applied at `g_sb.base = g_sbcfg.base` ≈`:26822`; `dos_env_blaster` `dos_env.h:101` | untested |
| DSP version (`cfg\dspver.txt`, default 4.05) | **PART** | changes the `E1h` reply (`vdd_sb.c:26`, `:155-156`; read in `main.c` near `DSPVER_PATH`) and nothing else: an "SB 2.01" still accepts every 4.xx command and answers mixer `80h`–`82h`. It does choose the guest's driver path (DMX: `vdd_sb.h:43-53`) | Doom (`vdd_sb.h:43-53`) |
| `SbModel` setting (`SB16|AWE32|SB Pro`) | **STORE** | `settings.h:217`; `SET_SBMODEL` is **read by nothing** in `src/` — the dialog offers three cards and every choice is the same one | — |
| AWE32: EMU8000 wavetable (`6x0h`, `Ax0h`, `Ex0h`) | **PART** | the chip is modelled (`src/vdd/vdd_emu8k.c`, #233 — register by register in [emu8k.md](emu8k.md)) but **not on the host's bus yet**: no `E620` in `BLASTER`, not registered in `main.c` | `emu8k_test` (off-VM) |
| `BLASTER` `T` and `H` | **PART** | `T3` (an SB 2.0) against a 4.05 DSP, recorded and left deliberately (`dos_env.h:71-77`); `H` omitted by default (`dos_env.h:59-63`) while mixer `81h` reports DMA 5 | Doom confirmed against this string (`dos_env.h:71-76`) |
| No card fitted (`cfg\nosb.flag`): reset withholds `AAh` | **IMPL** | `vdd_sb.c:205-213`; read at `main.c` near `NOSB_PATH` | untested |
| ACK gate (`cfg\sbgate.txt`) | **N/A** | a deliberate deviation from the hardware, **off by default** (`vdd_sb.c:27-33`, `:396-462`) | — |

---

## Gaps worth closing, in order

1. **Give the parser the whole command table** (§2). Argument count and reply length for every
   command in the guide, and a no-op that *consumes its bytes* where the action is absent. Today
   one unmodelled command can start a DMA transfer out of its own argument; this is the
   cheapest fix here and it removes a class of plausible wrong answers.
2. **A DMA channel that is not serviced must stall the DSP, not complete it** (§5). The early
   IRQ is exactly the answer an init-time self-test is looking for, delivered for the wrong
   reason.
3. **Make the mixer a mixer** (§4). `00h` reset with the guide's power-up values; the SB16
   `30h`–`47h` set consumed, and aliased to the SB Pro registers; both nibbles/channels; and
   `00h` treated as zero, not as "never programmed". A guest that mutes or sets volume the SB16
   way currently changes nothing.
4. **Two interrupt sources, not one flag** (§1, §3, §4, §5). Separate 8- and 16-bit pending
   bits: `2xE` and `2xF` each acknowledge only their own, `F3h` sets the 16-bit one, `82h`
   reports both and bit 2 for the MPU-401. Settle the unacknowledged-block question on an oracle
   before touching the edge behaviour.
5. **The older playback paths** (§3): direct DAC `10h` into the output, the ADPCM decoders
   (`16h`/`17h` are played as PCM noise today; `74h`–`77h`, `1Fh`, `7Dh`/`7Fh` absent), `80h`
   silence with its IRQ, the high-speed lockout.
6. **Input, even silent** (§5): fix `2Ch` (played as output), and let every input transfer
   write silence through `vdd_dma_write` and raise its IRQs, so a program that records does not
   hang.
7. **Make the model variants real** (§6): `SbModel` is a dead setting; `dspver.txt` should
   select a command and mixer set, not just an `E1h` reply; `BLASTER`'s `T`/`H` should follow
   the model (a measurement on the rig first — Doom is confirmed against today's string).
8. **MIDI through the DSP** (`30h`–`38h`, UART mode) and the identification set (`E2h`, the
   `E3h` string, `E4h`/`E8h` loop-back, `D8h`, `45h`/`47h`, the undocumented status reads).
9. **The CD-interface and AWE32 surfaces** are the last in line: `2x10h`–`2x13h` with no drive,
   and the EMU8000.
10. **An oracle probe** — `p_sb.asm`: reset timing, `E1h`/`E0h`/`E3h`/`E4h`→`E8h`, the idle values
    of `2xC`/`2xE`/`2xF`/`2x4`, the mixer's power-up bytes, `82h` after `F2h` and after `F3h`,
    and an unacknowledged auto-init block — asked of PCem's SB16 and dosbox-x, with every
    output poisoned first. Until it exists, nothing on this page is better than *untested*.
