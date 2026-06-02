# Build toolchain & XP-compatibility notes

How NTVDMEX is built today, and the specific traps that make a binary fail to load on a stock
XP SP3 machine. Decision rationale lives in [ADR-0006](../decisions/0006-build-toolchain-mingw-no-crt.md);
this file is the practical/operational companion.

## Toolchain `[FACT]`

- **Compiler:** `i686-w64-mingw32-gcc` (mingw-w64), 32-bit target. Verified present via Homebrew
  `mingw-w64`. `i686-w64-mingw32-gcc -dumpmachine` → `i686-w64-mingw32`.
- **Build driver:** CMake ≥ 3.16, out-of-source in `build/`.
- **Toolchain file:** `cmake/toolchain-xp32-mingw.cmake` (sets `CMAKE_SYSTEM_NAME=Windows`,
  the `i686-w64-mingw32-*` tools, and find-root modes).
- **Resource compiler:** `i686-w64-mingw32-windres` (manifest + version block).

Build:
```
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-xp32-mingw.cmake
cmake --build build
# or: ./scripts/build.sh
```

## The XP-compatibility traps `[FACT]`

These are the things that silently produce a binary that won't run on XP. Each is handled in
the build; listed here so they are not re-learned the hard way.

1. **UCRT is not on XP.** This mingw-w64 is **UCRT-default**: a normal link imports
   `api-ms-win-crt-*.dll` (eight of them), none of which exist on XP. → We link `-nostdlib`
   (no CRT at all) and supply our own entry point + `mem*` in `src/runtime.c`. Resulting
   imports are **only** `kernel32`, `user32`, `gdi32`, `comctl32` — all stock XP DLLs.
2. **PE version stamps.** XP's loader checks the PE optional-header subsystem/OS version. We pin
   them to **5.01** via `-Wl,--major-subsystem-version,5 --minor-subsystem-version,1`
   (and the matching OS-version pair).
3. **`mem*` self-recursion.** GCC will rewrite a hand-written `memset`/`memcpy` loop into a call
   to `memset`/`memcpy` — i.e. into itself — at `-O2`. `src/runtime.c` is compiled with
   `-fno-builtin -fno-tree-loop-distribute-patterns` to prevent that.
4. **No `malloc`/CRT heap.** With no CRT there is no `malloc`. We use static storage (the single
   `Console`) or will call Win32 `HeapAlloc` when genuine allocation is needed.
5. **XP's SxS parser is strict — keep the manifest minimal and comment-free.** Windows XP's
   side-by-side parser (`sxs.dll`, v5.1) is stricter than Vista+/7. A manifest with an **XML
   comment in the prolog** (between `<?xml?>` and the root `<assembly>`) makes XP fail activation
   at load with **"The application configuration is incorrect"**, even though later Windows loads
   it fine. → `res/ntvdmex.manifest` is kept to the canonical Microsoft Common-Controls 6.0 form
   with no comments; explanatory prose lives in `res/ntvdmex.rc` (windres strips `.rc` comments).
   The manifest can be omitted entirely for diagnosis with `-DNTVDMEX_EMBED_MANIFEST=OFF`.

## Verifying a build is XP-safe `[FACT]`

After building, confirm the import table is XP-only and the version stamps are right:
```
i686-w64-mingw32-objdump -p build/ntvdmex.exe | grep 'DLL Name'
#   -> kernel32 / user32 / gdi32 / comctl32 ONLY (no api-ms-win-crt-*, no msvcr<NN>, no ucrtbase)
i686-w64-mingw32-objdump -p build/ntvdmex.exe | grep -i 'SubsystemVersion'
#   -> Major 5 / Minor 1
```
A build-time assertion of "imports ⊆ {known XP DLLs}" is worth adding before M1, so a stray
modern API can't slip the requirement.

## The Luna look `[FACT]`

"XP Luna theme window" is achieved without any custom theming code: the application **manifest**
(`res/ntvdmex.manifest`, embedded as `RT_MANIFEST` via `res/ntvdmex.rc`) declares a dependency on
`Microsoft.Windows.Common-Controls` **6.0**. That single dependency opts the process into the XP
visual-styles engine (uxtheme), so the standard window frame renders with the Luna title bar and
border on a themed XP machine. No `<trustInfo>`/`requestedExecutionLevel` is included — that is a
Vista+ (UAC) construct, irrelevant on XP.

## Cannot be rendered off-XP `[FACT]`

The binary is a Windows GUI executable; its window can only be *seen* on Windows. On the macOS
dev host (no Wine installed) we verify **build correctness** (valid PE32, XP-only imports,
version stamps, embedded manifest) but **not the visual result** — that must be confirmed on an
XP SP3 VM. This is the same VM-first posture as ADR-0005.

## Open / later

- `[VERIFY]` Pick a debug-build story: keep symbols (`-g`, no strip) for WinDbg-on-VM once we
  start kernel/V86 work; the current Release build is stripped.
- `[VERIFY]` The **kernel-driver fallback** (ADR-0004) will need a *different* toolchain
  (WDK-era / MSVC) — mingw is usermode-oriented. Settle that only if/when Spike-001 forces it.
- `[VERIFY]` Add the import-allowlist build check noted above.
