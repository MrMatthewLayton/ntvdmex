# Session 44 — Notepad is a usable text editor, and the method changed

> Eight commits, `f4644a3` … `d82a27f`. Branch `m9/completeness`.

## ★★★★★ THE HEADLINE

**NOTEPAD FROM WINDOWS 3.11 IS A WORKING TEXT EDITOR ON THE WINDOWS XP DESKTOP.**
It opens a file through the real XP file dialog, you type into it, and File > Save
writes the text you typed. Help > About opens. Its menus grey and check their own
items. Measured, byte for byte — after typing `hello` at the start of a 59-byte
file, the file on disk reads:

```
00000000: 6865 6c6c 6f4e 5456 444d 4558 206f 7065  helloNTVDMEX ope
00000010: 6e65 6420 7468 6973 2066 696c 6520 7468  ned this file th
00000020: 726f 7567 6820 7468 6520 7265 616c 2058  rough the real X
00000030: 5020 6669 6c65 2064 6961 6c6f 672e 0d0a  P file dialog...
```

Session 43 left Notepad on the desktop with a menu that did nothing. It is now
the thing the north star names.

---

## ★★★ THE METHOD CHANGED, AND IT WAS THE USER'S CALL

Every service in this host had been found by RUNNING a guest and reading which
call it stopped on. That works, and it is why nothing here is guessed — but it
finds exactly **one wall per deploy/run/read cycle**, and it cannot see a call
that fails QUIETLY. Two did this session: File > Save did nothing at all, and
File > Open killed the app. Neither announced a missing service.

The user proposed: *disassemble the exe, find all its calls, implement them.* I
had argued against "implement the API" earlier in the session, and I was drawing
the line in the wrong place. **"Implement everything one binary calls" is finite,
enumerable from the file, and every entry is a call the program really makes.** It
also gives what demand-driven work does not: a completion criterion.

### `tools/ne/neneeds.py`

★ **An import is not necessarily work, and computing that distinction is the
whole value.** Most of what a Win16 program imports is 16-bit code inside the
module it imports from, running on the real CPU without asking us. Only an import
that reaches a WOW32 thunk is ours, and both halves are in the binaries:

```
import (module, ordinal) -> that module's entry table -> the bytes there
  `6a AA 68 00 00 68 II II 9a`  ->  a stub: id II, AA argument bytes. OURS.
  anything else                 ->  the module's own code. FREE.
```

⚠⚠ **AN EXPORT DOES NOT POINT AT ITS STUB**, and the first cut got both modules
wrong in opposite directions — COMMDLG reported as "0 need us" and the whole of
USER as native code. Two prologues sit in between:

* **COMMDLG**: `COMMDLG.1 GETOPENFILENAME` → `seg1:0x0000` = `9a c2 00 ff ff`, a
  far call, stub at **+5**. `wowthunks.py` independently puts it at `seg1:0x0005`.
* **USER/GDI**: `USER.56 MOVEWINDOW` → `55 8b ec 68 99 1f 5a 5d e9 e3 eb` — the
  **tail-jump** this project already documented. Following it lands on
  `seg1:0x0b7c`, exactly where `wow-user-surface.md` says its stub is.

The tail-jump is matched EXACTLY rather than by scanning for a jump, because
`USER.107 DEFWINDOWPROC` opens with the same four bytes and is genuinely 16-bit
code. Validated against four answers already known independently.

### What it says

| | |
|---|---|
| **NOTEPAD.EXE** | 118 imports · **34 reach us** · **32 serviced** · 2 to do |
| 19 Win3.11 guests, distinct | USER 84 · **GDI 46** · SHELL 17 · SOUND 6 · KEYBOARD 5 |

**~158 services for the entire shelf**, against the ~1000 thunked entry points
those modules define. That gap is the argument for enumerating per PROGRAM rather
than per API. ⚠ It also retires the "**GDI is 367 stubs**" figure quoted since
session 42: that counted every stub in the module, not the ones any program
calls. **Paint needs 41.**

### ⚠⚠ AND ITS LIMIT, WHICH EARNED ITSELF WITHIN THE HOUR

