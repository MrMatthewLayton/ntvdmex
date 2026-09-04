# Session 47 — the enumerator was lying, and it was hiding 40 services

> Branch `m9/completeness`. Follows [session 46](session-46.md).

## ★★★★★ THE HEADLINE

The user's suggestion was *"disassemble Notepad and Paintbrush, find the DLL
imports they use, and wire them up"* — which is the method this project already
had (`tools/ne/neneeds.py`, session 44). **The tool was under-reporting the job by
half**, and fixing the tool is the whole session:

| | before | after |
|---|---|---|
| MS Paint, GDI | 41 need us, 24 serviced | **76 need us, 67 serviced** |
| MS Paint, USER | 52 need us, 27 serviced | **92 need us, 80 serviced** |
| PBRUSH.DLL, GDI | 13 need us, 12 serviced | **16 need us, 16 serviced — complete** |
| Notepad, GDI+USER | 25 need us, 25 serviced | **56 need us, 52 serviced** |

Both reported icon defects are fixed and confirmed against stock ntvdm pixel by
pixel. `File > Save As` is **not** fixed, and is diagnosed to the instruction.

⚠ Regression gate **unchanged at `82 / 122 / 60 · 0001:229C`** across the whole
batch, including a `RegisterClass` change that touches every guest.

---

## ★★★★★ WHY THE LIST WAS SHORT: A VALIDATING WRAPPER IS STILL A THUNK

`neneeds.py` resolves each imported ordinal through the exporting module's entry
table and asks whether the bytes there are a WOW32 stub. It knew two export
shapes: the bare stub, and USER's *exact* tail-jump

```
55 8b ec  68 <retstub>  5a 5d  e9 <rel16>          ; push bp/mov bp,sp/push/pop dx/pop bp/jmp
```

Several of GDI's and USER's exports check an argument first, so the `5a 5d e9`
sits ten or eighty bytes further down:

```
gdi.372 EXTFLOODFILL  seg1:0x1b77
  55 8b ec / 68 90 1b            push the return stub
  8b 46 06 / 3d 01 00 / 76 06    validate the fill TYPE
  bb 01 e0 / e8 88 0b            …or complain
  5a 5d e9 fc e8                 pop dx / pop bp / JMP 0x048c
0x048c: 6a 0c 68 00 00 68 74 01 9a   -> id 0x174, 12 argument bytes
```

Everything in that shape was reported **free**. That is how session 46 found
`ExtFloodFill` and `CreatePen` only by running the guest, and it is why
`SetDIBits` — what `File > Save As` needs — was on neither the done list nor the
to-do list.

### ⚠⚠ THE FIRST FIX WAS WRONG IN A WAY THAT LOOKED RIGHT

Scanning forward for `5a 5d e9` and requiring the target to *be a stub* is safe.
Scanning forward **0x200 bytes** is not: these wrappers are 0x20–0x60 bytes
apart, so the window walks over the next six exports, collects their epilogues
too, and the "if they disagree, refuse" guard then refused almost everything.
`TEXTOUT` classified only because it happens to sit near the end of the segment.

⇒ **A guard that fails safe still gives a wrong answer if you feed it the wrong
function.** The bound is in the binary: the wrapper pushes the address of its own
`retf` at +3, so the body is exactly `[off, that imm16)` and the scan cannot
leave it. Two more shapes went in with it — a stub behind a near `call`
(`gdi.87 GETSTOCKOBJECT`) and a leading `eb 00` (`gdi.53 CREATEDC`).

### ★★★ AND IT CORRECTED A RECORDED FACT: `0x99` IS `CreateIC`, NOT `CreateDC`

Session 45 named `0x99` from a run and concluded *"the id does not track the
ordinal"*. With both ordinals now resolved from the binary side by side:

```
ord  53 CREATEDC  -> id 0x35, 16 args
ord 153 CREATEIC  -> id 0x99, 16 args      ← 153 IS 0x99
```

