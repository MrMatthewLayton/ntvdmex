# Session 34 — 2026-08-28 — DPMI exceptions are delivered, and krnl386 tells us what is wrong

Continues [session 33](session-33.md). GH #128. Branch `m9/completeness`.

**Where it started:** session 33's handoff. krnl386 is alive at the end of the run and
*waiting* — it registers a DPMI exception-6 handler through `INT 31h 0203` and then
executes `0f ff` (UD0) **on purpose** to check the handler is reached. We service `0202`
and `0203`; what is missing is only the delivery. PM step `0x63`.

**Where it ended:** the exception is delivered, the handler runs, it comes back — and eleven
more walls behind it fell in the same session, every one of them ours. PM step `0x63` →
**`0xd9`**. krnl386 loads `SYSTEM.DRV`, the error that stopped it in the middle of this
session ("Missing 16-bit system module") is **cleared**, and raw `INT nn` in unscanned
protected-mode code — the oldest silent VDM killer on this project — is **retired as a
class**. It now stops asking the 32-bit side to load a Win16 module.

| | before | after |
|---|---|---|
| PM steps | `0x63` | **`0xd9`** |
| WOW32 serviced / declined / unimplemented | 5 / 4 / 1 | **11 / 15 / 17** |
| krnl386's own diagnosis | *(never read)* | printed verbatim into the log |
| the last wall | a `#UD` we never delivered | **`WowLoadModule`** — the 16→32 module-load boundary |

---

## 1. The kernel builds the DPMI exception frame ITSELF

The `VTIB_FLT_SAV*` slots carry three values and a DPMI handler needs six, so session 19
left a note saying the faulting CS:EIP could not be recovered and nothing should be
resumed until the slots were calibrated "against a fault at a KNOWN address".

krnl386's UD0 **is** that fault: a known instruction, at a known offset, with a known CS
and a known SS:SP, raised deliberately. So rather than guess a frame, dump one — and the
reflect leaves `liveESP` 0x30 below the trampoline stack top, i.e. the kernel pushed
something we had never looked at. Printed with its offsets:

```
FLTSTK sel=0x015f lin=0x2000 top=0x1000 espNOW=0x...0fd0
  +0x0fd0 = 0x00000000      SS:SP+0x00  return IP   } LEFT ZERO
  +0x0fd4 = 0xc5f00000      SS:SP+0x02  return CS   }
                            SS:SP+0x04  error code   0000
                            SS:SP+0x06  faulting IP  c5f0   ← the UD0
  +0x0fd8 = 0x324601cf      SS:SP+0x08  faulting CS  01cf
                            SS:SP+0x0a  FLAGS        3246
  +0x0fdc = 0x001f0fea      SS:SP+0x0c  faulting SP  0fea
                            SS:SP+0x0e  faulting SS  001f
```

**That is the DPMI 0.9 16-bit exception frame, byte for byte**, with the return `CS:IP`
left zero for the host to fill. It is not a coincidence: this machinery exists in NT *for
ntvdm's DPMI*, so it emits the shape DPMI specifies.

krnl386's own handler confirms the layout independently — read the guest binary:

```
c5f2  push bp / mov bp,sp
c5f5  mov bl,6 / mov ax,0x203 / int 31h    ; put the previous handler back
c5fc  lea ax,[0xc61d] / mov [bp+8],ax      ; ← the faulting IP slot
c603  mov [bp+0xa],cs                      ; ← the faulting CS slot
c61c  retf
```

`[bp+8]` and `[bp+0xa]` are the faulting IP and CS exactly. It rewrites them so execution
resumes at `0xc61d` instead of at its own invalid opcode, and leaves by a far return.

⇒ **Delivery is three lines:** put a BOP of ours in the two zero words, point `CS:EIP` at
`g_pm_exc[n]`, leave the kernel's `SS:ESP` alone. The handler's `retf` lands on
`DPMI_FLTRET_COFF`, where the frame that remains is `errcode, IP, CS, FLAGS, SP, SS`, and
the resume is those values.

Measured, first try:

