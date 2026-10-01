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
