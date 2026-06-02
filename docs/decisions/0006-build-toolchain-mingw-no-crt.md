# ADR-0006: Build with mingw-w64 (i686) cross-compiler, no C runtime

- **Status:** Accepted
- **Date:** 2026-06-02
- **Deciders:** Matthew

## Context

We need a toolchain that produces a **32-bit** binary that **loads and runs on stock Windows
XP SP3** (ADR-0001/0005), while the day-to-day development happens on a modern macOS host. Two
forces shape the choice:

1. **32-bit only, forever.** V86 exists only in 32-bit mode (ADR-0001), so the target triple is
   permanently `i686`/x86 — never x64. The toolchain must default to that.
2. **Stock XP has no modern runtime.** The CRT a modern toolchain links by default matters. The
   Homebrew `mingw-w64` we have is **UCRT-default**: a normal link pulls `api-ms-win-crt-*.dll`
   (the Universal CRT). **UCRT does not ship with XP** — a CRT-linked binary fails to load there
   with missing-DLL errors. `[FACT — reproduced: a trivial WinMain link imported eight
   api-ms-win-crt-*.dll]`

A native MSVC/WDK toolchain (VS2005/2008-era) was the obvious "period-correct" option, but it
requires a Windows build host and an old IDE, which is friction against the macOS dev loop.

## Decision

Build with the **mingw-w64 `i686-w64-mingw32` cross-compiler driven by CMake**, and **link with
no C runtime** (`-nostdlib -nostartfiles`):

- A CMake toolchain file (`cmake/toolchain-xp32-mingw.cmake`) pins the cross compiler and the
  32-bit target; `CMakeLists.txt` hard-fails if the target is not 32-bit Windows.
- We supply our own entry point (`WinMainCRTStartup`) and the few `mem*` primitives the compiler
  may emit, in `src/runtime.c`. The binary then imports **only** the Win32 system DLLs that ship
  with XP (`kernel32`, `user32`, `gdi32`, `comctl32`).
- The PE subsystem and OS-version fields are pinned to **5.01** so XP's loader accepts the image.

## Consequences

- (+) Standalone XP-compatible binary (~20 KB), no redistributable runtime, fast cross-build
  from macOS; no Windows box needed just to compile.
- (+) Dropping the hosted CRT suits a project that will ultimately manage its own process image
  (low-memory reservation, `VDM_TIB`) — we were never going to lean on libc heavily.
- (−) We own a small freestanding runtime shim, and must keep `mem*` definitions from being
  "optimised" into self-recursive calls (compiled with `-fno-builtin
  -fno-tree-loop-distribute-patterns`).
- (−) No libc conveniences (`malloc`, `printf`, …); when we genuinely need allocation we call
  Win32 (`HeapAlloc`) or static storage. Acceptable, and arguably cleaner.
- Commits us to verifying the **import table stays XP-only** as the codebase grows (a build-time
  check is worth adding before M1).

## Alternatives considered

- **MSVC + WDK (VS2005/2008 era):** period-correct and needed eventually for the kernel-driver
  fallback (ADR-0004), but requires a Windows host and old tooling — rejected for the usermode
  dev loop *now*; revisit if/when we build a driver.
- **mingw-w64 keeping the CRT, targeting legacy `msvcrt.dll`:** the legacy `msvcrt.dll` *is* on
  XP, but this Homebrew build is UCRT-default and does not cleanly retarget the legacy import
  lib. Dropping the CRT is more robust and toolchain-independent.
- **Build a second, msvcrt-default mingw-w64:** more moving parts than supplying ~40 lines of
  runtime shim.
