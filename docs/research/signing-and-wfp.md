# Signing, WFP, and replaceability

Answers the first question that gates the whole project: *"NTVDM is a system file — can it even
be replaced?"*

## Code signing is not the blocker `[FACT]`
- **User-mode EXEs on XP-32 are not signature-verified at launch.** There is no mechanism on
  XP that refuses to run an unsigned/modified `.exe`. So `ntvdm.exe` being part of a signed OS
  does not stop replacement on signature grounds.
- **Kernel-mode driver signature *enforcement* (DSE/KMCS) does not exist on XP-32.** It is a
  Windows **Vista x64+** feature. On XP, unsigned drivers load with at most a warning for PnP
  installs; non-PnP drivers loaded via the Service Control Manager load with none. → our
  custom kernel driver (if needed, ADR-0004 fallback) is loadable.

## The real gatekeeper: Windows File Protection (WFP) `[FACT]`
- WFP (winlogon + `sfc.dll`) watches protected `system32` files. If you overwrite
  `ntvdm.exe`, WFP **silently restores** it from `%SystemRoot%\system32\dllcache` (or prompts
  for install media). So a naïve file swap is reverted.
- Defeating WFP is possible (replace the `dllcache` copy too, `SFCDisable` registry hack,
  patch `sfc_os.dll`) but invasive and fragile.

## Our approach (ADR-0002): don't replace, redirect `[BELIEF]`
- Windows chooses the VDM support process from the registry, not a hardcoded path. Repoint
  `HKLM\SYSTEM\CurrentControlSet\Control\WOW\cmdline` (DOS) and `wowcmdline` (Win16) at
  `ntvdmex.exe`.
- Result: every 16-bit launch routes to our binary, the signed file is untouched, WFP never
  triggers, and uninstall is a registry revert. `[VERIFY exact value names/format in Spike-001]`

## Summary
"Can it be replaced?" → **Yes.** Signing never blocks it; WFP would block a file swap but we
sidestep WFP entirely by redirecting via the registry. The hard part of this project is not
*replacing* NTVDM — it is *re-implementing* what it does (V86 host + DOS kernel + WOW).
