# Session 45 — MS Paint runs, has its menu, and paints

> Eleven commits, `7830d92` … `3a6035c`. Branch `m9/completeness`.

## ★★★★★ THE HEADLINE

**BOTH NORTH-STAR PROGRAMS NOW RUN.** `PBRUSH.EXE` from Windows 3.11 is a real,
sized, titled Win32 window on the Windows XP desktop — **"Paintbrush - (Untitled)"**,
its own icon, working caption buttons, its own taskbar button — with its **real
menu bar** (File · Edit · View · Text · Pick · Options · Help, 62 items from its
own resource) which **opens on Alt-F**, and it **paints**: it answers its own
`WM_PAINT` with `MoveTo`/`LineTo`/`PatBlt` and the pixels are on the screen.

It registers itself as an OLE server, reads WIN.INI, creates its frame, canvas,
toolbox, line-size and colour windows, takes and releases real device contexts,
loads its toolbox bitmaps, and its canvas has working scrollbars.

Evidence:
[`session45-pbrush-window.png`](../../research/evidence/session45-pbrush-window.png),
[`-menubar`](../../research/evidence/session45-pbrush-menubar.png),
[`-file-menu`](../../research/evidence/session45-pbrush-file-menu.png),
[`-painting`](../../research/evidence/session45-pbrush-painting.png),
[`-vs-stock`](../../research/evidence/session45-pbrush-vs-stock.png),
[`-after-getclientrect`](../../research/evidence/session45-pbrush-after-getclientrect.png).

★★ **And after a comparison against stock ntvdm, its whole UI is right**: the
toolbox with its colour tool icons, the line-size box, the colour palette bar and
the canvas with its scrollbars are all present and **geometrically
pixel-identical to the oracle**. See
[`-toolbox-icons`](../../research/evidence/session45-pbrush-toolbox-icons.png) and
[`-layout-matches-stock`](../../research/evidence/session45-pbrush-layout-matches-stock.png).

⚠ **Two content differences remain** — the palette swatches render dithered
black-and-white rather than in colour, and the line-size box shows its arrow but
not its bars. See the resume block.

---

## ★★★ SIX WALLS FELL, AND THE PLAN ON RECORD PREDICTED NONE OF THEM

Session 44's handoff said Paint's next steps were either its ~38 remaining GDI
calls or the architectural sent-vs-posted message split. **Neither was the
blocker.** One run — with `MessageBox` already implemented, so the guest could
talk — had Paint name its own walls in English:

| # | What Paint said / did | What it actually was |
|---|---|---|
| 1 | *"Failed to register server."* | SHELL's registration database (`Reg*`) — **and the anchor** |
| 2 | *"Not enough memory to perform this operation."* | `GetDC` answered 0 |
| 3 | GP fault in `PBRUSH.EXE 0003:074A` | `WM_CREATE` with a null `CREATESTRUCT` |
| 4 | *"Not enough memory to edit image."* | `LoadBitmap`'s 32-bit half (USER `0xaf`) |
| 5 | GP fault in `KRNL386.EXE 0001:355A` | `GetObject` answered 0 |
| 6 | window laid out for 1680×974 in a 1252×688 client | `GetClientRect` stepped over |

This is the third session running in which the **standing method note** —
*implement `MessageBox` first on any new guest* — was worth more than any
instrument. It turned five of those six into a sentence on screen.

---

## ★★★★★ AN ANCHOR MUST BE THE WHOLE STUB TABLE

`wow_shell_anchor()` recognised SHELL's code segment from **`ShellAbout` alone**,
and it was written up as deliberate: *"the table is identified by the first call
that needs it"*. That holds only while every guest that needs SHELL calls
`ShellAbout`. Notepad does. **MS Paint does not** — it calls `RegCreateKey`,
`RegSetValue`, `RegQueryValue` and `DragAcceptFiles`.

So SHELL was **never identified at all**. Every one of those was logged as
`"?'s table -- a DIFFERENT id space"` and answered by nobody — including
`DragAcceptFiles`, **which had been implemented since session 44**.

