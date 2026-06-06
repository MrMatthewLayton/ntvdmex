# Roadmap

Milestones are ordered by **risk and dependency**, not by feature glamour. Each milestone ends in
something runnable/observable. Win16 is intentionally late: it is built on the same V86 + DOS
foundation as everything before it.

## How each step is tracked

Every milestone/step moves through five stages:

> **Research → Spike → Impl → Test → Done**

- **Research** — recover the contract: disassemble XP `ntvdm` / `basesrv` / `ntoskrnl`, read
  ReactOS for logic & structures; findings land in [`research/`](research/).
- **Spike** — a minimal, throwaway proof in the experiment harness ([`tools/vdmhost/`](../tools/vdmhost/)):
  does it work *at all*? Driven and logged from the XP VM.
- **Impl** — the real, clean implementation promoted into the host (`src/`).
- **Test** — verified on the XP SP3 VM (the canonical bench, `scripts/xp-vm.sh`).
- **Done** — exit criterion met, committed, this file + [`STATE.md`](STATE.md) updated.

**Research and Spike are risk-scaled.** For undocumented territory (most V86/VDM work) they are
essential. For documented, low-risk work (e.g. parsing an MZ header) they compress toward
Research → Impl → Test — *don't spike what's already known.*

> Stage status: ⬜ not started · 🟡 in progress · ✅ done · `–` not applicable

**Honest caveat:** nearly everything proven so far lives in the **vdmhost spike**. The clean-host
**Impl** (`src/`, today just the Luna shell preview) is largely unstarted — hence the rows below
with ✅ Spike / ⬜ Impl. Promoting the proven DOS core from `vdmhost` into `src/` is itself a major
piece of M2.

---

## M0 — Feasibility ✅ DONE
Prove the premise before writing real code.
**Exit:** one real-mode instruction executed in V86 under our host, fault reflected to us. ✅

| Step | Res | Spike | Impl | Test | Done |
|------|:--:|:--:|:--:|:--:|:--:|
| Interception — our binary runs as the VDM host | ✅ | ✅ | ✅ | ✅ | ✅ |
| V86 keystone — `NtVdmControl` runs one real-mode instr, fault reflects to us | ✅ | ✅ | – | ✅ | ✅ |
| XP-targeted no-CRT build toolchain | ✅ | ✅ | ✅ | ✅ | ✅ |

- Interception **pivoted** from the WOW `cmdline` repoint to the **IFEO `Debugger`** on `ntvdm.exe`
  ([ADR-0007](decisions/0007-intercept-via-ifeo-debugger.md)) — the repoint was disproven (XP
  validates the host image), see [ADR-0002 superseded](decisions/0002-intercept-via-wow-registry.md).
- V86 keystone proven: `NtVdmControl(VdmStartExecution)` ran `mov ax,0xBEEF; mov [0x80],ax` on the
  real CPU; GP/BOP faults reflect back to us. **[ADR-0004](decisions/0004-reuse-kernel-vdm-ntvdmcontrol.md)
  is now Accepted.** Full contract: [research/ntvdmcontrol-and-v86.md](research/ntvdmcontrol-and-v86.md).

## M1 — Minimal V86 host ✅ DONE (proven as spike)
**Exit:** a real-mode program that does INT 21h AH=09h prints a string to our console.
✅ — "Hello, World" from a real `.COM` loaded off disk.

| Step | Res | Spike | Impl | Test | Done |
|------|:--:|:--:|:--:|:--:|:--:|
| Low-memory map + self-allocated VDM_TIB + `VdmInitialize` | ✅ | ✅ | ⬜ | ✅ | ✅ |
| Fetch the program from CSRSS (`GetNextVDMCommand`) | ✅ | ✅ | ⬜ | ✅ | ✅ |
| Enter V86, run a hand-written real-mode stub | ✅ | ✅ | ⬜ | ✅ | ✅ |
| Interrupt reflection (real-mode IVT) + BOP host-callback loop | ✅ | ✅ | ⬜ | ✅ | ✅ |
| INT 21h AH=09h prints to our console | ✅ | ✅ | ⬜ | ✅ | ✅ |

I/O-port (`IN`/`OUT`) trapping is **not** done yet (deferred to M3 device work); interrupt
reflection — the harder half — is. **Impl ⬜:** all of M1 still lives in the `vdmhost` spike.

## M2 — DOS kernel 🟡 IN PROGRESS
**Exit:** run a real-world DOS `.EXE` that does file + console I/O, transparently.

