# Session 74 — Duke3D runs, ZAR's VESA modes render, heaven7 renders, Heretic runs

> Session 74. Split out of `docs/STATE.md` on 2026-09-23.

> Verbatim from `docs/STATE.md`. Nothing has been edited or corrected,
> including conclusions a later session refuted — same rule as the rest of
> this archive. See [`README.md`](README.md).

### ★★★★★ s74c (17th, 17:15–18:30) — **DUKE NUKEM 3D RUNS. ZAR'S VESA MODES RENDER. TWO DPMI DEFECTS, BOTH "A 32-BIT CLIENT IS NOT A 16-BIT CLIENT".**

> **Rig `bin\ntvdmhost.exe` = `61093e09` = HEAD `d15a26a`. Stable zip on the share is
> STILL `f3c349d` / `a988c6e6` (`debug\prev\ntvdmhost_prev.exe`) — unchanged, per the
> rule. `debug\prev\ntvdmhost_a988c6e6.exe` is the displaced release host.**
> **By-hand OWED on `61093e09`: (1) Duke3D from `demo\msdos\duke3d` — expect the
> setup/title, demo, sound; (2) ZAR → options → a VESA1 and a VESA2 mode (the by-hand
> crash); (3) the DPMI regression set: Doom, Heretic, Hexen, ZAR VGA, Skyroads,
> heaven7, Notepad — all headless-green here, none eyeballed.** `cfg\` clean, ZAR's
> `USER1.CFG` restored to `VGA_320x200`.

**1. Duke3D "doesn't load at all" was its own exit 0 (`4012e1a`).** The morning's
corpus log already held the answer: the game writes *"You don't have enough memory to
run Duke Nukem 3D … 'Total memory free'"* to stdout and terminates. It sizes itself
from **INT 31h 0500** called from flat 32-bit code with the 30h-byte block on its
32-bit stack (`ES:EDI = 0x2f7:0x045d53xx`); the handler did `esb + (EDI & 0xFFFF)`,
wrote our 64 MB answer at linear `0x53xx`, and the game read its own uninitialised
buffer. `dpmi_rmcs_ptr` had learned this rule from Doom's 0300 mouse RMCS in s6x; it
is now `dpmi_caller_off()` (the caller's CS D/B bit decides 16 vs 32) and **0500 and
0303 (handler DS:ESI, RMCS ES:EDI) use it**. 0500 refuses an unreadable block with
8021 instead of writing it. **Headless: CONs compile, art/palette load, mode 13h,
DEMO1 plays with SB DMA — `runs/s74c_duke/shot01.png` is E1M1.** Duke3D is a bound
DOS/4GW *Professional*; no `DOS4GW.EXE` needed.

**2. ZAR crashed on selecting a VESA mode (`d15a26a`) — reproduced headless by
editing `USER1.CFG` `graphics VideoMode` to `VESA2_640x480` / `VESA1_640x480`.**
4F02 `0x4101` accepted, 0800 mapped the LFB, then **`#GP` on the first `rep stosd`
through `es=0x3af`**. The log had the cause three lines up: `DPMI-LDT: install
REJECTED … 0xC000011A`. ZAR builds the selector with **0009 CX=8092 — DPL 0, G=1** —
then 0007 base, 0008 limit `0x4afff` bytes. NT's `PspIsDescriptorValid` rejects any
non-null LDT entry whose DPL≠3, and G=1 over a raw `0x4afff` field is a 1.2 GB segment
it rejects again. DPL 0 is legal on real DOS only because DOS/4GW runs the client at
ring 0; a ring-3 host takes the client's CPL (the spec says so). **`dpmi_install`
forces DPL 3 on PRESENT descriptors** (first attempt forced it on freed/null ones too
→ 2375 rejections — a null descriptor is the one non-DPL-3 entry NT takes) **and 0008
clears a client-set G flag** (its limit is bytes; the host chooses G). Both paths
in-game at 640×480: `runs/s74c_zar/zar_vesa2_01.png` (LFB) and `zar_vesa1_01.png`
(banked, 4,735 `4F05` calls).

**3. Guards on `61093e09`, all headless, `runs/s74c_guards/`:** Doom/Heretic/Hexen
heartbeats 672/344/646 vs the morning's confirmed 674/359/656, same SB block counts,
same Hexen watchdog wind-down · ZAR VGA 579 vs 591, 0 faults · Skyroads `n8=0
max_ms=7` · heaven7 `0x4170` LFB, 16,389 `4F07` · Notepad (Win16) launches and
X-closes through the package host (`w16close_notepad.txt`) · **offvm 1426/0**.

**4. The other "never worked" guests, read not run:**
- **Chasm, Radiance:** *"Runtime error 200 at xxxx:0091"* — Turbo Pascal's CRT-unit
     `Delay()` calibration overflow on any CPU > ~200 MHz. Identical on real hardware
     and stock ntvdm; the fix is the well-known EXE patch (TPPATCH/ctbppat), not ours.
- **egasikio:** its readme says *"requires an EGA card … doesn't work on VGA-cards"*;
     it detects VGA and exits 0. Correct.
- **Fusion (Russian demo, own DPMI stub): DIAGNOSED, NOT FIXED.** It installs its own
     PM handler for **INT 33h** (`0205 → 0x187:0x55b`) and then issues `INT 33h AX=D021`
     as its *own* syscall; our patched site services vector 33h as the mouse and the
     demo aborts cleanly. The dispatcher chains only vector 21h to a client handler
     (`main.c` ~18376); 10h/16h/1Ah/33h are host-serviced regardless. **The right fix is
     "a client-installed PM handler owns the vector", but DOS/4GW installs passthrough
     handlers on all four for every game on the shelf** (`sel:0x40/0x58/0x68/0xcc`), so
     that change reroutes the whole shelf's BIOS/mouse traffic through DOS/4GW's
     reflector — not for deadline day. Candidate narrow rule: chain when the handler's
     selector is a 32-bit code selector the extender did not create.