`native16` does **not** mean free. `USER.174 LOADICON` classifies native16 —
correctly, its entry point is 16-bit code — and is the entire reason id `0xad`
exists, because USER does the resource lookup itself and *then* asks us to build
the object. That internal call is in no import table.

**Four more turned up the same way this session**: `MessageBox`, `CheckMenuItem`,
`EnableMenuItem`, and `LoadIcon` itself. ⇒ **the enumeration bounds the job; it
does not replace the log.**

---

## ★★★★★ IMPLEMENT `MessageBox` FIRST ON ANY NEW GUEST

The single most valuable service in the file, and it is not close. `USER.1
MESSAGEBOX` is a `native16` wrapper (`seg1:0x29e3`) reaching its stub `0x01`
internally, so the enumeration cannot see it — a run found it.

Implemented, **the guest immediately diagnosed its own failures in English**,
three times in one session, each of which had presented for hours as "nothing
happens":

1. *"Cannot open the C:\Documents and Settings\Matthew\My Documents\test.txt
   file."*
2. *"This file is empty and will be deleted. This file cannot be saved because it
   is empty."*
3. *"The C:\DOCUME~1\Matthew\MYDOCU~1\test.txt file is too large for Notepad."*
   — about a **59-byte file**.

Each sentence was the whole diagnosis. Two sessions of guesswork ended on the
first one.

---

## Part 1 — the menu does something

### `WM_COMMAND`, and both the number and the PACKING read out of the guests

Two independent readings, neither of them Notepad's:

* `commdlg seg3:0x0966` chains `cmp ax,0x110 / cmp ax,0x111` — WM_INITDIALOG then
  WM_COMMAND, the pair every dialog procedure handles.
* `sysedit seg1:0x0477` SENDS itself `SendMessage(hwnd, 0x111, <menu id>, 0)`.

⚠⚠ **Win16 and Win32 pack it differently.** Win32 puts the notification code in
the HIGH half of wParam and the control's handle in lParam; Win16 puts the id
alone in wParam and packs `(hwndCtl, notifyCode)` into lParam. Menu clicks agree
by luck; control notifications do not. So it is composed, not relayed — and the
run confirmed the control form independently: Notepad's edit control arrives as
`lParam = 0x01000160`, `EN_SETFOCUS` over the Win16 handle this host issued.

### ⚠⚠ Swallowing `WM_SYSKEYDOWN` had broken EVERY keyboard menu

These were relayed to the guest and returned as HANDLED, so `DefWindowProc` never
saw them — and Alt is how the OS opens a menu bar. Every guest had a perfect menu
with **no keyboard route into it at all**: Alt did nothing, so Alt+H, Alt+F4 and
F10 did nothing either. System keys now fall through (and are still posted).

### Help > About is NOT a dialog — SHELL.DLL, a fourth id space

`neimports.py` names the site outright: `notepad seg1:0x038f SHELL.22 SHELLABOUT`,
with `USER.174 LOADICON` five bytes before it. `SHELL.DLL` on XP is 5120 bytes and
is **34 WOW32 stubs and nothing else** — there is no Windows 3.11 About box in it,
which is why the box that comes up is XP's. *On a real XP box it is XP's too.*

`src/wow/wowshell.h`, anchored id `0x16` / 12 args / retstub `0x00c7`, resolved
non-resident name table → entry table (`seg1:0x00ba`) → the stub bytes.
Measured: `SHELL.DLL's code segment is 0x0b17`, `ShellAbout "Notepad" / ""
owner=0x0140 icon=0x8020 -> the app's own (04 bpp)`, `rc=1`.

---

## Part 2 — the layout cluster, from the run crossed against the file

★★ **The method that produced the list — reuse it.** The host log records every
unimplemented USER call with the return address it came from; **`from − 5` is the
call site**, and `neimports.py` names it out of the guest's own relocation chain.
Four came out, each agreeing with USER's export table independently:

```
0x38  MOVEWINDOW        notepad seg1:0x0061
0x7d  INVALIDATERECT    notepad seg1:0x0044
0xb3  GETSYSTEMMETRICS  notepad seg1:0x0c04
0x1f  ISICONIC          notepad seg1:0x0aba
```

