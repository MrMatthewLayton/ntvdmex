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

## Final gate (`runs/s89/final/`, 02:50–04:11) — PASSED
- **Parity sweep on F:** 97.3% (1251 of 1286; s87 was 1233 of 1267). Disagreeing probes
  identical to s87's list plus **p_tick2c:1** = #262 case B, deliberately left open.
- **Interleaved P F P F** (P = s87 `56d212e7`): Skyroads n8/max_ms same band; Doom 49/59 s
  sounding on all four; ZAR sounded on F1 and F2. **P2 (the s87 build) was silent this
  time.** So the earlier s88 silences were #239's intermittent shape and affect both builds.
- **Shelf** F = 14/16 (Solitaire, Minesweeper, Charmap, Clock, Task List close; P doesn't).
- **Lemmings** P F P F identical: complete, 0 planar bail sites (#269's interpreter change).
- **#276 data:** real mode 3–6 ticks over the 200 ms wait (AX=1); PM still 1 tick (8001).

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

## Morning (user back): Win16 gaps, compared against stock
New tool `scripts/bm/w16pair.bat`: one program under ours, then stock, with screenshots
(`runs/s89/pair*/`, `crop.py` pairs them). Builds `B_w16a..h` (`runs/s89/`); last =
`5ec08713` (`f7ac308`), shelf 15/16.

| Commit | What | Seen on the rig |
|---|---|---|
| `e56c1a2` | dialog base units from the template font (+ font on system controls); MapDialogRect converts itself; BeginPaint sends WM_ERASEBKGND | Calc border right (#282); Terminal dialog 192x130 = stock (#283) |
| `be1374e`, `1300bc4` | modeless dialogs get WM_INITDIALOG with CreateDialogParam's lParam (+6) and first tab stop; BeginPaint DC clips children | Charmap grid; Sound Recorder title + X (#286) |
| `67e1d88` | the relay no longer erases for the guest; WM_ERASEBKGND forwarded by USER's DefWindowProc with its DC translated | Clock beige like stock |
| `54d298f` | font only to system controls; DefDlgProc's erase = dialog colour | |
| `671cf97`, `0dfbb95` | EnumFontFamilies; GDI dispatch runs enumerations; default WM_PAINT erases what is owed; DC clips siblings by style | Sound Recorder labels/background like stock |
| `f7ac308` | LB_/CB_ messages translated (WM_USER-based per class); owner-draw dropped for string lists | Charmap font list = stock |

Findings: USER's CreateDialog routine `seg1:0x4b4a` (lParam at frame +6); GDI's
dispatch never acted on `f.enumreq`; Win16 control messages overlap by class.
Open (in #162): WM_CTLCOLOR, WM_DRAWITEM, #278, Media Player, XP theme vs classic frames.
