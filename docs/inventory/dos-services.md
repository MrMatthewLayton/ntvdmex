# DOS services — files, directories, PSP, memory, the HMA, INT 13h

> **Carried over from `docs/PARITY.md` on 2026-09-23**, which is now retired — this is
> the same measured data in the inventory's home. See
> [`README.md`](README.md) for the method and the status vocabulary.
>
> ⚠ **The `state` column below is still PARITY's four-state vocabulary**
> (`missing` / `guessed` / `implemented` / `verified`). Re-marking it against the code
> in IMPL / PART / STORE / MISS / N-A, with a `file:line` per row, is owed — and is the
> point of doing it: a row that reads `implemented` here may well be **PART**, which is
> the state that returns a plausible wrong answer.

# DOS file services — the FCB parser (`p_fcb.asm`, `p_find.asm`)

| item | state | notes |
|---|---|---|
| **`AH=29h` expands `*` into `?`s** | verified | **was stored literally** — `*.BAS` → `00 3F×8 'BAS'`, `*.*` → `00 3F×11` |
| `AH=29h` drive field, AL wildcard flag | verified | |
| `AH=0Fh/10h/11h/12h/13h/16h` FCB open/close/find/delete | verified | |
| `AH=4Eh/4Fh` find-first/next, error codes | verified | `AX=18` no-match in an existing dir, `AX=3` missing dir |
| a FAILED `4Eh` leaves a live search alone | verified | ⚠ mismatched in ONE run and clean in three since, nothing changed — the *first run after a deploy* pattern |

**The gap (s72):** `AH=29h` stored `*` literally. QBasic parses its file pattern with
29h and then matches each directory entry against the parsed FCB, so nothing matched
and its Open dialog listed **no files** while the directory pane beside it was correct.
⛔ I twice guessed QB used the **FCB search**; it enumerates with `AH=4Eh/4Fh`. One
trace line settled what two rounds of reasoning had not.

---

# File / directory / PSP / memory (`p_file`, `p_curdir`, `p_psp`, `p_mcb`, `p_alloc`, `p_err`)

Session 72 (evening). **The first harvest of the probes that were already written.**
36 probes exist in `tools/dostest/`; before this, 8 had ever been diffed. This is the
first of the remaining 28.

| probe | rows | result |
|---|---|---|
| `p_file` | 24 | all AGREE |
| `p_alloc` | 8 | all AGREE |
| `p_err` | 20 | **2 real gaps, closed** (see below) |
| `p_curdir` | 14 | AGREE + 5 abstained (not contracts) |
| `p_psp` | 8 | AGREE + 3 abstained |
| `p_mcb` | 7 | AGREE + 4 abstained |

### ★ The gaps this found and closed (`b580c4d`)

**`AH=3Dh` answered "file not found" (2) for every possible failure.** The handler
read `f == INVALID_HANDLE_VALUE` and stopped asking why.

| case | oracle | ours (before) |
|---|---|---|
| `err.after.3D.readonly` — read-only file opened for WRITE | `AX=0005` access denied | `AX=0002` |
| `err.after.3D.baddrive` — open on unclaimed `Y:` | `AX=0003` path not found | `AX=0002` |

Not cosmetic: a program told "not found" about a file that is plainly there goes
looking for it instead of reporting the real problem. ⚠ The comment directly above
that line **already blamed this exact collapse for the GDI.EXE wall** — the sharing
half was fixed in s37 and the mapping half was left sitting there. `AH=6Ch` had the
same line and the same bug.

Fixed by `dos_err_from_win32()` in `dos_err.h`, in the 59h table's own style: **both
sides of every row measured** — the DOS side is the oracle `CASE=` line, the Win32
side is the handler's new `win32=0x..` log (`2 -> 2`, `5 -> 5`, `3 -> 3`). Unmapped
codes log `UNMAPPED` and keep the old 2 rather than inventing an answer. `BX=0303`
(the access-denied class) then follows for free — 59h was always right about code 5,
nothing had ever handed it one. Pinned off-VM by `err_test.c` (36 checks), including
that the three causes stay **distinct**.

⚠ **There is deliberately no `ERROR_INVALID_DRIVE`(15) row.** The obvious guess is
that `Y:\...` arrives as 15; measured, it arrives as **3**. A 15 row would be an
unexercised invention dressed as evidence.

### Rows that are NOT contracts, and why (abstentions in `oracle-rules.json`)