> ⚠⚠ **In a log, "nobody wrote this service" and "nobody identified this module"
> are the same line.** An implemented service can be unreachable because its
> MODULE was never anchored, and nothing says so.

Fixed by generating the anchor from the binary:
`tools/ne/wowthunks.py --anchor <module>` emits every `(id, argbytes, retstub)`
triple in the module into [`src/wow/wowanchors.h`](../../../src/wow/wowanchors.h)
— SHELL 34 rows, GDI 367. Matching any row is still safe: a stub is 13 fixed
bytes that push both fields, so a wrong segment would have to hold `push <this
id>` with `push <these argbytes>` at exactly the offset the call returns to.

⚠ Three rows (`{0x2080, 0, …}`, one in GDI and two in USER) are probably ordinary
16-bit code that happens to match the shape. **Left in rather than filtered** —
the filter would be a guess too, and a row only fires on an exact triple.

---

## ★ THE FIRST DEVICE CONTEXT COMES OUT OF **USER**, NOT GDI

`wowgdi.h` said the honest gap was that nothing yet *produced* a DC or an object,
so any handle arriving there would be one this host never issued — and that the
producers would be GDI's `CreateDC`/`GetStockObject`.

The first producer is **`GetDC`, which is USER `0x42`**. The three GDI calls
already written could only ever answer *"not one of our tokens"* until something
in **another id space entirely** made one.

The object map grew a `kind` (it was a boolean `isdc`), because three different
calls dispose of a handle and getting it wrong is silent:

```
OBJ    brush/pen/bitmap/font          -> DeleteObject
DC     CreateDC/CreateCompatibleDC    -> DeleteDC
WINDC  BORROWED from GetDC/BeginPaint -> ReleaseDC, and ONLY ReleaseDC
STOCK  the system's own               -> nothing; success is reported anyway
```

⚠ `DeleteDC` on a borrowed DC does not fail visibly on Win32 — it damages the
window's DC cache and surfaces somewhere else. The map is the only place that can
still tell the difference, so it records it.

⚠ Freed slots **are** reused. 256 slots without reuse are gone in seconds
(Paint takes and releases a DC on every paint) — a certain failure traded for a
possible one, and the possible one is what Win32 does with its own handles.

---

## ★★ WM_PAINT IS RELAYED — AND NOT VALIDATING THE REGION LIVE-LOCKS THE HOST

`wowwin.h` had said since session 42 that `WM_PAINT` was left to `DefWindowProc`
*"the day"* GDI's id space was dispatched. That day was this one.

> ⚠⚠ **Win32 does not queue `WM_PAINT` — it SYNTHESISES one for as long as the
> window has an update region.** Relay it to the guest and return 0 and the pump
> hands you another immediately, forever, and the guest never gets a turn.

So the OS's `BeginPaint`/`EndPaint` runs **in the window procedure** (erasing and
clearing the region), and the rectangle it reports is kept in a per-window
pending-paint record for the guest to collect out of its own message loop.

⚠ **The cost, stated rather than discovered:** the guest's `BeginPaint` gets the
*remembered rectangle*, not a live clipped region. A guest that paints what it is
told will be right; one that relies on the DC being clipped to that region is
relying on something this does not reproduce. A second `WM_PAINT` arriving before
the guest answers the first **unions** with what is pending — replacing would
silently lose area.

Verified on both guest classes ([a fix measured on one guest is a fix for
none](session-32.md)): **Notepad is unchanged** — icon, menu, edit control,
scrollbars — and never calls `BeginPaint` at all, because its EDIT control is a
real Win32 control that paints itself.

---

## ★★ A GAP ONE GUEST NEVER EXERCISES LOOKS LIKE COMPLETENESS

`wowres_find` looked up **integer** resource ids, and its own comment said so.
Notepad's menu is `#0001`, so Notepad got a working menu bar and the limitation
sat there for two sessions looking like a finished feature.

