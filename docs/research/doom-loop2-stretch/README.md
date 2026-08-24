# Doom `R_ExecuteSetViewSize` — the death is stretch LENGTH, not code (session 20, 2026-08-24)

The three logs here are the experiment/control pair the session-20 conclusion rests on.
Everything else from that session is reproducible; these are the ones worth keeping,
because the claim they support is the reason the next session changes direction.

## The claim

`R_ExecuteSetViewSize` → `R_InitTextureMapping` (obj1+0x35870) **loop 2** is Doom's
`for (x=0;x<=viewwidth;x++) { while (viewangletox[i]>x) i++; ... }`. Watcom resets `i`
each `x`, so it runs **289 × ~3073 ≈ 890k inner iterations, ~3.5M instructions with no
BOP** — the first such stretch in the whole program. Every earlier phase is dense with
INT 21h/31h, so the host gets constant cooperative turns; here it gets none.

Break on that loop's **back-edge** and skip it, so it exits after one pass:

```
pmbp.txt:  03b0593f 0 2        # obj1+0x3593f = "jle 0x3590e", 2 bytes
```

| run | log | outcome |
|---|---|---|
| control, no skip, same binary | `control-no-skip-DIES.log.gz` | VDM torn down, **no `STAGE2` block at all** |
| loop 2 cut to one pass | `loop2-skip-SURVIVES-run1.log.gz` | clean wind-down, **`STAGE2: complete`** |
| loop 2 cut to one pass | `loop2-skip-SURVIVES-run2.log.gz` | clean wind-down, **`STAGE2: complete`** |

Across the wider session: **6/6 deaths without the skip, 2/2 survivals with it.**
Nothing else differed. So the arithmetic is not the bug — the uninterrupted **length** is.

## How to read them

Use `gzip -dc`, not `zcat` — macOS `zcat` looks for a `.Z` file and fails on these.

```
gzip -dc control-no-skip-DIES.log.gz | tail -5       # ends mid-stream on a COMPLETE \r\n line
gzip -dc control-no-skip-DIES.log.gz     | grep -c 'STAGE2: complete'    # 0  -> torn down
gzip -dc loop2-skip-SURVIVES-run1.log.gz | grep -c 'STAGE2: complete'    # 1  -> survived
gzip -dc loop2-skip-SURVIVES-run2.log.gz | grep -c 'STAGE2: complete'    # 1  -> survived
gzip -dc control-no-skip-DIES.log.gz | grep -E 'veh\{' | tail -1         # any=0 fatal=0
```

**The kill is kernel-driven.** Every host exit path logs before it goes — the watchdog
prints `watchdog terminating (wedged)`, the VEH prints `DPMI FATAL`, headless prints
`deadline reached`. None of the three appears in the control, `veh{any=0 fatal=0}` for
the whole run, and the log ends on a complete line rather than a torn write. So the
process did not exit through any path of its own.

## What "survives" does NOT mean

With loop 2 cut to one pass, `xtoviewangle[]` is left almost entirely unfilled, so Doom
exits a few hundred ms later (`run_ms=0xf72`, `plane-nonzero` all zero — **nothing was
drawn**). The measured result is "**the VDM was not torn down**", *not* "Doom rendered".
Do not upgrade that claim.

## Refuted en route (each by a dump, not an argument — do not re-run)

- **unallocated BSS** — obj3 is `0501`'d `BX:CX=0x86000` at `0x03b40000`; highest write `0x5c89c`. Inside.
- **corrupt `finetangent`** — dumped `finetangent[3072] = 0x00010032` (≈1.0007 in 16.16). Correct.
- **non-terminating `viewangletox`** — dumped: starts `0x121` (=viewwidth+1=289), ends all `0xffffffff`. Monotone.
- **Sound Blaster IRQ** — `nosb.flag` → identical death, same last line.
- **async injector** — early bails instrumented (commit `1099104`): **zero** attempts of any kind across the death window.
- **`pmnoirq.flag`** — *inconclusive, not a refutation*: with no ticks Doom wedges at `I_StartupTimer()` and never reaches `D_Display`.

Full context, landmark addresses and next steps: `return-ntvdm.md`, session-20 block.
