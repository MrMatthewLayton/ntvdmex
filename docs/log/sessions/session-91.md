# Session 91 — 2026-10-04: the score towards 95% (unattended)

The user confirmed Sound Recorder by ear and asked for an unattended run toward **95%**,
easy issues first. Score: **90% → 95.0%** (`tools/score/score.py`; MS-DOS 98, Win16 93, Product 90). Every change below
was verified on the rig, almost all **against stock NTVDM** with a deterministic probe;
A/B regression gates against the morning build `d0699606` (Skyroads, Doom, ZAR, the
16-program shelf) were run after each batch (`runs/s91_gate/`).

## Closed

| # | What | Verified by |
|---|---|---|
| #273 | VESA real-mode WinFuncPtr (`src/vdd/vbe_rm.asm`, B260:00C0); PMID decided against | `p_vesawf` 10/10 = the Tseng ET4000/W32p ROM (pcem-vesa) |
| #294 | COMMDLG PrintDlg on comdlg32 (all 8 ids now) | `w_cdlg` 11/11 = stock |
| #308 | Subclassing: 16-bit windows re-pointed; system controls subclassed the WOW way | `w_subcl` 17/17 = stock |
| #309 | WOWCallback16(Ex), WOWGlobal*16 from 32-bit thunk DLLs | `w_wcb` 8/8 = stock, real thunk DLL `tools/wintest/thunk32/` |
| #8 | NetBIOS VDD (INT 5Ch / INT 2Ah), POST routines included | `p_netb` 14/14 = stock; `net_test` 21 off-VM |

## Done and verified, issues still open for their remainders

- **#305** M8 WM_SETFONT/GETFONT (`w_font` 7/7), M9 activation/move/show/menu-select/
  min-max (`w_msgs` 13/13 with M10), M10 WM_SIZE sent, M11 dialog keys (Esc closes the
  modal Task List), M13 MDI (`w_mdi` 13/13) — = stock. Left: M12 (WM_DROPFILES).
- **LineDDA** (`w_ldda` 24/24 = stock): the void callback no longer stops the walk; the
  end point is excluded; a zero-length line makes no call.
- **stdio**: console input from a file on handle 0 (`p_stdin` 7/7 = stock = MS-DOS 6.22
  under PCem); AH=0Ah drops a redirected line's leading LF.
- **#11 SDK**: Microsoft-ABI VDDs load unmodified — the third-party BOP (RegisterModule /
  UnRegisterModule / DispatchCall) plus NTVDM.EXE's VDD service API in the shim
  (`p_isv` + `tools/dostest/isvtest/` 10/10 identical under stock and ours). Our own
  `claim_int` now reaches any user vector through generic stubs (#315;
  `sdk/sample/intecho.c` + `p_sdkint`).
- **Launch matrix** rows 4 (DPMI client) and 5 (in-guest redirection) have stock halves;
  row 5 needed two fixes: XP's COMMAND.COM run as the program gets DOS 5.00, and its
  direct JFT edits (`>`) are honoured at EXEC.
- **TASKMAN**: GetWindow gives foreign top-level windows Win16 aliases — the Task List
  lists the desktop like stock. **RECORDER**: CreateWindow's own hMenu applied — its menu
  bar is back. **#283**: every modal dialog shown after WM_INITDIALOG.
- **DOS**: RMDIR closes the guest's unfinished searches and retries; 6.22's COMMAND.COM
  runs a script from redirected input (`tools/dostest/cmd622/`).
- **GDI GetTextMetrics** wrote its 31 bytes in a Win32-like order since s45 (Overhang at
  +16); the Windows 3.1 order puts the nine BYTE fields at +16..+24 — found by
  `tools/wintest/w_tm` vs stock (every size field agreed; pitch, charset, overhang did not).
- **#239 (ZAR's intermittent silence)**: diagnosed, not fixed — see below.

## Findings worth keeping

- **#239: in a silent ZAR run IRQ5 is raised but never DELIVERED.** The self-test reads
  "pending" in both kinds of run; in the silent one ZAR retries, resets the DSP and gives
  up. Its wait loop polls DOS time through DOS/4GW's 16-bit code, and our PM injector only
  interrupts 32-bit application code — delivery depends on hitting the brief 32-bit
  window. (A DOSBox-style "complete short DMA blocks at once" change was tried on a wrong
  first theory, gated clean, and REVERTED because it did not touch this path.)
- **XP's USER.EXE answers subclassing with `SCLS` thunks** (EDITWNDPROC 301 etc.:
  `push <itself>; call CallWindowProc`, `SCLS`+class index at +34h).
- **NTVDM calls VDD I/O handlers as STDCALL** (nt_vdd.h names no convention; NT and the
  DDK compile /Gz). A cdecl handler made stock die at the first IN.
- **GetProcAddress takes a name pointer below 0x10000 as an ORDINAL** — every V86 string.
- **`WOWGlobalFree16`/`UnlockFree16` return TRUE (1)**; **WOWCallback16Ex's pArgs is the
  16-bit stack image**.
- **Real MS-DOS blocks in AH=08h at the end of a redirected file** (PCem = stock).
- **XP's COMMAND.COM does `>` by editing the PSP's JFT**, not with AH=46h.
- **Stock's DPMI host is laxer than the spec** on error paths, and hangs at 0503h with a
  bad handle. **Stock refuses 6.22's COMMAND.COM.**
- **A hung stock run holds its redirect file open**: per-probe output files now.

## Final build and its checks

Rig `bin\` = **`e1317a5d`** + `bin\wowshim\` (shim API v3). On that exact binary: every
Win16 probe with a stock baseline agrees (w_cdlg 11, w_ctl 30, w_font 7, w_gdi 20, w_genum
28, w_gthunk 16, w_kernel 2, w_kfile 22, w_kmem 16, w_kprof 12, w_kstr 18, w_ldda 24, w_mdi
13, w_misc 44, w_msgs 13, w_props 19, w_sound 23, w_subcl 17, w_tm 70, w_user 23, w_wcb 8)
except w_cwd 2/3, which the morning build also fails (a file handle number, pre-existing);
shelf 15/16 as always; Notepad Find; launch-matrix row 5 and redirected stdin = stock.
The final regression run caught two of tonight's own defects before they shipped: an MDI
child's WM_GETMINMAXINFO reaching Win32 as a 16:16 pointer (crash) and the EXEC-time JFT
mapping touching handles above 4. Both fixed and re-verified.

## Tooling added

- `scripts/dospair.sh` (a DOS probe under stock and ours), `scripts/w16stockshot.sh`
  (photograph a Win16 program under both), `tools/wintest/thunk32/`, the MS-ABI test VDD
  `tools/dostest/isvtest/`.

## Open, investigated

- #314 Media Player visuals + Open at C:\, #283 Terminal's dialog/client paint,
  #316 the /P shell's prompt inside `prog > file` (predates s91), #207 MEM /D.
- Cardfile: NOT a defect after all -- its card is the same size and offset as stock's;
  only the (intended) Luna frame is taller.