**5. Second pass (18:35–19:15, `dbcf44f`, `70fc097`) — the user's next two: "duke3d's
setup program crashes, and mouse clicks still don't work in zar".**
- **ZAR's buttons (`dbcf44f`).** ZAR installs an INT 33h **0Ch event handler from flat
     code** (mask `7e` = buttons only, handler `0x347:0x045d2240`) and polls motion with
     0Bh — so aiming worked and no click could: `mouse_cb_try()` served V86 guests only
     and **dropped a PM client's queue** ("not this path (yet)", counted `cb_pm`). New
     `dpmi_inject_pm_mousecb()` = `dpmi_inject_pm_irq()`'s mechanics with a **RETF
     frame** (CS:EIP, what the handler pops) and AX/BX/CX/DX loaded; 0Ch/14h store a
     full-width EDX from a 32-bit caller. Headless with `keys.txt` `m0`/`m1` (needs
     `qimode.txt`=`20`): **10 injected, 10 returned**, both buttons, press and release.
     The click's *effect* is by-hand: headless capture refuses ZAR's 800×600 window
     snapshot (instrument gap, unchanged).
- **Duke3D SETUP (`70fc097`) was four things**, each named by the log in turn:
     (a) *"DOS/16M error: [8] cannot open file ''"* — SETUP EXECs `.\setmain.exe` with
     its **own env segment** and we handed it over as-is; DOS **always** copies and
     appends `0001` + the program name, which is where DOS/4G finds its LE payload.
     (b) Then it sat at `0x042d16f1` until the **watchdog** killed it — a flat client
     that hooks the keyboard but not the timer never bumps `g_dpmi_iter`; "frozen 3 s"
     read as wedged. The watchdog's own live sample showed the EIP moving; **client code
     at a new EIP now resets the streak** (same idea as the Win16 GetMessage exemption).
     (c) The async timer path refused IRQ0 (`why=6`, app not hooked) and bumped nothing,
     so a Watcom `delay()` had no clock; it now does the BIOS's `0040:006C` bookkeeping
     there, billed to the owed count.
     (d) ⛔⛔ **Then Duke3D itself regressed — *"Playback failed, possibly due to an
     invalid or conflicting IRQ"* — and it was NOT code: `cfg\pmnoirq.flag` had appeared
     at 18:59 containing the watchdog's first log line.** `ntvdmex_path()` used a 16-slot
     ring shared across threads; the watchdog's path pointer was overwritten with the PM
     loop's `pmnoirq.flag` probe before its `CreateFile`, so the watchdog CREATED the
     knob that suppresses every PM IRQ. Two guard runs "passed" under it (their checks
     were too weak to notice no timer). Per-thread ring now (Win32 TLS — `__thread`
     wants libgcc's emutls). **Rule: a DPMI guest with no time → `dir cfg\` first.**
     SETUP's main menu is drawn (`runs/s74c_duke/shot02.txt`) and it lives the whole run.
- Guards on `9448cf27`: Duke3D plays (0x1574 ticks) · SETUP alive · Doom `STAGE2:
     complete` · ZAR VGA 579 HB / 0 faults · heaven7 LFB · Notepad opens/X-closes ·
     QBasic V86 click 2/2 · offvm 1426/0.

**Lessons.** *A guest that "doesn't load" usually said why — read its stdout out of
the log before the DPMI trace* (Duke3D's message was in the morning's log at 08:31).
*A regression need not be code — `dir cfg\` before bisecting* (a flag file with log
text in it). *"Wedged" needs a moving/not-moving test, not a trap count.*
*`& 0xFFFF` on an (E)SI/(E)DI/(E)DX from a client is a 16-bit assumption; grep them
all when one bites* (s6x fixed the RMCS one and left 0500/0303). *A cfg file is a
headless menu* — ZAR's by-hand crash became a 90-second loop.

### ★★★★★ s74b (17th, 07:30–08:50) — **HEAVEN7 RUNS WITH NO FLAG. THE PATCHER KNOT IS UNTIED. VESA'S RUNTIME HALF (4F04–4F09) IS DONE, SPEC-TESTED.**

> **Rig `bin\ntvdmhost.exe` = `e92ce8ab` = HEAD `9938226`. Stable zip on the share is
> STILL `eb466c56` — unchanged, per the user's rule. Rollbacks in `debug\prev\`:
> `f3468d65` (this morning's start), `0d85af53`, `6ba7524a`, `772d8911`.**
> `cfg\` is clean (no diagnostic flags). **Nothing needs the user until they can test
> by hand: heaven7, then Doom/Heretic/Hexen/ZAR, whose INT path changed.**

**1. heaven7 — the knot (`b45137b`).** All seven "INT sites" the eager scan wrote
`C4 C4` over are in `h7.EXE` at object+0x2d8: the LE code object is a PACKED payload
(s74's "runtime-generated" was wrong). Reproduced offline with the real `x86len.h`:
the six false sites score **44–48 votes of 48** — x86 self-synchronisation converges
on random bytes as well as on code; the vote measures convergence, not code-ness.
Entropy separates only at 1 KB windows by 0.3 bits over 281 real sites; rejected.
**Fix: `d32` regions are scanned and logged (`32-bit, NOT WRITTEN: would patch N`)
but never written**; the `#GP(IDT)` arm services on the CPU's evidence and patches on
the way past. 16-bit paths unchanged.
**That exposed a second defect:** the lazy arm restored `VTIB_ESP = fr[6]` — the
frame's **16-bit** SP — while its own comment claimed flat-SS clients were "declined
above". Nothing declined them. heaven7 survived by luck; **Doom (SS flat, ESP
`0x0443xxxx`) would have died on its first lazy INT.** The kernel saves full-width
SS:ESP/EIP in the TIB (`fcs:feip` = SS:ESP, `fss3` = EIP — calibrated on three known
faults); ESP now comes from the slot when SS is 32-bit (and only if its low half
matches the frame), EIP for a flat CS from `sav3`, byte-checked, with the 0501-block
reconstruction kept as a logged cross-check (`AGREE` on every one of 14 heaven7 +
17 Doom sites).
**Graded headless, `runs/s74b_lazy32/`:** heaven7 ×3 renders (22,524 colours) ·
Doom ×3 `STAGE2: complete`, 17 lazy INTs, 0 declined · Heretic/Hexen wind-down
identical to `eb466c56` · **ZAR A/B vs `f3468d65`: same frame, same wind-down
address** (its forced-exit path prints no STAGE2 summary — grade it by capture) ·
Skyroads `n8=0 max_ms=6–7`, guards intact · Notepad opens and closes ×2 · offvm
1396/0.

