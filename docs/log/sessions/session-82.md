# Session 82 — an unattended night: Tier 2 finished, the Win16 Edit menu, stereo, chain-4

**Date:** 2026-09-27 23:00 → 2026-09-28 · **Branch:** `m9/completeness` (every commit pushed)
**Mode:** the user left me unattended overnight with the rig to myself (their decisions, asked
before they left: push as I go; follow #202's order; deploy and launch on the rig freely; issues
get a comment and stay open until confirmed by hand).
**Host build:** rig `bin\` = **`f211af49`** (= `a6aabd3`). `debug\prev\ntvdmhost_prev.exe` =
`0e5b10e7` (untouched). **Nothing tonight is user-confirmed.**

---

## What was done (all pushed; every issue commented and left open)

| # | Commit | What | Rig evidence |
|---|---|---|---|
| #203 | `26a6670` | Settings > General > **DOS prompt**: XP's own / another COMMAND.COM + Browse; HKCU `DosPrompt`, below `cfg\shell.txt`; logged with source | `shellpick.bat`: 5 cases (6.22 chosen → 6.22 banner; cmd.exe refused; missing file; shell.txt wins; default) |
| #168 | `cb41d4f` | `p_ver` graded against the version the host's own log says is configured (`subject_expect`); `p_file` gains 5701h stamp, 34h flag bytes, 46h, 5Ch, 6Ch | found and fixed: 5701h on a read-only handle; 6Ch exists → 50h; 6Ch sharing |
| #160 | `23a6b3c` | Win16 clipboard bridge through krnl386 (GlobalAlloc/Lock/Unlock); EDIT forwards WM_CUT/COPY/PASTE/CLEAR/UNDO + EM_*; **menus wait for their WM_INITMENUPOPUP** | `clip16.bat`: Notepad paste + copy round-trip, Calc paste/copy |
| #161 | `ce6a077` | **IsWindow had been answered by ArrangeIconicWindows since s53** (a case label inserted between IsWindow and its body) — Paint's WM_SIZE relayout never ran | `pbtree.bat`: canvas (128,2) 1100x604 = stock; toolbox click reaches pbTool |
| #183 | `20a6d4b` | REP/LOOP/JCXZ count CX not ECX; host sampling profiler; differential interpreter fuzzer; `mybench.com` | see below — the speed-ups were measured slower and reverted |
| #189 | `d08b739` | **Stereo output**: SB16 stereo and GUS pan; OPL/speaker centred; waveOut + WAV recorder 2-channel | Heaven7 L/R corr 0.958, Doom 0.932 (mono = 1.000); Skyroads n8=0 max 7 |
| #184 | `a65d989` | Leaving chain-4 scatters the linear aperture into the planes (and gathers back) | `p_vgamem` clean on every row; Wolf3D/Mario 70 fps, Doom 35 |

| #154 | `ed02762` | Host Edit menu in text mode: Mark (drag, inverted), Select All, Copy, Copy Whole Screen, Paste (typed as scancodes) | `textedit.bat`: paste `echo PASTE154 OK` + Enter, copy shows the output |
| #155 | `0ac96ca` | Record Audio toggle, Open Capture Folder; screenshot use-after-free fixed and 32bpp frames saved | `capture.bat`: 4.05 s stereo WAV + BMP |
| #162 | `d51201e` | IsDialogMessage no longer bounces the guest's own message back to its queue (Charmap's 4k-calls/ms spin and 99k WM_CLOSE flood); Win16 WM_CLOSE defaults | Charmap one WM_CLOSE; Calc/Notepad close; clip16 unchanged |
| #189 | `ac05b26` | SB Pro stereo: mixer 0Eh bit 1, both-channel time constant, 0x90/0x91 | audio_test (fails with the halving removed); Skyroads unchanged |
| #175 | `ff895b6` `a6aabd3` | 8254: counter 2's GATE as a trigger (modes 1/5), mode 2/3 rising edge reloads, modes 1/4/5 one-shot count law | `p_pit` section H: `m2.retrig.reload` MISMATCH → AGREE; disputed rows get recorded rationales; pit_test 82 |

**Win16 shelf A/B** (`scripts/w16shelf.sh`, tonight vs the rollback `0e5b10e7`): identical on all
16 programs — no launch/close regressions from the menu, IsDialogMessage and IsWindow changes.

New issue **#212** (P1): programs start with interrupts disabled (IF=0); every oracle says IF=1.

### ★ Why WinMine and Charmap still don't end on the X (#162)
Read off XP's own USER.EXE: `DefWindowProc` (USER.107, seg1:1d5e) forwards to WOW32 only the
messages on a sorted whitelist (seg1:38fe) and returns 0 having done nothing for the rest --
**WM_CLOSE is not on it**. So real WOW applies the default in WOW32's 32-bit window procedure
after the 16-bit one returns, which works because it SENDS WM_CLOSE synchronously; we POST it.
A structural gap for every message off that list; needs the nested-run design.

## Findings worth keeping

### ★ The Win16 Edit-menu defect was a MENU defect, not a clipboard one
Notepad's text box is a real Win32 EDIT, so paste into it needed only WM_PASTE forwarded. But
Copy still did nothing: the menu's modal loop runs inside the exec thread's own pump, where the
guest cannot run, so the posted WM_INITMENUPOPUP (where Notepad enables Cut/Copy/Delete from
EM_GETSEL) was handled only after the menu had closed. Copy was grey on every first open. Fix:
`WM_SYSCOMMAND SC_KEYMENU/SC_MOUSEMENU` posts WM_INITMENU + a WM_INITMENUPOPUP per popup + a
marker, and the menu opens when the guest's GetMessage reaches the marker. **Affects every
Win16 program's menus** — the most important thing to check by hand.

### ★ A case label inserted between another label and its body
`case WOWUSER_ISWINDOW:` was followed by s53's `case WOWUSER_ARRANGEICONICWINDOWS:` and its
body, so IsWindow returned ArrangeIconicWindows' 0 for five weeks. Paint's frame WM_SIZE is
`if (IsWindow(canvas)) { relayout }` (pbrush seg3:0x1138, read off the binary). The s45 fix
that made Paint pixel-identical to stock had been silently undone by s53.

### ★ Wolf3D's "~80% of a core" is its own vsync spin (#183)
The new profiler (`cfg\hostprof.flag`, `scripts/hostprof.py`) put 40% of exec-thread samples in
**ntdll**: every 3DAh read calls `host_time_us` = QueryPerformanceCounter (a syscall on XP) + a
64-bit divide. A faster interpreter only spins more cheaply, so `MODEYTL ins/ius` cannot show
it. On a CPU-bound workload (`mybench.com`, interleaved on the rig): baseline **43.2 ns/instr**;
an opcode jump table 45–47; fetch through a pointer 48. **Both were faster on the Mac (+42–50%)
with identical behaviour proven by `scripts/interpfuzz.sh`, and slower on the rig's i686 GCC
build.** Reverted. ⛔ An off-VM speed-up is not a rig speed-up.

### ⚠ Programs start with virtual IF=0 (#212)
`p_ifst.com`: FLAGS & 0200h at the first instruction, after INT 10h, after INT 21h —
msdos622/dosbox-x/pcem all 0200, we 0000. `mybench.com` (no STI anywhere) had 36 PIT raises,
1,926 attempts, 0 delivered, and 0040:006C stood still. Defaulting s11's `sti; jmp far` entry
trampoline did **not** fix it (first exit still VTIB EFLAGS=0x30002, VIP cleared or not).
Reverted to opt-in; the trampoline now saves/restores the DPMI callback slot 0/1 bytes it
borrows (0x60–0x65) — defaulting it without that would have broken slot 0.

### Smaller
- `REP` took its count from full ECX and zeroed ECX/ESI/EDI high halves — latent since s80
  carries 32-bit registers. `JCXZ`/`LOOP` the same. Six checks, each confirmed to fail before.
- `5Ch` lock/unlock: we really lock (LockFile); 6.22 without SHARE answers error 1. **Open
  question for the user** (left as MISMATCH, not abstained).
- Charmap's X: the real window receives WM_CLOSE ~99,000 times in 10 s — identical on the
  rollback build, so pre-existing (#162 comment). WinMine's window closes but the host lingers.
- `pit.ch1.counting` flickers (AGREE in one run, 0 in others); pre-existing, not chased.
- Harness traps hit: a control line needs CRLF for controld; the sandboxed and unsandboxed
  shells have DIFFERENT `$TMPDIR`s (cost two A/B runs that were really A/A); zsh does not
  word-split `$var` (`set -- $g`); `git stash`/`pop` bumps mtimes, which bmstage then calls a
  stale build.

## New tools
`scripts/bm/{shellpick,clip16,pbtree}.bat` · rigshot `clipset`/`clipget`/`wclick` ·
`cfg\hostprof.flag` + `scripts/hostprof.py` · `scripts/interpfuzz.sh` + `tools/dostest/interp_fuzz.c` ·
`tools/dostest/{mybench,iftick,p_ifst}.asm` · dosdiff `subject_expect` rules.

## Next
The user's by-hand checklist (`checks.txt` on the share; the conversation has it too), then
#202's order: #172 (deferred with notes — understand #212 first) and #201.
