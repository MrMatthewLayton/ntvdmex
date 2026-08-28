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
| [35](session-35.md) | 2026-08-28 | **★★ A stepped-over call is not inert — it answers, at random.** The harness logged unimplemented WOW32 calls as *"registers untouched"*, which is true of registers and false of the result: the thunk's `sub sp,4` hole is never written, so krnl386 pops **stack litter** and branches on it. Printing that value settled two questions in one run — `0xc6` read `0x01b7` (non-zero → its caller's `or ax,ax / jne` took the **failure** path) and `0x2d` read `0x2714` (`>= 0x21` → `LoadModule` took the **success** path into `les si,[bp+6]` with a NULL parameter block, which *is* the terminal `#GP`). ⇒ **session 34's "next: implement `0x2d`" is premature**: `WowLoadModule` is reached only because `LoadModule` already failed with `AX=0x17`, and we are causing that. Also: the enclosing function **names itself** `LoadModule` through its own `LoadStart/LoadSuccess/LoadFail` strings (with a **narration switch** at `ds:[0x12b0]` still to be turned on), and two lies were fixed in `nedis.py` — capstone stopping dead at the first bad byte (a *silent* empty window), and `--wowfunc` scanning seg1 only, which reported **`0 caller(s)`** for the very call the run stops on. Sessions 33–34 were also found uncommitted and are now committed. |
| [34](session-34.md) | 2026-08-28 | **DPMI exceptions are delivered, and a raw `INT nn` in protected mode stops being fatal.** NT builds the DPMI 0.9 exception frame *itself* and leaves only the return `CS:IP` — measured against krnl386's deliberate `UD0`, whose every field was known in advance. **★ The fault table is indexed by the x86 EXCEPTION VECTOR, not an NT class** (#UD→6, #GP→0x0d): session 19 refuted, and the 8-entry table could never reach index 13. Eleven walls behind it, all ours — `AH=52h` had no PM thunk; a zero SFT chain head sent krnl386 round the IVT forever (117 MB); 64 file handles is a number it *refuses*; the host pool silently starved the PM handler table; WOW32 `0x98` is the file **seek**; `wowdecline.py` was under-reporting; **declining is a property of the CALL SITE, not the ID**; **a reserved LDT index is not a read-only one**; and **no commit-time scan can patch a region the guest fills after declaring it** — so a `#GP` with the IDT bit set is serviced as the interrupt it is. PM step `0x63` → **`0xd9`**, SYSTEM.DRV loads, and the frontier is now `WowLoadModule` — the 16→32 boundary itself. |
| [33](session-33.md) | 2026-08-28 | **The stock oracle answers.** `vdmdump.exe` reads a *live* stock ntvdm from outside (regions, low 1MB, needles, and its LDT via `ProcessLdtInformation`). Stock keeps krnl386's **whole file image resident** (`0x16440` bytes at `0x899f0`); **★★ the `LoadSegment(2)` wall is DOWN** — segments 2 and 3 load, at the same heap offsets stock uses, and `ExitKernelThunk` is gone from the run. Cause: session 32 zeroed the arena *gap* to stop a crash, and that gap is the mechanism that walks the staged image, so every segment was copied from the NE header. Then an "unimplemented BOP" turned out to be a **swallowed `INT 21h`** — our patch map is keyed by address and krnl386 copies its code — recovered from the module's file image. **krnl386 is now ALIVE at the end of the run**, waiting on a DPMI exception it triggers on purpose. Two refutations kept (staging truncation; widening the segment table). |
| [32](session-32.md) | 2026-08-27 | **krnl386 relocates itself** — a chained NE fixup can only be applied once, and krnl386's own pass is the one that converts paragraphs into selectors, so our loader must not pre-relocate it. `ExitKernelThunk(1)` cleared; the wall moves to `LoadSegment(2)`. |
| [31](session-31.md) | 2026-08-27 | **The WOW32 interface pinned to the byte**: args at `bp+16`, return value in a stack hole at `bp-16`, 29 of 82 ids self-named, and declining (`0xFFFF`) chains to real DOS. |
| [30](session-30.md) | 2026-08-26 | **Repo made public** (history purged of DOOM1.WAD first), wiki published, tracker reconciled 58→140 issues, docs consolidated. Then **#129 Win16 passthrough proved IMPOSSIBLE** (Windows validates the VDM image identity) and **#128 WOW started**: NE loader works, krnl386 loads and relocates on the rig. |
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
