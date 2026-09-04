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
| 2026-09-04 | **61.7** | 63.6 | 67.7 | 26.2 | Minesweeper menu + Game>New reset. LoadMenu downcall, filtered-take deadlock, accelerators. 893 checks. |
