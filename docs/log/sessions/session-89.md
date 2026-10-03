# Session 89 — unattended night (2026-10-03)

User away overnight; the plan was agreed in s88's last turn: gate the s88 build, #277,
Win16 gaps (#279, #216, #270, #278 buttons), the clock cluster (#261–#263, #276), the
interpreter reserve (#269), then a combined gate. **No push, no zip.**

User feedback at the start: Mario scroll good (#222 closed), no Skyroads lag (#177
closed), Doom sound "much improved" (#57 commented, left open).

## Where it stands (handoff)
- Branch `m9/completeness`, local only (pushed = `62e0194`).
- Builds in `runs/s89/`: `A_3c67faee.exe` (s88), `B_279/216/270/261/263/269.exe` cumulative.
- Rig jobs (detached, under `riglock.sh`): `gate0.sh` (s87 vs s88, interleaved),
  `w16test.sh` (#277/#279/#216/#270 + shelf), `oracle1.sh` (p_rtcw, p_tick2c, p_fdate).
  Logs `runs/s89/*.log`.

## Done (commits)
| Commit | Issue | What |
|---|---|---|
| `150b023` | #277 | `rt.bat` deletes `cfg\target.txt` after run / live / stop |
| (279) | #279 | DialogBox disables its owner's top-level window, re-enabled at unwind |
| (tool) | #279 | `rigshot xclick` = a real click on the X; `DISABLED` in `list`; `w16xclick.bat` |
| `4f82a5d` | #216 | USER 0x16c LookupIconIdFromDirectoryEx answered; 0xad kind = cursor(1)/icon(3), module vs predefined by hInstance; module cursors built from USER's bytes, cached |
| `77ce908` | #270 | GDI one-DC getters; GetDesktopWindow = 0x00e0 backed by a record; SYSTEMROOT + WOW32 0x7b GetShortPathName → 16-bit GetSystemDirectory has its drive |
| `8f43870` | #261 | CMOS clock writes (SET freeze/commit, DM, 12h) → VDM RTC offset; probe `p_rtcw` |
| `616ac4d` | #263 | files created/written after a guest date set carry the VDM date; probes `p_tick2c` (#262), `p_fdate` |
| `d037659` | #269 | interpreter: 0F 8x Jcc near, 0F 9x SETcc, ENTER n,0 |

## Findings
- **USER.EXE LoadCursor (`seg1:0x49c0..0x4b46`)**: NotifyWow(2) cache probe → if
  expwinver ≥ 3.0: FindResource(RT_GROUP_CURSOR) → **0x16c** picks the entry → FindResource
  (RT_CURSOR) → 0xad(kind 1, … bytes, size, name, hInst). LoadIcon is the same with
  NotifyWow(1), fIcon 1, 0xad kind 3. Stepped-over 0x16c ⇒ every module cursor NULL.
- **krnl386's system directory** = `SYSTEMROOT=` value through WOW32 0x7b + `"\SYSTEM"`
  (`seg1:0xce9f`, `0xc89f`).
- **#278 buttons**: class `sbutton` (app's own), not owner-draw; dialog items of app
  classes never get WM_CREATE. Recorded on the issue; deferred (both affected apps blocked
  by MMSYSTEM #5).
- Gate N1 (s88 build) ZAR silent, irq5=0 — #239's shape; awaiting N2.
