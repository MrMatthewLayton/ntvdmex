# Session 43 — Notepad from Windows 3.11, with its menu and its icon

- **Branch:** `m9/completeness`
- **Issue:** [#128](https://github.com/MrMatthewLayton/ntvdmex/issues/128)
- **Predecessor:** [session 42](session-42.md) — SYSEDIT ran on the XP desktop as itself,
  as real Win32 windows. The frontier was named as GDI.

---

## ★★★★★ THE HEADLINE

**Windows 3.11 `NOTEPAD.EXE` runs on the Windows XP desktop** — one of the two
north-star applications — with:

- its own **menu bar**, File / Edit / Search / Help, built from its own resource;
- its own **icon**, the 16-colour notepad-and-pencil, on the taskbar;
- the caption its program gives it, **"Notepad - (Untitled)"**;
- its `EDIT` control, XP theming, real scrollbars, a real taskbar button;
- and it **stays running**, sitting in its own message loop, until it is closed.

Twelve Windows 3.11 applications are on the rig and a triage script says how far each
of them gets.

---

## Part 1 — getting the guests onto the box

Eight raw floppy images appeared on the share. `tools/fat12.py` reads them here rather
than mounting them: `hdiutil` needs privileges, is not available everywhere, and does
its own filename mangling. ⚠ Its geometry comes from each image's **BPB**, not from
1.44 MB constants — hardcoding those would work for these eight and quietly corrupt the
first 720 KB image anyone tried afterwards.

★ **Two things I had asserted from memory, both wrong, both corrected by measurement:**

1. **The files are `KWAJ`, not SZDD.** That is literally the first four bytes of
   `NOTEPAD.EX_`.
2. **XP's `EXPAND.EXE` does not understand KWAJ — and does not say so.** It reports
   *"25112 bytes copied"* and writes the input verbatim under a name with the underscore
   stripped. A tool that succeeds while doing nothing is the worst kind, which is why
   `w16expand.bat` ends by listing compressed input beside expanded output: if the sizes
   match, nothing happened.

The decompressor that works is Microsoft's own 16-bit `EXPAND.EXE`, which ships
**uncompressed on Disk 6** — it has to, it is what unpacks the rest. Run under stock
ntvdm, with the IFEO Debugger key dropped and restored on every path and the restoration
verified by `reg query`.

⚠ Running EXPAND under **NTVDMEX** was tried first and is worth coming back to — a real
DOS workload with arguments and file I/O — but it produced no output file and no captured
stdout, which is the console/stdio blocker already on the STATE list.

⚠ **And a defect in my own script, caught by the size check.** The first cut expanded
everything to `%%~nf.tmp` and renamed afterwards — but `%%~nf` is the stem *without* the
extension, so `CALC.EX_` and `CALC.HL_` both became `CALC.tmp`, the help file overwrote
the program, and the rename published a `.HLP` as `CALC.EXE`. Exactly four files came out
valid and they were exactly the four with no matching `.HL_`. **A temp name that is not
unique is not a temp name.**

---

## Part 2 — twelve guests, and the failures name themselves

`scripts/wowtriage.sh` launches a set of guests and prints how far each got. *"Can I test
the Windows 3.x apps yet"* is not a question about one guest, and answering it by
reasoning about which APIs each probably needs is exactly the kind of guess this project
keeps being wrong about. Every app is cheap to launch; the honest answer is the table.

⚠ **The script caught itself on its second use.** Every run in its loop is `--no-deploy`,
which is right, but the first cut had no deploy *at all* — so a whole pass measured the
previous build and reported "no change" about a fix that had never reached the box. It
deploys and checksums once before the loop now. That is this project's oldest trap and it
appeared **three times in this session**; see Part 5.

**The table as it stands at the end of the session** — its OWN classes / its OWN windows /
whether one reached the desktop. `no exit` means the guest was still alive when the run's
watch time ran out, which for an interactive program is the *good* answer:

```
APP          bop   uni   cls   win  shown  end
NOTEPAD      541   153     1     2      1  no exit   ★ menu, icon, caption, message loop
WINMINE      540   195     1     1      0  clean
CALC         575   174     1     0      0  no exit
CLOCK        531   170     1     0      0  no exit
SOL          482   133     0     0      0  clean     "Out of memory"
CARDFILE     567   156     2     0      0  clean
CHARMAP      473   140     2     0      0  no exit
PBRUSH       792   203    10     2      0  no exit   ★ ten classes, two windows
WRITE        631   167     6     0      0  clean
TERMINAL     546   144     2     1      1  no exit   ★ a window on the desktop
PROGMAN      468   141     0     0      0  clean
WINFILE      598   147     0     0      0  clean
```

The failures named themselves, which is the point of running them:

- **CARDFILE and WRITE** stopped in `RegisterClipboardFormat` (USER `0x91`), registering
  the OLE 1.0 set — `ObjectLink`, `OwnerLink`, `Native`, `Binary`, `FileName`,
  `NetworkName` — and getting the sentinel `0`, which is the documented *failure* value.
- **WINFILE** said *"Cannot find COMMCTRL.DLL"* — not a host defect, a dependency that had
  not been extracted.
- **SOL** says *"Out of memory"*; **PROGMAN** stops on `0xad`.

---

## Part 3 — the three services that made Notepad run

### ★★★ `0xad` — "build me a predefined cursor or icon"

Notepad's initialisation (entry-table ordinal 14, `seg2:0x02a3`) does

```
02d0  push 0 / push 0 / push 0x7f02   ; LoadCursor(NULL, <ordinal>)
02dc  mov [0xb14], ax
02ee  cmp [0xb14], 0 / je -> sub ax,ax / ret     ; NO CURSOR, NO NOTEPAD
```

and `WinMain` (`seg1:0x0c38 or ax,ax / jne`) returns immediately on the 0 — which is why
it never registered a class. The run agreed field for field: the last call its task made
was USER `0xad` with `0x7f02` in its argument block.

`0xad` is one of the 56 ids USER's export table cannot name; its **call site** names it
(`user seg1:0x49e2`, the arm taken when the module handle came back 0): kind **1**,
ordinal at `[bp+6]`, high word at `[bp+8]`.

★★ **The host does not decide cursor-versus-icon.** Win16's predefined cursor and icon
ordinals share one range, both `LoadCursor` and `LoadIcon` reach this stub, and the two
exports that would disambiguate are reached through **relocation chains, not fixed call
targets** — both wrappers disassemble identically, so there is nothing to read
statically. But the **guest says which it is** a moment later, when it puts the handle in
`WNDCLASS.hCursor` or `hIcon`. So `0xad` returns a **token** remembering the ordinal, and
the real `LoadCursorA`/`LoadIconA` happens at `RegisterClass`.

### ★★ `0x76` — RegisterWindowMessage

Notepad then showed its window and **exited anyway**, on another unnamed id — and named
it by what it passes:

```
notepad seg2:0x05d1  push ds / push 0x201   ; ds:0x0201 = "commdlg_FindReplace"
        seg2:0x05dd  or ax,ax / jne
        seg2:0x05e1  jmp 0x02cb             ; -> sub ax,ax / ret  ABANDON INIT
        seg2:0x05e4  ... ds:0x0215 = "commdlg_help", same pattern
```

Those are the two message names the common dialogs register. Answered against the OS's
own atom table — the entire point of a registered window message is that two programs
registering the same string get the same number, so our own table would agree with
nothing. Measured: `commdlg_help -> 0xc075`, `commdlg_FindReplace -> 0xc07b`, and Notepad
reaches its own message loop at `seg1:0x0c94`, the address its disassembly predicted.

### ★ `0x91` — RegisterClipboardFormat

Same argument, same table. CARDFILE went 0 → 2 classes of its own, WRITE 0 → 6.

---

## Part 4 — a window someone can sit in front of

### `wowidle.txt` — a blocked task can wait forever

`WOWMSG_WAIT_MS` was a hardcoded six seconds so an unattended run finishes. A real Win16
task waits forever, and a program sitting in `GetMessage` with its window on the desktop
is **not stuck, it is waiting for the user** — quitting it after six seconds makes it
impossible to type into. `wowidle.txt` is milliseconds; **0 means forever**; absent keeps
the measuring default.

`scripts/bm/wowlive.bat` launches a guest and **leaves it running** — no fixed wait, no
screenshot-then-kill, no completion marker, because nothing completes. Deliberately a
separate file from `wowrun.bat`, which exists to *measure*; a flag would have made both
ambiguous.

### ★★★ The guest's own resources, as real Win32 objects — `src/wow/wowres.h`

A Win16 program does not have to call `LoadMenu`: it can name the resource in its
`WNDCLASS` and let `CreateWindow` attach it, which is what **both** Notepad and SYSEDIT
do (`MENU=#0001`). This host was reading that field and throwing it away.

The bytes are in the application's own file, so the host reads them.

⚠ **Both layouts were confirmed against the data before any host code existed.**
`tools/ne/neres.py` decodes the same two structures offline:

```
&File   -> &New / &Open... / &Save / Save &As... / &Print / ... / E&xit
&Edit   -> &Undo Ctrl+Z / --- / Cu&t Ctrl+X / &Copy / &Paste / ...
&Search -> &Find... / Find &Next F3
&Help   -> &Contents / ... / &About Notepad...
```

A wrong offset does not accidentally spell *"&About Notepad..."*. That is why the decoder
was written as a tool first: the reading checks itself.

The icon is the same shape. Notepad's `GROUP_ICON` decodes to type=1, count=2, entries
32×32 1bpp/**304 bytes** id=1 and 32×32 4bpp/**744 bytes** id=2 — and the resource table
independently lists ICON 1 at 304 and ICON 2 at 752 (744 rounded to the alignment unit).
**Two tables agreeing is the confirmation.** The richest entry goes to
`CreateIconFromResourceEx`, which is a DIB decoder the OS already has.

⇒ `0xad` answers **kind 3** as well — a module's own resource — because its call site puts
the name at the same offsets as kind 1. The token carries the kind so `RegisterClass`
knows whether to ask the OS or the guest's file.

### ★ Focus, size and the caption

Notepad puts the caret where it belongs by handling `WM_SETFOCUS` with `SetFocus(its edit
control)`, and this host delivered neither. `wowwin_proc` now forwards
`WM_SETFOCUS`/`WM_KILLFOCUS`/`WM_SIZE`, and `SetFocus` calls the OS's. Measured: Notepad
receives `WM_SETFOCUS`, calls `SetFocus(0x0160)`, then `InvalidateRect` and `MoveWindow`
on its edit control off `WM_SIZE`.

⚠ **Those two are SENT on real Windows and are POSTED here** — the guest sees them at its
next `GetMessage`. Delivering them synchronously means re-entering the guest from inside a
Win32 callback, which needs a nested run this host has not built. Written down rather than
discovered.

`SetWindowText` went in so a window has the name its program gave it.

### "Not responding" now has an instrument rather than a theory

The windows belong to the exec thread, so the question is whether that thread is pumping —
and one heartbeat line every two seconds while blocked says how many Win32 messages it has
dispatched. Measured: it is pumping and dispatching. **Whether XP still flags the window
needs a human at the machine**; it has not been reproduced under instrumentation.

---

## Part 5 — three instrument defects, all the same family

Every one of them is a tool reporting success while doing nothing:

1. **`echo 0>file` is not "write 0".** `0>` redirects handle 0, so the launcher wrote an
   **empty** file, the host kept its six-second default, and the script reported success.
2. **`copy ... >nul` failed silently.** `wowlive.bat` leaves a host *running*, so the next
   invocation found the previous one holding `C:\ntvdmex\ntvdmhost.exe` — and a run
   measured the OLD binary and reported "no change" about a fix that was not in the file.
   It kills, waits, checks `errorlevel` and prints both directory listings now.
3. **`wowtriage.sh` never deployed.** Same trap, same session.
4. And the knob's own startup log line never reached the log; the setting is announced at
   the **point of use** instead.

---

## Part 6 — the share

**111 MB archived** to `archive/2026-09-03/` — a 13 MB `doomhost.log`, 13 MB of
`screenshots/`, ~85 result logs, old BMPs, one-off probe outputs, and a **stale root
`ntvdmhost.exe` from 18 August** (the live one is in `bm/`; that stale copy was exactly
the "deployed the wrong exe" hazard). The keep-list was derived **from `main.c`**, not from
memory, so every knob and flag the host reads survived along with every `.bat`, the guest
binaries and the disk images. Verified afterwards with a baseline run.

⚠ One 61-byte file (`w16exp1.txt`) has an open handle on the XP side and refused to move.

---

## Measured

| | result |
|---|---|
| **baseline** (switches off) | `270 / 46 / 122 / 96 · 9·222·39 · 0001:229C` |
| **SYSEDIT** (frontier) | `622 / 152 / 308 / 145`, 8 windows, 4 MDI children, 4 EDIT texts, **its own 25-item menu and its own icon**, 0 message boxes, `ExitKernelThunk(0)` |
| **Notepad** | its window, menu, icon and caption; reaches its message loop; **stays running** |
| **Doom** | all eleven startup stages, 3.51 MB |

⚠ **SYSEDIT's numbers moved and it has a reason**: it names a menu and an icon in its
classes too, and now gets both. Its behaviour is identical.

---

## ▶ RESUME HERE

### 1. THE FRONTIER IS STILL GDI — but there is a nearer cluster first

Nothing draws its own client area yet, and that is what MS Paint needs: `WM_PAINT`
forwarded, `BeginPaint`/`EndPaint` on the real window, and **GDI's 367-stub id space,
dispatched nowhere at all**. ⚠ `DefWindowProc` currently validates the paint region, so
there is no repaint storm; the moment `WM_PAINT` is forwarded the guest MUST reach
`BeginPaint`/`EndPaint` or Win32 regenerates it forever.

### 2. What a run will ask for next, in the order the guests ask

- **`MoveWindow` (`0x38`)** — Notepad calls it on every `WM_SIZE` and nothing happens, so
  its edit control does not follow the window. Small, and immediately visible.
- **`WM_COMMAND` from the menu.** The menu is real, so clicking it produces a real Win32
  `WM_COMMAND` that `wowwin_proc` drops. Forwarding it is what makes File/Open *do*
  something — and then `DialogBox` becomes the next wall.
- **`DefWindowProc`/`DefFrameProc`/`DefMDIChildProc` as Win16 services** (`0x1bd`,
  `0x1bf`) — call the real ones on the real window.
- **`0xad` for PROGMAN** — it stops on a kind this host does not answer; read that site.
- **SOL's "Out of memory"** — a different failure entirely, and unread.

### 3. Closed this session

- **Only SYSEDIT could be tested.** Twelve Windows 3.11 guests are on the box, with a
  script that says how far each gets.
- **Notepad exits before creating a window.** It runs, with menu, icon and caption.
- **A Win16 task quits after six seconds.** `wowidle.txt`; 0 = forever.
- **The guest's own resources are unreachable.** `wowres.h` reads them out of its file.
- **"Not responding" is a theory.** It is a heartbeat.

### How to drive it

```bash
touch /private/tmp/xpshare/wowsched.txt /private/tmp/xpshare/wowcall.txt   # both needed
ARCHIVE=build/wowruns ./scripts/bmwow.sh                       # SYSEDIT, measured
TARGET='C:\WIN16\NOTEPAD.EXE' ./scripts/bmwow.sh --no-deploy   # any guest
./scripts/wowtriage.sh                                         # all twelve, one line each
python3 tools/bmp2png.py /private/tmp/xpshare/wow_shot1.bmp out.png --scale 2
```

- ★ **To leave a guest running and interact with it**, from the Mac:
  ```bash
  printf 'exec cmd /c "C:\\...\\ntvdmex\\wowlive.bat"\r\n' > /private/tmp/xpshare/control.txt
  ```
  It writes `wowidle.txt`=0 so the guest never times out, and does **not** kill anything.
  Stop it from the tray icon, the guest's own window, or `kill` through controld.
- ⚠ Always re-run with the switches OFF and confirm `270 / 46 / 122 / 96 · 9·222·39`.
- ⚠ `rigshot list` still writes an empty file — it is a GUI image, so its stdout does not
  redirect. **The screenshot is the evidence.**

### Ruled out — do not re-try

Everything in [session 42's list](session-42.md#ruled-out--do-not-re-try) still holds, plus:

- **XP's `expand.exe` on the Windows 3.1x disks.** It does not understand KWAJ and reports
  success anyway.
- **Reading USER's `LoadCursor`/`LoadIcon` wrappers statically** to tell cursor from icon.
  They are relocation chains and disassemble identically; the guest tells you instead.
- **A shared `HMENU`.** Win32 will not let two windows share one; build per window.
- **A `%%~nf.tmp` temp name.** Two resources with one stem collide.