| Step | Res | Spike | Impl | Test | Done |
|------|:--:|:--:|:--:|:--:|:--:|
| **M2.1** Real DOS process setup (≥640KB map, PSP, IVT seed, `.COM` at `PSP:0x100`) | ✅ | ✅ | ⬜ | ✅ | ✅ |
| **M2.2** INT 21h service surface (console + Win32-backed file I/O + misc) | ✅ | ✅ | ⬜ | ✅ | ✅ |
| **M2.3** MZ (`.EXE`) loader (header, relocations, segment setup) | ✅ | ✅ | ⬜ | ✅ | ✅ |
| **M2.4** DOS memory management (MCB chain, AH=48/49/4A) | ✅ | ✅ | ⬜ | ✅ | ✅ |
| **M2.5** Process plumbing (PSP command tail, env block, errorlevel) | ⬜ | ⬜ | ⬜ | ⬜ | ⬜ |
| **M2.6** Promote the DOS core from `vdmhost` spike → clean `src/` host | ⬜ | – | ⬜ | ⬜ | ⬜ |

Per-step exit criteria:
- **M2.1** — a `.COM` launches with a valid PSP and full conventional memory; a program that reads
  its PSP command tail sees the right bytes. ✅ **met** (spike `testps.com`: printed `args=[ HELLO]`
  from `DS:0x80`, and `himem=Y` proving a write/read at `0x90000` no longer faults). *Caveat:* the
  command tail is a fixed placeholder — real args need `CmdLine`, which `GetNextVDMCommand` does not
  populate yet (deferred to M2.5; the recovery of the real command line is the open item).
- **M2.2** — a program that opens/reads/writes a file (handles 3C/3D/3E/3F/40/42) and prints works.
  ✅ **met** (spike `filewr.com`: created `C:\ntvdmex\FILEIO.TXT` on disk, wrote/closed/reopened/read
  it back, printed `read back: Hello from DOS file I/O!`). Implemented: AH=02/06?/09 console,
  40 write, 3C/3D/3E/3F/42 file I/O (→ Win32 `CreateFile`/`ReadFile`/`WriteFile`/`SetFilePointer`),
  30 version; CF returned via the pushed FLAGS on the V86 stack. The rest of the ~40-function surface
  (input 01/08/0A, FindFirst/Next, get/set-attr, FCB calls, …) is added on demand as programs need it.
- **M2.3** — a real MZ `.EXE` (not just flat `.COM`) loads and runs. ✅ **met** (spike `helloexe.exe`:
  MZ header parsed, load module placed at `PSP_SEG+0x10`, the one relocation fixed up `mov ax,<seg>`
  to the load segment, `CS:IP`/`SS:SP` taken from the header → printed `Hello from a real .EXE!`).
- **M2.4** — a program that allocates/frees DOS memory runs. ✅ **met:** the self-checking
  `memtest.com` ran through `vdmhost` in **V86 on the real CPU** → `MEMTEST PASS`, exit 0
  (AH=4A shrink / AH=48 alloc / AH=4A resize / AH=49 free / AH=48 oversized-fails). Off-VM battery
  green 33/33 ([`tools/dostest/`](../tools/dostest/)) and verified under dosbox-x; `merge-on-alloc`
  now implemented (test T9). Impl ⬜ = the M2.6 `src/` promotion.
- **M2.5** — exit codes propagate to the launching shell; args + environment are visible to the guest.
- **M2.6** — the clean host (not the spike) runs Hello World, gated by an import-allowlist check.

## M3 — Device model + video/input ⬜
- [ ] Pluggable **VDD** interface (the third-party hook point, requirement #13)
- [ ] Video: trap text/VGA memory + INT 10h, render into a **Luna-themed window**
- [ ] Keyboard + mouse (INT 16h / INT 33h) from host events
- [ ] I/O-port (`IN`/`OUT`) trapping carried over from M1
- **Exit:** a DOS app with text-mode UI runs in a themed window.

## M4 — Memory extensions ⬜
- [ ] XMS, EMS, and **DPMI** (protected-mode DOS extenders, e.g. DOS/4GW titles)
- **Exit:** a DPMI/DOS-extender game or tool runs.

## M5 — Win16 / WOW foundation ⬜
- [ ] WOW bootstrap (`wowexec` analog), 16-bit `krnl386`/`user`/`gdi` hosting
- [ ] NE loader, 16-bit module/segment management
- **Exit:** a trivial Win16 .EXE loads and reaches its message loop.

## M6 — Win16 thunking ⬜
- [ ] 16:16 ↔ flat pointer translation; generic/flat thunks
- [ ] USER/GDI 16-bit objects mapped to Win32 handles; message bridging
- **Exit:** a real Win16 GUI app runs and paints.

## M7 — Peripheral VDDs ⬜
- [ ] Sound, networking, serial/parallel, etc., as pluggable VDDs (host-backed)
- [ ] Bare-metal vs virtualized device strategy per [risks.md](risks.md)

## M8 — Polish & SDK ⬜
- [ ] Pluggable VDD/driver SDK + docs for third-party developers
- [ ] Luna theming pass, full-screen story, installer/registration tooling
