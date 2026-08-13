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

### Kernel RE session 4 (2026-08-01) — THE ANSWER: PM faults reflect via VDM_TIB+0x634, which we never set up `[FACT, disasm]`

Fully traced XP's `KiTrap0D` #GP path for a protected-mode VDM fault (our `INT 31h`):

1. **Shared trap prologue** (`0x408d02`): `test [esp+8],0x20000` (V86?) → **non-V86 jumps to
   `0x409177`** (the V86 fall-through does BOP `C4 C4` reflection with `CS<<4` addressing).
2. **`0x409177`** builds the frame, then the decision (`0x4091e4`):
   `cmp word [CS],0x1b` → **if CS==`0x1B` (GDT flat) → normal exception path → our VEH**; else
   `cmp [EPROCESS+0x158],0` (VdmObjects) → **VDM process → `0x40925e`**. *This is exactly run 16:
   flat-CS delivers to the VEH, our LDT-CS diverts to the VDM path.*
3. **`0x40925e`**: `call 0x565041` (returns al; if `al&0xf!=0` → resume/handled) else
   `call 0x4f67f8(6)` else → terminate.
4. **`0x565041`** = the PM VDM dispatcher: resolves CS:EIP via the LDT (`0x45dd5f`), reads the
   faulting bytes, reflects ONLY `C4 C4` **BOPs** (sets `VTIB_EVENT=4`). A plain `INT 31h`
   (`CD 31`) is not a BOP → **returns 0** → not handled.
5. **`0x4f67f8`** = the PM exception reflector: for VM-clear (PM), CS≠`0x1B`, it saves the fault
   `CS:EIP:SS:ESP:EFlags` and `call 0x4f6f67`; on success it rewrites the trap frame to the handler
   and resumes; on **failure → returns 0 → terminate**.
6. **`0x4f6f67` → `0x4f6fed`** (PM branch): gets `VDM_TIB` (`[KPCR+0x18]→[+0xF18]`), then
   **`lea edi,[VDM_TIB+0x634]`**, `call 0x4f6e6f`.
7. **`0x4f6e6f`** decodes **`VDM_TIB+0x634`** — the PM fault-reflection control block:
   - `+0x00` (word) nesting/"in-handler" count · `+0x04` (word) **handler STACK selector (SS)** ·
     `+0x06` (word) saved fault SS · `+0x08` (dword) saved fault ESP · `+0x0C` (dword) saved fault EIP.
   - It saves the fault SS:ESP:EIP into `+6/+8/+0xC`, switches the guest to the handler stack
     (`SS=[+4]`, `ESP=0x1000`), and **resolves `[+4]` via `0x4f6dc0`; if 0/invalid → returns 0 →
     the whole reflect fails → terminate.**

**⇒ ROOT CAUSE (after ~30 runs): the kernel reflects a PM VDM fault by switching to a handler whose
stack/CS:EIP live in `VDM_TIB+0x634`. Real ntvdm populates that block (its PM fault handler + a
dedicated PM stack selector). Our host never sets it up, so `[VDM_TIB+0x634+4]` (the handler SS) is
0 → the kernel's selector-resolve fails → every PM fault terminates.** This unifies every prior
result: PM executes fine (run 28); the switch/LDT are correct (run 22); the fault just has nowhere
to reflect to.

**Next (implementation):** RE how ntvdm fills `VDM_TIB+0x634` (search ntvdm for writes to `+0x634`
and the PM-handler CS:EIP/SS it registers), then replicate: install a PM code+stack selector in our
LDT, point `VDM_TIB+0x634` at a PM handler stub that BOPs back to the host (so `INT 31h` reflects to
our stub → host services DPMI → returns to the client). Non-trivial but now fully specified.

### VM run 31 (2026-08-01) — implementing the fix: VDM_TIB+0x634 populated, TIB write CONFIRMED landing `[FACT, real CPU + disasm]`

Started implementing the run-4 root-cause fix. From ntvdm's own setup:
- **`VDM_TIB+0x634` block layout** (init routine `ntvdm 0x0f050ad7`; return `0x0f005040`):
  `+0x634` (word) nesting count · `+0x636` (word) **handler CS** (from global `[0xf09c178]`; bit 0 =
  16/32 flag) · `+0x638` (word) **handler SS** · `+0x63a` (word) saved fault SS · `+0x63c` (dword)
  saved fault ESP · `+0x640` (dword) saved fault EIP · `+0x650` (dword) packed **handler
  return-address CS:IP** (kernel pushes it on the handler stack — `0x4f71a0`).
- Populated `[+0x634]=0, [+0x636]=0x0F (code sel), [+0x638]=0x17 (stack sel)` before
  `dpmi_enter_pm`, and added a readback: **`tib==TEB[0xF18]` (match=YES)** and the values stick — so
  the write lands in exactly the TIB the kernel dereferences (`[KPCR+0x18]→[+0xF18]`).

**Result: still silent-terminates.** Since the SS (`+0x638`) is now valid (the resolve that used to
fail now passes), the reflect proceeds — but we set the handler **CS/SS, not the handler ENTRY
EIP**, so the kernel reflects the guest to `CS=0x0F:0` (zeroed) = linear `0x1000` (PSP garbage) →
re-fault → nested → terminate. Progress: the *original* terminate cause (invalid SS) is fixed; the
remaining gap is the handler entry + a real handler.

**Exact remaining work to a working reflect (§1 of the roadmap):**
1. **Pin the handler-ENTRY field.** `+0x650` is the return address the handler RETFs to, not the
   entry. The trap-frame CS:EIP the kernel resumes at is set in `0x4f67f8` (`[esi+0xc]/[esi+0x10]`);
   need to find which VDM_TIB field feeds it (search ntvdm for where it registers the DPMI PM
   fault/interrupt handler CS:EIP — near the `[0xf09c178]` handler-CS global).
2. **Build a PM handler stub:** a few bytes reachable via CS=`0x0F` that execute a **BOP (`C4 C4 nn`)** —
   the kernel's BOP path (`0x565041`) reflects BOPs cleanly as `VTIB_EVENT=4`, so the stub bounces
   control to our host loop, which then services `INT 31h` (read the saved fault state from
   `+0x63a/+0x63c/+0x640`, dispatch by AX, set returns, RETF back via `+0x650`).
3. Set `+0x650` = packed return CS:IP (client resume point) and the entry field = stub CS:IP.

The mechanism is proven and the TIB plumbing works; this is now bounded assembly/bookkeeping, not RE
of the unknown.

### VM run 32 (2026-08-01) — PM BOP REFLECTS: the PM→host primitive WORKS `[FACT, real CPU]`

With `VDM_TIB+0x634` populated and the TIB write confirmed (run 31), changed the guest's
post-switch code to a **raw BOP `C4 C4 58`** (instead of `INT 31h`). Result:

```
dpmi_enter_pm RETURNED event=0x00000004 CS:EIP=0x0000000f:0x00000125
```

**A protected-mode BOP executed on the real CPU, the kernel's PM dispatcher `0x565041`
recognized `C4 C4`, set `VTIB_EVENT=4`, and `dpmi_enter_pm` RETURNED to the host** with the
guest state — identical to how V86 BOPs surface. This is the observable PM→host mechanism we
needed, and it **validates the whole reflect strategy**: PM executes (run 28) + a PM BOP bounces
to the host loop (run 32). We can now iterate WITH observability (a BOP that fires => a visible
`RETURNED event=4`), ending the blind silent-terminate era.

**Remaining to close §1 (working INT 31h reflect):** the client issues `INT 31h` (`CD 31`), which
`#GP`s and takes the `0x4f67f8`→`VDM_TIB+0x634` exception-reflect path (NOT the BOP path). That
path still doesn't complete for us: a single BOP planted at `0x0F:0` was NOT hit (run 20/v20), so
the reflect either fails inside `0x4f67f8` or resumes at a `CS=0x0F:EIP` we haven't pinned. Two
threads to finish it, both now observable:
1. **Pin the handler entry**: `[+0x636]` bit 0 is a 16/32 flag (`0x4f7063 test [edi+2],1`) — our
   `0x0F` has bit 0 set (RPL3) so the kernel takes the 32-bit frame path, a likely mismatch for our
   16-bit handler. Find how ntvdm builds `[0xf09c178]` (the `+0x636` value) and the paired handler
   EIP (search ntvdm for the write to `0xf09c178` / the DPMI PM-handler registration).
2. **Carpet probe**: revert the guest to `INT 31h`, fill the PSP region (`0x1000..`) with a BOP
   pattern; if the reflect lands anywhere in it, `dpmi_enter_pm` returns and the returned CS:EIP
   pins the landing empirically.

Once the reflect lands on a BOP stub, the loop is: guest `INT 31h` → `#GP` → kernel reflect →
BOP stub → `dpmi_enter_pm` returns `event=4` → host services the DPMI call (guest state at the
VTIB regs / `+0x63a/+0x63c/+0x640`) → re-enter PM. Mechanism complete; then it's the INT 31h
service surface (roadmap §2).

### VM run 33 (2026-08-01) — +0x636 is a 16/32 FLAG; INT 31h reflect needs the separate handler registration `[FACT, disasm + real CPU]`

Two corrections from ntvdm's DPMI raw-mode-switch registration (`ntvdm 0x0f01a300`):
- **`VDM_TIB[0x636]` is NOT a CS selector — it is the 16/32-bit FLAG** (`[0xf09c178] =
  [CONTEXT.Eax+0xb0] & 1`, then copied to `[+0x636]`; `test [0x715],1` set if 32-bit). Our
  `+0x636=0x0F` wrongly selected the 32-bit reflect frame. Corrected to `0` (16-bit).
- **The handler CS:EIP is registered in ntvdm GLOBALS** (`[0xf09c15c]=IP, [0xf09c15e]=CS`, from a
  guest-provided structure via `Sim32pGetVDMPointer`), **not** in the `+0x634` block. So populating
  `+0x634/636/638` alone cannot supply the reflect target.

Carpet probe (run 33): guest `INT 31h`; host filled linear `0x1000..0x1124` (all below the INT at
`0x1125`) with BOPs; `+0x636=0`, `+0x638=0x17`, TIB match=YES. **Result: still silent-terminates —
no BOP hit.** So the `#GP` reflect does not resume anywhere in `0..0x124`. Given `0x4f6f67`'s tail
does not rewrite the block's CS/EIP (`[esi+0xc]/[esi+0x10]`), the reflect most likely resumes at the
**fault CS:EIP itself (`0x0F:0x125`)** → re-executes the `INT 31h` → nested → terminate; or it fails
earlier in `0x4f67f8`.

**Assessment:** the `INT 31h` (`#GP`) exception-reflect is materially more involved than the direct
BOP path (run 32) — it needs the DPMI PM exception/interrupt **handler registration** ntvdm does at
init (the `[0xf09c15c/15e]` globals + whatever kernel-side vector table pairs with them), not just
the `+0x634` stack block. **What's solid and observable: PM executes (28) and a PM BOP round-trips to
the host (32).** The remaining INT 31h routing is a focused RE of ntvdm's full DPMI-init handler
registration + the kernel's `0x4f67f8` handler-CS:EIP source — best taken fresh with the run-32
observability harness (a landed BOP prints `RETURNED event=4`).

### VM run 34 (2026-08-01) — the #GP reflect FAILS in-kernel (not a +0x634 field); need full ntvdm DPMI-init `[FACT, real CPU]`

Exhausted the field-guessing on `VDM_TIB+0x634`. Tried, all silent-terminate:
- `+0x636` = 0x0F (32-bit path) and = 0 (16-bit path); `+0x638` = 0x17 (valid SS, TIB match=YES);
- handler far-ptr `+0x64c`/`+0x650` = `0x0F:0x400` with a BOP stub there;
- BOP carpet over `0x1000..0x1124`; a BOP right AFTER the INT (`0x127`).

**None of the BOP catchers fired** — so the kernel's exception reflector `0x4f67f8` is **returning
"not handled" (0) and the process terminates**; it never resumes the guest at any address we can
plant a stub at. (Fast terminate, watchdog never runs ⇒ not a slow nested loop; it's an early
`return 0`.) So the failure is a **resolve/state check inside `0x4f67f8`/`0x4f6f67`**, gated on VDM
state we haven't established — NOT a missing `+0x634` value.

**Conclusion: stop guessing individual fields; replicate ntvdm's COMPLETE DPMI mode-switch init.**
ntvdm makes `INT 31h` reflect, so its full setup (whatever `0x4f67f8`'s `0x564ed5(&[0x714])` flags
+ resolves require) is the ground truth. Definitive next step: RE ntvdm's **mode-switch entry
handler** — the routine invoked when the client far-calls the `INT 2Fh/1687` entry (`ntvdm` 1687
site `0xf02d39f` → the returned entry) — and replicate its ENTIRE PM-setup sequence faithfully
(LDT, the `[0x714]` fixed-state init, the exception/interrupt handler registration, `VdmPMCliControl`,
the `+0x634` block), rather than cherry-picking fields. What remains solid and observable: PM
executes (28) and a PM BOP round-trips to the host (32); those give the harness to validate the full
init once replicated.


**Elimination (kernel RE):** the selector resolver `0x45dd5f` (used by all reflect resolves)
requires `(sel & 7)==7` (LDT, RPL3) and a valid process LDT. But the run-32 PM BOP went through
this SAME resolver (`0x565041` resolves CS to read the opcode) and SUCCEEDED — so **our selectors
resolve fine; the resolve is not the failure.** The `INT 31h` reflect fails specifically in the
exception-handler setup inside `0x4f6f67`, which needs ntvdm's registered DPMI PM exception handler.
This confirms the run-34 pivot: replicate ntvdm's full DPMI-init, don't patch `+0x634` fields.

### VM runs 35–41 (2026-08-02) — SOLVED by BYPASS: patch client INT nn → BOP at the switch `[FACT, real CPU]`

The run-34 plan ("replicate ntvdm's COMPLETE DPMI mode-switch init") is **superseded**. Rather than
make the native kernel reflect a raw `INT 31h` #GP (still unsolved), we **sidestep** it: the run-32
BOP primitive already round-trips PM→host cleanly, so at mode-switch the host scans the client's PM
code and rewrites each `CD 31`/`CD 21` → `C4 C4` (a BOP — same 2 bytes), recording the original
vector per CS offset (`g_int_vec[]`). The kernel reflects the BOP as `VTIB_EVENT=4`; the host's **DPMI
PM loop** (`src/host/main.c`, the `sw==0` block) runs PM → looks up the vector by fault EIP →
dispatches (INT 31h = DPMI, INT 21h = DOS) → writes returns+CF into the guest CONTEXT → advances +2 →
re-enters PM. All `DS:DX`/`ES:DI` buffers translate selector→linear via `dpmi_sel_base()`.

Incremental, each VM-confirmed on the real CPU:
- **run 35** — round-trip works end-to-end; **run 36** — an UNMODIFIED client runs (INT 31h/21h
  patched to BOP); **run 37** — the client PRINTS from PM (INT 21h AH=09 thunk).
- **run 38** — descriptor API: INT 31h `0000`/`0001` alloc/free, `0006`/`0007` get/set base, `0008`
  set limit, `0009` set access. Real LDT via `g_ldt[]` + `dpmi_install()` (svc 10 `NtSetLdtEntries`).
- **run 39** — extended memory `0500`/`0501`/`0502` (VirtualAlloc; in-process ⇒ linear = host ptr) +
  INT 21h AH=40 write.
- **run 40** — file I/O INT 21h `3C`/`3D`/`3E`/`3F`/`42` + descriptor alias `000A`.
- **run 41** — `0300` **simulate real-mode interrupt** (loads an RMCS at ES:DI, runs the INT via
  `dos_int21`, writes results back) — the way extenders route DOS/BIOS.

