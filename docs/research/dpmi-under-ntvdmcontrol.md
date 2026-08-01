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

### VM runs 7–8 (2026-08-01) — IN-PROCESS PROTECTED-MODE EXECUTION PROVEN `[FACT, real CPU]` ✅

Implemented the corrected architecture: `dpmi_run_pm()` builds a full CONTEXT from the
VDM_TIB (CS=0x0F, SS/DS/ES/FS/GS=0x17, EIP/ESP/EFLAGS/GP regs) and calls **`NtContinue`**
— the documented syscall that loads a context (incl. LDT selectors) and resumes at
ring 3, i.e. ntvdm's manual `iretd` done via the OS. PM is run **in-process**, not by
the kernel monitor.

- **Run 7** (guest does `INT 31h`): the log ends cleanly at `-> running PM in-process
  (NtContinue)` — **no VEH fault, no "NtContinue returned" line**. So NtContinue
  neither rejected the context nor returned: it entered PM. The host then exited
  without a catchable fault (the `INT 31h` → Win32 exception dispatch onto a 16-bit PM
  CS is the open problem, not PM entry).
- **Run 8** (guest spins in PM instead of faulting): the **Luna window stays ALIVE** —
  `ntvdmhost` keeps running, spinning in `jmp .pmspin` **in protected mode**. This
  proves in-process PM execution: NtContinue entered PM with our LDT code selector
  (base 0x1000), the guest ran past the switch return (0x125) into the spin, and 16-bit
  protected-mode code is executing inside the host process on the real CPU.

**Milestone:** the DPMI real→protected-mode switch **round-trips** — real mode →
`INT 2Fh 1687` detect → far-call → mode switch → **protected-mode code executing**.
The whole descriptor/LDT/service/switch stack is validated end-to-end. The keystone
risk (can we run PM at all under our architecture?) is **retired: yes.**

