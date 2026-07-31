# DPMI under `NtVdmControl` — feasibility & scoping (M4 slice 3)

> Status: **Research.** XMS + EMS (the real-mode two-thirds of M4) are done and host-wired
> (`dos_xms.h`, `dos_ems.h`). DPMI is the protected-mode third. This note establishes that it
> is feasible by *reusing the kernel VDM monitor's protected-mode support* (consistent with
> [ADR-0004](../decisions/0004-reuse-kernel-vdm-ntvdmcontrol.md)), records the recovered
> mechanism, and recommends DPMI be its own focused spike rather than a same-session impl.

## What DPMI is, and why it is hard here

DPMI (DOS Protected Mode Interface) is how a DOS program escapes the 1 MB real-mode wall by
running in **protected mode** while still calling DOS/BIOS (which are real-mode) underneath.
DOS extenders — DOS/4GW (the Watcom/DOS games staple), CauseWay, PMODE/W, Borland's DPMI16 —
are DPMI *clients*. Lifecycle of a client:

1. **Detect**: `INT 2Fh AX=1687h` → returns DPMI present (AX=0), the processor type, flags
   (bit 0 = 32-bit capable), the size of the host-private data block (SI paragraphs), and a
   real-mode **mode-switch entry point** in `ES:DI`.
2. **Switch**: allocate the private block, point `ES` at it, then **FAR-CALL `ES:DI`** with
   `AX` = 0 (16-bit client) or 1 (32-bit client). The CPU enters protected mode and returns
   with `CS/DS/SS/ES` now holding **protected-mode selectors** (LDT descriptors).
3. **Run**: the client is now in PM and makes services via **`INT 31h`**: allocate/free LDT
   descriptors (0000/0001), set base/limit (0007/0008), allocate DOS memory (0100), allocate
   extended memory (0501), map physical (0800), set PM/RM interrupt vectors (0205/0201),
   **simulate a real-mode interrupt / call a real-mode proc** (0300/0301), allocate a
   **real-mode callback** (0303), etc.
4. **DOS/BIOS from PM**: the extender thunks INT 21h/etc. down to real mode via 0300.

**The fundamental challenge.** Our whole stack so far runs the guest in **Virtual-8086** mode
(a sub-mode of PM that *looks* like real mode). DPMI needs the CPU to actually execute 16- or
32-bit **protected-mode** code with a client-controlled LDT — that is a different machine state
than V86, and it requires switching the VDM between V86 and PM and back on every DOS thunk.
Software like our `v86interp.h` cannot help: this is about the CPU's actual mode, not opcode
emulation. Either the kernel monitor switches the VDM to PM for us, or we build our own LDT +
mode management with `NtSetLdtEntries` and native PM execution (far harder, fault-prone).

## Key finding: the kernel monitor already runs PM in the *same* VDM `[FACT, disasm]`

The recovered `VDMSERVICECLASS` enum (see
[ntvdmcontrol-and-v86.md](ntvdmcontrol-and-v86.md)) already contained the tell:

```
10 VdmSetLdtEntries · 11 VdmSetProcessLdtInfo · 13 VdmPMCliControl
```

Disassembling XP `ntvdm.exe` (`reverse/`, MS-copyrighted, never committed) confirms these are
the DPMI plumbing and that **one `NtVdmControl` VDM runs both V86 and protected mode**:

- **`fcn.0f00532e`** (the per-iteration interrupt-flag manager) does:
  `esi = [TEB+0xF18]` (the VDM_TIB — same offset we use) → `getMSW()` → `test al, 1`
  (the CR0 **PE bit**). The **`MSW`/PE test distinguishes "guest is in V86" from "guest is in
  protected mode."** When PE is set it pushes `0xD` (**service 13 = `VdmPMCliControl`**) with a
  small struct and calls `NtVdmControl` (site `0xf0053b6`) to virtualize the client's interrupt
  flag *in protected mode*. So the monitor maintains a PM CPU state for the VDM and reflects PM
  events back to the host exactly like V86 events.
- `getMSW`/`getIF`/`setIF`/`DispatchInterrupts` are all real exported functions in `ntvdm.exe`;
  the VDM_TIB CONTEXT lives at `+0x2D8` (matches our `VTIB_EAX..` block) and is the state the
  monitor loads/saves across both modes.
- `INT 2Fh AX=1687h` is handled (immediate `87 16` at `0xf02d39f`) — ntvdm's own DPMI host.
- `DpmiSetIncrementalAlloc` + the string `"DPMI: Failed to set selectors %lx"` confirm ntvdm
  installs LDT selectors for the client via the LDT services (10/11) and manages a PM heap.

