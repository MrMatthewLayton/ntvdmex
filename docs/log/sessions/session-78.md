# Session 78 — the 82077AA: an absent chip whose float value meant *"wait"*

> 2026-09-23. Branch `m9/completeness`. Commit `ea485e7`.

One surface, taken through the loop the directive asks for: name the spec, enumerate the
units, mark from the code, probe, diff against oracles, fix, check by hand. The surface was
the floppy controller, and it turned out to be the **third** *"firmware present, chip
absent"* hole in a row — after the 8042's A20 gate and the MC146818.

It is also the one where that split had the sharpest consequence, and where **three
separate mistakes of mine were caught by the instruments rather than by luck.**

---

## 1. What was there, and what was not

The BIOS layer was complete and correct. INT 13h reads and writes sectors out of a real
image file (`main.c:24736`), the geometry is measured against 6.22 rather than assumed,
and both firmware advertisements agreed a drive was fitted — CMOS byte `10h` = `40h`, and
INT 11h's equipment word bit 0.

Underneath it, nothing claimed `3F0h`–`3F7h`. Every port fell through to the unclaimed
default of `FFh`.

### Why `FFh` is the worst possible answer for this one chip

`3F4h` is the **Main Status Register**, and it is the register a driver polls before every
single byte in either direction. `FFh` is `RQM=1` *and* `DIO=1` — *"I am ready, and I am
the one talking"*. The command-write loop, which is written this way in every BIOS and
every driver because it is written this way in the datasheet:

```asm
wait:   in al, 3F4h
        and al, 0C0h
        cmp al, 80h         ; RQM=1, DIO=0 -> it wants a command byte
        jne wait
```

`C0h` never equals `80h`. **It spins for ever** — no fault, no timeout, nothing in any log.

This is the MC146818's UIP bit one surface later: an absent chip whose float value happens
to mean *wait*. And the symmetry is worth keeping, because it kills the obvious repair:
**`00h` hangs too** (RQM clear is also *wait*). The only non-hanging absent value would be
`80h`, which is a different lie. ⇒ **There is no bus default that saves this. Only the
chip.**

A second, quieter one sat at `3F7h`: DIR bit 7 is DSKCHG, so `FFh` meant *the disk has
been changed* on every access, for ever — which makes a drive look permanently unreliable
rather than absent, and is much harder to attribute.

⚠ **Why nothing had noticed in 77 sessions.** Our guests reach the drive through INT 13h,
which never touches the ports. The programs that go direct were never on the shelf. Per
the scope rule that is not a reason to defer — it is the *reason* this went unmeasured.

---

## 2. The probe, and designing around a probe that can destroy its own evidence

`p_fdc.asm` asks the questions a **detection routine** asks. The safety problem was the
sharpest yet: the run writes `OUT.TXT` to the scratch floppy on two of the four hosts, and
**the floppy is the chip being probed** — worse than the 8237 case, because the FDC is not
a register file but a **conversation with no framing and no timeout**. A half-issued
command eats the BIOS's next command byte as one of its own parameters, and there is no
resynchronisation short of a reset, which is itself one of the things that can kill the
drive.

So: no write to DOR, DSR or CCR at all; no command that moves a head, transfers data or
changes configuration; and only two commands issued in the whole probe — **VERSION** and
**DUMPREG**, both stateless.

⛔ **SENSE INTERRUPT STATUS was deliberately not asked**, although it would say most about
reset behaviour. It *consumes* a pending interrupt, and if the BIOS had one outstanding
from the read that loaded the program, taking it hands the BIOS a disk operation that
never finishes. *The command worth most is the one that can eat state somebody else is
about to read.*

**The desync guard.** Every command is refused unless MSR says idle-and-ready; every result
phase is drained by the rule that needs no length table (read while `RQM=1 && DIO=1`, stop
when `CMD BSY` clears); and if a bounded drain expires, every later command is refused
rather than made worse. A refusal emits `FFFF`, which is not confusable with an answer.