PBRUSH registers `pbParent` with `MENU="PBrush2"`, and its resource table holds
`MENU PBRUSH2` — a **named** resource. The lookup could never match, so Paint's
window came up with **no menu at all, silently**: a class naming a menu the host
cannot find is indistinguishable from a class with no menu.

In an NE resource table a type or id word with the **high bit clear** is a byte
offset from the start of the resource table to a length-prefixed string in the
table's own pool. ⚠ The comparison must be **case-insensitive** — names are
stored upper case (`PBRUSH2`), programs ask in the case they wrote (`PBrush2`).

⚠ My first hypothesis was case sensitivity in an existing lookup. It was wrong —
there was no named lookup at all — and the file said so plainly.

---

## ★★★ STOCK ntvdm ON THE SAME BOX IS THE ORACLE

The user's read of the screen — *"the way it paints is completely wrong"* — was
right, and their instinct to reach for an oracle was right. But the oracle did
**not** need Windows 3.1 installed on the DOS 6.22 QEMU box: **stock `ntvdm` on
the XP rig runs the same `PBRUSH.EXE`**, on the same screen, at the same
resolution, with the same WIN.INI — and `scripts/bm/wowcompare.bat` already
starts both side by side (ours with the IFEO key, stock with it removed for one
`start` and restored immediately, verified by reading it back).

`rigshot` gained a **`tree`** verb so this stopped being pixel-estimation off a
scaled screenshot: it prints **every** top-level window with a given caption —
both Paintbrushes have the same one, so `FindWindow` would have compared a window
with itself — and each one's children with exact rectangles in the parent's
client coordinates.

With both frames at an **identical 1252×688 client**:

| child | stock (oracle) | ours, before | ours, after |
|---|---|---|---|
| `pbPaint` | at(128,2) 1100×604 | at(7,4) **1682×976** | at(3,2) 1251×687 |
| `pbTool` | at(3,2) 121×516 | at(4,2) **163×731** | unchanged |
| `pbSize` | at(3,521) 121×164 | at(4,736) **163×235** | unchanged |
| `pbColor` | at(128,608) 1099×66 | at(172,860) **1475×94** | unchanged |

★ **Root cause: `GetClientRect` (USER `0x21`) was stepped over.** Paint asked how
big it was, got nothing, and fell back to `width`/`height` from WIN.INI's
`[Paintbrush]` section — 1680×974 — so it laid its palette and line-size box out
**below the bottom** of a 688-tall client and made its toolbox taller than its own
parent. **Nothing was wrong with the drawing.** It was drawing the right picture
at the wrong size, in a window it had been given no way to measure.

★ **And the menu was never wrong.** Stock running the same binary shows
File · Edit · View · **Text** · Pick · Options · Help — identical to ours. The
reference screenshot that prompted the comparison has Font/Style/Size, so it is a
different Paintbrush build, not the Win3.11 one on the rig.

---

## ⚠⚠ A COUNTER OF MINE WAS LYING

`GetStockObject(9)` logged **"THE GDI TOKEN MAP IS FULL"** with nine of 256 slots
used. Index 9 is a **hole** in the stock-object numbering (between `NULL_PEN` and
`OEM_FIXED_FONT`); the OS correctly returned NULL, and the code reported the
wrong one of two different facts. Fixed — *"no such object"* and *"no room"* now
read differently.

`GetObject` also now prints the `LOGFONT`'s actual metrics, because Paint sizes
its whole toolbox from them and *"LOGFONT"* alone cannot be compared against an
oracle. It reports `h=16 w=7 weight=700 "System"`, which matches Win3.1's — **so
the font is not the remaining discrepancy.**

This is the same family as [a counter's layout is a
claim](session-21.md) and [an instrument must not infer its own
frame](session-27.md).

---

## STRUCTURES — ALL READ FROM A BINARY, NONE FROM A HEADER