**Conclusion:** the V86 keystone we already proved (`VdmInitialize` + a self-allocated VDM_TIB +
`VdmStartExecution` + trap-reflect) extends to protected mode through the *same* monitor — we add
LDT setup (services 10/11), PM interrupt-flag control (service 13), and a mode-switch that flips
the VDM_TIB CONTEXT into PM (set PE in the saved MSW/CR0, load PM CS:EIP/SS:ESP from LDT
selectors). This is squarely the "reuse the kernel VDM" bet of ADR-0004 — *not* a custom DPMI
host built on `NtSetLdtEntries` from scratch. That is the right path and a strong de-risking.

## Recon findings (2026-07-31) — mode-switch + LDT ABI recovered `[FACT, disasm]`

A pass over XP `ntvdm.exe` (r2 on `reverse/`, image base `0x0f000000`) scanning **every**
`call [NtVdmControl]` site (`ff 15 7c 14 00 0f`, 14 sites) and reading the pushed service number
+ ServiceData recovered the service map and, more importantly, the two keystone mechanisms.

**`NtVdmControl` service map (from the call-site scan):**

| Svc | Name (from enum)        | Site(s)                | ServiceData shape observed |
|-----|-------------------------|------------------------|----------------------------|
| 0   | VdmStartExecution       | `0xf0053d2` (+ main)   | `(0, NULL)` — runs the CONTEXT |
| 2   | VdmDelayInterrupt(?)    | `0xf005f6a`            | small struct |
| 3   | VdmInitialize           | `0xf00e6c2`            | the big init struct (ICA ptrs + TrapcHandler) — matches our `v86_init` |
| 4   | VdmFeatures(?)          | `0xf00fdd1`            | `&byte` flag |
| 5   | —                       | `0xf051111`            | — |
| 6   | —                       | `0xf045981`            | — |
| 8   | VdmQueueInterrupt(?)    | `0xf043ff8`            | `&word` |
| 10  | **VdmSetLdtEntries**    | `0xf05012b`            | **6-dword block, see below** |
| 11  | **VdmSetProcessLdtInfo**| `0xf05017a`            | struct built after a `rep movsb` (LDT table copy) |
| 12  | —                       | `0xf0415d3`            | words incl. 0x388/0x389 |
| 13  | **VdmPMCliControl**     | `0xf0053b6`,`0xf03d9d1`,`0xf03da59` | `{subfn}` where subfn ∈ {0,1,3} |

