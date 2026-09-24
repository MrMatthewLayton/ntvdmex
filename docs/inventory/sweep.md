# The full probe sweep, and its triage

> **Carried over from `docs/PARITY.md` on 2026-09-23**, which is now retired — this is
> the same measured data in the inventory's home. See
> [`README.md`](README.md) for the method and the status vocabulary.
>
> ⚠ **The `state` column below is still PARITY's four-state vocabulary**
> (`missing` / `guessed` / `implemented` / `verified`). Re-marking it against the code
> in IMPL / PART / STORE / MISS / N-A, with a `file:line` per row, is owed — and is the
> point of doing it: a row that reads `implemented` here may well be **PART**, which is
> the state that returns a plausible wrong answer.

> ## ⛔⛔⛔ 2026-09-24 — everything below this line was scored by an instrument that
> ## had stopped asking ten of its questions
>
> `paritysweep.sh` extracted a probe's extra oracle with `sed -n 's/^; *ORACLE-ALSO: *//p'`,
> which returns **the rest of the line** — so a header carrying a trailing comment
> word-split into `--host pcem --host (a --host real --host AMI …`, dosdiff rejected it,
> and the probe reported `NO ROWS` **and left the sweep**.
>
> **Ten of 47 were out**: `p_dma p_fdc p_kbc p_rtc p_uart p_vgaext p_vgamem p_vgareg
> p_video`, essentially every hardware-device probe in the project, plus `p_tsrc`.
> `p_vesa`, `p_pit`, `p_lpt`, `p_plan12` and `p_vesapm` survived **only because their
> lines happen to be a bare hostname**.
>
> ⇒ **An unusable probe leaves *both halves* of the fraction, so the score could only rise
> when a probe broke.** Fixed in `e618011` / `e89478e`. Two further defects in the same
> runner came out with it: `p_tsrc` is a **TSR payload, not a probe** — every sweep this
> project ever ran installed it on the rig — and `p_child` is **`p_exec`'s declared
> companion**, graded standalone, where both its "mismatches" were artefacts of having no
> parent.
>
> **The table below is kept for its per-row triage, which is still good. Its counts are
> not.** Current figures are in the section after it.

# The full sweep — all 36 probes now run at least once (s72 evening)

Before this session **8 of 36** probes had ever been diffed. All 36 have now been
run. This table is the map; it is not a claim that a clean probe means a verified
surface (see the warning under the cluster above).

| clean | `p_file` `p_alloc` `p_exec` `p_ovl` `p_ctab` `p_ctry` `p_defs` `p_misc` `p_redir` `p_umb` `p_unimp` ~~`p_ver`~~ `p_dir`* `p_rest`* |
|---|---|
| **abstained (environment)** | `p_curdir` `p_psp` `p_mcb` — 12 rows, rationales recorded |
| **fixed this session** | `p_err` (AH=3Dh error mapping, `b580c4d`) |
| **still disagreeing** | `p_disk` 13 · `p_xms` 7 · `p_sysvar` 6 · `p_lpt` 5 · `p_ioctl` 3 · `p_tsr` 3 · `p_plan12` 1 · **`p_ver` 2 (see below)** |

## ⛔ `p_ver` is no longer clean, and no code changed (2026-09-24)

Re-run today, `./scripts/dosdiff.py build/probes/P_VER.COM --host msdos622 --host ntvdmex`:

```
int21.30    AX   1606   0005   MISMATCH
int21.3306  BX   1606   0005   MISMATCH
```

**The rig is reporting DOS 5.00**, from `HKCU\Software\NTVDMEX\DosVersionMajor/Minor` —
the Settings dialog's persistent store, which survives reboots and wipes, and which no
`cfg\dosver.txt` was overriding. Nothing announced it; the host printed a version line
only when the *file* overrode. See standing hazard 11 in [`STATE.md`](../STATE.md);
fixed in `0e342c2`, which now prints the version **and its source** on every run.

⚠ **This is what "scores — re-run, never quote" is for.** The recorded figure
(670/677 = 99.0%) was true when measured and is stale now, and the thing that moved it
was **a machine setting, not a commit**. How long the rig has been on 5.00 is unknown,
so the date of this table's `p_ver: clean` is the earliest it can be trusted from.

### ▶ And the row itself is badly modelled — this is owed

`dosdiff.py` already abstains for **dosbox-x** on exactly these two fields, with the
right reason written out: *"DOSBox-X's reported DOS version is a CONFIGURABLE EMULATOR
SETTING … not an observation about MS-DOS … **our own value is selectable too, which is
exactly why DOSBox's cannot be truth**."* The note names our own knob and then compares
it to 6.22 anyway.

So the row as written tests **the setting**, not the implementation, and can be made
green or red by a dialog. The useful check is the falsifiable one: **does `AH=30h`
report what the configuration says it should?** — which would catch a real bug (the knob
ignored) that the present row cannot. Not implemented; recorded rather than bodged,
because making it abstain would hide the signal instead of fixing it.

