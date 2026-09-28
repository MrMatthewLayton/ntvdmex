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

⚠ **Found on the way: counter 2's wait was broken, and I first blamed IF.** On NTVDMEX the probe
read 0 after its first case. I read `FL=3293` as IF=0 and posted "sti does not stick" on #212 —
wrong (`0x3293 & 0x200` = `0x200`), corrected there, and `p_vif.asm` confirmed IF stays 1. The
real cause, found by replaying the probe's port sequence against the model off-VM: a gate-low
edge froze counter 2's elapsed count past terminal count, and a new count did not clear it, so
every "time it with counter 2" wait after the first returned at once. Fixed (`pit_test` T_WAIT);
now `p_pit0` AGREEs on every row on the rig, and `p_pit` is unchanged.
⛔ **Lesson: decode a flags word bit by bit before quoting it, and when a probe's result looks
impossible, check the probe's own instrument (here its timer) on the subject before the subject.**

## Round 6 (user, by hand) and the stock comparisons
Confirmed and closed: #213 (ZAR sound through a real game), #175 (timing and sound in every game,
PC speaker), #161 (PBrush vs stock side by side: functionally ~identical, ours has the Luna
theme). `scripts/bm/handstock.bat` starts programs under stock for a person, removing the IFEO
value only while they start.
⚠ **Stock Mario cannot be compared on this rig:** stock runs it in real full-screen, the display
goes to standby, and XP stays alive. The first "crash" was that plus a manual restart. Recovery
without a reboot: end stock's `ntvdm.exe`, then `rigshot dispreset`
(`ChangeDisplaySettings(CDS_RESET)` with the registry mode) brings the display back.

## Next
#213 by ear. Then #212 (IF at program start, as originally reported), #162 synchronous
delivery, #183 cheaper waits.