**2. VESA runtime half (`5be7cb6`, `9938226`) — tests from the PDF first, 31 new
checks, 25 of which fail on the previous code.**
- **4F06/4F07 were accepted and ignored** — the presenter never read the logical
     pitch or the display start, so a page-flipping guest saw page 1 forever with
     `004F` in hand. Now `vesa_stride` + `vesa_origin()` drive the 8bpp pointer, the
     direct-colour conversion and the written-extent probe. Failure codes per spec.
- **4F05 read BL as set/get** (spec: BH set/get, BL window). "Get window A" was a
     SET to whatever DX held. Window B fails; LFB mode fails AH=03.
- **4F08** next-lower width (10→8, 7→6), AH=03 in direct colour, reset to 6 on any
     mode set. **4F09** never called `pal_refresh()` (presenter kept the old palette);
     BL=02/03 → AH=02; DX+CX>256 → AH=02.
- **`pal_refresh()` took the EGA attribute path in VESA 8bpp** (a 4F02 never sets
     `mkind`), so indices 6, 8–15 showed DAC 0x14, 0x38–0x3F. Identity now.
- **4F04 did not exist; AH=1Ch reported 3 blocks and wrote 768 bytes** — the Heretic
     MCB overrun in another function. One 896-byte state block serves both; restore
     re-enters the mode with the don't-clear bit and refuses a foreign buffer.
- **Corpus inventory (`runs/s74b_vesa_corpus/inventory.txt`, 20 targets):
     heaven7 is the ONLY guest on the shelf that calls VESA** — 4F00/01/02/07. Its
     15,900 `4F07` calls are all `(0,0)`, none refused: display start as a flip/vsync
     idiom on a single buffer. **So its 320×176 placement is the demo's own** (most
     likely its CPU-speed render-size pick); no VESA question remains there.
     `graphics\VS87.EXE` produced no summary (died/hung) — not looked at.

**3. 09:00–09:20 — USER-CONFIRMED BY HAND on `e92ce8ab`: Doom, Heretic, Hexen, Zar, Wolf3D,
Skyroads all still play.** heaven7 loaded "in a box". **Closed (`6fce192`, rig `36c872e9`):**
read the guest — new `cfg\memdump.flag` (`<linear> <size>` → `debug\out\memdump.bin` at the
headless deadline) dumped heaven7's UNPACKED image (`runs/s74b_lazy32/h7_memdump_04330000.bin`).
Its help text is `-1 512x384 -2 640x480 -3 800x600 -a low -b average -t no text -n no sound
-l looping`, and a render table `(320,176) (512,280) (640,352) (800,440)` — one letterboxed
picture per screen height. **320x176 is the picture for its DEFAULT screen, 320x240 — an OEM
mode we did not publish.** It fell back to 640x480 and drew the default picture at
`y = 480-240+(240-176)/2 = 272`. Published 320x240x{8,15,16} as S3 `0x151/0x160/0x170`; the
unmodified demo now picks `0x4170` and draws edge to edge (its bars are its own). `h7 -2` =
640x352 in 640x480, also correct. Captures `shots_h7_320/`, `shots_h7_dash2/`.
**Music: heaven7 is GUS-ONLY** — the image scans for `ULTRASND=`, programs base+offset, has no
`BLASTER` string and no `0x2xx` port immediate. A Gravis Ultrasound model (GF1: 32 voices,
1 MB DMA-loaded sample RAM, envelopes, timers, IRQ) is a new device — not a same-day job.
`-n` silences it cleanly. By-hand owed on `36c872e9`: heaven7 default + `-2`.

**4. 09:20–10:35 — the rest of the VBE list (`fac0349` `5f42907` `f8b3410`), rig `7d883a85`:**
- **1024x768 + 1280x1024** (`0x105/0x116-118`, `0x107/0x119-11B`): `NTVDD_FRAME_MAXW/H`
     (1280x1024) in `ntvdd.h` now sizes the presenter's snapshots, its guard, the VESA list's
     cap and `VID_FB_MAX` from ONE number; VRAM 2 → 4 MB.
- **132-column text modes `0x108-0x10C`**: 4F01 answers in characters (model 0, B800),
     4F02 goes through the standard mode-3 set then applies the geometry, 4F03 remembers the
     VESA number until a standard set clears it. Renderer needed nothing — it was already
     `cols x 8` / `rows x cell_h`. QB's 80x25 unchanged on the rig.
