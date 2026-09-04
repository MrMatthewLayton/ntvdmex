# Session 46 — MS Paint is a working paint program

> Branch `m9/completeness`. One commit.

## ★★★★★ THE HEADLINE

**MS Paint draws, in colour, and what it draws stays drawn.** All three of the
defects the user reported are closed:

| reported | cause | state |
|---|---|---|
| "There is still no color palette" | `GetProfileString` (krnl386 id `0x3a`) unimplemented | **FIXED** |
| "the stroke does not persist" | the same call — a B/W image has a 1bpp canvas | **FIXED** |
| "Some drawing functions work, others (like fill) do not" | `ExtFloodFill` + **`CreatePen`** unimplemented | **FIXED** |

Measured end to end on the rig: a **red-outlined box**, a **flood fill** that
stops at that border, a **green ellipse** and a **purple brush stroke** — then
Paint minimised (the window's pixels genuinely destroyed) and restored, and the
whole picture repaints from Paint's own image.

Evidence:
[`session46-pbrush-colour-palette.png`](../../research/evidence/session46-pbrush-colour-palette.png),
[`-tools-in-colour`](../../research/evidence/session46-pbrush-tools-in-colour.png),
[`-persists-after-restore`](../../research/evidence/session46-pbrush-persists-after-restore.png).

---

## ★★★★★ THE PALETTE: PAINT ASKED WIN.INI A QUESTION AND WE DID NOT ANSWER

Session 45 established that Paint's palette brushes are **already grey when
created** — 32 `CreateSolidBrush` calls, every one R = G = B — and that its
canvas is a 1bpp bitmap, and concluded it believes it is in its black-and-white
image mode. It located the `CreateBitmap` chain and left the instruction *"read
that chain back to the global that holds the mode"*.

**The mode is not held in a global the DLL threads down; it is decided in eleven
instructions in `PBRUSH.EXE seg2`, and the input is WIN.INI.**

The route there was the guest's own data, not the guest's own code. The two
palettes are **two static tables, side by side in DGROUP**:

```
DG:0x0932  ffffff 000000 c0c0c0 808080 0000ff 000080 00ffff 008080 …   28 COLOURS
DG:0x09a2  ffffff 000000 fafafa 090909 f2f2f2 121212 e2e2e2 212121 …   28 GREYS
```

Grepping the binary for `09 09 09` — a value every previous run had logged as a
brush — found the second table in one pass, and `seg2:0x071f` copies **both** of
them into live arrays (`0x4cf2` colour, `0x2c60` grey). A scan for those two
addresses as immediates finds the one instruction pair that chooses:

```
08a4  cmp  word ptr [bp-8], 2        ; the LENGTH GetProfileString returned
08a8  jbe  0x08c1                    ; ≤ 2 -> AX = 0
08aa  lstrcmpi(ds:0x130 "COLOR", the buffer)
08b8  cmp ax,1 / sbb ax,ax / neg ax  ; AX = (equal) ? 1 : 0
08c6  or ax,ax / je 0x08d2
08ca  mov  word ptr [0x499a], 0x4cf2 ; -> THE COLOUR TABLE
08de  mov  word ptr [0x499a], 0x2c60 ; -> THE 28 GREYS
```

and `neimports.py` names the call four instructions above it:

```
seg2:call 0x089f      KERNEL.58 GETPROFILESTRING
      GetProfileString("Paintbrush", "clear", "COLOR", buf, 9)
```

⇒ **`[Paintbrush] clear=COLOR` in WIN.INI, and the DEFAULT IS "COLOR".** The run
log then says the rest in one line:

```
FUNC=0x0000003a … from=0x0ac7:0x08a4  (0009 | 6f0a 09c7 | 012a 09c7 | 090e 09c7 | 08f4 09c7)
   ★ arg[7] = "Paintbrush"
   -> UNIMPLEMENTED, STEPPED OVER … ANSWERED 0x00000000 (harness sentinel)
```

Zero characters returned, `0 ≤ 2`, grey table. Answered — 18 argument bytes, a
straight `GetProfileStringA` — it returns **5**, Paint compares `"COLOR"` with
`"COLOR"`, and the same run creates `0x0000ff`, `0x00ff00`, `0xff0000`, `0xffff00`
… and a canvas bitmap that is **`planes=1 bpp=32`** instead of `planes=1 bpp=1`.

### ⚠⚠ FIVE THINGS WERE REFUTED AND THE SIXTH WAS NEVER TRIED

Session 45's ruled-out list included *"a WIN.INI colour key (none exists)"*, and
that observation was **correct and led away from the answer**. There is no
`[Paintbrush] clear` key on this rig — and there does not need to be, because the
*default* is colour. The bug was never in the profile. **An unimplemented call
cannot return a default**, so it turned "the key is absent" into "the key says
something that is not COLOR", which is the one reading the guest treats as
monochrome.

⇒ **A call that hands back a DEFAULT is not optional plumbing.** `GetProfileInt`
was implemented three sessions ago for exactly this reason and its string twin
sat one id away, unnamed, for three sessions.

---

## ★★★★★ THE FILL: `ExtFloodFill`, AND A WRAPPER THAT HIDES A STUB

`tools/ne/neneeds.py` said MS Paint needs 41 GDI services, 24 of them serviced
and 17 to do — and **`ExtFloodFill` is not on either list.** It is counted as
*"free (16-bit)"*, because GDI.EXE's ordinal-372 export is not a bare tail-jump:

```
gdi seg1:0x1b77   push bp / mov bp,sp / push 0x1b90
                  mov ax,[bp+6] / cmp ax,1 / jbe 0x1b8b     ; validate fill TYPE
                  mov bx,0xe001 / call 0x2713               ; …or complain
     0x1b8b        pop dx / pop bp / jmp 0x048c
     0x048c        push 0xc / push 0 / push 0x174           ; ← THE ID
```

One validating instruction more than the scan looks for. That is the
`native16 ≠ free` trap in [session 44's list](session-44.md), and it is worth
restating in its strongest form: **the static tool under-counts, and the only
thing that finds these is a run.**

`neimports.py` also names the whole fill tool in one line each — it is
`PBRUSH.EXE seg4:0x1560..0x16e0`, and it calls `ExtFloodFill` **twice**, once for
a solid colour and once after `CreatePatternBrush`.

---

## ★★★★★ AND THEN THE SHAPES: A TOOL THAT CANNOT MAKE A PEN HAS NOTHING TO DRAW WITH

With the fill in, the box and the ellipse still drew **nothing** — and the log
made that look impossible: 48 `Ellipse` calls in one drag, every one returning 1,
at exactly the right coordinates.

They were all the **rubber band**. `GetROP2` returns `0x0007` = `R2_XORPEN`
between every pair, which is a preview drawing and undrawing itself. The commit
is the six calls after the button comes up, and two of them were stepped over:

```
SetBkMode(0x20c0, 0002)             id 0x02, 4 args   -- UNIMPLEMENTED
SetROP2(0x20c0, 000d)               id 0x04           -- serviced (R2_COPYPEN)
SelectObject(0x20c0, 0x2028)        id 0x2d           -- serviced (the brush)
CreatePen(006, 0002, 0x000000ff)    id 0x3d, 8 args   -- UNIMPLEMENTED
SelectObject(0x20c0, 0x2000)        …                 -- and it gave up
```

⇒ Paint asked for a 2-pixel `PS_INSIDEFRAME` pen in the colour it had been given,
got 0, and **correctly declined to draw with a pen that does not exist.** The two
calls it made are visible as data: `(000000ff …)` for the red box and
`(0000ff00 …)` for the green ellipse.

★ **Two independent readings agree on `0x3d`.** The run logged the id and 8
argument bytes; GDI.EXE's ordinal-61 wrapper at `seg1:0x177f` tail-jumps to
`0x0284`, which pushes 8 argument bytes and the id `0x3d`. ⚠ Here the id happens
to equal the ordinal, and elsewhere it does not (CreateDC is ordinal 53 and id
`0x99`), so every id in this session was resolved through GDI's entry table
rather than assumed from arithmetic.

⚠ **"Nothing draws" and "the tool is not selected" look identical from outside**,
and the first hypothesis was the wrong one — the tool was selected, the drag was
tracked, the geometry was right and the API returned success 48 times.

---

## What went in

**`src/wow/wow32.h`** — krnl386 `0x3a GetProfileString`.

**`src/wow/wowgdi.h`** — 24 GDI services, every id read out of `gdi.exe`'s own
entry table and stub:

| id | call | id | call |
|---|---|---|---|
| `0x02` | SetBkMode | `0x40` | CreateRectRgn |
| `0x03` | SetMapMode | `0x4b` | GetBkColor |
| `0x06` | SetPolyFillMode | `0x53` | GetPixel |
| `0x0c` | SetWindowExt | `0x55` | GetROP2 |
| `0x0d` | SetViewportOrg | `0x9a` | GetNearestColor |
| `0x0e` | SetViewportExt | `0xa3` | SetBitmapDimension |
| `0x15` | ExcludeClipRect | `0x16e` | UpdateColors |
| `0x18` | **Ellipse** | `0x172` | GetNearestPaletteIndex |
| `0x1c` | RoundRect | `0x174` | **ExtFloodFill** |
| `0x1f` | SetPixel | `0x25` | Polyline |
| `0x2c` | SelectClipRgn | `0x3a` | CreateHatchBrush |
| `0x3c` | CreatePatternBrush | `0x3d` | **CreatePen** |

⚠ `GetNearestPaletteIndex` answers **0 and says so in the log**: this host has
never made a palette object (`CreatePalette` reaches a stub no run has named), and
a wrong palette index is a wrong colour with no way to tell afterwards that it was
invented.

---

## ⚠ THE REGRESSION GATE MOVED BY EXACTLY ONE CALL, AND IT HAS A NAME

**`82 / 122 / 60 · 0001:229C`**, against session 45's `81 / 122 / 61`. Total
identical (264), declined identical (122); one call moved from UNIMPLEMENTED to
SERVICED and it is `GetProfileString(…, "NwcsInstalled", …)` at log line 3873,
which the bootstrap asks during its NetWare-shim probe. Improvement, not drift.

⚠ **The gate must be run with `wowsched.txt` and `wowcall.txt` ABSENT.** The
first attempt left them in place (because `wowlive.bat` creates them) and
measured `238/308/101` over 663 BOPs — a completely different configuration that
looks like catastrophic drift and is actually a guest running much further. Move
them aside, run, move them back.

---

## ▶ RESUME HERE

### Where MS Paint is

**It is a working paint program.** Colour palette, box, rounded box, ellipse,
brush, flood fill, and everything persists across a full repaint. Its layout is
pixel-identical to stock ntvdm, its menu bar opens, and mouse and keyboard reach
it.

### What is still open, in the order worth attacking

1. **"Menu clicks crash the app" — STILL NOT REPRODUCED** (session 45 could not
   either). Alt-F/Down/Enter opens File > Open, a COMMDLG modal that parks the
   whole VDM on the exec thread, which is indistinguishable from a hang from
   outside. **Ask the user which menu item**, or drive every item with
   `rigshot click` and watch for a real fault.
2. **The tools not yet exercised**: Text (`TextOut` id `0x21`, 12 args;
   `CreateFontIndirect` `0x39`), the cutout tools (`CreatePolygonRgn` `0x3f`),
   Edit > Paste (`PlayMetaFile` `0x7b`), and **File > Save**, which needs
   `SetDIBits`/`GetDIBits`/`StretchDIBits` — ⚠ those three are the only Paint
   imports whose wrappers have **no stub the scanner can find**, so they will
   need a run to name.
3. **Still unanswered and still not on the drawing path**: USER `0x10c`
   GlobalAddAtom (25 calls), `0x82` SetClassWord (18), `0x52`, `0x87`/`0x88`
   Get/SetWindowLong, and the WOW plumbing `0x13a`/`0x217`/`0x16c`.
4. `SelectPalette` (USER `0x11a`) and `RealizePalette` (`0x11b`) are called
   before nearly every draw and are still stepped over. ⚠ **Deliberately not
   implemented**: on a 32bpp display they change nothing, and Paint's
   `CreatePalette` reaches an unnamed stub, so an implementation would answer 0
   for an unknown handle — which is exactly what the sentinel already does.
   Implementing it would add a log line and no behaviour.

### How to drive it

```bash
SH=/private/tmp/xpshare
RES='C:\Documents and Settings\All Users\Documents\ntvdmex'

printf 'exec cmd /c ""%s\\wowlive.bat" C:\\WIN16\\PBRUSH.EXE"\r\n' "$RES" > $SH/control.txt
printf 'exec cmd /c ""%s\\pbtools.bat""\r\n'   "$RES" > $SH/control.txt   # box+fill+ellipse+stroke
printf 'exec cmd /c ""%s\\pbmin.bat""\r\n'     "$RES" > $SH/control.txt   # minimise/restore = the persistence test
```

⚠ **`pbrepaint.bat` (scrollbar clicks) DOES NOT FORCE A REPAINT** — the clicks
land and nothing scrolls, and two identical screenshots read exactly like "it
persisted". `pbmin.bat` minimises through the taskbar button, which destroys the
window's pixels, and is the only honest test of persistence here.

### Ruled out — do not re-try

Everything in [session 45's list](session-45.md#ruled-out--do-not-re-try) still
holds, plus:

* **"the B/W mode is decided in `PBRUSH.DLL`"** and **"read the `CreateBitmap`
  caller chain back to the mode global"**. Both were session 45's stated next
  step. The DLL is a *virtual bitmap manager* (its non-resident names say so:
  `VCREATEBITMAP`, `VBITBLT`, `VSTRETCHBLT`, `GETVCACHEDC`) and it is handed the
  planes and depth from far above; the decision is eleven instructions in
  `PBRUSH.EXE seg2` and its input is a profile string.
* **"the shape tools are not being selected"**. They are; the rubber band tracks
  the whole drag and the API succeeds every time.
* **`neneeds.py`'s TO-DO list as the definition of what is missing.** Three of the
  four calls that mattered this session — `ExtFloodFill`, `CreatePen`,
  `SetBkMode` — are on its *free* list.
