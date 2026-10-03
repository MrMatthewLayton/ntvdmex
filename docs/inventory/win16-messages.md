# Win16 messages — every message the host touches, by direction

> **Hand-kept** (s89), read out of `src/wow/` line by line. Its partner is the generated
> call table, [`win16-surface.md`](win16-surface.md). Most of session 89's bugs were not
> missing calls: they were messages delivered late, delivered untranslated, or never
> delivered. A call can be "handled" and still wrong because a message behind it is.
> Update this file in the same commit as any change to message handling.

Win16 and Win32 share most `WM_*` numbers below `0x0400`. They differ in three ways, and
every row below is one of them:

1. **Control messages are renumbered.** Win16 puts EDIT, LISTBOX, COMBOBOX and BUTTON
   messages at `WM_USER+n` (`0x0400+n`). Win32 moved them to `0xB0+n` (EM), `0x180+n` (LB),
   `0x140+n` (CB) and `0xF0+n` (BM). The same Win16 number means different things for
   different control classes.
2. **Parameters are packed differently.** Win16 has a 16-bit `wParam` and puts more in
   `lParam`, e.g. WM_COMMAND, WM_HSCROLL/VSCROLL, WM_MENUSELECT, WM_ACTIVATE,
   WM_CTLCOLOR and EM_SETSEL. Pointers are 16:16 far pointers, and handles are Win16
   handles or the host's tokens.
3. **Sent vs posted.** Real Windows SENDS many messages, so the procedure runs before the
   sender's call returns. This host could only POST until `wow_call16_sync()` (s89).
   Several rows are still posted because of that history.

Legend: ✅ translated correctly as far as measured · ⚠ handled with a known defect ·
❌ not handled (falls to "not implemented, answered 0" or never reaches the guest).

---

## A. Guest → window (SendMessage / PostMessage / SendDlgItemMessage / CallWindowProc)

**Routing.** If the target window has a 16-bit procedure, the message goes to it
unchanged, which is correct. Only system-class controls (EDIT, LISTBOX, COMBOBOX,
BUTTON, STATIC, SCROLLBAR, MDICLIENT) reach `wowuser_defproc`, which is where the
translation lives (`wowuser.h:~2816–3233`).

