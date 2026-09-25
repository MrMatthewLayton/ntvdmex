# Session 80 — execution chaining: Doom runs from the shell

**Date:** 2026-09-25 · **Branch:** `m9/completeness` · **Rig:** bare-metal XP box, watcher live.
**Host build:** **`2f803674`** on the rig's `bin\` (displaced `1ea8829d` archived as
`debug\prev\ntvdmhost_1ea8829d.exe`; `ntvdmhost_prev.exe` = `d57d586c`, untouched).
**Not yet confirmed by hand.**

---

## The two open questions — answered

1. **Graphics performance bar: "measure first, then decide."** Take the SR2 write-rate
   measurement for Wolf3D / Mario / Doom low detail and bring the numbers back; the
   speed-versus-correctness call is made on the data.
2. **GUS: yes** — work from the publicly archived Gravis GUS SDK, write our own
   `docs/ref/gus.md` citing it. The SDK is not mirrored in the repo.

---

## The result

**`doom` typed at 6.22's `COMMAND.COM` now starts Doom and it plays** (rig, headless, key
script; screenshots `runs/s80_chain/run2_shot0{0,2}.png`: the prompt in `demo\msdos\doom`,
then Doom's demo running). s79's reproduction died in DOS/4GW's own abort within seconds.

**Quitting Doom no longer crashes the host** — but it still ends the VDM rather than
returning to the shell. That is the remaining half of north star 3.

---

## What was actually wrong — not what s79's log appeared to say

s79 read the log as *"the client's PM `AH=4Ch` tears down the VDM, then the host faults"*.
Both are true, but neither is the cause. **Doom never chose to exit.** Decoding the
`AH=06h` calls made inside the final keyboard ISR gives DOS/4GW's abort message, printed
one byte at a time:

```
DOS/4GW Professional error (2002): transfer stack overflow on interrupt 09h at 1BF:00000070
```

The chain:

1. DOS/4GW hooks PM `INT 09h` at startup (`setPMvec 09 = 1cf:0024`) with a pass-up
   handler that chains to the previous PM vector — our `C4 C4 CF` default stub.
