# Session 41 — the message loop turns, on a real keystroke

- **Branch:** `m9/completeness`
- **Issue:** [#128](https://github.com/MrMatthewLayton/ntvdmex/issues/128)
- **Predecessor:** [session 40](session-40.md) — the host learned to **call 16-bit code**,
  `SYSEDIT.EXE` built its whole MDI interface and read two of its four files into memory,
  and then asked `GetMessage` and was told to quit.

---

## ★★★★★ THE HEADLINE

**A key pressed on the rig reaches a Win16 window procedure.**

```
WOWMSG: GetMessage with an empty queue -- BLOCKED for 0x30d ms; 1 message arrived
GetMessage           -> hwnd=0x0260 msg=0x0100 wParam=0x0041 lParam=0x001e0001 [removed]
TranslateMDISysAccel    hwnd=0x0160 -> 0 (no accelerator table in this host)
TranslateAccelerator    hwnd=0x0140 hAccel=0x0a8e -> 0
TranslateMessage        msg=0x0100 -> 0: this host produces no WM_CHAR
DispatchMessage         hwnd=0x0260 msg=0x0100 -> default procedure
GetMessage           -> hwnd=0x0260 msg=0x0101 wParam=0x0041 lParam=0xc01e0001 [removed]
```

`0x0041` is `VK_A`; `0x0260` is the `EDIT` control **SYSEDIT itself gave the focus to**;
`0x001e0001` is scan code 0x1e with a repeat count of 1, and `0xc01e0001` is the same key
coming back up. Twelve messages in a run, every one delivered and dispatched, and the run
still ends on `ExitKernelThunk(0)`.

Everything a Win16 program does after its startup arrives through that loop. Until it
could turn, nothing else about this half of the project could be measured at all.

**And a defect session 40 located but deliberately did not fix is fixed**, so
`SYSEDIT.EXE` now reads **all four** of its files and shows no message box.

---

## Part 1 — the CF that never reached the guest

Session 40 ran the stock oracle and it ruled: **stock `ntvdm` opens both 0-byte files with
no message box**, so *"Cannot read this file."* was ours. It located the defect in
krnl386's own `_lread` and stopped there on purpose, because the fix had to know *which
flags image the guest restores from*, and inventing a frame shape is how a host corrupts a
stack.

### The measurement, and it is an A/B inside one run

One breakpoint, one instruction wide: `krnl386 seg1:0x4549`, the `jae` whose other arm is
`0x454b mov ax,0xffff`.

| file | AX at the `jae` | `efl` | CF |
|---|---|---|---|
| `SYSTEM.INI` | `0x00e7` | `0x00010206` | **0** |
| `WIN.INI` | `0x01dd` | `0x00010206` | **0** |
| `C:\CONFIG.SYS` | `0x0000` | `0x00010207` | **1** ⇒ `AX = -1` |
| `C:\AUTOEXEC.BAT` | `0x0000` | `0x00010207` | **1** ⇒ `AX = -1` |

Our `AH=3Fh` answered `AX=0 CF=0` for **all four** — the log line says so. So the two
zeroes are the ones whose CF never arrived, and the reason the other two worked is not
that we delivered anything: `_lread` skips its buffer probe on a zero-length read
(`seg1:0x3d96 jcxz`), and that probe's `or byte es:[bx],0` was the only thing clearing the
CF its own `cmp ax,0xffff` had set two instructions earlier.

⇒ **The CF we return has never reached the guest.** A defect that was always there needed
an empty file to expose it.

### Where it went, read out of the guest

```
krnl386 seg1:0x5238  pushf                 ; ★ the flags image an IRET will restore
        seg1:0x56d3  lcall cs:[0x3c]       ; chain to the previous INT 21h handler = US
        seg1:0x56e5  pushf / 0x5706 popf   ; krnl386 PRESERVES what came back
```

and our previous handler, for all 256 protected-mode vectors, is **three bytes**:

```c
stub[off+0] = 0xC4; stub[off+1] = 0xC4;
stub[off+2] = 0xCF;                    /* BOP immediate AND the IRET */
```

Every DOS service in `dpmi_service_pm_int` reports failure in the guest's **live EFLAGS**
and steps EIP past the BOP — and the very next instruction the guest executes is that
`IRET`, which throws them away. That is right for a client that reached us through a
patched INT site or through the `#GP` on a raw `INT nn`, where no frame was ever built.
It is wrong for the way krnl386 gets here, which is the whole of WOW.

The fix puts the answer where a real `INT 21h` handler puts it — the flags image the
caller pushed — which the **V86** arm of this host has always done (`*pfl |= 1` at
SS:SP+4) and the protected-mode arm never did.

- **CF only.** ZF, SF, OF, AF and PF in live EFLAGS at that moment are the guest's own
  leftovers from whatever krnl386 executed on the way to the BOP. Copying those would be
  inventing an answer, silently, in the one place a wrong bit cannot be seen.
- **Keyed on the guest standing in the stub with the IRET as its next instruction** — both
  facts read out of the live machine, so a service that parked a different context (a
  `wowcall.h` callback, an EXEC) is correctly skipped.
- The IRET's width follows the **stub's** D bit; the stack's address width follows **SS's**.

⇒ *"Cannot read this file."* **2 → 0**, and the frontier run is strictly healthier.

---

## Part 2 — the message queue

### What the frontier actually was

SYSEDIT's loop, every call named from its own relocation chain (`tools/ne/neimports.py`):

```
seg1:0x0102  GetMessage(&msg, 0, 0, 0)                 ; USER.108
     0x0112  or ax,ax / jne 0x00c6                     ; ★ non-zero loops; 0 is WM_QUIT
     0x00c6  TranslateMDISysAccel([0x22], &msg)        ; USER.451
     0x00d8  TranslateAccelerator([0x20], [0x4ac], &msg)
     0x00ee  TranslateMessage(&msg)                    ; USER.113
     0x00f8  DispatchMessage(&msg)                     ; USER.114
```

### ★ Four ids the export table could not name, named by the run

`docs/research/wow-user-surface.md` names 385 of USER's 441 ids and **none of these four**,
which reads like "they are 16-bit code inside USER.EXE". The run refutes it. With
`GetMessage` answered the loop turns and they arrive as ordinary BOPs, naming themselves
by their call sites — which are named in turn from SYSEDIT's relocation chain:

| id | args | call site | name |
|---|---|---|---|
| `0x071` | 4 | `sysedit seg1:0x00f8` | **TRANSLATEMESSAGE** |
| `0x072` | 4 | `sysedit seg1:0x0102` | **DISPATCHMESSAGE** |
| `0x0b2` | 8 | `sysedit seg1:0x00ea` | **TRANSLATEACCELERATOR** |
| `0x1c3` | 6 | `sysedit seg1:0x00d4` | **TRANSLATEMDISYSACCEL** |

⚠ The `from` address is the **application's**, not USER's, because USER's exports reach
their stubs by **tail-jump, not by call** — which is what makes a call site able to name a
stub at all.

⇒ So the host fills the MSG **and** dispatches it. `DispatchMessage` is `wowcall_enter`:
session 40's machinery, with no new lines under it.

### ★ The MSG is 18 bytes, and both sides say so

`DispatchMessage` takes nothing but `lpMsg`, so everything a window procedure is called
with is in those bytes.

```
sysedit seg1:0x0102  lea ax,[bp-0x12]     ; the application reserves 18
user    seg1:0x1c43  mov bx,0x12          ; USER probes 18 before walking one
        seg1:0x1c4d  mov cx, es:[bx]      ; hwnd
        seg1:0x1c50  jcxz 0x1c78          ; ★ hwnd 0 -> dispatch nothing
        seg1:0x1c52  push cx / es:[bx+2] / es:[bx+4] / es:[bx+8] / es:[bx+6]
```

⇒ `+0 hwnd, +2 message, +4 wParam, +6 lParam`, then `time` and `pt` filling the rest.
⚠ `time` and `pt` are **not** pinned by any code this host has watched — they are filled
with the tick count and zeroes because leaving 8 bytes of the guest's stack untouched is
worse than filling them, and the choice is on the log line.

GetMessage's argument block was confirmed against a line this host had already printed:
`args=0x0a b=(0x0000 0x0000 0x0000 0x248a 0x0a9f)` — `+6/+8` is that stack MSG and
`+4/+2/+0` are the three zeroes.

### ★ Where a keystroke comes from, and what is relayed rather than invented

The Win16 post hangs off **`host_key_scancode`**, the single choke point a scripted probe
and a human press already share — a second tap for real keys only would have broken the
invariant that file exists to defend.

- **The virtual key code is the OS's answer.** `MapVirtualKey` is what Windows uses on its
  own keyboard path, so it follows the machine's actual layout and nothing is written from
  memory. A scancode it cannot map yields VK 0, posted as it is.
- **The lParam bit field is composed**, and only these bits: repeat count 1, the OEM scan
  code, the extended flag, and for a break the previous-state and transition bits.
  ⚠ Bit 29 — the **Alt context code** — is left 0 because this host tracks no modifier
  state. Stated, not silent.
- ⚠ **`TranslateMessage` returns 0 and that is the true answer, not a stub.** Producing a
  `WM_CHAR` from a virtual key needs keyboard state (shift, caps, dead keys) that nothing
  here keeps, and inventing it would put *wrong characters* into an edit control — the
  "runs but lies" class, in the one place a user would see it.
- ⚠ **`TranslateAccelerator` / `TranslateMDISysAccel` return 0** because this host has no
  accelerator table: `NotifyWow` deliberately does not keep the resource it is shown
  (`GlobalUnlock` is the next instruction). They used to get the same 0 from the harness
  sentinel; same value, different status — this one is a decision.

### ★ Where a keystroke goes: the guest's own choice

`SetFocus` (`0x16`) is implemented because a key has to be **addressed**, and SYSEDIT
calls it once per MDI child it builds (`sysedit seg1:0x02d8`):

```
SetFocus 0x01a0 (was 0x0000) -- keyboard messages now go here
SetFocus 0x01e0 (was 0x01a0)
SetFocus 0x0220 (was 0x01e0)
SetFocus 0x0260 (was 0x0220)
```

So the target of a key is the application's decision, not one this host makes. With no
focus, the message is not posted at all — and USER's own `DispatchMessage` `jcxz`es a null
hwnd anyway, so a key with nowhere to go is discarded *by the guest*, correctly.

### What is deliberately not built

- ⚠ **One queue, not one per task.** The host has one input source and every message is
  addressed to a window this host issued the handle for, so "whose message is it" has a
  single answer today. A second interactive task needs a task field on the entry plus
  krnl386's own task list — not a second copy of the file.
- ⚠ **`GetMessage`'s block is bounded (6 s).** A real Win16 task waits forever; a harness
  run has to end, and a host that hangs on an empty queue looks exactly like one that has
  crashed. When the wait expires the answer is `WM_QUIT` **and the log says the wait
  expired**, so "the application quit" is never confused with "nobody typed".
- The wait happens in `main.c`, not in `wowuser.h`: the thing being waited for is the
  host's keyboard event, and a service that blocks is one that cannot be reasoned about
  from the id space it lives in. ⚠ The host lock is **not** held across it — the UI thread
  takes that lock to push the keystroke.

---

## Measured three ways, because a fix measured on one guest is a fix for none

| | result |
|---|---|
| **frontier** (both switches on, keys scripted) | `661 / 186 / 308 / 150`, 8 windows, 4 MDI children, 4 `EM_SETHANDLE`, **0 message boxes**, 12 messages delivered and dispatched, ends `ExitKernelThunk(0)` |
| **baseline** (both switches off) | `270 / 45 / 122 / 97 · 9·222·39 · 0001:229C` — **exactly** unchanged, and **zero** queue lines: WOWEXEC never reaches its `PeekMessage` |
| **Doom** (the other guest class) | all eleven startup stages `V_Init → ST_Init`, 3.51 MB |

The CF fix fires 3 times inside the unchanged baseline and **0 times** in Doom — predicted
before that run, because DOS/4GW's `INT 21h` are patched sites in its own code and never
reach our stub.

---

## ▶ RESUME HERE

### 1. ★★★★★ THE FRONTIER: PIXELS

The message loop turns, so the two things it is *for* are now the only things missing, and
they are the same piece of work:

1. **A window has no pixels.** `wowuser_win_t` is a class, a rectangle, a style and a
   text, with a synthetic handle that says it is synthetic — deliberately, since session
   39. Giving it a real host window is what makes `WM_PAINT` honest (a window that has
   been shown and never painted has an update region; one with no pixels does not), and
   `WM_PAINT` is what drags in `BeginPaint`/`EndPaint` and therefore **GDI's id space**,
   which this host does not dispatch at all.
   ⚠ Do **not** synthesise `WM_PAINT` before there is something to paint on. A window that
   reports an update region it does not have is the same lie one level down.
2. **`ShowWindow` (`0x2a`) and `UpdateWindow` (`0x7c`) are called and unimplemented** —
   `sysedit seg1:0x01da` (the MDI client, inside `WM_CREATE`) and `seg2:0x0149` /
   `seg2:0x0152` (the frame, from `WinMain`). They are the guest telling us its window is
   meant to be visible, and they are the natural first half of (1).
3. `SYSEDIT`'s frame procedure does **not** handle `WM_PAINT`: `seg1:0x0177` passes
   anything it does not recognise to `DEFFRAMEPROC` (`USER.445`, id `0x1bd`), which under
   WOW is **ours**. Its own switch handles only `WM_CREATE`, `WM_DESTROY`, `WM_CLOSE`,
   `WM_QUERYENDSESSION`, `WM_WININICHANGE`/`WM_DEVMODECHANGE`, `WM_COMMAND` and
   `WM_INITMENUPOPUP` — read at `seg1:0x0149`-`0x0175`.

### 2. The nearest smaller pieces, in order of how much a run wants them

- **`DefFrameProc` / `DefMDIChildProc` / `DefWindowProc`** (`0x1bd`, `0x1bf`, and
  `DefWindowProc` is one of the unnamed ids — its call site will name it the moment a
  program takes the default path). Everything SYSEDIT does not handle goes there.
- **`WM_CHAR`**, which needs the host to keep keyboard state and call `ToAscii` — see the
  note above for why it is 0 today and what would make it 1.
- **An `EDIT` control that does something with a keystroke.** Its window procedure is ours
  (system class), and it currently answers `msg 0x0100 not implemented`. That is honest
  and it is also the visible end of the chain.

### 3. Closed this session

- **"Cannot read this file."** — located in session 40, fixed here. Our protected-mode DOS
  services now put CF where the guest reads it.
- **`GetMessage` answers the harness sentinel.** There is a queue.
- **Nothing feeds a Win16 queue.** The host's own keyboard does.
- **The host cannot dispatch a message.** `DispatchMessage` is a service and it is
  `wowcall_enter`.
- **`TranslateMessage` / `DispatchMessage` are 16-bit code inside USER.EXE.** They are
  not — ids `0x71` and `0x72`, named by their call sites. (This was written down as a
  finding earlier in the session, from the surface doc's silence, and the run refuted it.
  The doc's silence meant "the export table could not name it", not "there is no stub".)

### How to drive it

```bash
ARCHIVE=build/wowruns ./scripts/bmwow.sh              # deploy, run, collect
ARCHIVE=build/wowruns ./scripts/bmwow.sh --no-deploy  # re-run what is on the box
```

- **Three switches now, and the frontier needs all three:**
  ```bash
  touch /private/tmp/xpshare/wowsched.txt   # the Win16 task scheduler (session 38)
  touch /private/tmp/xpshare/wowcall.txt    # calling 16-bit code      (session 40)
  printf '20\r\n' > /private/tmp/xpshare/qimode.txt          # bit 5 = the key script
  printf 'w1200 1e w400 1e w400 1e w400 1e w400 1e w400 1e\r\n' \
      > /private/tmp/xpshare/keys.txt                        # six taps of 'A'
  ```
  ⚠ **`qimode.txt` and `keys.txt` must be put back** (`0` and `w40000`) before measuring a
  baseline, or the run carries six keystrokes it did not have last time.
- ⚠ **The share is not mounted in a fresh session.** Nothing works until
  `mkdir -p /tmp/xpshare && mount_smbfs -N //guest@192.168.1.29/ntvdmex /tmp/xpshare`,
  and that plus every later write to it needs the sandbox disabled.
- ⚠ **Always re-run with the switches OFF and confirm the baseline.** It did **not** move
  this session:
  ```bash
  L=build/wowruns/<the run>.log
  grep -c "WOWBOP 0x51" $L                              # 270
  grep -c SERVICED $L                                   #  45
  grep -c DECLINED $L                                   # 122
  grep -c "UNIMPLEMENTED, STEPPED OVER" $L              #  97
  grep -o "task=0x[0-9a-f]*" $L | sort | uniq -c        # 9 · 222 · 39
  grep -c 0001:229C $L                                  # the WOWEXEC GP box
  ```
- ★ A healthy **frontier** run: `grep -c 'GetMessage ->'` is **12**, `DispatchMessage` is
  12, `grep -c WOWCALL` is 23, `CreateWindow "` is 8, `WM_MDICREATE` is 4,
  `EM_SETHANDLE` is 4, **`Cannot read this file` is 0**, and it ends on
  `★ ExitKernelThunk(0x00000000)`.
- ★ The PM breakpoint that settled Part 1 is worth keeping as a pattern —
  `4549 0 0 2 1` in `pmbp.txt` (`PMBP=1` to stop `bmwow.sh` disarming it): column 4 bit 1
  means "the address is an offset in krnl386's seg1", column 5 `rep=1` means repeating,
  and `efl` on the hit line is the whole measurement.

### Ruled out — do not re-try

Everything in [session 40's list](session-40.md#ruled-out--do-not-re-try) still holds, plus:

- **Fixing the zero-length read by special-casing it.** The guest is correct at every
  step; the host was losing the flags for *every* read and only an empty file could show
  it.
- **Mirroring the whole flags word into the IRET frame.** Only CF is computed by these
  services; the rest are the guest's own leftovers.
- **Reading the surface doc's silence as "there is no stub".** It means the export table
  could not name the id. Four of the 56 unnamed ones are the message loop.
- **Fabricating a `WM_PAINT` to give the loop something to do.** There is nothing to paint
  on yet, and a window that claims an update region it does not have is a lie the next
  session would have to unpick.
