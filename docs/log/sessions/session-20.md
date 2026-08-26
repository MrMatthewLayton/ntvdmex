# Session 20 — 2026-08-24

> Archived verbatim from `return-ntvdm.md`, which was the single rolling handoff
> file until 2026-08-26. Split by session so it can be read by topic and date
> rather than by scrolling. **Nothing has been edited** — including conclusions
> that later sessions REFUTED, which are kept on purpose: the refutations are
> some of the most valuable material here.

```
═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ SESSION 20 (2026-08-24). ⚠⚠ ITS CENTRAL CONCLUSION IS **REFUTED** BY   ██
██     SESSION 21 -- the death was not stretch LENGTH, it was our own INT-site ██
██     patcher corrupting a `jle`. Kept for the landmarks and the rig notes,   ██
██     which are still good. DO NOT act on the "find the threshold length"     ██
██     plan below; it was chasing a symptom.                                   ██
═══════════════════════════════════════════════════════════════════════════════

  Branch `m9/completeness`. Gates were run on `d2256cd` FIRST, as session 19 asked.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ GATES ON `d2256cd` -- ALL GREEN (the session-19 open item, now closed)    │
└──────────────────────────────────────────────────────────────────────────────┘
```
   off-VM battery      580 checks / 16 suites, 0 failed   <- the corrected figure, verified
   check-imports.sh    pass (all imports XP-shipped)
   selftest.com        8/8 PASS   (and chain4=01 there: mode-Y did NOT disturb chained modes)
   dpmitest.com        0300/0301/0303 + nested INT 31h OK, clean exit
   dpmiback.com        real<->PM round trip, exact expected output
```
  ⚠ `dpmiback`'s `<<< MISMATCH >>>` is BENIGN: that line tests a sentinel `0x005A` at
    guest linear 0x1600 that only *dpmitest* writes. Not a regression. Do not chase it.
  ▶ The binary built from HEAD was byte-identical (sha `3c1629e7`) to the deployed
    d2256cd host, so the gates really did measure the working-render build.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ THE FINDING: SHORTEN THE LOOP AND THE VDM SURVIVES                     │
└──────────────────────────────────────────────────────────────────────────────┘

  `R_ExecuteSetViewSize` calls `R_InitTextureMapping` (obj1+0x35870). Its **loop 2**
  is Doom's `for (x=0;x<=viewwidth;x++) { while (viewangletox[i]>x) i++; ... }`.
  Watcom compiled it resetting `i` each x, so it is **289 x ~3073 = ~890k inner
  iterations, ~3.5M instructions with NO BOP** -- the first such stretch in the whole
  program. Everything before it is dense with INT 21h/31h, so the host gets constant
  cooperative turns; here it gets none.

  **THE EXPERIMENT.** `pmbp.txt` breaking on loop 2's BACK-EDGE and skipping it:
  ```
     03b0593f 0 2        # obj1+0x3593f = "jle 0x3590e", 2 bytes -> exit after ONE x
  ```
  ```
     no skip (control, same binary)   VDM torn down, NO STAGE2 block at all   6 / 6
     loop 2 shortened to one pass     clean wind-down, "STAGE2: complete"     2 / 2
  ```
  Nothing else changed. **The arithmetic is not the bug; the uninterrupted LENGTH is.**

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★ WHAT WAS REFUTED -- all by evidence, all cheap, do not re-run these      │
└──────────────────────────────────────────────────────────────────────────────┘
```
   "the BSS tables are unallocated"  obj3 is 0501'd BX:CX=0x86000 at 0x03b40000;
                                     the highest write is 0x5c89c. Comfortably inside.
   "finetangent[] is corrupt"        dumped finetangent[3072] = 0x00010032 (~1.0007
                                     in 16.16). Exactly right. focallength=0x008fe3e5.
   "viewangletox[] never terminates" dumped: starts 0x121 (=viewwidth+1=289), ends
                                     all 0xffffffff (-1). Monotone, terminates.
   "the Sound Blaster IRQ kills it"  nosb.flag -> IDENTICAL death, same last line.
   "the async injector kills it"     with early bails now logged: ZERO async attempts
                                     of ANY kind across the death window.
   "pmnoirq proves it"               INCONCLUSIVE, not a refutation: without ticks
                                     Doom wedges at I_StartupTimer() and never
                                     reaches D_Display at all.