- **4F10 VBE/PM** (report/set/get) and **4F15 VBE/DDC** (synthesised EDID 1.3, valid
     checksum, one block) — both from the published interface as Bochs/DOSBox answer; no PDF,
     no oracle, said so in the commits.
- `4F0A` PM interface stays a clean decline BY DESIGN (guests fall back; a proper one is
     position-independent code the client copies that we then have to service).
- video_test 197 → 219 checks; offvm **1418/0**. Guards on `7d883a85`: heaven7, Doom ×2 clean
     (the first post-deploy Doom took the watchdog wind-down — the standing rule, not chased).
- **PCem BOOTS AND EMULATES** (`~/PCem/roms` was missing — PCem created `~/PCem/` itself in
     s71; CMOS now 32 MB + C-first). The guest chain after POST is unobserved: needs the user at
     the Mac once (approve lldb, or PCem's screenshot key). ⛔ **DO NOT `lldb -p` unattended** —
     the pending SecurityAgent prompt wedged every unsigned exec on the Mac for 20 minutes
     (memory `lldb-attach-wedges-the-mac`).
- **heaven7 (`36c872e9`+) default = 320x240x16 LFB edge-to-edge; `-2` = 640x480. GUS-only
     audio; no GUS model.** By-hand owed on `7d883a85`: heaven7, then the six confirmed on
     `e92ce8ab` if you want the new stable to be this build.

**5. 11:00–14:30 — PCem IS AN ORACLE (`42ab7f8` `2451aa8`), and it found two defects
the spec audit had passed.** With the user's eyes for three minutes: DOS 6.22 boots to
`C:\>` in PCem (AMI 486, real IBM VGA), `AUTOEXEC` has the `A:\RUN.BAT` hook.
- `--load_drive_a` is a NO-OP in the wx build; A: is mounted via the config's `disc_a`
     line (rewritten per launch). Guest POST+boot ≈ 100–115 s; PCem writes the floppy image
     THROUGH while running, so the poll loop stands (default 240 s).
- ⛔ **`enable_sync = 1` STALLS THE GUEST WHEN THE WINDOW IS NOT BEING DRAWN** (another
     Space, behind a fullscreen app): CPU spins at ~25 %, nothing progresses. Five "boot
     never finishes" runs were this; every run that worked was one somebody was looking at.
     Both configs now `enable_sync = 0`. `pcem/` is gitignored — the note lives in
     `scripts/pcemoracle.py` and memory `pcem-oracle-setup`.
- New probe **`tools/dostest/p_vesa.asm`**: VbeInfoBlock + every ModeInfoBlock, BIOS
     pointers masked. dosdiff hosts **`pcem`** (IBM VGA) and **`pcem-vesa`** (Diamond
     Stealth 32 = Tseng ET4000/W32p, VESA 1.2 in ROM, `configs/NTVDMEX-VESA.cfg`); rule
     cases may be globs; a probe opts into extra oracles with `; ORACLE-ALSO: <host>`.
- **Found by the oracle, fixed:** VbeInfoBlock **Capabilities D0 = 0** (we honour the
     8-bit DAC but told guests not to ask) · **NumberOfImagePages = 0** for every mode (the
     s74 audit fixed the offset and left "one page" in it) · YCharSize 8 in 200-line modes ·
     DirectColorModeInfo D1 for 5:5:5 · Lin/BnkNumberOfImagePages follow +29.
- **Read and deliberately NOT copied**, each a recorded rule: card version/memory/OEM
     rev · Tseng's write-only-A/read-only-B window pair · 5:5:5 as "16 bpp" · TTY-in-graphics
     bit (we don't draw teletype into VESA modes, so we don't claim it) · 9-dot 80-column
     text cells (we render 8-dot and say 8 — fix by rendering 9, not by lying).
- Bochs and the real ROM split on YCharSize/DC-D1; with both voting those rows are
     DISPUTED; the two modes only Bochs offers abstain it, reason written down.
- **`paritysweep.sh p_vesa`: 131 rows, 128 comparable, 100 %, 3 abstained.**
- `p_vesapm` / `p_lpt` / `p_plan12` — "PCem-blocked" since the programme began — are now
     tagged `ORACLE-ALSO` and being swept (result in the session log / memory).
- ⛔ **NEVER `lldb -p` unattended** (memory `lldb-attach-wedges-the-mac`).
- **14:20 CORRECTION — the "only boots when watched" stall was AMI's "D: drive failure —
     Press F1"** (the seeded CMOS described two disks; the user had been pressing F1 without
     saying so). `enable_sync` was NOT the cause; the earlier commit message is wrong on that.
     CMOS 0x12=F0/0x1A=0/0x24..2C=0, checksum redone; **unattended boot+run ≈ 60 s**. Lesson:
     when a run works only when someone is watching, ask what the watcher DID.
- **`474f7b2e` (`1c8578b`+):** INT 10h AH=00 returns the video-mode FLAG in AL (20h/30h/3Fh —
     AMI ROM and SeaBIOS agree; we returned the mode); 4F0A and unknown 4Fxx return `AX=0100`
     (AL≠4Fh, "no such function" — both real BIOSes) not `014F`. **p_plan12 (PCem-blocked since
     GH #15), p_vesapm, p_lpt: clean vs real BIOSes. p_video: the `bda.crtc` row has the IBM VGA
     ROM's vote at last; 7 rows DISPUTED (IBM ROM vs SeaBIOS on cursor shape 0D0E/0607 and mode 7
     on a colour monitor — card facts, correctly ungraded).** Guards on `474f7b2e`: Doom, heaven7,
     Skyroads, QB, ZAR (capture A/B), Notepad — all clean. offvm 1424/0.