* **`CREATESTRUCT`** — a Win16 `CreateWindow` **argument block IS the structure**,
  field for field: `lpParam@0, hInstance@4, hMenu@6, hwndParent@8, cy@10, cx@12,
  y@14, x@16, style@18, lpszName@22, lpszClass@26`, plus `dwExStyle@30` = 34
  bytes. Confirmed independently by PBRUSH reading `cx` at `+0x0c` and `cy` at
  `+0x0a`. Placed on the **guest's own stack** below the arguments — where real
  USER puts it, and it needs no allocator.
  ⚠ Must go **below** the arguments: the procedure returns `retf 0x0a`, which
  discards exactly the argument bytes.
  ⚠ `CW_USEDEFAULT` (`0x8000` in Win16, `0x80000000` in Win32) is substituted
  from the real window's geometry.
* **`PAINTSTRUCT`** — `hdc@0, fErase@2, rcPaint@4` (four `int`s), 32 bytes against
  Win32's 64. Read off `nedis.py guest/win16/PBRUSH.EXE 3 0x08f8`: `ps` is at
  `bp-0x2a`, it pushes `[bp-0x2a]` as the HDC, and computes `right-left+1` —
  which is what identifies which pair is which.
* **`GetObject`** is a *conversion*, dispatched on the object's **real type** (ask
  the OS), then `min(nCount, win16size)` bytes — which keeps partial reads
  working. `BITMAP` 14/24, `LOGFONT` 50/60, `LOGPEN` 10/16, `LOGBRUSH` 8/16.
  ★ Paint confirmed two of them itself by asking for `cb=0x32` and `cb=0x0e`.
  ⚠ `bmBits` stays NULL — a 32-bit host pointer has no 16-bit address, and
  Windows returns NULL there for a device-dependent bitmap anyway.
* **Win16 bitmap resources** are BITMAPINFOHEADER packed DIBs (`biSize`=0x28) →
  `CreateDIBitmap`. ⚠ `biSizeImage` can be junk (PARROW's is 8 for a 104-byte
  image); it is cleared in our own copy of the header rather than by writing into
  guest memory.
* **Win16 `RECT`** is four `int`s = 8 bytes, against Win32's 16.

---

## ★ IDS ONLY A RUN CAN NAME

`neneeds.py` correctly classifies a `native16` export as free — its entry point
really is 16-bit code — but such a wrapper can reach a WOW32 stub **from inside
its own body**, and the tool cannot see that. Six more this session:

| id | is | ordinal | note |
|---|---|---|---|
| GDI `0x99` | `CreateDC` | 53 | ⚠ **the id is not the ordinal** |
| GDI `0x57` | `GetStockObject` | 87 | |
| GDI `0x30` | `CreateBitmap` | 48 | |
| GDI `0x52` | `GetObject` | 82 | |
| USER `0xaf` | `LoadBitmap` | 175 | proven by the export's **tail-jump** into the caller's function, `retf 6` |
| USER `0x27`/`0x28` | `BeginPaint`/`EndPaint` | 39/40 | |

⚠ Sometimes the id equals the ordinal and sometimes it does not. **Name each from
its own call site, never from arithmetic.**

---

## SERVICES ADDED

**SHELL** — the whole registration database: `RegOpenKey` `0x01`, `RegCreateKey`
`0x02`, `RegCloseKey` `0x03`, `RegDeleteKey` `0x04`, `RegSetValue` `0x05`,
`RegQueryValue` `0x06`, `RegEnumKey` `0x07`.

> ⚠ **It is backed by `HKCU\Software\NTVDMEX\Win16Reg`, not the real
> `HKEY_CLASSES_ROOT`.** A literal reading would have Paint register `PBrush`,
> `pbrush.exe` and a `StdFileEditing` verb as a **system-wide document handler**
> on the user's Windows install, on every launch, and not undo it on exit. This
> host routes a guest; it does not get to re-register the desktop's file
> associations as a side effect. Nothing needs it there — Win16 OLE clients look
> the database up through these same seven calls. HKCU for the same reason
> `src/host/settings.h` uses it: no administrator rights.
> ⚠ The root arrives as **two** numbers and both are real: Win16 defines
> `HKEY_CLASSES_ROOT` as `1` and PBRUSH passes `1`, but OLESVR passes
> `0x80000000`. Both accepted.

