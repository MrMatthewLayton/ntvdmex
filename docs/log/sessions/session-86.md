# Session 86 — the morning: PIF files and every easy ticket

**Date:** 2026-10-01 09:00 → midday · **Branch:** `m9/completeness` · **Nothing pushed.**
**User, at the start:** Doom and Skyroads still play fine on the 1 ms pacer (s85 closed #238
and #205). QBasic's Make EXE works once QuickBASIC's own paths are set (#171 closed); #172
closed. Asked for: PIF loading (`C:\QB45\QB.PIF` → `QB.EXE /L`, needed for CALL ABSOLUTE),
then all remaining easy tasks, ready for testing late morning or this afternoon.

Background agents in worktrees took #181, #176, #193 and #200; I took #243, #206, #164 and
the rig.

---

## #243 PIF files (`18fa893`)

Explorer queues the PIF itself as the VDM's program (`command fetch ... app=[C:\QB45\QB.PIF]`),
and #208 routed it through COMMAND.COM as a program: the "sidescrolling cursor". New
`src/host/pif.h` reads program (24h), start directory (65h) and parameters (A5h; the WINDOWS
386 section's copy wins); off-VM `pif_test` holds it to the user's own QB.PIF (12 checks).
Relative programs: start directory, then beside the PIF, then PATH; not found → a shell, never
the PIF's bytes. Rig: `PIF -> program [C:\qb45\QB.EXE] dir=[C:\qb45] params=[/L]`, and
QuickBASIC opened `C:\QB45\LIB\QB.QLB` — it does that only under `/L`.

## #206 INT 15h waits (`82db39e`)

AH=86h re-executes its BOP until CX:DX µs pass (the INT 16h pattern). For interrupts to flow
during the wait, the IRQ0/IRQ1/device gates now read the CALLER's IF in our BIOS stubs
(`DOS_CTAB_SEG:DOS_BIOS_STUBS`) as they already did in `DOS_HDLR_SEG` (`our_stub_cs_ip`).
AH=83h: a countdown the pacer posts to bit 7 of ES:BX; BDA 40:98/9C/A0 in step; one at a time.
AH=4Fh: CF=1, AL unchanged. New probe `p_int15w`: all rows AGREE on msdos622, dosbox-x, pcem
and ntvdmex; the previous build fails six. PCem's AMI answers 4Fh with AH=86h (recorded in
oracle-rules.json). INT 09h actually calling 4Fh is #244.

## #164 Win16 current directory (`e839020`)

New probe `tools/wintest/w_cwd`, ours vs stock on the rig: stock puts the task in its launch
folder; ours in C:\WINDOWS. Three causes:
1. WOW32 0x70's third buffer is the launch directory (we sent "", so WOWEXEC's
   SetCurrentDirectory failed before LoadModule) — now CSRSS's `cur=`.
2. Our scheduler switches tasks, so it keeps each task's directory (krnl386's TDB holds only
   the drive: `TDB+0x66 = 0x82`, `+0x67` empty) — recorded at launch and every WOW32 0x82,
   restored on resume.
3. INT 21h AH=47h answers the 8.3 path upper-cased, as DOS keeps it.
`w_cwd` now AGREES on curdrive and curdir; the file lands in the launch folder on both. (The
handle NUMBER differs, 12h vs 6 — a separate question.) p_drv: our 44 rows unchanged.
⚠ The probe's first runs created `C:\WINDOWS\W16REL.TXT` on the rig; deleted and verified.

## Agents

- **#181 UART** (`a26d0ea`): the OUT2 gate was already in `comm_update_irq`; the inventory had
  mis-marked it MISS. Pinned with tests; four COM slots, COM3/COM4 not fitted (needs a stock
  measurement). Remainder #245.
- **#176 DMA** (`51349ce`): status DRQ bits derived from the devices; command bit 2 disables a
  controller through one helper (`vdd_dma_grants`). ⚠ A masked SB channel now WAITS (as the
  card does) instead of raising a false end-of-block IRQ — gated on Doom/ZAR audio below.
  Remainder #246.
- **#193** answered: already fixed in s56 (`5b1f2ae`) — the `#GP(IDT)` raw-INT arm was what
  patched WIN87EM's FP INTs. Closed; the memory note that spawned it corrected.
- **#200 inventories**: six re-marked, six new (BDA, IDE, MSCDEX, DPMI, EXE formats; OPL/EMU8K
  refreshed). Gaps filed as #247–#256; notes on #179, #241.

Battery after the merges: **2224 checks, 0 failed**.

## Gate, and where it stands

Rig gate, interleaved A/B ×2, `b709ec6a` (this morning, user-confirmed) vs `f800ec5b` (HEAD):
Skyroads IRQ0 0x117a/0x1179 vs 0x1177/0x1178; Doom IRQ5 5068/5059 vs 5063/5067; ZAR IRQ5
716/723 vs 718/716; Doom/ZAR sounding seconds identical; Win16 shelf identical except one
Recorder harness TIMEOUT on N1 (4/4 on re-run). PIF launch re-checked on the combined build.