**6. 15:00–16:00 — VESACUBE + HEXEN'S LOADER.**
- **`demo\msdos\vesacube\VESACUBE.COM`** (`tools/vesacube/`, NASM real mode): menu of every
     banked VESA mode from 4F00/4F01; a SOLID cube — outward-wound faces, back-face culled on
     the rotated normal, 4-level flat shading, convex scan-line fill — page-flipped with
     `4F07 BL=80h` when two pages fit, `4F09` shade palette in 8 bpp, vsync on 3DAh with a tick
     fallback (rig: ~800 retrace edges / 930 flips in 12 s). Verified: QEMU screenshot, PCem
     ET4000 clean run, rig host-screenshot in 640x480x8 and 320x240x16. `VESACUBE 111` = 12 s
     headless. **By-hand owed: B, L, N, E, W.** (`scripts/bm/cubeshot.bat` drives the menu with
     rigshot; XP focus rules make it flaky.) My one bug: 4F03 returns the mode in BX, my record
     pointer. ⚠ The headless `capture.flag` shots came out BLACK for this guest while the
     framebuffer stats and a new unit check said the picture was there — instrument loose end.
- **HEXEN'S HI-RES LOADER (`2df5651`):** `planar hi_water=0`. Hexen is a PM (DOS/4GW) guest,
     so no interpreter routes its stores; they land wherever A0000 is mapped. **chain4 was 1
     from reset and only the GUEST's SR4 write changed it** — a real BIOS writes SR4=06h for
     planar modes at the mode set; ours didn't, so every map-mask write took
     `mask_skip_chain4`, the window never left the linear section and the four planes were
     copied on top of each other. AND render_planar/the engine used `st->plane[]`, never the
     host sections. Fix: the mode set programs SR4/SR2 like the BIOS (chain-4 off planar / on
     13h) and repositions the window; `PL(st,p)` = the host's section when present, for the
     engine, BIOS pixel services, latches and renderer alike. **Hexen shows the logo, Raven/id
     marks and skull progress bar** (`runs/s74b_lazy32/shots_hexen/`). Guards on `a988c6e6`:
     Lemmings level screen (0Dh/10h via `lemhp.bat`), Doom, Skyroads, QB CAVE, Heretic (no 12h
     loader in this version). Rig `bin\` = `a988c6e6`; **zip still `eb466c56`.**

**7. 17:10 — RELEASE.** User: *"tested all of the major apps and games with no significant
regressions, and the hexen hires loader works"* on `a988c6e6`. `./scripts/package.sh` →
`dist\ntvdmex-20260917-f3c349d.zip` (host md5 `a988c6e6`, commit `f3c349d`). On the rig from
the package's OWN `bin\`: `pkgtest.bat` install → selftest **8/8** → uninstall → rig host
restored; `pkgw16.bat` Notepad launches through the package host, rig host restored. Old
`ntvdmex-20260916-08824e0.zip` (`eb466c56`) moved to `debug\prev\`; `debug\prev\
ntvdmhost_prev.exe` = `a988c6e6` now; git tag `release-20260917`. **The zip is again
IMMUTABLE until the next confirmed build.**

**Score now:** see the s74b VBE table in the session log — **~82/100**, up from 60.
Open on VESA: `4F0A` PM interface (clean decline; nobody on the shelf calls it),
`4F15` DDC (decline), 1024×768 (presenter cap), and **still no oracle** (PCem needs
its romsets found).

### ✅ s74 RESOLVED — **DOOM'S "4px COLUMNS" WAS `detaillevel 1` IN ITS OWN `default.cfg`. NOT OUR CODE.**

> **It survived a rollback to `eb466c56` — which is the whole tell.** A defect that
> outlives a binary swap is not in the binary. Doom's LOW DETAIL mode (F5 in-game)
> halves horizontal resolution, drawing every column double-width; at 2x present
> scale that is the ~4px stride the user saw. It had been written into
> `demo\msdos\doom\default.cfg` when the game was quit, so it persisted across
> every host build.

**PROVEN, same binary `eb466c56`, only the guest's config changed:**

| `default.cfg` | odd-column-boundary changes (3D view) |
|---|---|
| `detaillevel 1` (as found) | **0** — every column pair identical |
| `detaillevel 0` (restored)  | **8279** — full 1px columns |

Restored to `detaillevel 0` on the rig; original saved as
`runs/s74_doom_regression/default.cfg.lowdetail.bak`. Status-bar labels
("AMMO/HEALTH/ARMS/ARMOR") are legible again, and were not before.

✅ **THE VESA BUILD WAS NEVER IMPLICATED.** Doom on `6d2f9af5` with detail restored:
7756 odd-boundary changes — full 1px columns, same as the confirmed build.

⚠⚠ **MY OWN ERROR, RECORDED.** I declared a headless baseline "renders correctly"
from a 320x200 thumbnail BY EYE. It was already low-detail; column doubling is
invisible at that size. That false baseline is what made a phantom regression look
real and sent me hunting my own VESA changes. **MEASURE THE PIXELS. An image small
enough to eyeball is small enough to lie.** (Second wrong turn in the same hunt --
see the mode-Y `fanouts` false lead below.)

▶ **h7 "did not work when I ran it" is EXPLAINED and still open:** heaven7 only gets
past wall 2 with `cfg\nopmpatch.flag` present, and the headless harness deletes it
after every run -- so a by-hand launch hits the eager INT-site patcher and dies.
That knot is still untied.

### ⛔ s74 (superseded by the block above) — the investigation that got there

> **The user ran Doom by hand on `6d2f9af5` and its raycaster drew ~4px-wide column
> strides instead of 1px. Heretic and Hexen were fine. h7 also did not work by hand.**
> Rig rolled back to the confirmed `eb466c56` immediately (standing rule: roll back
> FIRST on "worse"), stable zip untouched, user's log in `runs/s74_doom_regression/`.
> **The whole VESA branch (`96345f6`) is committed but MUST NOT go back on the rig
> until this is understood.**

**What is ruled OUT, by measurement:**
- **The patcher.** Footprint is byte-identical between the good and broken runs —
     same regions, same counts (`0x17`, `3`, `0x24`, `0x45`), same rejects. Only the
     base addresses moved, because `video_state` grew.
- **The EIP reconstruction (`bb5ae66`).** **Zero** reconstructions in Doom's log.
- **Struct growth aliasing the mode-Y planes.** The plane views are independent
     `CreateFileMapping` sections, not buffers inside `video_state`.
- **A stack blowup from the 2.09MB I added to `present_ddraw`.** Both instances
     (`main.c`, `present_demo`) are `static`. ⚠ But note the struct carries an explicit
     design constraint in its own comment — "stays something a caller can hold by
     value" — which 2.09MB violates in spirit. Worth undoing regardless.
- **Headless Doom on the VESA build renders CORRECTLY** (`runs/s74_doom_regression/
     test/`), same launch shape as the good baseline (`good/`).

⚠⚠ **A FALSE LEAD, RECORDED SO NOBODY REPEATS IT.** The mode-Y counters looked
damning — good run `fanouts=0x70` (112), broken `fanouts=0xdc61` (56,417), i.e.
"more fanouts than swaps". **It is normal.** Re-running Doom on the CONFIRMED build
gave `fanouts=0x44d44` (281,412) against `swaps=0x104e2`. The two logs being compared
were different launch shapes and durations. ⇒ **An A/B between two runs you did not
control is not an A/B.**

▶ **WHERE TO LOOK NEXT.** The screenshot path (`snap` -> BMP) is clean, so the defect
is in the ON-SCREEN path that headless never exercises: `gdi_present` /
`fs_stage`, which is exactly what changed to add the 32bpp snapshot. **Prime
suspect:** `gdi_present` can now set `biBitCount = 32` (the `split || direct`
branch). An 8bpp buffer described to `StretchDIBits` as 32bpp renders **exactly 4x
too wide** — which is the reported signature precisely. NOT PROVEN: `direct` should
be 0 for Doom (`snap_bpp` is assigned from `f->bpp` on every valid frame). Reproduce
it before fixing it; a by-hand run with the per-present values logged would settle it.

### ★★★★★ s74 LATE — **HEAVEN7 RENDERS. VBE 2.0 DIRECT COLOUR + A LINEAR FRAMEBUFFER.** (HEAD `96345f6`, rig host `6d2f9af5` — UNCONFIRMED)

**heaven7 draws its opening corridor ("we used to dream") and its "heaven seven"
title, 640x480x24, 17,707 distinct colours, 874 presents over the full 30s
deadline.** Captures in `runs/s74_vesa/shots/`. It was FOUR walls, three of them
ours — see the blocks below for 1 and 2:

3. **The lazy raw-INT path could not serve a flat 32-bit client** (`bb5ae66`) —
      NT's 16-bit exception frame truncates a flat client's EIP. Reconstructed from
      the client's own 0501 blocks, requiring a UNIQUE site holding `CD <vec>`.
      Serviced raw INTs 62 → 72; the demo reached its own code and printed its own
      `VESA error`.
4. **VESA** (`96345f6`). The eleven VBE sub-functions were all present — what was
      missing was everything a guest FILTERS on: only three 8bpp modes, `MemoryModel`
      hardcoded to 4 (packed) for every mode, no RGB field layout, and **no linear
      framebuffer** (attribute bit 7 and `PhysBasePtr` both 0), with **DPMI 0800
      unimplemented** so the advertisement could not have been honoured anyway.
      Now: twelve modes incl. 15/16/24bpp, a correct ModeInfoBlock, the LFB aperture
      at `VID_VESA_LFB_PHYS`, DPMI 0800/0801, and direct colour converted to ARGB for
      a presenter that gained a 32bpp snapshot beside its 8bpp one.
      **The 8bpp path is byte-for-byte unchanged** — Doom/Heretic/Hexen/ZAR run through it.
      Measured: `BX=0x4112` (640x480x24, LFB) ACCEPTED, aperture `0xE0000000` size
      `0xE1000` = exactly 640*480*3.

⚠ **NOT DONE — the geometry.** The picture is a **320x176 rectangle at (160,272)**
inside an otherwise correct 640x480 frame. x=160 is *exactly* `(640-320)/2`, so the
demo is centring horizontally; y=272 is NOT `(480-176)/2`=152. It calls neither
4F06 nor 4F07, so it is not panning. Unexplained — start here.

⚠ **heaven7 still needs `cfg\nopmpatch.flag`** to get past wall 2 (our eager
INT-site patcher corrupting its generated tables). That knot is NOT untied.

⛔ **HEXEN'S LOADER SCREEN IS NOT VESA** — measured, zero `INT 10h AX=4Fxx` calls in
an 89,636-line run from startup into gameplay. VBE work will not touch it; it needs
its own investigation.

### ▶ START HERE: **SESSION 74 (below) — HERETIC RUNS, USER-CONFIRMED BY HAND. `eb466c56` IS THE CONFIRMED BUILD.** Then s73 evening, 72, 71, 70, 69, 68, 61, 60, 59.

> **s74 close (2026-09-16, late). Rig host `eb466c56` (= `00c780e`'s binary) is
> USER-CONFIRMED BY HAND: Doom, Heretic and Hexen all play.** It is now the rollback
> copy (`debug\prev\ntvdmhost_eb466c56.exe`); the previous ones are
> `ntvdmhost_877eb238.exe` and `ntvdmhost_prev.exe` = `a5cd764b`. The package on
> the share is re-cut from this binary (`dist\ntvdmex-20260916-<sha>.zip`, VERSION.txt
> names host md5 `eb466c56`). Deadline is END OF THE 17th.
>
> The user also reports Doom's wave audio is now **flawless** where it was slightly
> glitchy before. ⚠ **Nothing on the audio path changed** — `23bd9ae..HEAD` touches
> only `vdd_video.c` (VESA 4F00) and one log line in `main.c`; the only timing-adjacent
> commit in the last 60 is `0d19439` (IRQ0 held in service, the 15th), which governs
> the real-mode arm while Doom's DMX ISRs run under the PM arm. Unexplained; not
> claimed. If it ever matters, an INTERLEAVED A/B against `debug\prev\ntvdmhost_prev.exe`
> is the only honest measurement.

### ▶ s74 later — **HEAVEN7: TWO OF OUR BUGS BACK TO BACK — EXEC'S ENV COPY, THEN THE INT-SITE PATCHER.** (HEAD `a7e9de0`, rig host `90f41fb9` — UNCONFIRMED)

The user asked why the heaven7 demo fails. Two things were wrong:

1. **The share's `demo\msdos\heaven7\` held only `h7.EXE`** — no `DOS4GW.EXE`.
      h7.EXE is a DOS/4GW *stub* that loads an external extender; the folder was
      incomplete. Copied in the standard `DOS4GW.EXE` (the same one ZAR ships).
2. **A real host bug, oracle-verified.** INT 21h AH=4Bh with env=0 ("inherit")
      must give the child a **COPY** of the parent's environment. We handed over
      the parent's block itself, and then `dos_psp_build` zeroed its first three
      bytes — so `COMSPEC=` became `PEC=C:\COMMAND.COM`, exactly where DOS appends
      the program name. DOS/4GW reads that slot to find what to load and died with
      `DOS/16M error [8]: cannot open file 'PEC=C:\COMMAND.COM'`. **Doom, Heretic,
      Hexen and ZAR never showed it because their extenders are bound into the game
      EXE — no EXEC, no copy.** Fixed `2a39000`: `dos_psp_build` no longer touches
      the env block; `exec_begin` allocates the child a copy (owned by its PSP,
      freed on exit); `exec_name` is captured verbatim from DS:DX. New relations in
      `p_child`/`p_exec` (copy, count word 0001, name tail/shape, parent intact):
      **5 rows, all MISMATCH before, all AGREE with 6.22 after.** offvm 1366/0.

**Wall 2, found when the user tried it by hand and it still failed (`3d451cf`):
OUR INT-SITE PATCHER, FOR THE FIFTH TIME.** The by-hand log died at exactly the
same instruction as the headless one — a file close that succeeds, then silence,
no STAGE2. The patcher had rewritten **7 "INT sites" inside the demo's own 0x3a000
LE allocation**, vectors `0x41 0x11 0x1a 0x11 0x33 0x08 0x31` preceded by
high-entropy bytes. **Nobody executes `int 08h` (IRQ0) or `int 41h`, least of all
from 32-bit PM code** — it is DATA. heaven7 generates its tables at runtime and
1 byte in 256 of noise is `0xCD`. The proof is in the same run: in DOS/4GW's own
region every patched site is a real `b4 09 cd 21` / `b8 02 09 cd 31`. And the
demo's region was scanned FIVE times — the first four found **zero** sites,
because the content had not arrived yet.

⇒ **The patcher now logs the 16 bytes around every site it patches, before it
patches them.** For five sessions the one thing it never recorded was what IT
clobbered — and for a guest that GENERATES code they are not in the binary at all
(the file has `0b c3` where we patched). `nopmpatch.flag` skips scanning; its
contents are an optional hex minimum region SIZE, so DOS/4GW's ~0x5000 of dense
real INT sites keep their patches while a 0x3a000 code+data+generated allocation
is left alone. **A DIAGNOSTIC, NOT A FIX.**

**MEASURED A/B:** with `nopmpatch.flag`=`20000` heaven7 **no longer dies silently
— it reaches `STAGE2: complete`.** That confirms the patcher was killing it.

**Wall 3 — ROOT-CAUSED, NOT FIXED (`a7e9de0`). THE TWO WALLS MEET, AND THAT IS THE
WHOLE PROBLEM.** With the eager scan excluded from the demo's region, its own
`int 31h` **sixteen bytes past the LE entry point** raises `#GP(IDT)` err=`0x018a`
(= IDT | vector 0x31) and is reflected to DOS/4GW's `#GP` handler instead of being
serviced — so the demo never gets its first DPMI call answered and never sets a
mode. (The "cannot make transparent segment" string on the fault stack was a red
herring: stack residue, not the cause.)