2. The key that launched the program (Enter's break code) arrives **before Doom installs
   its own `INT 09h`**, so DOS/4GW's handler takes it and chains to our stub.
3. The PM dispatcher had **no arm for vector 09h**: "unexpected PM stop" at `177:001b`,
   the injector abandoned the ISR at 0 phases, and the key stayed pending — re-offered and
   re-abandoned every pass. Eleven times.
4. Each abandonment leaked one frame of DOS/4GW's per-interrupt transfer stack. The twelfth
   overflowed it, and DOS/4GW ran its abort **from inside the ISR** → PM `AH=4Ch`.
5. The injector read that `0` as "the handler did not IRET", logged `PM ISR ABANDONED`,
   restored the dead client's context, and the main loop entered it again — every
   selector freed — and the host access-violated in the fault trampoline
   (`1f 0f a9 0f a1 61` = `pop ds / pop gs / pop fs / popa`).

Launched directly, no key reaches the program until it has hooked `INT 09h`, which is why
this was never seen. SETUP's "save and launch" is the same Enter key.

## The fixes (`src/host/main.c`)

- **The PM default handler for IRQ vectors 09h–0Fh reflects to the BIOS** when the real-mode
  IVT entry is still ours — for 09h, exactly what the V86 `BOP 09` arm does: consume the
  byte and EOI. A guest-hooked real-mode vector still stops loudly; a true nested-V86
  reflection does not exist yet and is not faked. Counted: `STAGE2: PM default IRQ handler
  reflected to the BIOS: n`.
- **`g_pm_client_exited`** — set by the PM `AH=4Ch` arm, checked where the main loop
  enters PM, and refused by every injector. An exit inside an ISR is now logged as
  `the client EXITED inside its vec 0xNN handler` and the dead client is never resumed.
- The stale `ver`/`MISMATCH` canary (linear `0x1600`) is gone; the exit line now prints
  the exit code and whether it happened inside an injected handler.

## Verification (rig, host `2f803674`)

| Run | Result |
|---|---|
| `chain.bat run` — shell, type `doom` | ✅ 0 abandoned ISRs (s79: 12), 1 reflection, IRQ9 `done=1`, Doom plays to the 60 s deadline |
| `chain.bat quit` — then F10, `y` | ✅ clean `AH=4Ch code=0x00`, no fault · ⛔ VDM ends; `ver` never runs |
| Doom direct (`bmqueue doom`) | ✅ 656 timer ISRs all `done=1`, 0 abandoned, 0 reflections |
| Skyroads | ✅ `n8=0 max_ms=7` |
| Win16 Notepad (`w16close.bat`) | ✅ opens, WM_CLOSE → DestroyWindow → WM_QUIT, host exits |

⚠ `scripts/bm/chain.bat` is new and is **not** in `bmstage.sh`'s staged list — copy it by
hand (CRLF). ⚠ `debug\tests\cmdcom\COMMAND.COM` is **XP's**; 6.22's is
`debug\tests\dos\COMMAND.COM` — the first run of `chain.bat` used the wrong one.
⚠ `controld exec` needs `cmd /c ""<quoted path>" args"` — a single pair of quotes around
path-plus-arguments silently runs nothing.

---

## Part 2 — returning to the EXEC parent (commit after `8fa3065`)

**`doom` → quit → `ver` works, and `doom` → quit → `doom` again works.** Rig, headless,
host **`b6a8a95b`**; ⚠ not yet confirmed by hand.

```
INT21h AH=4Ch -> client EXIT after 001faddf svc, code=0x00
DPMI: client teardown -- released 0xa memory blocks, 0x5 DOS blocks, 0x3b LDT slots
DPMI: client exited with a parent waiting (depth=01) -- back to real mode, terminating the child there
  EXEC: freed 0x6 more block(s) the child still owned
  EXEC: child exited rc=0x00, parent resumed (depth=00)
...
C:\...\demo\msdos\doom>ver
MS-DOS Version 6.22
```

**The design.** A PM `AH=4Ch` with `g_exec_depth > 0` now runs `dpmi_client_teardown()`,
clears PE, and hands the child to the same `dos_terminate()` every real-mode child uses —
which restores the parent's saved V86 frame. The exec loop then just `continue`s. A
top-level client's exit still ends the run, unchanged.

**What teardown releases:** every live 0501 block (a new `g_dpmi_owned[]` list), every live
0100 DOS block (`g_dpmi_dosblk[]`), every LDT slot above the switch-time watermark except
the host's own (default table, fault trampoline, PM-return catcher), vector hooks,
exception handlers, callbacks, and the interrupt/mode flags. **What it keeps:** the host's
selectors, reused by the next client.

**Four defects found on the way, each by the run that hit it:**

1. **An orphaned BOP in the parent.** The PM INT-site patcher had rewritten COMMAND.COM's own
   `int 21h` (AH=4Dh at `0100:0a6e`) to `C4 C4`. Teardown first only *forgot* the patch map,
   so the shell died on its first call after the child. ⇒ teardown restores every site
   first, guarded by `VirtualQuery`.
2. **The patch map outlived 0502.** A block the client released stayed in `pmap`, and the
   next walk read freed memory (host AV at `0x04581e53`). A latent bug before this session:
   any 0301 after a 0502 could do it. ⇒ 0502 drops the map entries inside the block, and
   removes it from `g_dpmi_blk[]` (the code-block scanner walked freed blocks too).
3. **A child's real-mode allocations were never freed.** `dos_alloc()` stamped every block
   `DOS_PSP_SEG`, so "free what the dead PSP owns" was impossible. DOS/4GW's five
   `AH=48h` blocks outlived Doom. ⇒ `AH=48h` stamps the current PSP (unchanged for a
   top-level program, whose PSP *is* `DOS_PSP_SEG`); child exit frees every block it
   owns. **This applies to every EXEC'd program, not just DPMI ones.**
4. **The host's own table split the arena.** For a DOS program the 256-vector default PM
   table comes from `dos_alloc` (the host pool is WOW's), so it sat at `0x165d` and the
   second `doom` loaded at `0x169f` instead of `0x242`. ⇒ teardown frees it and keeps the
   selector; the next client rebases the same slot. Second run: child at `0x242`, table at
   `0x177:0` linear `0x165e0` — identical to the first.

A new line, **`EXEC: chain after exit:`**, dumps the MCB chain at every child exit. It is
what named defect 4, and a leak is invisible in any other single line.

| Run (host `b6a8a95b`) | Result |
|---|---|
| `chain.bat quit` | ✅ `ver` prints `MS-DOS Version 6.22` after Doom exits |
| `chain.bat twice` | ✅ second Doom at `0x242`, 1,457 timer ISRs after the 2nd switch, all 2,403 `done=1` |
| Doom direct | ✅ 649 ISRs `done=1`, 0 abandoned, no fault |
| Skyroads | ✅ `n8=0 max_ms=7` |
| Win16 Notepad | ✅ opens, closes, host exits |
| Off-VM battery | ✅ 1591 checks, 0 failed |

⚠ **Still unmeasured:** SETUP → "save and launch" (same path, by hand); Heretic, Hexen and
Duke3D setups (the user's other reports); a real-mode TSR loaded before a DPMI client.

---

## Part 3 — north star 1 (after the user confirmed #3 by hand)

✅ **User, 2026-09-25:** *"Opening executables from shell, works. Setup > Game works for Doom,
Hexen, Duke3D."* `debug\prev\ntvdmhost_prev.exe` promoted to `b6a8a95b`.

1. **Measured** (`7178849`) — `STAGE2: MODEYTL`, written up in
   [`research/modey-cost-measurement.md`](../../research/modey-cost-measurement.md). The
   fan-out approximation was not the cheap option: Doom low ~93% host CPU and half the frame
   rate. Trap-per-store infeasible; interpret-while-multi-plane projected ~5–20%.
2. **User chose design C, Wolf3D/Mario first.** Built (`ed0a578`). Wolf3D's status bar now
   draws; Mario clean; both 70 fps. Three defects found on the way — 16-bit register
   write-back in `host_interp` (Wolf3D's `cdq/idiv` → INT 0 → `0000:0078`), VIF not kept in
   step with an interpreted `cli`, and declined `cdq`/`imul imm` under multi-plane masks.
   Plus a pre-existing stack overflow in the forced-exit report (512-byte buffer, 48 hot ports).
3. ⚠ **Cost:** Wolf3D is interpreted almost continuously (~70–82% host CPU, was ~38%).
   Lemmings (mode 12h) −1.5% interpreted throughput vs the confirmed build after confining
   every new check to mode Y — measured by interleaved A/B, not assumed.

▶ **Next:** user's hand test of Wolf3D/Mario on `0473d95d`; then Doom (32-bit interpreter) and
the chain-4 de-interleave. ⚠ Interpreter speed (~155 cycles/instruction) is now the lever.

---

## Part 4 — Doom's low detail (after the user confirmed Wolf3D and Mario)

✅ **User:** *"Mario and Wolf3D working!"* — prev promoted to `0473d95d`.

Doom fixed on the rig (`3a36b85`, host `8d795b96`, ⚠ not yet hand-confirmed): 35 fps at low
detail (was ~16), status bar `doomdetail` 0.294 (was 0.75). Method, in order:
1. A **site recorder** named where Doom writes the map mask — two drawers, self-contained.
2. A **new flat 32-bit interpreter** (`src/host/pm32interp.h`, 39 off-VM checks), run from the
   trapped `OUT` until the drawer returns.
3. A **detector** (`cfg\modeypm_detect.flag`) to test the assumption that nothing else stores
   under a multi-plane mask. It found 1,233/s — Doom's mask helper returns before its caller's
   latch copies. Fixed by stopping only once the run has touched the aperture; detector then 0.

★ The detector is the lesson: "only the drawers touch A0000" was an assumption, and it was
wrong in a way the picture alone would have hidden (the status bar is mostly redrawn by copy).

▶ Next: user's hand test of Doom low detail; then the chain-4 de-interleave (`p_vgamem`).