⚠ **The guard fired on its own on the host with no chip**, which is the behaviour it was
designed for: `FFh & D0h` is not `80h`, so nothing was ever written to `3F5h` on NTVDMEX,
and `fdc.desync` measured that rather than assuming it.

---

## 3. What the machines said

Two oracles — MS-DOS 6.22 under QEMU, and PCem with a real AMI 486 BIOS — and this is the
**first surface where they agreed on every question that has an answer**.

| case | 6.22/QEMU | PCem | **before** | **after** |
|---|---|---|---|---|
| `fdc.msr.idle` | `0080` | `0080` | `00FF` | **`0080`** ✅ |
| `fdc.cmdwait` — *does the loop exit?* | `0080` | `0080` | **`01C0`** ⛔ | **`0080`** ✅ |
| `fdc.dor.gate` | `000C` | `000C` | `000C` ⚠ | **`000C`** ✅ |
| `fdc.dir.dskchg` | `0000` | `0000` | `0080` ⛔ | **`0000`** ✅ |
| `fdc.version` | `0190` | `0190` | `FFFF` | **`0190`** ✅ |
| `fdc.dumpreg` | `0A01` | `0A01` | `FFFF` | `0A00` ⚠ |
| `fdc.alt.3f6` | `0050` | `0050` | `00FF` | `00FF` ⛔ |

★★★ **`fdc.cmdwait` is not a register value.** It is the datasheet's own loop, bounded to
65536 turns and asked whether it terminated; `AH=01` means it did not. **That is the row
the surface existed for, and it is the row that moved.** It is worth noting how few of this
project's probes ask a question shaped like that — most ask *what does this byte read*, and
a byte that reads wrong is a much smaller problem than a byte that means *keep waiting*.

⇒ **dosbox-x reproduced the hang independently** (`01C0`), which is a third voice on the
failure and not just on the value.

---

## 4. Three things I got wrong, and what caught each

### ⛔⛔ A mask can manufacture an agreement out of an absent device

`fdc.dor.low` masked DOR to bits 3:0. `FFh & 0Fh` is `0Fh` — out of reset, gated, drive 3
— a **perfectly plausible DOR**. So NTVDMEX, which had no register there at all, scored
`000F` against PCem's `000F`, and the row read as a **MATCH**.

The inventory already carries *"a property check passes by luck; pin exact values"*. This
is its second half: **check what the absent-device value becomes after your mask.** `FFh`
survives almost any narrowing as something that looks like data. The case now emits the
raw byte, where `FFh` cannot hide, and narrows the adjudicable question to the two bits
that are a property of the chip (`/RESET` and `DMAGATE`) rather than of the driver.

*Caught by:* re-reading my own diff rather than the verdict column.

### ⛔ A command length table is a framing decision, not a lookup

`09h` **WRITE DELETED DATA** sits in a gap between `08h` and `0Ah`, and no detection
routine ever issues it. I left it out. Omitted, it becomes an *"invalid command"* that
consumes **one** byte — after which its eight parameters are read as **eight more
commands**. Getting one entry wrong does not produce one wrong answer; it desynchronises
everything after it.

`11h` (SCAN EQUAL) and `18h` (a National part-ID command) were the **opposite** error:
µPD765/PC8477 commands the 82077AA does not have, on a part that answers `90h` to VERSION
and therefore claims it does.

*Caught by:* re-reading the datasheet's command table against my `switch` after writing it.

### ⛔⛔⛔ My own test discarded its guard

`driver_send()` returns 0 when a send *would have hung*. The first version of `fdc_test.c`
**ignored the return value.** So when I checked the `09h` fix by putting the bug back, the
test **passed anyway**: the chip executed on byte one, flipped to result phase, all eight
parameter sends were silently refused, and the drain then found the 7 result bytes the
check was looking for.

> **A guard that returns success is a lie the whole stack repeats — including when I am
> the one who wrote the guard.** The standing rule was about `install.bat`. It applies to
> test scaffolding exactly as hard, and scaffolding is where nobody looks.

