# Session 72 — The by-hand pass; Doom's E1M1 crash is ours; QBasic's drive list

> Session 72. Split out of `docs/STATE.md` on 2026-09-23.

> Verbatim from `docs/STATE.md`. Nothing has been edited or corrected,
> including conclusions a later session refuted — same rule as the rest of
> this archive. See [`README.md`](README.md).

## ★★★★★ SESSION 72 (2026-09-15, afternoon) — **THE BY-HAND PASS: THE PACKAGE WORKS ON A FRESH FOLDER, AND DOOM'S E1M1 CRASH IS NOW REPRODUCIBLE WITH THREE SUSPECTS DEAD.**

**HEAD `9ad5eff`, pushed. Battery 1316/0. RIG = `70351a20…` = USER-CONFIRMED, and
it is now also `bm\ntvdmhost_prev.exe` (the rollback); `bm\ntvdmhost_lemok.exe`
keeps `bf9534a9` (f79d954, Lemmings). Package `dist/ntvdmex-20260915-9ad5eff.zip`.**

### ✅ USER-CONFIRMED BY HAND
* **THE 18th DELIVERABLE WORKS.** From a fresh Desktop folder: `status` → 2
     (another program owns the VDM), `install.bat`, `status` → 0, `smoke.bat` →
     **eight PASS lines and ALL TESTS PASSED**, `uninstall.bat`, the rig's own host
     back in charge. This had **never been run by a human** before today.
* QBasic: welcome box and "Untitled" banner now draw, **mouse clicks navigate the
     cursor**, Alt/File menus open by mouse, pointer is an inverted cell.
* Mouse capture: exclusive to the guest, **Win** releases it, clicking recaptures.
     (One bad capture on the first launch after a deploy, **not reproduced** — the
     known "a first run after a deploy is not evidence" pattern.)
* **Mario now PLAYS** (it used to die on the intro). ⚠ Cause UNKNOWN — the s72
     simInt guard did **not** do it (`simint_rm=0`, and Mario makes no DPMI calls).
     Graphics leave artefacts behind moving objects: a mode-Y planar issue, open.
* `DATE$`/`TIME$` correct in a guest — the INT 1Ah RTC half landing.

### ✅ BOTH BUGS THE PASS FOUND ARE NOW CLOSED (user-confirmed)
* **QB's Open dialog navigates by mouse, lists files, opens and runs programs.**
     `INT 21h AH=29h` stored `*` literally where DOS expands it into `?`s: QB parses
     the pattern into an FCB and matches every directory entry against it, so a literal
     star matched nothing -- empty Files pane, correct Dirs pane. Oracle-pinned, fixed
     in `fcb_put_name` (`0dbe737`). ⛔ I guessed the FCB SEARCH twice; QB enumerates
     with `AH=4Eh/4Fh`, which one trace line showed.
* **Dialog labels stopped losing letters.** `Files`->`iles` was QB's accelerator
     characters BLINKING: Blink Enable is attribute-controller register 0x10 bit 3 and
     we kept a private flag only the BIOS call could move (`48d7a67`).
* ★★ **The instrument that ended it: `cfg\textdump.flag`** -- the text screen as the
     GUEST wrote it, beside each screenshot. It showed the pane empty IN THE BUFFER,
     clearing the search and the renderer at once, after three wrong guesses from
     pixels. ▶ When something is missing from a text screen, dump the CELLS first.
* ⛔ **NEW, user-reported, not investigated:** making EXEs from QBasic (the
     BC.EXE/LINK.EXE path) does not work properly. Filed at the user's request.

### ⛔ THE OPEN BUG THE PASS FOUND
* **QB File > Open lists NO FILES** (Dirs/Drives is correct). QB parses `*.BAS`
     with AH=29h into an FCB and searches with **AH=11h/12h**, not AH=4Eh. ⚠ I saw
     the empty pane in my own headless screenshots this morning and explained it away
     as a Tab-order quirk — it is a real bug.
* ~~**Doom dies ~1s after killing the imp on the ledge / the far zombie** on E1M1~~
     — ✅ **SOLVED AND USER-CONFIRMED, s72 evening.** It was **our own INT-site
     patcher corrupting a jump table it mistook for code.** See the block below;
     everything under "in what is now known rather than guessed" is superseded as a
     *conclusion* but kept because the eliminations were all correct.

