# What mode Y costs — the measurement north star 1 was parked on (s80)

**Date:** 2026-09-25 · **Host:** `f25c7ae9` (instrumented; no behaviour change) ·
**Rig:** bare-metal XP, 3.3 GHz (`cyc_per_us=3325`) · **Raw logs:** `runs/s80_ns1/`

The mode-Y fix was parked on a judgement nobody had measured: *"arming the A0000 trap
makes the interpreter the CPU."* The user's bar (2026-09-25): **measure first, then
decide.** This is the measurement.

## What is being measured

Every map-mask (SR2) write is an `OUT` to `3C5h`, so it **already traps**, and the host
already remaps A0000 on each one (`modey_remap_select`: `UnmapViewOfFile` +
`MapViewOfFileEx`). Single-plane masks are served correctly that way. **Multi-plane masks
are not:** one virtual page cannot store to two planes, so the host maps a scratch section
and fans its diff out afterwards — and a diff cannot see a store of the value already there,
which is the whole defect ([[doom-low-detail-two-plane-masks]]).

New instrument, `STAGE2: MODEYTL`, one value per second of mode-Y activity:

| row | meaning |
|---|---|
| `sel` | SR2 writes that reached the host |
| `swap` | the ones that actually moved the window |
| `fan` | multi-plane windows closed |
| `fanN` | bytes changed since the previous window — a **lower bound** on the guest's multi-plane stores |
| `us` | host time inside the remap/fan-out (RDTSC, converted once against QPC) |
| `flip` | CRTC `0Ch` writes — guest page flips, i.e. frames |

⚠ `fanB` is also printed and is **not** a store count: the scratch is not re-seeded between
two multi-plane masks, so its diff accumulates and is re-counted (~90M/s in Doom low).

## Results — steady state, per second

| | Doom **high** (renders right) | Doom **low** (broken) | Wolf3D in play (broken) | Mario (broken) |
|---|---|---|---|---|
| guest frames/s | **35** | **~16** (11–24) | 70 | 70 |
| SR2 writes/s | ~50,000 | ~12,000 | ~25,300 | ~70,000 |
| window remaps/s | ~46,000 | ~2,500 | ~24,500 | ~46,000 |
| multi-plane windows/s | ~2 | **~9,500** | ~3,650 | ~280 |
| multi-plane stores/s (lower bound) | ~200 | **~300,000** | **~550,000** | ~4,100 |
| host CPU in remap + fan-out | ~11% | **~93%** | ~38% | ~13% |
| masks used | single-plane | `0x03`, `0x0c` dominate | `0x03 06 07 0c 0e 0f` + singles | `0x05 0a 0f` + singles |

Per remap: **~2.3 µs** (Doom high: ~46k remaps ≈ 105 ms/s).
Wolf3D's numbers are from gameplay reached by a key script; its title screen alone shows
almost nothing. Mario alternates play (70 fps) with transitions (large fan bursts).

★ **The current approximation is not the cheap option.** Doom's low detail spends ~93% of
every second in the fan-out — scanning 64 KB per window, ~9,500 windows a second — and its
frame rate halves (16 vs 35 fps). The wrong picture is also the slow one.

## What the three designs would cost

**Interpreter throughput**, from the existing mode-12h interpreter on Lemmings:
1.40–1.48 G instructions over 44–54 s runs → **~30–40 M instructions/s** (≈80–110 cycles
per instruction). ⚠ Measured on 16-bit real-mode code.

| Design | Correct? | Projected cost | Verdict |
|---|---|---|---|
| **A. Status quo** — remap single-plane, fan out multi-plane | ✗ | as measured: 11–93% | the defect |
| **B. Trap every store while a multi-plane mask is live** | ✓ | 300k–550k+ faults/s × ≥2.3 µs = **0.7–1.3 s per second** | ✗ infeasible — unplayable |
| **C. Interpret while a multi-plane mask is live**; single-plane stays native + remapped | ✓ by construction: each store goes to every selected plane at the time of the write | see below | ★ recommended |

**Design C, projected** (store counts from the table; instructions-per-store assumed:
Doom's low column/span drawers ~7, Wolf3D's compiled scalers ~3):