**Remaining (increment 3b):** catch the PM `INT 31h`/faults to service DPMI calls. A
16-bit PM CS doesn't dispatch cleanly through the plain Win32 VEH path (run 7), so this
needs ntvdm's actual PM fault-reflection approach (or a stack/handler set up for it).
That answers the PM event taxonomy (unknown #3) and enables the `0000`/`0300` surface.

### VM run 9 (2026-08-01) — INT 31h does not surface via Win32 SEH; use the VDM TrapcHandler `[FACT, real CPU]`

Increment 3b attempt: made the data/stack selector 32-bit (D/B=1) so Windows could
deliver the `INT 31h` exception (run 7 had killed the process — a 16-bit stack can't
hold the exception frame), re-issued `INT 31h` in the client, and expected the VEH to
catch it.

Result: `ntvdmhost` runs at **100% CPU, spinning** (Task Manager) with no window and no
log — the guest **reached `.pmspin`, past the `INT 31h`**. So the VEH never fired and
the guest continued: **`INT 31h` from the in-process PM client does not raise a
catchable Win32 exception.** It is intercepted by the **VDM trap path** — the
`TrapcHandler` we registered in `VdmInitialize` (currently an empty stub) silently
swallows the fault and the kernel resumes the guest.

**Conclusion / increment 3b redirect:** PM faults + software INTs in a VDM are
delivered through the **`TrapcHandler`**, not SEH/VEH. To service `INT 31h` we must
implement that handler (recover its ABI: how the kernel invokes it, the fault/context
it passes, how to resume/skip the faulting instruction) — this is also how V86 BOP
events are ultimately sourced. Reverse ntvdm's registered trap handler next. The VEH
stays only as a last-chance crash logger.

### VM run 10 (2026-08-01) — plain Win32 SEH/VEH cannot catch PM faults; must use the monitor PM-entry `[FACT, real CPU]`

Followed run 9 with an `int3` (STATUS_BREAKPOINT — the easiest exception for the OS to
deliver) in the PM guest, to test whether *any* fault reaches our VEH under
`NtContinue`.

Result: the process **exited** (batch returned) but **no VEH log** appeared — the
breakpoint was not delivered to our vectored handler either. Cross-referencing:
- run 9 `int 0x31` → guest spun past (an IDT gate, no user exception);
- run 10 `int3` → process terminated, VEH never ran.

So once svc 10/11 make the LDT resolve correctly (runs 8–10), the guest executes on a
real 16-bit PM stack (SS=0x17) and **Windows cannot deliver exceptions to our user-mode
VEH in that context.** (In runs 1–6 the VEH *did* fire — but only because the LDT was
broken and the fault happened in a degenerate flat-ish state.) The VEH is therefore a
dead end for PM fault servicing.

**Definitive path for INT 31h (increment "3c"):** run PM through the **monitor
PM-entry** ntvdm uses — `0xf04483c`: mask ESP to 16-bit, **save the host register state
into the VDM_TIB host-save area** (offsets ~0xa0..0xd4) so the kernel can return to the
host, set the VDM-PM signal (`mov fs,0x3b` + the `[0x714]` flag), build an iret frame on
the guest stack from the VDM_TIB register fields, and `iretd`. Then the **kernel's VDM
trap handler** (registered via `VdmInitialize`) catches the PM fault/INT, saves the
guest state, restores the host, and returns with the event in `VTIB_EVENT` — exactly
like V86 BOPs. This is the intricate-but-correct mechanism; `NtContinue` runs PM but
bypasses this fault delivery. Also note ntvdm's ICA user-data field 9 is a code pointer
(`0xf044820`, a `retf`-to-guest resume trampoline), not the plain dword our `v86.c` sets
— a detail to revisit when wiring the monitor PM-entry.

**Status:** PM *execution* is proven (run 8, and re-confirmed each run: "PM ok" + the
guest reaches its PM code). INT 31h *servicing* is the open increment, now with a
precise, disasm-backed plan (replicate `0xf04483c`).

### VM runs 11–12 (2026-08-01) — the monitor PM-entry works; fault reflection still missing `[FACT, real CPU]`

Implemented `dpmi_enter_pm` (`src/vdm/dpmi_enter.S`), a faithful port of ntvdm's
`0xf04483c`: save host CONTEXT to `VDM_TIB+0x0C`, set `fs=0x3b` + the `[0x714]` flag,
load the guest register file, `lss` the guest stack, and far-jmp in. (Note: our
`ntvdmhost.exe` is based at **`0x0f000000`**, matching ntvdm — so host addresses like
`0x0f00e2af` are *our* code.)

- **Run 11:** faulted at `0x0f00e2af` = the `lss` in our own code, because after
  `pop ds` (guest DS=0x17, 64K limit) the `lss`/`jmp` read the `.data` globals via DS
  and overflowed the limit. **ntvdm uses a `cs:` override** on exactly those two
  instructions (`lss esp, cs:[..]`, `jmp cs:[..]`); we'd missed it. Fixed.
- **Run 12:** with the `cs:` override, the entry **succeeds** — control reaches
  `entering PM (monitor)`, the far-jmp runs the guest in PM. But the guest's `INT 31h`
  is still **not reflected** back: the process exits with no "PM event" line and no VEH
  fault. So the far-jmp/monitor-entry mechanics are correct, yet the **kernel does not
  deliver the PM fault as an event** to us (nor does the host-save CONTEXT get restored).

**Where the blocker now sits:** entering PM the ntvdm way is proven, but the kernel's
**VDM PM fault reflection** doesn't engage for our process. The likely cause is the
**VDM registration data** we pass to `VdmInitialize` being incomplete: ntvdm's ICA
user-data **field 9 is a code trampoline** (`0xf044820`, `retf`-to-guest) and its init
struct carries several code pointers, whereas our `v86.c` sets `p9 = &g_p9` (a plain
dword) and a minimal 2-pointer init. The kernel probably uses those to re-enter / reflect
the guest. **Next:** recover the exact `VdmInitialize` ServiceData + ICA layout from
ntvdm (the `0xf00e68a` struct) and provide correct code trampolines, so the kernel
reflects the PM `INT 31h` as a `VTIB_EVENT`.

### VM runs 13–14 (2026-08-01) — correct TrapcHandler; the execution-vs-reflection dichotomy `[FACT, real CPU]`

Recovered the real `VDM_INITIALIZE_DATA`: ntvdm calls `NtVdmControl(3, {0xf044820, &ICA})`
— so **`TrapcHandler = 0xf044820`** (a `retf`-to-guest trampoline: `and esp,0xffff; push
guestCS; push guestEIP; retf`), not the empty stub our `v86.c` had. (The ICA is 9 data
pointers, which we already matched.) Ported it as `dpmi_trapc` and installed it.

- **Run 13:** V86 still works (INT 21h markers + the 1687 switch run), so the new
  TrapcHandler is safe. But PM `INT 31h` is **still not reflected** via `dpmi_enter_pm`.
- **Run 14:** with the correct TrapcHandler, tried `VdmStartExecution` for PM (the V86
  mechanism). The VEH **fired** (fault reflected!) — but at `CS=0x1B EIP=0x127`, i.e.
  **base 0**: `VdmStartExecution` runs PM but does **not** load our LDT.

**The core dichotomy (now proven from both sides):**
| Mechanism | PM execution | Fault reflection |
|-----------|--------------|------------------|
| `dpmi_enter_pm` (our far-jmp, process LDT) | **correct** (base 0x1000) | **no** (process dies) |
| `VdmStartExecution` (kernel monitor) | **base 0** (wrong LDT) | **yes** (VEH/events) |

`svc 10/11` populate the **process LDT** (which our far-jmp uses → correct base), but
`VdmStartExecution` runs PM against the **VDM monitor's own LDT** (lacking our
descriptors → base 0). Correct execution and fault reflection currently come from two
different LDTs/mechanisms. **Unifying them** — getting the monitor to run PM against our
descriptors, or getting our far-jmp's faults reflected — is the open problem and needs
**ntoskrnl VDM-PM RE** (how `VdmpStartExecution`/the VDM trap path loads LDTR and decides
to reflect). That's a distinct, substantial research effort.

**Bottom line for the spike:** protected-mode *execution* under our architecture is
proven and the mechanism fully mapped; the `INT 31h` service surface is blocked on this
LDT/reflection unification, which is beyond quick iteration. A strong stopping point.

### VM run 15 + kernel recon (2026-08-01) — reflection is kernel-side; user-mode levers exhausted `[FACT]`

- **Run 15:** added `VdmPMCliControl(13,{3})` before the PM entry (ntvdm does this on its
  PM path). It **succeeds** (`svc13=0x0`) but does **not** arm fault reflection — same
  result (process exits at "entering PM", no event, no VEH). That was the last concrete
  user-mode lever.
- **ntvdm recon:** `fcn.0f00532e` (which calls the PM entry) is the body of the exported
  **`VDDSimulate16`** — ntvdm's *user-mode* "run the VDM" primitive, called from its main
  loop. So ntvdm runs PM from user mode too, yet its faults reflect. The difference must
  be kernel-side state we can't set from user mode.
- **ntoskrnl recon (`reverse/ntoskrnl.exe`):** `NtVdmControl` @ `0x4e09b7` gates on
  `KPROCESS+0x24b` bit0 (is-VDM) and `KPROCESS+0x158` (VdmObjects) — both set by our
  working `VdmInitialize`. Service 0 with `data!=0` → `0x4e0ac2`; `data==0` (our V86 run)
  → the dispatch at `0x52d1be`. The actual **PM-fault reflection** is not in
  `NtVdmControl` at all — it's in the **trap/exception path** (`KiDispatchException` /
  `VdmDispatchException`), which decides whether a fault in a VDM process is reflected to
  the monitor vs. delivered/terminated.