And Notepad's resize helper read straight off the disassembly, so the argument
order is the guest's, not a parameter list:

```
0044  InvalidateRect(hEdit, NULL, TRUE)
0061  MoveWindow(hEdit, 8, 2, [bp+6]-15, [bp+4]-4, TRUE)
```

⇒ `MoveWindow 0x0160 to (0008,0002) 04c4x029b`. The edit control fills its frame.

★ **`DefWindowProc` is not needed**: `USER.107` → entry-table `FIXED, seg 1,
offset 0x1d5e`, bytes `55 8b ec 68 86 1d …` — ordinary 16-bit code, not a stub.
USER implements it itself, which is why a whole run never produced one as a BOP.

---

## Part 3 — File > Open, and the `DialogBox` plan that was wrong

The plan was to implement `DialogBox`. **The binaries refuted it before a line was
written**: `USER.87 DIALOGBOX` → entry-table seg 1 offset `0x208e`, bytes
`55 8b ec 68 b1 20 …` — 16-bit code. Same for `CreateDialog` (`0x1ff0`),
`EndDialog` (`0x2120`), `DialogBoxParam` (`0x20fd`). **USER owns the dialog engine
and its modal loop.** And Notepad never reaches it: `notepad seg1:0x0192
COMMDLG.1 GETOPENFILENAME`.

⇒ a **fifth id space**, `src/wow/wowcommdlg.h`. Driving Alt-F-O on the live guest
produced exactly two unimplemented BOPs from a table never seen — `id 0x01, 4
args, retstub 0x0012` and `id 0x1a, 0 args, retstub 0x0090`. COMMDLG's stubs sit
at `seg1:0x0005` and `seg1:0x0083`; a stub is 13 bytes; `+13` gives both to the
byte. The ids are the export ordinals, agreeing seven times.

### ★★ The Win16 `OPENFILENAME` is `0x48` bytes and the GUEST says so

Not from a header. `notepad seg2:0x055d mov word [0x0b16], 0x0048` declares its own
size, and six field stores each land on a boundary of that layout:

```
seg1:0x015a -> +0x08 lpstrFilter        seg1:0x0150 -> +0x2c lpstrTitle
seg1:0x0164 -> +0x0c lpstrCustomFilter  seg1:0x017b -> +0x30 Flags = 0x1004
seg1:0x0146 -> +0x28 lpstrInitialDir    seg1:0x016e -> +0x38 lpstrDefExt
```

⚠⚠ **`hwndOwner` and `hInstance` are 2 bytes here and 4 in Win32**, so every field
after `+0x08` moves. Converted field by field; there is no memcpy that could be
right.

### ★★★★ And then it failed, and the guest said why

The dialog returned the path, `GetOpenFileName` → 1, and nothing happened. With
`MessageBox` in: *"Cannot open the C:\Documents and Settings\… file."*

**The XP dialog returns a LONG path and a Win16 caller cannot parse it.**
`KERNEL.74 OpenFile`'s parser rejects components like `Documents and Settings` —
the open fails **inside krnl386**, before our DOS layer (which handles long paths
perfectly well) is ever asked. ⇒ `GetOpenFileName` converts its answer with
**`GetShortPathNameA`**:

```
-> SHORTENED for a Win16 caller: "C:\DOCUME~1\Matthew\MYDOCU~1\test.txt"
```

A conversion, not a different answer — and what a 16-bit program on a real XP box
gets, for exactly this reason.

⚠ **REFUTED**: forcing `OFN_NOCHANGEDIR` was NOT the cause. It is kept anyway —
comdlg32 moves the *Win32* CWD, the guest's DOS-side CWD is a different object,
and the run showed them desynchronising.

---

## Part 4 — File > Save, and a latent bug worth more than the feature

Save wrote the right LENGTH and the wrong BYTES: the text as it was when the file
loaded, plus five bytes of heap litter.