```

┌──────────────────────────────────────────────────────────────────────────────┐
│ ⚠⚠⚠ SESSION 19'S BRACKET WAS WRONG, AND THE "INSTRUMENT LIES" WAS PARTLY US   │
└──────────────────────────────────────────────────────────────────────────────┘

  Session 19 bracketed the death to "a 157-byte window of movs that cannot fault".
  **That window was never executed.** At obj1+0x35b44 `test edi,edi / jne 0x35b7b`
  branches on `detailshift`, which is **0**, so the fall-through runs 0x35b48..0x35b79
  and jumps to 0x35bad. The probe at **0x35b8f sits in the not-taken branch -- DEAD
  CODE.** "NOT HIT" meant "never reached", not "died before here". The contradiction
  ("nothing here can fault") dissolves: nothing there RAN.

  And the breakpoint mechanism is **one-shot BY DESIGN** -- `dpmi_bp_disarm()` on hit
  unless `rep`/`skip` is set. So "7 armed, 1 hit" is not necessarily a lie.
  ⚠ `rep` does NOT work inside a BOP-free stretch: re-arming happens in
    `dpmi_bp_rearm_pending()`, which needs the host to get a turn -- exactly what
    such a stretch denies. A rep BP in loop 2 fires ONCE. Use `skip`, which is
    applied at the hit itself, when you need to alter a loop.

  ▶ **ALWAYS DISASSEMBLE AROUND A PROBE BEFORE BELIEVING "NOT HIT".** Check it is on
    an instruction boundary AND on a path the data actually takes.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ THE DEATH IS KERNEL-DRIVEN. That question is now settled.                 │
└──────────────────────────────────────────────────────────────────────────────┘

  Every host exit path logs BEFORE it goes: the watchdog prints "watchdog terminating
  (wedged)", the VEH prints "DPMI FATAL", headless prints "deadline reached".
  **None of the three appears.** `veh{any=0 fatal=0}` for the entire run, and the log
  ends on a COMPLETE line (\r\n), not a torn write. So the process did not exit through
  any path of its own: **the kernel tears the VDM down**, exactly as session 16 saw.

  ▶ NEXT: why does a long BOP-free PM stretch make XP kill the VDM? Candidates, in the
    order worth testing -- an interrupt going pending with nowhere to be delivered (the
    PMKERNEL comment's point: POPFD at CPL 3 cannot set VIF, and the kernel's delivery
    gate reads VIF), or a quantum/APC interaction with the far-jmp PM thread. The
    `skip` lever gives a CHEAP DIAL: vary how short loop 2 is cut and find the
    threshold length at which death starts. A threshold in INSTRUCTIONS points at the
    guest; one in MILLISECONDS points at the kernel's timer. **Measure the threshold
    before theorising further.**

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ LANDMARKS — the address arithmetic every probe needs. Get this wrong and │
│     you dump code bytes and "discover" nonsense (session 19 did exactly that).│
└──────────────────────────────────────────────────────────────────────────────┘
```
   obj1 (CODE) base 0x03AD0000     0501 BX:CX=0x00045000   [LE CODE OBJECT] in the log
   obj3 (DATA) base 0x03B40000     0501 BX:CX=0x00086000   <- 548,864 bytes, covers ALL BSS
   linear = base + offset. Disassembly operands are ALREADY obj3 offsets (unrelocated
   file values that the loader fixes against the DATA object), so [0x38fe0] -> obj3+0x38fe0.
   ⚠ Both bases were STABLE across all 11 runs this session. Re-check them in the log
     before trusting a probe address anyway -- one 0501 line, it costs nothing.

   CODE (obj1+off / linear)                  DATA (obj3+off / linear)
     0x1d1e0 / 03aed1e0  D_Display             0x32304 / 03b72304  viewheight   = 144
     0x1d1fe / 03aed1fe  call R_ExecSetViewSz  0x32308 / 03b72308  scaledviewwidth=288
     0x35a70 / 03b05a70  R_ExecuteSetViewSize  0x3230c / 03b7230c  viewwidth    = 288
     0x34e10 / 03b04e10  R_InitBuffer          0x38fe0 / 03b78fe0  setblocks    = 9
     0x35870 / 03b05870  R_InitTextureMapping  0x38fe4 / 03b78fe4  setdetail    = 0
     0x358fa / 03b058fa  end of loop 1  (HITS) 0x38ff8 / 03b78ff8  detailshift  = 0
     0x35924 / 03b05924  loop 2 outer body     0x34fe0 / 03b74fe0  viewangletox[0] = 289
     0x3593f / 03b0593f  loop 2 BACK-EDGE      0x39020 / 03b79020  xtoviewangle[]
     0x35943 / 03b05943  after loop 2 (NEVER)  0x01a84 / 03b41a84  finetangent[]
     0x1d180 / 03aed180  FixedDiv              0x04a84 / 03b44a84  finetangent[3072]