```
EXC: -> client handler for 0x06 at 0x01cf:0xc5f2 frame{err=0 cs:ip=0x01cf:0xc5f0
     fl=0x3246 ss:sp=0x001f:0x0fea} retf-> 0x0157:0x06e0
EXC RETURN -> resume 0x01cf:0xc61d fl=0x3246 ss:sp=0x001f:0x0fea err=0
```

`0xc61d` is the address krnl386's handler wrote into the frame. Round trip closed.

## 2. ★ The fault table is indexed by the EXCEPTION VECTOR, not an NT "class"

Session 19 read the handler table as indexed by *fault class*, with "KiTrap0D pushes 6 for
a #GP". Session 32 then filled all eight entries with the **same** `{selector, offset}` so
that no fault could kill the VDM silently — which stopped the silent kills and threw away
the index, the only channel that says *which* exception fired.

⚠️ **And the obvious reading was unfalsifiable**, because the RE'd class for a #GP is 6 and
#UD is x86 vector 6 as well. So: one 4-byte BOP site per index, and let the reflected EIP
name it. Then widen the table from 8 to **32** entries, because eight can never distinguish
"index == vector" from "index == a class that covers both" — index 13 has to be *reachable*
before the two readings can diverge.

They diverged in the next run:

| fault | index used |
|---|---|
| `0f ff` UD0 (x86 vector 6) | **6** |
| `mov cx, es:[bx+2]` on a null selector (#GP, x86 vector 13) | **0x0d** |

**The table is indexed by the x86 exception vector.** Session 19's class reading is
refuted. The 8-entry table could never have held an entry for a #GP at all — every #GP
before this session was dispatched through whatever index 6 happened to be.

The dispatch reads the index as the exception number, and is **guarded**: it delivers only
if the client has actually registered a handler for that exception, and otherwise stops
with a line naming the index. A wrong reading cannot call the wrong handler.

## 3. `INT 21h AH=52h` had no protected-mode thunk, and that was the #GP

The very first fault the new delivery path carried was ours:

```
INT21h AH=0x52 (PM thunk TODO) ... ES=0x00000000
```

At `seg1:0xbf94` krnl386 allocates one descriptor, calls `AH=52h`, and immediately does
`mov cx, es:[bx+2]`. We never set `ES:BX`, so it dereferenced the null selector. Same
shape as `AH=34h` and now the same treatment: run the V86 handler, then turn the SysVars
segment into a selector with `dpmi_seg_to_desc()`.

Not a mysterious fault — a function not answering, which only became *reachable* once
exceptions were delivered.

## 4. ⚠️ A zero chain head is not an empty chain — 117 MB of it

With `AH=52h` answering, krnl386 walked what we gave it:

```
bfaf  xor bx,bx
bfb1  mov cx, es:[bx+4]         ; entries in this block
bfb5  add ah, cl                ; running total
bfb7  cmp word ptr es:[bx], -1  ; offset FFFFh == end of chain
bfbb  je  0xbfcb
bfbd  mov cx, es:[bx+2]         ; next segment
bfc1  mov dx, es:[bx]           ; next offset
bfc6  call 0xbfde               ; re-base its scratch selector on it
bfc9  jmp  0xbfaf
```

`SysVars+4` — the system file table chain head — was zero. So it re-based its scratch
selector on `0000:0000` and read **the IVT** as an SFT header; word 0 there is not
`FFFFh`, so it followed the "next" pointer into the ROM and round a three-address cycle,
`0x00000000 → 0x000fa357 → 0x000bc370 → 0x00000000`, forever. One run produced **117 MB**
of `INT 31h 0007/0008`.

The terminator is at least as much the point of that structure as the count is. Shape
taken from stock rather than recalled: `lolprobe-stock-ntvdm.txt` has `SysVars+4 = A7:00CE`
and the block there reads `00 00 | 2A 03 | 05 00` — next `032A:0000`, five entries.

## 5. krnl386 counts file handles, and 64 is a number it refuses

With a terminated chain the walk ended and krnl386 exited quoting our own number back:
`ExitKernelThunk` carrying **0x40**. The threshold is in its code, twice:

```
bf7a  mov byte [bp-5], 0x7f     ; 127
bf8b  mov byte [bp-5], 0x64     ; 100, on the dx < 8 branch
bfcb  mov al,[bp-5] / cmp ah,al / jae ok  →  else the error exit at 0x987a
```

So `DOS_MAX_FILES` 64 → **128**, and the SFT advertises exactly that.

⚠️ **This is the real table, not a number to pass a check.** `dos_machine_t::fh[]` is 128
entries now and every bound in `dos_int21.c` moved with it; claiming 128 while keeping 64
slots is the "runs but lies" failure this project has paid for before.

## 6. ⚠️ The host pool overflowed silently — one release after writing the warning down

`wow_host_alloc()` carries a note: on the WOW path host memory must come from the pool,
and *a failure there looks like the guest's fault*. The SFT is `0x1d9` paragraphs and is
claimed in `wow_place_v86`; the 256-vector default PM handler table is `0x40` and is
claimed lazily at the mode switch. Sized at `0x200`, the SFT fitted and the handler table
did not — and **both of that function's failure paths were bare `return`s**.

The run died at PM step `0x0d` with nothing in the log pointing anywhere near it. Pool →
`0x400`, and a function that installs 256 interrupt vectors now says so when it cannot.

## 7. ★★ krnl386 says why it is giving up, and we were not reading it

Its last act before `ExitKernelThunk` is WOW32 `0xc4` — the fatal error box — and two
words of that frame are a far pointer to the message. There are seven strings in `seg1` at
`0xb937..0xba54` naming **five different failures**; picking between them from surrounding
behaviour is exactly the reasoning-instead-of-measuring this project keeps paying for.

The unimplemented-BOP path now prints them. No assumed frame: it tries every adjacent
`(offset, segment)` pair and prints only those that resolve to a real selector *and* look
like a C string, so a pair that is not a string prints nothing rather than a plausible lie.

```
FUNC=0xc4 MessageBox? args=0xe from=0xca37 (0000 8008 0000 b937 01cf 0ed0 001f)
  ★ arg[0x03] 0x01cf:0xb937 = "NTVDM KERNEL: Missing 16-bit system module"
```

**A new error, and the fourth of the five to be reached.** Not "Unable to initialize heap"
(cleared, session 31), not "Unable to open KERNEL executable" (cleared, session 31), not
"Inadequate DPMI Server". It is past its own module entirely and failing on another one.

## 8. Following SYSTEM.DRV from the open to the message box

With krnl386 self-reporting, the rest of the session was one loop: read the log, read the
guest binary, fix one thing, measure. Five walls, all ours.

**8a. WOW32 `0x98` is the file SEEK, and it was "unimplemented".** Between two reads of
SYSTEM.DRV krnl386 calls it with offset `0x0400` — which is that file's `e_lfanew`,
checked against the bytes on disk. Stepping the seek over left the file position where the
MZ read had left it, so it parsed whatever followed the MZ stub as an NE header.

**8b. ⚠ `wowdecline.py` was UNDER-REPORTING, silently.** It finds decline sites by the test
after the call — `test dx` or `cmp ax,0xffff` — and this site tests neither:

```
549b  call 0xb211      ; WOW32 0x98
549e  inc  dx          ; 0xffff + 1 == 0 -> ZF
549f  jne  0x54a4      ; SERVICED path
54a1  jmp  0x55a1      ; the sentinel FALLS THROUGH -> lcall cs:[0x3c]
```

The sentinel case is the fall-through, the mirror image of the `je` sites, so the tool
listed ten sites and never mentioned `0x98` at all. For a tool whose entire job is "which
calls may be declined", reporting by omission is the worst failure mode it has. Fixed, and
the count went **7 → 12** declinable sites.

**8c. ★★ DECLINING IS A PROPERTY OF THE CALL SITE, NOT OF THE ID.** The widened scan
immediately showed why that matters: krnl386 calls `0x97` (read) from two places with
opposite meanings.

```
seg1:0x5570  inc dx / je 0x55a7 -> mov ah,0x3f / jmp 0x56c8 -> lcall cs:[0x3c]
             ⇒ 0xFFFF means "ask real DOS". A decline is a true statement.
seg1:0x8a4e  inc dx / jne 0x8a59 -> xor dx,dx / or ax,0xffff / dec dx / retf
             ⇒ 0xFFFF is RETURNED TO THE CALLER as a failure.
```

`wow32_may_decline()` was keyed by ID, so we were declining at the second site too — turning
"we did not implement this" into "the read failed", which is a wrong answer rather than a
missing one. The log shows it plainly once you look: no `INT21h AH=3F` follows that one,
unlike every other read in the run. It is keyed by `(id, call site)` now, from the tool's
output, and `0x6f` (write) has the same split.

**8d. So the read that cannot be declined had to be implemented.** Argument layout measured,
not assumed — two calls in one run, and the first was followed by the chained `AH=3Fh`
reading exactly `0x40` bytes into that same buffer, which pins words 4-5 as a DWORD count,
6-7 as a 16:16 pointer and 8 as the handle. `DX:AX` is a 32-bit byte count, not a flag: the
failure path builds `0xFFFFFFFF` in `DX:AX`.

⇒ **"Missing 16-bit system module" cleared.** No message box in the run at all.

**8e. Two half-instruments, fixed because they cost runs.**
- The PM `AH=42h` (lseek) arm **logged nothing**. Every other file arm printed. The seek was
  being serviced correctly and the log's silence read exactly like it never happened, which
  sent the search after the chain-to-DOS path instead of after the real blocker.
- `INT21h AH=3F read 0x40b` printed the count and not the destination, so it could not
  distinguish a read that filled the buffer the guest meant from one that filled a different
  buffer. It prints selector:offset, the linear address, the file position and the first
  bytes now — which is how `4d 5a` (MZ) and `4e 45` (NE) became visible in sequence.
- These PM arms also still carried a hard-coded `h < 24` handle bound, three sessions after
  the table stopped being that size. Now `DOS_MAX_FILES`.

## 9. ★★ A reserved LDT index is not a read-only one

The next fault was a `#GP` at `seg1:0x8d80`, `mov bx, es:[bx]` — resolving an **imported**
relocation through the module reference table. The new register dump made it decidable in
one run instead of several:

```
ebx=0x0000038a  es=0x01b7{base=0x0002df00 lim=0x0000013f}  bytes@fault=26 8b 1f
```

`BX = (modref-1)*2 + es:[0x28]`, and `BX` is `0x24b` past the end of the selector. So either
`ne_modtab` was wrong or the module index was. **Dump the object, do not reason about it** —
`@es:0000` showed the database is *flawless*:

```
4e 45 ... 02 00 01 00      "NE", ne_cseg=2, ne_cmod=1
40 00 54 00 6c 00 7c 00    segtab 0x40, rsrctab 0x54, restab 0x6c, modtab 0x7c
```

Every table offset is the file's value **+4** — krnl386 has rebuilt the header with ten-byte
segment records (2 segments × 2 extra bytes), exactly as session 33 saw it do for itself. Its
own code confirms the same number independently: `mov bx, es:[0x26] / inc bx` produced `0x6d`,
and SYSTEM.DRV's file `ne_restab` is `0x68`.

⇒ `es:[0x28]` is `0x7c`, so `[si+4]` — the module index — is garbage, so **the relocation
records are not where krnl386 is reading them.** It reads through a selector based at
`0x2e060`; we had read the segment image to `0x1be20`.

**The cause is a guard of ours.** krnl386 stages the read through selector `0x17`, having
re-based it first — and `INT 31h 04F2` (commit descriptors) said:

```
INT31h AX=0x04f2 BX=0x00000017 CX=1 commit 1 from sel 0x17 -> installed 00000000
```

`if (a < DPMI_LDT_RESERVED) continue;` — index 2 is below the line, so the re-base was
**discarded, silently**, and the read went to the stale base while the walk read the new one.

The reason that guard exists is the **access byte**, not the base: a client that retypes the
initial DS/SS to code faults on its next stack write (GH #18 run 69). So take the base and
limit and let `dpmi_install()` apply its existing idx-2/3 force-to-writable-data rule, which
is where that rule already lives. Index 0 stays untouchable.

## 10. ⚠ An instrument that names half an address

`from=0x09bf` sent me to disassemble krnl386's segment 1, where `0x9bc` is the middle of a
six-byte `test` — the decode did not align, which is the only reason it was caught. The
frame word above the one being printed said `0x01d7`: **a different krnl386 segment**. The
call site printed as `from=SEG:OFF` now. Disassembled in the right segment it decodes cleanly
and matches the observed arguments word for word.

## 11. An interlude: the raw `INT 21h` (superseded by part 12)

PM step **`0xbf`**. The fault has moved and changed *kind*:

```
#GP err=0x0000010a  cs:ip=0x000f:0x01f7  es=0x0000{NO DESCRIPTOR}
```

`err=0x010a` decodes as **IDT bit set, vector 0x21** — a raw `INT 21h` executed from code the
INT-site patcher never rewrote. That is the hazard session 30 wrote down ("a PM guest cannot
reach the IVT, so any `INT nn` absent from the patcher's list stays a raw `CD nn` and
**silently terminates the VDM**") — and it is not silent any more. Exception delivery turned
the project's oldest silent killer into a fault with an error code that names the vector.

## 12. ★★ A raw `INT nn` in protected mode is now serviceable, not fatal

The `#GP err=0x010a` at `0x000f:0x01f7` decoded, and the bytes confirmed it:

```
bytes@fault = cd 21 86 e0 a3 13 00 cd
```

`0x0f` is the DPMI *initial* CS, and `04F2` showed why it pointed at unscanned code:

```
04F2 ... commit from sel 0x0c -> installed idx 1 base=0x03b30000 acc=0xfb
```

krnl386 loads SYSTEM.DRV into the block WOW32 `0xb8` VirtualAlloc'd and **re-bases the
initial code selector over it**, then executes there. Which the LDT fix in part 9 is what
made possible — index 1 is below `DPMI_LDT_RESERVED` too.

**The INT-site patch loop carried the identical guard**, so the descriptor was installed and
its `CD nn` sites were left raw. Widening that loop was necessary and *not sufficient*:

```
DPMI: code region 0x03b30000..0x03b3045f -> patched 00000000 INT sites
```

⚠ **The region was empty when it was declared.** krnl386 commits the descriptor *first* and
copies the code in afterwards, so **no commit-time hook can ever cover this** — it is the
mirror image of session 33's problem, where krnl386 copied code we *had* patched and left
the address-keyed vector behind.

⇒ **So service it where the CPU tells us.** A `#GP` whose error code has bit 1 (IDT) set is
not an exception the client asked for; it is a software interrupt the host failed to
intercept, and bits 3..15 are the vector. Confirm against the instruction bytes (`CD nn`
with `nn` matching), put the guest back on the faulting `CS:IP`/`SS:SP`/FLAGS, and run it
through the same `dpmi_service_pm_int()` the patched BOP path uses.

This retires the whole class. Any `CD nn` in protected mode, in any code we never scanned,
is now serviceable instead of fatal — and it is what a DPMI host is supposed to do anyway.
The site is patched on the way past (`C4 C4` in place plus the vector in the address-keyed
map) so the second execution takes the fast path; **that patch needs no length heuristic,
because the CPU has just executed those two bytes AS an interrupt**, which is the strongest
possible version of the evidence `x86len.h`'s vote was approximating.

Measured: **two** raw interrupts serviced in the next run, `INT 21h` and `INT 11h`, and PM
step `0xc0` → **`0xd9`**.

⚠ One refinement went in with it: the part-9 widening now requires the **present bit**
before honouring a low-index commit. An empty shadow slot commits as `base=0 access=0`, and
two of those arrive before krnl386 has written anything real — honouring them handed the
initial DS a base of zero. A blank entry is not the guest asking for something; it is the
guest having asked for a slot.

## 13. Where it stops now — the module-load boundary

```
FUNC=0x2d WowLoadModule args=0xc from=0x01d7:0x0f72 (0x0ec6 0x001f 0 0 0xffff 0xffff)
  -> UNIMPLEMENTED, STEPPED OVER
```

krnl386 is asking the **32-bit side to load a Win16 module**. Its caller, now readable in
the right segment:

```
0f53  sub sp,0x50 / mov di,sp / push ss / pop es   ; a 0x50-byte out-buffer
0f5f  push [bp+0xc],[bp+0xa],[bp+8],[bp+6]         ; its own four arguments
0f6b  push es / push di                            ; + far pointer to that buffer
0f6d  lcall 0xf08:0xb40c                           ; WOW32 0x2d
0f72  cmp ax,0x21 / jb 0x0fbf                      ; ★ AX < 0x21 is the ERROR path
```

⚠ **Everything after that `cmp` in this run is meaningless.** We stepped the call over, so
the return slot held whatever was on the stack — which happened to be ≥ 0x21, so the guest
took the success path it should have failed, and died three instructions later storing
through a null `ES` it had loaded from its *own* caller's argument (`les si,[bp+6]`, and
`[bp+6]` is `0000:0000`). The "UNIMPLEMENTED, STEPPED OVER" line says exactly this:
*findings that depend on execution after an unimplemented BOP are suspect.* The null-ES
fault is not a bug to chase; it is the shape of a missing answer.

## Regression

- `./tools/dostest/run.sh` — 209 checks, 0 failed.
- `./scripts/check-imports.sh` — XP-safe.
- `TIMEOUT=200 ./scripts/bmqueue.sh selftest.com` on the rig — **ALL TESTS PASSED**,
  File I/O included, which is the test that exercises the resized handle table.

---

## ▶ RESUME HERE — session 34 handoff

### Where it is

PM step **`0xd9`**. krnl386 opens, seeks, reads, relocates and loads `SYSTEM.DRV`, services
two raw protected-mode interrupts through code we never scanned, and stops here:

```
FUNC=0x2d WowLoadModule args=0xc from=0x01d7:0x0f72 (0x0ec6 0x001f 0 0 0xffff 0xffff)
  -> UNIMPLEMENTED, STEPPED OVER
```

That is the **16→32 module-load boundary** — krnl386 handing a module off to the 32-bit
half. It is the frontier now, and it is a real piece of work rather than another one-line
gap.

⚠ **Do not chase the null-`ES` fault that follows it.** It is downstream of a stepped-over
call: the return slot held stale stack, the guest passed a `cmp ax,0x21` it should have
failed, and stored through a pointer it had loaded from its own caller's argument. See
part 13.

### The next run

**Implement WOW32 `0x2d` WowLoadModule.** What is already pinned:

- **Six words of arguments** (12 bytes), pushed by `seg2:0x0f5f..0x0f6c` as the caller's own
  four (`[bp+6]`, `[bp+8]`, `[bp+0xa]`, `[bp+0xc]`) followed by a far pointer to a
  **`0x50`-byte output buffer** the caller carves on its own stack (`sub sp,0x50`, `es=ss`,
  first word zeroed).
- **`AX < 0x21` is the error return** (`0f72 cmp ax,0x21 / jb 0x0fbf`), so the success value
  is at least 0x21 — consistent with a module handle or selector rather than a boolean.
- Our NE loader already loads, relocates and binds the whole XP WOW module set, so the
  answer probably exists in `g_wow_mod[]` already; the work is the calling convention and
  the shape of that 0x50-byte buffer, not the loading.
- `tools/ne/nedis.py guest/ne/krnl386.exe 2 0x0f30 40` for the caller, and `--wowfunc 0x2d`
  for the stub and its other call sites.

Also outstanding, in the same window: WOW32 `0x39` (from `seg2:0x09ba`, args
`(ds:0x0b00, module_db_sel, es:[0x26]+1, 0)` — `es:[0x26]+1` points at the **module name**
in the resident-name table, so a name lookup or registration).

### Ruled out / settled this session — do not re-open

| lead | verdict |
|---|---|
| "The faulting CS:EIP cannot be recovered; the `VTIB_FLT_SAV*` slots need calibration" | **settled** — they are SS, ESP and EIP, and they are also unnecessary: the kernel's own frame carries all six fields |
| The fault table is indexed by an NT fault *class* (session 19, "KiTrap0D pushes 6") | **REFUTED** — #UD → 6, #GP → 0x0d. It is the x86 vector |
| Eight table entries are enough | **refuted** — index 13 was unreachable, so every #GP was mis-dispatched |
| The exception frame has to be built by us | **refuted** — NT builds it, in DPMI 0.9 16-bit layout, and leaves only the return `CS:IP` |
| `wowdecline.py`'s list is the list | **refuted** — it missed five sites by only understanding `je`; the sentinel case is the *fall-through* at a `jne` site |
| Patching INT sites when the client declares a region CODE is enough | **refuted** — krnl386 declares the region BEFORE copying code into it (`patched 00000000 INT sites`, then it fills the region and executes). No commit-time hook can see content that has not arrived; the fault path is the only place that always knows |
| The null-`ES` fault after `WowLoadModule` is a bug | **no** — it is downstream of a stepped-over call whose stale return passed a `cmp ax,0x21`. Implement `0x2d` before reading anything after it |
| Declining is a property of the ID | **refuted** — `0x97` and `0x6f` each have one site that chains to DOS and one that returns the failure to the app |
| The SYSTEM.DRV module database is malformed | **refuted by dumping it** — `"NE"`, `cseg=2`, `cmod=1`, and every table offset is the file's +4 for ten-byte segment records. The image was in the wrong place, not the header |

### Instruments added (all of these were earned)

- **A `#GP` with the IDT bit set is serviced as the interrupt it is** — vector from the
  error code, confirmed against the `CD nn` bytes, run through `dpmi_service_pm_int()`, and
  the site patched on the way past. This is a capability, not just an instrument: raw
  `INT nn` in unscanned PM code no longer kills the VDM.
- The kernel's fault frame is dumped **with its offsets** on every reflect (`FLTSTK`).
- **One BOP site per exception vector**, so the reflected EIP names the exception.
- Every delivered fault prints the **guest register file, DS/ES with base+limit+AR, the
  bytes at the fault, and `@es:0000`** — which is what turned "the module database is
  wrong" into "the database is perfect, the image is misplaced" without a second run.
- WOW32 `0xc4` prints its **string arguments** — krnl386 self-reports.
- WOW32 call sites print as **`from=SEG:OFF`**. Printing the offset alone sent this session
  to disassemble the wrong krnl386 segment; the decode failing to align was the only reason
  it was caught.
- `INT21h AH=3F` prints **where the read landed** (selector:offset, linear, file position,
  first bytes), and `AH=42h` prints at all — it was completely silent.
- `dpmi_install_default_pm_handlers()` says so when it cannot allocate.

### Standing hazards

- The DPMI fault stack is a fixed selector based at linear `0x2000`, and krnl386's handler
  writes stack bookkeeping through `SS:0x0a..0x0f` → linear `0x200a`. That is unallocated
  slack **by luck, not design**; if the layout shifts, look here.
- `wow_host_alloc()` returning 0 is silent at most call sites. Two are loud now.
- The 128-entry handle table is real, but nothing yet ties SFT *entries* to `fh[]` — the
  block advertises the right count with zeroed (free) entries. Fine until a guest walks
  them.

### The loop

```bash
./scripts/build.sh
ARCHIVE=build/wowruns ./scripts/bmwow.sh              # a WOW round (~90s)
./tools/dostest/run.sh                                # 209 checks, off-VM
./scripts/check-imports.sh build/ntvdmhost.exe
TIMEOUT=200 ./scripts/bmqueue.sh selftest.com         # the OTHER guest class, real HW
python3 tools/ne/wowdecline.py guest/ne/krnl386.exe   # which calls may be declined
python3 tools/ne/nedis.py guest/ne/krnl386.exe <seg> <start> <count>
```
