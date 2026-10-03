# Session 90 — 2026-10-03/04: twelve issues, the score to 90% (unattended)

The user asked for 10 issues fixed in an unattended run, chosen to improve the score.
**Twelve were closed**, each verified on the rig (most against stock NTVDM). Two more moved
forward (#295, #306), and six follow-ups were filed (#307–#312).
Score: **87% → 90%** (`tools/score/score.py`; MS-DOS 92, Win16 91, Product 78).

Pushed through `ff72ec9`, then the score commit. Rig `bin\` = **`d0699606`** (`runs/s90/AA_ctl.exe`),
plus `bin\wowshim\WOW32.DLL` and `NTVDM.EXE`, which are **new and needed** (package.sh ships them).

## Closed

| # | What | Verified by |
|---|---|---|
| #299 | SOUND.DRV gets its own table; stock answers 0 to every call | `w_sound` 23/23 = stock (it used to read 0 only through the step-over) |
| #296 | EnumFonts, EnumObjects, EnumProps | `w_genum` 28/28, `w_props` 20/20 = stock. EnumProps: oldest first, atom properties arrive **by name**, an empty window answers **0** (all three documented guesses were wrong) |
| #297 | GetClipboardFormatName, DlgDirSelect, SetParent, GetClassInfo, ChildWindowFromPoint, CallMsgFilter, GetInternalIconHeader, SHELL 38/39, GetKBCodePage, SetPaletteEntries (+ #295's extent getters) | `w_misc` 44/44 = stock. **WindowFromPoint had read its POINT backwards since s53** |
| #5 | Generic thunks, both directions | `w_gthunk` 16/16 = stock (mask bit order: CallProc32W bit 0 = last parameter, Ex = first) |
| #278 | Sound Recorder plays a WAV | Stopped → Playing 0.98 s → Stopped 2.00 s (`runs/s90/srp_play5.png`) |
| #270, #271 | The five owed probes vs stock | 99/99 equal; baselines kept in `tools/wintest/stock/` (13 probes) |
| #132 | SAFE MODE skips six subsystems | `scripts/bm/safemode.bat`: SAFE `[audio: no device, silent pump]`, selftest 8/8 |
| #245 | 16550 FIFO/break/loopback gate/THRE; COM3/COM4 like stock | comm_test 99; `p_com34` stock = C823 + 4 COM bases; `comport.com` all OK |
| #301, #303, #304 | BM_, focus wParam, EM_/LB_/CB_ pointer + structure messages | `w_ctl` 30/30 = stock |

## The multimedia chain (#278), which took most of the evening

1. **A fault inside a nested guest run was never delivered.** MMSYSTEM's #NP (segment 8 not
   yet loaded) happened inside `wow_call16_sync_ex`. That loop handed the fault-site BOP to the
   WOW dispatcher, where 0x57 is also the callback id, and it was "stepped over" 309,601 times.
   The new `dpmi_nested_fault()` delivers it to krnl386's handler (WOW only).
2. **MMSYSTEM has its own WOW table**, of exactly two ids: 2 = mmCallProc32 and 1 = a yield
   (`src/wow/wowmmedia.h`). Its ids are additive on KERNEL.581 `__MOD_MMEDIA`, which is 0 here.
3. **winmm finds WOW32.DLL and NTVDM.EXE by module name.** `src/shim/wowshim.c` is built twice
   into `bin\wowshim\` and loaded first thing in WinMain, because winmm caches "not under WOW".
   `call_ica_hw_interrupt` is **stdcall** (winmm pops nothing after the call).
4. IRQs raised that way are delivered from the PM loop **and from the GetMessage wait**.
5. MM_WOM_* etc. were missing from the relay's whitelist; they are now relayed verbatim.
   lParam is the program's own 16:16 WAVEHDR.
6. SetDlgItemText now goes through SetWindowText's path. Both set the real text **before**
   sending WM_SETTEXT, which fixed a status line that lagged one update behind.

## Also found and fixed on the way
- **The common dialog opened in Doom's folder.** XP's per-executable MRU is shared by every
  program we run; a NULL initial directory now means the task's directory, as in 3.1.
- **#306 (partial):** an idle WowWaitForMsgAndEvent now blocks for up to 50 ms. CPU went from
  100% to about 10%. The linger is a **third task** that the two-task scheduler loses.
- **Log loss:** the recovery block dropped its own line before the next *truncating*
  `log_write`, so "consecutive failed starts" had never reached any log. Separately, a
  "DOS: SysVars" line is written at offset 0 over the head; that one is not fixed.
- `audio_wave_start` zeroes its struct, which wiped `force_silent`. The rig caught it.

## Filed
#307 DragObject (a real drag loop) · #308 subclassing refused · #309 WOWCallback16Ex /
WOWGlobal*16 · #310 Win16 probes into dosdiff · #311 **LPT layout differs from stock**
(3 ports, LPT1=3BCh: the user should decide, since three DOS oracles say 378h) ·
#312 serial passthrough.

## Gates
- Shelf 15/16 after every Win16 batch (Terminal stays behind its modal dialog, as before).
- Skyroads, interleaved A/B: A (4844507f) n8 0x23/0x12; B (tonight) n8 0x10/0x2b; max_ms
  10–11 on both, so no regression. Guard line correct.
- Off-VM battery green: 46 binaries, 2769 checks.

## Owed by hand
`checks.txt`: Sound Recorder plays TONE.WAV. **Do you hear the tone?** That decides whether
SOUNDREC goes from partial to done.

## New tooling
`scripts/w16drive.sh` (drive a Win16 program from the Mac) · `scripts/bm/dosours.bat`
(dosstock's twin for our host) · `scripts/bm/safemode.bat` · `w16.inc` imports up to two
extra modules · probes `w_sound w_genum w_misc w_props w_gthunk w_ctl` and `p_com34`.
