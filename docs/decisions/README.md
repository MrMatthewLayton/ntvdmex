# Architecture Decision Records (ADRs)

Each ADR captures one significant decision: its context, the choice, and the consequences.
ADRs are **immutable once Accepted** — to revisit one, write a new ADR that supersedes it.

## Index

| ID | Title | Status |
|----|-------|--------|
| [0001](0001-no-cpu-emulation-v86.md) | Execute real-mode code in V86, no CPU emulation | Accepted |
| [0002](0002-intercept-via-wow-registry.md) | Intercept via WOW registry repoint, not binary replacement | Superseded by 0007 |
| [0003](0003-scope-dos-and-win16.md) | Scope = DOS + Win16/WOW; Win16 deferred behind a seam | Accepted |
| [0004](0004-reuse-kernel-vdm-ntvdmcontrol.md) | Reuse kernel VDM via `NtVdmControl` | Accepted |
| [0005](0005-target-vm-and-bare-metal.md) | Target XP in VM and bare metal; VM-first dev loop | Accepted |
| [0006](0006-build-toolchain-mingw-no-crt.md) | Build with mingw-w64 (i686) cross-compiler, no C runtime | Accepted |
| [0007](0007-intercept-via-ifeo-debugger.md) | Intercept via IFEO `Debugger` redirect on `ntvdm.exe` | Accepted |
| [0008](0008-pluggable-vdd-model.md) | Pluggable VDD model (clean ABI + MS-compat veneer; DirectDraw) | Accepted |

## Template

```markdown
# ADR-XXXX: <title>

- **Status:** Proposed | Accepted | Superseded by ADR-YYYY | Deprecated
- **Date:** YYYY-MM-DD
- **Deciders:** <names>

## Context
What is the problem/force? What constraints apply?

## Decision
The choice, stated plainly.

## Consequences
Positive, negative, and what this commits us to. Risks created.

## Alternatives considered
What else was on the table and why it lost.
```
