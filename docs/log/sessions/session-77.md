# Session 77 — the 8254 closes with BCD; PCem was never the blocker; a parity score scored against one oracle

> 2026-09-23. Branch `m9/completeness`. Commits `3b98752`, `8dee588`, `7570528`.

The session's theme is one mistake in three dresses: **treating one host's answer as the
answer**. It closed the PIT, settled a VGA register that had been "confirmed" wrong twice,
and invalidated a number this project had been quoting.

---

## 1. The 8254 is done — BCD, fix 5 (`3b98752`)

Control Word bit 0 was *stored and never consumed*: a counter programmed for four-decade
BCD went on counting in binary, and a written count of `0000` meant 65536 where the guest
had asked for 10000.

Implemented as a **boundary format, not a second arithmetic**. Everything inside the device
stays binary — `reload`, the latch, the count laws, the IRQ0 divisor — and a count is
decoded where it arrives through `40h`–`42h` and encoded where it leaves. The counting
element is then literally the same code in both bases, which is the point: a second
arithmetic path is a second thing to get wrong and only one of the two would ever be
exercised. Three behaviours fall out of `pit_wrap()` instead of being special-cased — the
`0000` maximum, mode 0 wrapping back to 9999 rather than through `0xFFFF`, and the
speaker's divisor. The binary path is bit-identical.

### Why this one was allowed to ignore the oracles

`pit.bcd.valid`: 6.22 `0`, **PCem `0`**, dosbox-x `1`. Two of three — *including the
real-BIOS machine* — do not implement the bit at all.

> **A `0` from a host that does not implement a feature is the absence of a measurement,
> not a measurement of absence.** The majority here was a majority of omissions.

The inventory had it parked as *"blocked on PCem; implementing from the datasheet would be
writing an expectation from memory"*, and that **conflated two different things**: the rule
is never write an expectation from **memory**, and Intel 231164-005 is a **cited source**.
So: implemented from the datasheet, marked *spec-implemented / unverifiable*, with an
abstention rule in `oracle-rules.json` that prints its rationale on every run.

### The evidence had to move off the probe

`pit.bcd.valid` asks a **property** — are all four nibbles decimal — which is exactly the
question a binary counter answers correctly one time in six, and did, on this project,
once. `pit_test.c` **T12** pins exact counts instead: `0x9989` ten clocks after `0x9999`
(the units decade *borrows*, it does not step to `0x8F`), `0x0000` at 9999, the wrap at
10000, IRQ0 on the ten-thousandth clock of a `0000` count, and **the same counter in binary
failing the nibble test** — the negative control, without which the positive one proves
nothing. **8 of the 14 failed against the previous code; the 6 that passed were the
coincidences.**

⇒ **Five fixes, zero mismatches against oracle consensus. The surface is closed.**

---

## 2. PCem runs unattended, and always could have

Rows had been parked on *"PCem needs the WindowServer / needs ROMs / user's terminal
only"* for several sessions, and **all of it was mine**: the XPC/Swift crash was the
command sandbox, and its data directory is `~/PCem/`, not the Application Support path our
own notes gave — so the ROMs were installed where it never looks. The last piece was a
standing rule, *"I PREPARE, THE USER CLICKS, I READ"*, generalised from **one** failed
launch. Two `⛔ user's terminal only` claims were sitting in `STATE.md` and `SOURCES.md` on
the strength of it.

```
PCEM_CFG=NTVDMEX-DOS622.cfg python3 scripts/pcemoracle.py run build/probes/X.com
```
~60 s, boot to `#END`, outside the command sandbox. It ran `p_vgaext` and a full
twelve-mode `p_vgareg` back to back this session.

⚠ `build/dosdiff` is the **dosbox-x host's scratch mount and is wiped every run** — a probe
built there survives a PCem-only sweep and vanishes the moment dosbox-x joins it. Build
into `build/probes`.

---

## 3. Input Status 0 is `0x10` (`8dee588`, `7570528`)

`p_vgareg` asks the wrong **shape** of question about these two registers: it reads each
port once and prints the byte. A byte is a value; what was in dispute was a *mechanism*, and
four hosts gave four values with nothing to choose between them. `p_vgaext` asks about the
mechanism, with every expectation written into its header **before** the first run.

| case | 6.22/QEMU | dosbox-x | **PCem (real AMI + IBM VGA)** | predicted |
|---|---|---|---|---|
| `is0.live` — AND and OR of 65536 reads | `0000` | `7070` | **`1010`** | **`1010`** ✅ |
| `is0.vsync` — CR11 enable, retrace, bit 7 | `0000` | `0001` | `0000` | `0001` ❌ |
| `fc.store` — write `00`/`0F`/`08`, read `3CA` | `00`×3 | `00`×3 | `FF`×3 | — |

