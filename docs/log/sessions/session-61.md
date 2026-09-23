# Session 61 — Skyroads perfect: the crystal was on the wrong lock

> Session 61. Split out of `docs/STATE.md` on 2026-09-23.

> Verbatim from `docs/STATE.md`. Nothing has been edited or corrected,
> including conclusions a later session refuted — same rule as the rest of
> this archive. See [`README.md`](README.md).

## ★★★★★★ SESSION 61 (CONCLUSION) — **SKYROADS PERFECT: THE CRYSTAL WAS ON THE WRONG LOCK.**

**Date:** 2026-09-10. **COMMITTED (this session, on `m9/completeness`).** Skyroads is
**user-confirmed "absolutely perfect" while keys are held** — better than its Aug-19
best. The wobble was never the keyboard yield or delivery (those were the 14%); **86%
of stalls were the clock never GENERATING the tick**, because `host_pit_sync` took the
device lock `g_lock` (shared with the renderer, mixer, ~68k port-traps/s).

**THE FIX (baked, no knob needed):** the PIT is split into
- `host_pit_generate()` — advance the 8254 from QPC + latch IRQ0, under a NEW
     micro-lock `g_pit_cs`; port handlers 0x40–0x43 take it via a `guard` hook in
     `pit_state`. The crystal can no longer be blocked by the renderer. `gen` 86%→0.
- `host_pit_deliver()` — the async attempt still needs `g_lock` (never-suspend-a-
     lock-holder), but takes it with `HOST_LOCK_TRY`: a busy lock SKIPS, the tick is
     already latched, cooperative delivery places it. `pit_skip`~50k/session, harmless.
- **pacer priority HIGHEST→NORMAL** (baked): once generation stopped needing to win
     the lock, HIGHEST starved the present thread on the single core (stepped fades,
     dropped frames). NORMAL = perfect. ⚠ mechanism is a strong guess — `ui_gap` moved
     the WRONG way, see [[timing-fidelity-frontier]].

⚠ **FIVE arbitration "arms" (keyirq 0/1/2/3, courier) were all REFUTED in-game first**
— they fixed the 14%. All left in as `keyirq`/`courier` knobs, defaults are the
winners (`keyirq=1`, `courier=0`). Also this session: machine-jamming LL keyboard hook
OFF by default, single-instance mutex, `host_panic_release` on close, capture
watchdog, GH#132 three-strikes cleared per run, one-folder rig relayout, quoted
`target.txt` path parse. Det-test 30/30, imports clean, Skyroads headless `gen=0` with
ZERO knob files.

### ▶ NEXT ACTIONS, IN ORDER