**Conclusion — where the spike stops:** every user-mode approach is exhausted. The
execution-vs-reflection unification is governed by ntoskrnl's trap dispatch, and cracking
it needs a **dedicated ntoskrnl RE effort** (trace `KiDispatchException`'s VDM path: the
exact condition under which a non-V86 VDM fault is reflected, and which LDT
`VdmpStartExecution` loads). That is a distinct research project, not incremental
iteration. **Protected-mode execution on the real CPU is proven and the entire user-mode
mechanism is mapped and built; the `INT 31h` service surface is blocked on this kernel
behaviour.**

### ReactOS/kernel research (2026-08-01) — the model was wrong: PM faults are USER-MODE exceptions, not VDM reflection `[FACT, ReactOS source]`

Cross-referenced ReactOS's NT-kernel reimplementation (mirrors XP's VDM design). Three
decisive findings:

1. **The GP fault handler only VDM-reflects V86 faults.** `KiTrap0D`
   (`ntoskrnl/ke/i386/traphdlr.c`) routes into VDM opcode emulation only when
   `KiV86Trap(TrapFrame)` is true — and `KiV86Trap` is literally
   `(EFlags & EFLAGS_V86_MASK) != 0`. A fault with **VM clear (protected mode)**, even in
   a VDM process (`VdmObjects` set), **falls through to standard exception delivery**
   (`KiDispatchException`) — it is *never* routed to `Ki386HandleOpcodeV86` /
   `KiVdmOpcodeINTnn`. This exactly matches our runs 8–15: PM `INT 31h`/faults are not
   reflected as `VTIB_EVENT`s because **the kernel doesn't do that for PM at all.**
2. **So real ntvdm must catch PM faults via USER-MODE exception handling** (SEH/VEH) —
   the normal exception path — not the V86 `VdmStartExecution` event mechanism. Our
   pursuit of `VTIB_EVENT` reflection for PM was chasing a V86-only mechanism.
3. **`MonitorContext` = `VDM_TIB+0x0C`** (the CONTEXT immediately before `VdmContext` at
   `0x2D8`; `VdmSwapContext` swaps between `MonitorContext` and `VdmContext`). Our
   host-save offset in `dpmi_enter_pm` was correct — but swap-back only happens for V86,
   so it's moot for PM.

**Corrected direction for the INT 31h surface (supersedes the "monitor PM-entry" plan):**
run PM via our far-jmp / `NtContinue` (proven, correct base), and **catch the PM
`INT 31h`/`#GP` with a user-mode handler** (VEH or a frame-based SEH). The real remaining
problem is *why the PM `#GP` doesn't reach our VEH* — a **user-mode exception-delivery**
issue for a 16-bit PM CS (D=0) context (runs 10/12/13 died with no VEH), NOT a kernel
mystery. Leads to try: a frame-based `__try/__except` instead of a VEH; verify the
exception actually reaches `KiUserExceptionDispatcher` (a 32-bit flat stack is in place
since run 9); check whether `int 0x31` raises a deliverable `#GP` vs a silent kill;
consider a **32-bit** PM client (flat CS, D=1) which real DOS/4GW-style extenders use and
whose faults dispatch cleanly. Then decode the faulting `CD 31`, service the DPMI call in
the handler, and resume via `NtContinue`. INT 31h service semantics (0000 alloc-descriptor,
0300 simulate-real-mode-INT) to crib from ReactOS `subsystems/mvdm/ntvdm/dos/dem.c` +
the DPMI 0.9 spec when wiring the surface.

### VM run 16 (2026-08-01) — 32-bit CS disproven; the variable is GDT-flat vs LDT selector `[FACT, real CPU]`

Acting on the ReactOS finding (PM faults = user-mode exceptions), tried a **32-bit code
selector** (D=1, `chi=0x0040fa00`) with size-independent client code (`INT 31h; JMP $`)
run via clean `NtContinue`, expecting the `#GP` to reach our VEH.

Result: **no change** — process exits silently, no VEH, no "NtContinue returned". So the
16-vs-32-bit CS is **not** the variable. Refining across all runs:

| Faulting CS | Path | VEH fires? |
|-------------|------|-----------|
| `0x1B` GDT flat (runs 1–6) | VdmStartExecution, base 0 | **yes** |
| `0x0F` LDT, 16-bit (runs 10/12/13) | far-jmp / NtContinue | no |
| `0x0F` LDT, 32-bit (run 16) | NtContinue | no |

**The variable is GDT-flat vs LDT selector, not the D-bit.** NT's user-mode exception
dispatch (`KiUserExceptionDispatcher`) is not delivering a fault whose `CS` is an **LDT
selector** to our handler — yet real ntvdm, which also runs its DPMI client on LDT
selectors, *does* catch those faults. So there is a **VDM-setup / kernel-delivery
difference** we haven't found: something ntvdm registers (beyond `VdmInitialize` +
TrapcHandler) that makes the kernel deliver an LDT-CS PM fault to its user-mode handler,
or a per-thread/VDM state the delivery path checks.

**Assessment:** the spike has now exhaustively mapped the problem from both the ntvdm and
ntoskrnl sides and every user-mode lever. The one remaining unknown — why an LDT-CS PM
fault reaches ntvdm's handler but not ours — is a subtle kernel exception-delivery
condition that needs a **dedicated `KiDispatchException`/`KiUserExceptionDispatcher` RE
pass** (how NT chooses to deliver vs terminate a ring-3 fault in an LDT/VDM context), not
more quick VM iterations. **Protected-mode execution is proven; the INT 31h surface is
blocked on this specific kernel-delivery condition.**

