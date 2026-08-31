# Project state — start here

> **This is the canonical resume point.** If you have never seen this project before, read
> this file top to bottom and you will know where it is, what works, what does not, and
> what to do next.

- **Last updated:** 2026-08-31 (session 37)
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

**Status: a Win16 program EXECUTES.** `wowexec.exe` — the WOW shell — is found, loaded and
run, and nothing has drawn a pixel yet. What works is the *bootstrap*, and as of session 37 it runs
a long way: krnl386 loads three of its four segments, installs its interrupt handlers,
**takes and returns from its own DPMI exceptions**, and loads **all eight** of the 16-bit
system modules — `SYSTEM.DRV`, `KEYBOARD.DRV`, `MOUSE.DRV`, `VGA.DRV`, `SOUND.DRV`,
`COMM.DRV`, `USER.EXE` and `GDI.EXE` — completes its bootstrap, reads `[boot] WOWSHELL` out of
`SYSTEM.INI`, finds and opens `C:\WINDOWS\SYSTEM32\WOWEXEC.EXE`, loads it and **runs it** —
and then reports, in Windows' own words, *"WOWEXEC caused a General Protection Fault in
module KRNL386.EXE at 0001:229C."* See #128 below.

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
| **Win16 / WOW — a program runs, but nothing draws** | `ntvdm.exe` is *also* the host for every 16-bit **Windows** program. The NE loader loads, relocates and binds the **whole** XP WOW module set on real hardware, **krnl386 executes** — protected mode, its own segments, its interrupt handlers, its own DPMI exceptions, and all eight 16-bit system modules — and it then finds, loads and **runs `WOWEXEC.EXE`**, which dies in `LoadCursor(NULL, IDC_ARROW)` because our task list holds an entry stock's does not. So a Win16 program executes; none has drawn a pixel, and there is no 16:16↔flat thunking. Since interception is an IFEO key on `ntvdm.exe`, and Win16 launches go through `ntvdm.exe` too, **installing NTVDMEX permanently would break every 16-bit Windows app today**. → [#128](https://github.com/MrMatthewLayton/ntvdmex/issues/128) |
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

