---
name: ntvdmex-style
description: Use when writing or changing C code in NTVDMEX (src/, tests/, tools/). Covers the house-style judgement calls that the style tools cannot make -- names, wrapped expressions, the steps of a function, comments and clean-room provenance -- and when to run scripts/style.sh.
---

# NTVDMEX house style: the judgement calls

`docs/STYLE.md` is the style. Most of it is applied by a tool. Do not hand-format what the tool
does; run it:

```sh
scripts/style.sh --fix FILE...     # header, comments, braces, statements, blank lines, file order
scripts/style.sh --check           # the whole tree, as CI runs it
scripts/style.sh --names FILE...   # names and types report
./scripts/offvm.sh                 # the off-machine battery: must stay green
```

The tool proves it changed no code, and refuses a file it cannot prove. If it refuses one, fix the
file by hand; never work around the proof.

What follows is what the tools cannot judge. Read the matching section of `docs/STYLE.md` before
a change that touches it.

## Names (STYLE.md 2)

- Name a thing for what it holds or does: `voiceIndex`, `bytesPerSector`, never `i`, `n`, `tmp`,
  `st`. No private abbreviations; the domain's own (`Lba`, `Crtc`, `Psp`) stay.
- Functions: PascalCase with the module's prefix (`DosMcb...`, `VddPit...`, `WowUser...`), file
  statics included. Globals `g_` + PascalCase. Members PascalCase. Booleans read as predicates
  (`isBlank`, `hasGlyph`).
- Windows types only (`BYTE`/`WORD`/`DWORD` for hardware widths, `INT`/`UINT` for counts, `BOOL`
  with `TRUE`/`FALSE`, `P...` pointers, never `LP...`).
- Names fixed outside the project never change (STYLE.md 7): exports, registry values, `cfg\`
  knob names, log formats the tests parse, Win16 export names and thunk ids.

## No magic values (STYLE.md 3)

A number or string that means something gets a named constant with the module's prefix, defined
where its users can see it. Tests are the exception: their expected values stay literal.

## Wrapped expressions and the steps of a function (STYLE.md 5)

- A long expression breaks **before** an operator, never aligned under a parenthesis. Each
  top-level term is whole on its own line, one level in; a term that itself wraps goes one level
  deeper, so the indentation shows what binds to what. A wrapped `&&`/`||` chain takes one
  condition a line; a wrapped ternary takes three lines.
- Separate the steps of a function with a blank line where it reads as a sequence.

## Comments (STYLE.md 6)

- Say what the code cannot: why, the constraint, what was measured. Not what the next line does.
- Markers: `[INFO]:`, `[CAUTION]:`, `[WARNING]:`; a section title is `TITLE (Importance = n):`.
  ASCII only.
- **Provenance is a claim.** Write what was observed and how (the probe, the reference host, the
  run). Never invent a measurement, and never describe how another program's code does something.

## Clean room (CLEAN-ROOM.md) -- not negotiable

No Microsoft code, no third-party code, no disassembly, no addresses inside someone else's binary,
nothing from Microsoft's source code or confidential design, however it was seen (leaked, at work,
under licence). A person who has seen the source or internal design of NTVDM, WOW, Windows 3.x or
MS-DOS does not work on the parts of NTVDMEX that do the same job (CLEAN-ROOM.md). Interface values the host must recognise at run time (thunk ids,
structure layouts the OS shares) are allowed; say where each is seen at run time.

## Before you finish

1. `scripts/style.sh --fix` on the files you changed, then `scripts/style.sh --check`.
2. `./scripts/offvm.sh` green; add or extend a test in `tests/unit/` for logic that can run off
   the machine.
3. For a change that should not alter behaviour: `FNCMP_ALL=1 ./tools/fncmp/fncmp.sh main .`
   must report IDENTICAL.
4. Nothing proves a change works on Windows XP except running it there. Say so rather than claim
   it.
