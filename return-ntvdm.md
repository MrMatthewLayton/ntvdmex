═══════════════════════════════════════════════════════════════════════════════
██ CHECKPOINT — 2026-08-19 (session 9). READ THIS FIRST ON RESTART. ██
═══════════════════════════════════════════════════════════════════════════════

▶ RESTART POINT (2026-08-19, IDE restart): HEAD = 2fb8d27 + doc commit; host = dpmi-harness-v81
  (staged on the share bm/); branch spike/dpmi-16bit-switch; 18 commits UNPUSHED. Working tree clean
  except the pre-existing untracked files (MAINICON.ico, demos/, scripts/kd_*.py, etc. -- NONE mine).
  Box = healthy resting state (watcher looping, controld v2 running). REMOTE CONTROL FULLY WORKS:
  `echo reboot > /tmp/xpshare/control.txt` reboots; `echo kill > .../control.txt` unwedges a hung host;
  `printf 'capture\r\n' > /tmp/xpshare/capture.flag` enables self-screenshots; fire tests via
  `printf '<name>\r\n' > /tmp/xpshare/cmd.txt` then read result_<name>.log + shot_<name>_*.bmp.
  Remount after a reboot: `mount_smbfs -N //guest@192.168.1.34/ntvdmex /tmp/xpshare` (LAN ops need
  dangerouslyDisableSandbox). THIS SESSION'S ARC: cracked the bare-metal PM wall (v68), proved 32-bit
  async IRQ visually (pm32irq, remote self-screenshots), built the remote-test rig (controld daemon +
  game-aware harness + self-screenshots), and brought up Skyroads -- which LOADS + RUNS but is blocked
  in sound/PIT-timing init (see the "SKYROADS BRING-UP" section below). NEXT = user's call: Doom (DOS/4GW,
  setup can disable sound) vs the sound epic vs another game. The retro I/O reflect fix (v74/v81) is the
  key durable bug fix this session -- real-mode direct port I/O now services on this hardware.

