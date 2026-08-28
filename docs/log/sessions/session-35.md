# Session 35 — the stepped-over call is answering, and `LoadModule` names itself

**Date:** 2026-08-28 (evening) · **Branch:** `m9/completeness` · **Issue:** [#128](https://github.com/MrMatthewLayton/ntvdmex/issues/128)

Session 34 ended pointing at **WOW32 `0x2d` WowLoadModule** as the next piece of work.
This session did not implement it, and the reason is the finding: **`WowLoadModule` is not
where the run goes wrong — it is a consequence of a failure upstream, and that failure is
ours.** Both branches that lead there were decided by *our own stack litter*, not by
krnl386.

Sessions 33 and 34 were also found entirely uncommitted (1187 lines of `src/`, plus all of
session 33's oracle tooling). Both are committed now, verified building and green first.

---

## Part 1 — Two lies found in `nedis.py`, both paid for immediately

The session started by trying to read the `WowLoadModule` caller that session 34's handoff
located at `seg2:0x0f5f`. `nedis.py` printed **nothing at all** for segment 2.

⚠ **capstone's `disasm()` STOPS at the first byte it cannot decode and returns what it
had.** A start offset landing mid-instruction therefore produced *silence*, and silence
from a disassembler reads as "there is no code here" rather than "I gave up on byte one".
Segment 2 begins with the string `"Application Compatibility…"`, so `--wowfunc` output and
any window starting before real code was empty.

- **Fixed:** `_disasm_resync()` emits `db 0x??  ; '<c>' -- not decodable, resyncing` and
  steps one byte on, so the output always spans the window it was asked for.
- **And the fix needed a fix:** the first version reported the instruction straddling the
  end of the window as `db … not decodable` — a *window edge* masquerading as a data byte,
  the same lie one level down. capstone is now fed 15 bytes past the window and yields stop
  at `end`.

⚠ **`--wowfunc` scanned segment 1 only.** All the stubs live in seg1, but the code that
*calls* them does not. For `0x2d` the tool printed **`0 caller(s)`** — a tool reporting that
an ID is never used when it is the very call the run stops on. Now it scans every segment,
and immediately found **two** call sites, not the one the handoff knew about:

| site | what it is |
|---|---|
| `seg2:0x0f6d` | the real one, reached in the run |
| `seg2:0x0fab` | a **fallback** that loads `WINOLDAP.MOD` (the DOS-app "old app" module) |

Both defects are the project's standing failure mode — an instrument that lies — and both
were found only because the raw bytes were dumped by hand and disagreed with the tool.

## Part 2 — ★ The enclosing function is `LoadModule`, and it says so itself

`retf 8` at `seg2:0x0fd6` pins it: a **far function taking four words**, entered at
`seg2:0x051c` (`enter 0xb6,0 / push ds / push si / push di`). It carries a diagnostic
printer gated on `ds:[0x12b0] & ds:[0x46c]`, and the strings it prints **name it**:

```
ds:0x0ab9 = "LoadStart = "      ds:0x0ac6 = "LoadSuccess = "
ds:0x0ad5 = "LoadFail = "       ds:0x0ae1 = " Failure code is 00"
ds:0x0477 = "WINOLDAP.MOD"
```

That is Win16 **`LoadModule`**, and the signature falls straight out of what the printer
formats and what `les` reads:

| | |
|---|---|
| `lpModuleName` | `[bp+0xc]:[bp+0xa]` — the pointer printed after `LoadStart = ` |
| `lpParameterBlock` | `[bp+8]:[bp+6]` — `les si,[bp+6]` at `0x0f7e` |
| return | `AX >= 0x21` is a module handle; `AX < 0x21` is a Win16 error code |

★ **A latent instrument, not yet used:** the `LoadStart/LoadSuccess/LoadFail` printer is
switched off only because `ds:[0x12b0]` is zero (`ds:[0x46c]` is already `3`). Setting it
should make **krnl386 narrate its own module loads**, the same trick session 34 used on
`0xc4`.

## Part 3 — ★★ THE STEPPED-OVER CALL IS NOT INERT — IT ANSWERS, AT RANDOM

The harness steps over unimplemented WOW32 calls and logged that as *"registers untouched
— the call did NOT happen"*. That sentence is true about registers and **false about the
call's result**: the thunk does `sub sp,4` before the BOP and `pop ax / pop dx` after it, so
the guest reads a **stack hole nobody wrote** and branches on whatever was there.

The log now reads the hole back and prints it. One run settled both open questions:

```
FUNC=0xc6              guest will read 0x020001b7 from the return hole
FUNC=0x2d WowLoadModule guest will read 0x01d72714 from the return hole
```

- **`0xc6` → `AX = 0x01b7`, non-zero.** Its caller does `or ax,ax / jne <failure>`
  (`seg1:0x4795`). So the failure was **litter**, not a decision.
- **`0x2d` → `AX = 0x2714` = 10004, which is `>= 0x21`.** So `LoadModule` passes
  `cmp ax,0x21 / jb` and takes the **success** path into
  `les si,[bp+6] / mov es:[si+2],di` — with `lpParameterBlock` **NULL** — which is the
  terminal `#GP`. Session 34 *deduced* this and warned not to chase it; it is now
  **measured**.

⇒ **`WowLoadModule` is only called because `LoadModule` already failed with `AX = 0x17`.**
The path is exact and was read off the disassembly: `0x0f16 cmp ax,0x20 / jb` →
`0x0f26 cmp ax,0x17 / jne` → equal, so `0x0f28` **sets `lpModuleName` to `FFFF:FFFF`**
(which is why the log shows a call with no module name in it) → `0x0f53 sub sp,0x50` →
the call. So implementing `0x2d` first would have been building the handler for a
failure we are causing.

## Part 4 — What `0xc6` probably is (STRONG LEAD, NOT PROVEN)

`0xc6` takes **one word** and is called from `seg1:0x4792`, inside a far function at
`seg1:0x46f1` that takes one selector (`retf 2`) and picks between WOW32 `0x9c` and `0xc6`
on `cl` bits `0x10` and `0x1`. Evidence for what it is:

- The function **immediately after it**, `seg1:0x47bc`, is exported as **`GLOBALFREEALL`**
  (ordinal 26).
- `0x46f1` has **21 call sites** across all three segments — a low-level utility, not part
  of a load path.
- In the run it is passed `0x0016` and `0x025e`; `0x025e` is the selector of the segment
  krnl386 had **just read in** (LDT idx `0x4b`, base `0x2e1e0`, limit `0x31f`).

⇒ `0x46f1` looks like the **per-object global-memory free** that `GLOBALFREEALL` loops over,
making `0xc6` its 32-bit side. **Not confirmed** — and it matters which, because if it is a
free then it sits in `LoadModule`'s *cleanup*, and the real `0x17` is generated somewhere
earlier still. **Do not implement it on this reading alone.**

## Regression

- `./tools/dostest/run.sh` — 209 checks, 0 failed.
- `./scripts/check-imports.sh` — XP-safe.
- Rig run `build/wowruns/wow_host_213851.log` — reaches PM step `0xda` (34 ended at `0xd9`),
  `wow32{ok=0xb decl=0x17 unimpl=0x12}`, same terminal fault. No regression; the new line is
  instrumentation only.

⚠ **A near-miss worth recording:** the first build after the edit **failed**, and
`run.sh` + `check-imports.sh` both **passed anyway** — against the *previous* binary still
sitting in `build/`. The stale-artefact trap, live. `rm -f build/ntvdmhost.exe` before
rebuilding, and check the mtime of what you just tested.

---

## ▶ RESUME HERE — session 35 handoff

### Where it is

PM step **`0xda`**. Unchanged in behaviour from session 34 — this session bought
**understanding and instruments**, not a new wall. The terminal fault is fully explained.

### The next run — in this order

1. **Decide the step-over policy, because it is currently answering for us.** Every
   unimplemented `0x51` hands krnl386 a random branch. Options: return a defined sentinel,
   or *stop* on the first unimplemented call whose result is consumed. The current
   behaviour is the worst of both — non-deterministic and invisible until this session.
   Whatever is chosen, `unimpl=0x12` calls per run are all deciding things.
2. **Turn on krnl386's own load narration**: set `ds:[0x12b0]` non-zero (`ds:[0x46c]` is
   already `3`) and let it print `LoadStart/LoadSuccess/LoadFail = ` and its failure code.
   This is the cheapest possible way to find where `0x17` is really generated, and it is
   krnl386 self-reporting rather than us inferring.
3. **Only then** decide between implementing `0xc6` and `0x2d`.

### Ruled out / settled this session — do not re-open

| lead | verdict |
|---|---|
| "Next: implement WOW32 `0x2d` WowLoadModule" (session 34's plan) | **premature** — `0x2d` is reached only because `LoadModule` failed with `0x17`, and that failure is driven by our own stack litter |
| The null-`ES` fault is downstream of a stepped-over call (session 34 *deduced*) | **CONFIRMED by measurement** — the hole holds `0x2714`, which passes `cmp ax,0x21` |
| `nedis.py` printing nothing means there is no code there | **refuted** — capstone stops at the first bad byte; it was never a claim about the segment |
| `--wowfunc 0x2d` reporting `0 caller(s)` | **refuted** — it scanned seg1 only; there are **two** callers, both in seg2 |
| A stepped-over call is inert ("registers untouched") | **refuted** — it returns stack litter through the `sub sp,4` hole, and krnl386 branches on it |

### Standing hazards

- Everything session 34 listed still stands (the DPMI fault stack at linear `0x2000`,
  silent `wow_host_alloc()` failures, SFT entries not tied to `fh[]`).
- **`nedis.py` output starting mid-instruction is now visible but still wrong** — a `db`
  run at the top of a window means the start offset is misaligned, not that the code is data.
- Findings that depend on execution after *any* unimplemented BOP remain suspect; the log
  now prints exactly what was handed back, so check it before believing a branch.

### The loop

```bash
rm -f build/ntvdmhost.exe && ./scripts/build.sh   # ⚠ delete first -- see the near-miss above
ARCHIVE=build/wowruns ./scripts/bmwow.sh          # a WOW round (~90s); needs the sandbox off
./tools/dostest/run.sh                            # 209 checks, off-VM
./scripts/check-imports.sh build/ntvdmhost.exe
python3 tools/ne/nedis.py guest/ne/krnl386.exe --wowfunc 0xc6   # now scans every segment
python3 tools/ne/nedis.py guest/ne/krnl386.exe 2 0x051c 0x60    # LoadModule's entry
```