★★ **Notepad's save path never asks for the handle again.** It is
`WM_GETTEXTLENGTH`, then `_lwrite` of that many bytes **out of the local-heap block
it handed the control at LOAD time** with `EM_SETHANDLE`. `EM_GETHANDLE` is called
ONCE, at load, when the control is still empty. But the control is a real Win32
`EDIT` and every keystroke since went into the OS.

⇒ the block is refreshed at `WM_GETTEXTLENGTH` — the last moment the guest touches
the control before writing. The length goes back first (the return hole is written
before the callback is entered) and the refresh runs behind it with `RET_KEEP`.
★ Safe while Notepad still holds the handle because **a Win16 LOCAL handle is
stable across `LocalReAlloc`**: the memory moves, the handle does not.

A three-call chain into the guest's own KERNEL, because only the guest's KERNEL
can touch the guest's heap — the exact mirror of the load path:

```
LocalReAlloc -> ACT_EDITLOCK: LocalLock -> ACT_EDITFILL: write, LocalUnlock
```

### ⚠⚠⚠ THE DANGLING FRAME POINTER — remember this shape

`wowcall_leave` **pops** the frame and returns a pointer to the slot it just
vacated. An action that issues a follow-up call pushes a new frame into **that same
slot**, so arming the next step through `g_wc[g_wc_depth-1]` also rewrote
`fr->action` underneath us — and the very next `if` in the return handler fired
immediately, with `res` still holding the allocator's handle instead of the lock's
offset. The block was "filled" at offset `0x2e72` (a handle, not an address) and
left in a state `LocalReAlloc` then refused, which Notepad reported as **"too large
for Notepad" about a 59-byte file**.

⇒ **copy `action`/`actarg` out of the frame before dispatching on them.** Nothing
else chained calls from an action, which is why it had never bitten.

---

## Part 5 — menus, and the message that was never forwarded

Opening the Edit menu now produces `GetMenu 0x0140 -> token 0x4000` and six
`GetSubMenu 0x4000 pos 0x0001` — Notepad walking into the Edit popup to grey and
check its six items.

★★ **An `HMENU` must be a TOKEN, not a truncation.** 32 bits into 16, and the
guest hands it straight back to `EnableMenuItem`, which is 16-bit code inside
USER — the value must survive a round trip and still name the right menu.

★★★ **The cluster looked unused because we were dropping `WM_INITMENUPOPUP`.** An
application greys its items when the OS says a menu is about to be shown. Found by
driving Alt+E on the live guest and watching nothing happen. `wParam` is an HMENU
and is converted to a token on the way across.

And with the popup message arriving, the run named two more ids the tool cannot
see: **`0x9a CHECKMENUITEM` and `0x9b ENABLEMENUITEM`**, six calls in one menu
opening, every one stepped over. *A menu that renders is not a menu that is
right*, and nothing in the run said otherwise — no error, no unimplemented import,
just every item enabled.

---

## Part 6 — GDI's id space opens

A **seventh** dispatcher (`src/wow/wowgdi.h`), and the first opened for its own
sake rather than because a guest stopped on it: Notepad reaches 3 of GDI's thunks,
**Paint reaches 41**. GDI's exports tail-jump like USER's, so the anchors are the
stubs one hop past the entry points:

```
id 0x044  2 args  retstub 0x033a  DELETEDC       (stub seg1:0x032d)
id 0x045  2 args  retstub 0x0354  DELETEOBJECT   (stub seg1:0x0347)
id 0x050  4 args  retstub 0x05de  GETDEVICECAPS  (stub seg1:0x05d1)
```

With a **GDI object token map**, same reason menus needed one. ⚠ Deleting forgets
the token as well as the object: a freed Win32 handle is reusable, so a stale
token would eventually name someone else's — the hazard `DestroyWindow` has and
the same answer.

⚠⚠ **The honest gap, stated in the file**: the calls that PRODUCE a DC or object
are not serviced yet (`CreateDC`, `GetStockObject` are native16 wrappers), so a
handle arriving at these three will usually be one we never issued. Reported as
exactly that, not guessed at.

---

## Part 7 — the harness, and three traps

### ⚠⚠⚠ XP's WOW VDM IS SHARED — a stale `ntvdm.exe` steals every launch

