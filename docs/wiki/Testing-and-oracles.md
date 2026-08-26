# Testing and oracles

Three layers, fastest first. **Use the fast one.** The rig is where correctness is finally
established, but it is minutes per round and every round has a way of lying to you.

---

## Layer 1 — off-VM batteries (seconds, no VM, no rig)

18 native test binaries under `tools/dostest/`, compiled for the build machine. They link
the real `src/dos/` and `src/vdd/` code — the DOS and device layers are header-only by
convention precisely so this is possible.

```bash
./tools/dostest/run.sh          # builds + runs everything; non-zero exit if anything fails
```

**18 batteries, 664 checks, ~10 seconds** — verified from a clean clone. Covers MCB, XMS,
EMS, DMA, PIC, PIT, SB, OPL, OPL synth, MPU, speaker, video, input, the PM interpreter, the
instruction-length decoder, the VDD bus, and the NE loader. **This is the loop to develop against.**

> ⚠️ The compiled batteries are gitignored, so **a fresh clone has no `tools/dostest/*_test`
> files** and running them directly matches nothing — in `bash` that silently "passes" a
> loop that ran zero tests. `run.sh` compiles them every time. Always go through it.

---

## Layer 2 — oracles

> **The voting rule, and it is the point of the whole thing:** the oracles vote on truth,
> and agreement between them *is* truth. **NTVDMEX does not vote** — it is the subject under
> test, and letting it into the consensus would be circular. Disagreement between oracles is
> never resolved by majority or coin-flip; it is reported as **DISPUTED** and needs a
> recorded rationale.

| Oracle | What it answers | Where |
|---|---|---|
| **Stock `ntvdm`** | "What does the thing we are replacing actually do?" Runs any target under Microsoft's VDM by dropping the IFEO key, restoring it on every exit path, and proving the state with `reg query`. Settled a keyboard question in one run after three wrong hypotheses. | `scripts/bm/rt_stock.bat`, `stockdoom.bat` |
| **Real MS-DOS 6.22 under QEMU** | "What does real DOS return for this call?" Automated capture. | `scripts/dosoracle/` |
| **Nuked-OPL** | "What should this note sound like?" Used strictly as a black box so our synth stays clean-room MIT. | `tools/oplref/`, `tools/oplprobe/` |
| **Doom's own WAD** | "What should this pixel be?" Ground truth built from the game's data files — replaced a home-made video metric that moved the *wrong way* when a bug was fixed. | `tools/doomoracle/` |

**The differential harness** ties them together — one probe `.COM`, several hosts, one diff:

```bash
./scripts/dosdiff.py tools/dostest/p_ver.com
./scripts/dosdiff.py tools/dostest/p_ver.com --json
```

⚠️ `tools/doomoracle/` needs `DOOM1.WAD`, which is **deliberately not in this repository** —
it is id's, not ours. Point the tool at your own copy.

---

## Layer 3 — the bare-metal rig

A real Windows XP box. Not optional: V86 and DPMI on real silicon cannot be tested anywhere
else. QEMU+HVF aborts on DOS/4GW's paged 32-bit protected mode *even under stock `ntvdm`*,
so every DOS/4GW game is untestable on the dev machine.

See [The bare-metal rig](The-bare-metal-rig) for operations, and
[Traps and lessons](Traps-and-lessons) for the ways it will mislead you.

---

## Instruments

Purpose-built tools, each of which exists because something could not otherwise be seen.

| Tool | What it does |
|---|---|
| `tools/dlgcheck/dlgcheck.py` | Parses `RT_DIALOG` templates out of the **linked PE** — the bytes the loader will parse, not the `.rc` that produced them — and reports control rectangles, out-of-bounds, overlaps and duplicate IDs. A dialog that compiles is not a dialog that renders, and checking used to mean a trip to the box. |
| `scripts/bm/rigshot.c` | Lets the rig see **its own window**. The host's built-in screenshot captures `g_vid.frame` — the *guest* framebuffer — so it can never show the caption, status strip, menu bar or a dialog. `rigshot` BitBlts the real desktop, plus `cmd`/`click`/`key`/`fg`/`list` for remote poking. |
| `tools/oplprobe/` | Single-note FM rig. Found five OPL defects that listening did not. |
| `scripts/dosdiff.py` | The differential harness above. |
| `dostrace.flag` | Logs every INT 21h call (AH/AL/BX/DX). **A differential instrument** — two runs differing by one typed space is what found the FCB terminator bug. |

> ⚠️ Every one of these can lie, and several have. `dlgcheck` reported 47 problems on its
> first run of which **45 were its own** (a `COMBOBOX`'s template height is the height of its
> *dropped list*). Check an instrument's model of its subject before believing its output.

---

## What "verified" means here

The project has a specific, hard-won standard:

- **Off-VM PASS** — the logic is right in isolation. Necessary, nowhere near sufficient.
- **VM-confirmed** — it runs under QEMU. Useful for real-mode work; meaningless for DPMI.
- **Bare-metal confirmed** — it runs on the real XP box, and the log proves *our* host ran
  (not stock).
- **User-confirmed** — a human watched it, or heard it. Doom's sound and mouse were both
  signed off this way, because no counter available to us could have.

For anything about *feel* — input latency, timing, audio — the headless rig is useless. It
runs with nobody typing, so every input-latency counter measures a path no key travels.
Hand the user a one-file A/B instead of burning rig rounds on a proxy.
