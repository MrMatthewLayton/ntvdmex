# Session 83 — the round-5 sweep, and ZAR's silence (#213)

2026-09-28. The user went through the round-5 checklist (`runs/s83/sweep.txt`) and the four
overnight decisions (recorded on #168, #183, #162, #175).

## The sweep, filed
- **Closed (user-confirmed):** #203, #184, #154, #153, #208, #160, #189, #209, #211.
- **New:** #213 ZAR silent (P0, fixed below) · #214 Doom Close Program leaves MIDI notes hanging ·
  #215 Calc's Ctrl+C/V accelerators · #216 dead Win16 menu commands (PBrush Zoom) · the user's
  three design asks: #217 overlay flicker + settings, #218 smart mouse (replaces Show Host
  Cursor), #219 pause the unfocused instance, #220 one shared Win16 tray icon.
- **Commented:** #155 rescoped (screenshots + combined video/audio only; the standalone WAV item
  goes), #162 Charmap has no font list or grid, #183 Lemmings' game screen flickers now and then,
  #171 Make EXE fails, #161 resize "ish", #141 still owed.

## #213 — ZAR silent: `da49025`, bin `74610909`
Recorded ZAR's attract demo on the rig with `cfg\wavrec.flag` and bisected
(`runs/s83/bisect/zarrec.sh <exe> <tag>`, 70 s per build):
`0e5b10e7` 63–66 s sounding · `ed1493b` 66 · **`323b35c` (#173) 0** · `001de0d2` 0 · fix 66.

**Cause:** the synchronous PM device-IRQ injector ran the client's handler inside
`dpmi_inject_pm_irq()` and called `vdd_pic_acknowledge()` afterwards, so the handler's EOI came
first and IRQ5 stayed in service. Before #173 the next timer tick's non-specific EOI cleared the
stuck bit by accident (IRQ0 was never in service to take it). #173 held IRQ0 correctly and
exposed it. The fix acknowledges before the handler runs and hands the line back on failure.
Gates: battery 1775 green; Skyroads n8=0 max_ms=7; Doom demo audio and IRQ5 counts unchanged;
Win16 Notepad closes on its X.

⚠ **The rollback build `0e5b10e7` is s81 Part 5**, not the end of s81: every s81 change after
Part 5 (#152, #208, #173, #153, #211, …) was as unconfirmed as s82's.

## #175 — IRQ0 in one-shot modes: `2234471`, bin `22822729`
Probe first (`tools/dostest/p_pit0.asm`: counter 0 per mode, 220 ms timed by counter 2): all
three oracles give exactly 1 IRQ0 for modes 0, 4 and a bare mode-0 re-write after TC; modes 1/5
follow the datasheet (0, gate tied high), with rationales in `oracle-rules.json`. The model now
raises once per count in 0/4 and never in 1/5; `pit_test` T_IRQ0; STAGE2 `oneshot_loads`.
A/B against `74610909` (`runs/s83/ab175/ab.sh`): Skyroads, Doom, ZAR and the 16-program shelf are
unchanged.

⚠ **Found on the way (#212):** on NTVDMEX the probe reads 0 after its first case because an
explicit `sti` does not stick (captured FL=3293 straight after `sti`; 17 IRQ0s in the whole run,
every async attempt `why=0x14`). That is a deterministic reproducer for #212 and probably #172.

## Next
#213 by ear. Then #212 (`sti` does not stick; `p_pit0.com` reproduces it), which likely
underlies #172; then #162 synchronous delivery and #183 cheaper waits.
