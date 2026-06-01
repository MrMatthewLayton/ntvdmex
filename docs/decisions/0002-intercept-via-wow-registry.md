# ADR-0002: Intercept via WOW registry repoint, not binary replacement

- **Status:** Accepted
- **Date:** 2026-06-01
- **Deciders:** Matthew

## Context
We must become the system's handler for 16-bit images (a true replacement, not a right-click
"run with…"). Two ways to do that:

1. Overwrite `%SystemRoot%\system32\ntvdm.exe`.
2. Repoint the registry values Windows reads to decide which support process to launch for a
   16-bit `CreateProcess`.

Signing is **not** the obstacle people assume: XP-32 does not verify user-mode EXE signatures
at launch, so even a file swap is not blocked on signature grounds. `[FACT]` The real
gatekeeper for option 1 is **Windows File Protection (WFP)**, which silently restores
protected `system32` files from `dllcache`. `[FACT]`

The launcher path is registry-driven: `HKLM\SYSTEM\CurrentControlSet\Control\WOW\cmdline`
(DOS VDM) and `wowcmdline` (Win16). `[BELIEF — exact value names to confirm in Spike-001]`

## Decision
Register NTVDMEX by **repointing the WOW `cmdline` / `wowcmdline` registry values** at
`ntvdmex.exe`. Do **not** replace the signed `ntvdm.exe`.

## Consequences
- (+) No WFP fight, no signed-binary tampering, cleanly reversible.
- (+) Satisfies requirement #14 (real replacement, every 16-bit launch routes to us).
- (−) Depends on the exact registry contract, which must be confirmed (folded into Spike-001).
- (−) Other interception layers may still exist (e.g. `ntvdm` referenced elsewhere); to verify.

## Alternatives considered
- **Overwrite `ntvdm.exe` + neutralize WFP** (replace dllcache copy / SFCDisable / patch
  `sfc_os.dll`): rejected as default — invasive, fragile, harder to uninstall. Kept as a
  fallback only if the registry path proves insufficient.