Foundational facts (proven, won't redo): the V86→PM switch works with **based 64K 16-bit LDT
selectors** (base=seg<<4); XP **rejects** a flat 4GB LDT descriptor (`STATUS_INVALID_LDT_DESCRIPTOR`).
Client = `tools/dostest/dpmitest.asm`; the STAGE0 marker `dpmi-harness-vNN` proves a fresh host ran.

### VM runs 42–43 (2026-08-02) — harden the patch scan + table services (0100/0101, 0204/0205, 0900–0902) `[FACT, real CPU]`

First hardening pass toward running a REAL extender (GH #2).

- **Patch scan hardened** (`src/host/main.c`). The known fragility was a bound of `0x2000` (8 KB): a
  real program's INT sites live well past that, so the client would #GP-hang on the first unpatched
  `INT 31h`. Now scans the **full 64K code selector**. The zeroed stack/BSS tail can't match
  `CD 31`/`CD 21`, so scanning it is harmless. `g_int_vec[]` doubles as a **revertible original-bytes
  map** (`!=0` ⇒ that offset was `CD <vec>`, now `C4 C4`), so a data mis-patch is detectable/undoable.
  **Why up-front, not lazy-on-fault:** a raw PM `INT 31h` #GP is exactly the reflect the kernel won't
  give us (runs 20–34) — we cannot wait for the fault to catch it, so every site must be found before
  the client runs. Residual risk: without a disassembler a `CD 31`/`CD 21` byte-pair that is *data*
  gets over-patched (x86 isn't self-synchronising); the revert map is the mitigation, and an
  unexpected-BOP path logs any surprise.
- **New INT 31h services** (no V86 round-trip, so buildable/reasoned off-VM like XMS/EMS bookkeeping):
  `0100`/`0101` DOS-memory alloc/free (routed to `dos_alloc`/`dos_free`; returns real seg + a based
  selector — the transfer buffer real extenders need for `0300`); `0204`/`0205` get/set **PM interrupt
  vector** (a `g_pm_int[256]` table, so a client's save/set/restore round-trips); `0900`/`0901`/`0902`
  get-disable / get-enable / get **virtual interrupt state** (`g_dpmi_vi`).
- **Client extended** (`dpmitest.asm`): exercises `0100`, `0205`+`0204` round-trip, `0900`/`0901`
  before the existing `0300` flow. These INT 31h sites sit **past the old 0x2000 bound**, so a clean
  pass also proves the full-64K scan caught them.
- **Deferred** (need a V86 round-trip, next increment): `0301` real-mode far-call, `0303` real-mode
  callback, and routing more INT 21h subfunctions through `0300`. The `default` case logs the AX of
  any unsupported INT 31h, so the first run of a real binary pinpoints the next gap.

**VM-CONFIRMED (2026-08-02, headless `dpmiauto` autorun → `vm/serial.log`):**
- **run 42** (`build dpmi-harness-v32`): `DPMI: patched 0xF INT sites -> BOP (full 64K scan, last off
  0x241)`; `0400 -> ver 0.90`; `0205 setPMvec int 1C = 0F:0199` + `0204 getPMvec` round-trip;
  `0900 -> cli` / `0901 -> sti`; `0300 -> simInt 0x21` prints; clean `4Ch` exit (sentinel 0x5A). One
  expected wrinkle: `0100 -> DOSmem ENOMEM max=0` — **correct DOS semantics**, a `.COM` owns all
  conventional memory on load, so nothing is free to allocate (the service correctly delegated to
  `dos_alloc`). Not a host bug.
- **run 43** (`build dpmi-harness-v33`): added a real-mode `INT 21h AH=4A` shrink to 64 KB in the
  client BEFORE the switch, freeing conventional memory. `0100` now SUCCEEDS: `-> DOSmem seg=0x1101
  sel=0x1F` (real segment + a based selector). 16 INT sites patched (the extra `4A` site included),
  last off 0x248 — full-64K scan proven again. All other services + clean exit unchanged.

Gate recipe (rig = [[vdm-host-test-harness]]): `scripts/build-test-iso.sh` → `qmp.py cd
/tmp/ntvdmex-test.iso` → (headless) `qmp.py cmd '{"execute":"system_reset"}'`, or (manual) Explorer
**F5** + run `D:dpmitest.bat` → read `vm/serial.log`. NB: `system_reset` gives a slow dirty boot
(~3–4 min to autologin) before `dpmiauto` fires; wait it out rather than assuming a hang.

### VM run 44 (2026-08-02) — the PM→V86→PM ROUND-TRIP works: INT 31h 0301 real-mode far-call `[FACT, real CPU]`

Until now the host only ever ran the guest in ONE mode per phase (V86 for DOS, then PM for the DPMI
client); `0300` faked a real-mode INT by calling `dos_int21` host-side without ever leaving PM. `0301`
(call real-mode procedure) needs the real thing: **run the client's real-mode code in V86 in the
middle of a PM session, then come back to PM.** It works, first try, on the real CPU.

Mechanism (`src/host/main.c`, the `0301` case): from the DPMI PM loop (where the guest is stopped at a
BOP and we're back in host code), save the full PM register file + the virtual MSW; rewrite the TIB
CONTEXT to V86 (EFLAGS.VM=1, CS:IP/SS:SP/DS/ES and the register file loaded from the RMCS at ES:DI);
**clear the MSW PE bit** (the symmetric inverse of `dpmi_switch_to_pm`, which SETs it) so the monitor
runs V86; push a far-return frame pointing at a planted **return-BOP catcher** (`DPMI_RMRET_OFF` in
`DOS_HDLR_SEG`, `C4 C4 0x54`); then loop on `v86_run()` exactly as the main DOS loop does. When the
proc `RETF`s it lands on the catcher → `v86_run` stops with `EVENT_INFO=0x54` → we copy the V86
register file back into the RMCS, restore the PM register file, **re-set the MSW PE bit**, and fall
through to the normal `EIP += 2` so the PM client resumes right after its `INT 31h`.

VM-confirmed (`build dpmi-harness-v34`): `callRM 0x0100:0x01f1 SS:SP=0x0100:0xfc00` → `RM proc returned
after 0 steps (OK)` (one `v86_run` — the proc wrote its sentinel and `RETF`'d to the catcher in a
single excursion) → the client read `0xBEEF` back through its PM data selector and printed **`DPMI:
0301 real-mode far-call OK (sentinel BEEF)!`** → clean exit (10 services, sentinel `0x5A`). So V86
guest code genuinely executed and its memory writes are visible to the PM client — the round-trip is
real both ways. 19 INT sites patched (full-64K), `0100` DOS-mem alloc still succeeds.

Key facts banked: (1) `v86_run`/`VdmStartExecution` runs V86 cleanly **mid-PM-session** — no monitor
state confusion from toggling PE + VM. (2) The process LDT registered by `dpmi_switch_to_pm` persists
across the V86 excursion, so returning to PM needs only PE re-set + the PM CONTEXT restored (no
re-`svc 10/11`). (3) A far-return BOP catcher is a clean, reusable "real-mode call finished" signal.

Known limitation (next increment): the up-front patch scan rewrites `CD 21`/`CD 31` → `C4 C4` across
the WHOLE 64K segment, which is shared between the PM and V86 views — so an `INT 21h` *inside* a `0301`
real-mode proc is a corrupted `C4 C4 <next>` BOP, not a clean `CD 21`. The test proc deliberately makes
no INT. Real extenders' real-mode helpers that DO call INT 21h will need either a scan that skips known
real-mode-only regions, or lazy per-view patching. `0303` (real-mode callback) reuses this same
round-trip inverted (plant a real-mode BOP entry; on a guest far-call to it, switch to PM and invoke
the client's PM handler with the RM regs in an RMCS) — now unblocked.

### VM run 45 (2026-08-02) — a real-mode INT 21h INSIDE a 0301 proc now works (un/re-patch the shared segment) `[FACT, real CPU]`

Closes run 44's known limitation. The switch-time scan rewrites every `CD 31`/`CD 21` in the code
segment → `C4 C4` so PM software-ints reflect to us, but that segment is ALSO the V86 view, so a
real-mode INT inside a `0301` proc was hitting a corrupted BOP. Fix: `g_int_vec[]` is already the
revert map (run 42), so `dpmi_unpatch()` restores the real `CD nn` bytes right before the V86
excursion and `dpmi_repatch()` re-arms the BOPs right after — during the excursion the proc's
`CD 21` vectors natively through the IVT to our INT 21h BOP stub and is serviced by the SAME
`dos_int21` the main DOS loop uses; the PM client only ever sees the patched segment.

VM-confirmed (`build v35`): the `0301` test proc now does `AH=02` (`print 'R'`) before its `RETF`.
`0301 -> callRM ...` → **`RM proc returned after 1 steps (OK)`** — the step count went 0→1 vs run 44
precisely because one BOP (the real-mode `INT 21h`) was serviced during the excursion — then the
client read `0xBEEF` back and printed `0301 real-mode far-call OK` and exited cleanly (sentinel `0x5A`,
20 sites patched). So a `0301` real-mode proc can now call DOS, which real extenders' RM helpers need.

Scope note: this un/re-patches only the client code segment (`g_dpmi_code_base`); a real extender's RM
helper living in separately-allocated conventional memory was never scanned/patched, so its INT sites
already worked. The window where the segment is un-patched is exactly the V86 excursion, which is
correct (V86 wants clean `CD nn`). Cost = two O(64K) byte scans per `0301` call — negligible.

### VM run 46 (2026-08-02) — the ROUND-TRIP INVERTED: INT 31h 0303 real-mode callback (V86→PM→V86) `[FACT, real CPU]`

`0301` proved PM→V86→PM; `0303` (allocate real-mode callback) proves the other direction —
**real-mode code far-calls into a protected-mode handler and back** — nested inside a `0301`
excursion, so the full chain is **PM→V86→PM→V86→PM** and it works first try on the real CPU.

Mechanism: `0303` alloc records the client's PM handler (`DS:SI`) + an RMCS buffer (`ES:DI`) in a
slot and returns `CX:DX` = a planted real-mode BOP entry (`DOS_HDLR_SEG:0x60+slot*4`, `C4 C4 0x55`).
A PM-return catcher (`C4 C4 0x56` at `DOS_HDLR_SEG:0x70`) is reachable via `g_pmret_sel`, a code
selector based at `DOS_HDLR_SEG` (0x500), installed lazily on the first `0303`. When guest real-mode
code (here, a `0301` proc) far-calls the callback entry, `v86_run` stops on the BOP;
`dpmi_invoke_callback` pops the far-call return frame, fills the RMCS with the RM register file +
return `CS:IP:SS:SP`, **re-patches** the segment, sets `MSW PE`, builds a PM stack with an `IRET`
frame pointing at the PM-return catcher, and runs the handler via `dpmi_enter_pm` until it `IRET`s onto
the catcher — then reads the RMCS back, **un-patches**, clears PE, and resumes V86 at the RMCS `CS:IP`
(the far-call return). The `0301` nested loop then continues the RM proc to its `RETF`.

VM-confirmed (`build v36`): `0303 -> cb slot 0 = 0x0050:0x0060 handler 0x0F:0x0271` → inside a `0301`
excursion `0303-cb slot 0 ret=0x0100:0x0270 -> PM handler 0x0F:0x0271` → `PM handler returned (OK)` →
`0301 -> RM proc returned after 1 steps (OK)` → the PM client read the handler's write back and printed
**`DPMI: 0303 real-mode callback OK (handler wrote CAFE)!`** → clean exit (13 services, sentinel `0x5A`,
24 sites patched). So a `0xCAFE` written by a PM callback that was invoked *from V86 in the middle of a
0301 call* is visible to the outer PM client — bidirectional mode transitions compose to arbitrary
depth.

Banked: `dpmi_enter_pm` is safe to call fresh from inside a `0301` excursion (the outer PM loop's
`dpmi_enter_pm` already returned, so it's not truly nested at the C level); a `handler that itself does
INT 31h/21h` would need the full PM dispatch (the spike handler only writes memory + `IRET`, so the
callback PM loop just watches for the return catcher). Together with 0300/0301, the real/protected-mode
bridge a DOS extender needs is now complete.

### VM run 47 (2026-08-02) — the PM interrupt dispatch is now REUSABLE: a 0303 callback handler that itself calls INT 31h/21h `[FACT, real CPU]`

Removes run 46's known limitation. The ~420-line INT 31h `switch(ax)` + PM INT 21h dispatch that lived
inline in the main PM loop is factored into **`dpmi_service_pm_int(m, tib, vec, steps)`** (returns 1 =
serviced, 0 = client `4Ch` exit, −1 = unexpected). The main PM loop now calls it, and — the point —
`dpmi_invoke_callback`'s PM handler loop routes any patched-`INT` BOP the handler raises through the
*same* function instead of bailing. So a real-mode callback whose PM handler does DOS output, allocates
descriptors, or sim-real-mode-ints is fully serviced, at arbitrary nesting depth (a callback can now
itself do `0301`/`0303`). The `mp`→`#define m (*mp)` alias keeps the moved body byte-for-byte the
original; the only edits were the outer-loop `continue`/`break` → `return 1/0/-1` transfers.

VM-confirmed (`build v37`): the spike client's `0303` PM handler now issues `INT 31h 0400` + `INT 21h
AH=09` before writing `0xCAFE`. Serial shows — *inside* the `0303-cb ... PM handler` stanza — `INT31h
AX=0x0400 -> ver 0.90` and `INT21h AH=09 print: "  [0303 handler ran nested INT 31h + INT 21h]"`, then
`PM handler returned (OK)`, and the outer client prints **`DPMI: nested INT 31h inside callback OK (ver
005A)!`** → clean exit (14 services, 28 sites patched — the 4 extra are the handler's own INT sites).
The nested INT is serviced by the identical code path as a top-level one, so the whole INT 31h/21h
surface is available to every handler a real extender installs (timer ticks, exception handlers, etc.).

### VM run 48 (2026-08-02) — a REAL multi-segment MZ .EXE (CS!=DS!=SS) loads, switches, and runs INT 31h/21h in PM `[FACT, real CPU]`

The whole spike so far used `dpmitest.com` — one 64K segment, `CS=DS=SS=PSP`, so the switch could get
away with a single based selector reused for data and stack. Run 48 proves the real target shape: a
genuine **MZ `.EXE` with three distinct segments** (`tools/dostest/dpmiexe.asm`, hand-built with NASM
`-f bin`: MZ header + a relocation on the `mov ax,DATA_SEG` immediate that loads DS). It exercises
`dos_load`'s `.EXE` path (header parse, relocation fixup, `CS!=PSP` entry) end-to-end, then the switch.

Two host changes made it work:
1. **`dpmi_switch_to_pm` builds THREE selectors** — code (`0x0F` @ `retcs<<4`), data (`0x17` @ **`ds<<4`**,
   was wrongly `ss<<4`), stack (`0x1F` @ `ss<<4`, a NEW dedicated selector). It registers a 4-entry LDT
   (svc 11) + sets all three via svc 10, then loads `CS=0x0F, DS=ES=FS=GS=0x17, SS=0x1F`. The three bases
   are published in `g_dpmi_seg_base[]`.
2. **`dpmi_sel_base()` is now per-selector uniform** — the host records the switch's code/data/stack
   bases into `g_ldt[1..3]` (client allocations move to index 4+), so a DS:/ES:/SS: buffer in any INT
   31h/21h service translates through the *right* base. For a `.COM` all three are equal (verified: run
   still green, `segbase C=D=S=0x1000`); for the `.EXE` they diverge.

The client's DATA references are emitted DATA-segment-relative (`sym - DATA`) — the segmented-addressing
discipline a real linker enforces; the first attempt (flat image-relative offsets) printed string tails
through the wrong DS base and was the giveaway that DS was correctly *distinct* from CS.

VM-confirmed (`build v40`): serial shows `retcs=0x110`, **`segbase C=0x1100 D=0x1170 S=0x12d0`** (three
different paragraphs) → `PM ok (CS=0x0F:0x2a)` → `INT31h AX=0x0400 -> ver 0.90` → `INT21h AH=09 print:
"  [PM INT 21h AH=09 printed via the DATA selector 0x17]"` → **`INT21h AH=09 print: "DPMI .EXE: PROTECTED
MODE OK -- INT 31h ver 005A, DS-relative print!"`** → clean `4Ch` exit. So an unmodified-shape 16-bit
protected-mode `.EXE` boots into PM under the host and its DS-relative DOS output resolves correctly.
(The `.COM`-specific `0x1600` sentinel probe in the `4Ch` log still prints `MISMATCH` for an `.EXE` — a
cosmetic false flag; the client's own `PROTECTED MODE OK` print is authoritative.)

**Run 49 (same session) extends `dpmiexe.asm` to do `INT 31h 0300` (simulate real-mode INT) from the
`.EXE`** — the DOS-services-from-PM path a real extender lives on. The catch a multi-segment `.EXE`
hits: the RMCS carries real-mode *segments*, but in PM `SEG data` is a selector, not a paragraph. The
client solves it the proper DPMI way — `INT 31h 0006` (get base of the DS selector `0x17`) → shift the
base to a paragraph → write it into `RMCS.DS`. VM-confirmed (`build v41`): `INT31h AX=0x0006 -> base
0x11a0` → `INT31h AX=0x0300 -> simInt 0x21` → the client resumes in PM and prints **`DPMI .EXE: 0300
simulate-real-mode-int OK!`** → clean `4Ch`. So an `.EXE` can round-trip DOS calls through PM with its
data addressed correctly.

### VM run 50 (2026-08-02) — a genuine THIRD-PARTY 16-bit DPMI binary runs unmodified `[FACT, real CPU]`

The milestone for GH #2: not our own client, but **`DPMIBACK.COM`** — authored by *Japheth* (Baron von
Riedesel, author of HX/HDPMI and JWasm), sourced verbatim from the bttr-software DPMI tutorial and
assembled with **his own assembler, JWasm** (built from source on the host: `Baron-von-Riedesel/JWasm`,
one `<malloc.h>`→`<stdlib.h>` macOS patch + a manual link). So it is written to the *published DPMI 0.9
spec*, not to our host's quirks. It exercises the init protocol our hand-written clients cheated on:
`AH=4Ah` shrink computed from `SP`, the `SI` host-data-paragraph path (skipped here since our `1687`
returns `SI=0`), **`BX` preserved across the mode switch** (holds the real-mode `CS`), and — the real
stress — an **RMCS built on the STACK** passed as `ES:DI = SS:BP`, driven through `INT 31h 0301` to
switch back to real mode. That last part only works because run 48 gave `SS` its own selector (`0x1F`)
and made `dpmi_sel_base()` per-selector: the host resolves the stack-based RMCS through the *stack*
base, which the pre-run-48 host (which forced `ES=0x17`/one data base) could not.

VM-confirmed (`build v43`): `PM ok` → 27× `AH=02` (`welcome in protected-mode`) → `INT31h AX=0x0301 ->
callRM 0x100:0x169 SS:SP=0x100:0xffaa` (the stack RMCS read back correctly) → the real-mode proc prints
`back in real-mode` during the V86 excursion → clean `4Ch` (28 services). The DOS-output flush is now
mirrored to COM1 (a one-line diagnostic add) so the headless log shows both strings. **An unmodified,
third-party, spec-written 16-bit DPMI client boots into PM under the host, round-trips PM→V86→PM, and
exits — no host logic change was needed beyond runs 47–49.** Next: the HX author's own 16-bit DPMI
regression `.EXE`s (`Src/HDPMI/Regress16/`: `I310102` resize-DOS-block, `mouevnt`, `RAWJMP7`) to surface
the `default: UNSUP AX=` gaps a fuller extender hits.

### VM run 51 (2026-08-02) — surface hardening (0102 + initial-selector setters), and the C-runtime WALL `[FACT, real CPU]`

Added `INT 31h 0102` (resize DOS memory block → `dos_resize`) and made `0007/0008/0009` (set
base/limit/access) apply to `idx>=1` — since run 48 records the switch's code/data/stack selectors
(`0x0F/0x17/0x1F`) in `g_ldt[1..3]`, a client that narrows/retypes its INITIAL selectors must take
effect (before, those calls silently no-op'd on idx<3). BX is now logged for all get/set-descriptor
services. Third-party regression: **`DPMIBACK` still passes green (`build v45`)** — the common path is
intact.

**The wall (honest):** Japheth's C-compiled `I310102.EXE` (tests `0102`) does NOT pass. Its C runtime,
right after PM entry, reconfigures the selectors we handed it — `setlimit 0x17=0x25cf` (narrow DS), then
`setbase/setlimit/setaccess` on `0x1F` (our SS) to base `0x1100`, limit `0x25cf`, **access `0xFB` (a
CODE type)** — and then **hangs on the next, non-INT instruction**. That is the unsolved problem, not a
missing service: a plain-instruction `#GP` in PM is NOT reflected to the host (only patched `CD nn`
BOPs are — runs 20–34). `DPMIBACK` works precisely because it touches only patched INTs + `0301`; a C
runtime that manipulates descriptors and then executes through them faults on an instruction we can't
see. So the next frontier for *C-runtime / full-extender* binaries is the **native PM-fault reflect**
(or an equivalent: single-step/emulate PM until the next INT), a separate deep spike — the INT-31h
service surface is no longer the bottleneck. `mouevnt`/`RAWJMP7`/`I310102a` are expected to hit the
same wall and are parked behind it.

### VM run 52 (2026-08-04) — pinpoint the C-runtime hang: it BLOCKS, it does not spin `[FACT, real CPU]`

The cheap diagnostic the run-51 hand-off asked for, to decide the big spike before committing to it.
Instrumented the DPMI PM loop with run-52 telemetry (`src/host/main.c`): a host-loop heartbeat
(`g_dpmi_iter`, bumped *before* each `dpmi_enter_pm`), the guest CS:EIP handed to the last entry, the
last event/vector serviced, and two VEH fire-counters (`g_veh_any`/`g_veh_fatal` — whether ANY PM fault
reaches us via SEH, and whether it takes the non-reflect fatal path). The watchdog was rewritten to
sample these every 250 ms and dump the guest bytes at a frozen wedge point. Build `dpmi-harness-v46`,
`I310102.EXE` staged as `dpmitest.com`, headless serial round.

**Result — the hang is reproduced and classified.** The log dead-stops at exactly the run-51 point:
```
INT31h AX=0009 CX=00fb sel 0x1f -> setaccess 0x0fb   ← last thing serviced
<frozen at 894 bytes, forever>
```
The PM loop iterated once more, called `dpmi_enter_pm`, and **never returned**. Three independent
channels converge:

| Channel | Observation | Rules out |
|---|---|---|
| Host-loop heartbeat | stopped cycling — no repeated INT servicing after `setaccess` | **(b) busy-poll** on a missing service |
| **QEMU vCPU (external `ps`)** | **0 % CPU, guest TIME frozen** | a **CPU spin** — the guest is *blocked*, not burning cycles |
| VEH counters / `DPMI FATAL` | never fired (no dump, no clean `ExitProcess(0xDE0)`) | the fault being **catchable via SEH** — the kernel did NOT deliver it to us |
| Watchdog `wd[]` samples | **zero** — even the watchdog thread never ran → vCPU idle | anything but a **process-wide kernel wedge** |

**Verdict: the deep PM-fault wall (case a) — but re-characterized.** Run 51 assumed "the guest
*spins*." It does **not**. The vCPU goes fully **idle** with **no thread runnable**, which means the
whole ntvdm process is **blocked in the kernel**: the kernel takes the plain-instruction PM `#GP` (the
next stack touch after the C runtime makes `SS`=`0x1F`'s descriptor code-typed `0xFB`), routes it into
its VDM-fault path, and **deadlocks** there because our host never registered the DPMI fault handler it
expects (`VDM_TIB+0x634` / `0x4f67f8`, runs 20–34). Not a cheap missing service, not a self-inflicted
`jmp $` regression — the genuine wall, and a *block*, not a *skip-resume spin*.

**Consequence for the fix (sharpens the choice).** Because the kernel *deadlocks* on the fault (rather
than skip-resuming past it), the only reliable sidestep is to **never hand the faulting instruction to
the kernel's VDM-fault path** — i.e. run PM under our own single-step/emulate loop (extend `host_interp`,
already used for the VGA mode-12h I/O traps) and detect/service the `#GP` ourselves. The alternative —
RE how ntvdm registers so `KiTrap0D` reflects PM faults as a VDM event — is the "proper" fix but is
exactly what has been unsolved across runs 20–34. **Emulation path recommended; opening as the next
spike (run 53+).**

*Tool caveat recorded:* the watchdog cannot sample a process-wide kernel wedge on `-smp 1` (no guest
thread is runnable, so the watchdog thread never gets the vCPU). The **external QEMU-CPU check** (`ps`
%CPU + guest TIME advancing) did the real discrimination and should be the primary hang-classifier for
future runs.

### VM run 53 (2026-08-04) — the emulation path WORKS: host-interpreted PM bypasses the wall `[FACT, real CPU]`

The fix run 52 pointed to. Instead of letting the kernel execute risky PM code (it *deadlocks* on a PM
`#GP`), run 16-bit PM in the existing `v86interp.h` core — the same bounded 8086 interpreter proven on
the mode-12h fill loops. **One enabling change:** every segment→linear site in the interpreter now routes
through a `seg_base()` resolver (`src/host/v86interp.h`). V86 keeps `seg<<4` (the `g_seg2lin` hook is
NULL → the QuickBASIC path is byte-for-byte unchanged); PM sets the hook to `dpmi_sel_base()`, so the
interpreter walks PM code with LDT descriptor bases. New driver `dpmi_run_pm_interp()` (`src/host/main.c`,
gated behind `g_dpmi_use_interp`): loads the guest PM regs into an `icpu`, runs `istep()`, and — since
the interpreter has no `INT` handler — stops cleanly on a raw `CD nn`, syncs the `icpu` into the
`VDM_TIB`, services through the **same `dpmi_service_pm_int()`** the kernel path uses, reloads, and
continues. No BOP patch needed. Any *other* unmodeled opcode is logged with its bytes as the spike's
to-do signal.

**Result (build `dpmi-harness-v47`, `I310102.EXE`) — both bets pay off:**
```
DPMI-INTERP: run 53 host PM begins CS:IP=0x0f:0x83 DS=0x17 SS=0x1f
INT31h AX=0008 setlimit sel 0x17 -> 0x25cf          ← serviced via the shared dispatch, then continued
DPMI-INTERP: unmodeled opcode at CS:IP=0x0f:0xa0 bytes=66 0f b7 f6 66 c1 e6 04 (steps=0x0e)
STAGE2: complete                                     ← clean exit, veh fatal=0, NO kernel deadlock
```
1. **The `#GP` vanishes under emulation.** An interpreter enforces no descriptor type/limit, so the
   code-typed-`SS` write that `#GP`s the real CPU (run 51) simply succeeds. The interpreter sailed *past*
   the C-runtime selector reconfiguration that walled runs 51–52 — no fault, no deadlock, clean process
   exit. **The wall is bypassed for host-interpreted PM.**
2. **The bridge is proven end-to-end:** interpret PM → hit `CD 31` → service INT 31h `0008` via the
   shared dispatcher → reload → keep interpreting. LDT-base resolution works.

**Next (run 54): 32-bit operand support.** The stopping opcode is `66 0F B7 F6` = `MOVZX ESI,SI`, then
`66 C1 E6 04` = `SHL ESI,4` — the C runtime does 32-bit register math via the `0x66` operand-size prefix,
which the 16-bit-only interpreter deliberately bails on. Widening the `icpu` register file to 32-bit and
handling `0x66`-prefixed variants (`MOVZX`/`SHL` first, then grow as real binaries demand) is the bounded,
incremental next step. The emulation path is validated; opcode coverage is now the only remaining work —
exactly the tractable, in-our-control frontier run 52 predicted. `g_dpmi_use_interp` currently defaults ON;
the kernel BOP path (which keeps `DPMIBACK` green) is preserved behind it until the interpreter is at
least as capable.

### VM run 54 (2026-08-04) — 32-bit operand support: the PM interpreter runs deeper into the C runtime `[FACT, real CPU]`

The first opcode-coverage increment on the run-53 emulation path. The interpreter (`v86interp.h`) was
16-bit only (it bailed on the `0x66` operand-size prefix); a DPMI C runtime does 32-bit register math in
its 16-bit segment via `0x66`. Widened it, **test-first** (`tools/dostest/interp_test.c`, 75/75 green,
all pre-existing 16-bit checks intact):

- **Register file widened to 32-bit** (`icpu.r[]` = `uint32_t[8]`) with correct x86 **partial-register
  semantics** — a 16-bit write preserves `E-reg[31:16]`; an 8-bit write preserves the other 24 bits.
- **Width-aware helpers**: `wmask`/`wsign`, `grw`/`srw` (read/write a register by width 1/2/4),
  `rd_mem`/`wr_mem` 4-byte paths, and **64-bit-safe carry** in `do_add`/`do_sub` so 32-bit `CF` is right
  at the boundary. `do_logic`/`do_shrot` take the width mask too.
- **`0x66` now sets `W = 4`** (was a hard bail), threaded through ALU, group1 (`80/81/83`), INC/DEC,
  TEST, MOV (reg/mem + `imm32`), XCHG, PUSH/POP r32 (4-byte stack slot), shifts, LEA, moffs.
- **New `0F` two-byte map** with MOVZX/MOVSX (`0F B6/B7/BE/BF`) — the exact instruction run 53 stopped on.
- Opcodes whose 32-bit forms aren't done yet (string ops, near CALL/JMP/RET, seg PUSH/POP) **bail safely
  on `osz`** rather than mis-execute, so the DPMI interp logs them as the next to-do instead of corrupting
  state.

**Result (build `dpmi-harness-v48`, `I310102.EXE`):** the interpreter ran *past* run 53's wall — executed
the 32-bit `MOVZX ESI,SI` / `SHL ESI,4`, serviced **more** of the C-runtime selector setup (`setlimit
0x17`, `setbase 0x1f`, `setlimit 0x1f` — deeper into the reconfiguration that deadlocked the kernel in
runs 51–52), advanced `CS:IP 0xa0 -> 0xbe` (14 -> 25 steps), and exited clean (no deadlock). It now stops
on `66 0F 02 C9` = **`LAR ECX, CX`** (Load Access Rights) followed by `SHR ECX, 8` — the standard "read a
descriptor's access byte" idiom.

**Next (run 55): PM descriptor-introspection instructions.** `LAR` (and likely `LSL` load-segment-limit,
`VERR`/`VERW`) are protected-mode ops that read the descriptor named by a selector. The host already holds
that state in `g_ldt[]`; the fix is a descriptor hook (like `g_seg2lin`) that the interpreter consults to
return the access-rights byte and set `ZF`. Small, bounded — the pattern established in run 54 (add the
opcode the log names, test off-VM, VM-confirm) continues.

### VM run 55 (2026-08-04) — LAR/LSL: the interpreter runs PAST the setaccess-to-code wall `[FACT, real CPU]`

Second opcode increment. Run 54 stopped on `LAR ECX,CX` (Load Access Rights) — a protected-mode
descriptor-introspection op. Added `LAR` (`0F 02`) and `LSL` (`0F 03`) to the interpreter's `0F` map, plus
a **descriptor hook** `g_sel_desc` (mirroring `g_seg2lin`): NULL in V86 (the ops bail there), and in PM
the DPMI host sets it to `dpmi_sel_desc()`, which reads `g_ldt[]` and returns the access-rights in LAR
format (access byte at bits 8-15, G/D/AVL nibble at 20-23) + the byte-granular limit, setting `ZF` for a
populated selector. Off-VM battery: **81/81 green** (the C-runtime idiom `LAR ECX,CX; SHR ECX,8 -> CL =
access byte`, valid/invalid `ZF`, `LSL` limit).

**Result (build `dpmi-harness-v49`, `I310102.EXE`) — the run-51/52 wall is fully traversed:**
```
INT31h 0009 setaccess sel 0x1f -> 0xFA      ← the code-typing of SS that DEADLOCKED the kernel (runs 51-52)
DPMI-INTERP: unmodeled opcode at 0x0f:0xcc bytes=68 3a 02 cb ... (steps=0x1d)
STAGE2: complete                            ← clean exit, no deadlock
```
The interpreter executed `LAR`, then serviced the very `INT31h 0009` that retypes the stack selector to a
**code** type — the operation whose *next stack write* `#GP`s the real CPU and deadlocks the kernel — and
**kept running**. In emulation there is no descriptor-type enforcement, so the stack access simply
succeeds. This is the wall that defined runs 51–54, now behind us. It advanced `CS:IP 0xbe -> 0xcc` and
stopped on `0x68` = **`PUSH imm16`**, a trivial common opcode the mode-12h interpreter never needed.

**Next (run 56): `PUSH imm` (`0x68` imm16/imm32, `0x6A` imm8)** and whatever follows — ordinary
integer/stack opcodes now, not PM-specific ones. The frontier has shifted from "can we even run PM?" to
routine opcode coverage of a normal C runtime.

### VM run 56 (2026-08-04) — `PUSH imm`: the first ordinary-opcode increment `[off-VM FACT; real-CPU host healthy]`

Third opcode increment, and the first that is *not* PM-specific. Run 55 stopped on `0x68` = `PUSH imm16`
(`68 3a 02` = `PUSH 0x023A`). Added `PUSH imm` to the interpreter's main map: `0x68` pushes a W-wide
immediate (imm16, or imm32 under a `0x66` prefix — a 32-bit C-runtime arg), `0x6A` pushes an imm8
sign-extended to W. Both decrement SP by W and write the SS:SP slot through `wr_mem` (the same
descriptor-base-resolved store path as `PUSH reg`), so they work identically in V86 and PM.

**Off-VM battery: 81 → 88/88 green.** New cases exercise the exact wall bytes and the widths:
`68 3a 02` writes `0x023A` at SS:SP (SP−=2, ip+=3); `6A FF` sign-extends to `0xFFFF`; a `6A 7F; POP AX`
round-trip; and `66 68 78 56 34 12` = `PUSH 0x12345678` (SP−=4, ip+=6). This is airtight proof of the
opcode's semantics — the interpreter is unit-tested against a flat memory array, so decode, immediate
width, sign-extension, and the stack write are all checkable natively.

**Real-CPU status (build `dpmi-harness-v50`).** The v50 host was booted on the XP VM (QEMU/HVF, real
silicon) headless and **ran a full DPMI PM session through the host interpreter on the real CPU** — the
`.COM` client `dpmitest.com` executed `0400`/`0100`/`0205`/`0204`/`0300`/`0301`/`0303` (incl. nested
callbacks) with no regression from the new opcode. That confirms v50 is healthy on hardware. The
*specific* `i310102` advance past `0x0f:0xcc` was **confirmed together with run 57** (`i31run.bat`, below):
the headless multi-client autorun can't reach `i310102` (`dpmitest.com` runs first and hangs on its 2nd
`0301` real-mode call, and the self-refresh that would reorder the batch fails — a running `.bat` is
file-locked), so `i31run.bat`, a single-client CD-direct runner, was added and launched interactively.

### VM run 57 (2026-08-04) — far `RET`: PUSH imm + RETF confirmed, the far-transfer follows the LDT `[FACT, real CPU]`

`RETF` (`0xCB`) + `RETF imm16` (`0xCA`) added to the interpreter: pop the W-wide offset, then a **2-byte
selector into CS** — trivial because `seg_base` already resolves the popped selector via the LDT (the same
machinery LAR/LSL use). This is the natural companion to run 56's PUSH imm: the observed idiom is
`PUSH seg; PUSH off; RETF`, a manual far-transfer. Off-VM **88 → 94/94** (`interp_test.c` T39-T41: plain
RETF sets CS:IP + SP+=4; `RETF imm16` releases the extra bytes; and the full `PUSH 0x0800; PUSH 0x0100;
RETF` idiom transfers to `0800:0100` with SP restored).

**VM-confirmed (build `dpmi-harness-v51`, `i310102` via `i31run.bat`) — PUSH imm AND far RET both work:**
```
run 55 wall (v49):  CS:IP=0x0f:0xcc  bytes=68 3a 02 cb ...  steps=0x1d  (29)
run 57      (v51):  CS:IP=0x1f:0x8d  bytes=c9 c3 68 77 ...  steps=0x312 (786)  STAGE2: complete
```
The interpreter blew past run 55/56's `PUSH imm16` wall and **CS changed `0x0f -> 0x1f`** — the
`PUSH seg; PUSH off; RETF` far-transfer following the popped selector through the LDT into selector `0x1f`,
the very descriptor the `setaccess 0x1f -> 0xFA` code-typing (the run 51-52 kernel-deadlock wall) just
retyped to CODE. 786 interpreted steps (vs 29), a **clean exit**, and the C runtime reached `main()`-level
I/O (it printed a real DOS string, "this test may fail for 16-bit clients due to selector tiling"). Both
run 56 (PUSH imm) and run 57 (far RET) are proven on the real CPU in one shot.

### VM run 58 (2026-08-05) — `LEAVE`: the C stack-frame epilogue `[FACT — VM-confirmed]`

`LEAVE` (`0xC9`) added to the interpreter: `SP = BP` (discard locals), then `BP = pop()` (restore the
caller's frame pointer). This is exactly the opcode run 57 stopped on — `c9 c3` = `LEAVE; RET near`, an
ordinary C function *epilogue*, the mirror of the `PUSH BP; MOV BP,SP` / `ENTER` prologue. 16-bit only;
the `0x66` 32-bit form (`ESP/EBP`) bails as `TODO`, matching the neighbouring stack ops. Off-VM
**94 → 98/98** (`interp_test.c` T42-T43: `SP<-BP` then pop with locals discarded and `ip+=1`; and the
E-reg high halves of SP/BP preserved under the 16-bit form).

**VM-confirmed 2026-08-05** on the real CPU via interactive `D:\i31run.bat` (host `dpmi-harness-v52`,
client `i310102`). The interpreter walked **past** run 57's `0x1f:0x8d` (`c9 c3`) epilogue wall — proving
`LEAVE` executed — and advanced **786 → 796 steps** (`steps=0x31c`), the C runtime again reaching
`main()`-level I/O (printed "this test may fail for 16-bit clients due to selector tiling") and exiting
clean (`STAGE2: complete`). The new wall is a genuinely different opcode further in:

```
DPMI-INTERP: unmodeled opcode at CS:IP=0x0000001f:0x000001b3 bytes=9c 66 52 66 52 66 50 51 (steps=0x0000031c)
```

`0x9C` = **`PUSHF`** (push the 16-bit FLAGS), heading a `PUSHF; PUSH EDX; PUSH EDX; PUSH EAX; PUSH ECX`
register-save sequence (the `66`-prefixed 32-bit pushes we already model). So run 58 is a clean, predicted
increment: `LEAVE` reached, one more opcode surfaced.

**Next (run 59): `PUSHF` (`0x9C`)** — push FLAGS onto the stack (its mate `POPF`/`0x9D` will surface right
after the matching restore). The interpreter already tracks flags in `c->flags`, so this is a plain
`push16(FLAGS)`; watch the `0x66`-prefixed 32-bit `PUSHFD` form and bail-or-model it like the other stack
ops. Read the next `DPMI-INTERP: unmodeled opcode` line off `i31run.bat` and follow it.

### VM run 59 (2026-08-05) — `PUSHF`/`POPF`: the FLAGS stack pair `[FACT — VM-confirmed]`

`PUSHF` (`0x9C`) and `POPF` (`0x9D`) added together (the natural pair; `POPF` surfaces right after the
matching restore, so bundling them mirrors runs 56/57). `PUSHF` pushes the flags we model
(`arithmetic | DF`) plus the always-set reserved bit 1; `POPF` loads back through the same mask, so the
round-trip is exact and `c->flags` never accumulates the bits we don't model (`IF`/`TF`/`IOPL`/`NT` are
dropped). 16-bit only; the `0x66` `PUSHFD`/`POPFD` 32-bit-`EFLAGS` form bails as `TODO`, matching the
neighbouring stack ops. Off-VM **98 → 106/106** (`interp_test.c` T44-T46: `PUSHF` word = modeled bits +
reserved; `POPF` restores from stack; a `PUSHF; XOR AX,AX; POPF` round-trip exact + `ESP` high half kept).

**VM-confirmed 2026-08-05** on the real CPU via interactive `D:\i31run.bat` (host `dpmi-harness-v53`,
client `i310102`). This was a **big** unblock — the interpreter walked past run 58's `0x1f:0x1b3` `PUSHF`
wall and ran a long stretch: **796 → 1308 steps** (`steps=0x51c`), and the C runtime printed *new*
`main()`-level output (`int 31h, ax=0100h, bx=0800h returned` — it now reports its DPMI DOS-mem-alloc
result via `printf`), exiting clean (`STAGE2: complete`). The new wall:

```
DPMI-INTERP: unmodeled opcode at CS:IP=0x0000001f:0x00000157 bytes=96 2b d8 80 7e fd 01 75 (steps=0x0000051c)
```

`0x96` = **`XCHG AX,SI`** (the single-byte `XCHG AX,r16` family, `0x90`–`0x97`; `0x90` is `NOP` =
`XCHG AX,AX`), here heading `XCHG AX,SI; SUB BX,AX; CMP byte [BP-3],1; JNZ …` — ordinary integer glue.

**Next (run 60): `XCHG AX,r16` (`0x90`–`0x97`)** — swap `AX` with the indexed 16-bit reg (`0x90` = NOP,
a no-op; the `0x66` form swaps `EAX`). Trivial: one `grw`/`srw` swap on `r[0]`↔`r[op&7]`. The interpreter
already has the two-operand `XCHG r/m,r` (`0x86/0x87`); this is just the accumulator short-form. Read the
next `DPMI-INTERP: unmodeled opcode` line off `i31run.bat` and follow it.

### VM run 60 (2026-08-05) — `XCHG AX,r16`: the accumulator short-form `[FACT — VM-confirmed]`

`XCHG AX,r16` (`0x91`–`0x97`; `0x90` = `XCHG AX,AX` = `NOP`, already handled) added: swap the `W`-wide view
of `AX`/`EAX` (`r[0]`) with the indexed reg, no flags. The `0x66` form swaps `EAX,r32`. Off-VM **106 →
112/112** (`interp_test.c` T47-T49: `XCHG AX,SI` swaps + leaves flags; `0x90` NOP + E-reg high halves kept
under the 16-bit form; `0x66 0x93` full `XCHG EAX,EBX`).

**VM-confirmed 2026-08-05** on the real CPU via interactive `D:\i31run.bat` (host `dpmi-harness-v54`,
client `i310102`). The interpreter walked past run 59's `0x1f:0x157` `XCHG` wall, advanced **1308 → 1476
steps** (`steps=0x5c4`), the C runtime printing yet more `main()` output (`... returned NC, eax=` — it's
mid-way through formatting a hex value) and exiting clean (`STAGE2: complete`). The new wall:

```
DPMI-INTERP: unmodeled opcode at CS:IP=0x0000001f:0x00000042 bytes=66 f7 f7 80 c2 30 80 fa (steps=0x0000005c4)
```

`0x66 F7 /6` (ModRM `0xF7` = mod 11, reg 6, rm 7) = **`DIV EDI`** — a 32-bit unsigned divide
(`EDX:EAX / EDI`), heading `DIV EDI; ADD DL,0x30; CMP DL,…` = printf's number-to-hex-string digit loop
(divide, `+'0'`, adjust). So the C runtime is now deep in output formatting.

**Next (run 61): the `F7` group (`MUL`/`IMUL`/`DIV`/`IDIV`/`NEG`/`NOT`/`TEST imm`)** — meatier than the
recent stack/glue ops: `DIV`/`IDIV` use the `EDX:EAX` (or `DX:AX`) pair and can fault `#DE` on divide-by-
zero or quotient overflow (the interpreter has no trap path — clamp/bail rather than UB). Model at least
`DIV` (reg 6) and its `MUL`/`NEG`/`NOT` neighbours; watch the `0x66` 32-bit width throughout. Read the
next `DPMI-INTERP: unmodeled opcode` line off `i31run.bat` and follow it.

### VM run 61 (2026-08-05) — the `F7`/`F6` group: MUL/IMUL/DIV/IDIV/NEG/NOT `[FACT — VM-confirmed]`

The group-3 handler (was `TEST`-only, reg 0/1) extended to the full set: `NOT` (reg 2, no flags), `NEG`
(reg 3, flags via `do_sub 0-e`), `MUL`/`IMUL` (reg 4/5 → `[E]DX:[E]AX`, `CF=OF` on overflow of the low
half), `DIV`/`IDIV` (reg 6/7 ← `[E]DX:[E]AX`, quotient→`[E]AX`, remainder→`[E]DX`). All width-aware
(`w=1/2/4`, the `0x66` 32-bit form threaded through). No `#DE` trap path, so a zero divisor or a
quotient that overflows the destination **bails to V86** (`return 0`) rather than emit UB — correct code
never hits it. Off-VM **112 → 128/128** (`interp_test.c` T50-T58: NOT/NEG(±)/MUL(±ovf)/IMUL/DIV/IDIV, the
byte `F6` DIV, the exact `66 F7 /6` DIV EDI, and the div-by-zero bail).

**VM-confirmed 2026-08-05** on the real CPU via interactive `D:\i31run.bat` (host `dpmi-harness-v55`,
client `i310102`). The `DIV EDI` (run 60's wall) is printf's hex-digit divide, and it **produced the
correct output**: the interpreter advanced **1476 → 1815 steps** (`steps=0x717`) and the DOS output line
*completed* —

```
int 31h, ax=0100h, bx=0800h returned NC, eax=36e, edx=27
```

— where `eax=36e` / `edx=27` **exactly match** the `DOSmem seg=0x36e sel=0x27` logged earlier in the same
run. So `DIV` formatted those two values to hex correctly: a self-checking confirmation, not just "ran
further". Clean exit (`STAGE2: complete`). The new wall:

```
DPMI-INTERP: unmodeled opcode at CS:IP=0x0000001f:0x000001c9 bytes=66 60 b8 00 00 b9 01 00 (steps=0x0000717)
```

`0x66 0x60` = **`PUSHAD`** (`0x60` = `PUSHA`, push all GP regs; the `0x66` form is the 32-bit `PUSHAD`),
heading `PUSHAD; MOV AX,0; MOV CX,1` — a callee saving the register file.

**Next (run 62): `PUSHA`/`POPA` (`0x60`/`0x61`)** — push/pop all eight GP registers in the canonical order
(`AX,CX,DX,BX,SP,BP,SI,DI`; `POPA` discards the pushed `SP` slot). `W`-wide (`0x66` = `PUSHAD`/`POPAD`).
Eight stack slots each; the pushed `SP` is its value *before* the push. Read the next
`DPMI-INTERP: unmodeled opcode` line off `i31run.bat` and follow it.

### VM run 62 (2026-08-05) — `PUSHA`/`POPA`, and **`i310102` RUNS TO COMPLETION** `[FACT — VM-confirmed]`

`PUSHA`/`POPA` (`0x60`/`0x61`) added: push/pop the whole GP file in canonical order
(`AX,CX,DX,BX,SP,BP,SI,DI`, indices 0–7), the pushed `SP` being its value *before* the push; `POPA`
restores `DI..AX` and *discards* the saved-`SP` slot. `W`-wide (`0x66` = `PUSHAD`/`POPAD`); the stack
offset stays 16-bit (address size). Off-VM **128 → 137/137** (`interp_test.c` T59-T61: push order + saved-
SP slot; a `PUSHA;POPA` round-trip after clobbering the file; full 32-bit `PUSHAD`/`POPAD`).

**This is the run that finishes `i310102`.** VM-confirmed 2026-08-05 on the real CPU via interactive
`D:\i31run.bat` (host `dpmi-harness-v56`): the interpreter walked past run 61's `PUSHAD` wall and the
client **hit NO further unmodeled opcode — it ran to its natural exit** (`INT 21h AH=4Ch` after `0x12f6`
services). It executed its full DPMI test sequence and printed correct, self-consistent results for every
call:

```
int 31h, ax=0100h, bx=0800h returned NC, eax=36e, edx=27      ; DOS-mem alloc -> seg 0x36e / sel 0x27
int 31h, ax=0,     cx=1  returned eax=2f                       ; alloc 1 descriptor -> sel 0x2f
int 31h, ax=0102h, bx=1800h returned NC, eax=102, edx=27, ebx=1800   ; resize DOS block -> OK
int 31h, ax=0102h, bx=-1   returned C,  eax=8,   edx=27, ebx=9c92    ; oversize resize -> fails, max 0x9c92
```

Every value cross-checks the host's own service log (`DOSmem seg=0x36e sel=0x27`, `sel 0x2f`, the two
`0102` resizes). So **Japheth's C-runtime DPMI client `i310102` (an HX Regress16 `.EXE`) now executes
end-to-end in our host PM interpreter on the real XP CPU** — allocation, descriptor management, block
resize (success + failure), formatted `printf` output, and a clean exit. The `<<< MISMATCH >>>` on the
`4Ch` line is the known cosmetic `.COM`-sentinel (`0x1600`) check that always logs `MISMATCH` for an
`.EXE` (run 48) — the client's own output is authoritative.

**Milestone:** the opcode-climb that began at run 53 (host-interpreted PM) reached its goal — a real,
unmodified, C-compiled DPMI client runs to completion through the interpreter. Runs 54–62 added exactly
the opcodes this one C runtime exercised (32-bit operands, LAR/LSL, PUSH imm, far RET, LEAVE, PUSHF/POPF,
XCHG AX,r16, the F7 mul/div group, PUSHA/POPA) — no PM-specific magic left, just ordinary integer/stack
coverage.

**Next (post-62): the driver `i310102` is exhausted — pick the next target.** Options, roughly in order:
(a) run a **larger/different client** through the interpreter to surface the next opcode gaps (e.g. the
other HX Regress16 `.EXE`s — `mouevnt`, `RAWJMP7` — or a DOS/4GW-style program, though 32-bit clients need
the separate 32-bit-PM work); (b) make `g_dpmi_use_interp` the **default** and run **DPMIBACK** (run 50's
third-party binary) through the interpreter, retiring the kernel BOP path; (c) start the **`INT 2Fh 1687`
advertisement** work now that a real binary runs clean end-to-end (still spike-branch only until broader
coverage). Whichever: drive it, read the `DPMI-INTERP: unmodeled opcode` (or clean-exit) line, follow it.

### VM run 63 (2026-08-05) — DPMIBACK runs through the INTERPRETER: the kernel path is subsumed `[FACT — VM-confirmed]`

Chose option (b): pushed **DPMIBACK** (Japheth's third-party 16-bit DPMI client, run 50) through the host
PM interpreter — which has been the default (`g_dpmi_use_interp = 1`) since run 53, so this is a pure
client swap (new one-shot runner `tools/dostest/dpbrun.bat`, `target.txt` → `dpmiback.com`; no host code
change beyond the `v57` marker). DPMIBACK is the ideal probe because it exercises the **real↔protected
round-trip** (`INT 31h 0301`) that `i310102` never touched.

**VM-confirmed 2026-08-05** on the real CPU via interactive `D:\dpbrun.bat` (host `dpmi-harness-v57`).
DPMIBACK switched to PM, ran in the interpreter (`CS=0x0f:0x132`), and executed **cleanly with zero
unmodeled opcodes** — it's hand-written asm, so it needs far fewer opcodes than the C runtime:

```
INT31h AX=0x0301 -> callRM 0x0100:0x0169 SS:SP=0x0100:0xffaa
0301 -> RM proc returned after 00000001 steps (OK)
INT21h AH=4Ch -> client EXIT after 0xba svc
  ==> DOS OUTPUT: [welcome in protected-mode / back in real-mode]
```

This is **identical to run 50's behaviour** — but run 50 went through the *kernel BOP* path, and this goes
through the *interpreter*. The `0301` PM→V86→PM excursion works because both paths call the same
`dpmi_service_pm_int` (main.c) to service it. So the interpreter now runs BOTH proven third-party clients
end-to-end: the C-runtime `i310102` (run 62) and the asm DPMIBACK (this run). The `<<< MISMATCH >>>` on
`4Ch` is again the cosmetic `.COM` `0x1600`-sentinel (`ver=0` here); the DOS output is authoritative.

**Consequence — the kernel BOP PM path is now dead code.** `g_dpmi_use_interp` has been `1` since run 53
and every proven client (i310102, DPMIBACK, dpmitest, dpmiexe) runs through the interpreter; the
`sw==0` kernel branch + `dpmi_enter_pm` are no longer exercised. They can be retired (or kept behind the
toggle as a reference) — that is option (b) *complete* in effect. Remaining: (c) the `INT 2Fh 1687`
advertisement (the host already RESPONDS to `1687` — see the `STAGE2` log — the caveat is only about
enabling it on `main`), and broader real-extender coverage (16-bit first; DOS/4GW-class 32-bit is the
separate 32-bit-PM spike).

**Next: broaden the client corpus / begin 32-bit PM.** The two easy proven clients run; the interpreter
is the single path. Concrete next targets: the remaining HX Regress16 16-bit `.EXE`s (`mouevnt`,
`RAWJMP7`) to shake out more opcodes, then a real 16-bit DOS extender; separately, the 32-bit-PM work
(32-bit `CS`/flat selectors, `NtVdmControl` 32-bit execution) that DOS/4GW-class extenders need.

### VM run 64 (2026-08-05) — `RAWJMP7` probe: finds the next gap = INT 31h `0305`/`0306` (raw mode switch) `[FACT — VM-confirmed]`

Ran a THIRD HX Regress16 client, **`RAWJMP7.EXE`** (copied to `tools/dostest/rawjmp7.exe`, runner
`rjrun.bat`, host `dpmi-harness-v58`) — a gap-finding probe, not an opcode run. RAWJMP7 exercises the DPMI
**raw mode-switch** path (its name = "raw jump"). VM result (interactive `D:\rjrun.bat`):

```
STAGE3: ... -> PM ok (CS=0x0f:0x395) -> DPMI PM loop       ; switch + interpreter entry OK
INT31h AX=0x0305 CX=0x69 -> UNSUP                          ; Get State Save/Restore addresses -- NOT IMPLEMENTED
INT31h AX=0x0306 CX=0x69 -> UNSUP                          ; Get Raw Mode Switch addresses    -- NOT IMPLEMENTED
INT31h AX=0x0301 -> callRM 0x110:0x214 SS:SP=0x110:0xff00  ; ...then a 0301 that never returns
```

So RAWJMP7 switches to PM and runs in the interpreter fine (zero unmodeled opcodes — like DPMIBACK, it's
asm), but it asks for **`INT 31h 0305` (Get State Save/Restore Addresses)** and **`0306` (Get Raw Mode
Switch Addresses)**, which the host returns UNSUP for; its subsequent `0301` real-mode call then never
returns (the RM proc at `0x110:0x214` depends on the raw-switch setup it couldn't get, so the excursion
stalls and the ~6 s watchdog kills the host — no `STAGE2: complete`).

**This is the concrete next capability gap, and it is exactly the real-extender path.** `0305`/`0306` are
the "raw mode switch" mechanism: instead of trapping every transition through `INT 31h`, the host hands the
client two callable entry points (real→protected and protected→real) plus state save/restore routines, and
the client far-calls them to switch modes itself. Real DOS extenders (CWSDPMI-style, and the DPMI-aware C
runtimes that prefer raw switching for speed) use this. Implementing it is a meaty feature — callable
mode-switch stubs on both sides, register-block conventions — comparable to the original V86↔PM switch
work, **not** an opcode increment.

**Next (run 65+): implement `INT 31h 0306` (raw mode switch addrs) + `0305` (state save/restore addrs).**
`0306` returns `BX:CX` = real→protected entry, `SI:DI` = protected→real entry; the client loads a register
block and far-calls the entry to switch. `0305` returns the state-save size + save/restore call addresses
(a no-op save is often acceptable for a cooperative host). Wire them into `dpmi_service_pm_int`, provide
the two switch stubs (reuse the proven `dpmi_switch_to_pm` / the `0301` PM→V86 machinery), then re-run
`D:\rjrun.bat` and follow the log. This unblocks the raw-switch extender class.

### Run 65 (2026-08-05) — GH #18: the `+0x638` real-CPU PM-fault trampoline (IMPLEMENTED; VM-CONFIRM PENDING) `[IMPL — needs the interactive VM gate]`

Strategic pivot (see [[playable-games-direction]]): the interpreter (runs 53–64) is the **fallback**; the
**mainline is running protected mode on the REAL CPU like ntvdm**. Kernel RE session 7 returned **GO** on
32-bit reachability (XP caps flat selectors to ~2GB but stock ntvdm shares that cap, so DOS/4GW is reachable
to ntvdm parity), so #18's PM-fault reflect is worth building for the whole game class. Run 65 **implements**
the `+0x638` PM-fault trampoline per Kernel RE session 6's actionable spec — host build `dpmi-harness-v59`,
probe `tools/dostest/pmfault.com`, runner `pfrun.bat`.

**What the reflect chain does (re-confirmed by disassembling `ntoskrnl 0x4f6e6f` this session).** The caller
passes `edi = VDM_TIB+0x634`; when the nesting counter (`+0x634`, word) is 0 it saves the interrupted context
and redirects execution:
```
edi+0 (TIB+0x634) nest counter : cmp==0 -> first level; inc'd afterwards
edi+4 (TIB+0x638) handler sel  : movzx eax,word -> installed as the faulting context's new CS
edi+6 (TIB+0x63a) saved CS     : <- word[trapctx+0]
edi+8 (TIB+0x63c) saved EIP    : <- dword[trapctx+4]
edi+c (TIB+0x640) saved slot3  : <- dword[trapctx+0x10]
then: trapctx.CS = [TIB+0x638] ; trapctx.EIP = 0x1000    (mov [esi+4],0x1000)
```
So the kernel jumps the faulting guest to **`selector([TIB+0x638]) : 0x1000`**. (The only `return 0` paths in
`0x4f6e6f` are the SEH unwind if `+0x634`/the trap-context is unwritable, and a `je` on `call 0x4f6dc0`'s
result — a residual gate to watch if the reflect still doesn't fire.)

**The implementation (`src/`, all cross-compiles clean, KERNEL32-only):**
- `ntvdm.h`: named the block `VTIB_FLT_NEST/FLAG/HSEL(0x634/636/638)` + `SAVCS/SAVEIP/SAV3(0x63a/63c/640)`.
- `main.c`: `dpmi_install_fault_trampoline()` allocates a g_ldt[] **code selector H** based at `DOS_HDLR_SEG<<4`
  (0x500), limit 0xFFFF, access 0xFA; a **BOP `C4 C4 57`** is planted at offset `0x1000` → linear **0x1500**
  (mapped V86 window, in the unused gap below env seg 0x600 / PSP 0x10000). `dpmi_arm_fault_trampoline()`
  writes `[TIB+0x634]=0 / [TIB+0x636]=flag / [TIB+0x638]=H` and is called **before every `dpmi_enter_pm`**
  (main PM loop + the 0303 callback loop) — the kernel inc's the nest counter, so it must be re-zeroed each
  entry. The main PM loop now detects the reflected fault (`event==4 && CS==H && EIP==0x1000`), reads the saved
  fault `CS:EIP` from `+0x63a/+0x63c` (+ slot3 from `+0x640`), and logs `GH#18: PM-FAULT REFLECTED`. The
  INT→BOP scan stays the fast path; the trampoline is the catch-all for raw (non-BOP) `#GP`s.
- `g_dpmi_use_interp` flipped to **0** for this build so the real-CPU kernel path runs (flip back to 1 to
  restore the VM-confirmed interpreter runs; the interpreter code is untouched).
- Probe `pmfault.asm`: detect DPMI → far-call switch → print a PM marker (patched INT 21h BOP) → execute
  **`HLT`** (privileged at CPL3 ⇒ raw `#GP(0)`, not a BOP, not a scanned INT) to fire the trampoline.

**VM-confirm (interactive, PENDING):** boot the VM with a display (`./scripts/xp-vm.sh run`),
`qmp.py cd /tmp/ntvdmex-test.iso`, double-click **`D:\pfrun.bat`**, read `vm/serial.log`.
- **Success** = the log reaches `PMFAULT: in PROTECTED MODE -- about to HLT` **and then** `GH#18: PM-FAULT
  REFLECTED to trampoline -- saved CS:EIP=... -- REAL-CPU PM #GP reflect WORKS`. That proves the kernel
  reflects a fault it silently swallowed in runs 20–34 — the keystone for real-CPU PM.
- **Failure** = the log stops after `about to HLT` (the VDM silently terminates, as in runs 20–34) → the
  reflect is still gated; next lead is `0x4f6dc0`'s predicate and the classifier bits (`0x714` 3/4/14) the
  monitor sets at init. Either outcome is a clean, decisive signal.
Servicing a *real* fault (emulate/skip the faulting instruction, then resume at the saved `CS:EIP`) is the
follow-on once the reflect is confirmed firing.

**VM RESULT (2026-08-05, interactive `D:\pfrun.bat`, host `dpmi-harness-v59`) — the reflect did NOT fire; a
clean, decisive NEGATIVE.** Full serial log:
```
STAGE0: WinMain entered [build dpmi-harness-v59]
STAGE2: DPMI 1687 -> AX=0 ES:DI=0x50:0x50
STAGE3: DPMI_BOP far-call LANDED @ 0x50:0x50 -- switching to PM
 [svc11=0 svc10=0] retcs=0x100 clo=0x1000ffff chi=0xfa00 segbase C=D=S=0x1000 -> PM ok (CS=0x0f:0x12c)
DPMI: patched 9 INT sites -> BOP (full 64K scan, last off 0x14a)
DPMI: PM-fault trampoline H=0x27 base=0x500 bop@0x1500          <- install OK
INT21h AH=09 print: "PMFAULT: in PROTECTED MODE -- about to HLT (raw #GP)..."   <- guest ran IN PM, printed
<log ends here -- no "PM-FAULT REFLECTED"; VDM silently gone>
```
**Interpretation — the wall is now precisely bracketed.** The trampoline installed correctly and the guest
demonstrably executed in protected mode (its *patched* `INT 21h` reflected as `event=4` and printed). So the
kernel's **C4 C4 BOP reflect (`0x565041`) works**, but the **raw (non-BOP) `#GP` reflect does not** — *even
with `+0x638` armed to a valid code selector whose `:0x1000` holds a live BOP*. Per session 6's reconciliation
of run 31 (setting `+0x638` made the reflect *proceed* to `sel:0x1000`), had the fault reached `0x4f6e6f` it
would have reflected here. It did not. **⇒ the HLT `#GP` is rejected UPSTREAM of the `+0x638` writer** —
inside `0x4f67f8` (the non-BOP fault reflect; top-gate `EPROCESS.VdmObjects` we pass, then the `[0x714]`
classifier on bits 3/4/14) or `0x4f6f67` — an early `return 0` on VDM state our host has not established. This
**confirms runs 31–34's localization on the real CPU** and proves the `+0x638` handler plumbing, while
necessary, is **not sufficient** by itself.

**NEXT (run 66) — find the upstream reject, statically.** Disassemble `0x4f67f8` and `0x4f6f67` in full (not
just the entry gate): identify every `return 0` predicate between the `VdmObjects` gate and the
`lea edi,[TIB+0x634]; call 0x4f6e6f` call, and which `[0x714]` classifier bits (3/4/14, `test eax,8/0x10/
0x4000`) or VDM_TIB fields each tests. Then determine which of those our `VdmInitialize`/monitor path leaves
unset that ntvdm's does set (session 6: bits 3/4/14 are written kernel-side at init or select the vector —
re-examine whether our init actually establishes them, since the live negative says something on this path is
missing). Instrument by re-running `pfrun.bat` after arming each candidate field. The `0x4f6e6f`-level
`+0x638` trampoline (run 65) stays in — it is the correct final hop; the missing piece is getting the fault
*to* it. (Keep `g_dpmi_use_interp`=0 for these probes; flip to 1 to restore the interpreter runs.)

### Run 66 (2026-08-05) — the `[0x714]` classifier-bits theory is REFUTED on the real CPU; the reject is a descriptor gate in the reflect body `[FACT — VM-confirmed + static disasm]`

Static RE of `0x4f67f8` (the non-BOP fault reflect) mapped its structure: past the `EPROCESS.VdmObjects`
gate (which we pass) it safe-reads `[0x714]` and **classifies on bits 3/4/14 against the fault vector**
(`[ebp+8]`; #GP=0xd). For a #GP, the generic reflect body `0x4f689c` — the path that reaches our `+0x638`
writer — is entered when **bit 3 is CLEAR** (bit 3 SET routes #GP to `0x4f69be`/`0x4f69dc` instead). The body
has three sequential gates, any returning 0 ⇒ terminate: `call 0x4f6d3c` (validate **CS**), `call 0x4f6dc0`
(validate **SS**), `call 0x4f6f67` (→ `0x4f6e6f`, the `+0x638` writer). `0x4f6d3c`/`0x4f6dc0` resolve the
selector via `0x45dd5f` and, in PM (EFlags.VM clear), reject unless the descriptor is present and of the right
type (`test type,4` = code for CS; `test type,2` = writable-data for SS). `0x4f6e6f` **additionally validates
the handler selector `[TIB+0x638]` via `0x4f6dc0`** before installing it as the new CS.

**Host instrumented (v60): log `[0x714]` bits 3/4/14 + force bit 3 clear before each PM entry.** VM re-run
(interactive `D:\pfrun.bat`) — the decisive line, seen in BOTH the probe and an autorun client:
```
GH#18: [0x714]=0xc0007130 bit3=0 bit4=1 bit14=1
...
INT21h AH=09 print: "PMFAULT: in PROTECTED MODE -- about to HLT (raw #GP)..."
<log ends -- still no PM-FAULT REFLECTED>
```
**⇒ bit 3 is ALREADY CLEAR (0).** Forcing it clear was a no-op and the HLT `#GP` still does not reflect.
**This REFUTES the "set/clear the `[0x714]` classifier bits" theory on the real CPU** (sessions 3/5/6's
residual worry) — definitively, with the live value logged (`0xc0007130`: bit4=1, bit14=1, bit3=0). With
bit3=0/bit4=1 a #GP (vector 0xd) provably *reaches* the generic reflect body `0x4f689c`, so the classifier is
not the blocker. **The reject is therefore one of the descriptor-validation gates in the body.** Since the
guest *already executed in PM* with CS=0x0f/SS=0x1f (it printed from PM), the CPU had already validated those
two descriptors — so gates 1/2 (CS/SS) should pass, and the **prime suspect is the handler selector `H` gate
inside `0x4f6e6f`** (`0x4f6dc0` on `[TIB+0x638]`). We set `H` to our own LDT code selector `0x27` (access
0xFA); `0x4f6dc0`'s `test type,2` gate wants writable-data, which a plain code selector fails — and per Kernel
RE session 6, **ntvdm does not pick `H` arbitrarily: its arm routine `0xf050ad7` sets `[TIB+0x638] =
word[TIB+0x36c]`**, a selector the kernel itself provides. Using our own selector is the likely mismatch.

**NEXT (run 67):** (1) read `[VDM_TIB+0x36c]` in our host after `VdmInitialize` and log it — if the kernel
populates it, use THAT as the handler selector `[TIB+0x638]` (and find where its `:0x1000` must hold the BOP,
or what the kernel expects there). (2) In parallel, statically finish `0x4f6dc0`'s exact post-`0x45dd5f`
predicate and `0x45dd5f`'s `ebx` type encoding to state precisely what descriptor `H` must be (present, DPL,
type). (3) Bisect empirically: instrument the host to report which of the three gates our CS/SS/H descriptors
would pass under a host-side replica of `0x45dd5f`'s rules. The `+0x638`/BOP machinery (run 65) is correct and
stays; run 67 is about giving `0x4f6e6f` a handler selector its `0x4f6dc0` gate accepts.

**Run 67 static prep (2026-08-05) — the reflect mechanism, corrected: run 65's design was wrong two ways
`[FACT — static disasm of `0x4f6e6f`/`0x4f6f67`/`0x4f6efd`]`.** Finishing the disasm of the reflect body
overturns the run-65 model (handler = a code selector with a BOP at :0x1000). The real mechanism:
- `0x4f6e6f` sets the interrupted context's **SS:ESP = `[TIB+0x638]`:0x1000** (it writes `[esi]=[TIB+0x638]`,
  `[esi+4]=0x1000`, saving old SS/ESP/EIP to `+0x63a/+0x63c/+0x640`) and validates `[TIB+0x638]` via
  `0x4f6dc0` = **it must be a writable-DATA selector** (matches `[TIB+0x638]=word[TIB+0x36c]`=`VTIB_ES`=0x17,
  a data selector). So `[TIB+0x638]` is the fault handler's **STACK**, NOT a code selector — run 65 made it
  code (0xFA), which `0x4f6dc0` rejects → the reflect returns 0 → terminate. **This is the concrete bug.**
- `0x4f6f67` then builds an **IRET fault frame** on that new stack (16- vs 32-bit per `[TIB+0x636]`; pushes the
  saved CS/EIP/EFlags/SS/ESP) and sets the new **CS:EIP** from a **VDM_TIB table indexed by fault class**:
  `0x4f6efd` does `[esi+0xc]=word[VDM_TIB + class*0x10]` (CS), `[esi+0x10]=dword[VDM_TIB + class*0x10 + 4]`
  (EIP), then validates `[esi+0xc]` as **code** via `0x4f6d3c` and bounds-checks EIP ≤ CS limit. So the handler
  CODE entry (where our BOP must live) comes from this per-class table in the VDM_TIB, not from `[TIB+0x638]`.
- **Corrected design for run 67:** (a) make `[TIB+0x638]` a **writable-data** selector whose base+0x1000 is a
  valid scratch stack; (b) plant our BOP (`C4 C4 nn`) at a **code** selector:offset and write that CS:EIP into
  the VDM_TIB per-class handler table at `[VDM_TIB + class*0x10]` for the #GP class; then the reflect switches
  to the scratch stack, frames the fault, and vectors CS:EIP to our BOP → `event=4` → host services. **Still
  needed:** pin the exact **fault-class index** for a PM `#GP` (the `ecx≤7` value `0x4f6f67` receives; trace
  how `0x4f67f8`'s classifier maps vector 0xd → class) and confirm the table's TIB base/stride (`class*0x10`
  from VDM_TIB, or from a sub-structure). Then rebuild and re-run `pfrun.bat`. The `+0x634` arming + BOP
  primitive stay; only the selector TYPE and the handler-CS:EIP planting change.

**Run 67 VM RESULT (2026-08-05, host v61) — the corrected machinery is right but the reflect STILL does not
fire; the wall is genuinely the "complete VDM-init state" one.** Implemented the corrected model: a
writable-data stack selector `0x2f` (base 0x2000) at `[TIB+0x638]`, a code selector `0x27` (base 0x500) with a
BOP at offset 0x80, an 8-entry class table at a host address (`0x0f026e60`) with entry 6 = `{0x27, 0x80}`, and
`[VDM_TIB+8]` = that table pointer, armed every entry. Serial:
```
DPMI: PM-fault reflect stkSel=0x2f codeSel=0x27 bop@code:0x80 tbl@0x0f026e60 class=6
GH#18: [0x714]=0xc0007130 bit3=0 bit4=1 bit14=1 tib8=0x00000000   (tib8 read is PRE-arm; arm then sets it)
INT21h AH=09 print: "PMFAULT: in PROTECTED MODE -- about to HLT (raw #GP)..."
<log ends -- no PM-FAULT REFLECTED; VDM silently gone>
```
Notably `[VDM_TIB+8]` read **0 before our arm** — i.e. the kernel did NOT populate it during VdmInitialize, so
it does expect the VDM host to set it (we do). Everything we can set from the host is set, and it still
terminates. **This confirms run 34's standing conclusion on the real CPU: the reflect is gated on VDM state
established by ntvdm's COMPLETE DPMI mode-switch init, not any single field we can name and poke.** We have now
positively mapped and satisfied: the `[0x714]` classifier (bit3=0, refuted as blocker), the `[TIB+0x638]`
stack-selector gate, the `[VDM_TIB+8]` handler-table + class-6 entry, and the `+0x634` save block — and it is
STILL not enough. The missing piece is upstream/structural, and a **silent VDM terminate gives no signal about
which gate rejects**.

**NEXT (run 68) — stop log-probing blind; make the wall OBSERVABLE via QEMU's gdbstub.** The decisive move is
kernel-side visibility: launch QEMU with `-gdb tcp::<port>` (or `-s`), attach `gdb`/`lldb`, set a hardware
breakpoint at the kernel VA `0x4f67f8` (and `0x4f6f67`/`0x4f6efd`/`0x4f6e6f`), fire `pfrun.bat`, and
**single-step the reflect to see exactly which predicate returns 0** for our HLT `#GP` — turning 15 runs of
blind field-guessing into one direct observation. (ntoskrnl is at kernel VA base `0x400000` per the r2 base;
resolve the live KPCR/`[0x714]`/VDM_TIB from the stopped context.) Alternatives if gdbstub is impractical:
(a) RE ntvdm's full `2Fh 1687` → mode-switch → DPMI-init path (`0xf02d39f` → entry, `0xf01a300`, `0xf050ad7`,
`0xf00532e`) and mirror EVERY field it writes, not a subset; (b) accept the interpreter as the 16-bit path
(it already runs i310102/DPMIBACK end-to-end) and revisit real-CPU PM once the wall is observable. The run-65…67
`+0x638`/table machinery is retained (it is provably the correct final hop); run 68 is about *seeing* the reject.

**Run 68 RESULT (2026-08-05) — HVF blocks the gdbstub; kernel single-stepping is NOT available on this VM.**
Started a gdbstub on the live QEMU via the monitor (`human-monitor-command: gdbserver tcp::1234`) — it returned
**`gdbstub: current accelerator doesn't support guest debugging`**. QEMU's gdbstub works only with TCG (or
KVM), not `-accel hvf`. So the "breakpoint `0x4f67f8` and single-step the reflect" plan is not viable while we
keep HVF (which is the whole point — real-CPU V86/PM). This is a hard tooling constraint, recorded so it isn't
re-attempted. `xp`/`x` memory reads over the monitor also returned nothing useful under HVF.

**⇒ Strategic fork for #18 (needs a call).** We have exhaustively established that the kernel PM-fault reflect
needs ntvdm's complete DPMI-init state, satisfied every host-settable field, and cannot observe the failing
gate without kernel debugging that HVF denies. The realistic options:
- **(A) TCG + gdbstub** — boot XP under software emulation (slow: 15–45 min, uncertain) to single-step the
  kernel reflect once and read the exact failing predicate. One-time but heavy; TCG executes the same kernel
  code so the finding transfers back to HVF.
- **(B) Guest KD + radare2 winkd** — enable XP kernel debugging on COM2 (`boot.ini /debug /debugport=COM2`;
  COM1 is the serial log), expose it as a socket, drive it with r2's winkd io plugin from the host. Proper,
  reusable kernel-debug capability; works alongside HVF (guest-side). Moderate–heavy setup.
- **(C) AVOID the reflect entirely (most pragmatic for the games goal).** The reflect is only needed for RAW
  faults. Most are IDENTIFIABLE by opcode and can be **pre-patched to BOPs like the INT scan already does**:
  extend the switch-time scan to `HLT`/`CLI`/`STI`/`IN`/`OUT`/`LGDT`/`LIDT`/etc. The one data-dependent class
  (the i310102 SS-retype: `INT 31h 0009` setaccess makes SS code-typed → the next stack write `#GP`s) is
  handled by **NOT applying the problematic retype to the real LDT** (keep the selector valid-data; the client
  doesn't actually need it code-typed) — mirroring the interpreter's "no enforcement" but on the real CPU. This
  keeps the hot path on the real CPU and sidesteps the intractable reflect. Likely the best route to real-CPU
  PM for the game class.
- **(D) Ship the interpreter as the 16-bit path** (already runs i310102 + DPMIBACK end-to-end) and, for 32-bit
  DOS/4GW, weigh a 32-bit interpreter extension (correct but slow for game inner loops) vs. option (C).
Recommended: **(C)** as the mainline real-CPU bridge, with **(B)** as the durable debugging capability if a
residual raw fault needs to be understood. The run-65…67 reflect machinery stays in place behind its toggle.

**Run 69 RESULT (2026-08-05, host v62) — option (C)'s targeted no-enforcement fix is NOT sufficient; i310102
still wedges on an invisible secondary fault.** Implemented the safe half of (C): `dpmi_install` now always
installs the initial DATA (idx 2/DS) and STACK (idx 3/SS) selectors as present writable-data (0xF2) on the
real LDT, no matter how the client retypes them (`g_ldt[].access` keeps the requested value for LAR/LSL). This
directly targets i310102's known wall (its C runtime does `INT 31h 0009` to set SS=0x1F access 0xFB=code). VM
(real-CPU path, `i31run.bat`, `g_dpmi_use_interp=0`): i310102 switched to PM, ran, serviced `setlimit 0x17` /
`setbase 0x1f→0x1100` / `setlimit 0x1f→0x25cf` / `setaccess 0x1f→0xFB` — then **the log stops dead at exactly
the same point as the original run-51/52 wall**, with **no watchdog `wd[]` samples** = run 52's *process-wide
kernel wedge* signature (on `-smp 1` a deadlocked-in-kernel process has no runnable thread, so the in-guest
watchdog never fires). So keeping SS valid-data did NOT prevent the fault: the client hits a raw fault right
after the setaccess anyway — candidates we cannot disambiguate blind: a subsequent DS/other retype, a
**stack-limit `#GP`** (it set SS limit `0x25cf` but its ESP may exceed it — the interpreter never enforced
limits either, so option (C) may also need "don't enforce limits" for data/stack), or a privileged op.

**Session conclusion (runs 65–69): blind fixing is exhausted.** Both real-CPU strategies — replicate the
reflect (65–67) and avoid it via no-enforcement (69) — die on an *invisible* secondary fault, and HVF denies
the gdbstub (68). Every further blind attempt costs a full ~5-min VM cycle for a silent yes/no. **The gating
capability is kernel-side visibility, i.e. option (B): guest KD.** Recommended next project: stand up XP kernel
debugging on COM2 (`boot.ini /debug /debugport=COM2 /baudrate=115200`; COM1 stays the serial log) exposed as a
socket, and drive it with radare2's winkd io plugin (`r2 winkd://...`) — then a SINGLE session reveals every
raw fault i310102/a game hits after the switch, turning runs 65–69's blind guessing into direct observation.
Until that exists, the honest 16-bit path is the interpreter (`g_dpmi_use_interp=1`, runs 53–64 — it already
runs i310102 + DPMIBACK end-to-end), and effort is better spent on the sound epic (#20/#21) or the guest-KD
capability itself. The run-65…69 real-CPU machinery (reflect + no-enforcement) stays behind `g_dpmi_use_interp`.

**Option B build (2026-08-05) — guest KD infrastructure stood up; transport + KDCOM sync WORK, r2 winkd's XP
profile layer is incomplete.** Built: `xp-vm.sh` now exposes guest COM2 as `vm/kd.sock` (a second `-serial
unix:...,server`); `tools/dostest/kddebug.bat` runs `bootcfg /raw "/debug /debugport=COM2 /baudrate=115200"
/a /id 1` (one double-click — VM-confirmed it edited boot.ini: the OS entry now shows `/debug
/debugport=com2 /baudrate=115200`); rebooted to activate. **r2 winkd results:** `r2 winkd://vm/kd.sock`
opens the pipe and prints CONNECTED but `Cannot retrieve pid from io`; **`r2 -d winkd://vm/kd.sock` gets
"Sync done! (1 cpus found)"** — so the KDCOM serial handshake over the QEMU unix socket genuinely works — but
still `Cannot retrieve pid` and `dr eip` = 0. So the transport + CPU sync are solid; r2's winkd EPROCESS/
register extraction (which hardcodes Windows-version-specific offsets) does not fully support XP SP3's KDCOM.
**NEXT to make KD usable:** (1) try r2 winkd against a HALTED kernel — add `/break` to boot.ini so XP stops
at boot for the debugger (RISK: if r2 can't continue it, the VM is stuck at the wait until boot.ini is fixed
offline; only do this with an offline-edit recovery ready); (2) or drive KDCOM at a lower level — a minimal
custom KDCOM client (send breakin `0x62626262`, parse the state-change packet, do `READ_CONTROL_SPACE`/
`READ_VIRTUAL` for regs+memory at kernel VA `0x400000 + 0xf67f8`) since r2's high-level layer is the gap, not
the wire protocol; (3) or use a maintained KD client that supports XP (WinDbg via the pipe, if a Windows host
or wine is available). The infrastructure (COM2 socket + kddebug.bat + confirmed sync) is reusable for any of
these. The kernel base can be found from the running target via IDT[0xd]→KiTrap0D (RVA 0x9090).

**Option B, custom KDCOM client (2026-08-05) — the wire protocol WORKS; XP uses STATE_CHANGE64 (type 7).**
Wrote `scripts/kdclient.py` (minimal XP 32-bit serial-KD client over `vm/kd.sock`) after r2 winkd proved
unusable for XP. First a QEMU fix: a bare `-serial unix:...,server` is SINGLE-USE (2nd attach → "Connection
refused"); switched `xp-vm.sh` to an explicit re-accepting `-chardev socket,id=kddbg,...,server=on,wait=off`
+ `-serial chardev:kddbg`. Then the breakthrough — the client connects, spams the breakin byte (`0x62`) while
pumping, and **received a correctly-framed 261-byte packet**: `PacketLeader=0x30303030`, **`PacketType=7`
(PACKET_TYPE_KD_STATE_CHANGE64 — XP+ uses the "64" wait-state-change, NOT the type-1 "32" struct; THIS is
what r2 winkd misparses → its eip=0)**, `PacketId=0x80800800` (INITIAL), `NewState=0x3030`
(DbgKdExceptionStateChange), and the kernel **halted at PC=0x80527bdc** (ntoskrnl). So framing + checksum +
breakin + STATE_CHANGE64 parse all work — the transport is fully ours now.

**Two bugs to fix next (they caused the stuck VM the user had to kill):** (1) the client breaks in but never
CONTINUES the kernel (`DbgKdContinueApi` = 0x3136), so XP FREEZES — every session MUST send Continue before
exit. (2) KD state persists across socket connections: after a break-in the kernel is HALTED and waiting for
DBGKD_MANIPULATE commands, so a FRESH connect + breakin to an already-halted kernel gets no new state-change
(it timed out). ⇒ the whole flow must be ONE persistent session: break in once → `DbgKdGetVersionApi`
(0x3146 → KernBase, to map 0x4f67f8 → runtime = KernBase + 0xf67f8) → `DbgKdWriteBreakPointApi` (0x3134) at
the reflect → `DbgKdContinueApi` to unfreeze → run `pfrun.bat` → catch the breakpoint STATE_CHANGE64 → read
the CPU context (`DbgKdGetContextApi` 0x3132) + memory (`DbgKdReadVirtualMemoryApi` 0x3130) → single-step the
gates. The manipulate send/recv scaffolding + GetVersion parse are already in `kdclient.py` (the manipulate
packet-id handshake still needs live tuning). **This is the closest we have ever been to SEEING the reflect
reject** — the wire works; only the KD session state-machine remains. (VM was killed mid-test; relaunch with
`./scripts/xp-vm.sh run`, KD is already enabled in boot.ini.)

**Harness note (learned this run):** do NOT `rm vm/serial.log` while QEMU holds it open — QEMU keeps writing
to the unlinked inode and the path reads empty. Truncate (`: > vm/serial.log`) instead, or restart QEMU. The
same log is mirrored in-guest at `C:\ntvdmex\ntvdmhost.log`. Also: a fresh (clean-shutdown) boot reads the CD's
new volume label immediately — no F5 needed; only a live `qmp.py cd` hot-swap into an already-open window needs
the navigate-away-and-back refresh. `qmp.py click` takes **px in 1024×768 space** = guest-pixel × 1.28.

**Option B COMPLETE (2026-08-06) — `scripts/kdclient.py` is a WORKING XP kernel debugger over KDCOM; every
primitive PROVEN on the live HVF kernel.** The KD session state-machine is finished and VM-confirmed. What was
wrong in the earlier scaffold, fixed by reading ReactOS `drivers/base/kdcom/kddll.c` + `sdk/include/.../windbgkd.h`:
- **Packet-id handshake.** `INITIAL_PACKET_ID=0x80800000`, `SYNC_PACKET_ID=0x800` (the wire's first id
  `0x80800800` = INITIAL|SYNC). The kernel's `RemotePacketId` starts at **INITIAL (`0x80800000`, no sync)** and
  it accepts a data packet ONLY if `PacketId == RemotePacketId` (kddll.c:290, strict `==`). The scaffold sent
  `0x80800800` → the kernel **ACKed but IGNORED** every manipulate. Fix: our outgoing data-packet id starts at
  `0x80800000` and toggles low-bit per ack; **ACK inbound packets with `id & ~SYNC`** (kddll.c:166). Advance
  our send-id whenever a response arrives (the kernel accepted us), not only on a cleanly-seen ACK.
- **Struct offset.** `DBGKD_MANIPULATE_STATE64` has NO `pshpack`; its union `u` is 8-aligned at **offset 0x10**
  (ApiNumber@0, ProcLevel@4, Processor@6, ReturnStatus@8, pad@0xC, u@0x10). The scaffold put the request body
  at 0xC. `sizeof=56`; appended payload (CONTEXT / read-memory bytes) starts at offset 56.
- **KD state PERSISTS across socket connections.** A fresh connect to an already-halted kernel gets no new
  state-change from breakin bytes (it's not polling — it's blocked in `KdReceivePacket`). Fix: **RESET-on-connect
  resync** (`send_control(PKT_RESET,0)`; kddll.c:176 makes the kernel reset both ids to INITIAL, echo RESET, and
  re-send its pending STATE_CHANGE). `break_in()` resyncs first and only sends breakin bytes (SPARINGLY) if the
  kernel is actually running — buffered breakin spam was causing repeated re-breaks.
- **Resume past the break-in int3.** The break-in halts with the trap-frame EIP sitting **on the 0xCC of
  `RtlpBreakWithStatusInstruction`** (`mov eax,[esp+4]; int3; ret 4` at RVA `0x50bdc`; runtime `0x80527bdc`).
  Plain `DbgKdContinueApi`/`ContinueApi2` just re-execute the int3 → re-break (observed 80/80). Fix (what WinDbg
  does): `get_context` → `EIP+=1` → `SetContext` (needs a prior GetContext: `KdpContextSent`, and
  `Data->Length==sizeof(CONTEXT)`=716) → Continue. `KD.resume()` bundles this.

**VM-CONFIRMED live (KernBase `0x804d7000`, slide `0x800d7000`):** GetVersion (major 15 / minor 2600 / machine
0x14c), `read_vmem` (bytes @KernBase = `MZ…`), `get_context`/`set_context` (EIP `…dc`→`…dd`), `write_bp`
@`0x805cd7f8` (=reflect `0x4f67f8` rebased) → handle 1 status 0, `restore_bp` status 0, and **single-step**
(Continue2 TraceFlag=1) tracing 6 kernel insns with exc `0x80000004` (STATUS_SINGLE_STEP). This is the exact
kernel-side visibility HVF's absent gdbstub (run 68) and r2's XP-incompatible winkd denied — **runs 65–69's
blind wall is broken.**

**RESUME (next run) — SEE the reflect reject.** `python3 scripts/kdclient.py session vm/kd.sock steps=300`
(default bps = the reflect chain `0x4f67f8/0x4f6f67/0x4f6efd/0x4f6e6f/0x4f6d3c/0x4f6dc0`, auto-rebased). It
arms bps, resumes, and waits; then in the VM double-click `D:\pfrun.bat` (host v62, `g_dpmi_use_interp=0`, raw
PM #GP). On the bp hit at `0x4f67f8` it dumps regs+code and single-steps `steps` instructions logging rebased
EIP+EAX — so we watch the classifier `[0x714]` bits 3/4/14, the `0x45dd5f` selector resolve, and the
`0x4f6f67→0x4f6efd/0x4f6e6f` gates, and see the exact predicate that returns 0. (Confirm `D:\pfrun.bat` +
`pmfault.com` + host v62 are on the mounted CD first; rebuild the test disc if not.) Everything uncommitted on
`spike/dpmi-16bit-switch`; `kdclient.py` is the durable capability.

**Reflect-session attempt #1 (2026-08-06) — two operational lessons; NOT yet observed.** First live run of
the reflect session surfaced two things to fix before the observation lands:
- **DANGER: never breakpoint the reflect GATE subroutines `0x4f6d3c`/`0x4f6dc0` (the descriptor validators
  via `0x45dd5f`).** They are called *constantly* by NORMAL kernel selector validation, so a `0xCC` there
  floods KD with breaks at raised IRQL and **instantly bugchecks + reboots XP** (attempt #1 armed all 6 chain
  addresses and the box rebooted before pfrun even ran). Fix: `kdclient.py` DEFAULT_REFLECT is now **only
  `0x4f67f8`** — the VDM-specific entry, reached only when `EPROCESS.VdmObjects!=0`, so it stays quiet until a
  VDM raw #GP — then **single-step** into the body (`steps=N`) to reach the gates without leaving `0xCC` in
  them. (A reboot harmlessly clears all our `0xCC` bytes — fresh kernel image.)
- **The GUI trigger must be verified.** A blind `Win+R` → type `D:\pfrun.bat` → Enter did NOT run pfrun
  (serial.log showed only the boot-time dpmiauto/dpmitest output, no `PMFAULT:` strings) — focus/timing. Use a
  reliable trigger: telnet (`localhost:2323`, user `ntvdmex` if enabled) running `D:\pfrun.bat` is fully
  scriptable; else screendump-verify the Run dialog opened before typing. NB the reflect bp at `0x4f67f8` is
  hit ONLY by a raw PM #GP (patched-INT DPMI clients reflect as `C4C4` BOPs at `0x565041`, which returns 1 and
  never reaches `0x4f67f8`), so the autorun's dpmitest does not trip it — the bp is clean until pmfault's HLT.
- **Aftermath: the bugcheck reboot WEDGED the boot** (dirty-volume autochk / early hang; KD breakin got 0
  response and 0 debug packets for 90s), recovered with a QMP `system_reset`. So START THE NEXT REFLECT RUN
  FROM A CLEAN DESKTOP (fresh `./scripts/xp-vm.sh run` if needed), arm `session vm/kd.sock steps=400 0x4f67f8`
  (single safe bp), then fire `D:\pfrun.bat` with a VERIFIED trigger (screendump the Run dialog, or a working
  telnet). The KD toolset itself is proven and unchanged; only the trigger + a healthy VM remain.
- **Attempt #2 (clean cold boot, single bp) — ROOT CAUSE of the reboot loop found: it is the AUTORUN, not KD.**
  On a fresh boot the login autorun `dpmiauto.bat` runs `dpmitest` under host v62 (`g_dpmi_use_interp=0`,
  real-CPU). dpmitest reaches STAGE2 then hits its 2nd-0301 hang — which on the real-CPU path IS the run-52/69
  **process-wide kernel WEDGE** — and on `-smp 1` that wedge trips XP's bugcheck watchdog → **reboot, every
  boot**, before a stable window exists. Serial.log confirmed only dpmitest ran (no `PMFAULT:` lines); our
  `0x4f67f8` bp got 0 hits (dpmitest uses BOP-patched INTs, never raw-#GPs there — the bp is genuinely quiet).
  So the reboots are the real-CPU host's own hazard, not the debugger. **THE FIX for the next run: neutralize
  the autorun first** — before triggering pfrun, disable/clear the `dpmiauto` Run-key (or delete
  `C:\...\dpmiauto.bat`) so the box boots to a QUIET stable desktop; then arm `session vm/kd.sock steps=400
  0x4f67f8` and run `D:\pfrun.bat` manually. pmfault's fault is a MINIMAL raw `HLT` #GP (not dpmitest's
  setaccess wedge), so it should reach `0x4f67f8` cleanly and let the single-step trace land — WITHOUT the
  autorun wedging the box first. (Break-in reliability was also fixed this session: `break_in` now sprays
  continuously + nudges with a non-flushing RESET; proven 0.2s halt of the running kernel.)
- **Attempt #3 (pfrun-only CD, autorun neutralized) — the deeper blocker: my aggressive break-in
  DESTABILIZES the KDCOM state and bugchecks the kernel.** Built a minimal CD (host v62 + pfrun + pmfault +
  dosstub, NO dpmitest/dpmiexe/i310102) so the autorun's `:waitcd` loop finds no `D:\dpmitest.com` and exits
  harmlessly — VM-confirmed: serial.log stayed EMPTY, desktop stable, no dpmitest wedge. BUT after a break-in
  stress test (4×) + arming the session + a single Win+R, the box **rebooted again before pfrun was even
  triggered**. With dpmitest gone AND pfrun not yet run, the only remaining agent is **kdclient's own break-in
  churn** — the burst-of-breakin-bytes + repeated RESET control packets. Cumulative breakin/RESET storms
  appear to corrupt XP's KDCOM state machine → bugcheck. ⇒ **`break_in` must be made WinDbg-GENTLE: send ONE
  breakin byte and wait patiently (seconds), no byte-sprays, no RESET storms; only RESET once on connect.**
  The READ primitives (GetVersion / read_vmem / get/set context / single-step) are safe and proven — it is
  specifically the break-in *acquisition* method that is too aggressive. Until break_in is gentled (or real
  WinDbg is used via the pipe), each extended KD session ends in a reboot. This is the concrete next fix
  before the reflect observation can land; the observation itself is otherwise fully set up (single bp
  `0x4f67f8` + single-step + pfrun-only CD recipe above).
- **Attempt #4 (gentle break_in) — VM DISK IS NOW CORRUPTED from the accumulated crashes; STOP + RESTORE.**
  Implemented the WinDbg-gentle break_in (single breakin byte, patient wait, no sprays, no RESET storms;
  RESET only once on connect). It armed the single bp cleanly and the box confirmed `running` — but the VM
  **still rebooted on a bare Win+R (pfrun NOT triggered, dpmitest absent, break_in already done)**. With every
  candidate agent eliminated, the cause is the VM itself: the ~6 accumulated bugchecks/hard-kills this session
  left the XP install damaged (it threw the "Windows did not start successfully" recovery menu, then
  spontaneously reboots on minimal interaction). ⇒ **The reflect observation cannot land on this corrupted
  image.** Recovery: `qemu-img snapshot -a clean vm/xp.qcow2` (VM off) reverts to the **`clean` snapshot (ID 1,
  2026-06-02)** — a pristine XP (NB: pre-provisioning, so no ntvdmex autologin/autorun/IFEO; pfrun sets its own
  IFEO, and no autorun = no wedge, which is ideal; may boot to a login screen needing one interactive logon).
  **NEXT SESSION (clean VM):** restore snapshot → `./scripts/xp-vm.sh run` (pfrun-only CD staged at
  vm/transfer.iso) → arm `session vm/kd.sock steps=400 0x4f67f8` (gentle break_in, single safe bp) → run
  `D:\pfrun.bat` → capture the single-step trace of the reflect gate returning 0. All the tooling + the recipe
  are in place; only a healthy VM was missing. The KD read-debugger (version/mem/context/bp/single-step) is
  proven and durable regardless.

### Kernel RE session 3 (2026-08-05) — FIXED_NTVDMSTATE is at fixed linear `0x714`, and the reflect gate wants bits 0,1,9 `[FACT, static disasm]`

Re-opened the PM-fault-reflect track (GH #18) — the strategic prize: if the kernel reflects PM VDM faults
to us, PM (16- *and* 32-bit) runs on the **real CPU** like ntvdm, retiring the interpreter (and the
`INT nn`→BOP patch) for the general case. On XP-32 this is provably solvable — `ntvdm.exe` does it on the
same kernel — and the binaries are in `reverse/`.

**Env re-established on the dev host:** `cabextract reverse/NTOSKRNL.EX_` / `reverse/NTVDM.EX_` →
`/tmp/ntvdmex-re/{ntoskrnl,ntvdm}.exe`; `r2` on ntoskrnl (VA base `0x400000`). Landmarks match the older
sessions: `NtVdmControl` @ `0x4e09b7`; `KiDispatchException` reads `fs:[0x124]→[+0x44]→[+0x158]` (VdmObjects).

**Decisive finding — the fixed-state is at a *fixed linear address* `0x714`, read directly (not via a
VdmObjects pointer):**

```
0x4f63dd:  mov eax, [0x714]      ; FIXED_NTVDMSTATE flags
           and eax, 0x203
           cmp eax, 0x203         ; branch to the MONITORED path only if bits 0,1,9 all set
           jne  0x4f63f8          ; else the not-monitored path
```

The same `[0x714] & 0x203 == 0x203` predicate recurs at `0x4f65bd`; near NtVdmControl, `0x4e0b2e:
mov ecx,[0x714]; test cl,2` gates on bit 1. **Our `src/vdm/dpmi_enter.S` only ever sets bit 9**
(`lock or [0x714],0x200`) and merely *tests* bits 0,1 (`test [0x714],3`); a bare `VdmInitialize` never
establishes bits 0,1. So `(flags & 0x203)` can't reach `0x203` on our PM entry ⇒ the kernel takes the
**not-monitored → don't-reflect** branch. **Candidate root cause for the silent-terminate on a PM `#GP`.**

**Hypothesis to test (VM):** set FIXED_NTVDMSTATE bits 0,1 (0x3) — the way real ntvdm's init does, not a
blind poke — before PM entry, then re-run the faulting case (i310102's SS-retype `#GP`, or a `HLT` in PM)
and see whether the fault now reflects to our handler/VEH instead of terminating.

**Not-yet-closed (before calling it solved):** (1) confirm the `0x4f6xxx` predicates are on the
`#GP`/trap-reflect path vs. the PIC/virtual-IF interrupt-injection path — **locate `KiTrap0D` and read its
`[0x714]` gate directly**; (2) find where real ntvdm sets bits 0,1 (its `VdmInitialize` site + low-memory
FIXED_NTVDMSTATE setup) and mirror it; (3) confirm on a live VM under WinDbg watching the `[0x714]` read at
fault time. Full context: GH #18. Bits 0,1,9 `= 0x203` is the concrete lever this session surfaced.

### Kernel RE session 5 (2026-08-05) — the `[0x714]&0x203` gate is the INTERRUPT-INJECTION path, NOT the `#GP` reflect; session 3's bits-0,1,9 lead is retired `[FACT, static disasm]`

Closed session 3's **not-yet-closed item (1)** — "confirm the `0x4f6xxx` predicates are on the
`#GP`-reflect path vs. the interrupt-injection path; locate `KiTrap0D` and read its `[0x714]` gate
directly." Re-loaded `/tmp/ntvdmex-re/ntoskrnl.exe` in `r2` (VA base `0x400000`) and traced both the
`[0x714]&0x203` gate *and* `KiTrap0D` end-to-end. **The two are different code paths, and session 3
conflated them.**

**1. The `[0x714]&0x203==0x203` gate (`0x4f63dd`, `0x4f65bd`) is the VIRTUAL-INTERRUPT INJECTION path.**
When the predicate holds it `call 0x50a58d`, which loads the VDM_TIB (`[0xffdff018]→[+0xf18]`, the
kernel-mode mirror of `dpmi_enter.S`'s `fs:[0x18]→[+0xF18]`) and writes **`VTIB_EVENT(0x5a8)=3`**,
`0x5ac=0`, `0x5b0=0` — *byte-for-byte the event-3 "interrupt pending" block our own `dpmi_enter.S`
writes at label `2:`.* The enclosing `0x4f63a6–0x4f647f` routine scans guest opcode bytes
(`movzx ecx,byte[edi]; inc edi`), bounds-checks `MmHighestUserAddress`, and injects a vector
(`push 2; call 0x54bd4f`). This is the VDM privileged-opcode / virtual-IF interrupt-delivery engine.
**Bits 0,1,9 here govern whether a pending virtual interrupt is INJECTED into the guest — not whether a
`#GP` is reflected.** Setting bits 0,1 (session 3's hypothesis) would change interrupt *delivery*, and
does not address the PM-fault silent-terminate. **⇒ Session 3's bits-0,1,9 lead is a detour; retired.**
(It is not *irrelevant forever*: once PM runs on the real CPU, this same gate is what delivers timer /
keyboard IRQs into PM — Doom needs it — so bits 0,1 matter later, for injection, just not for #18's
terminate.)

**2. `KiTrap0D`'s actual `#GP`-reflect path (re-confirms session 4).** `KiTrap0D` (~`0x409090`) reflects
in two stages at both call sites (`0x4090b7`, `0x40926c`):
```
call 0x565041      ; BOP reflect — handles ONLY C4 C4 (sets VTIB_EVENT=4), returns 0 for a raw #GP
test al,0xf / jne handled
push 6 / call 0x4f67f8   ; the NON-BOP fault reflect; returns 0 → VDM terminates
test al,0xf / jne handled
KfLowerIrql / jmp <terminate>
```
- `0x565041` computes the fault linear addr — V86: `(EIP&0xffff)+((CS&0xffff)<<4)`; **PM: resolves the
  selector via `0x45dd5f`** (`test [trapframe+0x72],2` = EFLAGS.VM) — then `cmp word[edi],0xc4c4`:
  reflects a `C4 C4` BOP (→ `VTIB_EVENT=4`, the path our INT→BOP patch exploits) and returns **1**;
  otherwise returns **0**. This is why the bypass works in PM and a raw `#GP` does not go here.
- `0x4f67f8` top-gate = `[[fs:0x124]+0x44]+0x158] != 0` = **`EPROCESS.VdmObjects != 0`** (XP offset
  0x158) — **which we already satisfy** (the V86 monitor allocates it; BOP reflect proves it). Past the
  gate it *safe-reads* `[0x714]` via `0x564ed5` (an SEH-guarded `*(&0x714)`) and classifies on NTVDMSTATE
  **bits 3/4/14** (`test eax,8 / 0x10 / 0x4000`) + the fault vector — a *classifier*, not the injection
  gate. It then enters the reflect body (`0x4f68b8+`) and calls `0x4f6f67`.
- `0x4f6f67` branches on trap-frame `EFLAGS.VM`: V86 → build an INT frame on the guest stack and vector
  through the **real-mode IVT** (`mov eax,[ecx*4]`); **PM (`0x4f6fed`) → load VDM_TIB, `lea edi,[edi+0x634]`,
  `call 0x4f6e6f`** — reflect through **`VDM_TIB+0x634`**, exactly session 4's answer.
- `0x4f6e6f` decodes the `VDM_TIB+0x634` block: `+0x634`(word)=nesting counter (0 first time, `inc`'d);
  `+0x638`(word)=**handler selector** installed as the new CS; new **EIP=0x1000** (`mov [esi+4],0x1000`);
  `+0x63a/+0x63c/+0x640`=save slots for the interrupted CS/EIP. If the handler selector is 0/invalid the
  reflect fails → `return 0` → terminate.

**Net: session 4 + run 34 stand as the mainline for #18; session 3 pointed at the wrong gate.** The PM
`#GP` reflect is `KiTrap0D → 0x4f67f8 → 0x4f6f67 → VDM_TIB+0x634`. Run 34 already localized the failure to
an **early `return 0` inside `0x4f67f8`/`0x4f6f67`/`0x4f6e6f`, gated on VDM state we haven't established**
(and proved it is *not* a missing `+0x634` field — field-guessing was exhausted in runs 31–34). Run 33
also found the **handler CS:EIP is registered in ntvdm GLOBALS `[0xf09c15c/15e]`**, not in the `+0x634`
block. So the standing plan is unchanged from run 34: **replicate ntvdm's COMPLETE DPMI mode-switch init**
(the `[0x714]` fixed-state setup incl. the classifier bits 3/4/14, the exception/interrupt handler
registration at `[0xf09c15c/15e]`, `VdmPMCliControl`, and the `+0x634` block) rather than cherry-pick
fields — with the run-32 landed-BOP harness (`RETURNED event=4`) for observability. **Do NOT re-run the
bits-0,1,9 experiment.**

Next static step (if pursued): RE ntvdm's 1687-entry mode-switch handler (`ntvdm 0xf02d39f` → the returned
entry) and its DPMI-init, capturing the exact `[0x714]` byte pattern it writes + the `[0xf09c15c/15e]`
handler registration, then mirror the whole sequence in `src/vdm/`. Landmarks: `NtVdmControl @ 0x4e09b7`;
`0x565041`/`0x4f67f8`/`0x4f6f67`/`0x4f6e6f` (this session); `0x50a58d` = the event-3 injection raiser.

### Kernel RE session 6 (2026-08-05) — the ntvdm-side DPMI-init map: exact `[0x714]` bit semantics + the handler-registration chain to mirror `[FACT, static disasm of ntvdm.exe]`

Followed session 5 with the **ntvdm.exe** side (base `0x0f000000`, `.text @ 0x0f001000`), to produce the
concrete init sequence to replicate in `src/vdm/`. ntvdm reaches the FIXED_NTVDMSTATE either absolutely
(`[0x714]`, since the kernel maps the VDM's first MB at flat 0 in the ntvdm process) or via base ptr
`[0xf0774d0]` (the VDM linear base). **`[0x714]` bit map, from every write/test site:**

| bit | mask | meaning | set/cleared at |
|----|------|---------|----------------|
| 0 | 0x1 | virtual-IF state A (interrupt pending/serviceable) | `0xf004933` `or [0x714],1` (arg==3 path), tested `0xf00442b`/`0xf005378` |
| 1 | 0x2 | virtual-IF state B | `0xf004919` `or [0x714],2` (arg==1 path) |
| 2 | 0x4 | (mode/state) | `0xf0510c5` `or [base+0x714],4` |
| 9 | 0x200 | **in-monitor** (gated on guest IF) | `0xf044860` `or [0x714],0x200` / `0xf044897` clear — this is the routine our `dpmi_enter.S` ports (`0xf04483c`) |
| 12 | 0x1000 | mode-transition | cluster A `0xf05498d` set / `0xf054a0c` clear |
| 13 | 0x2000 | mode-transition | cluster A `0xf054978`/`0xf0549f2` set / `0xf054986`/`0xf054a00` clear |

**Key correction to session 3/5:** bits 0,1 are the **dynamic virtual-IF state** written by the STI/CLI /
`VdmPMCliControl` emulator (`0xf004913`, arg 1→bit1, arg 3→bit0) — runtime interrupt bookkeeping, NOT a
static "monitored" enable. The kernel classifier `0x4f67f8` tests bits **3/4/14**, which **ntvdm never
writes** — so those are set kernel-side (VdmInitialize/monitor) or select the reflected vector, and are
*not* the thing our host is missing. **⇒ the reflect blocker is the handler plumbing below, not a `[0x714]`
bit we forgot to poke.** (Re-confirms run 34: "not a field guess; replicate the whole init.")

**The PM fault/interrupt HANDLER registration chain (the piece runs 33–34 were missing):**
1. **`0xf01a300`** (DPMI raw-mode-switch / handler-register service): the client passes a far pointer to a
   `{IP@0, CS@2, DATASEG@4}` struct; ntvdm resolves it (`VdmMapFlat` on `CONTEXT.SegCs`, then
   `Sim32pGetVDMPointer`) and stores **`[0xf09c15c]=IP`, `[0xf09c15e]=CS`, `[0xf0a419c]=DATASEG`**,
   the 16/32 flag `(CONTEXT.Eax & 1) → [0xf09c178]` and `→[VDM_TIB+0x636]`, and if 32-bit `[0x715] |= 1`.
   (`0xf005ba1` is a second/internal registration that fills the same globals from the live CONTEXT.)
2. **`0xf050ad7`** (per-PM-entry arming of the `+0x634` block, called before `dpmi_enter`): sets
   `[VDM_TIB+0x634]=0` (nesting counter), `[VDM_TIB+0x638]=word[VDM_TIB+0x36c]` (the **handler selector**
   the kernel installs as the new CS), `[VDM_TIB+0x636]=[0xf09c178]` (16/32 flag).
3. **Kernel reflect (session 5):** `0x4f6e6f` sets the faulting context to **`CS=[VDM_TIB+0x638]`,
   `EIP=0x1000`** and saves the old CS/EIP into `+0x63a/+0x63c/+0x640`. So the kernel jumps the guest to
   **`selector([VDM_TIB+0x638]) : 0x1000`** — which must be an ntvdm **PM-fault trampoline stub** that reads
   the real handler from `[0xf09c15c/15e]` and dispatches. **This reconciles run 31 exactly**: run 31 set
   `[+0x638]=0x17` and the reflect *did* proceed to `[+0x638]:0x1000`, but nothing was planted there →
   garbage → re-fault. The missing piece was never a `+0x634` field — it was (a) registering the handler
   globals and (b) planting the trampoline at `selector:0x1000`.

**⇒ Actionable replication spec for real-CPU PM #GP reflect (the `src/vdm/` work for #18):**
- Allocate an LDT **code selector `H`** whose linear base+`0x1000` holds a **BOP stub (`C4 C4 nn`)** (reuse
  the run-32 primitive: a PM BOP cleanly reflects to the host loop as `VTIB_EVENT=4`).
- Per PM entry, arm the block like `0xf050ad7`: `[TIB+0x634]=0`, `[TIB+0x638]=H`, `[TIB+0x636]=<16/32 flag>`.
- On the host's `event=4` from that stub: read the saved fault `CS:EIP/SS:ESP` from `+0x63a/+0x63c/+0x640`
  (+ the guest regs in the VTIB block), service the fault the way our INT→BOP loop already services INT
  31h/21h (or, for a genuine PM exception, emulate/skip the one instruction), then resume the client at
  the saved CS:EIP. This routes a **raw non-BOP PM `#GP`** into the same host loop that already runs
  i310102/DPMIBACK — without needing ntvdm's globals `[0xf09c15c/15e]` at all (we substitute our stub for
  ntvdm's trampoline).
- Keep the existing INT→BOP scan as the fast path; the `+0x638` trampoline is the catch-all for faults the
  scan can't pre-patch (SS-retype `#GP`, `HLT`, privileged ops).

**Strategic caveat (record before committing runs):** this gets **16-bit** PM on the real CPU. **32-bit
DOS/4GW (Doom, #19) additionally needs flat 4GB selectors, which XP's LDT REJECTS**
(`STATUS_INVALID_LDT_DESCRIPTOR`, base+limit ≤ `MmHighestUserAddress`; runs 20–30) — a *separate, unsolved*
blocker on top of the reflect. And the interpreter already runs the 16-bit clients end-to-end. So the
real-CPU reflect is worth doing for #18 correctness/fidelity, but it is **not by itself** the Doom unlock;
the 32-bit-flat-selector problem is the gating item for #19 and should be scoped before sinking many runs
into the reflect. Landmarks (ntvdm): register `0xf01a300`; arm `0xf050ad7`; monitor-entry `0xf04483c`;
VdmPMCliControl/IF `0xf004913` + `0xf00532e`; globals `[0xf09c15c]=IP/[0xf09c15e]=CS/[0xf09c178]=16-32 flag`.

### Kernel RE session 7 (2026-08-05) — GO/NO-GO on real-CPU 32-bit PM: the LDT is hard-capped to `MmHighestUserAddress` (~2GB), and that is the SAME cap stock ntvdm runs under `[FACT, static disasm of ntoskrnl.exe + ntvdm.exe]`

**The scoping question (P0, gates #18-impl-scope):** #19 (Doom's DOS/4GW) is *32-bit* PM. A DOS/4GW client
conventionally wants a **base-0, 4GB-limit flat data selector**. Runs 20–30 found XP's LDT rejects a flat
4GB descriptor (`STATUS_INVALID_LDT_DESCRIPTOR`). Before spending runs on the `+0x638` reflect (which only
buys real-CPU PM if 32-bit is reachable), settle: **does real ntvdm give a DOS/4GW client 32-bit flat
selectors on this exact XP kernel, and if so how — GDT? a bounded "flat within user space" LDT selector?**
This session traced both the kernel install-validator and ntvdm's descriptor-install call. Disasm-only, no VM.

**1. The kernel LDT descriptor validator is `0x557514` — instruction-level decode.** `NtVdmControl`
dispatch (`0x4e09b7` → chain at `0x52d1be`): **service 10 (`VdmSetLdtEntries`) → `0x55775b`**, service 11
(`VdmSetProcessLdtInfo`) → `0x557a14`. `0x55775b` requires each selector be 16-bit (`test 0xffff0000,sel;
jne err`) and calls **`0x557514` once per 8-byte descriptor**; a `0` return ⇒ the whole svc-10 fails
(caller does `setge cl` → DPMI reports failure). `0x557514` decodes the descriptor and enforces:
- **Range gate (`0x5575a1`):** build the full 32-bit `base` (bytes 2-4,7) and the **effective byte limit**
  — page-granular limit is expanded `limit = (raw20<<12)|0xFFF` when the **G bit (dword1 bit 23)** is set,
  and expand-down segments get the D/B-dependent max (`0xFFFF`/`0xFFFFFFFF`). Then `top = base + byte_limit`
  and: reject if `base > top` (wrap) **or `top > MmHighestUserAddress`** (`[0x488bdc]`). ⇒ **every present,
  loadable descriptor must satisfy `base + limit ≤ MmHighestUserAddress`.**
- **DPL forced 3** (`edx & 0x6000 == 0x6000` else reject); **S=1 code/data only** (system segs rejected);
  **conforming code rejected** (`(access&0x18)==0x18 && access&4 → reject`); reserved bit21 must be 0.
- Fast-accept only for a truly empty descriptor (type+S bits zero, DPL zero). A *not-present* (P=0) desc
  skips the range check but is useless once loaded (#NP) — no backdoor to a usable 4GB selector.

**Quantified (the number that matters):** `MmHighestUserAddress` on stock 2GB-split XP = **`0x7FFEFFFF`**
(`0xBFFEFFFF` with `/3GB`). Worked example of a true 4GB flat request — base 0, G=1, raw limit `0xFFFFF`:
expand → `0xFFFFF*0x1000+0xFFF = 0xFFFFFFFF`; `top = 0 + 0xFFFFFFFF = 0xFFFFFFFF > 0x7FFEFFFF` ⇒ **REJECTED**.
The **largest installable flat data selector is base 0, limit ≈ `0x7FFEFFFF` (2GB−64KB), page-granular** —
NOT 4GB. This confirms runs 20–30 at the instruction level and pins the exact cap and the exact branch.

**2. Stock ntvdm is subject to the identical cap — it does NOT work around it.** ntvdm's descriptor
install is at **`0xf050120`: `push 0xa` (svc 10) → `call NtVdmControl`**, and it passes the client's **raw
16 descriptor bytes straight through** (`[eax..eax+0xf]` = two 8-byte descriptors, sel `esi` + `esi+8`) into
the ServiceData block with **no ntvdm-side clamp**. So when a DPMI client (incl. DOS/4GW) asks for a 4GB
flat selector, ntvdm hands it to `0x557514`, the kernel rejects it, and ntvdm returns DPMI failure — **real
ntvdm cannot give any client a true 4GB LDT selector either.** The DPMI host still runs 32-bit clients: the
validator honours the D/B big bit and ntvdm carries the 16/32 flag end-to-end (`[0xf09c178]`, `[0x715]|=1`,
per-entry `[TIB+0x636]` — session 6). LDT selectors only (TI=1); no GDT selector is minted for clients
(`KeI386SetGdtSelector` exists but is not on the DPMI descriptor path).

**⇒ VERDICT: GO — real-CPU 32-bit PM is reachable, and there is NO XP-fundamental block that stops NTVDMEX
from reaching stock-ntvdm parity for DOS/4GW.** The 4GB→~2GB "flat" cap is a **platform constant shared with
ntvdm**, not an NTVDMEX regression. Because a DOS game addresses only a few MB (Doom < 16 MB) — all offsets
well under the 2GB limit — 32-bit execution never faults on the limit. So **#18's `+0x638` reflect is worth
building for the whole 32-bit game class (to ntvdm parity), not just 16-bit fidelity.** The interpreter stays
the fallback; it already runs the 16-bit clients and is the honest path for any client that needs a literal
4GB descriptor the real CPU can't provide.

**The selector recipe (what to install for a 32-bit client, matching what ntvdm can install):** via svc 10
(`dpmi_install`/`VdmSetLdtEntries`), an LDT data selector with **base 0, G=1 (page-gran), raw limit
`0x7FFEF` (⇒ byte limit `0x7FFEFFFF`), D/B=1 (32-bit big), DPL 3, S=1, present, non-conforming**; the code
selector likewise base 0 (or the client's CS base) with D/B=1 for 32-bit. Do NOT request limit `0xFFFFF` —
it fails the range gate. This is directly checkable off-VM against `0x557514`'s rules before any VM run.

**Residual risk (bounded, and shared with ntvdm — not a blocker):** an extender that, at init, `LSL`s its
flat data selector and *asserts the limit is literally `0xFFFFFFFF`* would abort on the 2GB cap. That is a
risk **real ntvdm has too** (same kernel path), so it is not something #18 introduces; DOS/4GW itself does
not do this (it uses whatever limit the host grants), which is why DOS/4GW games run under stock NTVDM. If a
specific title trips it, the interpreter fallback covers it. Verified by disasm only; the VM check comes when
#18's reflect lands and a 32-bit client is driven through `dpmi_service_pm_int`.

Landmarks (kernel): `NtVdmControl @ 0x4e09b7` → dispatch chain `0x52d1be`; **svc 10 `VdmSetLdtEntries`
`0x55775b`**, svc 11 `0x557a14`; **descriptor validator `0x557514`** (range gate `0x5575a1`, cap vs
`MmHighestUserAddress [0x488bdc]`); selector→descriptor reader (fault path, session 5) `0x45dd5f`.
Landmarks (ntvdm): **svc-10 install `0xf050120`** (raw-descriptor passthrough); 16/32 flag globals
`[0xf09c178]`/`[0x715]` (session 6). Env `/tmp/ntvdmex-re/*.exe` (re-extract if gone); `r2` bases: ntoskrnl
VA `0x400000`, ntvdm `0x0f000000`.

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

### Run 70 (2026-08-11) — KD-observed: a BOP client CANNOT reach the `#GP` reflect; only a raw `#GP` can `[FACT — VM-confirmed via guest KD]`

First live use of the working guest KD harness (`scripts/desktop_trace.py`, `/debug`-only image,
break-in + arm reflect bp `0x805cd7f8` + robust LOAD_SYMBOLS servicing) against a real client on the
real-CPU path (`g_dpmi_use_interp=0`, host `dpmi-harness-v62`).

- **Trigger:** `D:\dpbrun.bat` (DPMIBACK, a BOP-based asm DPMI client) with the reflect bp armed.
- **Result:** DPMIBACK ran **end-to-end** (serial: switch to PM `CS=0x0f:0x132` → `INT31h 0301`
  real↔protected round-trip → `welcome in protected-mode` / `back in real-mode` → `4Ch` exit) and the
  reflect bp `0x4f67f8` was **never hit** (0 serviced, pure idle).
- **Why — the trap-vector split (the correction):** NTVDM BOPs are `C4 C4` = an invalid `LES`
  encoding ⇒ **`#UD` → KiTrap06 → `VdmDispatchBop`**. The `#GP` reflect at `0x4f67f8` hangs off
  **`#GP` → KiTrap0D** (via the "BOP-only" gate `0x565041`). **Different trap vectors.** So *no*
  BOP-based client (DPMIBACK **or** `i310102`) can ever reach `0x4f67f8` — only a genuine
  privileged-instruction `#GP` (pfrun/`pmfault`'s `HLT`) routes through KiTrap0D toward the reflect.
  This retro-explains why runs 65–69 (and the session-3 desktop_trace attempt) never saw `0x4f67f8`:
  the raw `#GP` bugchecks/terminates *before or instead of* reaching it, and BOPs bypass it entirely.
- **Consequence for #18:** to OBSERVE the reflect decision, the only trigger is the raw `#GP`
  (`D:\pfrun.bat`), and the observer must classify halts (reflect-bp / benign break-in / benign
  LOAD_SYMBOLS / **unexpected fault**) and stop-and-dump the unexpected one — `desktop_trace` (bp
  only) and `kdclient.do_session` (blindly resumes non-bp halts) both step past it. New harness:
  `scripts/pmfault_observe.py`.
- **Operational:** DPMIBACK's host leaves a runaway `ntvdmhost.exe` stuck at the `.EXE` 4Ch-exit
  sentinel (`<<<MISMATCH>>>`, run 48); its watchdog spins, pegs the single CPU, and starves the
  guest UI (frozen pointer). `dpbrun.bat`'s taskkill is insufficient — recover with QMP
  `system_reset` (image-safe; provisioning persists). The `/debug`-only image screendumps at native
  **1024×768**, so `qmp.py click` coords map 1:1 (the old ×1.28 was an 800×600-image artifact).

**pfrun (raw `#GP`) observation — ATTEMPTED, not yet captured (harness race fixed).** Built
`scripts/pmfault_observe.py` to classify each KD halt (reflect-bp / benign break-in / benign
LOAD_SYMBOLS / **unexpected fault**) and dump+single-step the unexpected one. First run had a
**receiver race**: the loop used `wait_state_change` AND `get_context` for halt detection, so an
async halt-state-change arriving *during* a `get_context` call desynced the two receivers (get_context
expects a manipulate reply, got a state-change) → mutual stall with the kernel halted at a `0xCC` at
raised IRQL → **watchdog rebooted the guest** before pfrun was even triggered. (Proof the reflect bp
itself is safe to arm during idle: the Run-70 dpmiback session armed the *same* bp `0x805cd7f8` and
idled cleanly to completion.) **Fix applied:** the observer now does **get_context-ONLY** halt
detection (exactly like `desktop_trace.py`), classifying purely by EIP — no competing `wait_state_change`
in the main loop. Recovery churn from the repeated resets landed the guest in XP's "Windows did not
start successfully" menu, so the `/debug`-only image was restored from `vm/xp-debugonly-backup.qcow2`
(clean). **RESUME:** relaunch, break in, arm `0x805cd7f8`, trigger `D:\pfrun.bat` promptly, and read
the observer's `UNEXPECTED FAULT`/`REFLECT HIT` dump — the raw `#GP` is the only path to `0x4f67f8`.

**pfrun (raw `#GP`) observation — BLOCKED by VM instability (4/4 attempts rebooted).** Tried to catch
the raw `#GP` at `0x4f67f8` four times with three observer variants (mixed wait/get_context; get_context-
only; passive wait_state_change-only). Every attempt the guest **rebooted at the arm/connect stage,
before pfrun could be triggered.** Ruled out: the observer receiver-race (fixed) and the get_context
idle-churn (fixed — the passive observer generated zero idle traffic and still rebooted). The passive
observer even reached a clean idle (serviced the ~27 on-connect LOAD_SYMBOLS re-enumeration, then idle,
no crash *caught by KD*) and the guest still spontaneously rebooted ~10s later during GUI clicks — i.e.
NOT a KD-caught bugcheck. The one session that did NOT reboot (Run-70 dpmiback) triggered its client
immediately and kept the guest busy in the VDM. **Working conclusion: arming a software bp (`0xCC`) at
the VDM `#GP`-reflect entry `0x805cd7f8` and letting this heavily-dilated HVF guest sit idle
destabilizes it → reboot.** So KD single-step observation of the reflect is not reliably achievable on
this VM via a software bp + idle-wait. This is the same environmental wall runs 65–69 hit from the other
side (HVF denies the gdbstub; blind fixing exhausted). **UNTRIED alternatives worth a future session:**
(a) HARDWARE bp via `cont2` DR0/DR7 (no `0xCC` in kernel code — avoids the IRQL/int3 destabilizer);
(b) keep the guest BUSY (arm, then immediately drive pfrun with a pre-staged one-keystroke trigger so
there is no idle window); (c) accept the interpreter for 16-bit DPMI (already runs i310102 + DPMIBACK
end-to-end) and pivot to the Sound epic (#20/#21). Reliable-input helper added: `scripts/gtype.sh`
(one atomic key-chord per char — beats this VM's laggy/interleaving QMP send-key).

### Run 71 (2026-08-11) — KD-OBSERVED: a raw PM `#GP` silently terminates the VDM — it never reaches the reflect, never breaks KD, never bugchecks `[FACT — VM-confirmed via guest KD]`

The pfrun observation finally SUCCEEDED, via two fixes to the two blockers: (1) **trigger** = an autorun
CD (`autorun.inf` → `pfrun.bat`, `/tmp/ntvdmex-auto.iso`) mounted via QMP — 100% reliable, no GUI
keyboard/mouse (this VM's QMP `send-key` drops chars even at 0.6s/char, and clicks lag); (2) **reboot** =
mount the autorun CD the *instant* the bp arms (a log-watcher auto-fires `qmp.py cd` on `armed h=`), so
the guest goes BUSY in the VDM immediately — the 4 prior idle-wait attempts all rebooted, the one busy
run (dpmiback) never did. This run the guest **did NOT reboot**.

Observed, real-CPU, host `dpmi-harness-v62` (`g_dpmi_use_interp=0`), reflect bp armed at `0x805cd7f8`:
- serial: pfrun switched to PM (`CS=0x0f:0x12c`), patched its 9 INT sites to BOPs, printed
  **`PMFAULT: in PROTECTED MODE -- about to HLT (raw #GP)...`** — then serial STOPS dead at the HLT.
- KD observer: **NOTHING** — no reflect-bp hit at `0x4f67f8`, no exception state-change, still idle.
- screen: healthy desktop, **no bugcheck, no reboot**. The VDM (ntvdmhost) is silently gone.
- KD was provably LIVE (not desynced): pfrun could only run because the observer correctly resumed the
  guest after arming — the guest ran freely, so "no KD break" is a true negative, not a lost link.

**Conclusion (directly confirms runs 65–69's "invisible fault"):** the kernel's KiTrap0D handles a raw
(non-BOP) PM `#GP` in a VDM by **silently tearing the VDM down** — a *handled* path, so it neither
reflects (never reaches `0x4f67f8`) nor raises an unhandled exception (no bugcheck, no KD break). The
`#GP` reflect at `0x4f67f8` is reachable ONLY via the BOP-gated `0x565041` branch (run 70: BOP = `C4 C4`
= #UD/KiTrap06 for our INT-patch dispatch; the reflect hangs off #GP/KiTrap0D behind a BOP gate). **So
chasing the raw-`#GP` reflect for real-CPU PM is a DEAD END** — a raw privileged instruction just kills
the VDM.

**Actionable pivot for real-CPU PM (GH #18/#19):** do NOT rely on the `#GP` reflect. Instead **pre-patch
identifiable privileged instructions (HLT/CLI/STI/IN/OUT/LGDT/LIDT/…) to `C4 C4` BOPs**, exactly like the
existing INT-site scan, and service them through the already-proven host BOP / event-4 mechanism (the
same path that runs i310102 + DPMIBACK). This is run-68 "option C", now VALIDATED by direct observation:
the reflect is unreachable for raw faults, but the BOP/event-4 dispatch already works. Next spike: extend
`dpmi_patch_int_sites` to also scan+patch privileged opcodes, then re-run pfrun (HLT→BOP) and confirm the
host services it instead of the VDM dying. Tooling from this session: `scripts/pmfault_observe.py`
(passive, classifies+dumps), `scripts/gtype.sh`, the autorun-CD trigger pattern.

<!-- RUN-70-PFRUN-RESULT -->

### Kernel RE session 8 (2026-08-11) — `0x4f67f8` is a reflect-DECISION gated on `VdmObjects`, sitting inside KiTrap0D's in-kernel VDM instruction EMULATOR `[FACT, fresh disasm of ntoskrnl.exe @ base 0x400000]`

Motivated by run 71 (raw #GP silently terminates; `0x4f67f8` never even reached). Prior sessions treated
`0x4f67f8` as "the reflect entry" and RE'd downstream; this session RE'd the entry itself and its
neighbourhood, which reframes the problem.

- **`0x4f67f8` is a small, aligned (called) function — the VDM PM-fault reflect DECISION:**
  ```
  0x4f67f8  mov eax,[0xffdff124]      ; KPCR+0x124 = current KTHREAD
  0x4f67fd  mov eax,[eax+0x44]        ; KTHREAD.ApcState.Process = EPROCESS
  0x4f6800  cmp dword [eax+0x158], 0  ; EPROCESS+0x158 = VdmObjects
  0x4f6807  jne 0x4f680c             ; VDM -> classify
  0x4f6809  xor eax,eax; ret          ; NOT a VDM -> return 0 (no reflect)
  0x4f680c  ... lea esi,[0x714]; call 0x564ed5 (safe-read [0x714]); test eax,8 (bit3) ...
  ```
  So the reflect **hard-requires `EPROCESS+0x158` (VdmObjects) ≠ 0**, then classifies on `[0x714]` bit 3
  (matches run 66). `0x564ed5` = a ProbeAndRead-a-user-dword-with-SEH helper (reads `[0x714]` safely).
- **The surrounding region IS KiTrap0D's in-kernel VDM instruction emulator.** Bytes just before
  `0x4f67f8` are `out dx, eax` + a cluster of jumps (`0x4f679b/0x4f67b5/0x4f67da`), and `0x4f675e-0x4f67c4`
  is the `[0x714]` monitor-state manager (copies guest IF bit9 into `[0x714]`, manages bits 14/18 from
  `ebx&0x4000` / `[ebp-4]&0x40000`, gated on global `[0x47c798]` bits 1/2/3, inside an fs:[0]=`[0xffdff000]`
  SEH frame). ⇒ **the kernel EMULATES privileged VDM ops (e.g. `OUT`) in-kernel**; `0x4f67f8` is just the
  one branch that reflects to the monitor.
- **Why run 71's bp never hit:** `0x4f67f8` is reached by a *jmp* from the opcode dispatcher; our raw `HLT`
  is decoded to a different (terminate) branch. `/c call 0x4f67f8` finds no direct call — it's dispatched.

**Reframe + testable next steps (the actionable output):**
1. **`HLT` was a MISLEADING probe.** The kernel emulates the privileged ops *games* use (`IN/OUT` for
   VGA/sound, `CLI/STI` for IF) but has no case for `HLT` → terminate. So pfrun/pmfault's HLT proves
   nothing about game viability. **NEXT PROBE: replace HLT with `OUT DX,AL` (a VGA-style port write) in a
   pmfault-style .COM and observe** whether KiTrap0D emulates it (guest continues; ideally the write
   reaches our VDD) vs terminates. That directly tests whether real-CPU PM I/O virtualization already
   works for our VDM — potentially unblocking games *without* any reflect.
2. **Verify our VDM sets `EPROCESS+0x158` (VdmObjects).** The reflect decision returns 0 without it.
   Confirm our `NtVdmControl(VdmInitialize)` (src/vdm/v86.c) establishes VdmObjects the way ntvdm's full
   init does; if a further service call is needed, that's a concrete gap.
3. **RE KiTrap0D's VDM #GP dispatch head + opcode table** (upstream of `0x4f67f8`) to enumerate which
   opcodes route to emulate vs reflect vs terminate. Landmarks: emulator body ~`0x4f6740-0x4f680c`,
   `[0x714]` manager `0x4f675e`, monitor global `[0x47c798]`, reflect-decision `0x4f67f8`, safe-read
   `0x564ed5`, EPROCESS+0x158=VdmObjects, KTHREAD+0x44=ApcState.Process.

### Run 72 (2026-08-11) — BREAKTHROUGH: the kernel reflects a real-CPU PM `OUT` as **event 0 (I/O)** — PM I/O virtualization WORKS; only host servicing is missing `[FACT — VM-confirmed]`

Direct test of RE session 8's hypothesis ("HLT was a misleading probe; the game-relevant privileged ops
are kernel-virtualized"). Built `tools/dostest/outprobe.asm` (identical real→PM path to pmfault, but fires
`OUT DX,AL` to VGA DAC ports 0x3C8/0x3C9 instead of `HLT`) + `outrun.bat`; ran it real-CPU (host
`dpmi-harness-v62`, `g_dpmi_use_interp=0`) via an autorun CD — **no KD, no breakpoint, so no reboot risk**.

serial.log:
- switch to PM OK (`CS=0f:12c`), INT sites patched, printed `OUTPROBE: in PROTECTED MODE -- about to OUT`.
- then: **`DPMI: unexpected PM stop event=0x00000000 CS:EIP=0f:0x138`** — and the watchdog `b@enter` bytes are
  `ba c8 03 b0 00 ee ...` = `mov dx,0x3C8; mov al,0; OUT DX,AL`, with the guest frozen on the `ee` (OUT) at
  EIP 0x138. It did NOT print "OUT survived" — the PM loop spun on the unhandled event → watchdog terminated.

**Interpretation — the key result for #18/#19:** unlike `HLT` (which the kernel silently terminates, run 71),
a PM `OUT` is **trapped by the kernel and reflected to our monitor as `VTIB_EVENT=0`** — the *identical*
I/O-trap event our V86 device path already handles ("I/O-port traps reflect as event 0", M3). So **real-CPU
protected-mode I/O virtualization already works at the kernel level.** The wall was never "the kernel won't
reflect our faults" — it was that our DPMI PM loop only services `event==4` (BOP) and the `+0x638` reflect,
so it treats `event==0` as an "unexpected PM stop" and spins. HLT genuinely terminates (no kernel case), but
the port I/O games use for VGA/sound is virtualized.

**Actionable, bounded, off-VM-codeable next step:** in `main.c`'s DPMI PM execution loop (the `for steps`
after `dpmi_enter_pm`), handle `event==0` the same way the V86 service loop does — decode the port from
`VTIB_EVENT_INFO` and dispatch through `host_try_io` / the device bus (VDD), then advance the guest EIP past
the faulting I/O instruction and re-enter. Then re-run outprobe → expect `OUT survived` + a port-0x3C8 hit in
the VDD. This routes real-CPU PM port I/O to our VDDs and is the concrete unlock for the game class (VGA/
sound), independent of the (dead-end) `#GP`-reflect. CLI/STI (PVI) and the EIP-advance width per I/O opcode
(IN/OUT AL/AX, imm8/DX forms) are the follow-on details. Probe + runner: `tools/dostest/outprobe.asm`,
`outrun.bat` (autorun-CD trigger).

### Run 73 (2026-08-11) — FIX SHIPPED + VM-CONFIRMED: real-CPU PM port I/O is serviced through our VDDs `[FACT — VM-confirmed]`

Implemented the run-72 next step. `src/host/main.c`: new `host_try_io_pm()` (a PM-addressed twin of
`host_try_io` — decodes the faulting `IN/OUT` at `dpmi_sel_base(CS)+EIP` instead of V86 `CS<<4:IP`,
dispatches via `vdd_bus_io`, advances the guest EIP past the insn), wired into the DPMI PM loop: on
`ev==VDM_EVENT_IO (0)` / `GPFAULT (2)`, service via `host_try_io_pm` and `continue` (else fall through to
the old dispatch/stop). Cross-compiles clean, import-allowlist KERNEL32-only.

VM result (real-CPU, `g_dpmi_use_interp=0`, `outprobe.com` via autorun CD):
```
OUTPROBE: in PROTECTED MODE -- about to OUT 0x3C8/0x3C9 (VGA DAC)...
OUTPROBE: OUT survived -- guest RESUMED (kernel emulated the I/O!).
OUTPROBE: done, exiting cleanly.
INT21h AH=4Ch -> client EXIT after 5 svc
```
The guest executed BOTH real-CPU protected-mode `OUT`s (ports 0x3C8 then 0x3C9), our host serviced each
through the VDD bus and stepped past it, the guest resumed, and the client ran to a clean `4Ch` exit
(contrast run 72: same probe on the old host spun on event 0 → watchdog terminate). **So real-CPU PM
port I/O now reaches our VDDs — the concrete game-class unlock (VGA/sound), on the real CPU, with no
`#GP` reflect.** Probe: `tools/dostest/outprobe.asm`.

**End-to-end CONFIRMED (`ioverify.com`, VM-confirmed):** wrote a DAC palette entry via PM `OUT` (idx5 =
`0A/14/1E`) then read it back via PM `IN` → `IOVERIFY: read back = 0A 14 1E` (exact match) + clean `4Ch`
exit. So `host_try_io_pm` services BOTH the `OUT` and `IN` directions in PM and the video VDD's palette
state actually changed (not just EIP advancing) — the fix is real end-to-end, and the `IN` path works.
Follow-ons: CLI/STI (PVI) + INT-in-PM if a real client needs them; then drive a real PM VGA graphics
client (mode set + framebuffer) on the real CPU and see it render through the VDD (milestone #6 payoff).
Probes: `tools/dostest/outprobe.asm`, `tools/dostest/ioverify.asm`.

Note (autorun gotcha): XP caches autorun by volume LABEL — re-mounting the SAME label does NOT re-fire;
rebuild the CD with a fresh label to re-trigger (this run needed a fresh-label rebuild after a no-fire).

## References
- [ntvdmcontrol-and-v86.md](ntvdmcontrol-and-v86.md) — the `VDMSERVICECLASS` enum, VDM_TIB/CONTEXT
  offsets, the V86 keystone.
- [ntvdm-architecture.md](ntvdm-architecture.md) — overall ntvdm component map.
- [ADR-0004](../decisions/0004-reuse-kernel-vdm-ntvdmcontrol.md) — reuse the kernel VDM monitor.
- XP `ntvdm.exe` disasm (`reverse/`): `fcn.0f00532e` (PE test + `VdmPMCliControl`), `getMSW`
  `0xf0041b5`, `DispatchInterrupts` `0xf00442c`, `DpmiSetIncrementalAlloc` `0xf04fe94`, the
  `2Fh 1687` site `0xf02d39f`.
