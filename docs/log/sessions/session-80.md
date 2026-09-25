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

## Next: returning to the EXEC parent

The PM `AH=4Ch` arm returns 0 and the main loop treats that as the end of the VDM.
Returning to `COMMAND.COM` needs a real **DPMI client teardown**: the switch-in at
`dpmi_switch_to_pm` is one-shot and builds global state that nothing takes down (LDT and
`g_ldt_next`, the INT-site patches, `g_pm_int[]`, callbacks, memory blocks, `g_pmret_sel`,
`g_dpmi_client32`, `g_pm_app_hooked_timer`, …). After teardown, drop to V86 and run the
real-mode DOS `AH=4Ch` for the current PSP so the parent resumes — and a **second** DPMI
client (run Doom twice) must then work. `chain.bat quit` is the acceptance test: `ver` must
print after Doom exits.