**✅ `0x10`, bit 4 Switch Sense, and the prediction was exact.** Every host answers a
*constant*, so the shape was never wrong — only the value. PCem says `0x10` in all twelve
modes. ⛔ **This row had been recorded as "confirmed `0x00`" twice, on one oracle each
time.**

**❌ Bit 7, the CRT interrupt: prediction falsified.** PCem's IBM VGA never raises it.
Recorded as a gap rather than built — two of three hosts against, and no DOS guest this
project has met uses the VGA vertical interrupt.

**Feature Control cannot be adjudicated by anything we have.** Two hosts accept the write
and discard it; PCem's `0xFF` is an undecoded port floating high. Three ways of not having
the register. We keep the spec's read-back: *spec-implemented, unverifiable* — the same
footing as BCD.

⚠ And the four-host table that motivated the probe **was itself wrong**: dosbox-x's Feature
Control is `0x00`, not the `0x70` recorded — that is its Input Status 0 value copied into
the row below, the two bytes being adjacent in the buffer they were read off by eye.
*A hand-transcribed table is a claim.*

---

## 4. ⛔⛔⛔ The parity score was scored against one oracle

`tools/vgaparity.py` reported **"689/768 bytes, 89.7%"** against `vgareg.ref.txt` — 6.22
**under QEMU**, i.e. the Bochs VGABIOS, not period-correct firmware. With PCem available,
the same probe under a real AMI BIOS and a genuine IBM VGA ROM gives a second reference, and

> **the two oracles disagree with each other on 78 of 768 bytes** — the same order as the
> error the score was reporting.

So a byte we "failed" may have been us matching real hardware, and a byte we "passed" may
have been us matching an anachronism. **The number could not tell the two apart, and it was
quoted as though it could.** `vgaparity.py` now scores only the bytes both oracles agree on
and reports the disputed ones with how we answer each. The hand-maintained DAC-pixel-mask
carve-out is deleted — the general rule covers it, and PCem says `0xFF`, which is what we
say.

### Two defects in `p_vgareg`, both of which manufactured data

1. **The mono row picked its port from the mode number, not Miscellaneous Output bit 0** —
   violating `ref/vga.md`'s own thesis that a mode *is* the register values. On PCem, whose
   BIOS leaves MiscOut `0x67` for mode 7, it read `3B4`, found nothing, and reported 51 of
   64 bytes as disagreeing. None of it meant anything.
2. **The mono row was contaminated by the hand-programmed cases before it.** Mode X ends
   with `CR11 = 0xAC` — write protect on, `CR00`–`CR07` refused — so the following BIOS mode
   set could not do its job. Two captures a day apart disagree: the recorded reference
   carried Mode X's `CR09`, a fresh run of the **same binary** carried nine more of Mode X's
   registers. **A row that does not reproduce is not evidence**, and it was being diffed
   across hosts as though it were.

⚠ The old reference's per-group form was quietly lossy as well: its loader filled only
offsets 0 and 4–63, so Feature Control, Input Status 0 and the DAC Pixel Mask were read as
**zero whatever the file said**. They happened to be `0x00` on that host, so nothing caught
it. Both references re-captured as whole buffers.

---

---

## 5. The rig answered, and the gap was one defect wearing five hats (`104c4ed`, `730e31d`)

The watcher's heartbeat was live, so both probes went through it rather than waiting on a
human. `p_vgaext` came back **`1010`** — byte for byte with PCem. Parity came back **92.0%**
(635 of the 690 agreed bytes), and **the 55 misses were five registers, not scatter**:
`CR0A`/`CR0B` in all twelve modes, `AR10` in eleven, `GR07`, `GR05`, `SR02`.

**`VGA_MODEDEFS` already held the right values, and `vga_load_modedef` already loaded
them.** The gap was that six of those registers are **not read back from the file at all** —
the port read answers from a *live shadow*, because the shadow is what the rendering engine
uses. The mode set wrote the file, the file was right, and the guest saw the old value.
Reproduced off-VM first, matching the rig byte for byte, so the whole fix loop ran without
the rig. ⇒ **689/690, 99.9%.**

⚠ One observable change rides with it: `cur_shape` was the hard-coded 8-line CGA `0x0607`
where the table says `0x0D0E`. `vdd_cursor_lines` already rescales an 8-line shape, so the
drawn cursor moves by **one scan line**.

### The tie-break rule needed an exception on its first outing

