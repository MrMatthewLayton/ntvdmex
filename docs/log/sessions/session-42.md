# Session 42 — SYSEDIT.EXE on the Windows XP desktop

- **Branch:** `m9/completeness`
- **Issue:** [#128](https://github.com/MrMatthewLayton/ntvdmex/issues/128)
- **Predecessor:** [session 41](session-41.md) — the message loop turned on a real keystroke,
  and SYSEDIT read all four of its files. Nothing had drawn a pixel.

---

## ★★★★★ THE HEADLINE

**`SYSEDIT.EXE` is on the Windows XP desktop, as itself.**

A frame window titled **"System Configuration Editor"**, four cascaded MDI children
titled `C:\WINDOWS\SYSTEM.INI`, `C:\WINDOWS\WIN.INI`, `C:\CONFIG.SYS` and
`C:\AUTOEXEC.BAT`, each with a real `EDIT` control holding the file's text, a taskbar
button, and **no VDM window anywhere** — an NTVDMEX icon in the system tray instead.

Every window in that sentence is a **real Win32 `HWND`**.

---

## ⚠ Part 0 — the wrong answer, and why it was wrong

The first attempt at pixels drew a Windows 3.x desktop — teal background, grey frames,
navy caption bars, 8x16 ROM-font titles — into the VGA framebuffer, so the Win16 window
tree appeared **inside the NTVDMEX window**. It worked: the client area went from black
to teal. The user stopped the session:

> "Are you trying to draw WOW16 apps *inside* NTVDMEX? That's what it looks like, and
> that's wrong! WOW16 apps should NOT draw inside the NTVDMEX window. They should draw
> to the Windows XP desktop, which is what Windows XP already does."

That is exactly right and it is what WOW **is**. `wow32.dll` gives every Win16 window a
real Win32 `HWND` — which is why a 16-bit program on XP gets a taskbar button, a real
title bar, real focus, real clipping against other applications, and the OS's own input
and painting. A VDM *console* window is what a DOS session gets; a Win16 program is not
a DOS session. And reimplementing a window manager while running on one is both more
code and less faithful.

★ **The smell to recognise:** if the answer involves inventing a desktop, a caption bar,
a border style or a font for chrome, stop. That is the DOSBox-shaped answer, and this
project is explicitly not DOSBox.

`wowdraw.h` was deleted. Everything below is the right shape.

---

## Part 1 — a Win16 window is a Win32 window

`src/wow/wowwin.h`.

| Win16 | maps to |
|---|---|
| `RegisterClass` | a real `RegisterClassA` whose `lpfnWndProc` is **ours** |
| `CreateWindow` | a real `CreateWindowExA`; the `HWND` lives in `wowuser_win_t` |
| `ShowWindow` / `UpdateWindow` | the OS's |
| `MDICLIENT`, `EDIT` | the **OS's own classes** — they exist, so use them |
| input | the real window's own Win32 messages, translated into the Win16 queue |

⚠ **`CW_USEDEFAULT` MUST BE TRANSLATED, NOT PASSED.** Win16's is `0x8000` and Win32's is
`0x80000000`. Both constants now live beside the one function that converts them. The
`WS_*` style bits, by contrast, **are** the same values in both and go straight across.

⚠ **`MDICLIENT` requires a `CLIENTCREATESTRUCT`** and fails without one. Not a
workaround — the documented contract of the class we chose to use rather than
reimplement.

⚠ **Win32's `WM_CREATE` and Win16's are two different messages.** Win32 sends one to
`wowwin_proc` during `CreateWindowEx`, about the real window; the guest's own, about its
object, is delivered afterwards through session 40's callback machinery. Conflating them
would re-enter the guest from inside `CreateWindowEx`.

### ⚠⚠ Threading is the design, not a detail

A Win32 window belongs to the thread that created it: its window procedure runs on that
thread, and `BeginPaint` and every GDI call against its DC must be made from it. Guest
code runs on the **exec thread**, so that is where the `HWND`s are created — and that
thread's Win32 queue is pumped:

- bounded, at every WOW32 BOP, so the window stays alive while the guest works;
- **blocking inside Win16 `GetMessage`** (`MsgWaitForMultipleObjects`), which is exactly
  where a Win16 task is supposed to wait.

⚠ A guest in a long BOP-free stretch will make its window unresponsive. Real WOW gives
each Win16 task its own Win32 thread; we have one exec thread and a cooperative
scheduler. Stated, not discovered later.

⚠ Session 41's Win16 keyboard tap on `host_key_scancode` is **removed**. It was right
about the DOS path and wrong about this one: keys now arrive as real Win32 messages
addressed to the real window, with the OS's own focus deciding which window gets them.
Feeding the Win16 queue from the 8042 as well would deliver every key twice, to a window
the OS had not focused.

### ★ The cascade is the MDI client's, and finding that out is why the commit is shaped as it is

The first cut created the MDI children with a plain `CreateWindowEx`. The run said what
was wrong with that: SYSEDIT passes **`MCS_STYLE == 0`**, and 0 is not *"no style"*, it
is *"give me the MDI defaults"* — which only an MDI client can supply. `CreateWindowEx`
read it as four borderless children stacked at (0,0) filling the client, which is exactly
what the desktop showed.

Forwarding `WM_MDICREATE` to the **real** client instead gets the default child style,
the cascade, a caption, a system menu and a place in the window list:

```
@0000,0000  ->  @0016,001d  ->  @002c,003a  ->  @0042,0057      (measured)
```

Same argument as using the real `EDIT` class rather than drawing a text box.

---

## Part 2 — the text in the controls

The controls were empty. The text is in the **application's own local heap** — the block
this host asked the guest's KERNEL to allocate in session 40 — which the 32-bit side
cannot address. Real WOW has the same problem and solves it the same way: read the text
out and give it to the real control.

Reading it means locking it, and only the guest's KERNEL can. ★ **The ordinals are named
by krnl386's own non-resident name table**, so nothing here is a list from memory:

```
5 LOCALALLOC   0x3ddb      7 LOCALFREE    0x3df7      9 LOCALUNLOCK 0x3e55
6 LOCALREALLOC 0x3e1f      8 LOCALLOCK    0x3e0b
```

and both of the two used disassemble to `mov bx,[bp+6] … retf 2` — one WORD argument,
far, which is what `LocalLock(HLOCAL)` takes.

★ **A sink was not enough.** A sink keeps a *value*; `LocalLock` returns a **pointer the
host then has to follow**, and following it is work that can only happen after the guest
returns. So `wowcall.h` frames carry an **action** now, named by the call that asked for
it; the BOP handler does the read, the `SetWindowText` and the matching `LocalUnlock` —
we took the lock, so we owe the release, and leaving a moveable block permanently locked
would quietly pin the application's heap.

Measured: exactly four actions, one per control, with the right sizes — `0xe7` for
SYSTEM.INI, `0x1dd` for WIN.INI, and 0 for the two files that really are empty.

### ⚠ And one defect worth the note it gets

The first run ran the EDIT-text action on a `WM_CREATE` callback **and** on a
`LocalAlloc` callback as well — following a pointer that was never a pointer and issuing
a `LocalUnlock` against a lock nobody had taken. `wow32_frame_t` is a stack local and
`cbact` had been left un-set, so **an uninitialised byte was being read as a decision**.
Every field of that frame is initialised at the BOP now, and the comment says why.

---

## Part 3 — no VDM window, and a tray icon

On real XP a Win16 program shows no VDM window. Ours sat on top of the guest's own
windows showing a **black text screen** — not merely redundant but misleading, because
there is nothing for a Win16 guest to draw there: krnl386 never sets a video mode.

So for a `-w` launch the window is created and never shown, and the menu behind it goes
to the **system tray** — Settings, Screenshot, About, Close Program, Exit, and "Show
NTVDMEX Window". Right-click for the menu, double-click to bring the window back.

⚠ The window is still **created**: it owns the present surface, the raw-input
registration, the frame timer and the tray callbacks. Hidden is a state; absent would be
a second code path through everything the UI thread does.

⚠ The tray menu is deliberately short — mirroring the whole menu bar would offer
Fullscreen and Capture Input for a machine with no screen and no focus.

► A DOS guest keeps its window and gets no icon. Whether it should have one too is an
open question (the user's, and a fair one); the only thing that would change is the
condition on `g_wow_launch`, which is why it is one flag.

---

## Measured

| | result |
|---|---|
| **frontier** (both switches on) | `613 / 141 / 308 / 147`, 8 windows + 4 MDI children **all with real HWNDs** (zero "NO REAL WINDOW"), 4 EDIT texts, **0 message boxes**, ends `ExitKernelThunk(0)` |
| **baseline** (both switches off) | `270 / 45 / 122 / 97 · 9·222·39 · 0001:229C` — unchanged |
| **Doom** | all eleven startup stages `V_Init → ST_Init`, 3.51 MB |

New tooling: `tools/bmp2png.py` (rigshot's BMPs are the only eyes this project has on the
rig, and `sips` refuses them; `--crop`, `--scale`, `--zoom`), and `scripts/bm/wowrun.bat`
now takes two desktop screenshots — one mid-run and one at the end, because "nothing was
drawn" and "it was drawn and then the window closed" are different facts.

---

## ▶ RESUME HERE

### 1. THE FRONTIER: GDI, AND A GUEST THAT PAINTS

SYSEDIT is on the desktop because almost nothing it shows is its own drawing: the frame,
the MDI chrome and the `EDIT` controls are all the OS's, which under WOW means ours to
delegate. **MS Paint is not like that.** It paints its own client area, and that needs:

1. **`WM_PAINT` translated** in `wowwin_proc` and posted to the Win16 queue, instead of
   being left to `DefWindowProc`. ⚠ `DefWindowProc` currently validates the region, so
   there is no repaint storm; the moment `WM_PAINT` is forwarded, the guest MUST reach
   `BeginPaint`/`EndPaint` or Win32 will regenerate it forever.
2. **`BeginPaint` / `EndPaint`** (USER `0x27` / `0x28`) against the real window — the
   `HWND` is owned by the exec thread precisely so this is legal.
3. **GDI's id space**, which this host does not dispatch at all: 367 stubs, 365 distinct
   ids, mapped in `docs/research/wow-user-surface.md`'s sibling table. Every one of them
   has a real Win32 counterpart operating on the HDC `BeginPaint` returned.

### 2. The nearer, smaller pieces

- **More messages into the queue.** `wowwin_proc` translates keyboard and `WM_CLOSE`
  only. `WM_SIZE` is the next one that matters: SYSEDIT's `mpchild` handles it and would
  then size its own EDIT control, which is currently done by our CW_USEDEFAULT default.
- **Input has not been tested end to end since the source changed.** Session 41's proof
  used scripted scancodes through the DOS path, which is gone; keys now come from the
  real window and nothing on the rig types into it. A `rigshot`-style "send keys to the
  foreground window" verb would close that.
- **`DefFrameProc` / `DefMDIChildProc` / `DefWindowProc` as Win16 services** (`0x1bd`,
  `0x1bf`). SYSEDIT passes everything it does not handle to `DefFrameProc`, and the
  answer is now easy: call the real one on the real window.
- **`SetWindowText`, `DestroyWindow`, `MoveWindow`, `GetClientRect`** — all one-liners
  now that there is a real window to put them to.

### 3. Closed this session

- **Nothing has drawn a pixel.** SYSEDIT is on the desktop.
- **A window is an object with no pixels behind it.** It is a real `HWND`.
- **`MDICLIENT` and `EDIT` are ours to implement.** They are the OS's, and we use them.
- **The EDIT controls are empty.** They hold the files' text.
- **The host window shows a black screen during a Win16 run.** There is no host window.

### How to drive it

Unchanged from session 41 except that `qimode.txt`/`keys.txt` no longer feed Win16:

```bash
touch /private/tmp/xpshare/wowsched.txt /private/tmp/xpshare/wowcall.txt
ARCHIVE=build/wowruns ./scripts/bmwow.sh
python3 tools/bmp2png.py /private/tmp/xpshare/wow_shot1.bmp out.png --scale 2
```

- ★ `wow_shot1.bmp` (mid-run) and `wow_shot2.bmp` (end) are written by `wowrun.bat`.
  ⚠ `rigshot list` still writes an empty file — it is a GUI image, so its stdout does not
  redirect. **The screenshot is the evidence, not the window list.**
- ⚠ Always re-run with the switches OFF and confirm `270 / 45 / 122 / 97 · 9·222·39`.

### Ruled out — do not re-try

Everything in [session 41's list](session-41.md#ruled-out--do-not-re-try) still holds, plus:

- **Drawing Win16 windows inside the NTVDMEX window.** The whole of Part 0.
- **Registering our own `MDICLIENT` or `EDIT` class.** They exist; cloning them against
  our own window procedure is the same mistake one level down.
- **Creating MDI children with `CreateWindowEx`.** `MCS_STYLE == 0` means "MDI defaults",
  and only the MDI client has them.
- **Passing `CW_USEDEFAULT` straight through.** Different values in Win16 and Win32.
- **Feeding the Win16 queue from `host_key_scancode`.** That is the DOS 8042 path; a real
  window gets real Win32 keyboard messages.
