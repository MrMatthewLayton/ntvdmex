# Session archive

Every working session's notes, split out of `return-ntvdm.md` — the single rolling
handoff file this project used until 2026-08-26. It had grown to 4,000 lines of
reverse-chronological narrative, which is a fine scratchpad and an impossible thing to
hand to anyone.

**The content is verbatim. Nothing has been edited or corrected**, including conclusions
that a later session refuted. That is deliberate: on this project the refutations have
been worth more than the conclusions, and a note that says *"session 22's cause for each
was wrong"* is only useful if session 22 is still there to be wrong.

- Looking for **where the project is now**? → [`docs/STATE.md`](../../STATE.md)
- Looking for **how something works, or why**? → the [wiki](https://github.com/MrMatthewLayton/ntvdmex/wiki)
- Looking for **what happened on a given day**? → below.

| Session | Date | Headline |
|---|---|---|
| [29](session-29.md) | 2026-08-26 | Host UI: a constant caption, a status strip reporting program/width/CPU mode, five menus folded into a tabbed Settings dialog. `rigshot` — the rig can see its own window at last. |
| [28](session-28.md) | 2026-08-26 | **MS-DOS 6.22's own COMMAND.COM runs.** Five defects in an afternoon, four in code every guest uses — biggest: INT 21h AH=0Ah blocked the exec thread, which deadlocks a shell absolutely. Settings move into the registry. |
| [27](session-27.md) | 2026-08-26 | **★ Doom's mouse fixed and user-confirmed.** A 32-bit EDI masked to 16 bits. Both previously filed explanations were wrong, and so was the headline "Doom never asks". |
| [26](session-26.md) | 2026-08-25 | Four fixes confirmed — and **three instrument errors in one session**. The headless rig cannot see input lag: every latency counter measured a path no key travels. |
| [25](session-25.md) | 2026-08-25 | **★★ Doom is playable with sound — the project's stated bar is MET.** Status bar fixed by *disassembling DOOM.EXE* (`I_ReadScreen` reads the write plane); PCM to 99.999% by pacing the PIT. |
| [24](session-24.md) | 2026-08-24 | **The timer is NOT starved** — session 23's central chain refuted at its first link. A counter's *layout* is a claim. |
| [23](session-23.md) | 2026-08-24 | Both remaining defects re-diagnosed. ⚠️ Its audio/timer chain is refuted by session 24; the status-bar half stands. |
| [22](session-22.md) | 2026-08-24 | **Doom is playable** — menu, a whole level, intermission, PCM + MIDI. |
| [21](session-21.md) | 2026-08-24 | **★ The five-session `R_ExecuteSetViewSize` death was OURS**: our own INT-site patcher rewrote a `jle`'s displacement. Doom plays its demo with sound. |
| [20](session-20.md) | 2026-08-24 | ⚠️ **Central conclusion REFUTED by session 21.** Kept for the landmarks and rig notes, which stand. |
| [19](session-19.md) | 2026-08-23/24 | Doom completes its entire startup matching stock ntvdm line for line and renders its title screen. Not playable yet. |
| [standing reference](standing-reference.md) | (sessions ~15–17) | Rig operations, instruments, landmarks and older traps, pruned of narrative. Includes the OPL timbre fix. |
| [13](session-13.md) | 2026-08-20 | M9 API complete; mode 12h is the wall. |
| [12](session-12.md) | 2026-08-19 | Checkpoint. |
| [11](session-11.md) | 2026-08-19 | Checkpoint. VME/VIF interrupt gating. (Notes that sessions 6, 8, 9, 10 were pruned as stale restart snapshots.) |
| [7](session-07.md) | 2026-08-18 | Checkpoint. |

> Sessions 1–6, 8–10 and 14–18 have no surviving block: some were pruned as stale restart
> snapshots at the time, and the earliest work predates this file. Their record is the
> commit history and `docs/research/`.