If an `ntvdm.exe` is already running, Windows hands a new 16-bit launch to **that**
VDM instead of creating a fresh one — so **the IFEO Debugger hook never fires**,
our host never starts, and the app comes up under stock. **Independent of the key
being correct.** This is the second cause of *"it's only running stock NTVDM"* and
it was silently stealing relaunches after every compare run.

### ⚠⚠ The freeze watchdog was killing every idle Win16 guest at 150 s

The DPMI watchdog calls a run wedged when `g_dpmi_iter` stops advancing — 600
samples on a WOW run. That bound assumed `wowrun.bat` ended every run at 75 s, and
session 43's `wowidle.txt`=0 broke the premise without telling the watchdog. **A
guest left on the desktop for a human was `TerminateProcess`'d 150 seconds after it
went idle**, and the only note went to `wdprobe.log` — the main log just stopped
mid-heartbeat. *Found by the user, who looked at the box and said "it's only
running stock NTVDM".*

A parked guest genuinely looks frozen: it sits at one EIP inside our own blocking
`GetMessage`, which is what a Win16 task waiting for input IS. The watchdog cannot
tell that from a wedge by sampling — and does not have to, because the host put it
there and can say so (`g_wm_inwait`).

### New rig tools

* **`scripts/bm/wowkeys.bat`** — bring a caption forward, send VKs, screenshot.
  ★ Keys and not `PostMessage(WM_COMMAND)`: posting would test our translation
  while SKIPPING the thing that produces a WM_COMMAND — the OS's own menu.
* **`scripts/bm/wowcompare.bat`** — ours and STOCK ntvdm running the same guest on
  the same desktop. IFEO removed for exactly one `start`, restored unconditionally
  and read back.
* ⚠ **`exec cmd /c` needs an EXTRA enclosing pair of quotes once there are
  arguments** — `cmd /c` strips the first and last quote, so the batch never runs
  and reports nothing. `exec cmd /c ""C:\...\x.bat" "arg" 0x12"`.
* ⚠ **`rigshot` logs to `<share>\rigshot.txt`, not stdout** (GUI subsystem).
* ⚠ **A `controld exec` opens a visible `cmd.exe` that STEALS THE FOREGROUND**, so
  `rigshot key` sent as separate execs lands in that console. Put the whole
  `fg` + keys sequence in ONE batch file.

---

## Measured

| | result |
|---|---|
| **baseline** (switches off) | **`270 / 64 / 122 / 78 · 9·222·39 · 0001:229C`** |
| **Notepad** | opens a file via the real XP dialog, edits, **saves the typed text**; Help > About; menus grey/check; own icon, caption, message loop |
| **NOTEPAD.EXE surface** | 118 imports · 34 reach us · **32 serviced** · 2 to do |
| **PBRUSH.EXE GDI** | 41 reach us · 3 serviced · 38 to do |

⚠ **The baseline moved, and every call is accounted for.** `270/64/122/78` against
session 43's `270/46/122/96`: the total and the task split are unchanged, and the
18 that moved from UNIMPLEMENTED to SERVICED are named by the log — **9
`OemToAnsi` and 9 `AnsiToOem`**, conversions krnl386's own bootstrap was already
making while we answered with the harness sentinel.

---

## ▶ RESUME HERE

### 1. ★★★ THE NEXT THING IS ARCHITECTURAL: SENT vs POSTED MESSAGES

Win32 **sends** `WM_INITMENUPOPUP`, `WM_PAINT`, `WM_GETTEXTLENGTH` and expects the
window procedure to **return a value before it proceeds**. This host can only
**post** — the guest picks the message up later from its own `GetMessage`.

⚠ **MEASURED CONSEQUENCE, NOT A THEORY**: the menu-state calls are implemented and
correct, and they do not take effect while the menu is on screen, because Win32
runs the menu's modal loop **NESTED ON THE EXEC THREAD** and the guest cannot run
until that loop returns.

⚠⚠ **AND IT BLOCKS PAINT.** `WM_PAINT` is a sent message. GDI cannot work until a
guest can answer one synchronously, so this is on the critical path to the north
star's other half — implementing more GDI calls first will hit this wall the
moment Paint tries to paint.

