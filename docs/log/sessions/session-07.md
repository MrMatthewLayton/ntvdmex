# Session 7 — 2026-08-18

> Archived verbatim from `return-ntvdm.md`, which was the single rolling handoff
> file until 2026-08-26. Split by session so it can be read by topic and date
> rather than by scrolling. **Nothing has been edited** — including conclusions
> that later sessions REFUTED, which are kept on purpose: the refutations are
> some of the most valuable material here.

```
═══════════════════════════════════════════════════════════════════════════════
██ CHECKPOINT — 2026-08-18 (session 7). ██
═══════════════════════════════════════════════════════════════════════════════

BIG PICTURE: the 32-bit DOS/4GW real-CPU foundation is now PROVEN END-TO-END and
ALL PUSHED. GH #18 is broken for both 16-bit (runs 72-78) and 32-bit (runs 80-82).

REPO STATE (branch `spike/dpmi-16bit-switch`): **all pushed to origin, 0 unpushed**
  (HEAD = `274b4aa`). New this session: `f86e850` (runs 80-81 + run 78 confirm),
  `274b4aa` (run 82). Working tree clean except pre-existing untracked (MAINICON.ico,
  demos/, scripts/kd_*.py, scripts/trace_break.py, vm/ artifacts) — NONE mine; leave them.
  Host builds clean: `./scripts/build.sh` → build/ntvdmhost.exe (has runs 80-82).

WHAT WAS DONE + VM-CONFIRMED THIS SESSION (all now FACT; docs runs 78/80/81/82,
  [[dpmi-realcpu-pm]] memory):
  • run 78 = async IRQ0 injection into a hooked PM INT 08h — CONFIRMED (tmrhook box
    marched off the injected ISR, no polling; user saw it live). The timer-hook path Doom needs.
  • run 80 = a 32-bit (CS.D=1) PM `OUT` reflects as event 0 and is serviced — the run-72
    gate holds for 32-bit. Prereq: fixed INT 31h 0009 D/B parse (was CH low nibble → now
    `(ECX>>12)&0xF`). Probe tools/dostest/pm32io.asm.
  • run 81 = the DPMI mode SWITCH itself produces a working 32-bit CS: `dpmi_switch_to_pm`
    honors client_is_32bit (D/B=1 CS/DS/SS); main.c derives is32=EAX&1 + mirrors to g_ldt[];
    INT 2Fh 1687 now BX bit0=1. No 16-bit regression. Probe pm32sw.asm.
  • run 82 = a 32-bit renderer: mode 13h + grayscale DAC palette (768 PM OUTs) + a `rep stosd`
    vertical-gradient framebuffer fill → the Luna window showed the ramp. Probe pm32gfx.asm.

VM STATE (IMPORTANT): the XP VM was **left RUNNING** with 2 idle DOS windows (pm32sw +
  pm32gfx). An IDE restart likely orphans/kills that qemu. ON RESUME, start clean:
  `pkill -9 -f qemu-system-x86_64` then `./scripts/xp-vm.sh run` (boots the CURRENT
  vm/xp.qcow2, which is the freshly-restored clean debug-only image from this session).
  If it wedges on the logon logo: `cp vm/xp-debugonly-backup.qcow2 vm/xp.qcow2` then relaunch.

▶▶ RESUME — #3 remaining work for a REAL extender + REAL game (run-79 inventory items 3-5),
  needed once a 32-bit client uses callbacks / async timer hooks / >64K offsets:
  1. Widen the two catcher IRET frames (`dpmi_run_callback`, `dpmi_inject_pm_irq`) + the
     injected-IRQ frame to DWORD EFLAGS/CS/EIP when the target selector is 32-bit
     (`dpmi_sel_is32`). The catcher/trampoline CODE selectors must have D/B matching the client.
  2. Gate the `EIP/off & 0xFFFF` masks in the PM loop / `dpmi_service_pm_int` / `g_pm_int[].off`
     / buffer-offset reads on `dpmi_sel_is32` (note `g_int_vec[]` is only 0x10000 wide — a
     32-bit fault EIP>64K needs a different dispatch key).
  3. Base-0 ~2GB G=1 flat-selector ALLOC test (INT 31h 0000/0007/0008/0009) — the DOS/4GW flat
     model; verify it installs (dpmi_install already does the >1MB→G=1 path) and executes.
  4. Then a real DOS/4GW extender, then a real game (the acceptance test).

HARNESS LESSONS (hard-won this session): **ONE VDM/probe at a time** — 4 concurrent VDMs on
  this time-dilated HVF guest REBOOTED XP, which then wedged >6 min on the dirty-boot logon
  (autochk at 102% CPU) → forced a restore. Prefer a FRESH BOOT per probe. A looping/idle prior
  probe also holds `\\.\COM1`, so later probes get no serial → the SCREENDUMP is authoritative.
  Autorun CD trigger: fresh volume LABEL each mount (`autorun.inf` `open=<run>.bat`), hot-swap via
  `python3 scripts/qmp.py cd <abs-iso>`. Only ONE qemu at a time. See [[vdm-host-test-harness]].
═══════════════════════════════════════════════════════════════════════════════
```