**USER** — `GetDC` `0x42`, `GetWindowDC` `0x43`, `ReleaseDC` `0x44`, `LoadBitmap`
`0xaf`, `BeginPaint` `0x27`, `EndPaint` `0x28`, `GetClientRect` `0x21`,
`SetScrollRange` `0x40`, `SetScrollPos` `0x3e`, `SetCursor` `0x45`.

**GDI** — `GetStockObject` `0x57`, `CreateDC` `0x99`, `CreateCompatibleDC` `0x34`,
`CreateBitmap` `0x30`, `CreateCompatibleBitmap` `0x33`, `CreateSolidBrush` `0x42`,
`SelectObject` `0x2d`, `GetObject` `0x52`, `MoveTo` `0x14`, `LineTo` `0x13`,
`PatBlt` `0x1d`.

⚠ Drawing coordinates are **sign-extended, not widened**: `0xFFF0` is 16 pixels
left, not 65520.

---

## ⚠ THE REGRESSION GATE MOVED — AND IS FULLY ACCOUNTED FOR

**`64/122/78` → `81/122/61`** (serviced / declined / unimplemented), still ending
at `0001:229C`.

Declined is unchanged and the total is identical: the delta is **exactly 17
`GetStockObject` calls** that krnl386's own bootstrap was already making and that
the widened GDI anchor now lets us answer. An improvement, not drift — the same
shape as session 44's `OemToAnsi`/`AnsiToOem` move.

> ⚠ **The new gate is `81 / 122 / 61 · 0001:229C`.** Re-run with the switches
> **off** and confirm it before believing any later measurement.

---

## TOOLING ADDED

* `tools/ne/neneeds.py --stubs` — prints each stub's **retstub** (stub + 13),
  computed from the file, so an anchor can be widened without waiting for a run.
* `tools/ne/wowthunks.py --anchor <module>` — emits a module's whole stub table
  as a C array; `src/wow/wowanchors.h` is generated from it.
* `scripts/bm/rigshot.c` — `list` now prints each window's **window and client
  rectangles**; new **`tree "<caption>"`** verb walks every matching top-level
  window and its children with positions in the parent's client coordinates.

---

## ★★★ THE SECOND HALF: THE ORACLE, AND THREE REFUTED HYPOTHESES

After the user compared the screen against a real Paintbrush, the session turned
into a measured chase. It is worth reading as a sequence, because three of the
four leads were wrong and each was wrong in an instructive way.

**The oracle** — `wowcompare.bat` + the new `rigshot tree` verb — turned "it
paints wrong" into a table of exact child rectangles against stock ntvdm running
the same binary. That table named `GetClientRect`, and fixing it corrected the
canvas but not the toolbox.

| # | Hypothesis | Verdict |
|---|---|---|
| 1 | `GetDeviceCaps(HORZSIZE/VERTSIZE)` — our 1680/640 = 2.625 matched the over-scale against stock's 2.0 *exactly* | **REFUTED.** Forcing the ratio to 2.0 changed the toolbox by nothing, `163x731` to the pixel. |
| 2 | "the guest is never told its size" | **REFUTED.** `WM_SIZE` had been relayed since session 43; re-adding it was a duplicate case value and the compiler said so. The log shows it arriving with the correct `lParam 0x02b004e4` = 1252x688. |
| 3 | `IsWindow` / `IsWindowVisible` answered 0 | ★ **CORRECT.** |
| 4 | `GetDeviceCaps(NUMCOLORS)` = -1 causes the monochrome bitmaps | **REFUTED.** Substituting 256 logged `= 0x0100` and produced the same 33 1bpp bitmaps. |