- **Doom low:** ~27k multi-plane stores/frame × ~7 ≈ 190k instructions/frame ≈ **5–6 ms/frame**
  interpreted. At 35 fps ≈ 20% of a CPU, against today's ~93% in the fan-out.
  **Probably faster than today, not slower.** Window entry is one fault; consecutive
  `0x03`/`0x0c` windows stay inside the interpreter (the `OUT` is interpreted too).
- **Wolf3D:** ≥550k stores/s × ~3 ≈ 1.7M instructions/s ≈ **5%**, plus ~3,650 entries/s.
- **Mario:** ~4k stores/s — negligible.

**What C costs in engineering:** `v86interp.h` is **16-bit only** (address size 16-bit,
`0x67` bails). That covers Wolf3D and Mario, which are real-mode programs. **Doom's
renderer is 32-bit flat protected-mode code**, so Doom needs the interpreter extended to
32-bit addressing for at least the instructions its drawers use.

⚠ Also in scope for correctness, independent of the design: the chain-4 de-interleave
defect `p_vgamem` found (`vgamem.chain4.abcd`) — Doom writes while chained, then unchains.
An address generator that routes stores at write time fixes both; the fan-out fixes neither.

## Caveats

- `fanN` is a lower bound; true store counts are higher (same-value stores are invisible).
- Instructions-per-store for C are estimates, not measurements; interpreter speed is from
  16-bit code. The first step of C should measure both on the real guests.
- One run per guest, headless, on one machine.

---

## Design C, built — Wolf3D and Mario (s80, the user's choice: "C: Wolf3D/Mario first")

**How it works.** `modey_needs_interp()` is true while the guest is in unchained 256-colour
mode with a **multi-plane map mask or write mode ≠ 0** — the two cases no mapping can serve.
While it holds, the V86 loop runs the guest in the host interpreter (as mode 12h already does),
and every A0000 access goes through `vga_planar_write`/`vga_planar_read` — the full VGA
pipeline (write modes 0–3, latches, set/reset, bit mask, GR4), which already targets the
host's four plane sections. The window opens and closes on a trapped `OUT`, so it cannot miss
one; a page trap is not an option (it freezes V86 guests on real hardware). While the
interpreter serves a multi-plane window the remap points A0000 at the first selected plane
instead of the scratch, so the seed + fan-out are gone. Knob: `cfg\modeyinterp_off.flag`.

**Three defects the first runs found:**

1. **`host_interp` carried registers 16 bits wide** — high halves zeroed on entry, dropped on
   exit. Wolf3D's `FixedByFrac` (`mov eax,[bp+6]` interpreted; `cdq / idiv dword` declined to
   the real CPU) divided a truncated EAX, took INT 0, and IRET'd to `0000:0078`. Now 32-bit.
   Latent for the mode-12h path too; Lemmings is pure 16-bit and never showed it.
2. **VIF was not kept in step with an interpreted `cli`**, so the loop's gate (`IF | VIF`) could
   inject inside a closed region. Mode Y now writes VIF back with IF, and only yields to a
   pending IRQ when the guest's IF would let it be taken.
3. **Declined opcodes under a multi-plane mask:** a decline runs natively *until the next
   trap*, so a native A0000 store in that stretch reaches one plane. Added `cdq`/`cwde`
   (`66 99`/`66 98`) and `imul r,r/m,imm` (`69`/`6B`) — 11 new checks in `interp_test`.
   Wolf3D's declines went 1,077 → 3 (startup only: `rep outsb`, x87 `fild`); Mario's 8,084 → 0.

Found by an instruction ring (`cfg\myring.flag`, last 64 interpreted instructions, dumped when
the guest lands at CS=0) — kept, off by default.

**Results (rig, host `0473d95d`):**