**Why it is hard, stated so the next attempt does not rediscover it:** running
guest code means RETURNING to the VDM loop; `wowcall_enter` parks a context and
the guest runs when we return. To answer inside `wowwin_proc` we would have to
re-enter the VDM execution loop recursively from inside a Win32 window procedure
that is itself inside a BOP handler inside that loop. ⚠ Whether a nested
`VdmStartExecution` is viable is **UNTESTED** — that is the experiment.

⚠ **Moving windows to their own thread does NOT solve it and must not be tried
blind**: a Win32 window belongs to its creating thread, and the guest calls
`BeginPaint` from the exec thread, so the HWNDs have to stay there. That is why
the design is as it is (session 42).

### 2. If pixels sooner are preferred: Paint's 38 GDI calls

`tools/ne/neneeds.py guest/win16/PBRUSH.EXE --todo` prints them, named. The
producers matter first — `CreateDC`, `CreateCompatibleDC`, `CreateCompatibleBitmap`,
`SelectObject`, `GetStockObject` — because the three delete/query calls already in
place can only report "not one of our GDI tokens" until something issues one.
⚠ `CreateDC` and `GetStockObject` are **native16 wrappers**, so their internal ids
come from a run, not from `neneeds.py`.

### 3. Notepad's last two

`COMMDLG.11 FINDTEXT` (id `0x0b`) and `COMMDLG.20 PRINTDLG` (id `0x14`), 4 args
each — one far pointer to a struct. Both need their structure read out of the
guest the way `OPENFILENAME` was. Neither is on the path to a usable editor.

### 4. Not verified, and not claimed

* **`EndDialog`** is implemented and **no run has opened one of Notepad's own
  dialogs**. It calls the real Win32 one and reports its refusal rather than hiding
  it — USER's `DialogBox` is 16-bit and runs its own modal loop, so our "dialog" is
  a plain window Win32 has nothing to end. The first run that opens a Find dialog
  will say how that loop learns it is over.
* **The dialog-item helpers** (`GetDlgItem`, `GetDlgItemText`, `SetDlgItemInt`,
  `SendDlgItemMessage`) are in and **unexercised** for the same reason.
* **`EnableMenuItem`/`CheckMenuItem`** are implemented but have **not been observed
  firing since**, for the sent-message reason above.

### How to drive it

```bash
touch /private/tmp/xpshare/wowsched.txt /private/tmp/xpshare/wowcall.txt
ARCHIVE=build/wowruns ./scripts/bmwow.sh          # SYSEDIT baseline, measured
python3 tools/ne/neneeds.py guest/ne/notepad.exe --todo    # what is left, named
python3 tools/ne/neneeds.py guest/win16/*.EXE --todo       # the whole shelf
```

Leave a guest running and drive it:

```bash
SH=/private/tmp/xpshare
printf 'exec cmd /c "C:\\...\\ntvdmex\\wowlive.bat"\r\n' > $SH/control.txt
B='C:\Documents and Settings\All Users\Documents\ntvdmex\wowkeys.bat'
printf 'exec cmd /c ""%s" "Notepad - (Untitled)" 0x12 0x48 0x41"\r\n' "$B" > $SH/control.txt
```

⚠ Always re-run with the switches OFF and confirm **`270 / 64 / 122 / 78 ·
9·222·39`**.

### Ruled out — do not re-try

Everything in [session 43's list](session-43.md#ruled-out--do-not-re-try) still
holds, plus:

* **Implementing `DialogBox`.** USER owns the dialog engine; its exports are
  16-bit code. Notepad reaches COMMDLG directly.
* **`DefWindowProc` as a service.** `USER.107` is 16-bit code.
* **`OFN_NOCHANGEDIR` as the cause of File > Open failing.** It was the long path.
* **Trusting `neneeds.py` alone.** `native16` does not mean free — four wrappers
  this session reached stubs it cannot see.
* **`tasklist` as an "is it ours" check.** It prints "INFO: No tasks running" and
  exits zero; use `tasklist | find "ntvdmhost"`.
