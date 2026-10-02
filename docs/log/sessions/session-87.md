# Session 87 (2026-10-02) — the fifteen

The user asked for 15 issues done unattended. Six worker agents in worktrees, grouped by area, sharing the rig through `scripts/riglock.sh` (one holder for the rig AND the oracles; deploy + run in one locked call).

## What landed (merged, gated, pushed as `130d802`; rig `bin\` = `56d212e7`)
#183 interpreter speed (44.15 → 36.65 ns/instr on the rig) · #194 0x66 forms, IRETD, PM IRET · #248 DPMI 0503h/0304h/16 callbacks/0400h CL/8022h · #250 VDM clock offsets · #255 EXEC min/maxalloc + NE/PE → Windows · #34 INT 24h path · #251 AUX/PRN via BIOS · #256 INT 17h DX, PM 86h, PM stdout redirection, port 42h · #254 enhanced keyboard BIOS · #249 INT 33h 25h–34h · #252 INT 10h text/pixels in every mode, pages · #53 VBE 4F0Ah PM block · #179 IDE adapter, empty · #246 8237A remainder · #163 five Win16 tests.
Remainders filed: #261–#276. Watcher bug #277.

## Verification of the merged build
- Off-VM battery 2713/0; interpfuzz digest of the merged interpreter == #194's branch (`792e8c7e5b65a835`) — the #183/#194 conflict changed no behaviour.
- Full parity sweep: 97.3% (1233/1267), 15 probes with disagreements — the same counts on `978589de` for 14; `p_kbd` and `p_vbepm` checked row by row: oracle DISPUTES, no regression. `p_int53f` unusable (known).
- Gate A/B ×2 vs `978589de`: Skyroads, Doom/ZAR audio, Win16 shelf unchanged; Lemmings no bails either build.

## Lessons
- **Sessions ended three times; background agents and run_in_background shells die with them.** Worktrees + commits survive; resume agents with SendMessage. A job started with `nohup … & disown` SURVIVED (the final verification). Tell workers to commit after every step.
- **The rig watcher writes `cfg\target.txt` per run and never removes it** (#277) — every interrupted run left NTVDMEX launching a probe. Restore `bin\` and clear `target.txt` after any interruption.
- Merging `oracle-rules.json` by regex silently dropped a rule once; resolve as an exact set union of both sides (`:2:`/`:3:`) and check `missing=0 extra=0`.

## Owed by hand (in `checks.txt`, one at a time)
Duke3D 800×600 via 4F0Ah (#53) → keyboard feel in EDIT (#254) → `dir a:` Abort/Retry/Fail (#34) → NOTEPAD.EXE at the prompt (#255) → `date` (#250) → QB `SCREEN 13: PRINT` (#252) → PRN to PRINTOUT.TXT (#251) → Lemmings/Wolf3D/Mario feel (#183/#194). Win16 stock values owed: #271.
