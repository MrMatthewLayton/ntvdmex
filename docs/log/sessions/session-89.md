# Session 89 — unattended night (2026-10-03)

User away overnight; the plan was agreed in s88's last turn: gate the s88 build, #277,
Win16 gaps (#279, #216, #270, #278 buttons), the clock cluster (#261–#263, #276), the
interpreter reserve (#269), then a combined gate. **No push, no zip.**

User feedback at the start: Mario scroll good (#222 closed), no Skyroads lag (#177
closed), Doom sound "much improved" (#57 commented, left open).

## Where it stands (handoff)
- Branch `m9/completeness`, **local only** (pushed = `62e0194`, end of s87). s88 + s89
  commits await the user's go-ahead to push.
- Tonight's cumulative build **F = `runs/s89/B_262.exe` (`2fa914f1`)**, built from `3426561`
  (later commits are docs/rules only). The final gate (`runs/s89/final.sh`, log
  `final.log`, results `runs/s89/final/`) leaves it in the rig's `bin\`. Manager
  `ntvdmex.exe` = `6a5666e0` (unchanged).
- `checks.txt` still holds s88's one test (Clock + X buttons), unanswered; F includes all
  of it. Next by-hand test after that: Paintbrush View > Zoom In — does the pointer change?

## Done (all verified unless marked)
| Commit | Issue | What | Evidence |
|---|---|---|---|
| `150b023` | #277 | `rt.bat` deletes `cfg\target.txt` after run / live / stop | rig: `cfg\` clean after a run |
| `…`/tool | #279 | DialogBox disables the owner's top-level window; re-enabled at unwind | rig: real click on Terminal's X (HTCLOSE) — DISABLED, stays; Notepad control closes |
| `4f82a5d` | #216 | USER 0x16c LookupIconIdFromDirectoryEx; 0xad kind = cursor/icon; module cursors from USER's bytes, cached; SetCursor resolves them | rig: Paintbrush `sidearow`/`pick` BUILT (were NULL). **Zoom pointer owed by hand** |
| `77ce908` | #270 | GDI getters; desktop handle 0x00e0; SYSTEMROOT + WOW32 0x7b | rig: bkmode 2, desktop ok/iswindow 1, sysdir `C:\WINDOWS\SYSTEM` |
| `8f43870` | #261 | CMOS clock writes (SET freeze/commit, DM, 12 h) | p_rtcw: agrees wherever 3 oracles agree |
| `616ac4d` | #263 | file stamps carry the VDM date after a guest date set | p_fdate: 26CFh three ways = 6.22, PCem |
| `3426561` | #262 A | INT 1Ah AH=01h moves DOS's clock | p_tick2c measured; case B (raw 006C store) left open on purpose |
| `d037659` | #269 | interpreter: 0F 8x Jcc near, 0F 9x SETcc, ENTER n,0 | superset fuzz OK, +10 checks; Lemmings in final gate |
| `c929636` | — | **riglock takes over only a DEAD holder's lock** (pid) | see lesson below |
| `9cb7820` | — | oracle-rules: rationales for the p_rtcw / p_fdate disputes | |

Shelf on F: 14/16 X-close, identical to s88 (Terminal is now *correctly* inert to a real
click; the shelf's posted WM_CLOSE still reaches it).

## Findings
- **USER.EXE LoadCursor (`seg1:0x49c0..0x4b46`)**: NotifyWow(2) cache probe → if
  expwinver ≥ 3.0: FindResource(RT_GROUP_CURSOR) → **0x16c** → FindResource(RT_CURSOR) →
  0xad(kind 1, hInst at +18, bytes +10, size +6). LoadIcon = NotifyWow(1), fIcon 1, kind 3.
- **krnl386's system directory** = `SYSTEMROOT=` through WOW32 0x7b + `"\SYSTEM"`
  (`seg1:0xce9f`, `0xc89f`).
- **#278 buttons** are class `sbutton` (app's own); dialog items of app classes never get
  WM_CREATE. Only SOUNDREC, MPLAYER, PROGMAN use app classes in templates. Deferred, on the issue.
- **ZAR silent on s88 in the first gate (N1)** — not reproduced in 7 clean runs (with and
  without the manager, after Doom). #239's intermittent shape; recorded there. A manager
  start-delay patch was built and NOT shipped (`runs/s89/mgrfix.patch`).

## Lessons
- ⛔ **riglock's 45-minute "stale" rule took over a LIVE 52-minute gate.** Two queued jobs
  then drove the rig on top of it (N2's ZAR and shelf, the first ZAR A/B — all void; the
  A/B's runs lasted 5–16 s and `ntvdmex.exe` was locked). Fixed: the lock records the
  holder's pid and is taken over only when that process is dead. Killing an overrun job:
  `kill -9` (its EXIT trap would swap `bin\` under the next job).
- ⛔ Don't edit a shell script a waiting job is executing (bash reads as it goes): kill
  the waiter and re-queue.
- `json.dump` reformatted all of oracle-rules.json; it is `indent=1, ensure_ascii=True`.
- `rigshot close` POSTS WM_CLOSE — it cannot test whether an X is live. `rigshot xclick`.

## Not done
- #276 (PM tick during INT 15h AH=86h): data run in the final gate only; the fix touches
  the PM IRQ0 batching Doom's timing was tuned on — not an unattended change.
- #262 case B, #278 WM_CREATE chain, #269 remainders (listed on the issues).