The host has a **lazy** `#GP(IDT) → RAW INT → service + patch` arm, and it is the
architecturally right mechanism: it patches only bytes the CPU **actually executed
as an interrupt**, so it has *no false positives by construction*. It serviced
**62** real INTs in the same run. It could not serve heaven7, for two stacked
reasons:
1. Its guard read `if (gcb && ...)` where `gcb = dpmi_sel_base()` → `g_ldt[].base`,
      which is **0 for exactly the descriptor a flat 32-bit client runs on** (base 0,
      limit 4 GB, D/B=1). It was declining those faults *by accident*, reading a
      legitimate base as "no selector".
2. ⚠ **But fixing that predicate alone is WORSE than the bug** — measured before it
      shipped. **NT hands us a 16-bit exception frame whatever the client is**, so a
      flat client's EIP (which *is* its linear address, the base being 0) arrives
      **truncated to 16 bits**: heaven7 reports `0x231c` for an instruction that lives
      at `~0x0433231c`. `gcb + fr[3]` then names **low memory**, and a chance `CD nn`
      match there would write `C4 C4` into an innocent page — the patcher's own
      failure mode, relocated. **That build was thrown away, not shipped.**

⇒ `a7e9de0` declines on the **real** criterion (the frame is trustworthy only for a
16-bit faulting CS, which is what all 62 serviced faults were) and **logs the reason
once, naming the limitation**. A 16-bit CS with base 0 is now serviced correctly.

