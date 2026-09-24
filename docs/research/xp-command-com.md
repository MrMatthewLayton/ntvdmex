# XP's own COMMAND.COM as a guest — what actually stops it

**Measured:** 2026-09-24, bare-metal rig, host `bbea3134`.
**Why:** *"If I open stock NTVDM on XP it gives me a DOS prompt, so NTVDMEX should
do the same"* — and the right shape for that is not a shell we write, it is
**`COMMAND.COM` as the guest**, exactly as stock does it.

---

## It is not "we need a shell" — it loads and runs today

`C:\WINDOWS\SYSTEM32\COMMAND.COM` (50,620 bytes) **loads and runs** under NTVDMEX
today. Launched with `scripts/bm/dosrun.bat - C:\WINDOWS\SYSTEM32\COMMAND.COM`:

```
STAGE2: loaded 0x0000c5bc from C:\WINDOWS\SYSTEM32\command.com
STAGE2: running .COM (entry 0x0100:0x0100)
STAGE2: BOP2F ax=0xb700   APPEND install check
STAGE2: BOP2F ax=0x4300   XMS install check
STAGE2: BOP2F ax=0x4310   XMS entry point
STAGE2: BOP2F ax=0x5501   COMMAND.COM interface
STAGE2: BOP2F ax=0xb707
STAGE2: BOP2F ax=0x122e   ×5      <- then it terminates, cleanly, printing nothing
STAGE2: complete
```

## ⛔ The DOS version IS necessary — I said it was a red herring and it is not

`main.c` (~23206) records that XP's COMMAND.COM refuses 6.22 outright — *"Incorrect DOS
version"* — and there is a `cfg\dosver.txt` knob. I wrote here that **"forcing 5.0
changed nothing: the same five calls, the same clean exit."** That was wrong. I compared
the two runs' SUMMARY lines instead of their calls. Counted properly:

| reported version | INT 2Fh calls COMMAND.COM makes |
|---|---|
| **6.22** (our default) | **zero** — it refuses at the version check and exits |
| **5.0** | **nine** — `b700 4300 4310 5501 b707` then `122e` ×5 |

⇒ **The version override is the difference between "refuses immediately" and "gets to
the last step before printing".** It is step one, not a distraction.

⚠ The user found this, not me: they opened `System32\COMMAND.COM` by hand with the
override removed and got *"Incorrect DOS version"* on screen — the failure I had just
told them did not matter.

⚠ **And one of my own readings was wrong on the way here.** I reported *"zero INT 21h
calls — the classic tell that the guest did nothing"*. The guest was running the whole
time; my grep matched one log format while these are logged as `BOP2F` lines.
**An absence produced by a bad pattern is not an absence in the report.**

---

## What `AX=122Eh` is, read off COMMAND.COM itself

`from=` in the log is our own INT 2Fh BOP stub, not the caller, so the call sites were
found statically: seven `B8 2E 12` (`mov ax,122Eh`) in the binary, **four consecutive at
file offsets `0x7d2b`–`0x7d58` and a fifth at `0x7d85`** — exactly the 4+1 observed.
Disassembled (`.COM`, so guest offset = file offset + `0x100`):

```asm
00007E25  xor cx,cx
00007E27  mov es,cx
00007E29  xor di,di            ; ES:DI = 0000:0000 GOING IN
00007E2B  mov ax,0x122e
00007E2E  mov dl,0x0           ; DL selects the table: 0, 2, 4, 6, then 8
00007E30  int 0x2f
00007E32  mov word [0x917a],es ; ...and it STORES WHAT COMES BACK
00007E36  mov [0x9178],di
          ; repeated verbatim for dl=2 -> [0x9182]:[0x9180]
          ;                      dl=4 -> [0x9192]:[0x9190]
          ;                      dl=6 -> [0x9176]:[0x9174]
00007E67  call 0xaf03          ; two more table getters that are not 122Eh
00007E7A  call 0xaf24
00007E85  mov ax,0x122e
00007E88  mov dl,0x8           ; the fifth -> [0x919a]:[0x9198]
```

⇒ **`AX=122Eh` with `DL=n` is a GET: it must return a far pointer in `ES:DI`.**
COMMAND.COM zeroes `ES:DI` first, calls, and stores the result into its own
table-pointer variables.

**Our INT 2Fh handler passes everything it does not recognise straight through** — a
deliberate policy, and documented as such (`main.c` ~24866: *"we pass everything we do
not recognise straight through, so the guest reads its own registers back as our reply
— the same 'does nothing, reports success' shape as the DPMI 0300 bug"*). Here the
guest supplied `0000:0000`, so it reads `0000:0000` back and **stores five null
pointers**, then terminates without printing.

⚠ `DH` in the log is `0xBA` — leftover, because the code only ever sets `DL`. Only
`DL` is the selector.

⚠ **What the five tables CONTAIN is still unknown and is not guessed here.** The
observed facts are the selector values and that a far pointer is expected back.

---

## The cheap way to learn the table format — ground truth exists

XP's COMMAND.COM **works under stock ntvdm on this same box**, so stock implements
`122Eh` and the correct answers are sitting in memory a few feet away. The machinery to
read them already exists:

1. `scripts/bmstockdump.sh` dumps a **live stock VDM** from outside with `vdmdump.exe`.
2. Run COMMAND.COM under stock (`scripts/bm/w16stock.bat` is the pattern for dropping
   the IFEO key and restoring it), dump, then read COMMAND.COM's own variables at
   `[0x9178]/[0x917a]`, `[0x9180]/[0x9182]`, `[0x9190]/[0x9192]`, `[0x9174]/[0x9176]`,
   `[0x9198]/[0x919a]`.
3. Those five far pointers name the tables; dump the bytes at each.

⇒ The format does not have to be recalled or inferred from a spec we do not hold. **It
can be read off the machine that already answers correctly.**

---

## What the prompt actually costs

| | |
|---|---|
| **Default to `COMMAND.COM` when nothing is named** | Small — one more rung on the target chain. ⚠ It must sit strictly **below `target.txt`**, or the headless harness (which relies on "CSRSS named nothing → `target.txt`") breaks |
| **Report DOS 5.0 for it** | Trivial — the knob exists; make it automatic when the guest *is* `command.com` rather than a file on the share |
| **Service `INT 2Fh AX=122Eh`** | **The real work.** Five far pointers to correctly-shaped tables. Bounded, and with a live oracle for the contents |
| **Whatever it asks next** | Unknown until `122Eh` answers. The same loop applies |

⛔ **`cfg\dosver.txt` was REMOVED from the rig afterwards.** Left at 5.0 it would have
made every later DOS test report a different DOS version with entirely plausible logs —
the `pmnoirq.flag` shape.

A copy of the binary under test is at `debug\out\XPCMD.BIN` on the share (not committed
— it is Microsoft's).
