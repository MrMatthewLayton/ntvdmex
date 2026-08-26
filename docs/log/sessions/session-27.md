# Session 27 — 2026-08-26

> Archived verbatim from `return-ntvdm.md`, which was the single rolling handoff
> file until 2026-08-26. Split by session so it can be read by topic and date
> rather than by scrolling. **Nothing has been edited** — including conclusions
> that later sessions REFUTED, which are kept on purpose: the refutations are
> some of the most valuable material here.

```
═══════════════════════════════════════════════════════════════════════════════
██ ★★★★★★ **DOOM IS FULLY PLAYABLE — MOUSE INCLUDED.** (2026-08-26, session 27)██
██     USER-CONFIRMED BY HAND ON THE RIG: *"mouse working in Doom."*          ██
═══════════════════════════════════════════════════════════════════════════════

The last open defect is closed. Graphics, PCM, MIDI, keyboard AND mouse all working on
real silicon, under a from-scratch DPMI host, with no guest patch of any kind.

`cd28879`  **DOOM WAS ASKING ALL ALONG. ES:(E)DI, AND THE (E) WAS MASKED AWAY.**
  Session 26 left this filed as two competing explanations -- unimplemented INT 33h
  functions, or a mis-patched `CD 33` calling us with arbitrary EAX. **BOTH ARE WRONG**,
  and the widened instrument said so in ONE headless run before a line of fix was written.

  ▶ WHAT THE INSTRUMENT SHOWED (the AX values as they are, plus the caller's site):
      - the calls come from Doom's own DPMI thunk table -- `c3 cd 30 c3 | c4 c4 | c3` --
        so the site is REAL CODE and **mis-patching is refuted**;
      - 2595 of them arrive through DPMI 0300 with AX=0xffff, which is **the value our
        own INT 33h reset wrote back into that structure on the first call**. Reading
        back exactly what you last wrote is the signature of an address that is nobody's
        but ours.
  ▶ THEN A PROBE THAT PRINTS BOTH CANDIDATES RATHER THAN PICKING ONE:
```
    RMCS 0300 int=0x33 es=0x18f edi=0x03dc9158 esb=0x0 cl32=1
         masked@0x00009158 eax=0x4e800000   <- junk in the guest's low memory
         full  @0x03dc9158 eax=0x00000003   <- I_ReadMouse, "read buttons"
         (next call)       full eax=0x0000000b   <- "read counters"
```
  ▶ CAUSE: the real-mode call structure is passed in **ES:(E)DI** and every arm masked
    the offset to 16 bits. We read the function number out of somebody else's memory --
    **and wrote our answers there too**, corrupting two words of the guest's low memory
    on each of 2915 calls. FIX = `dpmi_rmcs_ptr()`: follow the CALLER'S D/B bit, which
    is the rule the CPU already imposes (16-bit `mov di,x` leaves the top half stale).
    Applied to 0300, 0301/0302 and the 000B/000C descriptor buffers.
  ▶ SAFE FOR THE EXTENDER, **MEASURED NOT ASSUMED**: DOS/4GW's own traffic comes from
    16-bit code, where this reduces to the mask already there -- 0302 logs
    `es=0x1f edi=0x00004b54`, 000B/000C `edi=0x0000146c`, masked and full identical in
    every sample. Only a 32-bit caller changes, and for one the old code was never right.
  ▶ AFTER: `MOUSEI33 ax: 0003x000004fb 000bx000004fb` -- 1275 calls each to the two
    functions reported as never happening. Run healthy: STAGE2 complete, no PM stops,
    no exceptions, all video modes supported.

✅ **CONFIRMED BY HAND ON THE RIG (2026-08-26): the mouse works in Doom.** Headless proved
  the CALLS (1275 each of AX=3 and AX=0Bh per 45 s run, with the register block finally
  landing at the right address); the user's hand proved the FEEL. Neither could have done
  the other's job -- see [[headless-rig-cannot-see-input-lag]]. Build md5
  `4939ae65372e7a7eb1f69dc2e72a377a`; play with `scripts/bm/doomex.bat`, capture on
  **Win+F10**, `msens.txt` tunes sensitivity.
  ► RESIDUAL, ONE CALL IN 2551: the FIRST 0300 with BL=33h uses a DIFFERENT structure
    (edi=0x03dc9240, 0xe8 bytes from the other) and reads AX=0x53c1, which is not a
    mouse function and is not initialised by anything Doom does to `dpmiregs`. It was
    junk before the fix too (0xe0c1). One call, harmless, unexplained -- do not let it
    masquerade as a cause if something else is wrong.

★ METHOD, and it is the whole session: **THE BUCKET WAS THE BUG IN THE INSTRUMENT.**
  `i33oth=1079` could not distinguish the two hypotheses because it threw away the one
  field that separates them. Widening it cost 40 lines and one run; either hypothesis
  would have cost a session and one of them was the wrong fix. Same shape as
  [[counter-layout-is-a-claim]]. And the new dump was given **ITS OWN 5 s GATE** -- the
  KEYLAT block only runs once a KEY has been seen, which silently disables it for a
  headless run where nobody types, i.e. exactly this run. That paid for itself the same
  hour it was written.
```