The oracle boots to `A:\` and the rig runs probes from deep inside the share, so a
family of rows compares two *environments*:

* `int21.19.curdrive` — the drive the program started on (A: vs C:).
* `curdir.*` (4 rows) — `AH=47h`'s absolute path: `"ZZCD"` vs
  `"DOCUME~1\ALLUSE~1\..."`. Both correct for where they were started, and **ours is
  properly 8.3-shortened**. ★ The contract this probe exists for (GH #134) is
  untouched and still holds: **all four buffers are identical on each host**, i.e. an
  EXEC does not clobber the current directory.
* `psp.02.memtop`, `psp.int24.live` — how much memory this machine has left, and
  where its INT 24h handler happens to live.
* `mcb.head`, `mcb.block` BX/CX/DX — arena layout. The *structural* checks are not
  abstained and agree: the `'M'` signature (`0x4D`) and the chain ending at `9FC0`.

⛔ **THE HARNESS WAS MISREPORTING THESE.** A deliberately abstained row came out as
`NO-DATA`, whose summary reads *"missing evidence ... check for a truncated log"* —
accusing the harness of being broken when the subject had answered perfectly well and
the oracle had simply been told not to vote. `dosdiff.py` now separates **ABSTAINED**
(a recorded decision) from **NO-DATA** (missing evidence), and prints the abstention
count rather than letting it vanish into "no mismatches".

### ⚠ What these probes do NOT cover, despite appearances

`p_file`'s own header claims `46h, 5Ch, 67h, 6Ch`; none of them are emitted. Several
rows compare only `CF`, so "it returned success" is checked while the returned
*value* is not — `int21.5700.getdate` and `int21.34.indos` are both this shape. An
all-AGREE probe is not the same as a verified surface.

---


# ✅ The HMA, closed (s72)

`p_xms` measured us refusing the HMA twice over — `AH=00h` answered `DX=0` ("no HMA")
and `AH=01h` answered `BL=0x90` ("HMA does not exist") — against an oracle that has
one. An unimplemented **feature**, not a wrong number.

★ **AND IT TURNED OUT TO BE THERE ALL ALONG.** The first attempt went straight to
`VirtualAlloc(0x100000, MEM_RESERVE|MEM_COMMIT)` and got **`ERROR_INVALID_ADDRESS`
(0x1E7)** — which says *"something already owns this"*, not *"you may not have it"*,
and those need opposite responses. Asking `VirtualQuery` first showed the region
already `MEM_COMMIT`: **NT had mapped the VDM's HMA the whole time and we were simply
refusing to admit it.** In this design a guest linear IS a host VA, so there was
nothing to allocate. ▶ *Query before you allocate; an error code that means "occupied"
is good news wearing a bad hat.*

**Proven, not claimed.** The XMS arm's own comment warned that reporting an HMA we do
not provide "would have been a lie", so the probe now writes a pattern through
`FFFF:0010` and reads it back: **`A55A`/`1234` return exactly as written.** It only
does so when `AH=01h` succeeded — on a `DOS=HIGH` machine that memory is the running
kernel, so the oracle reports the untouched sentinel `0xDEAD` by design.

⛔ **AND THE FIRST VERSION OF THAT TEST PASSED WHILE PROVING NOTHING.** It read
`POISON` / `cmp ax,1`, and POISON's whole job is to overwrite AX — so the compare
tested the poison, both hosts took the "did not get it" branch, and the row agreed
`DEAD == DEAD`. The trap this file's own header warns about, walked into anyway. The
result is saved into `[hmaok]` the instant the call returns now.

⚠ **No A20 aliasing**, which is a decision already recorded in `dos_xms.h`: *"an NT VDM
does not wrap at 1 MB — the line is effectively always open."* We model the A20 **flag**
(`AH=03h`..`07h`), not the address wrap.

Remaining in `p_xms`: `xms.08.queryfree` **BH** (oracle `0xAA`, ours leaves poison).
`BH` is **undefined by the XMS spec** for this call, so it stays RED and undecided
rather than being matched by invention.

---


# ✅ INT 13h — it was never unimplemented, it had no disk (s72)

`p_disk` showed 13 mismatches and I filed them as *"INT 13h unimplemented: answers
`AH=80h` and leaves the probe's poison in BX/CX/DX"*. **That reading was wrong.**

`src/dos/dos_disk.h` states the design in its first paragraph: *"a drive is a disk
**IMAGE FILE**, or it is absent. Nothing is synthesised."* The layer is fully
implemented against that model and unit-tested (`disk_test.c`, 23 checks). With no
`cfg\FLOPPY.IMG` on the rig it correctly answered **"drive not ready"** to everything —
and the registers that looked like untouched poison were a call that had properly
**failed**, because a failed call does not write them.

Given a disk, **all 18 rows AGREE**:

| | |
|---|---|
| `int13.08.params` | `CX=4F12` (79 cyl, 18 sec) · `DX=0101` (2 heads, 1 drive) · `BX=0004` (1.44MB) |
| `int13.15.type` | `AX=0100` — floppy, no change-line |
| `int13.02.read.boot` | `AX=0001`, one sector read |
| `disk.boot.sig` / `.oem` | `55AA` / `MTOO` — the oracle's scratch floppy is mformat-made too |
| `int25.absread` | agrees, flags contract included |

`./scripts/mkfloppy.sh` builds the image rather than leaving a magic 1.4 MB file on
the share — `mformat -f 1440`, the oracle's exact geometry, plus a known file so a
guest reading the data area can prove it read *this* disk. The BPB is asserted. ⚠ Two
builds are not byte-identical (mformat stamps a time-based volume serial); the
geometry is, and that is the contract.

▶ **Before calling a surface unimplemented, check that it has something to work on.**

⚠ **Rig state:** `cfg\FLOPPY.IMG` is now present and should stay — without it `p_disk`
goes back to measuring an absent drive. It backs INT 13h/25h only; DOS file I/O on A:
still goes through Win32, so no other probe or guest is affected.

---
