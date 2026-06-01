# Spike-001: V86 keystone via `NtVdmControl`

- **Status:** ⬜ Not started
- **Risk addressed:** The make-or-break premise of ADR-0004 — that a non-Microsoft binary can
  drive *real XP SP3* `ntoskrnl`'s VDM into V86. No open-source project proves this.
- **Time box:** keep it minimal (~a few hundred lines + a tiny real-mode stub).

## Question
Can our own `ntvdmex.exe` initialize a VDM via `NtVdmControl`, enter V86 on the real CPU, run
one trivial real-mode instruction sequence, and have the resulting fault/interrupt reflected
back into *our* usermode handler?

## Hypothesis
Yes — `ntoskrnl` does not check caller identity, only that the expected `VDM_TIB` and
low-memory layout are present. If we reconstruct that layout (recovered from disassembly +
dosemu-style discipline), `VdmInitialize` → `VdmStartExecution` will run our code in V86.

## Method
1. **Recover the contract** (from XP `ntvdm.exe`/`ntoskrnl` disassembly): `NtVdmControl`
   service enum, `VDM_TIB` layout + where its pointer lives, required low-memory reservation.
2. **Build host stub:**
   - `NtAllocateVirtualMemory` to reserve/commit the first ~1MB+64KB at vaddr 0; lay down an
     IVT and our real-mode program.
   - Populate `VDM_TIB`; set CS:IP to the stub.
   - `NtVdmControl(VdmInitialize, …)` then `NtVdmControl(VdmStartExecution, …)`.
3. **Guest stub (real-mode):** smallest meaningful test — e.g. `OUT` to a trapped port, or
   `INT 21h AH=09h` to print `$`-terminated string.
4. **Handler:** on fault/interrupt return, log the cause + register state, service trivially,
   resume.

## Success criteria (must hit all)
- [ ] `VdmInitialize` returns success from our (non-MS) process.
- [ ] V86 entered; CS:IP advances through the guest stub on the real CPU (confirm via debugger
      / trap log — not emulation).
- [ ] At least one #GP (port I/O) or software interrupt is reflected into our usermode handler
      with correct register/segment state.
- [ ] Verified in a **VM** and on **bare metal** (ADR-0005).

## Possible outcomes & decision impact
- ✅ **All criteria met** → promote **ADR-0004 to Accepted**; proceed to M1.
- ⚠️ **Init OK but reflection/layout mismatch** → recover more of the contract; iterate.
- ⛔ **Stock XP rejects a third-party VDM host** (identity/path check, undocumented gate) →
  **supersede ADR-0004** with a custom-driver ADR (route 2) and re-scope M1.

## Notes / toolchain to settle as part of this spike
- Build toolchain that targets XP SP3 (driver + usermode): VS2005/2008 + WDK era, or modern
  cross-build with XP-compatible CRT. `[VERIFY]`
- Need syscall access to `NtVdmControl` (via `ntdll` import or direct stub). `[VERIFY]`
- Kernel debugging setup (WinDbg over named pipe/serial to the VM) for trap inspection.

## Result
_(empty — fill in after running: what worked, what didn't, recovered structure offsets, and
the decision taken.)_
