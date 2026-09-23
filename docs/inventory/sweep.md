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

# The full sweep — all 36 probes now run at least once (s72 evening)

Before this session **8 of 36** probes had ever been diffed. All 36 have now been
run. This table is the map; it is not a claim that a clean probe means a verified
surface (see the warning under the cluster above).

| clean | `p_file` `p_alloc` `p_exec` `p_ovl` `p_ctab` `p_ctry` `p_defs` `p_misc` `p_redir` `p_umb` `p_unimp` `p_ver` `p_dir`* `p_rest`* |
|---|---|
| **abstained (environment)** | `p_curdir` `p_psp` `p_mcb` — 12 rows, rationales recorded |
| **fixed this session** | `p_err` (AH=3Dh error mapping, `b580c4d`) |
| **still disagreeing** | `p_disk` 13 · `p_xms` 7 · `p_sysvar` 6 · `p_lpt` 5 · `p_ioctl` 3 · `p_tsr` 3 · `p_plan12` 1 |

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