The id tracked the ordinal all along; the wrong ordinal was attached to it. It
survived unnoticed because an information context and a device context answer
every query identically — **servicing an IC as a DC works, and only the name was
wrong.** Both are answered now, and `CreateIC` gets a real `CreateICA`.
⇒ *A wrong name is not harmless: that paragraph generalised a rule from a case
where the rule did not apply.*

---

## ★★★★★ THE TWO ICON DEFECTS, BOTH MEASURED

### 1. Paintbrush had no icon: its `GROUP_ICON` is NAMED

Read straight out of the two resource tables:

```
NOTEPAD.EXE   RT_GROUP_ICON  #1          -- an ordinal, so it worked
PBRUSH.EXE    RT_GROUP_ICON  "PBRUSH"    -- a NAME, and USER id 0xad REFUSED it
```

The refusal was deliberate and said so in the code: *"a NAMED resource … not
something a run has shown us, so it is refused by name"*. One had shown it — MS
Paint, every time, in silence, because a class with no icon is
indistinguishable from a class Windows gives the default icon to.

⚠⚠ **This is the third time the same gap has been found.** Session 45 hit it on
MENUS (`MENU="PBrush2"`), fixed `wowres_find` for menus, and left the icon and
cursor lookups integer-only. **A fix not carried to every lookup of the same kind
is half a fix.** Paint's seven cursors are named too — `"FLOOD"`, `"CROSSH"`,
`"PICK"`, `"TEXT"`, `"SIDEAROW"` — so a paint program's pointer never changed
shape either. Both now resolve, and `SetClassWord(GCW_HCURSOR)` (18 calls a run,
all previously stepped over) is what applies them per tool.

### 2. Notepad's icon was grey — and it was the TASKBAR, not the caption

The user said the icon "should mostly be cyan, but rendered grey". The file has
the colour: ICON #2 is 4bpp with the standard 16-colour palette, 127 of its
opaque pixels cyan. The host log said it was building that one
(`icon=0x0001 (the app's own, 04 bpp)`). So it was run **beside stock ntvdm on
the same desktop** and the pixels counted out of one screenshot:

| | cyan-ish pixels |
|---|---|
| our caption icon | 53 ✓ |
| **our taskbar icon** | **0** |
| stock's taskbar icon | 59 ✓ |

Same window, same `HICON`, two renderings — so the taskbar was not drawing the
icon we built. A `WNDCLASSA` has one icon field, and a class with no **small**
icon makes Windows derive one; what came out was a washed-out monochrome version
of the right picture.
⇒ Register with `WNDCLASSEXA` and build a real 16×16 with
`CreateIconFromResourceEx(…, SM_CXSMICON, SM_CYSMICON, …)` from the same group.
Re-measured: **ours 85 cyan-ish, stock 62** — a match.
Evidence: [`-before`](../../research/evidence/session47-notepad-icon-vs-stock-before.png),
[`-after`](../../research/evidence/session47-notepad-icon-vs-stock-after.png).

⚠ The lesson is the oracle's, not the icon's: *"our log says we built the right
icon"* was true and did not answer the question. Only the side-by-side did.

---

## What went in — 40 services

**GDI (22):** `TextOut`, `GetTextExtent`, `GetTextMetrics` (the Win16
`TEXTMETRIC` layout read off `NOTEPAD.EXE seg1:0x1192` — it computes
`tm+8 + tm+0` for the line height and `tm+10 << 3` for the tab stop, which pins
the `short` prefix twice over), `SetTextAlign`, `CreateFontIndirect`,
`CreateDC` (0x35) + `CreateIC` (0x99), `DPtoLP`, `GetBitmapBits`,
`SetBitmapBits`, **`SetDIBits`/`GetDIBits`/`StretchDIBits`**, `CreatePalette`,
`GetNearestPaletteIndex` (a real lookup now that a producer exists), and session
46's batch.

**USER (18):** **`DefWindowProc`**, `SetClassWord`, `Get/SetWindowLong`,
`GetKeyState`, `GetSysColor`, `GetMessagePos`, `GetMessageExtraInfo`,
`GetDesktopWindow`, `BringWindowToTop`, `DrawMenuBar`, `ShowCursor`,
`GetCursorPos`, `SetCursorPos`, `ScreenToClient`, `InvertRect`,
`GlobalAddAtom`/`GlobalDeleteAtom` (25 calls a run — MS Paint registers itself as
an OLE server), `SelectPalette`/`RealizePalette`, the five caret calls,
`SetWindowPos`, `GetScrollPos`.

