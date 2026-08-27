# Project state — start here

> **This is the canonical resume point.** If you have never seen this project before, read
> this file top to bottom and you will know where it is, what works, what does not, and
> what to do next.

- **Last updated:** 2026-08-27 (session 31)
- **Branch:** `m9/completeness`
- **Tracker:** [140+ issues](https://github.com/MrMatthewLayton/ntvdmex/issues) — reconciled against the repo on 2026-08-26 (`tools/gh/backfill.py` is the manifest)
- **Knowledge base:** the [wiki](https://github.com/MrMatthewLayton/ntvdmex/wiki)
- **Day-by-day history:** [`docs/log/sessions/`](log/sessions/)

---

## What this is

**NTVDMEX is a replacement for `ntvdm.exe` on 32-bit Windows XP SP3.** It runs DOS
programs by executing 16-bit code on the **real CPU** in Virtual-8086 mode — reusing the
NT kernel's own VDM machinery through the undocumented `NtVdmControl` syscall — not by
emulating a CPU. It is not DOSBox and it is deliberately not a fork of anything.

**The goal:** install it so that every MS-DOS *and* Win16 launch routes to NTVDMEX and
stock `ntvdm` lies dormant. Not by overwriting `System32\ntvdm.exe` — that is Windows File
Protection territory — but by an Image File Execution Options `Debugger` value on
`ntvdm.exe`, which achieves the same routing and is reversible with one `reg delete`.

**The bar it was built against:** run real DOS games well — Doom, Skyroads, ZAR — with
flawless sound. **That bar is met** (Doom is fully playable, sound and mouse included).

### ★ The north star now: **run MS Paint and Notepad from Windows 3.x**

The DOS half works, so the goal moved to the Win16 half:

> **Run MS Paint (`PBRUSH.EXE`) and Notepad (`NOTEPAD.EXE`) from Windows 3.x under
> NTVDMEX.**

A good bar for the same reasons Doom was: small, iconic, and impossible to fake. Between
them they exercise the whole stack — NE loading, the KERNEL 16→32 boundary, USER windows
and menus, GDI drawing, mouse and keyboard. Paint in particular has to actually paint.

**Status: not started.** No Win16 program has run yet; `wowexec.exe` has never executed
and nothing has drawn a pixel. What works is the *bootstrap* — see #128 below.

---

## Where it actually is

### ✅ Working, and confirmed by hand on real hardware

| | Status |
|---|---|
| **Doom** | **Fully playable** — 3D rendering, status bar, menus, PCM + MIDI, keyboard **and mouse**. Runs its own 32-bit code through DOS/4GW on real silicon. |
| **Skyroads** | **Fully playable** — menus, Controls, level select, in-game. |
| **MS-DOS 6.22 `COMMAND.COM`** | Runs as a guest: prompt, line editing, internals (VER/VOL/CLS/ECHO/SET/TYPE/COPY/DIR/EXIT), and an external program EXEC'd and **returned from**. |
| **DOS API** | 103 INT 21h functions. XMS 3.0, EMS (LIM 4.0). |
| **Video** | Text (authentic IBM ROM font), mode 13h, mode 12h planar, VESA banked. Windowed GDI + exclusive-fullscreen DirectDraw, Luna-themed. All ten QuickBASIC demos render with **zero pixel defects**. |
| **Sound** | SB16 PCM at **99.999%** delivery, clean-room OPL2/OPL3 FM (MIT, Nuked used only as a black-box oracle), MPU-401 MIDI, PC speaker. |
| **DPMI** | 0.9 host running unmodified third-party clients; real-CPU protected mode; 32-bit DOS/4GW. |
| **Host UI** | Menu bar, status strip (program \| 16/32-bit \| Real/Protected mode \| capture state), six-tab Settings dialog backed by `HKCU\Software\NTVDMEX`. |

### ❌ Not working — and these are the honest blockers

| | Why it matters |
|---|---|
| **Win16 / WOW — bootstrap runs, no app yet** | `ntvdm.exe` is *also* the host for every 16-bit **Windows** program. The NE loader now loads, relocates and binds the **whole** XP WOW module set on real hardware — but nothing is executed yet, and there is no 16:16↔flat thunking. Since interception is an IFEO key on `ntvdm.exe`, and Win16 launches go through `ntvdm.exe` too, **installing NTVDMEX permanently would break every 16-bit Windows app today**. → [#128](https://github.com/MrMatthewLayton/ntvdmex/issues/128) |
| **Console/stdio integration** | DOS output is buffered and flushed to `CONOUT$` at exit, so shell redirection and piping are bypassed and every DOS program pops a window. Blocks non-interactive use. |
| **In-guest redirection** | `echo x > file` writes to the screen and leaves the file 0 bytes. Three fixes attempted, all at the wrong end. |
| **No INT 13h / INT 25h / 26h** | No direct disk access. |
| **No TSRs** | AH=31h is handled but residency is explicitly *not* honoured (and says so, rather than pretending). |
| **`MEM.EXE` reports wrong figures silently** | The "runs but lies" class — the most expensive kind. |
| **ZAR** | Needs VBE 2.0 hi-colour + linear framebuffer. |
| **40 of 46 settings** | Stored in the registry, honoured by nothing. `settings_apply()` in `src/host/main.c` is the honest list of what actually works. |

---

## Next actions, in order

> ⚠️ **The plan changed on 2026-08-26.** #129 was going to make "leave it installed"
> safe by handing Win16 launches back to stock ntvdm. **That is impossible** — measured
> three ways (see #129). Windows validates the VDM image's identity, so a renamed copy is
> refused outright, and the real name re-enters us through the IFEO hook. So there is no
> safe install story until WOW exists, and #128 moved onto the critical path.

1. **[#128] WOW / Win16 — IN PROGRESS. The loader half is DONE and the WOW32 half
   has STARTED: krnl386 is past its heap and doing real file I/O.**

   ### The bootstrap (session 30, unchanged and still true)
   On real hardware the **entire XP WOW module set loads, gets LDT selectors and
   binds**: krnl386 + system/keyboard/mouse/sound/comm drivers + gdi + user + shell
   + toolhelp + wowexec. Every import resolves; 27 descriptors installed and
   confirmed by `LAR` readback. Site counts match the off-VM battery to the digit
   (KERNEL 495, GDI 781, USER 1269, WOWEXEC 144). `src/wow/ne.h` + a 209-check
   battery over all 15 real binaries.
   ⚠️ **krnl386 is a LIBRARY, not a program** — no stack of its own, and its `CS:IP`
   is a DLL *init* entry. Bootstrap: init krnl386 → user + gdi → run **wowexec.exe**
   (the PROGRAM) → wowexec launches the app.
   ⚠️ **Load every module, assign every selector, then relocate ONCE.** Relocation is
   not idempotent. Entry indicator **`0xFE` is a CONSTANT**, and **ADDITIVE adds**.
   ⚠️ **Its init entry demands `AX == 0x4B4F`**, runs in **V86** (not PM), and turns
   itself into a 16-bit DPMI client. **LDT indices below `DPMI_LDT_RESERVED` are
   force-typed to data.** **A PM guest cannot reach the IVT**, so any `INT nn` absent
   from the patcher's list stays a raw `CD nn` and **silently terminates the VDM**.
   The `INT 2Fh 168A` vendor API is **REQUIRED**; our LDT is not user-mapped, so
   krnl386 gets a **descriptor-table shadow** reconciled on entry to any PM interrupt
   service. `04F2` = "commit CX descriptors from selector BX"; `04F1` = the private
   twin of `0000`.

   ### ★ The WOW32 half (session 31) — the interface is PINNED and 5 functions run
   The 16↔32 boundary lives in **exactly one module**: only krnl386 has these stubs,
   user/gdi/drivers funnel through KERNEL. `0x51` is the generic gateway and the
   whole interface is **82 integer function IDs**, now with **29 of them NAMED by
   krnl386's own export table** — no inference at all. See
   [`wow32-call-surface.md`](research/wow32-call-surface.md) for the frame diagram,
   the argument convention and the work list, and `src/wow/wow32.h` for the code.

   ⚠️ **Arguments are at `bp+16`, not `bp+12`.** Session 30's "VirtualAlloc's argument
   order is not pinned down, two readings possible" was an **instrument that lied** —
   the trace read four bytes low and printed the caller's far return address as the
   first two arguments. There was only ever one reading.
   ⚠️ **The return value is NOT a register.** The thunk does `sub sp,4` before the BOP
   and `pop ax / pop dx` after it. It must be written into that stack hole at
   `[bp-16]`. Getting this wrong is silent.
   ⚠️ **`SysVars+0x6A` was zero, and krnl386 WRITES through what it finds there.** Its
   init builds six far pointers into DOS's data area from a table named by that word;
   with SysVars zeroed those became offsets into `DOS_HDLR_SEG` — our own INT 21h BOP
   stub and DPMI entry points. `dos_wow_publish()` plants the table now, shaped like
   the one `lolprobe` measured off stock. **Clearing "error #2: Unable to initialize
   heap" needed this, not just the allocator.**
   ★ **Implemented:** `0xb8` VirtualAlloc (krnl386 services **DPMI 0501** with it),
   `0xb9` VirtualFree, `0xbc` GlobalMemoryStatus, `0xcf` GetSystemDefaultLangID,
   `0x78` (record the DOS data area).
   ★ **DECLINING IS A REAL ANSWER.** krnl386 hooks INT 21h in PM and chains to
   `cs:[0x3c]` — real DOS, i.e. **our own working layer** — when the 32-bit side
   returns `0xFFFF`. Seven file functions are declined and krnl386 now **opens a real
   file and gets handle 5 back**, which then appears as the argument to its
   subsequent get-date and close calls. ⚠️ Only where the call site says so:
   `tools/ne/wowdecline.py` finds three IDs where `0xFFFF` is a plain error.

   ### ★★ THE MEMORY MODEL (session 31) — krnl386 carves from `ES + 0x10`
   Its DPMI bring-up (`seg1:0xd688`) does `push es / int 2Fh 1687 / pop ax /
   add ax,0x10`, puts the DPMI host's private data there and grows **every** later
   allocation upward — **without a single INT 21h `AH=48h`**. So whatever sits above
   `ES + 0x10` is memory krnl386 believes is its own.
   ⚠️ Entered with `ES = DOS_PSP_SEG` it carved from `0x110`, where `dos_alloc` had
   already put **its own four code segments**. It now gets a real PSP block covering
   all remaining conventional memory, allocated last.
   ⚠️ **On the WOW path host memory comes from `wow_host_alloc()`, not `dos_alloc()`.**
   A `dos_alloc()` after `wow_place_v86` finds nothing, and the failure looks like the
   guest's fault — it cost two regressions in one sitting (the 168A vendor stub →
   "Inadequate DPMI Server"; the default PM handler table → `AH=35h` reporting vector
   0x21 as `0000:0000`).

   ### ★★ ERROR #3 CLEARED — krnl386 finds its own executable
   At `seg1:0xc257` it reads `PSP+0x2Ch` (the environment segment), scans past the
   strings to the **double NUL**, reads the count WORD and takes what follows as the
   program's full pathname — the MS-DOS 3.0+ convention, and **the only channel it
   uses**. `dos_psp_build` zeroes the first three bytes of the env block, which is
   right for a fresh PSP and destructive here; `wow_place_v86` rebuilds it with
   **krnl386's own path** (the `-a` argument).
   ★ Measured: `INT21h AH=3D open "C:\WINDOWS\SYSTEM32\KRNL386.EXE"`.
   **Two of krnl386's five errors are now cleared.**
   ★ `0xc9` = `GetCurrentDirectory` (INT 21h `AH=47h`; `AX=0x4717` at the BOP names
   it). One of the three that may **not** be declined. Unimplemented BOPs in a run:
   **zero**.

   ### ⏹ Where it stops today — the mechanism is now EXPLAINED
   krnl386 dies in its **NE segment-table copy loop** at `seg1:0xd4e5..0xd4f3`,
   whose trip count is `es:[0x1c]` = `ne_cseg`, read from an NE header it has just
   copied in. The header is not a header: with the debugger resolving selectors,
   `@ds:si` at that moment reads `c4 cf c4 c4 cf …` — **our own default PM handler
   stubs**. So `ne_cseg` is nonsense, the loop's `stosw`/`movsw` walk off the
   segment, and an unreflected PM #GP tears the VDM down silently — no VEH, no
   watchdog line, no last log entry.
   The whole route there is confirmed by breakpoint hits, not inference:
   `0x662f → 0x5cf8 → 0x5a42 → ret 8 → 0x7ed4 → 0x63b4 → 0x7f11 → 0x4648 →
   retf 6 → 0xd4c0 → 0xd4db`. The `pop es`/`pop ds` suspects were cleared the same
   way: they execute fine.
   ⇒ **The open question is now "what was supposed to fill that buffer", not "where
   does it die".** The buffer is `[0x5a0]`, a 64 KB selector krnl386 builds over **its
   own stack** at `seg1:0xc17e` — scratch, not a mapped image. `seg1:0x1812` is
   `OpenFile(name, &ofstruct, OF_EXIST)`, an existence probe, which is why the file is
   opened and closed without being read, and `0xd02b` builds a structure rather than
   reading. Something between `seg1:0xc181` and `seg1:0xc29f` is meant to fill it.
   krnl386 **is** using our `VirtualAlloc` block as its heap (`0007 setbase
   0x03a70000`, `0008 setlimit 0x8807f`). Our NE loader puts module images in HOST
   memory (`0x0295xxxx`) while krnl386 does its own loading — two loaders, two copies,
   and that is the thing to reconcile.

   ### ★ The PM INT 21h FILE API (session 31) — krnl386 opens SYSEDIT.EXE
   The real blocker was not in the WOW32 layer: five of krnl386's protected-mode DOS
   calls (`AH=34h/0Eh/DCh/43h/57h`) were landing in a **"PM thunk TODO"** arm and were
   never answered.
   ⚠️ **`dos_int21.c` resolves a guest pointer as `(DS << 4) + DX`** — right for V86,
   meaningless for a selector — so every pointer-taking function was excluded from the
   PM path. DOS/4GW never exposed this because it services its own DOS calls
   internally; **krnl386 is the first guest to chain them to us.** `pm_int21_xfer()`
   bridges it through a conventional-memory transfer buffer (the same shape as DPMI's
   own translation buffer). `AH=34h` returns a far pointer, so it now builds a real
   **selector** with `dpmi_seg_to_desc`.
   ⚠️ The transfer buffer is allocated **only on the WOW path** and every arm is gated
   on it, so a DOS or DOS/4GW run takes byte-identical paths to before.
   ★ **Measured:** `FUNC=0xc1 → DECLINED → INT21h AH=3D open
   "C:\WINDOWS\SYSTEM32\SYSEDIT.EXE" → AX=5`. krnl386 asks its 32-bit companion, is
   declined, chains to real DOS, and our thunk turns its protected-mode pointer into a
   filename DOS can open. "PM thunk TODO" is now **zero** for a whole run.

   ⚠️ **THE INT 21h TRACE IS OPT-IN (`dostrace.flag`).** "Zero `AH=3Fh` in the log" was
   filed as a finding earlier in the same session and meant nothing — the log did not
   print ordinary DOS calls at all. *An absent line in a log that does not print that
   line is not a measurement.*

   ### ⚠️ Instrument hazards that cost this session, all now fixed
   ⚠️ **A 2-byte BOP over a 1-byte instruction eats its neighbour.** Breakpoints on
   `c3`/`1f`/`c9` silently changed what the guest did — the `c3` at `seg1:0x662f`
   ate the first byte of the instruction at `0x6630`, which sets AX for krnl386's
   first INT 31h, so it asked for `0x0000` instead of `0x000A` and died at PM step
   1. `dpmi_bp_arm()` now measures the instruction with `x86len.h` and **REFUSES**.
   *This also refuted an earlier conclusion in this same session* — "the breakpoints
   never fired, therefore that code is never reached" — when the run had died forty
   entries earlier.
   ⚠️ **THE EXECUTING krnl386 IS AT LINEAR `0x1410` (segment `0141`), NOT at the
   base the bind stage logs.** Breakpoints armed at `0x02950000+off` report
   themselves ARMED with the right displaced bytes and never fire — a dead copy.
   `csbase=` is printed on every PM heartbeat now.
   ⚠️ **`target.txt` leaked between DOS and WOW runs.** `rt.bat` writes it for every
   DOS test; `wowrun.bat` never set its own, so **every** WOW run of session 31 was
   told to load `C:\test\selftest.com` — a DOS `.COM` — as its Win16 program.
   `wowrun.bat` now establishes its input. No measurement taken while that was true
   can be trusted.
   ⚠️ **The watchdog thread logs ONE sample per WOW run and then stops**, for reasons
   not yet found; it is not a usable instrument here. The `PMHB` heartbeat comes
   from the main loop, which is provably alive, and `DPMI-BP HIT` now resolves DS/ES
   and dumps `@ds:si` / `@es:di` — a debugger that makes you guess where a selector
   points is most of the way to being no debugger.
   ⚠️ Before consulting any other NTVDM project, read
   [`reference-projects.md`](reference-projects.md).
   Still unknown: **`INT 31h 04F3`**, and what four of the six `SysVars+0x6A` pointers
   mean (two are pinned: LASTDRIVE and the current-drive byte).

   ### Tools for this work
   `tools/ne/nedis.py` (16-bit disassembly with the WOW32 stubs named inline;
   `--wowfunc <id>` gives the stub, its callers and the argument-building code),
   `tools/ne/wowmap.py` (names the surface from the export table),
   `tools/ne/wowdecline.py` (which calls may be declined), `scripts/bmwow.sh` (drive a
   WOW run on the rig through controld, which the watcher path cannot do).

2. **[#131] Console/stdio integration.** Independent of WOW and needed regardless:
   anything script-driven behaves differently under NTVDMEX than under stock.
3. **[#130] Installation & routing.** Blocked on #128 — an installer is not useful while
   installing breaks every 16-bit Windows program.
4. **Known DOS defects** — #133 redirection, #134 the `$p` prompt, #47 MEM.EXE lying.

---

## Getting started on a machine that has never seen this

```bash
# Build (macOS/Linux cross-compile to XP-32; needs mingw-w64 i686)
./scripts/build.sh                 # -> build/ntvdmhost.exe

# Fast test loop -- no VM and no rig needed. Builds the batteries, then runs them.
./tools/dostest/run.sh                 # 18 batteries, 839 checks, ~10s, non-zero on failure
```

- The build is **no-CRT on purpose**: the toolchain is UCRT-default and UCRT is absent on
  XP, so a CRT-linked binary will not load there. `src/runtime.c` supplies the entry point
  and `mem*` primitives. Verify with `./scripts/check-imports.sh`.
- **`build/ntvdmhost.exe` is the host.** `build/ntvdmex.exe` is a small separate launcher.
  Deploying the wrong one has cost more than one session — checksum what you deploy.

For the bare-metal rig, the oracles, and how to run anything against real hardware, see
the wiki's testing pages. **Do not skip them**: the single most reliable way to waste a
day on this project is to measure stock `ntvdm` by accident and believe the result.

---

## How to read the rest of the docs

| Path | What it is |
|---|---|
| [`docs/log/sessions/`](log/sessions/) | The day-by-day archive, verbatim, refutations included. |
| [`docs/decisions/`](decisions/) | Architecture decision records. |
| [`docs/research/`](research/) | Raw findings — disassembly, kernel RE, oracle disagreements, measurement runs. |
| [`docs/research/wow32-call-surface.md`](research/wow32-call-surface.md) | **The 82 WOW32 functions krnl386 needs**, with argument sizes. The #128 work list. |
| [`docs/research/evidence/`](research/evidence/) | Screen captures that back specific claims. |
| [`docs/ROADMAP.md`](ROADMAP.md) | Milestones. |
| [`docs/GLOSSARY.md`](GLOSSARY.md) | VDM, VDD, DPMI, WOW, thunk, BOP, IFEO… |
| [`docs/risks.md`](risks.md) | Standing risks. |
| [`docs/reference-projects.md`](reference-projects.md) | **Read before consulting any other NTVDM project.** What we may and may not read, and why. |

---

## The one thing to internalise

This project has repeatedly been wrong in the same way, and it is always the same shape:
**an instrument that lies**. A counter whose layout implies a claim it cannot support. A
video metric that moved the wrong way when a bug was fixed. A test that silently ran
against stock `ntvdm`. A dialog checker that reported 47 problems of which 45 were its
own. A "before" and "after" run that analysed the same stale file.

The habits that actually work, learned the expensive way:

- **Build ground truth from something that is not us** — the game's own WAD, a real MS-DOS
  under QEMU, stock `ntvdm`, Nuked-OPL. Oracles vote on truth; NTVDMEX does not vote.
- **Read the guest binary.** Disassembling `DOOM.EXE` fixed in an hour what twenty runs of
  host instruments could not.
- **When a guest dies at an address, diff the bytes there against the file on disk.** That
  found a five-session bug that was ours all along.
- **A trace that prints the request but not the answer is half an instrument.**
- **Predict the number before the run.**
- A fix measured on one guest is a fix for none — re-run the other class (V86 vs DPMI).