Now counted and asserted (`g_send_fail`). With `09h` removed the check fails; with it
present, 37/37 green.

*Caught by:* the habit of breaking the fix to confirm the test bites. **That step is the
only reason this was found**, and it is worth stating plainly: a green battery written
alongside the code it tests proves nothing until you have seen it go red.

---

## 5. What was implemented

`src/vdd/vdd_fdc.{c,h}` — the register file, the three-phase command protocol, and every
command that does not move sector data: VERSION, SENSE INTERRUPT STATUS (all three of its
cases), SPECIFY, CONFIGURE, PERPENDICULAR, LOCK, SENSE DRIVE STATUS, DUMPREG, RECALIBRATE,
SEEK, READ ID. IRQ6, gated on DOR bit 3.

★ **MSR is derived, never stored.** Every bit of it is a statement about state that lives
somewhere else, so a stored copy is a second opinion waiting to drift — the lesson the
8254's OUT pin taught, and a stronger rule here because MSR *is* the protocol.

⛔ **The data commands are PART, and marked PART.** READ/WRITE/READ TRACK/FORMAT are
recognised, consume their parameters, and terminate with the documented **abnormal
termination** (ST0 code `01`, ST1 *no data*, seven result bytes, the interrupt a real part
raises). They do **not** answer *"invalid command"*: that would be a chip contradicting
the `90h` it just gave for VERSION, and a driver can act on a media error where it cannot
act on a controller that contradicts itself. `fdc_test.c` §14 asserts the gap **so that
closing it cannot be silent.**

**Dormant by default**, the property that made the RTC's IRQ8 safe: IRQ6 is raised only in
response to a command the guest issued, only if the guest gated it through DOR bit 3, and
only reaches anything if the guest unmasked it at a PIC whose master IMR starts `0xFC`.

### Two claims, and the gaps between them are deliberate

`3F0h`/`3F1h` (SRA/SRB) are driven only by a part strapped for **PS/2 mode**; we present a
PC/AT machine, so they are left unclaimed and float to `FFh` — which is what **both**
oracles report for `3F0h`. (Both drive `3F1h` and disagree about it, `C1h` against `51h`,
so there is nothing to copy there either.)

⛔ **`3F6h` is not ours at all.** The FDC does not decode offset 6; on a PC/AT it is the
hard-disk controller's alternate status register, and both oracles answer `50h`
(DRDY\|DSC) from their IDE side while we answer `FFh`. **That is a real gap and it belongs
to the ATA surface.** Claiming the whole eight-port block would have turned the row green
by taking a register that is somebody else's — which is exactly the shape of fix this
project keeps learning to refuse.

---

## 6. Still open on this surface

| Row | Verdict |
|---|---|
| `fdc.alt.3f6` | Filed against **IDE/ATA**. Found by this probe; not this probe's to fix |
| `fdc.dumpreg` `0A00` vs `0A01` | The count agrees. The first byte is **where the head is** — their BIOS seeked to cylinder 1 to load the program, ours has never moved because **INT 13h does not drive the chip**. A *coherence* gap between two doors onto one drive, and the same rule the A20 gate and the RTC both arrived at: one medium, two doors, one answer |
| `fdc.dor.raw`, `fdc.dir.raw`, `fdc.sra.srb` | **Not adjudicable** — the oracles disagree with *each other*, and in each case about the machine's state at the moment of asking rather than about the register |
| BDA `0040:003E`–`0048` | MISS. Consistent today; stops being consistent the moment either door moves the other's state |
| DMA channel 2, motor-off timing, non-DMA execution | MISS — all downstream of the data path |

⚠ **Every remaining dispute on this surface is a question about the machine's state at the
moment of asking** — which motor is spinning, which drive is selected, where the head sits
— **dressed up as a question about a register.** Separating those was the probe's job, and
the two places it failed to were caught by the oracles disagreeing with each other, not by
me.

---

## 7. ⛔⛔⛔ And then the regression run found that the sweep had been lying (`e618011`)

