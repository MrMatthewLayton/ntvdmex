# Inventory — the BOP surface (`C4 C4 nn`)

**Surface:** the instruction a 16-bit guest uses to call *out* of the VDM into the
32-bit host. `C4 C4` is an invalid `LES` encoding on a real 8086; NTVDM claims the
whole two-byte prefix and reads the third byte as a call number.

**Spec:** ⛔ **none public.** This is one of the genuinely unspecified surfaces named in
[README.md](README.md) — like the `NtVdmControl` contract and the WOW32 thunk tables.
**The oracle is the spec**, and the oracles here are (a) the Microsoft-shipped 16-bit
binaries that issue BOPs, read directly, and (b) stock ntvdm's behaviour when they do.

⚠ Every row below whose "meaning" column is blank is blank **on purpose**. Nothing in
this file may be filled in from memory; see the PIT mode-3 incident in
[parity-by-inventory](../../README.md) — a remembered fact and a one-oracle
measurement are both single opinions.

---

## 1. The problem this file exists to record

**The number space is NTVDM's, and we have been allocating out of it as if it were
ours.** Our codes are assigned in `src/host/main.c` (the map comment at ~line 499) and
were chosen freely. At least two of them collide with numbers a real NTVDM guest
actually issues:

| code | what a real NTVDM guest uses it for | what WE do with it | source |
|---|---|---|---|
| `0x50` | issued once by XP's `COMMAND.COM` | `DPMI_BOP` — real→protected mode switch | `main.c:441` |
| `0x54` | issued **15×** by XP's `COMMAND.COM`, with a sub-function byte | `DPMI_RMRET_BOP` — DPMI 0301 return catcher | `main.c:448` |

And a collision *inside* our own allocations, worth fixing on sight:

| code | use A | use B |
|---|---|---|
| `0x57` | `DPMI_FAULT_BOP` (`main.c:527`) | `WOWCALL_BOP_CODE` (`wow/wowcall.h:89`) |

### ✅ The fall-through is gated (2026-09-24)

An unmatched BOP is now **refused and logged**, not handed to `dos_int21()`:

```
STAGE2: UNIMPLEMENTED BOP -- refusing (NOT an INT 21h call): bop=0x54 next=0x01
        at 0x9342:0x03ce ax=0x0002 bx=0x0001 dx=0x95d7
STAGE2: task done -> reporting exit code 0x000000bd to CSRSS
```

**We stop; we do not skip.** Skipping needs the BOP's encoded *length*, which is
per-call — `0x54` carries a sub-function byte, so `EIP += 3` would leave that byte to be
executed as an instruction. And the exit code is a distinct non-zero `0xBD`: the defect
being closed was a guest killed by an unimplemented call and reported as a **clean exit
0**, so reusing 0 would have left the lie in place with better logging on top of it.

**The gate is measured, not reasoned.** The diagnostic shipped first (`92e2136`) and the
battery was run with it — necessary, because the BIOS arm deliberately sets
`handled = 0` and falls through, so the control flow alone does not settle who depends
on this:

| battery | guests | all confirmed loaded | fall-throughs |
|---|---|---|---|
| DOS | Doom · Hexen · Duke3D · Heretic · heaven7 · Skyroads · Wolf3D · Mario · Lemmings · Chasm · Gothica · Radiance · Fusion · Skyxmas · vesacube · MEM · 6.22 `COMMAND.COM` | 17/17 | **0** |
| Win16 | Notepad · Paint · WinMine | 3/3 | **0** |
| — | **XP's `COMMAND.COM`** | 1/1 | **1** |

⛔ **Lemmings' first row was a lie and nearly went into that table.** The harness said
`NO SUCH TARGET` (wrong entry name) and the count of fall-throughs was, truthfully,
zero — from a guest that never started. Every row above is gated on
`STAGE2: target.txt loaded`, not on the absence of a hit. ⚠ And the *first* version of
that check looked for `STAGE2: loaded`, which is not what the host prints — it scored
**every** guest as "never ran", including the one with a 4.9 MB log. An absence proves
nothing, and a presence-check that never matches proves nothing either.

### ⛔ What the fall-through did before the gate

The real-mode exec loop matches each BOP code with an exact `==`, and **its last arm
hands anything unmatched to `dos_int21()`** (`main.c` ~26932). INT 21h's own stub is
`C4 C4 20`, so that fall-through was never required — it simply answers for every BOP
it was never given, using the guest's `AH` as if it were a DOS function number.

XP's `COMMAND.COM` issues `BOP 0x54 / sub 0x01` with `AX=0x0002`. `AH=0` is DOS
"terminate". The guest was killed by its own unimplemented call and we reported a
**clean exit, code 0**. See [xp-command-com.md](../research/xp-command-com.md).

Diagnostic in place since `92e2136`:

