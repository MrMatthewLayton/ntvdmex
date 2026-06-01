# Roadmap

Milestones are ordered by **risk and dependency**, not by feature glamour. Each milestone
should end in something runnable/observable. Win16 is intentionally late: it is built on the
same V86 + DOS foundation as everything before it.

> Status legend: ⬜ not started · 🟡 in progress · ✅ done · ⛔ blocked

## M0 — Feasibility 🟡
Prove the premise before writing real code.
- [ ] **Spike-001:** V86 keystone via `NtVdmControl` (the make-or-break test) — see spikes/
- [ ] Confirm WOW registry repoint launches our binary as the VDM support process
- [ ] Confirm we can build XP-targeted usermode + kernel binaries (toolchain decision)
- **Exit criteria:** one real-mode instruction executed in V86 under our host, fault reflected to us.

## M1 — Minimal V86 host ⬜
- [ ] Process bootstrap: reserve low 1MB+ at vaddr 0, set up VDM_TIB
- [ ] Enter V86, run a hand-written real-mode COM stub
- [ ] I/O-port trap + interrupt reflection plumbing (no devices yet — just route to stubs)
- **Exit:** a real-mode program that does INT 21h AH=09h prints a string to our console.

## M2 — DOS kernel ⬜
- [ ] INT 21h dispatcher, PSP, FCB/handle file I/O mapped to host (Win32) calls
- [ ] DOS memory management (MCBs, allocate/free/resize)
- [ ] COMMAND.COM-equivalent shell + .COM/.EXE (MZ) loader
- **Exit:** run a real-world DOS .EXE that does file + console I/O.

## M3 — Device model + video/input ⬜
- [ ] Pluggable **VDD** interface (the third-party hook point, requirement #13)
- [ ] Video: trap text/VGA memory + INT 10h, render into a **Luna-themed window**
- [ ] Keyboard + mouse (INT 16h / INT 33h) from host events
- **Exit:** a DOS app with text-mode UI runs in a themed window.

## M4 — Memory extensions ⬜
- [ ] XMS, EMS, and **DPMI** (protected-mode DOS extenders, e.g. DOS/4GW titles)
- **Exit:** a DPMI/DOS-extender game or tool runs.

## M5 — Win16 / WOW foundation ⬜
- [ ] WOW bootstrap (`wowexec` analog), 16-bit `krnl386`/`user`/`gdi` hosting
- [ ] NE loader, 16-bit module/segment management
- **Exit:** a trivial Win16 .EXE loads and reaches its message loop.

## M6 — Win16 thunking ⬜
- [ ] 16:16 ↔ flat pointer translation; generic/flat thunks
- [ ] USER/GDI 16-bit objects mapped to Win32 handles; message bridging
- **Exit:** a real Win16 GUI app runs and paints.

## M7 — Peripheral VDDs ⬜
- [ ] Sound, networking, serial/parallel, etc., as pluggable VDDs (host-backed)
- [ ] Bare-metal vs virtualized device strategy per [risks.md](risks.md)

## M8 — Polish & SDK ⬜
- [ ] Pluggable VDD/driver SDK + docs for third-party developers
- [ ] Luna theming pass, full-screen story, installer/registration tooling