**Unknown #1 — the mode-switch primitive — RESOLVED.** `fcn.0f00532e` (the per-iteration
run/dispatch) is decisive: it loads `edi = 0x20000` (**EFLAGS.VM**, bit 17), calls `getMSW`, and
`test al,1` (the client's *virtual* PE bit). Two paths:
- **V86 path** (virtual PE == 0), at `0xf0053ca`: `or dword [esi+0x398], edi` — i.e.
  **`VTIB_EFLAGS(+0x398) |= 0x20000` (set VM)** — then `NtVdmControl(0, NULL)` = VdmStartExecution.
- **PM path** (virtual PE == 1): does **not** set VM; virtualises the client IF via
  `VdmPMCliControl(13, {3})`, then runs.

So **the VM bit in `VTIB_EFLAGS` (offset 0x398) is the sole V86-vs-PM selector for the *same*
VdmStartExecution.** To enter PM: **clear VM**, load `CS/SS/DS/ES` in the CONTEXT with **LDT
selectors**, set `EIP/ESP`, and call VdmStartExecution — the kernel monitor executes real
protected-mode code and reflects PM faults/INTs back like V86. (`getMSW` returns a *virtual* MSW
ntvdm maintains for a real DPMI client; we control the mode directly, so our host doesn't need it.)

**Unknown #2 — the LDT install ABI — RESOLVED.** `fcn.0f050100` builds the service-10 ServiceData
as **exactly the `NtSetLdtEntries` 6-dword argument block**:

```
struct VdmSetLdtEntries_Data {   /* NtVdmControl(10, &this) */
    DWORD Selector0;   /* +0x00 */   DWORD Entry0Low;  /* +0x04 */   DWORD Entry0Hi;  /* +0x08 */
    DWORD Selector1;   /* +0x0C */   DWORD Entry1Low;  /* +0x10 */   DWORD Entry1Hi;  /* +0x14 */
};
```

`Entry{0,1}{Low,Hi}` are the two dwords of a standard x86 LDT descriptor (base/limit/access);
two selectors can be set per call, or set `Selector1 = 0` for one. So to hand the client a PM
code/data/stack selector we build a normal descriptor and call `NtVdmControl(10, ...)`.
Service 11 (`VdmSetProcessLdtInfo`) copies an LDT *table* (`rep movsb`) — the one-time per-process
"here is the client's LDT base/limit" registration so the monitor loads LDTR.

**Unknown #3 — PM event taxonomy — deferred to the spike (resolve empirically).** The V86 taxonomy
is known (0=IO, 2=GP, 4=BOP, 6=IRQ; `VTIB_EVENT`=0x5A8). Whether a PM `INT 31h`/`INT 21h` surfaces
as a BOP (if we vector it through a PM IDT gate to a `C4 C4 nn`), a GP fault (event 2) we decode, or
a distinct PM event code is not cheap to pin statically — the spike's host loop will **log the raw
`VTIB_EVENT`/`VTIB_EVENT_INFO` and CS:EIP** on the first PM stop and read it off the VM. This is the
one unknown the spike is *designed* to answer.

## Spike results — increment 1 (2026-08-01, VM-run) `[FACT, real CPU]`

First VM run of the 16-bit switch spike (`tools/dostest/dpmitest.com` +
`src/vdm/dpmi.c`). The `ntvdmhost.log` on the real CPU shows the **switch plumbing
works end-to-end**:

```
BOP2F ax=0x00001687
DPMI switch @ 0x50:0x50 -> PM ok (VM cleared, CS=0x0000000f EIP=0x00000125)
```

i.e. INT 2Fh AX=1687h detected → client far-called the entry → we installed a code
LDT selector (CS became **0x000F** = our `DPMI_SEL(1)`), cleared VM, and rewrote
CS:IP to the PM return address (0x0F:0x125). `v86_set_ldt_entries` (service 10)
returned success. **So the mode-switch CONTEXT rewrite + LDT install are proven.**

**Blocker:** the host then **crashes (access violation) the instant it runs PM** —
the log ends right after `PM ok`, with no `STAGE3-DPMI: PM stop` line, so
`VdmStartExecution` faults *inside* the monitor rather than returning an event. The
PM fault is not being reflected to the host; it propagates as a real exception.
Setting FS/GS to the data selector (they were left holding the real-mode segment,
an invalid PM selector) did **not** fix it — necessary but not sufficient.

**Diagnosis / next increment:** the kernel monitor needs **PM-specific
initialisation before its first PM `VdmStartExecution`** that our V86-only bring-up
skips. Prime suspects, from the recon: **service 11 `VdmSetProcessLdtInfo`** (register
the process LDT base/limit so the monitor can load LDTR — without it the PM selectors
resolve against nothing) and/or the **service-13 `VdmPMCliControl`** PM-cli path that
`fcn.0f00532e` runs on the PM branch. Increment 2 must recover the service-11
ServiceData ABI (the `rep movsb` LDT-table copy at `0xf050169`) and call it (plus any
PM-cli init) before switching. Only then will PM code run and INT 31h reflect (which
finally answers unknown #3).

### VM run 2 (2026-08-01) — crash diagnosed via a vectored exception handler `[FACT, real CPU]`

Added a VEH (`dpmi_crash_veh` in `main.c`) that catches the PM fault, dumps it, and
exits cleanly (no WER). The captured fault is decisive:

```
DPMI switch @ 0x50:0x50 -> PM ok (VM cleared, CS=0x0000000f EIP=0x00000125)
DPMI FATAL: exception code=0xC000001E at 0x00000127
  host: EIP=0x00000127 CS=0x0000001b EAX=0 EBX=0 ESP=0x0000fffe
  guest PM: CS=0x0000000f EIP=0x00000125 SS=0x00000017 DS=0x00000017 EFL=0x00000202 event=0x04
```

- **`0xC000001E` = `STATUS_INVALID_LOCK_SEQUENCE`** (a `LOCK` prefix on an invalid
  instruction). The CPU was executing **garbage in the IVT at linear `0x127`**.
- **PM code genuinely executed**: our selectors loaded (CS=`0x0F` code, SS/DS=`0x17`
  data), VM clear (EFL `0x202`), and EIP advanced from the far-call return `0x125`
  to `0x127`.
- But it ran at **linear `0x127` = base 0 + offset `0x127`**, not
  `(retCS<<4) + 0x127`. So **our LDT code selector resolved with base 0** — the
  descriptor is installed (svc 10 succeeded) but **the monitor is not loading our LDT**.

**Conclusion:** service 10 alone is insufficient; the LDT must be *registered* with the
monitor (LDTR loaded) — i.e. **service 11 `VdmSetProcessLdtInfo` is required before the
switch**. That is increment 2's concrete task: recover its ServiceData/LDT-table ABI
(fcn.0f050140) and call it, then re-run. The VEH stays as permanent PM-fault
instrumentation.

### VM runs 3–5 (2026-08-01) — LDT registered + descriptor verified; monitor still won't load our LDT `[FACT, real CPU]`

Increment 2 added service 11 (`VdmSetProcessLdtInfo`) registration and per-run
diagnostics. Three tightly-scoped iterations:

- **Run 3:** `[svc11=0xC0000004 svc10=0x0]` — service 11 rejected with
  **`STATUS_INFO_LENGTH_MISMATCH`**. The ServiceData length dword must be the *total*
  `PROCESS_LDT_INFORMATION` byte size (`8 + count*8`), not the descriptor count.
- **Run 4:** fixed the length → **`svc11=0x0` (success)** — the kernel now accepts our
  LDT table. But the base-0 fault **persisted**.
- **Run 5:** logged the descriptor. `retcs=0x100 clo=0x1000ffff chi=0x0000fa00` decodes
  to **base=0x1000, limit=0xFFFF, access=0xFA** — a *correct* code descriptor
  (base = retCS<<4). Yet the guest still executes at **linear 0x127 (base 0)**.

**Definitive conclusion:** the descriptor is right, svc 10 + svc 11 both succeed, but
the monitor **does not load our LDT (LDTR) when it runs the PM context** — so selector
`0x0F` resolves base 0. Our raw "clear VM + `VdmStartExecution`" bypasses ntvdm's
**DPMI-mode enable**: the real switch (fcn.0f050a16 → `VdmMapFlat` to build `ExpLdt`,
then the switch trampoline `0xf044a58` with `&CONTEXT`, plus the service-13
`VdmPMCliControl` PM path in fcn.0f00532e) is what puts the VDM into protected mode so
the monitor loads the LDT and reflects PM faults. **Increment 3:** replicate that
enable sequence (map `ExpLdt` via `VdmMapFlat`, drive the switch trampoline / svc 13),
*then* PM executes at the right base and INT 31h reflects. Everything up to the
monitor's PM-mode flag is now proven correct.

### VM run 6 + the architecture correction (2026-08-01) — PM runs in USER MODE via `iretd`, not the kernel monitor `[FACT, disasm]`

Run 6 set the virtual-MSW PE bit (`word[TIB+0x668] |= 1`, what `getMSW` reads) before
the switch — **no change**, same base-0 fault. That proved the *kernel* monitor's
LDT-load isn't driven by the user MSW, and forced a closer read of how ntvdm actually
executes PM.

**The correction (decisive):** `fcn.0f00532e`'s PM branch does **not** call
`VdmStartExecution` (service 0) — the V86 branch does (`or [eflags],0x20000` then
`NtVdmControl(0,NULL)`), but the PM branch calls **`0xf04483c`**. And `0xf04483c` ends
in a bare **`iretd`** (plus a `popfd; pop es/ds/gs/fs; popal; lss esp; iretd` variant):
it builds an interrupt-return frame from the VDM_TIB register fields
(`+0x388`..`+0x3a0` = EAX..SS) on the guest's own stack and **returns into the client
directly, in ntvdm's own ring-3 context. There is no NtVdmControl call on the PM path.**

So the real architecture is:
- **V86** → run by the **kernel monitor** (`NtVdmControl VdmStartExecution`).
- **Protected mode** → run **directly in the host process via `iretd`** into the LDT
  selectors, using the *process's own LDT*. A PM fault / `INT 31h` surfaces as an
  ordinary **Win32 exception** that ntvdm catches (SEH/VEH) and services, then `iretd`s
  back. `svc 10/11` install the descriptors into the process LDT that this in-process
  execution uses; `getMSW`/the MSW is ntvdm's *own* bookkeeping, not a kernel switch.

**Why our spike faulted:** we fed a VM-clear context to `VdmStartExecution`, but the
kernel monitor doesn't run PM — it mis-ran our context (base 0). **All of our
descriptor + service-10/11 work is correct and reusable; only the execution mechanism
was wrong.**

**Increment 3 (redirected):** replace "clear VM + VdmStartExecution" with an
**in-process user-mode PM engine**: after installing the LDT selectors, save host
state, load the guest GP + segment registers, and `iretd` into `CS=0x0F:EIP` with
`SS/DS/ES=0x17`; catch the resulting `INT 31h`/fault as a Win32 exception in the VEH,
service it, and resume. This is real low-level asm (no-CRT) + an SEH-based PM service
loop — a substantial engine, and the correct path. The keystone risk (can we run PM at
all?) is now *lower*, not higher: it's plain user-mode execution, no undocumented
kernel PM path needed.

## Open unknowns (what the spike must nail down) `[VERIFY]`

1. **The mode-switch primitive.** Exactly how ntvdm flips the VDM from V86→PM after the client
   far-calls the 1687 entry: which fields of the VDM_TIB CONTEXT change, whether a dedicated
   `NtVdmControl` service performs the switch or it is implicit in `VdmStartExecution` once PE is
   set in the saved state, and how the monitor reports a PM fault/INT (the event taxonomy in PM).
2. **LDT descriptor install ABI** for `VdmSetLdtEntries` (service 10): the ServiceData struct
   (selector, two descriptor dwords, à la `NtSetLdtEntries`?) and whether descriptors are
   per-VDM-process (service 11 `VdmSetProcessLdtInfo` sets the table).
3. **PM event reflection**: when PM code does `INT 31h` / `INT 21h` / faults, what `VTIB_EVENT`
   value comes back and where CS:EIP/selectors are read (16- vs 32-bit operand size).
4. **32-bit vs 16-bit clients.** 16-bit DPMI (Borland, older) is the simpler first target;
   DOS/4GW is **32-bit** and needs 32-bit PM execution + a 32-bit flat data selector — likely a
   second step.
5. **Extended memory for `INT 31h 0501`.** Can reuse the XMS-style host-heap backing, but the PM
   client addresses it via a *selector* whose base we set with service 10 — so the lock/linear
   address path in `dos_xms.h` (currently the host pointer) becomes real here.

## Recommended plan — DPMI is its own spike, not a same-session impl

DPMI is the largest single risk in the whole project after the original V86 keystone. Treat it
with the same discipline that proved V86 (Research → **Spike** → Impl → Test):

- **Do NOT advertise DPMI yet.** Leave `INT 2Fh AX=1687h` unhandled (returns "no DPMI", AX
  unchanged/nonzero) until the switch actually works. Advertising it and then failing the
  far-call would crash extenders *worse* than absence (they fall back to "no DPMI → run real-mode
  or abort cleanly"). This is a deliberate gate, recorded here.
- **Spike target (M4.3-spike):** a hand-written 16-bit DPMI "hello": `2Fh 1687` (served by a real
  host entry) → far-call the entry → confirm we are in PM (`getMSW` PE set, CS is an LDT
  selector) → `INT 31h AX=0000` (allocate one descriptor) → `INT 31h AX=0300` to thunk `INT 21h
  AH=09` (print) back to our real-mode INT 21h surface → exit. If that round-trips on the real
  CPU, the monitor reuse is proven and the INT 31h surface can be built incrementally (testable
  off-VM for the bookkeeping parts — descriptor table, memory alloc/free — exactly like XMS/EMS).
- **Then 32-bit / DOS/4GW** as a follow-on once 16-bit PM round-trips.

### Why this is the honest call for this session

XMS and EMS were tractable (real-mode, pure bookkeeping + a memcpy bridge) and are delivered with
off-VM batteries (36/36 + 30/30) and host wiring. DPMI's core is a CPU-mode transition through an
undocumented kernel path; writing an INT 31h surface before the switch is proven would be building
on an unverified foundation. The research above turns "is this even possible under our
architecture?" into "yes, via monitor reuse — here is the mechanism and the spike to prove it,"
which is the valuable, low-regret deliverable now.

## References
- [ntvdmcontrol-and-v86.md](ntvdmcontrol-and-v86.md) — the `VDMSERVICECLASS` enum, VDM_TIB/CONTEXT
  offsets, the V86 keystone.
- [ntvdm-architecture.md](ntvdm-architecture.md) — overall ntvdm component map.
- [ADR-0004](../decisions/0004-reuse-kernel-vdm-ntvdmcontrol.md) — reuse the kernel VDM monitor.
- XP `ntvdm.exe` disasm (`reverse/`): `fcn.0f00532e` (PE test + `VdmPMCliControl`), `getMSW`
  `0xf0041b5`, `DispatchInterrupts` `0xf00442c`, `DpmiSetIncrementalAlloc` `0xf04fe94`, the
  `2Fh 1687` site `0xf02d39f`.