★★★ **The layout bug was two of the smallest calls in USER.** Paint asks
`IsWindowVisible(pbTool)` and `IsWindowVisible(pbColor)` — Windows 3.1 Paintbrush
can genuinely hide both — got 0 for each, concluded its own toolbox and palette
were hidden, gave the canvas the whole client and **never resized them**. They
kept the size they were created at, from Paint's fallback height of 974
(`SM_CYFULLSCREEN - SM_CYMENU`, the default it passes to `GetProfileInt` because
this rig's WIN.INI has no `[Paintbrush] height`).

> **The layout was never mis-computed. It was never RE-computed.**

Answering those two truthfully made every child **pixel-identical to stock**:
`pbPaint at(128,2) 1100x604`, `pbTool at(3,2) 121x516`,
`pbSize at(3,521) 121x164`, `pbColor at(128,608) 1099x66`.

★ **And #4 refuted itself usefully**: the 1bpp bitmaps are **masks**. The Win16
idiom for a coloured icon is a monochrome pattern plus `SetTextColor`/`SetBkColor`
at blit time, and the run makes 34 of each. They were never evidence of a
mis-detected display.

## ★★★ THE DRAWING SET, AND THE TOOLBOX

Ten more GDI calls, each confirming its own reading out of its own arguments:

* `0x22 BitBlt` — rop `0x00CC0020` = SRCCOPY, both DC fields our own tokens.
* `0x23 StretchBlt` — `(dst 0x79 x 0x204) <- (src 0x3a x 0x117)`. ★ 58x279 is
  exactly the `pToolbox` DIB this host loads and 121x516 is exactly the toolbox
  measured against stock — **two numbers the session already knew
  independently, both turning up in one argument block.** This single call is
  the tool icons.
* `0x1b Rectangle` — `Rectangle(hdc,0,0,121,516)`, the toolbox's own border.
* `0x04 SetROP2` (13 = R2_COPYPEN) and `0x07 SetStretchBltMode`
  (3 = COLORONCOLOR) — both internal stubs, so only a run could name them, and
  the two constants are what make it a reading rather than a guess at an ordinal.
* `0x01 SetBkColor`, `0x09 SetTextColor`, `0x0b SetWindowOrg`, `0x1e SaveDC`,
  `0x27 RestoreDC`.

**GDI's id space is now answered for everything MS Paint calls.**

## ⚠ TWO HARNESS TRAPS HIT AGAIN

* **A stale artefact nearly produced a wrong conclusion.** `bmwow.sh` (the gate)
  overwrites `C:\ntvdmex\ntvdmhost.log`, so a log fetched after a gate run is
  the *baseline's*, not the guest's. It read as "GDI is fully answered, only four
  USER ids left" — from a 3976-line log ending at the WOWEXEC `0001:229C` fault.
  Check `grep -c pbParent` before believing a Paint log.
* **A shell one-liner deployed a stale binary** because `cp` ran even though the
  build had failed. Gate the deploy on `grep -q '^Built:'`.

---

## ▶ RESUME HERE

### What is still wrong

★ The window, the menu (which opens), the toolbox with its colour icons, the
line-size box, the palette bar, the canvas and its scrollbars are all present and
**geometrically pixel-identical to stock ntvdm**. Two content differences remain:

1. **MS Paint is running in its BLACK-AND-WHITE image mode.** The user's
   observation split the problem exactly: *"the tool palette renders the right
   colors, but the color palette is shades of grey"*, and that is the whole
   diagnosis in one sentence — the toolbox is a colour DIB `StretchBlt`ed
   straight to the screen and never round-trips, while everything Paint
   *composes* is monochrome.

   ★★★ **AND THE COLOUR IS NOT LOST IN THE BLIT — PAINT NEVER HAS IT.** The
   brushes it creates are already grey when created: `0x00090909`, `0x00121212`,
   `0x00212121`, `0x00323232`, `0x00404040`, … — 32 of them, **every one with
   R = G = B**, which is the luminance of the standard palette. Paint is drawing
   a greyscale palette on purpose, into 1bpp bitmaps, because it believes it is
   in black-and-white mode.

   ── WHAT IS ESTABLISHED ────────────────────────────────────────────────────
   * `PBRUSH.DLL` is what creates every 1bpp bitmap, and its **only** device
     query in a whole run is `GetDeviceCaps(NUMCOLORS)`, three times.
   * The call chain is located: `PBRUSH.DLL seg1:0x09c1` is the `CreateBitmap`
     (confirmed by `neimports.py`, and `0x09c1+5 = 0x09c6` is the `from=` the log
     carries). Its `cPlanes`/`cBitsPixel` are **parameters** read as BYTEs from
     `[bp+6]`/`[bp+4]`; its one caller is `seg1:0x00e8`, which passes them from
     `[bp+0x0c]`/`[bp+0x0a]` — i.e. threaded down from further up still.
   * ⚠ Selector `0x09b7` is **PBRUSH.DLL's** segment 1, not a PBRUSH.EXE segment.
     A pass was wasted mapping it by segment ordering; `neimports.py` names call
     sites directly and should have been the first tool reached for.

   ── ⚠⚠ REFUTED — DO NOT RE-TRY ─────────────────────────────────────────────
   * `NUMCOLORS` at **-1, 256 and 16** — all three give byte-identical 1bpp
     output. It is the DLL's only device input and **no value changes anything**.
   * `GetObject`'s `bmBitsPixel`. We report the OS's `0x20`, and 32bpp is a depth
     Win16 never had (1/4/8/16/24), and it is the only pixel format Paint can
     see — so 24 looked certain. It changed nothing.
   * `GetNearestColor` — never called in a whole run.
   * A WIN.INI colour/format key — Paint reads `OmitPictureFormat`, `width`,
     `height`, `extensions` and the `[intl]` keys, and nothing about colour.
   * `GetDeviceCaps(HORZSIZE/VERTSIZE)` — see the table above.

   ▶ Next step: the mode is decided in **PBRUSH.EXE**, not the DLL, and passed
   down. Untested inputs it reads: `NUMPENS` (-1, deliberately left alone) and
   `LOGPIXELSX/Y` (both 96). Better: read the caller chain above `seg1:0x00e8`
   in PBRUSH.DLL back to the global that holds the mode, and find who sets it.
2. Unanswered, none of them on the drawing path: USER `0x10c GlobalAddAtom` (25
   calls), `0x51`, `0x87 GetWindowLong`, `0x88`, and the WOW plumbing `0x13a`,
   `0x217`, `0x16c`.

### How to drive it

```bash
# Paint, left running for a human to use
SH=/private/tmp/xpshare
printf 'exec cmd /c ""C:\\...\\ntvdmex\\wowlive.bat" C:\\WIN16\\PBRUSH.EXE"\r\n' > $SH/control.txt

# ★ THE ORACLE: ours and stock ntvdm side by side, same binary, same box
printf 'exec cmd /c ""C:\\...\\ntvdmex\\wowcompare.bat" C:\\WIN16\\PBRUSH.EXE"\r\n' > $SH/control.txt
# then, for exact geometry of BOTH:
rigshot.exe tree "Paintbrush - (Untitled)"

# the enumerated list, and the anchors
python3 tools/ne/neneeds.py guest/win16/PBRUSH.EXE --todo --stubs
python3 tools/ne/wowthunks.py --anchor guest/win16/gdi.exe
```

⚠ Always re-run the gate with the switches **off** and confirm **`81/122/61`**.

### Ruled out — do not re-try

Everything in [session 44's list](session-44.md#ruled-out--do-not-re-try) still
holds, plus:

* **"Paint's blocker is GDI's remaining calls"** and **"Paint's blocker is the
  sent-vs-posted split"**. Both were on record; neither was true. Six other
  things were.
* **Installing Windows 3.1 on the DOS 6.22 QEMU box to get an oracle.** Not
  needed for app-level behaviour — stock `ntvdm` on the XP rig runs the same
  binary on the same screen and is already wired up. (It would still be the right
  oracle for something XP's own WOW reimplements.)
* **The menu bar being wrong.** It matches stock exactly; the Font/Style/Size
  reference is a different Paintbrush build.
* **The system font as the cause of the toolbox scale.** Measured: it matches.
* **Anchoring a module on one call.** See above — it silently un-implements
  everything else in that module.