**So heaven7 needs the eager scan for its flat 32-bit `int 31h` sites — and the
eager scan is exactly what corrupts its generated tables.** That is the knot.
Either the heuristic gets smarter, or the lazy path gets a frame wide enough to
locate a flat client's instruction. **heaven7 moved two walls in one session and is
still not playable.**

⚠ **The patcher heuristic itself was NOT changed** — Doom/Heretic/Hexen/ZAR are
confirmed on it and the release is imminent. The tightening (e.g. never patching
the hardware IRQ vectors 0x08-0x0F, which no application executes) is written
down, not done.

(`dosdiff`'s rig adapter was fixed to the s73 layout in passing — it had gone
stale and only said so by "no disputes" off one host.)

⚠ **RIG IS ON `f5b86403`** (= `ddedb494` + the patcher's byte logging and the
inert `nopmpatch.flag`), **displacing the confirmed `eb466c56`** (rollback copy
`debug\prev\ntvdmhost_eb466c56.exe`). The EXEC-env change is a strict superset —
it only touches the inherit-the-env path, which Doom/Heretic/Hexen/ZAR do not use —
and it is oracle-clean + offvm-green, but it is **not user-confirmed by hand.**
The package/USB is still `eb466c56` until it is.

### ★★★★★ s74 — **HERETIC RUNS: OUR VESA 4F00 HANDLER WROTE PAST THE CALLER'S 256-BYTE BLOCK AND OVER THE NEXT MCB.** (HEAD `00c780e`, rig host `eb466c56`)