### The Doom crash, in what is now known rather than guessed
No `HOSTFAULT`, `veh{any=0 fatal=0}`, **no Application Error in XP's event log and
no Dr Watson log**, and **not one line of the shutdown path**. The process is
terminated outright — NT killing a VDM whose state it will not accept.
Eliminated **by measurement**, one user run each:
| suspect | how | result |
|---|---|---|
| sound / SB / IRQ5 | `cfg\nosb.flag` | crashed, **0** IRQ5 deliveries (was 521) |
| mouse + DPMI real-mode simulation | `cfg\nomouse.flag` | crashed, **0** `simInt 0x33` (was ~1389) |
| async injection racing the mode switch | `g_simint_busy` guard | **`simint_rm=0`** — never hit |

⛔⛔ **A METHOD TRAP, MINE:** three crashes ended on the same two `simInt 0x33`
lines and I built a theory on it. It was **coincidence** — `simInt` was simply the
most frequent line in the log. ▶ Before calling "it always ends on X" a
fingerprint, ask what share of all lines X is.
⛔ **AND I ASKED FOR A RUN THAT COULD NOT ANSWER:** the counter I needed was
printed only in the exit report, which a killed guest never writes. Counters that
bear on a crash now ride the PM heartbeat.
▶ ~~**NEXT:** the DPMI/PM IRQ0 arm~~ — **wrong suspect; see below.** The IRQ0 arm
still auto-EOIs and selector `0x317` (base `0x041A0000`, limit `0x19`) is still
**refused by NT twice and never installed in the LDT**; both remain open defects
on their own account, but neither was this crash.

### ✅ THE ANSWER (s72 evening, `6a2174a`) — A JUMP TABLE IS DATA, EVEN INSIDE A CODE OBJECT

**The instrument that ended it: `bm\vdmwatch.exe`** (`scripts/bm/vdmwatch.c`, built by
`scripts/build-vdmwatch.sh`) — a tiny attach-and-log debugger. Nothing we own runs
after the kernel gives up on a process, but `KiDispatchException` forwards every
exception to the **debug port first**, before the user-stack write that fails here.
It answers everything `DBG_EXCEPTION_NOT_HANDLED` (the no-debugger path) and logs the
code, address, register file, CS/SS descriptors, code+stack bytes and the exit code.

It caught the kill exactly: **`ACCESS_VIOLATION` whose fault address IS the EIP** —
`cs:eip=02bf:04c4c4fa`, an unmapped page — first chance, second chance, exit
`0xC0000005`, `veh{any=0}` to the end. `scripts/doomstack.py` then walked the core it
dumps: the guest `jmp`'d through a **near-pointer table at Doom obj1+`0x2cc6c`** and
landed on entry[1] = `0x04c4c4fa`.

**That value is `0x0416cdfa` with its middle two bytes overwritten by `C4 C4`** — the
BOP our INT-site patcher writes. Three of the table's five entries point into
obj1+`0x2cdXX`; little-endian that is `XX cd 16 04`, so the middle pair reads as
`CD 16` = INT 16h, a **serviced** vector, and `x86_int_site_is_real()` passes because a
table of code pointers decodes into plausible instruction streams. The guest writes the
table **after** the first scan, so the **second** scan corrupts it — DOS/4GW re-declares
its code selector on every file load (`AH=0009`/`000c`), re-running
`dpmi_patch_code_region` over the same range. `pmap_get` stops a site being patched
twice but **not a new candidate that only appeared once data was written.**

This is the **fourth** time this patcher has rewritten non-code — ZAR's call
displacement, `R_InitTextureMapping`'s `jle` displacement and the FP range 34h..3Fh are
the other three, all documented at the call site — and the first found by measurement
rather than a hunt.

**The fix:** a `cd nn` candidate lying inside an **aligned dword that points back into
the region being scanned** is a jump/call table entry, not two instructions — skip it.
Safe by the same argument as every arm beside it: a genuinely raw INT so aligned is
still serviced out of the `#GP`.