- Rig `bin\` = **`f800ec5b`** (HEAD's host); rollback `debug\prev\ntvdmhost_b709ec6a.exe`.
- `checks.txt`: QuickBASIC from QB.PIF + CALL ABSOLUTE. `report.txt` empty.
- Closed: #171 #172 #193 #206 #181 #176 #164 #200. Open for the user: #243 (PIF).
  Filed: #244 (INT 09h → 4Fh, PM C0h/87h), #245 (UART), #246 (DMA), #247–#256 (inventory
  gaps), #257 (inventory leftovers).

---

## Afternoon batch (user: "#258 plus another batch; test this afternoon; then a zip for a friend")

User's report on #243: QB loads from QB.PIF and MOUSE.BAS (CALL ABSOLUTE) works; Make EXE
from the PIF failed.

- **#258 (`5d0676b`)** — not the PIF: `QB.EXE /L` direct failed the same. QB's Quick Library
  loader takes **AX after a successful AH=4Ah** as the block's segment; we left the caller's
  `4Axx`, so QB.QLB loaded at 4AD1h (block was 9CD1h), `~QBLNK.TMP` got a garbage `.LIB`, and
  Make EXE freed a block that never existed ("Error in loading file (QB.QLB) - Internal
  error"). New probe `p_memax`: all three oracles return AX=ES after 4Ah; the Microsoft
  kernels return AX=ES-1 after 49h (dosbox-x leaves it). Fixed both; QB /L direct and via a
  PIF now build T_CAVE.EXE linked with QB.LIB. On the way: EXEC re-owns the child's program
  block (was 0100h).
- **#251, two parts (`043b5cc`, `316fd16`)** — stdin through a handle is a cooked console
  line (was EOF), rig-tested with `t_stdin`; IOCTL 44h unsupported sub-functions answer
  invalid-function (new `p_ioctl2`, every row agrees except an AH the oracles dispute).
  AUX/PRN still open.
- **Agents, merged:** #253 BDA (0010/0013 written, 006C seeded, a real 1 KB EBDA at 9FC0h,
  C1h answers it), #174 8259A (rotation, SMM, ICW1 resets, fully nested slave — slave held
  while master IR2 in service), #247 DPMI 0300h (every vector through IVT[BL], all registers
  written back, BIOS stubs serviced in nested loops via `v86_bios_bop()`; rollback knob
  `cfg\simintrefl_off.flag`). The #247 merge conflicted with the BDA edits in the moved BIOS
  block; ported INT 12h and C1h into `v86_bios_bop()`. Probes p_int15/int15w/bios/memax/
  ioctl2: 0 mismatches on the merged build. Battery 2340/0.
- README refreshed for the zip (`78d6942`).

## Evening (2026-10-01/02) — Duke3D input lockup, DirectDraw tearing

- **#259 Duke3D fullscreen input lockup.** `GetMessage` serves posted messages before input; `WM_APP_PRESENT` is re-posted as soon as the frame body starts, so once a body took ~a frame period input was never reached (40 keys held 11 s, raw mouse frozen, Alt+Enter dead — the guest and the frame body ran on). Fix: the top-level pump drains queued input before each present. Worst key latency 31 ms after. Logs: `runs/s86_duke/`.
- **#260 DirectDraw fullscreen tore more than GDI.** The flip ignored VSync. Round 1 timed it ourselves (wait_vblank + NOVSYNC): tear-free, but the new flip counters showed the driver (Quadro K4000, NVIDIA 321.01) **already syncs every flip** — we waited twice, present cost a whole frame (16 ms), snapshots landed mid-frame and Mario's text flickered. Round 2 (shipped): two back buffers + `DDFLIP_DONOTWAIT`, drop a frame rather than block; fallback to our timing if flips land at once. Present 1–3 ms; user: Doom/Duke3D/ZAR/Mario butter smooth. User also felt Doom's audio improve — plausible (less UI time under `g_lock`), **not measured**.
- Rig display: Quadro K4000 / 321.01 (FX 1800 removed); UltraVNC mirror driver `mv2` installed (`scripts/bm/gpuinfo.bat`; its dxdiag half produced nothing).
- Duke3D `DUKE3D.CFG` on the rig remapped to modern FPS keys (backup `DUKE3D.CFG.bak`; mouse look = U toggle).
- Gate `runs/s86_gate/` (A=799968bd, B=978589de, ×2): Skyroads/Doom/ZAR/shelf unchanged; A1 ZAR silent was a single baseline-side run; B2 shelf cut off by session end (B1 = A1 = A2).
- Commit `5e1b739`; rig `bin\` = `978589de`. Nothing pushed.