### ⚠⚠ AND `USER.107 DEFWINDOWPROC` IS *NOT* PURELY 16-BIT — a note corrected

`wowuser.h` said, with reasoning: *"USER implements it ITSELF, which is why a
whole run of Notepad never produced one as a BOP."* Its prologue is 16-bit; the
function is not:

```
user seg1:0x1d5e  push bp / mov bp,sp / push 0x1d86
      0x1d73      lcall 0x1d37:0x38fe      ; USER's own half
      0x1d78      or ax,ax / jne 0x1d81
      0x1d7c      pop bx / pop bp / cdq / jmp bx   ; handled -> return
      0x1d81      pop dx / pop bp / jmp 0x03e8     ; NOT handled -> forward
      0x03e8      call 0x013c / push 0xa / push 0 / push 0x6b   ; ← WOW32 id 0x6b
```

The supporting evidence was true and did not mean what it was taken to mean: it
showed USER's own half had handled everything Notepad passed on, not that the
32-bit half did not exist. Everything it forwards had been getting the sentinel;
it now reaches `DefWindowProcA` on the real `HWND`, which is where non-client
painting, sizing, activation and the system menu come from.

**Constants read out of the guest, never from a header:** `GCW_HCURSOR = -12` and
`GWL_STYLE = -16` from `PBRUSH seg3:0x09b8` and `seg3:0x0241`; `GetKeyState`
tests the high bit (`and ax,0x8000` at `seg3:0x1346`).

---

## ⚠ `File > Save As` — NOT FIXED, AND DIAGNOSED TO THE INSTRUCTION

Reproduced exactly as reported: the dialog opens, and Paint dies on OK with its
own message box — *"PBRUSHX caused a General Protection Fault in module
KRNL386.EXE at 0001:5349."*

**The chain, end to end:**

1. `GetSaveFileName` works and returns a path.
2. Paint calls `KERNEL.102 DOS3CALL`, which enters krnl386's own INT 21h
   dispatcher at `seg1:0x5300`.
3. `seg1:0x5343` is a four-instruction helper: `les di,[0x275]` /
   `mov al,es:[di]` — it reads the **current-drive byte** out of the DOS
   structures krnl386 cached at boot. It **#GP**s.
4. That cache is built at `seg1:0xc033`: `INT 21h AH=52h` (list of lists), then
   six pointers copied out of `es:[di+0x00/0c/10/18/24/28]` with their **segment
   halves stored as 0** — and at `seg1:0xc0fd`, `mov ax,2 / int 31h` (**DPMI
   Segment→Descriptor**) converts the SysVars segment into a selector that is
   written into all six.
5. The run shows that call succeeding: `INT31h AX=0002 BX=0x50 -> sel 0x018f`.
6. **And then selector `0x018f` is REDEFINED**:
   `INT31h AX=000C BX=0x018f <- desc base=0x03b4c1c0 limit=0x0000003f`.
   The fault dump confirms it — `es=0x018f{base=0x03b4c240 lim=0x3f}` with
   `di=0x7a0`. **A 64-byte selector, indexed 1952 bytes in.**

### ⚠⚠⚠ THE MECHANISM ABOVE WAS FIRST WRITTEN UP WRONG — CORRECTED

The first reading was *"the selector is freed and then recycled to somebody
else"*, and it was checked afterwards rather than before. It does not survive the
check: **`grep -c recycled` over the whole run is 0, and there is no `DPMI 0001`
on `0x018f` anywhere.** Nothing was ever freed. What the log actually shows is

```
INT31h AX=0002 BX=0x50   -> sel 0x018f              ; we mint it, idx 0x31
INT31h AX=000C BX=0x018f <- base=0x0002a800 lim=0x31f   ; THE GUEST SETS A
INT31h AX=000C BX=0x018f <- base=0x03b4c1c0 lim=0x003f   ; DESCRIPTOR ON IT
LDTSYNC idx 0x31 <- guest wrote base=0 limit=0x190 acc=0x0f INSTALL FAILED
```

