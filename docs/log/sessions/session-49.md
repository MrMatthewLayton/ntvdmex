# Session 49 — MS Paint saves a file

> Branch `m9/completeness`. Follows [session 48](session-48.md).

## ★★★★★ THE HEADLINE

**`File > Save As` works.** MS Paint writes a valid 24-bit `.BMP` to the directory
the user chose and stays running:

```
C:\Documents and Settings\Matthew\Desktop\TEST.BMP     4,909,014 bytes
  magic "BM"  filesize 4909014  bits@54
  BITMAPINFOHEADER 40  1680 x 974  planes 1  24 bpp  uncompressed
```

Zero GP faults; `ntvdmhost.exe` still up afterwards with its window on screen.

★★★★★ **AND THE USER CONFIRMED IT BY HAND** — *"Save As worked in Paint this
time"* — which is the standard this project holds itself to and the same one that
closed Doom's sound and Doom's mouse. The harness measurement and the human agree.

Two bugs stood between session 48 and that, and both were the same shape:
**a stepped-over call whose sentinel answer means "yes".**

---

## ★★★★★ 1. OLESVR DEREFERENCED A NULL IT WAS HANDED — `GetParent`

Session 48 left the save dying in `OLESVR.DLL at 0003:1548`. The fault frame is
the whole diagnosis:

```
bytes@fault = 26 83 7f 0e 00      cmp word ptr es:[bx+0x0e], 0
fault regs  = ... ebx=0x0000  es=0x0000{NO DESCRIPTOR}
```

A null far pointer, dereferenced unchecked. Three log lines before it say where
the null came from:

```
FUNC=0x2e from=OLESVR seg3:0x1528 (0x0200) -> UNIMPLEMENTED, ANSWERED 0
FUNC=0x35 ...                              -> DestroyWindow 0x0200 -> destroyed
FUNC=0x87 from=OLESVR seg3:0x153e (0,0)    -> GetWindowLong(0x0000,0)
                                              ★ NOT ONE OF OUR WINDOWS; 0
```

`USER.46 GetParent` was stepped over, so OLESVR asked **window 0** for its window
long and dereferenced the zero it got back.

★ **And the right answer was knowable before the run.** Window `0x0200` is
`CreateWindow("DocWndClass","Doc", style=0x40000000)` — `WS_CHILD` — and its
argument block carries `hwndParent = 0x0140`; thirty lines earlier the log has
`SetWindowLong(0x0140, 0000, 0x09b70000)`, OLESVR storing its own server object
on exactly that window. So `GetParent(0x200) -> 0x140` hands back the pointer
OLESVR itself put there.

⚠ **AND I WAS DISASSEMBLING THE WRONG BINARY.** `guest/win16/OLESVR.DLL` is the
Windows 3.11 copy; the guest loads **XP's**, out of `system32`. Same size,
different md5, different code at `seg3:0x1548`. Modules that exist in both places
— OLESVR, OLECLI, SHELL, MMSYSTEM, COMMDLG, VER — must be analysed from the
rig's copy, and those now live in `guest/wow/`.

Implemented with the rest of the cluster OLESVR reaches: `GetParent`,
`GetWindow`, `GetClassName`, `GetWindowTask`, `GetProp`/`SetProp`/`RemoveProp`,
`GlobalFindAtom`, `GlobalGetAtomName`.
⚠ `GetClassName` must return the **Win16** name: every Win32 class this host
registers is prefixed (`NTVDMEX16.pbParent`), and OLE looks its own server window
up by the name it registered.
⚠ A property name may be an **atom** — a far pointer with a null selector and the
atom in the offset — which `wow32_argstr` correctly refuses to read. Handling
only strings would store nothing, find nothing, and hand back the same null.

---

## ★★★★★ 2. THE FILE WENT TO THE WRONG DIRECTORY — `SetCurrentDirectory`

With OLESVR fixed the crash was gone and the log showed a complete, correct .BMP
being written — six `AH=40h` writes of `0xF000`/`0xD7A0` bytes, a seek to end
reporting `0x004AE7D6`, a seek back to 0, then 14 and 40 bytes (the two headers)
and a close. **It was just in the wrong place**: `C:\Documents and
Settings\Matthew\TEST.BMP`, one level above the Desktop the user chose.

```
FUNC=0x82 from=seg1:0x53c0  arg = "C:\WINDOWS"
FUNC=0x82 from=seg1:0x53c0  arg = "C:\DOCUME~1\Matthew\Desktop"
   -> UNIMPLEMENTED, STEPPED OVER ... ANSWERED 0
```

⚠⚠ **The sentinel is "success" at this call site, which is why it was silent.**
`inc dx / je <error>` — our 0 makes DX 1, so krnl386 takes the *success* arm and
tells the application the directory changed. It never did, so the create resolved
against the old current directory. **A stepped-over call that answers "yes" is
worse than one that answers "no."**

⚠ It is **not declinable** and this repo already said so before the bug:
`wowdecline.py` lists `0x82` among three sites where `0xFFFF` is a plain error
krnl386 reports to the app rather than chaining to DOS. Session 47 tried
declining anyway and only moved the fault. So it is *performed*, against the
host's own current directory — which is where our DOS layer already resolves
relative paths (`SetCurrentDirectoryA(g_cur)`).

---

## ▶ RESUME HERE

### What is proven

* Save As: no crash, correct file, correct directory, Paint survives. Verified by
  reading the file's headers off the rig, not from a log line.
* Drawing still works (three strokes on the canvas, screenshotted).

### ⚠ What is NOT proven, and a NEW observation

**A save of a drawing was not demonstrated.** Not because the save failed — but
because **after mouse drags, the Alt-key menu route stops opening the menu.** On a
freshly launched Paint, `Alt`,`F`,`A` opens Save As reliably (that is how the
verified save was driven); after three `rigshot drag`s on the canvas, the same
sequence does nothing and `rigshot fg "Save As"` reports NOT FOUND. Escape first
does not help.

★ **This is probably the user's original "menu clicks crash the app" report seen
from another angle** — the same area, and still not a crash but a
non-response. ▶ First hypotheses to test, in order: mouse **capture** left set
after a drag (`SetCapture`/`ReleaseCapture` are serviced — check they pair);
focus parked on the `pbPaint` child so `WM_SYSKEYDOWN` never reaches the frame;
`ClipCursor` accepted-but-not-applied leaving the guest believing it still owns
the pointer.

### Then broaden — unchanged from session 48

Solitaire **9** services, Minesweeper **15**, Media Player **20** (and **0** from
MMSYSTEM — its imports are all 16-bit code inside MMSYSTEM.DLL). ~35 distinct
services cover six programs. ⚠ `SetTimer` is the one that is not a pass-through.

### Ruled out — do not re-try

* Everything in [session 48's list](session-48.md#ruled-out--do-not-re-try).
* **Declining krnl386 `0x82`.** Twice now. It is an error at that site, not a chain.
* **Analysing OLESVR/OLECLI/SHELL/MMSYSTEM/COMMDLG from `guest/win16/`.** The
  guest loads XP's copies from `system32`; use `guest/wow/`.
