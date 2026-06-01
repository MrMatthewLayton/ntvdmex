# Reference projects — what each is actually good for

The key insight: **no existing open project does what we do** (real V86 on NT via
`NtVdmControl`). Each reference is useful for a *different* layer. Be precise about which.

| Project | CPU approach | Good reference for | NOT a reference for |
|---------|-------------|--------------------|---------------------|
| **ReactOS NTVDM** | Software emulation (**Fast486**) | DOS kernel logic, VDD interface shape, WOW structures, the *intent* of NTVDM behaviour | V86 / `NtVdmControl` execution — it doesn't use them |
| **Linux dosemu / dosemu2** | **V86** via `vm86()` (then KVM) | The architectural *shape* we want: usermode host + kernel V86 + trap-and-reflect loop; real-mode memory image setup; fault/signal discipline | NT-specific structures (`VDM_TIB`, `NtVdmControl`) — wrong OS |
| **DOSBox / DOSBox-X** | Software emulation | DOS API behaviour, device (VGA/SB/timer) semantics, DPMI/XMS/EMS reference, compatibility quirks | Anything about executing on the real CPU |
| **PCem / 86Box** | Full machine emulation | Accurate hardware device behaviour (VGA/VESA, sound, timers) to mirror in our VDDs | Execution model; far heavier than we need |
| **Shipping XP `ntvdm.exe` / `ntoskrnl` (disassembly)** | **V86** (the real thing) | The *only* ground truth for the `NtVdmControl` contract, `VDM_TIB` layout, low-memory setup | — (this IS the spec we must recover) |
| **WINE (Win16 side)** | n/a (API reimpl) | Win16 API semantics, thunking concepts, NE loader behaviour | Its execution model differs from NT/WOW |

## Practical posture (per user)
- Lean on **ReactOS** for DOS/VDD/WOW *logic and structures* — but never assume it validates
  our V86 path.
- Treat **dosemu** as the conceptual blueprint for the V86 host loop.
- Recover the **`NtVdmControl`/`VDM_TIB` contract from XP disassembly** (clean-room mindful).
- Use **DOSBox/86Box** as oracles for device behaviour when building VDDs.

## Legal note
Prefer the shipping binaries' *behaviour* (disassembly for interop) and clean-room notes over
any leaked NT source. Flag a decision before relying on any source of questionable provenance.