```
STAGE2: BOP FALL-THROUGH -> INT21: bop=0x54 next=0x01 at 0x9342:0x03ce ax=0x0002
```

---

## 2. Our allocations

Read off the code, `file:line` each. Status is about *our* implementation.

| code | name | purpose | status | where |
|---|---|---|---|---|
| `0x08` | — | INT 08h timer tick stub | IMPL | `main.c:21965` |
| `0x09` | — | INT 09h BIOS keyboard | IMPL | `main.c:21972` |
| `0x10` | — | INT 10h video | IMPL | `main.c:21959` |
| `0x16` | — | INT 16h keyboard | IMPL | `main.c:21960` |
| `0x1A` | — | INT 1Ah BIOS time | IMPL | `main.c:21973` |
| `0x20` | — | **INT 21h DOS** | IMPL | `main.c:21958` |
| `0x2F` | — | INT 2Fh multiplex | IMPL | `main.c:21974` |
| `0x33` | — | INT 33h mouse | IMPL | `main.c:21961` |
| `0x35` | `MS_CB_BOP` | mouse callback return | IMPL | `main.c:6384` |
| `0x43` | — | XMS far-call entry | IMPL | `main.c:21977` |
| `0x50` | `DPMI_BOP` | DPMI real→PM switch | IMPL | `main.c:441` ⚠ **collides** |
| `0x54` | `DPMI_RMRET_BOP` | DPMI 0301 return catcher | IMPL | `main.c:448` ⚠ **collides** |
| `0x55` | `DPMI_CB_BOP` | DPMI 0303 callback entry | IMPL | `main.c:454` |
| `0x56` | `DPMI_PMRET_BOP` | DPMI PM-return catcher | IMPL | `main.c:457` |
| `0x57` | `DPMI_FAULT_BOP` | PM fault trampoline | IMPL | `main.c:527` ⚠ **shared** |
| `0x57` | `WOWCALL_BOP_CODE` | WOW 16-bit call return | IMPL | `wowcall.h:89` ⚠ **shared** |
| `0x58` | `DPMI_RAW2PM_BOP` | DPMI 0306 real→PM | IMPL | `main.c:470` |
| `0x59` | `DPMI_RAW2RM_BOP` | DPMI 0306 PM→real | IMPL | `main.c:472` |
| `0x5A` | `DPMI_FLTRET_BOP` | fault-handler return | IMPL | `main.c:542` |
| `0x67` | — | INT 67h EMM | IMPL | `main.c:21978` |
| `0x11`–`0x17`, `0x25`–`0x30` | — | BIOS services (equipment, memory, disk, …) | IMPL/PART | `main.c:21988`, arm at `~24575` |
| *else* | — | refused + logged, exit `0xBD` | ✅ gated | `main.c` ~26950 |

---

## 3. What XP's `COMMAND.COM` actually issues

**Measured, not remembered:** every `C4 C4` in `C:\WINDOWS\SYSTEM32\COMMAND.COM`
(50,620 bytes, md5 `be67d29ca914de072d9971e3fffc4050`), located by byte scan and
disassembled with `ndisasm -b 16`. **16 sites: 15 × `0x54`, 1 × `0x50`.**

| file off | BOP | sub | registers set immediately before | what the code does with the result |
|---|---|---|---|---|
| `0x02AB` | 54 | `0F` | `BX=0` (after `AH=48h` alloc, `ES`=block) | `AX=BX` → compared against 0 |
| `0x02D0` | 54 | `0F` | `BX`=block size (after 49h/48h realloc) | `cmp bx,ax` — **BX is an output** |
| `0x1581` | 50 | — | `ES`=PSP, after `AH=30h` version check | followed immediately by `INT 20h` |
| `0x1A84` | 54 | `02` | `DX=0x0258` | `mov [0x2d2],al` — **AL is an output** |
| `0x1B74` | 54 | `0D` | `DS:DX` → a buffer just built by `stosw` | then `AX=0x3D00`, `INT 21h` (open) |
| `0x1DF3` | 54 | `08` | 6-word `0xFFFF` frame, `BP=SP`, `ES`=CDS, `AH=0` | `lahf` / `add sp,0xC` — **CF is an output** |
| `0x283E` | 54 | `01` | `DX=0x95D7` (a struct filled from `[es:0x32B/0x32D]`) | `jnc`, then `cmp ax,0x8000` |
| `0x2A56` | 54 | `0E` | `SI=0x04B9`, `CX=0x0539` | `or dx,dx` — **DX is an output** |
| `0x2B24` | 54 | `09` | — (after a size calc stored at `[0x961F]`) | falls straight through |
| `0x2DEB` | 54 | `0A` | `0xFFFF` frame, `BP=SP`, `ES=[0x458]`, `AH=19h` first | `BP=AX`, `lahf`, `add sp,0xC` |
| `0x2E25` | 54 | `0B` | `CX=[0x32B]`, `BX=[0x32D]`, `AH=19h` first | `jnc` — **CF is an output** |
| `0x2EE5` | 54 | `08` | 6-word `0xFFFF` frame, `BP=SP`, `AH=1` | `lahf` / `add sp,0xC` |
| `0x2F1B` | 54 | `0B` | `CX=[es:0x32B]`, `BX=[es:0x32D]` | — |
| `0x303D` | 54 | `06` | `BX=[0x32B]`, `AX=[0x32D]` | `jc` — **CF is an output** |
| `0x307D` | 54 | `10` | — | `or al,al` — **AL is an output** |
| `0x4E24` | 54 | `00` | — | — |