★★★ THE BARE-METAL PM WALL IS CRACKED (16-bit AND 32-bit DPMI real-CPU PM run on real
silicon). Session 8's "the kernel declines to run our PM VDM" diagnosis was WRONG: the
event-3-at-entry was NOT a kernel PM-init gap -- it was OUR OWN monitor's interrupt-pending
guard in src/vdm/dpmi_enter.S (label 2, lines 45-51 & 122): when the client enters PM with
IF=1 (VTIB_EFLAGS_PM=0x202) AND ds:[0x714]&3 signals a pending hardware interrupt, dpmi_enter
reports VTIB_EVENT=3 and DOES NOT enter the guest (EIP stuck at entry -> the exact session-8
symptom: event 3 at `mov ax,0x400`, EIP unadvanced). We run PM IN-PROCESS (far-jmp), NOT via
VdmStartExecution, so while PM executes the kernel is not managing this VDM's interrupt assist
-- [0x714]'s pending bits are STALE real-mode state (a timer IRQ0 latched during the pre-switch
DOS INT 21h calls). On real 3.3GHz silicon a tick is essentially ALWAYS pending at switch time
(QEMU+HVF's dilated clock rarely had one) -> the monitor bailed with event 3 forever.

FIX (committed `c0831b1`, host v68): in the main PM loop, on ev==3 clear the stale [0x714]&3
pending bits and re-enter (bounded to 0x10000 so a genuinely re-arming pending can't spin).
The guard fires EXACTLY ONCE per run then PM executes free -- confirming the bits are stale,
not re-arming (as predicted).

BARE-METAL-CONFIRMED via the SMB loop (real-CPU path, g_dpmi_use_interp=0), each ran the
event-3 guard once then ran to a clean INT 21h 4Ch exit:
  • dpmitest.com  -- FULL 16-bit DPMI surface: 0400/0100/0205/0204/0900/0901/0300/0301/0303
      (real-mode callback with NESTED INT 31h+21h inside it). The headline.
  • outprobe.com  -- 16-bit PM `OUT` to VGA DAC survives + resumes.
  • pm32io.com    -- 32-bit (D/B=1) PM `OUT` reflects + services.
  • pm32sw.com    -- the 32-bit MODE SWITCH itself yields a working 32-bit CS; OUT serviced.
  • pm32flat.com  -- base-0 ~2GB G=1 FLAT selector (the DOS/4GW model) renders via ES:[0A0000h],
      775 svc calls, clean exit. The 32-bit flat model EXECUTES on real HW.

COMMITS THIS SESSION (branch `spike/dpmi-16bit-switch`, all local -- PUSH when ready):
  • `242f884` v67 -- route real-HW I/O reflect (event 3) through host_try_io at 4 sites +
      AUTOEXIT_PATH headless-exit + PM-fault byte-dump diagnostic + filebuf 512KB (session-8 fixes).
  • `777fd40` -- session-8 checkpoint doc.
  • `c0831b1` v68 -- THE CRACK (clear stale event-3 pending guard).
  • `d438552` -- session-9 checkpoint doc.
  • `afafbcb` v69 -- headless robustness (log cap + PM-loop time cap + byte-dump rate-limit). ← HEAD.
  Build clean: `./scripts/build.sh` -> build/ntvdmhost.exe (v69, KERNEL32-only). Staged to bm/.

RIG: RECOVERED + GREEN (selftest 8/8). v69 deployed and RE-CONFIRMED on real HW: dpmitest runs
the full 16-bit DPMI surface + clean 4Ch exit, log is ~3 KB (cap works, no regression).

★ REMOTE-TEST UPGRADE (session 9, box is "not easily accessible" -> minimise human touch):
  • HOST SELF-SCREENSHOT (v70, commit fee5702): present_ddraw_save_bmp() dumps the 8bpp back-buffer
    (occlusion-proof) to C:\ntvdmex\shotNN.bmp every ~2s in headless mode (UI-thread timer, capped 40);
    rt.bat ships them to the share as `shot_<test>_shotNN.bmp`. So GRAPHICAL runs (Skyroads, pm32gfx,
    mode13, pm32irq box-march) are now verifiable REMOTELY by reading/diffing BMPs -- VNC/monitor no
    longer needed. Palette byte-order matches gdi_present (0xAARRGGBB -> RGBQUAD). NOT yet end-to-end
    tested (needs the watcher up).
  • runwatch.bat now SELF-INSTALLS to `%ALLUSERSPROFILE%\Start Menu\Programs\Startup\ntvdmex-watch.bat`
    and refreshes watcher.txt every loop (real heartbeat). After ONE bootstrap double-click, every
    reboot AUTO-STARTS the watcher (box auto-logs in) -> no more per-reboot human action. Combined with
    v69's headless caps (infinite/wedged runs self-exit in 30s), the rig is now largely hands-off.
  • Can't bootstrap autostart purely over SMB: guest can mount only ntvdmex + SharedDocs (=All Users\
    Documents); C$/ADMIN$ are DENIED (simple file sharing), and Startup is outside those shares. So the
    ONE remaining human action is a single double-click of bm\runwatch.bat; it's self-perpetuating after.
  • Shares (smbutil view, guest): ntvdmex, SharedDocs (rw), C$/ADMIN$ (denied), IPC$. Ports: 445/139
    open, 135 closed -> NO remote-exec / remote-shutdown RPC reachable.
  BM SCRIPTS now snapshotted in scripts/bm/ (rt.bat, runwatch.bat, controld.c).

★ v70 WEDGE + v71 BULLETPROOFING + controld (the recovery story):
  • v70 added host self-screenshot; a real-mode run (selftest) then HUNG under v70, and the REAL-MODE
    exec loop had NO headless cap (only the PM loop got one in v69) -> it wedged rt.bat's start/wait
    permanently. VNC is fully dead now (capture returns NO file after a DOS mode switch) + no remote-exec,
    so it could not be unwedged remotely -> the box needs a power-cycle. LESSON: any exec path without a
    headless cap can permanently wedge the rig.
  • v71 (commit bf43e33) BULLETPROOFS it: (1) the real-mode V86 loop now honours PM_HEADLESS_MS=30s too
    (a hung real-mode run self-exits -> would have auto-recovered the v70 wedge in 30s); (2) self-capture
    is now OPT-IN via CAPTURE_FLAG (a file on the share) so non-graphical tests never touch the capture
    path. With real-mode + PM caps, NO run can permanently wedge the rig again.
  • controld.exe (commit 2c8a0e8, scripts/bm/controld.c, build-controld.sh): a tiny XP-safe (no-CRT,
    KERNEL32/USER32/ADVAPI32 only) CONTROL DAEMON, separate from the test watcher, that NEVER runs guest
    code so it can't wedge. It polls `control.txt` on the share: `reboot`->ExitWindowsEx force reboot,
    `poweroff`, `kill`->taskkill ntvdmhost (unwedge a hung host). Writes `controld.txt` heartbeat; singleton.
    rt.bat + runwatch.bat both `start` it (singleton) and rt.bat refreshes the Startup runwatch each run.
  • BOOTSTRAP DONE (2026-08-19): box power-cycled, watcher auto-started from Startup, controld
    bootstrapped via rt.bat. PROVEN: controld receives commands (control.txt) + writes controld.txt
    heartbeat; **`kill` REMOTELY RECOVERED a wedged watcher** (taskkilled a hung ntvdmhost -> start/wait
    returned -> watcher resumed). NOT yet working: **`reboot`** -- v1's ExitWindowsEx reached the handler
    but never rebooted (box stayed up, returned to idle) = a privilege/API detail.
  • controld v2 (commit 6081b81, STAGED as bm/controld_v2.exe): reboot via `shutdown.exe -r -f` primary +
    ExitWindowsEx fallback that REPORTS GetLastError to the heartbeat; new `quit` command. Can't hot-swap
    while v1 runs (singleton mutex + .exe file-locked on share, v1 has no quit). runwatch.bat now
    SELF-UPGRADES controld on start (taskkill controld -> copy controld_v2.exe -> relaunch).
    ▶ NEXT SESSION (one small action): restart the watcher (or Start->reboot) ONCE -> v2 loads ->
      `echo reboot > /tmp/xpshare/control.txt` should reboot via shutdown.exe (heartbeat shows the error
      code if not). Then remote reboot works with no physical access ever again.
  • ALSO OPEN: v71 `selftest.com` HANGS the watcher (real-mode run; the 30s real-mode cap did NOT fire ->
    likely hangs INSIDE v86_run/VdmStartExecution so the loop-top cap never runs). controld `kill`
    recovers it. This hang appeared v70+ (v68 selftest passed) but is NOT the BMP capture (v71 capture is
    opt-in/off). INVESTIGATE next session: what in v70/v71 makes a real-mode guest wander off. Meanwhile
    dpmitest (PM path) is the safe smoke-test. HEAD = 6081b81 (12 unpushed).

pm32irq REFRAMED (it was NOT a code bug). `pm32irq.com` is an INFINITE mode-13h animation demo
(`jmp .frame`; never calls INT 21h 4Ch) -- like animate/bounce/mode13/pm32gfx. Under the headless
SMB auto-exit harness it ran the PM loop forever (wedged rt.bat's `start /wait` -> wedged the
watcher) and logged each INT 31h 0400 yield every iteration -> a 148 MB log FLOOD. The 32-bit
async-IRQ frame code it exercises ALREADY WORKS (dpmi_inject_pm_irq widens the IRET frame to dword
for 32-bit handlers, run 83; see main.c ~line 2034). v69 hardens the host so no runaway can harm
the rig again: 4 MB log cap (log.h LOG_MAX_BYTES), a 30 s headless PM-loop wall-clock cap
(g_headless + PM_HEADLESS_MS -> self-exits so the watcher survives), and byte-dump rate-limit (32).
  ► RECOVERY (if ever wedged again): NO remote-exec (only SMB 445/139; 135 closed, guest-only, no
    net/rpcclient/impacket -> a remote `shutdown` RPC is NOT reachable). Blind VNC input works
    (capture dead): Win+R -> `taskkill /f /im ntvdmhost.exe` -> Enter can clear a hung host, but
    it's a gamble without a screen (an earlier stray Alt+F4 likely closed the watcher window). With
    v69's caps a wedge shouldn't recur; worst case, kill ntvdmhost / restart runwatch.bat on the box.

INFINITE VISUAL DEMOS ARE MONITOR-ONLY: pm32irq, pm32gfx, animate, bounce, mode13, blitfast, and
the vga/vesa demos loop forever and must be watched on the box's PHYSICAL display, NOT the headless
SMB loop (which auto-times-out at 30 s now, so they no longer wedge -- but you still won't SEE them).

★★★ 2026-08-19: pm32irq VISUALLY CONFIRMED on BARE METAL via the host self-screenshots (fully remote,
  no monitor). Enabled capture.flag, fired pm32irq.com; host wrote 11 shotNN.bmp (320x200 mode13),
  rt.bat copied them, I read+analysed them on the Mac: the red 24px box MARCHES x=20->65->116->168->219
  (wrap at 240) -> 30->81->... driven ONLY by [hits] which is bumped ONLY by the hooked 32-bit PM INT 08h
  ISR. ⇒ **async IRQ0 injection into a 32-bit PM timer hook WORKS on the real CPU** (the widened dword
  IRET frame, run 83, is correct) -- the checkpoint's "last 32-bit gap" is CLOSED. The earlier pm32irq
  "hang" was purely the infinite-demo-under-headless-harness issue (fixed by v71's 30s cap), NOT a broken
  async path. Self-screenshot pipeline PROVEN end-to-end. PNGs: build/shots/pm32irq_*.png.

★★ 2026-08-19 SKYROADS BRING-UP (real-mode game via the game-aware harness). Skyroads LOADS + RUNS
  on the host (game rt.bat: folder->C:\game, target SKYROADS.EXE, capture). Surfaced + fixed 3 real host
  gaps (commit 2fb8d27, host v81): (1) ★ RETRO I/O REFLECT -- on this box the IOPL-0 IN/OUT #GP reflects
  as event 3 with CS:IP pointing AFTER the faulting insn (EIP already advanced), so host_try_io (decodes
  AT CS:IP) declined; new host_try_io_retro decodes the IN/OUT ENDING at CS:IP + services w/o advancing
  EIP (fixes real-mode port I/O generally; Skyroads' IN AL,DX vblank poll now flows); (2) OPL2 detect stub
  at 388/389 (status bits toggle so detection loops complete; no FM synth); (3) wall-clock PIT pump in the
  real-mode loop (a heavy I/O-trap loop starves the UI thread that raises IRQ0). BLOCKED: Skyroads hooks
  INT 08h and paces its init on the PIT rate IT programs -- needs real timer/OPL emulation (the SOUND EPIC
  #20/#21), not a quick fix. So Skyroads is sound/timing-init-bound. Remote control ALL WORKING now:
  reboot via `echo reboot > control.txt` (controld v2 shutdown.exe) or the watcher-injection
  `q&shutdown -r -f -t 00` > cmd.txt; `kill` unwedges; self-screenshots via capture.flag.
  OPTIONS from here: (a) SOUND EPIC (OPL2 timer + PIT-rate emulation -- unblocks Skyroads + many games);
  (b) a LESS sound-coupled real-mode game; (c) DOOM (DOS/4GW 32-bit -- the path we cracked; Doom's setup
  can select no-sound, so it may reach gameplay without the sound epic). HOST NOW v81, staged.

▶▶ RESUME — NEXT STEPS (in order):
  1. DONE (2026-08-19): pm32irq 32-bit async IRQ visually confirmed remotely. Next visuals to grab the
     same way (set capture.flag, fire, read shot_*.bmp): pm32gfx (32-bit gradient), mode13, and Skyroads
     (real-mode game -- but MIND the v71 real-mode selftest hang below; a real-mode graphical run may hang
     -> use controld `kill` to recover).
  2. A REAL DOS/4GW extender, then DOOM (the acceptance test) -- the 32-bit flat model executes on bare
     metal (pm32flat) AND 32-bit async timer hooks work (pm32irq), so both prerequisites Doom needs are in.
  3. Broader INT 31h surface as a real extender demands it (0305/0306 raw switch, page-lock, phys-map).
  OPEN BUG: v71 selftest.com HANGS the watcher (real-mode; 30s cap didn't fire -> likely hangs INSIDE
  v86_run so the loop-top check never runs). controld `kill` recovers it. Investigate before trusting
  real-mode graphical runs (Skyroads). PM-path tests (dpmitest, pm32*) are safe + self-cap at 30s.
  (Everything above the session-8 banner is now the authoritative state; session-8's "kernel
   won't run PM" conclusion is SUPERSEDED -- it was our own guard.)

═══════════════════════════════════════════════════════════════════════════════
██ CHECKPOINT — 2026-08-18 (session 8). SUPERSEDED by session 9 above. ██
═══════════════════════════════════════════════════════════════════════════════

BIG PICTURE: this session (1) landed runs 83-86 (committed+pushed), then (2) PIVOTED
TO **BARE-METAL TESTING on a REAL Windows XP box** because QEMU+HVF SIGABRTs on DOS/4GW's
paged 32-bit PM (`mmu_gva_to_gpa`, see [[hvf-paged-pm-blocker]]). On real silicon: **real
mode fully works (selftest 8/8)**; **protected mode hit a NEW, OBSERVABLE frontier** (kernel
returns event 3 the instant we run PM). The HVF VM is effectively RETIRED for this work.

REPO STATE (branch `spike/dpmi-16bit-switch`):
  • COMMITTED + pushed: `af5246a` (run 86 raw keyboard + runs 83-85), `5928ff6` (E0 keys +
    per-byte IRQ1 counter + qmp key-hold). HEAD = `5928ff6`.
  • **UNCOMMITTED (the bare-metal fixes, v65→v67):** `src/host/main.c` + `src/vdm/ntvdm.h`.
    These are GOOD and bare-metal-confirmed for real mode — COMMIT THEM FIRST on resume:
      - ntvdm.h: `VDM_EVENT_IO_HW 3` + taxonomy note (real HW reflects I/O as event 3; HVF used 0).
      - main.c: route event 3 through host_try_io at 4 sites (grep VDM_EVENT_IO_HW); `AUTOEXIT_PATH`
        marker + auto-exit-on-guest-exit (test mode, line ~2849); `filebuf` 128KB→512KB;
        the "GH#18 PM-FAULT ev=.. bytes=.." byte-dump diagnostic in the PM loop (~line 2804);
        version string now `dpmi-harness-v67`.
  • Host builds clean: `./scripts/build.sh` → build/ntvdmhost.exe (v67). KERNEL32-only.
  • New scripts (committed? NO — untracked): scripts/build-game-iso.sh, build-doom-iso.sh;
    games/ is gitignored. tools/dostest/pm32irq.* pm32flat.* were committed in af5246a.

▶▶ THE BARE-METAL TEST RIG — how I drive tests MYSELF (no user clicks). ══════════════════
  BOX: Dell OptiPlex 760, Core2 E8600 3.33GHz, Quadro FX 1800, 4GB, SB X-Fi, XP Pro SP3.
       IP = **192.168.1.34**. On the same LAN as this Mac. (Network ops need
       `dangerouslyDisableSandbox` — LAN IP isn't in the sandbox allowlist.)
  SMB SHARE (THE reliable control channel): `//192.168.1.34/ntvdmex` (GUEST access, no pw).
       Mount: `mkdir -p /tmp/xpshare; mount_smbfs -N //guest@192.168.1.34/ntvdmex /tmp/xpshare`
       Maps to `C:\Documents and Settings\All Users\Documents\ntvdmex` on the box. Read+write.
  VNC (UltraVNC :5900, pw `NTVDMEX`, mirror driver installed): **INPUT works, CAPTURE is
       black + WEDGES after any DOS full-screen mode switch** — UNRELIABLE, do NOT depend on
       it. vncdo venv: `/tmp/vncenv/bin/vncdo -s 192.168.1.34::5900 -p NTVDMEX capture x.png`.
       (Reboot un-wedges it; capture still black on this Quadro even with mirror driver + accel
       off + 16bpp — treat VNC as dead for viewing; use SMB + the box's physical monitor.)
  THE AUTONOMOUS SMB TEST LOOP (robust, VNC-free):
    1. A WATCHER must be RUNNING on the box: `bm/runwatch.bat` (user double-clicks once in the
       console; it loops, drops `watcher.txt` heartbeat). If absent on resume, ask user to run it.
    2. Fire a test:  `printf 'selftest.com\r\n' > /tmp/xpshare/cmd.txt`
    3. Watcher (~2s poll) runs `rt.bat selftest.com`, which: pulls fresh host from bm/, sets IFEO
       Debugger→C:\ntvdmex\ntvdmhost.exe, writes target.txt, drops the autoexit marker, runs
       dosstub (→ntvdm→our host; host AUTO-EXITS when the DOS prog exits), then copies
       C:\ntvdmex\ntvdmhost.log → `result_selftest.com.log` on the share.
    4. Read `/tmp/xpshare/result_selftest.com.log`. The host log includes the guest's console
       output ("==> DOS OUTPUT: [...]") so PASS/FAIL tables are readable.
  DEPLOY A NEW HOST: build → `cp build/ntvdmhost.exe /tmp/xpshare/bm/ntvdmhost.exe`. rt.bat
       auto-pulls it every run — no separate stage step. (Bump the version string to confirm.)
  SHARE LAYOUT: `bm/` = {ntvdmhost.exe(v67), dosstub.com, rt.bat, stageall.bat, runwatch.bat,
       tests/ (32 binaries: selftest, memtest, xms/ems/timer/mouse/key, io*, mode/vga/vesa,
       dpmitest, pmfault, pm32*, i310102, dpmiback, ...)}. Root: result_*.log, watcher.txt, cmd.txt.
  ONLY TEST NTVDMEX (per user): do NOT run stock ntvdm — XP's ntvdm is known-good, and a stock
       full-screen DOS run is what wedges VNC. NTVDMEX is windowed.

FINDINGS THIS SESSION (bare metal): ════════════════════════════════════════════════════════
  • **selftest = 8/8 PASS on real hardware** (DOS mem, File I/O, XMS, EMS, PIT, mouse, keyboard,
    video 13h). Real mode is SOLID. Confirmed autonomously via the SMB loop.
  • **event-3 = the real-hardware I/O reflect.** HVF reflected IOPL-0 IN/OUT as VTIB_EVENT=0;
    real silicon uses **VTIB_EVENT=3**. That was the ONLY diff blocking real-mode I/O (Skyroads
    stopped on `IN AL,DX`). Fixed (VDM_EVENT_IO_HW). host_try_io self-validates so routing is safe.
  • **dpmitest (16-bit PM): the SWITCH works, the kernel WON'T RUN PM.** svc10/11 install the LDT
    (CS=0x0f valid, AR=0xfa present/code/DPL3, base 0x1000), then the very first PM step returns
    **event 3 at a harmless `mov ax,0x0400` (b8 00 04), EIP UNADVANCED** — i.e. the kernel declines
    to execute our PM VDM. On HVF this class of problem was a SILENT terminate; on real silicon it's
    an OBSERVABLE event with full state = real #18 progress. Likely cause (matches old #18 RE): stock
    ntvdm does a fuller PM-run init — `VdmPMCliControl` (NtVdmControl service 13) + PM interrupt-flag
    control (fcn.0f00532e reads getMSW/PE bit) — that our host skips; HVF was lenient, real silicon isn't.
  • Current build: `g_dpmi_use_interp = 0` (real-CPU kernel PM path). Interpreter fallback (=1,
    ran i310102/DPMIBACK on HVF) is UNTESTED on bare metal.

▶▶ RESUME — NEXT STEPS (in order): ════════════════════════════════════════════════════════
  0. COMMIT the uncommitted bare-metal fixes (event-3, auto-exit, filebuf, PM-fault diagnostic).
  1. CRACK the PM-entry event 3 (the Doom/DOS4GW gate, now tractable because observable):
     (a) on the unhandled event 3, try RESUME/step a few times — does EIP advance (per-instruction
         trap) or stick (hard "won't run PM")? (b) RE stock ntvdm's PM-run path and replicate the
         `VdmPMCliControl`/service-13 + MSW/PE init before VdmStartExecution; (c) confirm whether a
         PM `C4 C4` BOP reflects as event 4 on real HW (it did on HVF) — if PM BOPs also changed, the
         INT-patch scheme needs the same event-3 treatment.
  2. Quick win option: build with `g_dpmi_use_interp = 1`, fire `i310102.exe` / `dpmiback.com` — proves
     16-bit DPMI works on bare metal via the interpreter regardless of the kernel PM issue.
  3. Skyroads should now render (event-3 I/O fix) — but it's graphical, and VNC viewing is dead;
     needs the user's physical monitor, or a working capture. Real-mode game validation still pending.

HVF VM: retired for this work but still on disk (vm/xp.qcow2). The Doom shareware was extracted to
  C:\DOOMS on that VM (irrelevant now). Do NOT chase DOS/4GW on HVF — it SIGABRTs on paged PM.

───────────────────────────────────────────────────────────────────────────────────────────────
(Everything below is the SESSION-7 checkpoint — HISTORICAL context; superseded by the above.)
───────────────────────────────────────────────────────────────────────────────────────────────

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

═══════════════════════════════════════════════════════════════════════════════
██ POWER-DOWN CHECKPOINT — 2026-08-13 (session 6). SUPERSEDED — history below. ██
═══════════════════════════════════════════════════════════════════════════════

WHY THIS EXISTS: the PC was powered down mid-track. Everything needed to resume cold
is in this banner; the dated session logs below are history/detail.

REPO STATE (branch `spike/dpmi-16bit-switch`):
  • 3 LOCAL commits, NOT pushed to origin — `735637d` (run 78), `b5e2c5c` (run 79),
    `a1ee68a` (handoff). `git log origin/spike/dpmi-16bit-switch..HEAD` to see them.
    Decide whether to `git push` on resume (was left unpushed intentionally).
  • Working tree clean except pre-existing untracked files (MAINICON.ico, demos/,
    scripts/kd_*.py, scripts/trace_break.py) — NONE are mine this session; leave them.
  • Host builds clean: `./scripts/build.sh` → build/ntvdmhost.exe. tmrhook.com assembles.

VM STATE: powered OFF, clean. No qemu running. Backup intact at
  vm/xp-debugonly-backup.qcow2 (restore → vm/xp.qcow2 if XP wedges). This session did
  NOT boot the VM (user deferred VM-confirm to keep momentum). So TWO things are unverified.

WHAT WAS DONE THIS SESSION (detail in the SESSION 6 block just below, + docs runs 78-79):
  • run 78 = #2b async IRQ0 injection into a hooked PM INT 08h — IMPLEMENTED, code compiles,
    but NOT VM-confirmed.
  • run 79 = #3 (32-bit/DOS4GW) kickoff — landed D/B-aware host_try_io_pm (safe, no
    regression) + a full line-referenced 32-bit plan. Core 32-bit work NOT started (needs VM).

▶▶ RESUME — DO THESE TWO, IN ORDER (both need the XP VM booted):
  1. CONFIRM run 78. Boot VM (`./scripts/xp-vm.sh run`; ~5-6 min, poll screendumps until a
     stable non-black/non-loading frame >2MB). Trigger `tmrhook.com` via an AUTORUN CD with a
     FRESH volume label (hdiutil makehybrid; XP caches autorun by label) — the runner is
     tools/dostest/tmhrun.bat. Take 2 QMP screendumps ≥1 s apart: the red box should MARCH
     across mode 13h with NO input and NO INT 1Ah polling → proves the host is injecting IRQ0
     into the client's own PM INT 08h handler ~18.2×/s. If it marches, mark run 78 a FACT
     (update docs/research/dpmi-under-ntvdmcontrol.md run 78 + [[dpmi-realcpu-pm]] memory).
  2. START #3 for real. FIRST PROBE: write a minimal 32-bit client (16-bit DPMI entry → alloc a
     code selector → INT 31h 0009 set access 0xFA + D/B bit → far-jmp into 32-bit → a 32-bit
     `OUT 0x3C8,AL` → report), the smallest test of "does the kernel reflect a 32-bit PM I/O as
     event 0" (the run-72 gate, for 32-bit). If yes, proceed down the run-79 plan: honor
     client_is_32bit in src/vdm/dpmi.c switch as a base-0 ~2GB G=1 flat selector (NOT 4GB — XP
     NtSetLdtEntries rejects flat-4GB; stock ntvdm runs the same ~2GB cap, so DOS4GW is reachable
     to parity); 1687 BX bit0=1; arm VTIB_FLT_FLAG with client width; widen the 2 catcher IRET
     frames (dpmi_run_callback, dpmi_inject_pm_irq) to dword EFLAGS/CS/EIP; gate the EIP/off
     &0xFFFF masks on dpmi_sel_is32(). Full inventory = docs run 79.

HARNESS GOTCHAS (hard-won): only ONE qemu at a time (`pkill -9 -f qemu-system-x86_64` before a
  fresh launch); never `rm vm/qmp.sock` while qemu holds it; QMP send-key is LOSSY → always
  trigger clients via autorun CD (fresh label each mount); never `pkill` a KD session mid-halt
  (stale-halt wedge = reboot-only). Standing directive: DPMI is NOT done — measure against the
  games bar (Doom/Skyroads/ZAR playable), not the last milestone (see [[dpmi-completeness-directive]]).
═══════════════════════════════════════════════════════════════════════════════

Resume NTVDMEX — GH #18 real-CPU protected mode, via the guest kernel debugger. Session 3 SOLVED the KD tooling (the ~28 "wall" is gone) — the debugger is now a working, reliable instrument. North star unchanged (superset of XP-32 ntvdm; bar = Doom/Skyroads/ZAR).

★★★ SESSION 6 (2026-08-13) — runs 78-79 committed on `spike/dpmi-16bit-switch` (in sync w/ origin as of session start; new commits `735637d` run 78, `b5e2c5c` run 79 are LOCAL, push when ready). VM was NOT booted this session (user deferred VM-confirm to keep momentum). TWO deliverables:
  • RUN 78 (#2b async IRQ0 injection) — IMPLEMENTED, **VM-CONFIRM PENDING**. `dpmi_inject_pm_irq()` in src/host/main.c: on a latched IRQ0, if the client hooked INT 08h in PM (`g_pm_int[8]`, via 0205) and `g_dpmi_vi`, snapshot the interrupted PM ctx, push a 16-bit IRET frame → PM-return catcher on the client stack, vector to the handler, run it through the shared dispatcher, restore ctx on IRET (clears/restores g_dpmi_vi like a real INT). Main-loop wiring keeps polled `pit_int08` + adds `g_pm_irq0_latch` (survives CLI) + `g_in_pm_irq` guard. Catcher-selector install factored to shared `dpmi_ensure_pmret_sel()`. Probe `tools/dostest/tmrhook.asm` (+ `tmhrun.bat`): HOOKS INT 08h; ISR bumps a counter; box marches off the counter with NO INT 1Ah polling → movement proves injection. **CONFIRM: run `D:\tmhrun.bat` via autorun CD; 2 screendumps ≥1s apart, box should march.**
  • RUN 79 (#3 32-bit kickoff) — `host_try_io_pm` now **D/B-aware** (new `dpmi_sel_is32()` reads `g_ldt[].flags&0x4`; picks default opsize + full-EIP step from it). PROVABLY no regression: no current client sets D/B (none uses INT 31h 0009), so all D=0 clients decode identically. Docs run 79 = the full line-referenced 32-bit inventory + ordered plan. RESUME #3 (NEEDS VM): (1) honor `client_is_32bit` in `src/vdm/dpmi.c` switch — build a **base-0 ~2GB G=1 flat** selector (NOT 4GB: XP `NtSetLdtEntries` rejects flat-4GB; stock ntvdm runs under the same ~2GB cap, so DOS4GW reachable to parity); (2) 1687 BX bit0=1; (3) arm `VTIB_FLT_FLAG` with client width; (4) widen the two catcher IRET frames (`dpmi_run_callback`, `dpmi_inject_pm_irq`) to dword EFLAGS/CS/EIP when target is 32-bit; (5) gate the `EIP/off & 0xFFFF` masks on `dpmi_sel_is32`. **FIRST PROBE:** a minimal 32-bit client (16-bit DPMI entry → alloc code sel → 0009 set 0xFA+D/B → far-jmp 32-bit → 32-bit `OUT 0x3C8` → report) — smallest test of "does the kernel reflect 32-bit PM I/O as event 0" (the run-72 gate, for 32-bit).

★★★ SESSION 4 (2026-08-11) — THE FAULT-TRIGGER QUESTION IS ANSWERED (and Session-3's recommendation below is REFUTED). Session 3 said "use a BOP-based DPMI trigger (dpbrun/i31run), NOT raw pmfault." **WRONG — VM-confirmed via KD.** A BOP client can NEVER reach the reflect at 0x4f67f8: NTVDM BOPs are `C4 C4` = an invalid LES encoding ⇒ #UD ⇒ KiTrap06 ⇒ VdmDispatchBop, whereas 0x4f67f8 hangs off #GP ⇒ KiTrap0D (via the "BOP-only" gate 0x565041). DIFFERENT TRAP VECTORS. Proof: ran DPMIBACK on the real-CPU path (host v62, g_dpmi_use_interp=0) with the reflect bp armed — it ran end-to-end (PM switch, 0301 round-trip, clean exit) and the bp was NEVER hit (0 serviced). ⇒ **ONLY a raw privileged-instruction #GP (pfrun/pmfault HLT) routes toward 0x4f67f8.** So the correct trigger is `D:\pfrun.bat` after all — Session 3 had the polarity backwards.
  TOOLING: built `scripts/pmfault_observe.py` — classifies every KD halt (reflect-bp / benign break-in 0x80527bdc / benign LOAD_SYMBOLS 0x8052e4c4 / **UNEXPECTED FAULT**) and dumps ctx+stack+code + single-steps the unexpected one. FIXED a receiver race (it first mixed wait_state_change + get_context → desync → the kernel stalled at a 0xCC at raised IRQL → watchdog REBOOT before pfrun even ran; the reflect bp itself is SAFE — the dpmiback session armed the same bp and idled fine). Now get_context-ONLY, exactly like desktop_trace. The repeated resets pushed XP into its "did not start successfully" recovery menu → restored `vm/xp.qcow2` from `vm/xp-debugonly-backup.qcow2` (clean). VM currently POWERED OFF but clean.
  ★★★ RUN 71 (2026-08-11) — pfrun OBSERVATION SUCCEEDED, and it's the #18 ANSWER (docs run 71). Two fixes cracked it: TRIGGER = an autorun CD (`autorun.inf`→pfrun.bat, `/tmp/ntvdmex-auto.iso`) mounted via QMP the INSTANT the bp arms (a log-watcher auto-fires `qmp.py cd` on `armed h=`) — 100% reliable (this VM's send-key drops chars even at 0.6s/char) AND keeps the guest BUSY so it does NOT reboot (the 4 idle-wait attempts all rebooted; arming the 0xCC at 0x805cd7f8 + letting the dilated HVF guest sit idle destabilizes it). RESULT: pfrun ran real-CPU (serial: PM switch CS=0f:12c → "about to HLT" → serial STOPS), and the KD observer saw **NOTHING** — no reflect-bp hit at 0x4f67f8, no exception, no bugcheck, guest healthy. KD was provably live (pfrun could only run because the observer resumed the guest correctly). **⇒ the kernel's KiTrap0D SILENTLY TERMINATES the VDM on a raw PM #GP — a HANDLED path, so it never reaches the reflect and never breaks KD. Chasing the raw-#GP reflect is a DEAD END** (directly confirms runs 65-69's "invisible fault").
  ★★ OPTION C IS DEAD (2026-08-11), and the DPMI-init RE thread advanced (docs "Kernel RE session 8"). Option C (BOP-patch privileged insns) is blocked two ways: the BOP is 2 bytes (C4 C4) but HLT/CLI/STI/IN-OUT-DX are 1 byte (no in-place swap), AND those are ops the KERNEL is meant to virtualize, not us. RE session 8 (fresh ntoskrnl disasm) reframes #18: `0x4f67f8` is NOT "the reflect entry" — it's a small reflect-DECISION function (`if EPROCESS+0x158 (VdmObjects)==0 return 0; else classify [0x714] bit3`), and it sits INSIDE KiTrap0D's in-kernel VDM instruction EMULATOR (bytes right before it are `out dx,eax` — the kernel emulates OUT for a VDM). Our raw HLT is dispatched to a terminate branch, never reaching `0x4f67f8`. **⇒ HLT was a MISLEADING probe** (the kernel has no HLT case); the game-relevant ops (IN/OUT/CLI/STI) may already be emulated in-kernel for a proper VDM. ★★★ RUN 72 (2026-08-11) — BREAKTHROUGH, the OUT probe answered it: a real-CPU PM `OUT` is trapped by the kernel and reflected to our monitor as **event 0 (I/O)** — the SAME event our V86 device path already handles. Built `tools/dostest/outprobe.asm` (OUT DX,AL to VGA 0x3C8/0x3C9 instead of HLT) + `outrun.bat`, ran via autorun CD (NO KD, no reboot risk). serial: PM switch OK → "about to OUT" → `DPMI: unexpected PM stop event=0x00000000 CS:EIP=0f:0x138` (frozen ON the OUT). **⇒ real-CPU PM I/O virtualization WORKS at the kernel level; HLT was a red herring (no kernel case → terminate).** The only gap is HOST-side: our DPMI PM loop services only event==4 (BOP)+the reflect, so it treats event==0 as "unexpected" and spins → watchdog kills it.
  ★★★ RUN 73 (2026-08-11) — DONE + VM-CONFIRMED: real-CPU PM port I/O now flows to our VDDs. Added `host_try_io_pm()` in src/host/main.c (PM-addressed twin of `host_try_io`: decodes IN/OUT at `dpmi_sel_base(CS)+EIP`, dispatches `vdd_bus_io`, advances EIP) and wired it into the DPMI PM loop on `ev==VDM_EVENT_IO(0)`/`GPFAULT(2)`. outprobe.com now prints "OUT survived -- guest RESUMED" for BOTH OUTs (0x3C8/0x3C9) and exits cleanly (4Ch). So the #18 wall is broken: real-CPU protected-mode port I/O is virtualized to our VDDs, no #GP reflect needed.
  ★★★ RUNS 73b + 74 (2026-08-11) — DONE + VM-CONFIRMED (visual): (73b/ioverify) PM DAC write→readback round-trips through the VDD (`read back = 0A 14 1E`), proving IN+OUT both service end-to-end. (74/mode13) a REAL protected-mode VGA client renders on the real CPU: `INT 10h` mode 13h (now routed to the VDD in PM), alloc an A0000 framebuffer selector (`INT 31h 0000/0007/0008`), `stosb`×64000 pixel fill, and the ntvdmhost Luna window SHOWS the 320×200 gradient. Host changes: patch scan also rewrites `CD 10`; `dpmi_service_pm_int` routes `vec==0x10`→video VDD; new `g_dpmi_done` so the run-52 watchdog stands down on clean exit (keeps the window up). ⇒ real-CPU PM graphics through our VDD, no reflect/interpreter. Milestone #6 capability demonstrated.
  ★★★ RUN 75 (2026-08-11) — DONE + VM-CONFIRMED (visual): real-CPU PM ANIMATION. animate.com loops forever scrolling a mode-13h gradient (fill with a per-frame phase offset + INT 31h 0400 yield); two screendumps 6s apart differ = motion, no watchdog kill. Host: PM loop cap raised to run-until-window-close (g_running); watchdog now kills ONLY a sustained freeze (resets on g_dpmi_iter progress), stands down on g_dpmi_done. So a game-shaped redraw loop runs indefinitely on the real CPU rendering through the VDD.
  ★ DPMI is NOT done — see [[dpmi-completeness-directive]]. Answer "is DPMI done?" against the games bar (Doom/Skyroads/ZAR), not the last milestone. DONE so far (real-CPU 16-bit PM, VM-confirmed): run 72 PM I/O trap=event0, 73 I/O servicing, 73b ioverify round-trip, 74 mode13 static render, 75 animation, bounce (palette via PM OUT + moving box), **run 76 INPUT (INT 16h arrow keys drive a box; also fixed extended-key capture in WM_KEYDOWN, which fixes V86 arrows too; INT 33h mouse routed, untested)**.
  REMAINING (suggested order): (1) DONE input (run 76); (2a) DONE polled timing (run 77 — INT 1Ah + BIOS tick advance in the PM loop, timerbox VM-confirmed); (2b) **async IRQ0 injection to a PM INT 08h hook** (games like Doom HOOK the timer) — SPEC (worked out, not yet built): in the PM loop when `InterlockedExchange(&g_irq0_pending,0)` fires, after `pit_int08`, if `!g_isr_active && g_dpmi_vi && g_pm_int[0x08].sel`: save CS/EIP/EFLAGS/vi, push a 16-bit IRET frame {EFLAGS, g_pmret_sel, DPMI_PMRET_OFF} onto the PM stack (linear = `dpmi_sel_base(SS)+(SP&0xFFFF)`, via pokew), set CS:EIP=g_pm_int[8], `g_dpmi_vi=0; g_isr_active=1`; then in the main PM loop detect the catcher (ev==BOP && csv==g_pmret_sel && eip==DPMI_PMRET_OFF) → restore saved CS:EIP/EFLAGS + vi, `g_isr_active=0`, continue. PREREQ: ensure g_pmret_sel + the DPMI_PMRET catcher BOP are allocated at PM ENTRY (currently lazy on first 0303). Test client: a PM prog that INT 31h 0205-installs an INT 08h handler that ++a counter and moves a box → box moves with NO polling. Watch: don't double-count the BIOS tick (pit_int08 already ++0040:006C); a hooking handler usually doesn't touch it. (3) **32-bit DPMI / DOS4GW** — the big one, Doom's extender (all work so far is 16-bit; needs 32-bit flat selectors + 32-bit PM exec + 32-bit EIP/IO handling; host_try_io_pm masks EIP to 16-bit); (4) **raw mode switch (INT 31h 0305/0306)** for real extenders (RAWJMP7 stalled here); (5) **broader INT 31h surface** — 0002 seg→desc, 0600-0604 page-lock, 0800/0801 phys-map (VESA LFB); (6) **a REAL extender + REAL game** end-to-end (the acceptance test); (7) retire the g_dpmi_use_interp toggle (confirm real-CPU path runs i310102/DPMIBACK). Probes: mode13/animate/bounce/kbdbox + *run.bat. Trigger via AUTORUN CD (fresh LABEL each time; GUI keyboard is lossy). HARNESS GOTCHA: only ONE qemu at a time — never `rm vm/qmp.sock` while a qemu holds it (unlinks the socket → screendump fails); `pkill -9 -f qemu-system-x86_64` to clear leftovers before a fresh launch. Docs: runs 72-76 + RE session 8. Option C below is SUPERSEDED/dead.
  (SUPERSEDED) RESUME (Session 5) = the VALIDATED real-CPU-PM direction is run-68 OPTION C: do NOT rely on the #GP reflect. **Pre-patch identifiable privileged instructions (HLT/CLI/STI/IN/OUT/LGDT/LIDT/…) to `C4 C4` BOPs**, like the existing INT-site scan (`dpmi_patch_int_sites`), and service them via the already-proven host BOP/event-4 mechanism (the same path that runs i310102 + DPMIBACK). NEXT SPIKE: extend the patch scan to privileged opcodes → re-run pfrun (its HLT now a BOP) → confirm the host services it instead of the VDM dying. If that pays off it opens real-CPU PM for the game class; if not, the interpreter (g_dpmi_use_interp=1) already covers 16-bit DPMI and the Sound epic (#20/#21) is the other high-value track. Tooling ready: `scripts/pmfault_observe.py` (passive KD observer, classifies+dumps), `scripts/gtype.sh`, the AUTORUN-CD trigger (build: stage + `autorun.inf` open=pfrun.bat → hdiutil; mount on `armed h=`). VM boot recipe: `./scripts/xp-vm.sh run` → poll screendump sz>2MB, exclude black a50ae736 / loading 5ea20161 → native 1024×768. If resets pile up → XP recovery menu → restore `vm/xp.qcow2` from `vm/xp-debugonly-backup.qcow2`. See docs runs 70-71 + [[kd-guest-debugger-ops]].
  (Session-3 text below is retained for the KD operational recipe, but its "BOP trigger" conclusion in ★ THE LAST MILE is superseded by the above.)

★ SESSION-3 RESULT (2026-08-07): KD TOOLING WORKS END-TO-END. `scripts/desktop_trace.py` breaks in at the idle /debug-only desktop, arms the reflect bp @ 0x805cd7f8, and services the module-load break stream ROBUSTLY (drives continues off get_context as the authoritative halt-detector; the old wall was just a MISSED state-change packet, not corruption). VALIDATED: serviced 28 of pfrun's module loads past the old wall, zero wedge, stayed live. The **/debug-ONLY provisioned image is backed up as `vm/xp-debugonly-backup.qcow2`** (cp over vm/xp.qcow2 to reset; boots normally to desktop — NO re-provision needed). NewState constants: 0x3030=EXCEPTION, 0x3031=LOAD_SYMBOLS.

★ THE LAST MILE (next step): with the bp armed, `pfrun` (RAW PM HLT #GP via pmfault) did NOT hit 0x4f67f8 — it bugchecked/rebooted the guest. The reflect is `KiTrap0D→0x565041(BOP-only)→0x4f67f8`, so a RAW #GP bypasses the BOP-gated 0x565041 and never reaches the reflect. **RESUME = build/mount a BOP-based DPMI trigger (i310102 / dpbrun-style, INT 31h path) instead of raw pmfault, then run `python3 -u scripts/desktop_trace.py` (break-in ~110s → arm → GUI-launch the trigger → catch reflect → 400-step single-step trace).** The tooling is ready; only the trigger needs swapping. GUI trigger recipe: Start(50,748)→Run(232,596)+Enter→type path→OK button ~(188,715), move-away-before-click, be patient (slow VM).

STATE OF THE VM (session 3, 2026-08-06):
- **Fresh XP reinstalled, provisioned, backed up.** `vm/xp-fresh-install-backup.qcow2` = bare fresh install (autologin Test/blank, NO /debug). `vm/xp-break-stuck.qcow2` = provisioned (autologon + /debug on COM2 + IFEO ntvdm→ntvdmhost) PLUS **/break** — halts deterministically at boot init; the halt PERSISTS so you can hammer continue variants with NO reboot between tries (cp it over `vm/xp.qcow2` to reset). Internal snapshot `freshinstall` also exists.
- **KD attach + read is SOLID:** KernBase 0x804d7000, GetVersion 15.2600, read_vmem, get/set_context (verified set_context moves EIP), write_bp/restore_bp all work. Reflect entry static 0x4f67f8 → runtime **0x805cd7f8** (slide 0x800d7000).

THE BREAKTHROUGH (why "resume" seemed broken):
- resume()/cont() were fine. The real obstacle: with **/debug**, the kernel executes an int3 (**DbgLoadImageSymbols**, runtime **0x8052e4c4**) on EVERY module load so a debugger can load symbols — each HALTS the CPU until continued past (advance EIP+1 since byte@EIP==0xCC, then DbgKdContinueApi/DBG_CONTINUE). Confirmed on the wire: Continue is ACK'd with the correct id, kernel resumes, re-breaks at the next module.
- **CRUCIAL corollary:** these module-load breaks ONLY halt when a debugger is CONNECTED. With /debug and NOBODY connected, boot runs normally to the desktop (breaks are no-ops). So **/break was the wrong turn** — it forces a connection during boot, turning every module load into a serviced halt (molasses + the sync wall). The RIGHT architecture: /debug-only (no /break) → boot normally to desktop → connect at the desktop → break-in once → arm bp → trigger pfrun → service pfrun's ~10-15 module-load breaks → catch the reflect #GP.

THE ~28 WALL — SOLVED (was NOT a KDCOM sync bug, despite earlier suspicion):
- At the "wall" the kernel is HALTED and fully responsive (get_context works) — the client had simply MISSED one state-change packet (receive timing) and then waited while the kernel waited for us (mutual stall). NewState decode confirmed the ~28 are LOAD_SYMBOLS notifications (0x3031), NOT int3 exceptions; pc=0x8052e4c4 is a MmIsAddressValid validator the reporting thread sits in.
- FIX (in `scripts/desktop_trace.py`, the current harness): drive continues off `get_context` (returns EIP if halted, None if running) as the authoritative halt-detector; decouple Continue from receive; single-byte break-in (cadence>secs) for zero leftover. Superseded scripts: kd_boot_servicer.py, kd_servicer2.py, trace_break.py (all /break-era; ignore). Off-VM RE env: /tmp/ntvdmex-re/ntoskrnl.exe (HAS symbols; r2 base 0x400000); vm/kddll_ref.c = ReactOS KDCOM source.
- Old /break test-bed (`vm/xp-break-stuck.qcow2`) is now only of historical use — /break forces molasses; the /debug-only backup is the one to use.

TOOLING BUILT THIS SESSION (all in scripts/):
- `kd_boot_servicer.py [bp_runtime_hex] [max_secs]` — poll-attach, arm bp, continuously service the break stream, distinguish the reflect bp (0x805cd7f8) from symbol breaks (0x8052e4c4), single-step-trace on the reflect HIT. **This is the harness to finish** once sync is robust.
- `trace_break.py` — earlier /break attach+trace (superseded by kd_boot_servicer).
- `kdclient.py` — break_in() now sparse (cadence=90, secs=260); do_session deadline 600.

THE RECIPE TO FINISH (once sync is robust):
1. Get a /debug-ONLY provisioned image (restore `vm/xp-fresh-install-backup.qcow2` → re-provision with `vm/provision.iso` autorun, which adds /debug WITHOUT /break). Boots normally to desktop.
2. At the idle desktop, run the (sync-hardened) servicer: break-in once, arm bp @ 0x805cd7f8, then service continuously.
3. Trigger `C:\ntvdmex\pfrun.bat` in the guest (GUI: Start→Run; files already at C:\ntvdmex — do NOT remount provision.iso, its autorun re-runs provision.cmd). The guest runs freely at idle desktop so GUI input works.
4. pfrun → ntvdmhost → PM fault → #GP → reflect bp HIT at 0x805cd7f8 → single-step 400 logging rebased EIP+EAX → SEE the gate that returns 0.
Reflect chain (static VAs, r2 base 0x400000): 0x4f67f8(entry)→0x4f6f67→0x4f6efd(CS:EIP from VDM_TIB[class*0x10])→0x4f6e6f(SS:ESP=[TIB+0x638]:0x1000)→gates 0x4f6d3c(CS)/0x4f6dc0(SS) via 0x45dd5f. class=6 for #GP. **NEVER bp 0x4f6d3c/0x4f6dc0** (hot in normal selector validation → floods KD → bugcheck). Only bp 0x4f67f8 + single-step.

HARD-WON RULES (see [[kd-guest-debugger-ops]]): never `pkill` a KD script while CPU~100% (stale-halt wedge → reboot-only); background python needs `-u` (block-buffered else); `system_reset` is the safe image-safe recovery and provisioning persists; QEMU/qmp ops need `dangerouslyDisableSandbox:true`; screendump-diff (identical over ~6s) distinguishes halted vs live. VM is EXTREMELY slow (~110s break-in latency, multi-min boots). Landmarks: ntoskrnl VA base 0x400000, KernBase 0x804d7000; ntvdm 0x0f000000; host v62 = build/ntvdmhost.exe (g_dpmi_use_interp=0 real-CPU + reflect trampoline; =1 restores the VM-confirmed interpreter). VM launch = `./scripts/xp-vm.sh run` (COM1→vm/serial.log, COM2→vm/kd.sock, QMP→vm/qmp.sock).

FALLBACK (still true): the interpreter (g_dpmi_use_interp=1) already runs i310102 + DPMIBACK end-to-end — 16-bit DPMI is effectively done for the games bar. If the KD sync fight isn't worth it, ship the interpreter and pivot to the Sound epic (#20 SB16 / #21 OPL), the biggest unbuilt piece. But the KD trace is now genuinely CLOSE — only the sync-hardening stands between here and the #18 observation.