⇒ **krnl386 reuses LDT index 0x31 because it maintains its OWN allocation over
the same LDT** — through `DPMI 000C` *and* by writing the descriptor shadow
directly (`AX=04F2` commits it). It never asked us for that index and has no way
to know `dpmi_seg_to_desc` took it. **Two allocators, one table, growing into each
other.**

★★ AND THAT IS WHY NOTEPAD DIES THE SAME WAY (the user, after session 47's first
run). This is not a Paint defect and never was: it is krnl386's, so it is
every guest's, and any new program we try will hit it the first time it touches
a file. That moves it from "a Paint bug" to **the single blocker in front of
everything**.

▶ **The fix is an allocator-ownership change**, and the pool cannot be picked by
guess: krnl386 was observed writing indices `0x30`, `0x31`, `0x34` and `0x122`,
so neither "low" nor "high" is demonstrably safe. ⚠ **The next step is a
measurement, not a patch**: log every LDT index krnl386 touches across a full
run, then take the host's private pool from a range it demonstrably never uses.
⚠ And the change lands in the DPMI selector allocator, which is Doom's shared
path — [[fix-measured-on-one-guest]] requires a DPMI guest re-measured in the
same session.

### ⚠ Two things ruled out on the way, both worth not re-trying

* **Declining `0x82`/`0xc1`** (the two unimplemented krnl386 DOS-dispatcher calls
  beside the fault). Tried through `wow32ret.txt` as a one-run experiment: the
  fault **moved** from `0001:5349` to `0001:53DB`, i.e. declining takes the
  intended chain-to-DOS arm and that arm dereferences the same broken cache. Not
  the cause. ⚠ `wow32ret.txt` was restored to empty.
* **The long path.** `GetSaveFileName` returned
  `C:\Documents and Settings\Matthew\Desktop\test.BMP` and this host's own log
  said *"NO 8.3 NAME … a Win16 OpenFile will probably refuse it"*. That WAS a
  real defect and is **fixed** — `GetShortPathNameA` fails on a file that does
  not exist yet, which is every Save As, so the **directory** is shortened and the
  leaf put back (`C:\DOCUME~1\Matthew\Desktop\test.BMP`). It did not change the
  crash, which is how we know it was not the cause.

---

## ▶ RESUME HERE

1. ★★★★★ **`File > Save As` — the DPMI selector-recycling fix above.** Everything
   needed is written down; what is missing is the allocator change and a Doom
   re-measurement in the same session.
2. **What is still unserviced** (22 ids, all enumerated and named):
   GDI `Escape`, `EnumObjects`, `LineDDA`, `CreatePolygonRgn`,
   `GetCharABCWidths`, `GetPaletteEntries`, `CreateMetaFile`/`PlayMetaFile`/
   `CloseMetaFile`; USER `IsDialogMessage`, `SetDlgItemText`, `GetDlgItemInt`,
   the three dialog-button calls, `ModifyMenu`, `GetMenuState`, `TabbedTextOut`,
   `ScrollWindow`, the three clipboard calls. ⚠ `EnumObjects` and `LineDDA` take
   **16-bit callbacks** and need `wowcall`, not a pass-through; the metafile trio
   is Edit > Paste.
3. **"Menu clicks crash the app" is now partly reproduced**: File > Save As is a
   menu click that crashes. Whether that is the user's whole report is unknown —
   ask which other items.

### Ruled out — do not re-try

Everything in [session 46's list](session-46.md#ruled-out--do-not-re-try), plus:

* **`neneeds.py`'s "free (16-bit)" column as a statement about work.** It was
  wrong for 40 services. The tool is fixed; the habit of trusting a static
  classifier over a run is what to keep in mind.
* **"the id does not track the ordinal" as a general rule for GDI.** It came from
  one misidentified export (`0x99`). Resolve both directions from the entry table.
* **A fixed-size forward scan for an export's tail jump.** Bound it by the
  export's own pushed `retf` address.
