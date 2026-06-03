# ADR-0007: Intercept via IFEO Debugger redirect on ntvdm.exe

- **Status:** Accepted
- **Date:** 2026-06-03
- **Deciders:** Matthew
- **Supersedes:** [ADR-0002](0002-intercept-via-wow-registry.md)

## Context
ADR-0002 (repoint the WOW `cmdline` at our binary) is **disproven**. On XP SP3 the DOS-VDM host
launch validates the host image: our binary never runs as the host under any substitution —
`cmdline` repoint to any path, *or* physically replacing `system32\ntvdm.exe` (WFP defeated by
also overwriting `dllcache`). Even a byte-copy of the genuine ntvdm fails from a non-canonical
path. Every attempt yields "C:\…\dosstub.com is not a valid Win32 application" with **no STAGE0**
(our log written on the very first instruction never appears). Full constraint table:
`docs/research/ntvdmcontrol-and-v86.md`.

We still need a **true, transparent replacement** (requirement #14: every 16-bit launch routes to
us, nothing run separately). The image-validation wall blocks *being the host image*. But it does
not block being launched *as a normal process in the host's place*.

`[FACT]` **Image File Execution Options (IFEO) works.** Setting
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe`
`Debugger = C:\…\ntvdmex.exe` makes the loader launch **our binary in ntvdm's place** whenever
anything would launch `ntvdm.exe`. Verified on the VM: a 16-bit launch fires our binary, which
ran to completion (`STAGE0 → STAGE1 → STAGE2 → DONE`) **in the real host context** — a live
console window, valid std handles, and ntvdm's actual command line:

```
C:\ntvdmex\vdmhost.exe "C:\WINDOWS\system32\ntvdm.exe" -f -i1
```

Because we are launched as a debugger (a normal process), the host-image validation never applies.

## Decision
Register NTVDMEX via an **IFEO `Debugger` redirect on `ntvdm.exe`**. Do not repoint `cmdline`,
do not replace the signed `ntvdm.exe`.

## Consequences
- (+) **Transparent** — every 16-bit `CreateProcess` that would spawn ntvdm spawns us instead.
  Nothing to run separately; satisfies requirement #14.
- (+) **Registry-only, reversible, no WFP fight, no signed-binary tampering** — a single value.
- (+) **Our code actually runs** in the genuine launch context (console + ntvdm's args).
- (−) We are handed ntvdm's args (`-f` required flag; `-i<n>` instance) but **not** the DOS
  program — that still arrives via CSRSS `GetNextVDMCommand`, which returns `0x57` until we
  replicate ntvdm's pre-fetch registration (`NtVdmControl(VdmInitialize)` at ntvdm `0xf01abb6`,
  `RegisterConsoleVDM(1,…)` at `0xf014078`, console + low-memory setup). That is the next build.
- (−) IFEO redirects *all* `ntvdm.exe` launches; we must eventually do the full VDM job (or, as a
  transitional measure, proxy to the genuine ntvdm) so 16-bit apps keep working.

## Alternatives considered
- **WOW `cmdline` repoint / `ntvdm.exe` replacement** (ADR-0002): disproven — image validation.
- **Standalone host** (`ntvdmex.exe <prog>`): rejected by requirement — must be transparent, not
  a separate invocation.
