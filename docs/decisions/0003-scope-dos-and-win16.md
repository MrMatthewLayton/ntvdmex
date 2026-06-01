# ADR-0003: Scope = DOS + Win16/WOW; Win16 deferred behind a seam

- **Status:** Accepted
- **Date:** 2026-06-01
- **Deciders:** Matthew

## Context
NTVDM hosts two distinct guest worlds: **DOS** (real-mode, INT 21h, DPMI/XMS/EMS) and
**Win16** via **WOW** (Windows-on-Windows 16-bit: `krnl386`/`user`/`gdi` thunking down to
Win32). The product goal includes both. Win16/WOW is the single largest and least-precedented
chunk — even ReactOS's WOW is notably incomplete — and it is built *on top of* the same V86 +
DOS foundation as DOS support.

## Decision
Commit to **both DOS and Win16** as the end goal, but sequence Win16/WOW as **milestone M5+**,
after the DOS path is working. Architect the DOS core to leave a **clean seam** for WOW
(loader abstraction, address-space manager, interrupt/thunk dispatch) so Win16 is an addition,
not a rewrite.

## Consequences
- (+) Early, runnable value (DOS) while the riskiest foundation is proven.
- (+) Avoids carrying WOW complexity before V86 + DOS are de-risked.
- (−) Requires discipline now to keep the DOS core WOW-ready (documented seams).
- (−) Total project size is large; Win16 roughly triples the DOS-only effort.

## Alternatives considered
- **DOS-only v1:** simpler, but the user explicitly wants Win16; deferring ≠ dropping.
- **Win16 first:** rejected — it depends on the DOS/V86 foundation, so it cannot come first.