0. **★★★ GAMES NEED SHORT 8.3 PATHS.** Doom (& likely others) won't LOAD from
      `games\<Name>\` — the long "Documents and Settings" path corrupts an INT 21h
      filename (`DOOM.EXE`→`DOOM.ETX`→`DOOME`). Hand the guest a `GetShortPathName` 8.3
      path. ⚠ The user is doing a one-by-one game pass and will report what works; this
      blocks every game that reads its own files by full path.
0b. **THE CLOSE-ZOMBIE may recur** (host holding its binary/log after window close);
      `controld` `kill` recovers it without a reset. Watch for it during the game pass.

### ▶ SUPERSEDED SESSION-61 ACTIONS (kept for the trail; the crystal fix replaced them)

1. **★★★★★ N REPEATED RUNS PER ARM — THE FIX IS BUILT AND UNPROVEN.** The tick
      courier (`courier.txt = 1`, ships **OFF**) is implemented and its dangerous half
      is validated, but a single 45 s run **cannot** resolve its effect: five runs of
      nominally the same configuration gave anomalous-gap counts of **50, 75, 81, 85,
      240**. Any A/B must be N runs per arm with the **run shape checked** — Skyroads
      sometimes plays a ~10 s intro at the BIOS 18.2 Hz and sometimes goes straight to
      180 Hz, and `IRQ0TL persec`'s first entry says which (`0x14` = intro, `0xb6` =
      no intro). Do not compare across shapes; I did once and it cost a round.
2. **★★★★ THE REAL LEVER IS THE PORT TRAP (2.33 us), AND THIS SESSION IS THE FIRST
      DIRECT EVIDENCE FOR IT ON THE WOBBLE.** IRQ0 and IRQ1 compete for one scarce
      thing — a moment when the guest is in exec with interrupts on. Every arbitration
      tried just moves the loss: yield on → the clock waits; yield off → **half the
      keystrokes go past 64 ms**; courier on → the clock gains a little and keys got
      worse. Arbitration cannot create windows. The trap cost is what destroys them
      (Skyroads' OPL helper alone runs ~68,000 port traps/s), and it is also handoff
      item #2 from s60. **Own session.**
3. `guests` (w15, 63%) is still the heaviest score lever; **CALC and WRITE remain
      unjudged** on the rig since s59.

### ★★★★★ WHAT THE RUNS ACTUALLY SAID

The s60 handoff pinned the wobble on **async delivery starving** — "75% of IRQ0
attempts bail `not_in_exec`" — and prescribed a lock-free courier. The first half of
that is measured and true; the inference from it is **wrong**, and two new
instruments say so:

```
anomalous gap = one spanning >= 2 of the period THE GUEST ITSELF programmed
STAGE2: IRQ0WHY gen=.. del=.. anom[raise,att,nie,yld]=..
```

| in the gaps where Skyroads missed a tick | run A | run B | run C | run D | run E |
|---|---|---|---|---|---|
| IRQ0s the 8254 generated | 110 | 192 | 85 | 119 | 362 |
| injection attempts made | 53 | 76 | 85 | 81 | 253 |
| **attempts never made** | **57** | **116** | **0** | **38** | **109** |
| **keyboard yields** | **57** | **116** | **0** | **38** | **109** |
| bailed `not_in_exec` | 4 | 1 | 0 | 0 | 4 |

**`raises − attempts == yields`, exactly, in every run.** Every tick that lost its
injection attempt lost it to `host_irq_sink` handing the timer's one async
opportunity per raise to a pending key (`g_keyirq_retry`, ON by default,
`KEYIRQ_MAX_YIELD` = 3 in a row = 16.7 ms at 180 Hz). The worst gap measured reads
`raise=3, yld=2, att=1` → **20 ms**. That is the wobble in four numbers, and it is
an **identity within each run**, not a comparison between runs — which is why it
survives the noise that sinks everything else here.

⚠ **`not_in_exec` IS THE HEALTHY BASELINE, NOT THE FAULT.** It runs at **76% during
the ~6,000 gaps that are keeping perfect time** and 0–4 events total inside the
anomalous ones. A statistic that is *higher* when the clock is working cannot be
what breaks it; the cooperative path absorbs those bails routinely.

⚠ **AND THE YIELD MUST NOT SIMPLY BE REMOVED.** `keyirq.txt = 0`, measured:
**51 of 102 keystrokes past 64 ms, worst 1864 ms** (against 0–1 and 5–250 ms with
it on). That is the "loses keys" result this file already records twice.

### ★★★ TWO DEFECTS IN MY OWN INSTRUMENT, BOTH FOUND BY RUNNING IT

1. **THE "LONG GAP" THRESHOLD WAS A FIXED 8 ms AND THE GUEST CHANGES THE RATE UNDER
      IT.** Skyroads' intro runs at the BIOS 18.2 Hz (55 ms), so **187 of the first
      run's 319 "big gaps" were the intro ticking correctly** — 187 measured against
      ~182 predicted from the timeline. The threshold is now **relative to the period
      the guest programmed**, which is the only frame that means anything.
2. **I/O PER *GAP* IS CONFOUNDED BY GAP LENGTH.** A gap ten times longer collects
      ten times the I/O whatever the guest is doing. ⚠ **The s60 note records the
      music-overrun hypothesis as REFUTED on this number** (`big gaps 5.3 io/gap vs
      360`); the same raw ratio came out **25x the other way** on this session's runs.
      Neither figure means anything. As a **rate** the answer is clean and does
      confirm the s60 conclusion: **0 port ops/ms during anomalous gaps against 169/ms
      during normal ones.** The guest is not hammering the OPL when the clock slips.

### ★★★ THE TICK COURIER — BUILT, DEFAULT **OFF**, HARD PART VALIDATED

`tick_courier_thread`: a thread that retries a still-pending IRQ0 **outside
`g_lock`**, woken by the raise site, one tick per wake, bounded by
`COURIER_BUDGET_US`. An immediate second attempt in the sink cannot work — injecting
IRQ1 leaves the guest entering INT 09h with interrupts off — so the retry has to
wait microseconds for the handler to IRET, which nothing in the old structure could
do. **V86 only (`!g_dpmi_pm`), so it cannot regress Doom.**

★★ **THE `g_lock` INTERLOCK PROBLEM IS SOLVED, AND THE SOLUTION IS MEASURED.**
`host_irq_sink`'s note names the prerequisite for ever moving a suspend outside the
lock: *"a separate suspend-safe handshake (the exec thread marking itself
un-suspendable while it holds g_lock)"*. That handshake already shipped — the CPU
throttle's — and it is now in `async_inject_irq`: re-read `g_in_exec` **after** the
suspend has landed (`GetThreadContext` is what makes "landed" true) and resume
instantly on 0. **`left_exec` fired 4 times in 45 s**: four real races where the
guest left `v86_run` between the pre-check and the suspend. Without it those are
four suspends of a possibly-lock-holding thread. Plus `g_async_ctxwr`, a
single-context-writer interlock (**`ctx_busy` = 52**), because two threads building
IRET frames from the same context would collide on the guest's stack —
previously safe only *by accident*, since every caller ran under `g_lock`.
`vdd_pic_ack_autoeoi()` replaces the ack-then-eoi pair for auto-EOI'd lines: the
pair's transient set/clear of the shared ISR byte is not safe without the device
lock, and losing IRQ1's in-service bit is the "press a key and everything hangs"
fault.

⚠ **IT SHIPS OFF BECAUSE THE DATA DOES NOT SUPPORT TURNING IT ON.** Its one run cut
delivery-caused gaps (30 → 19) while generation-caused gaps rose, and key latency
was worse — the same competition running the other way. Against a 5x run-to-run
spread, none of that is a result. See next action 1.

---