**The find took one log read, not a run.** The s73 chain dump said the MCB at
`0x25f4` was corrupt between "alloc 16 paras" and "free it". The DPMI trace shows
what sits between those two lines — exactly one thing:

```
INT31h AX=0100 BX=0010 -> DOSmem seg=0x25e4        (256 bytes)
RMCS 0300 int=0x10 ... eax=0x00004f00               (VBE: get controller info, ES:DI = that block)
INT31h AX=0101 BX=0347 -> DOSfree
```

`vdd_video.c`'s 4F00 arm wrote the OEM string at **+0x100** and the mode list at
**+0x120** of the caller's buffer, unconditionally. `0x25e4:0x100` **is** `0x25f4:0`
— the MCB. Its signature became `'N'` (of `"NTVDMEX VESA"`), the walk stopped, and
the 480 KB above went invisible. VBE 2.0 §4.3: the block is **256 bytes unless the
caller preset `"VBE2"`**, and the OEM string / mode list belong in the reserved area
at +34. Hypothesis 3 from the s73 block ("something of ours wrote there") — and it
was not a patcher or a probe, it was a BIOS service answering with more than it was
asked for. Doom never probes VESA; Heretic does; that is the whole difference.

**Fixed (`00c780e`):** OEM string at +0x22, mode list at +0x40, 512-byte form and
2.0-only fields only when `"VBE2"` was preset. `video_test` now poisons 256..511 and
checks nothing lands there (fails 3 checks on the old handler). The ENOMEM chain
dump now also prints the 16 bytes at the MCB where the walk stopped.

**Measured on the rig, headless (`runs/s74_heretic/`):** the 64-para allocation
succeeds at `0x25e4`; Heretic sets mode 13h, shows the title screen and **plays its
E1M1 demo** — HUD, weapon, "ethereal arrows" pickup message, SB streaming (`sb_blocks=
0xaa4`), ~2000 INT 33h polls — until the headless deadline (`heretic_shot01/03.png`).
Doom on the same host: mode 13h, SB streaming, deadline — unchanged. offvm **1366/0**.

~~▶ **OWED: a by-hand Heretic run**~~ **Done — the user ran Doom, Heretic and Hexen by
hand on `eb466c56` and all three play.** (The Hexen glance was owed because its VESA
probe, if any, goes through the same handler; it passed.)

> **s73 close (2026-09-16, ~19:00). HEAD `23bd9ae`, rig host `877eb238` = HEAD.
> Package `dist\ntvdmex-20260916-23bd9ae.zip` on the share, fresh-folder verified 8/8.
> USER-CONFIRMED BY HAND TODAY: Doom, Hexen, Zar, Wolf3d all play; QB builds a
> runnable standalone EXE (`MKEXE.BAT`). Deadline is END OF THE 17th, not the 18th.**
>
> **Open, in the user's priority order:** (1) **HERETIC** — see the evening block;
> everything needed is written down and the loop is headless. (2) Win16 polish: the X
> does not close WinMine/Charmap (app-specific — Notepad closes cleanly; `w16close.bat`
> tests it headlessly), Calc/Charmap/Clock draw incorrectly. (3) Bubbles palette,
> Matrix_1 slow. MPLAYER GPFs at `0001:3983` (MCI), not chased.

---