| Win16 | message | status | notes |
|---|---|---|---|
| `0x000C` | WM_SETTEXT | ✅ | 16:16 → linear, `SetWindowTextA`. **And SetWindowText on a window with its own procedure SENDS it there first** (s89 -- Sound Recorder's buttons) |
| `0x000D` | WM_GETTEXT | ✅ | writes into the guest buffer, bounded by wParam |
| `0x000E` | WM_GETTEXTLENGTH | ✅ | also refreshes the EM handle block |
| `0x0300–0x0304` | WM_CUT/COPY/PASTE/CLEAR/UNDO | ✅ | raw to the real control |
| `0x0220` | WM_MDICREATE | ✅ | MDICREATESTRUCT rebuilt; WM_CREATE sent to the guest |
| `0x0221–0x0230` | other WM_MDI* | ❌ | MDIDESTROY, MDIACTIVATE, MDITILE, MDICASCADE, MDIGETACTIVE… |
| `0x0030/0x0031` | WM_SETFONT / WM_GETFONT | ❌ | font token ↔ real HFONT |
| `0x0400+n` EDIT | EM_* | ⚠ | Accepted: 0 GETSEL, 1 SETSEL (packing fixed), 8–11, 17, 21–23, 25, 29. **Missing:** 2–7 GETRECT/SETRECT/SETRECTNP/SCROLL/LINESCROLL, 14–16, **18 REPLACESEL**, 19 SETFONT, **20 GETLINE**, 24 FMTLINES, 26 SETWORDBREAK, **27 SETTABSTOPS**, 28 SETPASSWORDCHAR, ≥ 0x41E |
| `0x040C/0x040D` | EM_SETHANDLE / EM_GETHANDLE | ✅ | answered against the guest's local heap |
| `0x0400+n` COMBOBOX | CB_* → `0x140+n` | ⚠ | n=0..24 all mapped; strings copied (max 255). **18 GETDROPPEDCONTROLRECT (struct) answered 0** |
| `0x0401+n` LISTBOX | LB_* → `0x180+n` | ⚠ | n=0..0x22 mapped. **Struct/array ones answered 0:** SELITEMRANGEEX, **GETSELITEMS**, **SETTABSTOPS**, ADDFILE, **GETITEMRECT**, anchor index |
| LB/CB | GETTEXT on owner-draw without HASSTRINGS | ✅ (s89, #304) | the item data is written into the guest's buffer (was: the 16:16 pointer passed as flat) |
| `0x0400–0x0404` BUTTON | BM_GETCHECK/SETCHECK/GETSTATE/SETSTATE/SETSTYLE → `0xF0+n` | ✅ (s89, #301) | keyed on the class; not yet exercised by a shelf program |
| `0x0400–0x0401` STATIC | STM_SETICON/GETICON → `0x170+n` | ❌ | |
| SCROLLBAR | SBM_* (Win16 has none — uses SetScrollPos etc.) | — | the calls, not messages |
| `0xFFFF` | broadcast (HWND_BROADCAST) | ❌ | SendMessage answers 0; PostMessage does not check the hwnd |
| | message posted to a window with no 16-bit procedure inside a modal loop | ⚠ | dropped silently (`wowdlg.h:574`) |

## B. Real window → guest procedure (the host delivers)

From `wowwin_proc` (`wowwin.h`) unless noted.

| Win16 | message | delivery | status | notes |
|---|---|---|---|---|
| `0x0001` | WM_CREATE | sent | ✅ | CREATESTRUCT (34 bytes) on the guest stack; -1 fails the create. **Template-built application controls get it too** (s89, #302 -- Sound Recorder's SButton) |
| `0x0002` | WM_DESTROY | **sent** (nested) | ✅ (s89, #305) | to the window, then each child with its own procedure, all still alive; posted only as a fallback. Charmap now saves its font |
| `0x0005` | WM_SIZE | **posted** | ⚠ | should be sent |
| `0x0007/0x0008` | WM_SETFOCUS / WM_KILLFOCUS | **posted** | ⚠ | wParam now mapped to the guest's hwnd (s89, #303); still posted, not sent |
| `0x000F` | WM_PAINT | posted | ✅ | the host runs Begin/EndPaint; the guest's BeginPaint gets the rectangle and a DC clipped to it (#287) |
| `0x0010` | WM_CLOSE | posted | ✅ | |
| `0x0014` | WM_ERASEBKGND | sent from the guest's BeginPaint | ✅ | DC token; the default applies the class brush (s89) |
| `0x0019` | WM_CTLCOLOR | **sent** (`wow_call16_sync`) | ✅ | built from Win32 `0x0132–0x0138`; brush token mapped back; 3.x defaults (s89) |
| `0x0100–0x0102` | WM_KEYDOWN/KEYUP/CHAR | posted | ✅ | |
| `0x0104/0x0105` | WM_SYSKEYDOWN/UP | posted | ✅ | the OS default also runs (menus) |
| `0x0106/0x0103/0x0107` | WM_SYSCHAR / WM_DEADCHAR / WM_SYSDEADCHAR | — | ❌ | |
| `0x0110` | WM_INITDIALOG | sent | ✅ | modal and modeless; **modeless: sent synchronously while the dialog is still hidden, then shown** (s89, #302 -- Charmap's TT mark) |
| `0x0111` | WM_COMMAND | posted | ✅ | repacked: Win16 `wParam=id, lParam=MAKELONG(hwnd16, code)` |
| `0x0112` | WM_SYSCOMMAND | — | ⚠ | only SC_KEYMENU/SC_MOUSEMENU (menu replay); the others go straight to the OS |
| `0x0113` | WM_TIMER | posted | ✅ | lParam = TIMERPROC; DispatchMessage calls it |
| `0x0116/0x0117` | WM_INITMENU / WM_INITMENUPOPUP | posted | ✅ | menu token |
| `0x011F` | WM_MENUSELECT | — | ❌ | status-bar help text (Write, Paintbrush) |
| `0x0114/0x0115` | WM_HSCROLL / WM_VSCROLL | **sent** (nested) | ✅ (s89, #300, rig-verified on Paintbrush) | repacked: Win16 `wParam=code, lParam=MAKELONG(pos, hwndCtl16)` |
| `0x002B` | WM_DRAWITEM | **sent** (nested) | ✅ (s89, #302; Charmap's font list = stock) | DRAWITEMSTRUCT converted (26 bytes Win16, DC token) on the guest stack |
| `0x002C` | WM_MEASUREITEM | **sent** (nested) | ✅ (s89, #302) | width/height copied back |
| `0x0039/0x002D` | WM_COMPAREITEM / WM_DELETEITEM | **sent** (nested) | ✅ (s89, #302; not yet exercised) | structures converted; COMPAREITEM's answer = return value |
| `0x0006/0x001C` | WM_ACTIVATE / WM_ACTIVATEAPP | — | ❌ | Win16 packs `lParam=MAKELONG(hwnd, fMinimized)` |
| `0x0003` | WM_MOVE | — | ❌ | |
| `0x0018` | WM_SHOWWINDOW | — | ❌ | |
| `0x0024` | WM_GETMINMAXINFO | — | ❌ | MINMAXINFO is 10 POINTs of 16-bit values |
| `0x0083/0x0084/0x0085/0x0086` | WM_NCCALCSIZE/NCHITTEST/NCPAINT/NCACTIVATE | — | ❌ (by design for now) | Luna frames are kept (user decision); only a program that hooks its own non-client area would notice |
| `0x0210` | WM_PARENTNOTIFY | — | ❌ | |
| `0x0011/0x0016` | WM_QUERYENDSESSION / WM_ENDSESSION | — | ❌ | |
| `0xC000+` | registered `commdlg_FindReplace` | posted | ✅ (s89, #294, rig-verified on Notepad) | relayed to the owner with the guest's own FINDREPLACE pointer, flags copied back |
| `0x0233` | WM_DROPFILES | — | ❌ | DragAcceptFiles is answered, but the drop is not forwarded (`wowshell.h:669`) |
| `0x0200–0x0209` | mouse messages | posted | ✅ | moves coalesced |
| `0x00A0–0x00A9` | WM_NC* mouse | — | ❌ | |
| `0x0300–0x030F` | clipboard owner messages (RENDERFORMAT, DRAWCLIPBOARD…) | — | ❌ | |
| `0x0012` | WM_QUIT | from GetMessage | ✅ | |
| | TranslateMessage | — | ⚠ | makes no WM_CHAR; WM_CHAR comes only from the Win32 relay. TranslateMDISysAccel answers 0 |

**Everything not listed in B never reaches the guest:** `wowwin_proc` hands it to
DefFrameProc / DefMDIChildProc / DefWindowProc.

## C. The guest's defaults (DefWindowProc / DefDlgProc)

- **DefWindowProc** is USER's 16-bit code. It forwards to us (`0x6b`) only messages whose
  bit is set in `g_dwp_forward` (`wowuser.h:1798`): WM_CLOSE, WM_ERASEBKGND, WM_PAINT.
  A message joins the list once its parameters are translated, because 0x6b passes the
  rest raw to DefWindowProcA.
  - The `0x6b` WM_CTLCOLOR arm (`wowuser.h:8227`) **cannot be reached**: `0x0019` is not
    in the forward table. It is harmless (the answer would be 0 either way), but it
    misleads a reader.
- **DefDlgProc** calls the DLGPROC first; on FALSE `wowuser_dlg_default` runs:
  - WM_CLOSE → IDCANCEL.
  - WM_CTLCOLOR → 0.
  - WM_PAINT / WM_ERASEBKGND → COLOR_BTNFACE.
  - Everything else → DefWindowProcA, raw.
  - ❌ **No dialog keyboard defaults** on that path: no default button on Enter, no Esc,
    no Tab order. IsDialogMessage covers the common case: it copies the Win16 MSG raw into
    a Win32 MSG and calls IsDialogMessageA.
- **DefFrameProc / DefMDIChildProc** → the A versions, raw parameters.

---

## The message gaps, as work

Ordered by how many shelf programs are likely to show the gap. These are estimates until
a batch is measured against stock.

| # | gap | kind | who shows it |
|---|---|---|---|
| ~~M1~~ ✅ | **WM_HSCROLL / WM_VSCROLL** relayed with the packing converted | translation | any program with its own scroll bars (Write, Cardfile, Charmap, Paintbrush, Terminal) |
| ~~M2~~ ✅ | **BM_*** on BUTTON controls (`0x400+n → 0xF0+n`) | translation | every dialog with check boxes or radio buttons set by message |
| ~~M3~~ ✅ | **Owner-draw**: WM_MEASUREITEM / WM_DRAWITEM / WM_DELETEITEM / WM_COMPAREITEM, sent through `wow_call16_sync` with the structures converted | structural | Charmap? File Manager, Control Panel-type lists, #288 |
| M4 (½ ✅) | WM_SETFOCUS / WM_KILLFOCUS **wParam mapped to the guest's hwnd** — and sent rather than posted, now that sending is possible | bug | anything that checks which window lost focus |
| M5 | EM_REPLACESEL / EM_GETLINE / EM_SETTABSTOPS / EM_SETFONT / EM_LINESCROLL / EM_GETRECT | translation | Notepad Find/Replace (#285), Write, Terminal, Sysedit |
| M6 | LB_GETSELITEMS / LB_SETTABSTOPS / LB_GETITEMRECT / CB_GETDROPPEDCONTROLRECT (array/struct) | translation | multi-select lists, tabbed lists (Winfile, Program Manager) |
| ~~M7~~ ✅ | LB/CB GETTEXT on owner-draw without HASSTRINGS: the 16:16 pointer is passed as flat | **bug** (memory write to a wrong address) | owner-draw lists |
| M8 | WM_SETFONT / WM_GETFONT to system controls | translation | programs that set a control's font (Charmap, Terminal) |
| M9 | WM_MENUSELECT, WM_ACTIVATE(APP), WM_SHOWWINDOW, WM_MOVE, WM_GETMINMAXINFO, WM_SYSCHAR | translation | status-bar help, activation-aware programs, minimum sizes (Clock) |
| M10 (½ ✅: WM_DESTROY) | WM_SIZE / WM_DESTROY **sent**, not posted | ordering | layout that must happen before the next call returns |
| M11 | Dialog keyboard defaults on the DefDlgProc path (Enter / Esc / Tab) | structural | every dialog without IsDialogMessage in its loop |
| M12 | WM_DROPFILES forwarded | translation | File Manager → Notepad drag-and-drop |
| M13 | Remaining WM_MDI* | translation | Program Manager, File Manager, Sysedit |

Also: the stale comment in `wowwin.h` that says the paint DC is not clipped was corrected
in this commit (#287 clips it).
