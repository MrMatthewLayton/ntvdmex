# Progress — the daily number

**One row per day, newest first.** Written by `tools/score/score.py --append`; the
model it computes from is [`tools/score/model.json`](../tools/score/model.json).

```bash
NOTE="what moved today" ./tools/score/score.py --append
```

**Target:** +1–2%/day, ≤1 month of solid work remaining (set 2026-09-04).

**Baseline, 2026-09-04:** overall **60.8** (DOS 63.6 · WoW16 65.7 · Product 26.2).
The table keeps ONE ROW PER DAY — a re-run replaces that day's row rather than
adding a second, so a day scored three times cannot read as three days of work.
That means the baseline row is overwritten by the day's final score; it is
recorded here so the first day's delta is not lost.

> ⚠ **Read the number as a rate, not a verdict.** Two of the fourteen Win16 items
> are measured from the binaries every run; everything else is a human attestation
> with its evidence recorded next to it in `model.json`. The score is exactly as
> honest as those attestations, and the way it goes bad is someone raising one
> because the thing *looks* done. In this project things that look done have been
> wrong four times in four sessions — a stepped-over call still answers.

> ⚠ **This baseline is lower than the 65% quoted on 2026-09-04, and nothing
> regressed.** That review scored DOS against the *games* bar (Doom/Skyroads/ZAR
> → 85%) and Win16 against the *completeness* bar, then averaged them. Scored
> against one bar — an `ntvdm` superset on XP-32 — DOS is ~63%, because a
> completeness bar counts INT 13h, TSRs, the error model and shell redirection,
> none of which a game needs. Same project, one bar, from here on.

| Date | Overall | DOS | WoW16 | Product | What moved |
|---|---|---|---|---|---|
<!-- SCORES -->
| 2026-09-06 | **79.6** | 85.4 | 78.7 | 57.5 | #136: 16 more settings honoured (7 → 23 of 47) — audio, display, XMS/EMS, the floppy image. Found the **PC speaker made no sound at all** and fixed it; corrected the score's OPL2/3 claim to OPL2. Suite 1036→1086, and **rig-gated after all** — the box was up; a sandboxed LAN probe fails silently and had read as dead. ★ **#47 CLOSED**: `MEM.EXE`'s phantom `Upper 1,663K` was a second absolute-offset read — MEM keeps the SysVars *segment*, discards the offset, and reads `:0x008C` as the conventional/upper line. Every figure now matches the 6.22 oracle's shape row for row. **Afternoon:** menu-defect CLOSED (Paint holds the capture; stock moves it on Alt). **~80 services** across four batches -> CLOCK, MPLAYER, RECORDER, CHARMAP all launch; SOL, WINMINE, CHARMAP user-confirmed DONE (6 done, 5 partial of 19). Install/uninstall verified behaviourally. Launch matrix gains a Win16 row: our Character Map is **pixel-identical to stock** in the client area. Suite 1086. |
| 2026-09-05 | **72.4** | 83.7 | 67.7 | 43.0 | #44,#49,#50,#45,#52,#132 CLOSED; #15 REFUTED; #47 root cause (SDA on SysVars+0x45); #130 target.txt precedence; INT 20h child-exit; launch matrix comparison real -- stock REFUSES MEM.EXE. Suite 893->1036. |
| 2026-09-04 | **61.7** | 63.6 | 67.7 | 26.2 | Measured the redraw: log 9%, pump 0%, repaint ~10ms. Latency hypothesis refuted. |