| | Before (fan-out) | Design C |
|---|---|---|
| Wolf3D status bar | FLOOR/SCORE/LIVES/AMMO numbers and weapon **missing** | **all present** (`runs/s80_ns1/OFF_02.png` vs `wF_02.png`) |
| Wolf3D frames/s in play | 70 | 70 |
| Wolf3D host CPU | ~38% (fan-out) | **~70–82% (interpreting)** |
| Mario frames/s | 70 | 70 |
| Mario host CPU in play | ~13% | ~12% (remap) + ~2% (interp) |
| Mario declines under multi-plane | 8,084 | 0 |

⚠ **Wolf3D is interpreted almost continuously**, not only while drawing: it leaves a
multi-plane mask or write mode set through its game logic. ~18 M instructions/s ≈ 155 cycles
per instruction. Frame rate is unaffected on this 3.3 GHz box; a slower machine would feel it.
The interpreter's own speed is now the lever (decode caching), not the design.

**Regression, mode 12h (Lemmings), interleaved A/B against the confirmed build `b6a8a95b`:**
interpreted throughput first measured −6%; the per-instruction checks, the timing wrapper and
the `fanN` shadow were each confined to mode Y, leaving **−1.5%** (1.093 G vs 1.110 G
instructions in 54 s) with guest frames unchanged (1,617–1,623 retrace edges in every run).

Other regressions green: Skyroads (`n8=0 max_ms=6`), Doom direct (719/719 ISRs, no fault),
Win16 Notepad, off-VM battery (interp_test 181 checks).

**Still open for north star 1:** Doom (32-bit protected mode — the interpreter needs 32-bit
addressing), and the chain-4 → unchained de-interleave (`p_vgamem`).

---

## Design C for Doom — the flat 32-bit interpreter (s80)

**What runs where.** A map-mask site recorder (`MODEY-PM site`) showed Doom sets its two-plane
masks in exactly two places: the top of its low-detail column drawer (`0435bd17`) and span
drawer (`0435c165`). Both are self-contained assembly — `pushad`, set the mask, an unrolled
store loop, `popad; ret` — and everything that touches A0000 is inside them. The C renderer
between them runs with the mask still multi-plane but stores nothing to video memory.

So Doom is interpreted **from the trapped `OUT` until the drawer returns**, not for as long as
the mask is multi-plane (which would interpret the whole renderer). `src/host/pm32interp.h`
is a new, host-agnostic flat 32-bit interpreter — 32-bit addressing with SIB, the integer set
Watcom and id's assembly emit, exact flags, and an **exact decline** (an unmodelled instruction
changes nothing). 39 off-VM checks in `tools/dostest/pm32interp_test.c`, including a
drawer-shaped routine end to end.

**The stop rule needed one refinement, found by measurement.** Doom also writes masks through a
generic helper (`out dx,ax; pop ebx; ret`) and does the work in the caller — the status-bar
latch copies and `0Fh` clears. Stopping at the helper's own `ret` handed that work to the real
CPU. `cfg\modeypm_detect.flag` (keeps the scratch window during native stretches so stray
native stores show up in `fanN`) measured up to **1,233 native stores/s** under a multi-plane
mask. Rule now: stop at a return only once the run has touched the aperture. Re-measured with
the detector: **0 native multi-plane stores across the whole run.**

**Results (rig, host `8d795b96`):**

| | Before | Design C |
|---|---|---|
| Doom **low** frames/s | ~16 | **35** |
| Doom low status bar (`doomdetail`; 0.28 = correct, 0.75 = defect) | 0.75–0.77 | **0.294** |
| Doom low host CPU in remap/fan-out | ~93% | ~5% (+ interpreting ~240 M instructions over the run) |
| Doom **high** status bar / frames/s | 0.28–0.29 / 35 | 0.293 / 35 (unchanged) |

Drawer runs ended by the drawer returning: 377,753 of 378,501. Declines: 505 (`sti`/`popf`
in interrupt paths). Cap hits: 109.

Regressions: Doom high and direct, Hexen, Heretic (A/B'd on the confirmed build — it never
reaches mode Y headless on either), Wolf3D, Mario, Skyroads; off-VM battery 1,641 checks.

Knobs: `cfg\modeypm_off.flag` (disable), `cfg\modeypm_detect.flag` (count native stores).
**Still open:** the chain-4 → unchained de-interleave (`p_vgamem`).