The round's broad regression check is `paritysweep.sh`. It reported `p_fdc` as **NO ROWS**
— the same as `p_dma`. That is an *absence*, and the standing rule says an absence in a
report means nothing, so I went to find out why instead of accepting it.

The extra-oracle parser was:

```bash
for o in $(sed -n 's/^; *ORACLE-ALSO: *//p' "$D/$p.asm"); do extra="$extra --host $o"; done
```

**`sed` returns the rest of the line.** So a probe header reading

```
; ORACLE-ALSO: pcem   (a real AMI 486 BIOS -- see scripts/pcemoracle.py)
```

word-split into `--host pcem --host (a --host real --host AMI --host 486 …`, dosdiff
rejected the command line, the probe produced nothing, and **it left the sweep entirely.**

### The blast radius

**Ten of 47 probes** — `p_dma p_fdc p_kbc p_rtc p_uart p_vgaext p_vgamem p_vgareg
p_video`, which is essentially **every hardware-device probe this project has written**,
plus `p_tsrc` for a different reason. `p_vesa`, `p_vesapm`, `p_pit`, `p_lpt` and
`p_plan12` survived *only because their `ORACLE-ALSO` lines happen to be a bare hostname
with no comment*.

⚠ **The trailing comment is the natural thing to write, and it is contagious.**
`p_fdc.asm` got its line this session by copying `p_uart.asm`'s. I introduced the eleventh
instance of a bug while looking straight at it. **A parser that punishes a comment must
not do it silently.**

### And the score could only ever go up when something broke

`PARITY 99.3% of comparable rows (558 of 562)`. Every unusable probe silently leaves
**both halves of the fraction** — so a probe that does not run and a probe that does not
exist are indistinguishable in a percentage, and breaking one *improves* the number. For
three sessions that headline was computed over 562 rows that excluded every device probe
in the project.

> This is the VGA parity lesson again, in a different instrument: **a score is only ever
> about the questions that were actually asked.** There, 89.7% was void because it was
> scored against one oracle. Here, 99.3% was void because ten probes had quietly stopped
> asking.

### Three fixes, because the parse bug was only the first

1. **Take the first word only.** `p_fdc` now contributes 11 rows — 9 agree, and the 2
   mismatches are exactly the ones measured by hand.
2. **`NO ROWS` now says why.** It read identically for *"the probe crashed"*, *"the host
   was silent"* and *"the runner built a nonsense command line"* — and the last is the one
   a reader would never guess. `p_tsrc` now reports *"no cases parsed — did the probe emit
   a canonical dump?"*, which is what led to the third fix.
3. **A file matching `p_*.asm` is not necessarily a probe.** `p_tsrc.asm` is the **resident
   half** of `p_tsr`: it is `incbin`'d into `p_tsr.com`, emits no dump, and is not meant to
   be launched. The glob ran it anyway on every pass — **so every sweep this project has
   ever run installed a TSR hooking INT 60h on the rig** and then filed the probe as
   "unusable" for ever. A probe is *defined* by including the scaffolding that emits the
   dump, so the runner now asks that rather than trusting the filename.

The summary also refuses to let the percentage stand alone while any probe is unusable.

---

## 8. State at the end

- Host `bbea3134` deployed to `bin\`. **Not user-confirmed** — `debug\prev\ntvdmhost_prev.exe`
  remains `71ef4737`, and the stable zip `dist\ntvdmex-20260917-4847355.zip` remains the anchor.
- **offvm 1591/0** (+37 for `fdc_test.c`).
- **Skyroads `n8=0 max_ms=6`**, `pacer_prio=0 joy_thread=0 pit_split=1` — the documented
  guard exactly. **Doom runs**, 17 PM fault reflections, unchanged.
- ▶ **Owed from a human: a by-hand pass.** The change is a device that did not exist, so
  the risk to existing guests is low and measured low — but *"one observable change at a
  time, with a by-hand check between"* is the rule, and this is an observable change.
