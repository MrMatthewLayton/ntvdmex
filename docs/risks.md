# Risk Register

Likelihood (L) and Impact (I): Low / Med / High. Ordered by severity.

| ID | Risk | L | I | Mitigation | Owner / tracked by |
|----|------|---|---|------------|--------------------|
| R1 | Stock XP `ntoskrnl` won't let a third-party binary drive VDM via `NtVdmControl` (undocumented gate, identity check, or unrecoverable layout) | Med | **High** (kills ADR-0004) | Spike-001 first; custom-driver fallback ready (ADR-0004 §Fallback) | Spike-001 |
| R2 | "Bare-metal VGA/VESA" (requirement #8) is infeasible while the XP GUI owns the display | **High** | Med | Plan A = virtualize video + blit to Luna window (what real NTVDM does windowed); revisit full-screen cooperative path at M3 | M3 / risks review |
| R3 | Win16/WOW thunking is vast and under-documented; ReactOS WOW is incomplete | High | Med | Defer to M5 behind a seam (ADR-0003); lean on WINE + ReactOS for API semantics | ADR-0003 |
| R4 | Recovering undocumented `VDM_TIB`/`NtVdmControl` structures is slow/error-prone | Med | Med | Disassembly + dosemu analog; record offsets in research/; clean-room discipline | research/ |
| R5 | Bare-metal vs VM device divergence (timers, sound, NIC, video) | Med | Med | Pluggable VDD model isolates host specifics; explicit bare-metal validation each device milestone (ADR-0005) | M3/M7 |
| R6 | XP-targeting toolchain friction (WDK era vs modern, CRT compatibility) | Med | Low | Settle toolchain during Spike-001; document in spike notes | Spike-001 |
| R7 | DPMI / DOS-extender protected-mode hosting (not V86) adds a second execution mode | Med | Med | Scope to M4; design address-space manager to handle both real-mode-in-V86 and PM descriptors | M4 |
| R8 | Legal exposure if relying on leaked NT source | Low | High | Prefer behaviour/disassembly-for-interop; flag any questionable-provenance source before use | reference-projects.md |

_Review cadence: re-rate at each milestone exit and after Spike-001._
