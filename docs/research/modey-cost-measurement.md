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