\* `p_dir` and `p_rest` were **probe bugs, not host bugs** — see below.

### ⛔⛔ TWO FALSE POSITIVES, AND THE SECOND NEARLY GOT "FIXED"

`p_dir`/`int21.3600.baddrive` and `p_rest`/`int21.3200.baddrive` both used **DL=26
(Z:)** to mean "a drive that is not there". Z: is not free on this panel: DOSBox-X
mounts it as its utility drive, and **the XP rig has it mapped to
`\\server\storage`** (confirmed with `net use` before anything was changed). So the
question was never asked, and the rows read as NTVDMEX bugs:

    int21.3600.baddrive   oracle AX=FFFF   ours AX=0002
    int21.3200.baddrive   oracle AX=32FF   ours AX=3200

`AX=0002` is *two sectors per cluster* — a **successful** query of the real Z:. Our
answers were right for the machine they ran on, and "fixing" them would have made
`AH=36h`/`32h` deny a mapped network drive. Switched to **Y:**, unclaimed
everywhere; both now agree — `FFFF` **with carry clear** (it is not a CF error) and
`AL=FF`. ⚠ `p_err.asm` already used Y: *and said why*; the reasoning had not reached
the other two probes.

### `p_plan12` — do NOT fix toward this one
`int10.set.mode12` (oracle `AX=0020`, ours `AX=0012`) is **INT 10h**, where the table
at the top of this file says the QEMU oracle is SeaBIOS, *a rewrite*. It joins
`16.01.enh` and `bda.crtc` as **blocked on PCem**, not as a defect.

### `p_disk` — a real gap the probe cannot fairly measure
We answer `AH=80h` (timeout) and leave **BX/CX/DX holding the probe's poison**, i.e.
INT 13h is unimplemented rather than answering. But the probe targets **floppy drive
0**, and the rig's A: is empty (`~A` in the boot line), where "not ready" is a
defensible answer. ▶ Needs a probe aimed at a drive the machine actually has before
any of it can be called a defect.

---

# Triage of the sweep — what is a defect and what is not (s72 evening)

The sweep left ~37 disagreeing fields. Triaged, they are **four different things**,
and only the first is work on NTVDMEX.

## 1. REAL GAPS (left RED on purpose — these are the backlog)

| where | evidence | what it means |
|---|---|---|
| ~~**XMS: there is no HMA**~~ | ✅ **CLOSED (s72)** — see the section below | `xms.00.version` **DX now AGREES at 1**, and the guest can write and read `FFFF:0010`. |
| `xms.08.queryfree` BH | oracle `0xAA`, ours leaves poison | BH is undefined for `AH=08h`; the oracle writes something deliberate. Undecided — do not "fix" without provoking it. |
| `p_tsr` `tsr.paras.still.held` | oracle `0x26`, ours `0x21` | The block a TSR still holds. 5 paragraphs apart; the free-memory rows beside it are abstained, this one is not. Uninvestigated. |
| `p_sysvar` 6 BUF rows | `sysvars.sft0` comes back as **`EEEEEEEE` — the probe's own poison** | DOS internals (List of Lists, DPB, SFT, CDS, NUL). Poison means *we never wrote it*. `cds0`/`cds2` start correctly (`"A:\"`, `"C:\"`) and then diverge. krnl386 walks the SFT, so this is WOW-relevant. |
| `p_disk` INT 13h | `AH=80h` + poison in BX/CX/DX | Unimplemented rather than answering — but the probe targets **floppy drive 0** and the rig's A: is empty, where "not ready" is defensible. **Needs a probe aimed at a drive the machine has.** |

## 2. BLOCKED ON PCem — measured against a BIOS that is a REIMPLEMENTATION

`p_lpt` (**5 rows**: `int11.equipment`, `int17.00.print` ×2, `int14.03.status`,
`int14.02.recv.timeout`) and `p_plan12` (`int10.set.mode12`) are all **INT 10h/11h/14h/17h**.
The table at the top of this file says it plainly: QEMU's SeaBIOS is a rewrite and is
**not evidence about a BIOS**. These are not defects and must not be "fixed" toward
SeaBIOS. They join `16.01.enh` and `bda.crtc` on the PCem list.

## 3. FIVE FALSE POSITIVES — probes asking a question the panel could not answer

⛔ **Every one of these read as an NTVDMEX bug.** Two were one commit from "fixing"
correct code.