**★★★★★ USER-CONFIRMED: "Doom survived the crash!"** on build `a5cd764b`, which is now
the rig baseline (`bm\ntvdmhost_prev.exe` = the previous confirmed `c91b521e`).
Regression-clean: **ZAR** (the patcher's heaviest user) shows byte-identical patch
counts with the guard firing **0** times; **Skyroads** `max_ms=0x14`.

⚠ **Doom's repro is BY-HAND ONLY** — headless can't even load the WAD (the target path
is mangled to `GAMESDOOME`, the known path-specific DOS/4GW blocker), so it never
reaches the second scan.

⛔ **AND THE GUARD WAS SILENT IN THE RUN THAT PROVED IT** (`f036ba5`): its log line
shared the `rej++ < 16` cap, and that scan rejects **334** byte pairs before reaching
the table. What actually proved it was differencing two runs' counters —
`patched 3, rejected 0x14e` became `patched 0, rejected 0x151`, and `0x14e + 3 = 0x151`.
The guard now has its own counter, always printed in the scan line. *An absence in the
report means nothing unless the report says what it left out* — a fresh instance of
this project's most repeated lesson.

---

## ⏸⏸ SESSION 72 CLOSE (2026-09-15 evening) — **READ THIS BEFORE ANYTHING ELSE**

### ⚠ A BY-HAND PASS IS OWED AND THE DEADLINE IS THE 17th (i.e. TOMORROW)

**The package is built and headless-verified; it has not been in front of a human.**

* **`dist/ntvdmex-20260915-5a72811.zip`** — host `ffdea07a`, HEAD `5a72811`.
     Verified by `bm\pkgtest.bat` from a fresh folder: `/status` correctly saw another
     program's Debugger value → `/install` → **selftest 8/8 THROUGH the package host**
     → `/uninstall` → the rig's own routing restored. The previous package was
     **23 commits stale** — it predates the Doom fix AND the confirmed QB fixes.
* ⛔ **The rig still runs `a5cd764b`** (the user-confirmed Doom build), so **four
     host changes have never been seen by a human**:

     | change | commit | risk |
     |---|---|---|
     | **The DOS SFT** | `9ca0437` | **Moves conventional memory −7.4 KB for EVERY DOS guest.** Biggest risk. |
     | The HMA | `7bf65e8` | New guest-visible memory at `FFFF:0010`. |
     | `AH=3Dh` error codes | `b580c4d` | Programs branch on these. |
     | Jump-table guard logging | `f036ba5` | Cosmetic, rides along. |

* ▶ **Ask the user for three things (~10 min):** (1) **Doom E1M1, kill the distant
     imp** — the regression check that matters most, because the SFT and HMA both moved
     memory under the fix confirmed this afternoon; (2) **QBasic File > Open, navigate,
     run** — the memory map moved beneath it; (3) **one memory-tight guest** (Duke3D is
     the known one), which is where 7.4 KB would show.
* Rollback: ~~`bm\ntvdmhost_prev.exe` = `c91b521e`~~ **s73: `debug\prev\ntvdmhost_prev.exe`
     = `a5cd764b` (the confirmed build); `c91b521e` is `debug\prev\ntvdmhost_c91b521e.exe`.**

### ⚠ RIG STATE THAT MUST NOT BE LOST
`cfg\FLOPPY.IMG` is now on the share and **must stay** — without it `p_disk` goes
back to measuring an absent drive. Rebuild with `./scripts/mkfloppy.sh`. It backs
INT 13h/25h only; DOS file I/O on A: still goes through Win32.

### The three scores, all MEASURED (do not quote, re-run)
```
./scripts/paritysweep.sh   36 probes · 532 rows · PARITY 97.9% (459/469 comparable)
                              99.6% excluding the 8 rows with NO valid oracle
                              ⇒ only 2 gradeable rows remain open
./scripts/offvm.sh         30 tests · 1361 checks · 0 failed
tools/score/score.py       86.9%  (DOS 89.7 / WOW 86.3 / product 76.8)
```
⚠ The overall score did **not** move today, and that is correct: it scores attested
capability items, not probe rows. Nudging one because something looks done is how it
becomes a vanity metric — its own header says so.

### What closed today
1. ★★★★★ **Doom's E1M1 kill crash — USER-CONFIRMED** (`412624a`). Six sessions of
      silent VDM deaths were **our own INT-site patcher corrupting a jump table on a
      re-scan**. Found by attaching a debugger and reading the dead guest's core.
2. `AH=3Dh` answered "file not found" for **every** failure (`b580c4d`).
3. A DOS guest had **no SFT** — and `SysVars+4 = 0` is an SFT at *segment 0*, so a
      walker reads the IVT (`9ca0437`).
4. The **HMA** — NT had mapped it all along; we were refusing it (`7bf65e8`).
5. **INT 13h** — never unimplemented, it simply had no disk (`a662c58`).

### New tooling, all reusable
| tool | what |
|---|---|
| `bm\vdmwatch.exe` | Attach-and-log debugger: sees the exception the kernel refuses to deliver, dumps a core. **The thing that cracked Doom.** |
| `scripts/doomstack.py` | Walks a vdmwatch core against the LE object map. |
| `scripts/offvm.sh` | The whole off-VM battery in one command (was 30 hand-compiles). |
| `scripts/paritysweep.sh` | The whole oracle tier in one command. |
| `scripts/mkfloppy.sh` | Builds `cfg\FLOPPY.IMG` for INT 13h. |

### Still open
* ⛔ **QBasic cannot build EXEs** (BC/LINK) — user-reported, uninvestigated. Today
     **eliminated the obvious suspect**: `p_exec` and `p_ovl` are clean, so EXEC,
     load-without-execute and overlay relocation all match DOS. It is elsewhere.
* 2 gradeable parity rows: `p_tsr` paras-still-held; `xms.08` **BH** (undefined by
     the XMS spec — left red and undecided rather than matched by invention).
* 8 rows **blocked on PCem** (`p_lpt`, `p_plan12`, `p_vesapm`): SeaBIOS is a rewrite
     and there is no DOS ground truth for a VESA BIOS. **Do not fix toward them.**
* Older: Mario mode-Y artefacts, Heretic load screen, Hexen hi-res, Duke3D memory.

### Method lessons worth more than the fixes
* ⛔ **A one-host run is not a pass** — `p_drv` printed "no disputes" from an
     oracle-only run and was really 27 mismatches.
* ⛔ **Before calling a surface unimplemented, check it has something to work on**
     (INT 13h).
* ⛔ **An all-AGREE probe is not a verified surface** — several emit fewer functions
     than their headers claim, and many rows compare only `CF`.
* ⛔ **A guard's own log line must never share a budget with what it guards against**
     — the jump-table guard logged nothing in the very run that proved it works.
* ⛔ **Five false positives**, two within a commit of "fixing" correct code: a probe
     naming *"the default drive"* or **Z:** compares two **machines**, not two
     implementations. Fix it by giving the probe the **relation**, not just an abstention.
* ★ `ERROR_INVALID_ADDRESS` means *"occupied"*, not *"denied"* — **query before you
     allocate** (the HMA).

---

## ★★★ SESSION 72 (2026-09-15, morning) — **QBASIC'S "FILE SYSTEM PROBLEMS" WERE THE DRIVE LIST: A DRIVE WITH NO MEDIA MUST STILL BE SELECTABLE, AND CHDIR NEVER MOVES THE CURRENT DRIVE.**

**HEAD `a9b6c51`. Battery green (1302/0). Candidate `build/ntvdmhost.exe` = `86bb80b1…`,
verified headless on the rig and then TAKEN OFF AGAIN: the rig is at the mouse-confirmed
`7e9080bb…` (`c896300`), rollback `bm\ntvdmhost_prev.exe` = `bf9534a9…`. Deadline: the 17th.**

The user's round-5 verdict on QB.EXE was "lots of problems, mostly around the file
system". Driven headless (`scripts/bm/qbopen.bat`, `dostrace.flag`, screenshots), the
Open dialog listed the directory correctly and `[-C-]` alone under Dirs/Drives, on a
machine with A:, C:, D: and Z:. QB sizes that list by the classic probe -- for each
letter `0Eh`, `19h`, compare, `0Eh` back -- and our `0Eh` selected a drive only when
`SetCurrentDirectoryA("X:")` succeeded, which an empty floppy or CD-ROM drive refuses
with NOT READY. Asked the oracle first (`tools/dostest/p_drv.asm`, MS-DOS 6.22):

* `0Eh` selects any letter with a device behind it from the CDS, without touching the
     media -- the phantom B: on a one-floppy machine reads back through `19h`. Only a
     letter with no device (D: under LASTDRIVE=E, Z:) is refused, silently.
* `3Bh C:\ZZDRV` issued from A: leaves `19h` at A:, and `47h` for C: then answers
     `ZZDRV`: every drive keeps its own directory. Ours moved the process (and so the
     current drive) to C:. A bare `3Bh "C:"` is path-not-found (3).
* ⚠ `47h` or `3Bh` on the phantom drive PROMPTS "Insert diskette for drive B:" and
     hangs the oracle run. Two probe revisions learned that.

Fix (`src/dos/dos_int21.c`): `m->vdrive` holds a drive that exists but cannot be
entered; `dos_cur_drive()` answers `19h`/`47h`/`36h`/`1Bh`/FCB/IOCTL for it, and
`v86_path()` prefixes every relative path with it so an access fails ON that drive
the way DOS's would. `3Bh` on another drive only sets that drive's `=X:` variable
(what `"X:"` resolves through; `SetCurrentDirectory` does not maintain it, so
`C:` → `D:` → `C:` now also returns to the directory it left, not the root).

**Rig, headless, on the candidate:** `p_drv` selects A: / D: / Z:, refuses B:, keeps
the drive across a cross-drive chdir. QB.EXE: Dirs/Drives shows `[-A-] [-C-] [-D-]
[-Z-]`; `PERSONAL`↵ enters and relists; `BLIT.BAS`↵ opens (its `.MAK` miss is QB
looking for a make file -- normal, and the "CMAK" of the old log); `D:\`↵ on the
empty CD-ROM is error 3, Esc, clean exit; Save As writes the file. Skyroads on
baseline (`n8=0x66 max_ms=0x13`).

**▶ NEXT (needs the user):** deploy the candidate on their go (`build/ntvdmhost.exe`;
the confirmed build is already the rollback plan) and re-run the QB
pass by hand: File > Open shows four drives, a `.BAS` opens, Save As saves. Then
edit.com. ⚠ The Doom regression the user reported on the 14th is still deferred, and
its log is gone -- my QB runs delete `out\ntvdmhost.log`; copy a user's log to `runs/`
before queueing anything.

### ★★★★★ s72, afternoon — **THE PACKAGE SMOKE TEST WAS RUNNING THE STUB, NOT THE SELF-TEST: A DOS LAUNCH FROM cmd/BATCH NOW RUNS THE REAL PROGRAM.** (HEAD `11e4a13`, host `caff9e08…`.)

Running the fresh-folder package install on the rig (`scripts/bm/pkgtest.bat`,
what the friend's machine does on the 18th) exposed that `smoke.bat`'s
`selftest.com` reported "File I/O FAIL=21" -- on a host that had just opened the
file it was launched from. Two bugs, both on the path that has always mattered
for the 17th and was never exercised, because every rig run goes through
`dosstub.com` + `target.txt`:

1. **We ran the wrong program.** The first `GetNextVDMCommand`
      (`VDM_GET_FIRST_COMMAND`) fills only the console Title and CurDirectory.
      Explorer puts the program's path in the Title -- the sole reason a
      double-click has ever worked -- but a launch from `cmd.exe`, a batch file, or
      `smoke.bat` gives `title=[]` (via `start`) or the typed command WITH its
      arguments (direct). So we loaded the embedded 4-byte `mov ah,4Ch/int 21h` stub
      and reported a clean exit; the packaged smoke test "passed" having run nothing.
      Stock ntvdm consumes the real command in its exec-BOP path with a SECOND fetch,
      `VDM_FLAG_DOS` (`reverse/ntvdm.exe` 0xf04ed86 / 0xf00ac1e). We now do the same:
      AppName comes back as the program's full path, CmdLine as its tail, and that
      wins over the title heuristics and target.txt. The harness `dosstub.com` is
      recognised by name so target.txt still names the game there; a WOW launch
      (first fetch FALSE, err 0x57) is untouched.
2. **We never told CSRSS the task ended, so a second program in the same window
      never ran.** On exit we now report the errorlevel (`GetNextVDMCommand` with
      ExitCode) and `ExitVDM`. That report blocks for the console's next command
      (stock ntvdm's resident idle state), so it runs on a helper thread while the
      main thread ExitVDMs; and because that call is itself handed the queued
      follow-up command, we relaunch it (`CreateProcess`, same console) in a fresh
      host. Measured: `selftest.com` run twice in one `cmd` window now runs twice.

`selftest.asm`: the File I/O test named `C:\ntvdmex\ST$.TMP` -- the first rig
layout, on nobody's machine now -- made relative and deleted after.

**Rig, headless (candidate `caff9e08…`), all verified, then TAKEN BACK OFF:**
`selftest` 8/8 under a bare `selftest.com`, a redirected run, a `start /wait`, and
a second run in the same window; the fresh-folder package install (differently
named dir) → smoke 8/8 → uninstall restores → the rig's own host back; the
target.txt game harness still loads Skyroads (timing baseline `max_ms=0x13`).
`smoke.bat` now fails on a `FAIL=` line or a missing `ALL TESTS PASSED`, not only
on a crash. Package `dist/ntvdmex-20260915-11e4a13.zip` carries the fix. **Rig is
the mouse-confirmed `7e9080bb…`; candidate `build/ntvdmhost.exe` = `caff9e08…`
holds the QB drive fix + this, awaiting one by-hand deploy.**

---