1. **[#128] WOW / Win16 — IN PROGRESS. ★ A WIN16 PROGRAM EXECUTES: `WOWEXEC.EXE` IS
   FOUND, LOADED AND RUN.**

   ### ▶ START HERE: [session 37](log/sessions/session-37.md#-resume-here--session-37-handoff)
   That block is the live handoff — where it is, the leads already **ruled out**
   (do not re-try them), the next run, the instruments, and the standing hazards.
   Everything below it is background.

   **Session 37 in one paragraph.** `GDI.EXE` was never rejected — we could not **open**
   it. `seg2:0x218a` has exactly two instructions that return `0x0B` over its whole
   `0x570` bytes, a breakpoint on each named `0x2242` (validation), and the first `0x40`-byte
   read there came back `AX = 6` — **ERROR_INVALID_HANDLE**, not six bytes — from file handle
   `2`. Handle 2 was never a handle: the open had failed and `2` is the DOS code the host
   returned for *every* open failure, so `-> AX=0x0002` in the trace meant two opposite
   things and the wall had been read the wrong way round off that one line. **`AL` in a DOS
   open is a bit field, not a number** — bits 0-2 access, bits 4-6 the SHARE.EXE sharing
   mode, bit 7 no-inherit — and the protected-mode arm compared the whole byte against 0 and
   1, so krnl386's `al = 0x80` fell through to "anything else" and asked Windows for
   `GENERIC_READ | GENERIC_WRITE`. USER.EXE imports GDI, so `GDI.EXE` was **already open and
   never closed**; a second open asking for WRITE against a `FILE_SHARE_READ` handle is
   `ERROR_SHARING_VIOLATION` (`gle=0x20`, measured), and GDI was the only module ever open
   twice at once. Access now comes from `AL & 7` on both arms, **we stopped enforcing a
   SHARE.EXE we do not emulate** (both arms share read *and* write, because bare DOS locks
   nothing), and a failed open reports the failure it actually suffered instead of "file not
   found". Two instruments had to be fixed first: `dpmi_bp_load()` read `char buf[1024]` and
   **silently dropped six of eleven breakpoints** behind a header comment, showing five
   confident arms and 42 hits while answering nothing; and the open trace printed AX without
   `AL`, `CF` or the Win32 error, while `AH=3E close` printed nothing at all. WOW32 `0x88
   GetDriveType` was also implemented — 26 stepped-over calls sweeping A: to Z:, whose only
   caller does `cmp al,2` — and it is a **closed gap, not a moved wall**.
   **Then WOW32 `0x080` turned out to be `GetPrivateProfileString`**, named not by inference
   but by its own arguments, which point at the DGROUP strings `"BOOT"`, `"WOWSHELL"`,
   `"WOWEXEC.EXE"` and `"SYSTEM.INI"`; thirty bytes later krnl386 hands that buffer to
   `LoadModule`. With it and `0x039 GetProfileInt` answered, krnl386 **completes its
   bootstrap and names the program it wants**. Three more walls then fell. `LoadModule`
   was failing on **one wrong byte** — `0x0e` in krnl386's per-drive flag table at DGROUP
   `0x2a2`, which routes every `AH=47h` for C: through a pre-handler that forces `CF` on
   every path — and that byte was written because our protected-mode `AH=44h` whitelist
   admitted `AL=00/06/07` and sent the rest to the TODO arm *"because everything else takes a
   DS:DX buffer"*: true of most of `AH=44h` and **false of exactly the three that classify a
   drive** (`08h` removable? `09h` remote? `0Eh` drive map?), which are register-only and
   which the V86 side answered with an `else { OKCF(); }` — *success*, with the caller's own
   registers as the answer. With them answered **`WOWEXEC.EXE` opens**, and WOW32 calls go
   237 → 2116. That run died in a **1884-iteration retry loop** (`seg2:0x2a08`: allocate, ask
   WOW32 `0x7d` whether the result is acceptable, on `0` allocate another — leaking two bytes
   of stack per iteration). `0x7d` is one of the 53 unnamed ids, so instead of guessing it a
   **knob** was built (`wow32ret.txt`, which changes an unimplemented answer for one run and
   logs every use as an experiment); with `0x7d → 1` the loop runs **once** and
   **`WOWEXEC.EXE` loads and executes**. It then takes a GP fault and krnl386 says so in
   Windows' own words — visible only because the string decoder, which had been gated on
   krnl386's own MessageBox id, now decodes *any* call's string arguments: the next message
   came through a different id from a different module. Two instruments were built on the
   way: **`pmchg.txt`** (which byte changed, and at which PM event — it named the drive-table
   write in one run) and **`wow32ret.txt`**.
   ~~⚠ The furthest point depends on an EXPERIMENT (`wow32ret.txt` must contain
   `7d 00000001`).~~ **CLOSED in session 38** — `0x7d` is a service now. Both its call sites
   are inside krnl386's task-database creator (`seg2:0x2984`, which allocates `0x320` bytes
   and stamps `"TD"` into them); the retry loop offers **aliases of the same memory**, so the
   question can only be about the selector's numeric value, and `seg2:0x2a22` uses the return
   **as** the selector — so the answer is to **echo the argument**. Measured: the one call is
   asked about `0x03b7` and the next LDT write gives that selector `limit=0x31f`.
   `wow32ret.txt` ships **empty** and `0 override(s)` is the correct deploy line.
   **What kills WOWEXEC is `LoadCursor(NULL, IDC_ARROW)`** — a NULL instance is the
   documented way to ask for a system cursor. USER passes it to `GetExpWinVer`, krnl386's
   `GetExePtr(0)` walks its task list and **matches krnl386's own bring-up record, whose
   instance handle is `0`**, then returns that record's module-handle field, `0xFFFF`, which
   USER loads into `ES` — `mov es,ax` at `seg1:0x229c`, `#GP` with `err=0xfffc`.
   **Session 38 resolved the fork, and the obvious branch is refuted.** `TDB+0x1c` has
   **exactly one writer in the whole of krnl386** — `seg2:0x2e02`, inside `InitTask`, from
   the module's own DGROUP — and the bring-up record never goes through `InitTask`. So its
   `hInstance` is zero on real Windows too and there is nothing there to fix; session 37's
   expected `0x001e` came from the stock pattern `hInst == SS` bar the low bits, and the
   bring-up record's `SS = 0x001f` is **our host's entry stack**, not a Win16 DGROUP.
   What is wrong is that the record is still **in the list**: krnl386 unlinks it itself at
   `seg1:0xcd36`, after `LoadModule` returns — and `LoadModule` never returns, because the
   new task runs to its fault inside it. ⇒ **an ordering defect, not a value defect.**
   Measured with two repeating breakpoints (`seg1:0x2225`, `seg1:0x9a16`): the list is
   `0x03b7 -> 0x01ef -> 0`, i.e. **WOWEXEC's task IS linked, at the head**, and `GetExePtr`
   skips it (`+0x1c = 0x03d7`) and matches the record behind it. The fault dump alone could
   not tell that from a one-element list, and an inference drawn from it was wrong.
   The task launch is also mapped end to end now — `seg1:0x97c2` parks the creating task's
   `SS:SP` in `DI:CX`, switches `SS:SP` to the new stack, calls WOW32 `0x74` (which carries
   `wExpWinVer = 0x030a`, read out of `ne_expver`), pops the Win16 entry frame and **`iret`s
   into the task at `seg1:0x9879`**.
   ★★ **And the next piece of work has a name: krnl386 HAS NO SCHEDULER — we are the
   scheduler.** All seven Win16 scheduling primitives — `Yield`, `OldYield`, `DirectedYield`,
   `WaitEvent`, `PostEvent`, `SetPriority`, `LockCurrentTask` — are **pure exported
   pass-throughs** to WOW32 with no 16-bit body and *no call or jump to them anywhere in the
   binary*. krnl386 keeps the state (the task list, each parked task's `SS:SP` at
   `TDB+0x02/+0x04`) and hands every decision to the 32-bit side, which on real WOW runs each
   task on its own thread and blocks it. We answer all seven with the harness sentinel, so a
   task that has been entered never gives control back. WOWEXEC's entry is the textbook Win16
   `__astart` and its `WaitEvent(0)` sits exactly between `InitTask` and `InitApp`.
   The common thunk (`seg1:0x2bb6`) pushes `[0x228]` before the BOP, parks the frame's
   `SS`/`BP` in `[0x6a4]`/`[0x6a6]`, and afterwards does `cmp ax,[0x228] / jne` into
   `seg1:0x98ab`. ⚠ **That is a RE-ENTRANCY GUARD, not a scheduling lever** — `0x98ab`'s
   *incoming* task is `AX`, the caller's own, so the sequence means *"someone else became
   current while I was in the 32-bit side; park them and put me back."* Writing `[0x228]`
   from the host does **not** yield; it parks the wrong stack in the wrong TDB and undoes
   itself. (This entry said the opposite for one commit — the correction is in the session
   log, with the instruction that settles it.) What survives is the **addressing**:
   `seg1:0x2bc9` makes the guest's `DS` krnl386's DGROUP at every WOW32 BOP, so `[0x228]` is
   readable for free — every call line now carries `task=0x....` and a run is a task timeline
   (`9 task=0 | 222 task=0x01ef | 27 task=0x03b7`, and never back).
   ★★★ **THE REAL LEVER IS THE EPILOGUE MODE.** The thunk does not have one return path, it
   has **38**: a mode word at `bp-0x18`, `push 0`ed at `seg1:0x2bc7`, popped at `seg1:0x2c0b`
   and dispatched through the table at `cs:0x2a36`. **krnl386 never sets it** (the only other
   access clears it), so 37 epilogues exist for the 32-bit side. **Mode 25 is the task
   switch-back**, pairing instruction-for-instruction with the launcher: `seg1:0x97be push
   [0x228] / push bp / mov di,ss / mov cx,sp` … `seg1:0x9827 mov ss,di / mov sp,cx / pop bp /
   pop [0x228]`. ★ **MEASURED**: returning `0x74` through mode 25 puts the creating task back
   on its own stack, current again — the `0001:229C` GP fault disappears — and with a non-zero
   launch result the boot task runs on to `seg1:0xcd0b` and `seg1:0xcd30`
   (`[boot] 386GRABBER`), **the read immediately before the unlink**, which no run had ever
   reached — and then **runs the unlink itself**: `seg1:0xcd36` unlink, `es:[0xfa]=0` (unsign
   the record), **`[0x228] = 0`**, `SS=DGROUP / SP=0x210`. **krnl386's boot task ENDS ITSELF**,
   and the `#GP` loop that follows is not a bug: `seg1:0x321f mov es,[0x228] / test es:[0x18],2`
   dereferences a **null selector** (`err=0`, exactly as logged) because nothing scheduled the
   next task. ⇒ the frontier is one fact: **`[0x228] == 0` means "no task is current; schedule
   one", and nothing does.** The remaining work is the **order** — return the creator first,
   resume the recorded `0x74` frame with mode 0 on that cue; the thunk frame *is* the context.
   New knob `wowmode.txt` (⚠ the most dangerous file in the tree — and a fault loop still
   makes a 268 MB log). See [`session-38.md`](log/sessions/session-38.md).

   **Session 36 in one paragraph.** The frontier moved from an address to a **module
   name**. Session 35's `WOW32_UNIMPL_RET = 0` — written but never run — turned out to be
   the largest single step of this epic: krnl386 goes from loading **one** system module
   to **six** (`SYSTEM.DRV`, `KEYBOARD.DRV`, `MOUSE.DRV`, `VGA.DRV`, `SOUND.DRV`, then
   `COMM.DRV`). Behind it were two walls, both ours and both *instruments* rather than
   mechanisms. First, **protected-mode `INT 15h` had no arm at all**: a COMM.DRV segment
   runs `mov ah,0C0h / int 15h`, every other BIOS vector krnl386 uses had a PM twin, and
   the raw `CD 15` was correctly identified as a raw INT, handed to a function with no arm
   for it, and fell out of the bottom as *"unexpected PM stop event=0x4"* — the run died
   two bytes into a driver, naming an address rather than a cause. It now answers exactly
   what the V86 arm answers, and `AH=C0h` is deliberately **refused** rather than stubbed,
   because the caller's next instructions read a **model byte** out of a table we would
   have to invent. Second, the WOW32 MessageBox decoder accepted only `0x20..0x7E`, so it
   printed the caption (identical for every module) and rejected the body —
   `"Please re-install the following module…\r\n\t\tCOMM.DRV"` — as "not a C string": **it
   named the class of failure and withheld the instance**. With both fixed, one repeating
   breakpoint at `seg1:0xcca4` reads the loader's return code per module, and `COMM.DRV`
   comes back **`AX = 0`** — not `2`/`4`/`0x0B`/`0x0F`, so "file not found / bad EXE / too
   many handles" are excluded by measurement. `COMM.DRV` is also the **only** one of the
   six that takes a `#NP` demand-load fault, and all ten of its segment-2 imports **exist**
   in krnl386/SYSTEM.DRV — checked, so that lead is closed before it was chased. A third
   defect cost a run on the way: **a one-shot breakpoint was the only kind that re-planted
   itself under a standing guest**, firing 512 times with byte-identical registers and then
   retiring before the pass it existed to observe. krnl386's own `/B` boot log was tried
   and **removed** — it self-disables silently, and so does the `[0x12b0]` poke.
   **Part 2 of the same session** narrowed it further: `LoadModule` (`seg2:0x051d`) returns
   **0**, which in Win16 is *"out of memory"*, not "not found" — and AX is already 0 at the
   earliest instrumented checkpoint, so the origin is upstream of `seg2:0x0e0b`. The
   relocation pass, the segment loads and COMM.DRV's own `LibMain` are all **closed by
   measurement**, and the `#NP` everyone would chase turns out to be krnl386 calling
   **`WEP`** — teardown, *after* the verdict. What is left is structural and visible in the
   files. **Part 3 closed it.** The bisect ran down through `LoadModule` — five stages, then
   an untested sixth, then the entry-point call — to `seg2:0x2da6 or ax,ax`, where AX is
   **the DLL entry point's return value**: `1` for all five that load, **`0` for COMM.DRV**,
   whose module handle was perfectly good. And COMM.DRV's own `LibMain` ends by returning
   **the word at `0040:0008`** — LPT1's base address in the BIOS data area. Nothing had ever
   written that table, **while our INT 11h equipment word (`0x4021`) declares one parallel
   port**: our own BIOS contradicting itself. Writing only what the equipment word already
   claims — LPT1 at `0x0378`, no serial ports, rather than inventing hardware nothing answers
   for — makes **COMM.DRV load, and `USER.EXE` behind it**. ⚠ One lead was **wrongly closed**
   on the way: "LibMain runs and takes this branch" was written up as "LibMain is not the
   cause", and it was the cause. *A measurement that something happens is not a measurement
   of what it returns.*

   **Session 35 in one paragraph.** No new wall — this one bought *understanding*, and
   corrected the plan. The harness logged an unimplemented WOW32 call as "registers
   untouched — the call did NOT happen", which is true of the registers and **false of the
   result**: the thunk does `sub sp,4` before the BOP and `pop ax / pop dx` after it, so a
   stepped-over call hands krnl386 a **stack hole nobody wrote**, and it branches on the
   litter. Printing that value settled two questions in a single run. `0xc6` handed back
   `0x01b7`, and its caller does `or ax,ax / jne <failure>` — so that failure was **ours**,
   not a decision. `0x2d` handed back `0x2714`, which is `>= 0x21`, so `LoadModule` took its
   **success** path into `les si,[bp+6] / mov es:[si+2],di` with a NULL parameter block —
   which *is* the terminal `#GP` session 34 deduced and warned against chasing, now measured.
   ⇒ **`WowLoadModule` is not the frontier**: it is only ever called because `LoadModule`
   already failed with `AX = 0x17` (the path is exact — `cmp ax,0x17` at `seg2:0x0f26`, and
   `0x0f28` then *overwrites* `lpModuleName` with `FFFF:FFFF`, which is why the call carries
   no module name), so implementing `0x2d` first would have been writing the handler for a
   failure we cause. The enclosing function meanwhile **names itself**: its own
   `"LoadStart = "` / `"LoadSuccess = "` / `"LoadFail = "` strings make `seg2:0x051c` Win16
   **`LoadModule`**, `retf 8`, `lpModuleName` at `[bp+0xc]:[bp+0xa]` and `lpParameterBlock`
   at `[bp+8]:[bp+6]` — and its narration is switched off only by a zero at `ds:[0x12b0]`,
   which is the cheapest way to find where `0x17` is really generated. Two lies were also
   fixed in `nedis.py`: capstone **stops dead at the first undecodable byte**, so a
   misaligned start produced a *silent empty window* (seg2 opens with a string), and
   `--wowfunc` scanned segment 1 only — it reported **`0 caller(s)`** for `0x2d`, the very
   call the run stops on, when there are **two**, the second a `WINOLDAP.MOD` fallback.

   **Session 34 in one paragraph.** DPMI exception delivery works, and nine walls behind
   it fell — every one of them ours. The first was never a missing frame: **NT builds the
   DPMI 0.9 16-bit exception frame itself** and leaves only the return `CS:IP` zero for the
   host to fill, measured against krnl386's deliberate `UD0` (an exception whose every
   field was known in advance) and confirmed independently by its own handler's writes to
   `[bp+8]`/`[bp+0xa]`. **The kernel's fault table is indexed by the x86 exception vector,
   not an NT "class"** — #UD arrives at 6 and #GP at `0x0d`, refuting session 19 and
   showing the 8-entry table could never reach a #GP at all. Then, in order: `INT 21h
   AH=52h` had no PM thunk (so `ES` was the null selector — that *was* the #GP);
   `SysVars+4`, the SFT chain head, was zero, and **a zero head is not an empty chain**, so
   krnl386 read the IVT as an SFT header and cycled forever (117 MB in one run); it counts
   file handles and **refuses 64** (it wants 100 or 127, both literals in its code), so the
   real table is 128; growing the SFT then starved the 256-vector PM handler table out of
   the host pool, silently, exactly as that function's own comment warned; WOW32 `0x98` is
   the file **seek** and was unimplemented, so every read after the first landed at the
   wrong file offset; `wowdecline.py` was **under-reporting** declinable sites because it
   only understood `je`, not the `jne` fall-through; **declining turned out to be a property
   of the CALL SITE, not the ID** — `0x97` has one site that chains to DOS and one that
   returns the failure to the app, and we were declining at both; and finally **a reserved
   LDT index is not a read-only one**: `INT 31h 04F2` discarded krnl386's re-base of
   selector `0x17`, so an image it staged there was read to a stale address while it walked
   the relocations at the new one. Finally, **a raw `INT nn` in protected mode is retired as
   a class**: krnl386 re-bases the initial CS over a block it fills *after* declaring it, so
   no commit-time scan can ever patch it — but a `#GP` whose error code has the IDT bit set
   IS that interrupt, and servicing it there (vector from the error code, confirmed against
   the `CD nn` bytes) turns the project's oldest silent VDM killer into an ordinary serviced
   call. PM step `0x63` → **`0xd9`**; "Missing 16-bit system module" cleared; SYSTEM.DRV
   loads. **Next: WOW32 `0x2d` WowLoadModule** — krnl386 handing a module to the 32-bit
   half, which is the 16→32 boundary itself rather than another one-line gap.

   ### Session 33 in one paragraph
   Stock ntvdm was used as an oracle for the first time *from the outside*: `tools/vdmdump` reads a live VDM's memory and its whole LDT
   (`ProcessLdtInformation` works on XP against another process), and
   `tools/ne/dumpscan.py` locates an NE's segments in the dump. That settled the
   layout — and then two hypotheses drawn from it were **tested and refuted**, which is
   how the real cause surfaced: `LoadSegment` never reads the file, it `rep movsd`s each
   segment in from a **staged image block** that is walked by *reclaiming* what has been
   consumed. Session 32 had set that reclaim's gap to zero on purpose (to stop it
   overwriting live code), so every segment was copied from offset 0 — the NE header.
   With the gap restored, segments 2 and 3 load, at the same heap offsets stock uses.
   The next wall, an "unimplemented native BOP", was a **swallowed `INT 21h`**: our
   patch map is keyed by linear address and krnl386 *copies* its patched code, so the
   `C4 C4` travels and the vector is lost — recovered now from the module's own file
   image. krnl386 then installs its INT 10h handler, registers a DPMI exception-6
   handler, and executes `0F FF` (UD0) **on purpose** to check it is reached. It waits
   there. **Next: DPMI exception reflection.**

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

   ### ★★ THE NE HEADER IS PLACED — and krnl386 reaches LoadSegment
   Nothing in krnl386's bring-up reads its own NE header from disk (`seg1:0x1812` is
   `OpenFile(..., OF_EXIST)`, the `0xd02b` chain is structure-building, and the
   handle→selector path at `seg1:0xcf9f` never runs — all measured). It parses whatever
   is at `[0x5a0]`, the selector it builds over `base(SS) + SP` at `seg1:0xc17e`.
   ★ **That base is FIXED** — `SS:SP` is `0x1f:0x0FFE` at `seg1:0xc0d6`, `0xc123` **and**
   `0xc164`, the entry SP unchanged. So the loader can place the header there, and does:
   the header + tables are copied **immediately above krnl386's stack**, in the SAME DOS
   block (allocated separately they come out one paragraph apart — DOS puts an MCB header
   between allocations), and it enters with `SP` at the very top rather than top-2.
   ⚠️ Copy from the **NE header**, not the start of the file: every table offset in an NE
   is relative to the header, so it must be at offset 0 of that selector.
   ⇒ `ne_cseg` reads **4**, the copy loop runs four times instead of 65536, and the run
   goes from PM step `0x31` to **`0x3a`** with **12** WOW32 calls instead of 9.

   ### ⏹ SUPERSEDED (sessions 33-34) — the LoadSegment wall, kept for the method
   > This section describes where the run stopped in session 32. It is **no longer where
   > it stops**: segments 1-3 load and krnl386 now fails much later, on the *other*
   > system modules. Kept because the reasoning below is how the arena was understood,
   > and because two of its conclusions were later refuted.
   krnl386 builds its module database entry, then calls `seg1:0x90d9` for segment 1 of
   itself. It returns 0, so krnl386 takes `mov al,1 / call 0x987a` → WOW32 `0x02`
   **ExitKernelThunk(1)** → `int3`: a deliberate, traceable exit rather than a silent
   teardown. `0x90d9`'s early checks pass (`ne_cseg` = 4; it indexes `ne_segtab` at
   `es:[0x22]` with **ten-byte** records, exactly what `0xd45a`'s copy loop builds), so
   read on from `seg1:0x911d`.
   ★ **The failure is krnl386's RELOCATION pass, traced to the instruction.** Every
   step of `seg1:0x90d9` LoadSegment succeeds — `ne_cseg` check, self-load test,
   `flags=0xc142` (loaded), `handle=0x0207`, `call 0x937e` returns the handle, the
   relocation count is read off the segment — and then `call 0x8cb6` (**apply
   relocations**) returns **0** and `0x92b5` jumps to the failure tail. Both are
   breakpoint hits. ⇒ **This IS "two loaders, two copies"**, one step later than it
   looked: our NE loader already relocated the image that is *executing*, and krnl386
   loads its own segments a second time and relocates that copy itself.
   ★ **The relocation records were never copied into memory.** An NE segment with
   `NE_SEG_RELOCS` is followed *in the file* by a count word and 8-byte records; a
   conventional loader applies and discards them, but krnl386 re-reads them out of the
   LOADED segment (`seg1:0x921b`, then `0x8d54`). With only `length` bytes copied it was
   decoding whatever followed as records and taking them for **imported** fixups.
   `wow_place_v86` copies them now, and the walk measurably changes branch — `seg1:0x8dc7`
   (INTERNALREF) instead of `0x8d6a` (imported), with `@ds:si` byte-for-byte the file's
   records.
   ⚠️ It does **not** yet clear the wall: `0x8dc7` is entered once rather than four times,
   so record 1's fixup still does not complete.
   ▸ Next: read `seg1:0x8e0e` onward — handle `0x0207`, `test al,1`, the patch via
   `0x8e3f`.

   ★ **The module database it builds is WELL-FORMED** — dumped at `seg1:0x9145`
   (`esbase=0x1fca0`): starts `"NE"`, `ne_cseg=4`, `ne_segtab=0x40`, and all four ten-byte
   records carry both the loaded bit and a real handle (`0207/020e/0216/021f`), with
   seg4's minalloc grown to `0x1ba2+0xe00` by krnl386 itself. So the header placement is
   doing its job, `0x9068` is **never called**, and this is NOT the "two loaders" problem
   it looked like. Every check inside LoadSegment passes as far as `seg1:0x9183`; read on
   from there.
   ▸ `seg1:0x1493` is the segment-load **notification** (the source of the two `BOP 0x56`
   calls); `seg1:0x22b2` is **GlobalAlloc**.
   ▸ **CX at entry is a kept-but-UNPROVEN hypothesis**: `seg1:0xc164` turns CX into a
   paragraph count for the `[0x5a0]` arena and it measured 0 from entry, so it is now
   handed the window size. It changed nothing observable.
   ▸ Also decoded: **BOP `0x56` is the per-call 16→32 gateway**, sub-function on the stack
   at `SS:SP`, `add sp,6` after it. At `seg1:0x14cc` its return is not tested — a
   notification, harmlessly stepped over.

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

   ### ▶ How to drive it
   `ARCHIVE=build/wowruns ./scripts/bmwow.sh` deploys and runs a WOW round on the rig
   (add `PMBP=1` to keep `pmbp.txt` armed). ⚠️ **SMB writes to `/private/tmp/xpshare`
   need the sandbox disabled.** `dostrace.flag` turns on the INT 21h trace — it is
   **opt-in**, so its absence is not evidence. **Breakpoint addresses are LINEAR =
   `csbase + offset`, and `csbase` moves whenever an allocation size changes** — derive
   it per run from a `cs:eip=0x0000000f:` line, and **always check the armed line's
   `displaced <bytes>` against `nedis.py`**. Full operational detail, including the
   immediate next breakpoints, is in
   [`session-31.md`](log/sessions/session-31.md#-resume-here--the-operational-detail-a-fresh-context-needs).

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
