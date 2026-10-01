# Session 85 — an unattended night: #172 isolated, #238, #205

**Date:** 2026-10-01 00:15 → morning · **Branch:** `m9/completeness` · **Nothing pushed.**
**Mode:** unattended overnight. The user's plan for the night: (1) post the #172 and #226
notes and settle the owed-tick gate change, (2) #238, (3) #205, then update `checks.txt`.
Before leaving, the user closed out everything that was waiting on them: 23 issues
(#237 #228 #229 #224 #231 #235 #232 #233 #223 #226 #230 #218 #168 #167 #165 #166 #187
#143 #142 #58 #212 #139 #170). Bugs found later get new issues.

---

## 1. #172: the owed-tick gate change was dropped (`be3e788`)

`2a86dcb` shipped two changes: the mode-Y interpreter's `irq` stop, and the PM cooperative
timer arm opening on the owed COUNT as well as the latch. Isolated on Doom's menu-quit route
(`runs/s85/owed/`, `dq.sh`, keys Esc/Up/Enter/Y, interleaved ×3, HEAD `67ae89bf` against
latch-only `10bd5a99`):

| | quit window IRQ0 | REPLAYED_LOUD |
|---|---|---|
| owed count (HEAD) | 139/s ×3 | 50, 48, 50 |
| latch only | 140, 140, 139/s | 45, 45, 44 |

The owed-count arm bought nothing and cost ~5,800 declined injections per run in DOS/4GW's
16-bit start-up. Dropped. #172 and #226 notes posted.

## 2. #238: the PIT pacer woke 485 times a second (`39e96c4`)

**The s84 diagnosis was wrong.** "78% of attempts on IF=0" was the *cooperative* gate
(`vtib01`), reading the VTIB at 3DBench's only trap, the EOI inside its own ISR. New
instruments (all print on the headless forced exit, which is how 3DBench runs end):
`STAGE2: async why` (moved into `async_why_report`), `STAGE2: exec share (#238)` (guest vs
host time per event, the busiest BOPs), `STAGE2: irq0 coop skip` (by caller of our stubs),
and `STAGE2: persec raise/async/coop/nie/bop/io/hostms/pace`, a per-second timeline.

What they showed (`runs/s85/3db/`):
- 3DBench has two phases. Title (0–5 s): 448,000 INT 16h BOPs a second ("key or 91 BIOS
  ticks"). Benchmark: no BOPs, one trap per tick (the EOI).
- In the benchmark, 1,000 raises/s → ~500 delivered and ~500 `not_in_exec` bails.
- **The pacer woke 483–494 times a second.** `Sleep(1)` on XP is ~2 ms even under
  `timeBeginPeriod(1)`. The pacer is the only thread whose attempt can land on a guest
  that never traps, and one wake places one tick.

**Fix:** the pacer waits on a periodic `timeSetEvent(1 ms, TIME_CALLBACK_EVENT_SET)`
(bound by name; the `Sleep` loop is the fallback). Wakes ~955/s; **delivery 50% → 93.5%**.
`courier.txt=2` adds ~1.5% more, so it stays off.
**Gate**, A/B ×2 against `51c72ed0` (`runs/s85/gate/`): Skyroads IRQ0 0x117d/0x117f against
0x1180, stretches n8 21/33 against 36/33 (the s84 norm is 25–60, so memory's "n8=0" is
stale), Doom and ZAR audio identical, shelf identical except one Notepad harness TIMEOUT on
F1 (4/4 on re-run).
**Open:** the last ~6%; and 3DBench still shows 00.0 at 170 s. Its score comes from timing
one full demo cycle, and the frame countdown `[B828]` never starts. That is a separate
question, not investigated.

## 3. #205: the PM fault stack was inside the program

Doom typed at 6.22's COMMAND.COM **did not reproduce on the current build** (it ran), but
did on s81's `0e5b10e7`. Then `cfg\pmchg.txt` (`2fd2 0`) watched DOS/4GW's selector-table
pointer (`[0AA2h]` in its data segment, linear 0x2FD2 in that layout; the faulting code is
`mov es,[0aa2] / mov byte es:[bx],3` at 01a7:2efe):

```
0x2FD2: 0x50 -> 0xaf  DOS/4GW init stores its selector
0x2FD2: 0xaf -> 0x00  across the reflect of its first raw INT 21h (AX=30FFh)
```

**Cause:** the PM fault-reflect stack selector was based at linear **0x2000** (top
0x3000). It was meant to be "below the program" when programs loaded at segment 0x1000; they
load at ~0x243 now. Every reflected PM fault wrote its frame at 0x2FC0..0x2FDF *inside the
running program*. Under XP's larger shell the program sits 448 bytes higher; on the current
build one paragraph higher (PSP 0x244), so the pointer escaped by luck. The scribbling did
not stop: a watch on 0x2FD4 changed **9 times** in one Doom run on `b4077a23`.
**Fix:** the stack is a 64 KB host static (`g_flt_stack`), like `g_flt_tbl` beside it.
Watch on 0x2FD4 with the fix: **no change**; FLTSTK `lin=0x0f711c40`; Doom runs from 6.22's
shell. Battery green.

---

**Rig `bin\`** and **`checks.txt`**: see the end of this file.

## Where it stands (end of night)

- **HEAD `53ba893`**, rig `bin\` = **`b709ec6a`** (= HEAD's host). Rollback
  `debug\prev\ntvdmhost_67ae89bf.exe` (yesterday's; that file had been holding tonight's
  latch-only test variant `10bd5a99` by mistake, now corrected and md5-checked).
- **`checks.txt`**: one test, Skyroads by hand (steering, music, speed) on the new pacer;
  quit from inside the game. `report.txt` empty.
- Issues: #172, #226, #238, #205 have tonight's findings. #238 and #205 left open for
  the user. Nothing pushed.
