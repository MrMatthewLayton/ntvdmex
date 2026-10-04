# Session 91 — 2026-10-04: the score towards 95% (unattended)

The user confirmed Sound Recorder by ear and asked for an unattended run toward **95%**,
easy issues first. Score: **90% → 94.3%** committed mid-run (see the end for the final
figure). Every change below was verified on the rig, almost all **against stock NTVDM**
with a deterministic probe; two regression gates (A/B against the morning build
`d0699606`) showed Skyroads, Doom, ZAR and the 16-program shelf unchanged.

## Closed

| # | What | Verified by |
|---|---|---|
| #273 | VESA real-mode WinFuncPtr (`src/vdd/vbe_rm.asm`, B260:00C0); PMID decided against | `p_vesawf` 10/10 = the Tseng ET4000/W32p ROM (pcem-vesa) |
| #294 | COMMDLG PrintDlg on comdlg32 (all 8 ids now) | `w_cdlg` 11/11 = stock |
| #308 | Subclassing: 16-bit windows re-pointed; system controls subclassed the WOW way | `w_subcl` 17/17 = stock |
| #309 | WOWCallback16(Ex), WOWGlobal*16 from 32-bit thunk DLLs | `w_wcb` 8/8 = stock, real thunk DLL `tools/wintest/thunk32/` |
| #8 | NetBIOS VDD (INT 5Ch / INT 2Ah), POST routines included | `p_netb` 14/14 = stock; `net_test` 21 off-VM |

Also done and verified, issues still open for their remainders: #305 M8 WM_SETFONT/GETFONT
(`w_font` 7/7) and M13 MDI messages (`w_mdi` 13/13); stdio input from a redirected handle 0
(`p_stdin` 7/7 = stock = MS-DOS 6.22 under PCem); TASKMAN's Task List (foreign windows get
Win16 aliases, as WOW gives them); every modal dialog shown after WM_INITDIALOG (#283).

## Findings worth keeping

- **XP's USER.EXE answers subclassing with `SCLS` thunks.** EDITWNDPROC (301), BUTTONWNDPROC
  (303), STATICWNDPROC (302), SBWNDPROC (304), LBOXCTLWNDPROC (307), the combo's (344),
  MDICLIENTWNDPROC (444): each is `push <itself>; call CallWindowProc`, tagged `SCLS` +
  class index at +34h. That address is GetWindowLong(GWL_WNDPROC) for a system control.
- **`WOWGlobalFree16` / `WOWGlobalUnlockFree16` return TRUE (1)**, not GlobalFree's 0.
- **WOWCallback16Ex's pArgs is the 16-bit stack image** (lowest word = the last PASCAL arg).
- **Real MS-DOS BLOCKS in AH=08h at the end of a redirected file** (PCem and stock agree;
  DOSBox-X answers 0Ah). That is why `cat.com` "hung" under both hosts in s57 — correct.
- **`claim_int` only reaches a device whose vector has a BIOS stub.** 2Ah/5Ch were added;
  the stub area (DOS_CTAB_SEG:0300h) has 2 slots left (#315).
- **GetProcAddress takes a name pointer below 0x10000 as an ORDINAL** — every V86 string
  is below that. Copy guest strings out before passing them to a Win32 API.
- **Stock refuses 6.22's COMMAND.COM** ("Incorrect DOS version"); ours runs it, now also
  from a script on redirected input (`tools/dostest/cmd622/`).
- **Stock's no-wait NetBIOS name registration takes seconds** (still pending after 2 s);
  a probe must wait ~8 s before comparing.

## Tooling added

- `scripts/dospair.sh <probe>` — one DOS probe under stock and ours, rows side by side.
- `scripts/w16stockshot.sh <folder> <EXE> <name>` — photograph a Win16 program under stock
  and under ours (`runs/stockshot/`).
- `tools/wintest/thunk32/` — a real 32-bit thunk DLL for WOW32 callback tests.
- `scripts/bm/dosstock.bat` / `dosours.bat` windows lengthened to 30 s.

## Open, investigated

- #314 Media Player: client not erased, SScrollbar drawn vertical, Open dialog at C:\.
- #283 Terminal: the dialog now lands where stock's does but paints only its list; the
  main client is not erased; function-key bars show.
- Cardfile: the card is laid out taller than stock's, with an extra header rule.