```

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ NEXT SESSION: START HERE                                               │
└──────────────────────────────────────────────────────────────────────────────┘

  **1. FIND THE THRESHOLD LENGTH.** This is the one measurement that splits the
  remaining hypotheses, and nothing should be built before it.
  ```
     a threshold in INSTRUCTIONS  -> the guest is doing something illegal eventually
     a threshold in MILLISECONDS -> the kernel's timer/quantum is killing the VDM
  ```
  ▶ **The back-edge skip is a 2-point dial only** (full 3.5M = dies; one pass ≈ 12k =
    survives), because a `skip` re-arms via `dpmi_bp_rearm_pending()`, which needs a
    host turn the stretch never grants -- so it fires ONCE and ends the loop. For a
    CONTINUOUS dial use Doom's own `screenblocks`: loop 2 costs
    `(viewwidth+1) x ~3073` and `viewwidth = screenblocks*32`. Write a `DEFAULT.CFG`
    into `C:\DOOMS` with `screenblocks 3` … `11` and the stretch sweeps ~300k…~990k
    instructions with **no host change and no guest patch** — a real user setting, so
    nothing about the run is artificial. Find where death starts.
  ▶ Timing needs a clock in the log: BP-HIT lines print `after NNNN svc`, not a time.
    Add `GetTickCount()` to the BP HIT and ASYNC lines before sweeping, or the
    milliseconds answer is unobtainable.

  **2. THEN ask why the kernel kills it.** Leading candidate, from the `PMKERNEL_PATH`
  commentary already in `main.c`: an interrupt goes pending with nowhere to land —
  **POPFD at CPL 3 cannot set VIF, and the kernel's delivery gate reads VIF**, so an
  in-process far-jmp PM guest can never be handed a hardware interrupt. The async
  SuspendThread injector exists to work around exactly that, and it is measurably
  ABSENT across the death window. `pmkernel.flag` (run PM under the kernel monitor
  instead of the far-jmp path) is the opt-in lever built for this question.

  **3. STILL OWED FROM SESSION 19: confirm the render is stably correct.** Needs the
  `pmbp` skip `03aed1fe 00000000 00000005` plus `capture.flag`, several runs, eyes on
  the physical screen. Not attempted in session 20.

  ⚠ **WHAT "SURVIVES" MEANT, PRECISELY.** With loop 2 cut to one pass, `xtoviewangle[]`
    is left almost entirely unfilled, so Doom exits a few hundred ms later
    (`run_ms=0xf72`, `plane-nonzero` all zero — **nothing was drawn**). The result
    measured is "**the VDM was not torn down**" (`STAGE2: complete` present, all three
    host exit paths silent), **NOT** "Doom rendered". Do not upgrade that claim.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ▶ EVIDENCE ARCHIVE + EXACT REPRO (session 20)                                │
└──────────────────────────────────────────────────────────────────────────────┘
```
   build/doomlogs/result_doom_004351.log   probe @0x35bbd -> HIT; finetangent dump
   build/doomlogs/result_doom_004622.log   probe @0x35943 -> NOT hit (after loop 2)
   build/doomlogs/result_doom_004651.log   probe @0x358fa -> HIT (end of loop 1)
   build/doomlogs/result_doom_004821.log   viewangletox TAIL dump (all -1)
   build/doomlogs/result_doom_004906.log   pmnoirq: wedges at I_StartupTimer -- INCONCLUSIVE
   build/doomlogs/result_doom_005042.log   rep BP in loop 2: fires ONCE (rep cannot re-arm)
   build/doomlogs/result_doom_005229.log   raised bail cap: still zero async near death
   build/doomlogs/result_doom_005514.log   nosb.flag: IDENTICAL death
   build/doomlogs/result_doom_005631.log   ★ loop-2 skip -> STAGE2: complete (run 1/2)
   build/doomlogs/result_doom_005921.log   ★ CONTROL on the same binary -> death
   build/doomlogs/result_doom_010039.log   ★ loop-2 skip -> STAGE2: complete (run 2/2)