`gen-vgamodedefs.py` now merges both references — agreement emitted, disagreement resolved
to PCem and **listed by name** in the generated header. It earned its keep immediately:
`GR7` agrees at `0x0F` in the graphics modes but splits in text/CGA, QEMU `0x0F` against
PCem's real IBM VGA `0x00`. Regenerating from QEMU alone would have written `0x0F` into the
text modes **against the real card — and it would have looked like a fix, because parity
would have moved.**

⛔⛔ **But "PCem wins" cannot be applied blind.** `INT 10h AX=0007` gives MiscOut `0x66` on
QEMU — a genuine MDA-compatible mode 7 — and `0x67` with a **40-column CRTC** on PCem, whose
BIOS does not enter mode 7 on that machine. Taking PCem there would have programmed a
40-column colour text setup for every mode-7 request, from **30 tie-broken bytes, every one
of them "measured"**. Mode 7 is now excluded by name.

> **An oracle is only authoritative about the question it actually answered.**

⛔ **One byte left, recorded rather than bundled:** `modeX` `CR0F`. Same defect class —
`crtc_in` *derives* the cursor address where the hardware register is storage. The honest
fix makes the BIOS cursor calls write `CR0E`/`CR0F`, touching every path that moves the
cursor: too large a blast radius to ride along with a change that already needs a by-hand
check.

---

---

## 6. The 8259A — a reference, a marking from the code, and two fixes (`d5fc5f3`)

The inventory read *"10 fields, all AGREE"* — in the retired PARITY vocabulary, with a
note that re-marking against the code was owed. **That is exactly the state the 8254 was
in before it turned out to have seven gaps.**

> **An all-AGREE probe is not a verified surface. It is a verified list of questions.**

Marking it from the code predicted five gaps, and **none of them was among the ten
questions the probe asked.** `docs/ref/pic.md` is new — the three registers and the one
rule, ICW1's six side effects, the OCW2/OCW3 encodings, poll, SMM, SFNM, the cascade trap.

Three new probe cases, and the *evidence* behind the two fixes differs in a way worth
keeping:

| case | 6.22/QEMU | dosbox-x | PCem | ours |
|---|---|---|---|---|
| `pic.ocw2.rot.speoi` | `0000` | `0000` | `0000` | `0001` → **`0000`** ✅ |
| `pic.ocw3.poll` | **`0000`** | `0001` | `0001` | `0001` → **`0000`** ✅ |
| `pic.icw1.readsel` | `0000` | `0000` | `0001` | `0000` ⛔ open |

**1. `E0h` is still an EOI — unanimous, so no judgement was needed.** We had it under
*"other rotate/priority forms: nop"* and did neither half. The rotation half stays
missing, and that is the right half to be missing: an ISR bit that is never cleared does
not cost one interrupt, it kills that priority level and everything below it.

**2. Poll — implemented on a 1-vs-2 split.** Only QEMU models it. The other two drop the
P bit, so their `0x01` is *the absence of a measurement* — the BCD shape again, but
**better evidenced than BCD**, because there the sole implementer had no corroboration
and here it agrees with the datasheet. It was worth doing on a split because of the
failure *shape*: a poll read and a status read are the same `IN` on the same port, so
dropping P fails silently and plausibly.

**3. ICW1's read-select reset — found, measured, and deliberately not fixed.** All three
answer `AH = 0x00`, but only PCem discriminates; the other two clear the IRR on ICW1, so
both registers are zero and the read cannot say which it returned. **One oracle is not a
pass.**

⚠ **The first cut of that case was measuring the wrong thing**, and it is worth keeping:
it restored the real IMR straight after the ICW sequence, unmasking IRQ0, so the latched
request was *delivered* before the read. It was measuring interrupt latency. PCem's
answer survived the fix, so the split is real.

⚠ `pic_test.c` **could not reach the port side at all** — the handlers are static and its
bus stub threw them away, so every question it could ask was about the host-side API,
which is the half that already worked. 6 of the new checks failed against the old code.

## State at session end

- `offvm` **1480/0** (+36: 14 BCD, 3 external registers, 9 mode-set, 10 PIC).
- **VGA register parity 99.9%** (689/690 of the two-oracle agreement). *Re-run, never quote.*
- `bin\` = `5b51036d`. Stable zip `dist\ntvdmex-20260917-4847355.zip` **untouched**;
  checkpoint `59fac7d`.
- **User confirmed by hand:** Doom and Skyroads, after the Input Status 0 change.
- **Owed from a human:** a by-hand look after the mode-set fix — the one-scan-line cursor
  move is the only visible part of it.