### VM runs 17–19 (2026-08-01) — the real blocker: XP's kernel SWALLOWS PM VDM faults `[FACT, real CPU]`

Switched to **flat selectors** (base 0, 4GB) so the faulting CS resolves the (high, flat)
`KiUserExceptionDispatcher` address — the hypothesis being that our old base-0x1000 CS put
the dispatcher out of range. New behaviour, and it reframes everything:

- With flat selectors the guest **no longer dies** on a fault — it **runs past `INT 31h`
  AND past `HLT`** (a guaranteed ring-3 `#GP`) and **spins at 100% CPU** (Task Manager,
  run 18). The VEH never fires.
- So `HLT`/`INT 31h` do fault, but the fault is **neither delivered to our VEH nor fatal**
  — **XP's kernel intercepts the PM VDM fault, skips the instruction, and resumes the
  guest** (via the TrapcHandler path). This holds with an empty *or* the real TrapcHandler.

**This is the core blocker, finally pinned:** XP's kernel (unlike ReactOS, which lacks PM
VDM support) **handles PM VDM faults by skip+resume** — it swallows them. So catching
`INT 31h` via a user-mode exception is impossible: the kernel eats the fault before user
mode sees it. (This also explains all of runs 8–16.) The only way to service `INT 31h`
is to make the kernel **vector** it to a handler we control rather than skip it — i.e.
the `KiVdmOpcodeINTnn` path (ReactOS): for an `INT nn` the kernel reads the vector from
the real-mode IVT at `0:nn*4` and transfers there (RPL-masked CS for PM). **Untested
next step: set `IVT[0x31]` to a PM handler selector:offset we own and see whether the
kernel vectors `INT 31h` there** (distinguishing INT-vectoring from the HLT skip).

**Harness:** spinning PM guests lock `ntvdmhost.exe` (stale-host reruns) and block the
log. Added a **watchdog** thread (3s → terminate) so every DPMI run self-terminates;
`ExitProcess` hangs unwinding the PM engine thread, so it uses `TerminateProcess`.

### VM runs 20–22 (2026-08-01) — runs 17–19 DEBUNKED: the flat LDT descriptor never installed `[FACT, real CPU]`

Rebuilt the headless rig (serial → `vm/serial.log`, autorun) and added serial markers at
every step of the switch. Three harness bugs were masking the host entirely (fixed):
`dpmitest.bat` used `start /wait "%prog%"` — `start`'s first *quoted* arg is the window
**title**, so it opened an empty titled cmd and ran nothing; the QMP `type` helper mapped
`\` to qcode `backslash`, which types `#` on this VM's **UK keyboard** (real `\` = qcode
`less`); and after a QMP CD hot-swap XP autologin **beats the optical re-enumeration**, so
`D:\` looks empty for several seconds (autorun now polls for the CD; a manual re-read via
Explorer forces it).

With the host actually running and its svc-status logged, the real finding:

- **`VdmSetLdtEntries` (svc 10) AND `VdmSetProcessLdtInfo` (svc 11) both returned
  `0xC000011A = STATUS_INVALID_LDT_DESCRIPTOR`** for the flat descriptor (base 0, limit
  `0xFFFFF`, G=1). So **the flat LDT never installed** — runs 17–19 ran PM with an
  empty/bogus LDT, and their "guest spins past HLT at 100% CPU / kernel swallows the fault"
  is an **artifact of the broken LDT**, not real kernel behaviour. **Runs 17–19 are retracted.**
- **Why XP rejects it** (ReactOS `PspIsDescriptorValid`, `ntoskrnl/ke/i386/ldt.c`): after the
  present/DPL=3/type checks it computes `SegLimit = G ? (limit<<12)|0xFFF : limit` and rejects
  unless `Base + SegLimit <= MmHighestUserAddress` (~`0x7FFEFFFF`). A flat 4 GB segment
  (`Base+SegLimit = 0xFFFFFFFF`) is far above the user ceiling → invalid. (ReactOS *disabled*
  this check for DOS32 compat; **real XP enforces it**.) A DOS/4GW-style flat client is thus
  impossible via a single 4 GB LDT selector on XP — it must use a limited selector (or GDT).
- **Fix — based, 64K, 16-bit selectors** (`base = seg<<4`, limit `0xFFFF`, G=0, D=0). These
  map the guest's real-mode segment (linear `seg<<4`, all < 1 MB, comfortably in user space).
  **Result (run 22): `svc11=0 svc10=0` — the switch SUCCEEDS**, VM bit cleared, `NtContinue`
  enters ring-3 PM at `CS=0x0F:0x125`. With a based CS/SS the resume EIP/ESP are the
  real-mode **offsets**, not linear addresses.
- **Genuine post-switch behaviour (valid LDT):** the guest's `HLT` (ring-3 `#GP`) makes the
  process **silently terminate within ~3 s** — no VEH, no watchdog line, no crash dialog.
  This is **runs 10–16's** behaviour (silent terminate), *not* runs 17–19's spin — confirming
  10–16 were the valid runs and the flat detour was noise. **The real blocker stands (run 15):
  a raw PM `#GP` on an LDT CS is not reflected to user mode; reflection is kernel-side.**

**Next (increment 3b, now testable on a *valid* PM entry):** distinguish a raw `#GP` (HLT,
terminates) from a software `INT nn`. XP's VDM trap path handles `INT nn` specially
(ReactOS `KiVdmOpcodeINTnn`: read `IVT[nn]`, vector there with an RPL-masked CS) — so
`INT 31h` may be **vectored to `IVT[0x31]`** even though `HLT` terminates. Change the client's
post-switch opcode to `INT 31h`, point `IVT[0x31]` at a handler we own, and see whether the
kernel vectors it (the DPMI service surface) rather than terminating.