```
  ⚠ **`build/` IS GITIGNORED, so those 11 logs (48MB) are LOCAL ONLY** and do not
    survive a clean or a fresh clone. The three ★ runs — the experiment/control pair the
    whole conclusion rests on — are therefore COMMITTED, gzipped (~190KB each), at
    **`docs/research/doom-loop2-stretch/`**, with a README that states the claim, how to
    read the logs, and what "survives" does and does not mean. If you need one of the
    other eight, re-run it; the recipe is below.
  Reproduce (sandbox MUST be off for share writes -- see RIG NOTES):
  ```
     printf '03b0593f 0 2\r\n' > /private/tmp/xpshare/pmbp.txt      # the survival case
     ARCHIVE=build/doomlogs TIMEOUT=300 ./scripts/bmqueue.sh doom
     rm -f /private/tmp/xpshare/pmbp.txt                            # ALWAYS clean up
     # verdict: grep -c 'STAGE2: complete'  ->  1 = survived, 0 = VDM torn down
  ```
  Commits: `1099104` (early-bail instrumentation + `scripts/bmqueue.sh`),
  `16ef457` (this analysis). Gates re-run GREEN after the instrumentation change:
  580/16 off-VM, check-imports, and bare-metal `selftest.com` 8/8 on the new binary.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ▶ RIG NOTES (session 20)                                                     │
└──────────────────────────────────────────────────────────────────────────────┘
```
   scripts/bmqueue.sh   NEW. Queues a target and waits on the RESULT LOG'S MTIME.
                        ARCHIVE=build/doomlogs archives each result -- doomrun.bat
                        overwrites ONE filename, which destroyed the 52MB reference run.
   sandbox              writes to /private/tmp/xpshare are DENIED by the tool sandbox.
                        Rig commands need it disabled; a blocked write looks like a
                        240s timeout, not an error.
   ICMP                 the box does NOT answer ping. Liveness = watcher.txt's mtime.
   headless_ms.txt      45000 on the share. Deliberate, standing, leave it.
```
  ⚠ **STILL NOT DONE from session 19: the render was NOT re-confirmed stable.** Every
    Doom run this session was a no-skip fault hunt with no graphics. The ~1-in-3
    flake remains unmeasured.
```