| probe | the flaw | fix |
|---|---|---|
| `p_dir`, `p_rest` | used **Z:** as "a drive that is not there" — DOSBox-X mounts Z:, and **the rig has it mapped to `\\server\storage`**. Our `AX=0002` was *two sectors per cluster*: a **successful** query of a real drive. | → **Y:**, unclaimed everywhere |
| `p_ioctl` ×3 | `4408/4409/440E` passed **BL=0, "the default drive"** — A: on the oracle, C: on the rig. Asked "is a floppy removable?" vs "is a hard disk removable?" and called the two correct answers a disagreement. Its own comment asserted the false part: *"no host has a single-floppy alias on its default drive"* — the oracle's default **is** A:, which has one. | → **BL=3 (C:)**, a fixed disk everywhere |

▶ **The lesson, twice over:** `p_err.asm` already used Y: *and wrote down why*, and the
reasoning never reached the other probes. **A hazard recorded in one probe does not
propagate.** And the abstention machinery only models an *oracle* being unable to vote —
here it was the **subject's** environment that made the question invalid.

## 4. ENVIRONMENT — abstained, with a rationale each (18 rows total)

Current drive, absolute paths, top-of-memory, vector addresses, MCB layout, free-memory
figures, XMS totals, the XMS driver's own revision and code bytes. ★ In each case the
probe's *real* contract is still checked and still holds — e.g. all four `p_curdir`
buffers are identical on each host (an EXEC does not clobber the cwd), and `mcb`'s `'M'`
signature and `9FC0` chain end both agree.

---

---

# 2026-09-24 — the sweep, with every probe actually asking

First run after `e618011`/`e89478e`. **Re-run, never quote** — but the *shape* below is
the point, and the shape is what the broken instrument was hiding.

| | broken | fixed |
|---|---|---|
| probes | 47: 34 clean, 3 dirty, **10 unusable** | 46: **40 clean**, 6 dirty, **0 unusable** |
| rows | 628 (558 agree, 4 mismatch) | **748** (670 agree, **7** mismatch, 71 abstained) |
| **PARITY** | **99.3%** (558/562) | **99.0%** (670/677) |

**+120 rows, +3 net mismatches, and the score went DOWN.** That is the instrument working:
every probe that had stopped asking was silently improving the number.

## Every one of the seven, accounted for

| Probe | Row | Why it is open |
|---|---|---|
| `p_tsr` | paras-still-held | long-standing, recorded |
| `p_xms` | `xms.08` BH | **undefined by the spec** — nothing to match |
| `p_vgamem` | the one `13h`→unchained case | mode-Y exactness is **PARKED**: `vdd_video.c:1991` has no address generator, and A0000 is mapped RAM with no write hook |
| `p_kbc` | `kbc.outport.d0` `01CF` vs `0103` | bits 0 and 1 — **the two with a meaning to software** — match. PCem additionally sets 2, 3, 6, 7 (keyboard clock/data, two undefined). **One oracle is not a pass**; recorded rather than copied |
| `p_dma` | `dma.status.idle` `0400` vs `0000` | channel 2's **TC**, latched by the floppy read that loaded the probe. ⚠ Sharper than it was: we now *have* an FDC, so this is attributable to its **DMA data path** (inventory step 1) rather than to the 8237A model |
| `p_fdc` | `alt.3f6` `0050` vs `00FF` | **the ATA alternate status register — not the FDC's.** Filed against the IDE/ATA surface; claiming the port would turn the row green by taking somebody else's register |
| `p_fdc` | `dumpreg` first byte `01` vs `00` | the **present cylinder of drive 0**. Their BIOS seeked there to load the program; our head has never moved because INT 13h does not drive the chip. Not adjudicable |

⇒ **No unexplained mismatch, and no regression from the 82077AA landing.**

## Two probes are now skipped, and that is correct

| Probe | Why |
|---|---|
| `p_tsrc` | **Not a probe** — the resident half of `p_tsr`, `incbin`'d into it, emitting no canonical dump. Every sweep before this one *ran* it, installing a TSR that hooks INT 60h on the rig |
| `p_child` | **A companion**, declared in `p_exec.deps`. Its cases are *relations between a child and its parent*, and run alone it has none. Graded properly by `p_exec`, which is clean on all four |

⚠ **`p_child` is the instructive one.** Standalone it reported `child.env.copy` as *shared*,
which looks exactly like the **Heaven7 environment-sharing bug** — a fixed, famous defect.
The cause was that our top-level PSP is **its own parent**
(`main.c:23023`, `dos_psp_save_vectors(NULL, DOS_PSP_SEG, DOS_PSP_SEG)`), so the probe
compared our environment segment against itself. *A plausible-looking regression of a
famous bug is the worst artefact a sweep can produce*, and it had been sitting in the
"still disagreeing" line for sessions.

## One probe added

`p_fdcreg` — all ten DUMPREG bytes, because `vdd_fdc.c`'s byte **order** was written from
memory and nothing had checked it. Confirmed on two machines (6.22's byte 7 = `12h` = 18
sectors per track pins it as EOT). 3 rows, clean; the byte string is correctly **DISPUTED**
between the oracles rather than graded against us.
