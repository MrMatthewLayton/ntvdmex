# Session 30 — 2026-08-26

Two halves. First: **the repository itself** — made public, history cleaned, tracker
reconciled with reality, docs consolidated, wiki published. Then: **the WOW/Win16
epic (#128)**, which is now on the critical path because #129 turned out to be
impossible.

---

## Part 1 — the repo can now be picked up cold

**Public, with a wiki.** GitHub *silently* refuses wikis on private Free-plan repos —
the API accepts the PATCH and `has_wiki` just stays `false`. Going public unlocked it.

⚠️ **Before publishing, history was audited — and it mattered.**
`tools/doomoracle/DOOM1.WAD` (4.2 MB) was still reachable in history. A past commit had
removed it saying *"it is id's, not ours"*, and publishing would have undone that,
because removing a file does not remove it from history. Purged with `git filter-repo`
along with three of Japheth's HX test binaries. `.git` went **55 MB → 4.4 MB**. Backup
bundle at `../ntvdmex-prepurge-backup.bundle`.
**When making any repo public, audit HISTORY, not just the tree.**

A full copyright audit followed: no Microsoft/IBM/id binaries were ever committed;
Nuked-OPL is fetched to a gitignored dir and never linked; the ReactOS-derived
`VDM_COMMAND_INFO` layout is an OS ABI. One residual: four annotated lines of ntoskrnl
disassembly in `docs/research/dpmi-under-ntvdmcontrol.md`, flagged but not changed.

**Tracker reconciled: 58 → 140 issues (43 open, 97 closed, 9 closed epics).**
`tools/gh/backfill.py` is the manifest — re-runnable, matches on title. Every closed
issue names its evidence. Four that *look* done are deliberately left open with the
missing half stated (#28, #34, #49, #56).

**Docs consolidated.** `return-ntvdm.md` (4,000 lines) split verbatim into this archive
— verified line by line, zero non-blank lines lost. `docs/STATE.md` rebuilt; it had said
*"M4 in progress"* for three weeks while Doom was fully playable. Wiki authored in
`docs/wiki/` and published by `tools/wiki/publish.sh` (repo is source of truth).

**The fresh-clone test found a defect in the docs written the same day.** Cloning the
public repo and following `STATE.md` verbatim: build fine, but the documented test loop
(`for t in *_test`) matches **nothing** — those binaries are gitignored. Under `bash`
that silently "passes" a loop that ran zero tests. `./tools/dostest/run.sh` is the entry
point; it compiles *then* runs. Also added a rig-provisioning page, since standing up a
new XP box from zero previously lived only in the scripts.

---

## Part 2 — #129 Win16 passthrough: IMPOSSIBLE, and that changes the roadmap

**Detection works.** Measured, not assumed — the host never logged its own command line,
so that instrument came first:

```
DOS : ntvdmhost.exe "…\ntvdm.exe" -f -i20
WOW : ntvdmhost.exe "…\ntvdm.exe" -f -i1 -w -a …\krnl386.exe
```

`-w` is the discriminator (some sources say `-m`; **not on XP SP3**). Second independent
tell: `GetNextVDMCommand` returns FALSE `err=0x57` on a WOW launch. Matched as a whole
token, not a substring — a DOS path containing `-w` handed to stock would be a worse
failure than the one the guard prevents.

**Handing the launch back cannot be done.** Three routes, all closed by measurement:

| Route | Result |
|---|---|
| Spawn `System32\ntvdm.exe` | IFEO is keyed on image **name**, evaluated inside `CreateProcess` → re-enters us → fork bomb per launch |
| Spawn a **renamed copy** | Child exits `rc=0xFF` instantly. *Not* a `STARTUPINFO` problem — passing our real one gave the identical result |
| Point `wowcmdline` at the renamed copy | **"sysedit.exe is not a valid Win32 application"** — Windows validates the VDM image's identity ⇒ this is the root cause of the `rc=0xFF` above |
| Repoint `Control\WOW\cmdline` | Does not route to us before **or after a reboot**; breaks DOS launching entirely |

The `cmdline`/`wowcmdline` split is real and tantalising (`-a …\krnl386` there matches the
live launch exactly) but repointing it does not work — the original "disproven" finding
stands, with the reboot hypothesis now eliminated.

⇒ **`wow_refuse()` ships**: detect, then fail loudly with a dialog naming the cause and
the exact `reg delete`. DOS regression checked — MS-DOS 6.22 still boots.

⇒ **There is no safe "leave it installed" story until WOW exists.** #130 (installer) is
blocked on #128, which moved from far-future onto the critical path.

---

## Part 3 — #128 WOW: the NE loader works

`tools/ne/nedump.py` was written **before** the C loader and pointed at real binaries,
because what they contain decides what must be implemented.

**`src/wow/ne.h`** — header-only, no Windows/VDM dependency, exercised off-VM by an 18th
battery (`ne_test`, 35 checks: synthetic image always runs; real-binary assertions SKIP
when the Microsoft files are absent).

**On the rig, krnl386 loads and relocates inside NTVDMEX:**

```
WOWTRY: parsed OK. segs=4 mods=0 movable=0xa4 align=4 autodata=4
  CS:IP=1:c02b  SS:SP=0:0  expects Win 3.10
  relocs seg 1..4 ok, cumulative sites patched=0x1ef
```

★ **495 patched sites from 13 relocation records.** That number is the whole argument for
getting relocation *chains* right: the obvious reading (one record, one fixup) would have
patched 13 sites and silently missed 97%, and krnl386 would have died somewhere unrelated.

### The LDT blocker — and a retraction

Selectors were refused with `STATUS_INVALID_PARAMETER_1`. Varying index/access/limit/base
changed nothing.

**I first concluded "refused on WOW launches, accepted on DOS launches". That was WRONG** —
the experiment had two variables (the WOW probe ran after `v86_init`, the DOS probe ~500
lines later). The clean 2×2 showed `dos-early` fails too. Bisecting:

```
dos-early / B-emsframe / C-csrss-register / D-after-getcommand   REFUSED
E-after-get-tib                                                  SUCCESS  ← flips here
```

**`v86_get_tib()` is what makes the kernel accept LDT entries** — not `csrss_get_command()`,
which was the seductive answer *because* it's the call that fails on `-w`. It isn't: D
still fails after the fetch succeeds. And `v86_get_tib()` works fine on a WOW launch, so
one call moved earlier **unblocks it**. There was never a WOW-specific restriction — an
ordering requirement the DOS path satisfied by accident of where its code sits.

### krnl386 is a LIBRARY — the execution plan was wrong

About to jump to `seg1:0xc02b`, re-read the header: `prog_flags 0x8309` includes `0x8000`.

```
krnl386 / user / gdi   LIBRARY  SS:SP=0:0  stack=0
wowexec / sysedit      PROGRAM  stack=0x2000
```

A DLL has **no stack of its own** and its `CS:IP` is an *initialisation* entry with the
Win16 DLL register convention. Jumping there would have produced a meaningless death that
looked like a finding.

**The real bootstrap, per the binaries:** init krnl386 → init user + gdi → run
**wowexec.exe** (the PROGRAM, first thing with a stack) → wowexec launches the app. Which
is also why the launch says `-a …\krnl386.exe`: the first *library* to bootstrap.

---

## ⚠️ Traps that cost rounds today

- **`WinMain` has THREE `log_write` calls and every one TRUNCATES.** A probe placed after
  the first, then the second, had its output silently erased by the next — reading exactly
  like "the code never ran". Cost three rounds. Diagnostics go after the **last** one
  (~line 9941) or into `p`. `C:\ntvdmex\ldtprobe.log` is now an untruncated sink.
- **Grepping the binary for a string proves nothing.** `zput` is `static inline`, so GCC
  turns short literals into immediate stores and they never reach `.rdata`. I nearly
  concluded a function had been optimised away.
- **SMB attribute caching served a stale copied log** — again. Copy to a fresh name.
- **`sips` crops from the CENTRE**, not the top-left. Two screenshots of grass.

## Next actions

1. **DLL initialisation calling convention** for krnl386 (stack, `DS`=autodata, `DI`, `CX`).
2. **Import-by-ordinal** resolution so user/gdi can bind to krnl386 — the gate to apps.
3. Then, and only then, something worth executing.
4. Independent of WOW: **#131 console/stdio** (redirection/piping bypassed today).

**Rig left with:** IFEO `Debugger` **set** and `wowtry.flag` **present**. Clear both to
return the box to stock.