### VM run 23 (2026-08-01) — valid LDT + real INT 31h + monitor entry still does NOT reflect `[FACT, real CPU]`

With the switch now succeeding (run 22), tested the architecturally-correct path end to end:
based valid LDT (svc10/11=0) → **`dpmi_enter_pm`** (the `0xf04483c` port that saves the host
CONTEXT to `VDM_TIB+0x0C` so the kernel's VDM trap path can reflect back) → guest executes a
real **`INT 31h`** (software INT, the kernel's `KiVdmOpcodeINTnn` path, distinct from a raw
`#GP`). Serial reached `entering PM (monitor far-jmp, dpmi_enter_pm)` and then **nothing** —
`dpmi_enter_pm` did **not** return, no `RETURNED` line, no watchdog, silent terminate.

So the reflection failure is **independent** of: (a) LDT descriptor validity (now valid),
(b) #GP-vs-software-INT (INT 31h behaves the same as HLT here), and (c) NtContinue-vs-monitor
entry (both terminate). The one thing we have NOT replicated is whatever ntvdm registers so the
**kernel** chooses to reflect a PM VDM fault to the host instead of terminating the process.
`VdmInitialize`'s `IcaUserData` is only the **PIC/ICA** emulation data (IRQ reflection), not a
PM fault-handler table — so it is not the lever.

**Next: RE `ntoskrnl` (reverse/NTOSKRNL.EX_).** Find `KiTrap0D`'s VDM branch and the PM-fault
reflection routine (`Ki386VdmReflectException` / `VdmDispatchException` / `KiDispatchException`
VDM case) and read the EXACT predicate that selects reflect-to-VDM vs deliver-to-usermode vs
terminate for a ring-3 fault whose CS is an LDT selector in a VDM process. That predicate names
the per-thread/VDM state (or handler-table pointer) we are missing. This is the dedicated kernel
RE pass flagged after run 16 — now with every user-mode variable eliminated.

### Kernel RE session 1 (2026-08-01) — foothold + leads for the KiTrap0D/VdmDispatchException pass `[FACT, disasm]`

Started the ntoskrnl RE flagged after run 23. Environment + facts to resume fast:

- **Tooling:** `r2` (radare2) works on `reverse/ntoskrnl.exe` (XP SP3, 2008-04-14; VA base
  `0x400000`, so file offset = VA − 0x400000). Use `-e scr.color=0`; ignore the "Relocs not
  applied" warning for static disasm. `reverse/NTVDM.EX_` is compressed; the annotated
  `ntvdm.exe` loads at image base `0x0f000000` (`r2 -e bin.baddr=0x0f000000`).
- **`NtVdmControl` @ VA `0x4e09b7`.** Service # = arg0 (`[ebp+8]`→esi). **Service 3
  (`VdmInitialize`) is special-cased** and bypasses the VDM-state gate; **service 14
  (`VdmQueryVdmProcess`)** also special. **All other services require** `EPROCESS[+0x158]`
  (the **VdmObjects** pointer) ≠ NULL **and** `EPROCESS[+0x24b] & 1`, else fail.
- **Kernel VDM state accessors (XP SP3 offsets):** current KTHREAD = `fs:[0x124]`;
  `KTHREAD[+0x44]` = Process (KPROCESS/EPROCESS); **`EPROCESS[+0x158]` = VdmObjects**. The
  kernel reads VdmObjects fields `+0xac`, `+0xba` (bit 0), `+0xbc` in dispatch/query helpers
  (e.g. VA `0x46bd53`, `0x54b72f`). This VdmObjects struct is where the PM reflection target
  almost certainly lives.
- **Search recipe that works:** find VdmObjects reads with
  `/x 8058010000` (`mov eax,[eax+0x158]`) and `/x b858010000` (`cmp [eax+0x158],reg`). Hits
  cluster in two regions: `0x46bxxx`/`0x42xxxx` (trap/dispatch helpers) and
  `0x565xxx–0x566xxx` (NtVdmControl service handlers w/ ProbeForWrite+SEH — NOT the fault path).

**Refined hypothesis to test next:** the kernel's user-exception delivery needs a **flat** CS to
reach `KiUserExceptionDispatcher` (run 16: GDT-flat CS → VEH fires; LDT CS → no VEH). For a VDM
process (`VdmObjects`≠NULL) `KiDispatchException` instead calls **`VdmDispatchException`**, which
reflects the PM fault to a target read from **VdmObjects / the VDM_TIB** (a PM IDT or PM
exception-handler vector). We never populate that PM IDT, so `INT 31h` reflects to a null/garbage
target → silent terminate (no VEH, matches runs 10–16, 22–23).

**Exact next step:** locate `KiDispatchException` (calls the ntdll `KeUserExceptionDispatcher`
global for normal delivery; calls `VdmDispatchException` in the `VdmObjects≠NULL` branch), then
disasm `VdmDispatchException` to read (a) its TRUE/FALSE return condition and (b) the VdmObjects/
VDM_TIB field it uses as the reflection CS:EIP. That field = the PM IDT/handler table we must set
up (candidate fix: populate the guest PM IDT `[0x31]` with a selector:offset that BOPs to the host).

### Kernel RE session 2 (2026-08-01) — reflection is in KiTrap0D, gated by the VDM fixed-state flag `[FACT, disasm]`

Traced `KiDispatchException`'s VDM branch and ruled it OUT as the reflection point:
- `KiDispatchException` (VA ~`0x421cf7`): `fs:[0x124]`→KTHREAD, `[+0x44]`→Process, `cmp [+0x158],0`
  (VdmObjects). If NULL → normal path `0x40ec73`; if non-NULL → `jmp 0x44664c`.
- **`0x44664c` does NOT reflect** — it just records fault bits
  (`[ebx+0x298] |= [trapframe+0x88] & 6`) and exits to the normal dispatch tail (`0x40ec73`/
  `0x40ec85`). The predicate it calls, `fcn.0x441467`, returns TRUE only when `VdmObjects==NULL`
  **and** a PCR flag (`[0xffdff018]+0x164`) is set — a teardown special-case, not our path.

⇒ **PM-fault reflection happens earlier, in `KiTrap0D` (the #GP handler), before
`KiDispatchException`.** This fits run 16: a GDT-flat CS fault reaches our VEH (normal path),
but an LDT-CS fault does **not** — `KiTrap0D` diverts it onto the VDM path. So the kernel
**recognizes** our PM fault as a VDM fault; it just **fails to complete the reflect** and
terminates instead.

**The reflect it attempts** (per ntvdm's `0xf04483c`/`dpmi_enter_pm` contract): save guest →
`VDM_TIB+0x2D8`, restore the host-save CONTEXT at `TIB+0x0C`, set `VTIB_EVENT`, resume the host
(so `dpmi_enter_pm` *returns* with the event). For the kernel to take this path it must believe a
host is **monitoring** the VDM — gated by the VDM **fixed-state / in-monitor flag** that
`dpmi_enter_pm` writes at linear **`[0x714]`** (`lock or [0x714],0x200`, then `test [0x714],3`).
Prime suspect: the kernel reads the VDM fixed state at an address our minimal `v86_get_tib`/
`VdmInitialize` never established (real ntvdm maps a `FIXED_NTVDMSTATE`-style block the kernel
knows), so `[0x714]` is the wrong cell → kernel sees "unmonitored" → terminate.

**Exact next step:** find `KiTrap0D` (the #GP IDT handler) and its VDM branch; read where it
tests the VDM fixed-state/monitor flag and how it locates that memory (a fixed linear address, or
a pointer in VdmObjects/VDM_TIB). Confirm whether our `[0x714]` matches. If the kernel expects a
`FIXED_NTVDMSTATE` block we don't map, mapping/initialising it is the fix. (Cross-check against
ntvdm's own `VdmInitialize` call site + any low-memory fixed-state setup it does before PM entry.)

### VM run 24 (2026-08-01) — BREAKTHROUGH: VdmStartExecution delivers the PM fault to user mode `[FACT, real CPU]`

Plan B (empirical). Key asymmetry: our **V86** faults already reflect (DOS INT 21h works) because
V86 runs *inside* `VdmStartExecution` (the kernel monitor); `dpmi_enter_pm`/`NtContinue` far-jmp
into PM *outside* it → silent terminate. So: set up the PM state (valid based LDT, PE bit, VM-clear
CONTEXT, CS=`0x0F:0x125`) and then `continue` back to the loop so **`v86_run` (`VdmStartExecution`)**
runs the PM CONTEXT. Result (v8):

```
-> PM ok (VM cleared, CS=0x0f EIP=0x125) -> running PM via VdmStartExecution [expt B]
STAGE3-DPMI: watchdog thread started
DPMI FATAL: exception code=0xc000001e at 0x00000127
  PM fault ctx: CS=0x1b EIP=0x127 SS=0x23 ESP=0xfffe  AX=0x0 CX=0xfffe BX=0x0 DS=0x17
```

**The VEH FIRES** (`dpmi_crash_veh`) — the first time ANY PM fault reached user mode (vs. silent
terminate for all of runs 8–23). So **running the PM guest under `VdmStartExecution` is the missing
mechanism** for kernel→user delivery of PM VDM faults.

State read: the `INT 31h` (CD 31 @ `0x125`) was consumed (EIP `0x125`→`0x127`), but the guest
resumed with **CS=`0x1B`** (flat GDT user selector) not our LDT `0x0F`, so `0x1B:0x127` = **linear
`0x127` = the IVT** (garbage) → `0xC000001E` STATUS_INVALID_LOCK_SEQUENCE (executing junk that
decodes as a bad LOCK prefix). The wrong CS almost certainly comes from the **MonitorContext at
`VDM_TIB+0x0C`**, which `dpmi_enter_pm` populates but bare-`VdmStartExecution` (expt B) did not —
so the kernel restored a stale host/monitor context on reflection.

**Next:** combine — populate `TIB+0x0C` (host-save CONTEXT, as `dpmi_enter_pm` does) *before*
`VdmStartExecution`, so the kernel restores a VALID context on reflection and the guest resumes at
`0x0F:0x127`. Then chase a CLEAN `INT 31h` trap (stop the guest ON the INT so the host can service
it) rather than letting it run past into the IVT. Enhance the VEH to dump all segs + the faulting
bytes to confirm each step.

### VM run 25 (2026-08-01) — PM INT 31h INTERCEPTION WORKS: reflect → VEH → fix-up → clean resume `[FACT, real CPU]`

Building on run 24. Enhanced the VEH dump: the fault at `0x1B:0x127` is executing the **IVT**
(`bytes@fault: f0 53 ff 00...` = `LOCK PUSH`, hence `0xC000001E`), while the guest's real code at
linear `0x1125` is intact (`cd 31 eb fe` = `INT 31h; jmp $`). Register state at the fault:
`DS=ES=FS=GS=0x17` (our LDT data sel), but `CS=0x1B`/`SS=0x23` (**flat**), `EIP=0x127`
(=int_site+2, past the INT), `EDX=0x125` (=int_site), `EAX=0` (the guest's DPMI function). VM bit
clear (EFL `0x10282`).

So under `VdmStartExecution` the kernel **reflects the PM `INT 31h`**: it advances EIP past the INT
and reloads `CS`/`SS` to the flat selectors — the ONLY corruption is `CS` is flat (`0x1B`) instead of
our LDT `0x0F`, so the guest lands at linear `0x127` (IVT) and faults into our VEH.

**Confirmed the fix:** the VEH sets `CS=0x0F`, `SS=0x17` and returns `EXCEPTION_CONTINUE_EXECUTION`.
Result: **the guest resumes cleanly in PM and runs 3 s with NO further fault** (watchdog stops the
`jmp $` spin). So the reflect **consumed** the INT; only CS/SS needed restoring.

**⇒ Working PM INT 31h hook:** guest `INT 31h` (PM) → kernel reflect → VEH fires with the guest
state → VEH services the DPMI call, sets returns, restores `CS/SS`, `CONTINUE_EXECUTION` → guest
continues. The VEH is the DPMI dispatcher. Vector is recoverable from `[int_site]` (`EDX`), function
from `AX`.

**Caveat (robustness):** the VEH only fires because the mangled flat `CS:small_offset` lands in
low memory (IVT) and faults — reliable for a low-offset 16-bit `.COM`, but a guest whose flat
`CS:offset` hit valid bytes could run past unhooked. Robust path (later): find where the kernel
reads the PM-interrupt reflection target and point it at a real flat host handler (so no CS mangling
/ downstream fault). For the spike, the VEH hook is sufficient to build + prove the INT 31h surface.

**Next:** wire the INT 31h dispatch into the VEH (read vector from `[EDX]`, function from `AX`,
service e.g. `AX=0000` alloc-descriptor via svc 10, return selector, resume) and VM-confirm a real
DPMI round-trip.

### VM run 27 (2026-08-01) — CORRECTION: the guest does NOT execute under VdmStartExecution (runs 25–26 retracted) `[FACT, real CPU]`

Added a sentinel: the guest's FIRST PM instruction is `mov word [0x600],0xBEEF` /
`mov word [0x602],0xCAFE` (writes to DS:0x600 = linear 0x1600), and the VEH dumps linear 0x1600.
Result:

```
DPMI INT31h #1: AX=0x0 ... [site EDX=0x125 EIP=0x127 b@site=c7 06 00 06] sentinel@0x1600=00 00 00 00
```

- `b@site` (at `EDX`) = `c7 06 00 06` = the guest's first instruction (`mov word [0x600],..`), so
  **`EDX` = the PM ENTRY point, not an int-site**.
- **`sentinel@0x1600 = 00 00 00 00` — the guest's first instruction did NOT execute.**
- `EIP` = entry+2, a FIXED advance independent of instruction length (a 6-byte `mov`).

**⇒ Under `VdmStartExecution` the PM guest never runs.** Feeding the V86 monitor a PM (VM-clear,
LDT-CS) CONTEXT produces a deterministic, code-independent fault (`CS→flat 0x1B`, `EIP=entry+2`,
`EDX=entry`) that surfaces to the VEH — but it is NOT `INT 31h` reflection and NOT PM execution.
**Runs 25–26's "PM INT 31h interception works" is RETRACTED.** (run 6 stands: `VdmStartExecution`
does not run PM.) The one real gain from B: `VdmStartExecution` + a PM context *does* deliver a
fault to the user-mode VEH (vs. `dpmi_enter_pm`'s silent terminate) — a kernel-delivery clue, but
not a working path.

**Open, now-critical question:** has the guest EVER executed a PM instruction on the real CPU?
Neither `dpmi_enter_pm` (silent terminate) nor `VdmStartExecution` (no run) is sentinel-confirmed.
**Next:** revert the switch to `dpmi_enter_pm` (the real ntvdm far-jmp) WITH the sentinel guest, and
capture linear 0x1600 from a concurrent reader (the watchdog thread) before/at the terminate — to
prove whether the far-jmp executes PM code at all. If yes → PM runs, the blocker is purely the
fault reflection (back to plan A: KiTrap0D). If no → PM entry itself never lands, and the switch
CONTEXT/LDT needs rework before anything else.

### VM run 28 (2026-08-01) — DEFINITIVE: PM code EXECUTES on the real CPU via dpmi_enter_pm `[FACT, real CPU]`

The decisive test. Guest (PM): `mov word [0x600],0xBEEF` / `mov word [0x602],0xCAFE` then `jmp $`
(sentinel + spin, NO INT, so it won't fault if PM runs). Switch reverted to **`dpmi_enter_pm`** (the
real ntvdm far-jmp, `0xf04483c`). The watchdog thread samples linear `0x1600` (= DS:0x600) every
250 ms. Result:

```
wd[0..9] sentinel = ef be fe ca    (all 10 samples, full 3 s, until watchdog terminate)
```

`ef be fe ca` = **`0xBEEF, 0xCAFE`** — **the guest's PM instructions executed**, and the guest then
spun cleanly for 3 s (no fault, no silent terminate). **Protected-mode code genuinely runs on the
real CPU via `dpmi_enter_pm`.** (Confirms the long-assumed "run 8" claim, now sentinel-verified.)

**This settles the foundation and re-aims the whole effort:**
- ✅ V86→PM switch works (run 22); ✅ LDT installs (svc10/11=0); ✅ **PM code executes (run 28)**.
- ❌ The SOLE remaining blocker is **fault/INT reflection**: `dpmi_enter_pm` runs PM fine until the
  guest faults (e.g. `INT 31h`), at which point the kernel silently terminates instead of reflecting
  to our host (no VEH, no `VTIB_EVENT` return). Runs 24–27 (VdmStartExecution / VEH artifact) were a
  detour — that path doesn't even run PM.

**⇒ Back to plan A, now on solid ground: RE `KiTrap0D`'s PM-VDM branch** — why a ring-3 LDT-CS fault
in this VDM terminates instead of reflecting (save guest→`TIB+0x2D8`, restore host CONTEXT@`TIB+0x0C`,
set `VTIB_EVENT`, return to the host loop). One clue banked from run 24: under `VdmStartExecution`
the kernel WILL deliver a fault to our user-mode VEH — so the delivery machinery exists; we need the
state that makes `dpmi_enter_pm`'s in-PM fault take the reflect path. Candidate levers to test on the
now-proven PM base: the `[0x714]` in-monitor flag, the host-save CONTEXT@`TIB+0x0C` contents, and the
per-thread VDM "monitored" state.

### VM run 29 (2026-08-01) — ntvdm-side PM-entry setup deltas ruled out; reflection is kernel-side `[FACT, disasm + real CPU]`

RE'd ntvdm's PM execution orchestrator **`fcn.0f00532e`** (the caller of the PM entry `0xf04483c`).
Its structure: record `CurrentMonitorTeb=fs:[0x18]`; set `VDM_TIB[0x670]=1`; set
`VDM_TIB[0x2D8]=0x10007` (guest CONTEXT.ContextFlags — the guest regs at 0x374–0x3A0 are this
CONTEXT: Eip@0x2D8+0xB8=0x390, Cs@0x394, EFlags@0x398, Esp@0x39C, Ss@0x3A0); `getMSW` PE test →
PM branch: (conditionally) `VdmPMCliControl(3)`, then `and [0x39A],0xFD` (clear VM bit), then
`call 0xf04483c`; on return, dispatch `VTIB_EVENT` via table `0xf064a60`, loop while `[0x670]`.

Tested/ruled out each ntvdm-side delta as the reflection lever:
- **`VDM_TIB[0x670]=1`** (we had 0): set it → the guest's `INT 31h` **still silent-terminates**
  (so fast the watchdog thread never prints). Not the lever.
- **`VdmPMCliControl` (svc 13)**: TWO sites — init-ish `0xf004908` (data=2) and per-entry
  `0xf0053ad` (data=3). BOTH are gated on **guest IF == 0** (`getIF` check). Our guest has IF SET
  (`EFLAGS=0x202`), so **ntvdm itself skips both** → not the lever. (svc 13 = PM CLI/STI
  virtualization: manages `[0x714]` bits 0/1 = the virtual interrupt-pending flag.)
- `CurrentMonitorTeb` is an ntvdm-internal global (used by the TrapcHandler), not kernel-checked.
- `[0x2D8]=0x10007` — we already set it (`v86_set_entry`, `VTIB_CONTEXT`).

⇒ Our PM-entry setup matches ntvdm's for an IF-set guest, yet the `INT 31h` fault terminates
instead of reflecting. **The gap is kernel-side**, not in the ntvdm-replicated setup. Definitive
next step: RE `KiTrap0D`'s #GP path (`reverse/NTOSKRNL`) — for a ring-3 LDT-CS fault in a VDM
process, the exact predicate/state that selects reflect-to-VDM (save guest→CONTEXT@0x2D8, restore
host MonitorContext@`TIB+0x0C`, set `VTIB_EVENT`, return to host) vs. terminate. Foothold banked
in "Kernel RE session 1/2": `KiDispatchException`@~0x421cf7 gates on `EPROCESS[+0x158]` (VdmObjects);
`KiTrap0D` is the lower #GP handler that reflects BEFORE `KiDispatchException`.

### VM run 30 (2026-08-01) — base-0/2GB-limit CS installs but STILL no VEH delivery (limit is not the variable) `[FACT, real CPU]`

Kernel RE (session 3) of the #GP path: the `KiTrapXX` shared prologue
(`0x403972`: build KTRAP_FRAME, `test [esp+0x70],0x20000` → V86 path `0x403ac0`) shows a **PM VDM
fault is NON-V86**, so it goes down the generic path → `KiDispatchException`, whose user-mode
delivery validates the CONTEXT against **`MmHighestUserAddress`** (`0x40ecad`). That surfaced a
hypothesis: prior LDT CSs had a 64K limit, so `KiUserExceptionDispatcher` (a high user address) was
out of segment range → delivery impossible; a base-0 CS whose limit covers all user space should
both install AND let the fault reach the VEH.

Tested (run 30): CS/SS = base 0, limit `0x7FFEF` (G=1 → 0x7FFEFFFF), 32-bit; size-independent guest
(`INT 31h; jmp $`); linear EIP/ESP.
- **The descriptor INSTALLS** (`svc10/11=0`, `clo=0x0000ffef chi=0x00c7fa00`) — confirms the
  `base+limit <= MmHighestUserAddress` install rule.
- **But the `INT 31h` STILL silently terminates** — no VEH, no watchdog. **Hypothesis DISPROVEN:**
  segment-limit / dispatcher-reachability is NOT the variable. An LDT-CS VDM PM fault is genuinely
  **routed away from normal user-mode exception delivery** (run 16 stands) and the divert (the VDM
  reflect) fails → terminate — regardless of the CS limit. Reverted to the proven based-64K config.

**State of the search:** PM executes (run 28); the LDT-CS PM fault neither delivers to the VEH nor
completes the VDM reflect. Ruled out: ntvdm-side setup deltas (run 29), CS limit / dispatcher
reachability (run 30). The reflect-vs-terminate decision + why the reflect fails remains inside
`KiTrap0D`'s pre-`KiDispatchException` VDM handling (the `0x403ac0` V86 path is for V86; the PM-VDM
equivalent, and where it decides to divert an LDT-CS fault, is the next target). This is a deep
kernel-disasm task; foothold: `KiTrapXX` prologue `0x403972`, V86 branch `0x403ac0`,
`KiDispatchException` VDM record-branch `0x421cf7`→`0x44664c`, user-delivery validation `0x40ec99`.

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