### What can already be said from this

1. **`BOP 0x54` takes a one-byte sub-function immediately after the code.** Eleven
   distinct sub-functions: `00 01 02 06 08 09 0A 0B 0D 0E 0F 10`.
2. **Some sub-functions are stack-based.** `08` and `0A` push six `0xFFFF` words, set
   `BP=SP`, call, then `lahf` / `add sp,0xC` / `sahf` — a frame the host writes into,
   and a carry flag the host sets.
3. **`AH=19h` (get current drive) is issued immediately before several of them**, which
   looks like a deliberate state sync rather than a coincidence — it appears at `0x1DF3`,
   `0x2DEB`, `0x2E25`, `0x2F1B`.
4. **The pair `[0x32B]` / `[0x32D]` is passed repeatedly** in `CX`/`BX` or `BX`/`AX`.
5. ★ **`BOP 0x54 / sub 01` is the one that kills us**, and the live trace corroborates
   the static read exactly: `DX=0x95D7` in the log, `mov dx,0x95d7` at file `0x283E`.

⚠ **NOT established: what any of these sub-functions MEAN.** `sub 01` sits on the path
that a shell would use to obtain its next command line, and `0x8000` is a suggestive
thing to compare a return against — **that is a hypothesis with no measurement behind
it.** Do not write it into this table until stock ntvdm has been asked.

### And one thing that IS established, from the same disassembly

At file `0x1563`:

```
mov ah,0x50 ; mov bx,es ; int 21h     ; set PSP
mov ah,0x30 ; int 21h                 ; get DOS version
cmp ax,0x0005                         ; <-- AX, not AL
jz  ok
mov dx,0x21d7 ; call print            ; "Incorrect DOS version"
```

The check is `cmp ax,5` on the **whole word** — `AL`=major, `AH`=minor — so it demands
**exactly 5.00**, not "5 or later". `cfg\dosver.txt` = `5.0` satisfies it; anything else
does not. That matches the measured 0-calls-vs-9-calls result in
[xp-command-com.md](../research/xp-command-com.md) and pins *why*.

---

## 3a. Who else issues one — a measured negative

Byte-scanned every `.exe`/`.com` under `demo/msdos/` and every `.exe`/`.dll`/`.drv`
under `demo/win16/` on the rig. `C4 C4` pairs turn up in nineteen DOS binaries and
three Win16 ones, and **every one of them is noise**: runs of `C4` inside compressed or
resource data (`ZAR/SOUND/SETSOUND.EXE` alone has 362, 344 of them `C4 C4 C4`), with no
third byte matching a plausible call number and none of them at a code site.

⚠ A byte pattern is not an executed instruction, so this is weak evidence on its own —
but it agrees with the sweep (below) and with the shape of the mechanism: **BOPs are how
*NT's own* 16-bit components talk to the 32-bit side.** Ordinary period software has no
reason to know the instruction exists. That bounds the collision risk to
Microsoft-shipped guests — which is a small set, and `COMMAND.COM` is the one that
matters.

---

## 4. Owed

1. ~~Gate the fall-through.~~ ✅ done, measured — see §1.
2. **Resolve the `0x57` double-booking** between `DPMI_FAULT_BOP` and
   `WOWCALL_BOP_CODE`. They are probably never live in the same guest, but "probably" is
   how the `0x54` collision survived too.
3. **Re-number ours out of the range real NTVDM guests use**, or dispatch on more than
   the code byte. A collision is only invisible until an NTVDM-aware guest turns up.
4. **Measure `BOP 0x54`'s sub-functions against stock ntvdm** — `tools/wintest/stock.sh`
   is the pattern for dropping the IFEO key (⛔ read its warnings first). The refusal log
   line now prints `AX`, `BX`, `DX` and the sub-function byte, so a side-by-side against
   stock has something to compare.
5. **6.22's own `COMMAND.COM` loads, runs, and makes ZERO INT 21h calls** (this battery,
   `result_dos.log`). It issues no BOPs either, so it is a different question entirely
   and has not been looked at.
