# Session 71 — The text-mode application class -- QBasic's three symptoms were six host defects

> Session 71. Split out of `docs/STATE.md` on 2026-09-23.

> Verbatim from `docs/STATE.md`. Nothing has been edited or corrected,
> including conclusions a later session refuted — same rule as the rest of
> this archive. See [`README.md`](README.md).

## ★★★★ SESSION 71 (2026-09-14, morning) — **THE TEXT-MODE APPLICATION CLASS: QBASIC'S THREE SYMPTOMS WERE SIX HOST DEFECTS, ALL OFF-VM TESTABLE.**

**HEAD `b7ccd6e`, NOT pushed. Battery green (input 60, video 162; full suite green).
★ DEPLOYED with the user's go: rig = `836e9440…`; rollback `bm\ntvdmhost_prev.exe` =
the confirmed `f79d954` (`bf9534a9…`). Awaiting the user's QB re-test. Deadline: the
17th — three days.**

**Midday addendum — the user's "still not working" was measured on the OLD build
(never deployed), and its log named two more defects, now fixed in `b7ccd6e`:**
7. **IRQ1 stayed in service after the first key** (`keyirq=1` across ~19 presses). QB's
      hook EOIs only the keys it swallows and chains to the BIOS for the rest, leaving the
      EOI to the BIOS handler; our INT 09h BOP arm never sent one. It does now, exactly as
      the INT 08h arm EOIs IRQ0.
8. **The INT 33h event handler (0Ch) is now CALLED** (`mouse_cb_try`; return stub BOP
      0x35 at `DOS_HDLR_SEG:005C`; context saved host-side; one in flight; 2 s timeout;
      PM handlers counted as `cb_pm`, not called). QB called 03h twenty times in a whole
      run — Microsoft's text UIs take all their mouse input through the callback.
⚠ Also seen: old-build QB detected CGA (zero BDA video bytes) and sat in its
snow-avoidance `cli`/`3DA`-per-character loop. ⚠ `irq0_inj=` counts only exec-loop
deliveries; the async path delivered the rest (`gap_ms[]` showed a healthy 18 Hz) —
do not read a low `irq0=` as a stalled timer.

The user's report on QB.EXE 4.5 (`demos/qb45` on the share): a graphical mouse
pointer drawn over a text screen, no menu opened, and nothing could be typed. None
of it was QBasic's. Each was read from the code (and the guest binary) and pinned as
a battery check before the fix:

1. **Typing (`vdd_input.c`)** — QB's INT 09h hook (`1DDB1h`) does `in al,60h`, looks
      at the byte, and for every ordinary key chains to the BIOS via `int 0EFh` (the
      saved vector). On an 8042 the BIOS's own `in al,60h` reads the SAME byte again;
      our FIFO had popped it on the hook's read, so the BIOS arm found the FIFO empty,
      translated nothing, and the ring at 0040:001E stayed empty. Now the BIOS arm
      serves that byte once from `sc_last` (`sc_bios_owed`; STAGE2 `owed=`), superseded
      by any newer byte. Every INT 09h hook that peeks the port and chains — Turbo
      Pascal's CRT unit, most TSRs — had the same gap. Input battery T9.
2. **Alt menus (`vdd_input.c`)** — the translation never consulted Alt: Alt+F arrived
      as `AH=21 AL='f'` where the BIOS stores `2100h`, so every editor's accelerator
      typed a letter. Replaced the two ASCII columns with the IBM four-column table
      (plain/Shift/Ctrl/Alt, F-keys incl. F11/F12, Ctrl+arrows `7300h` etc., enhanced
      Alt+grey codes); CapsLock inverts Shift for letters only, NumLock for the keypad
      only. Input battery T10. Also 0040:0096 bit 4 (enhanced keyboard present) is set.
3. **The pointer (`main.c` present path + `vdd_video_text_cursor`)** — in a text mode
      the driver has no pixels; it rewrites the ATTRIBUTE of the cell under the pointer
      (INT 33h 0Ah masks, defaults `77FFh`/`7700h`). The host stamped its 16x16 arrow into
      the text frame regardless. Now text modes redraw the cell through the masks (0Ah
      BX=0 stores them; BX=1 falls back to the default and is counted).
4. **Mouse menus (`main.c` INT 33h)** — the driver's text screen is 640x200 whatever
      the font, so apps do `row = DX/8`. We returned the 400-line frame row: every row
      doubled, a menu-bar click landed two rows down. `i33_vy/i33_py` scale Y in text
      modes (03h/04h/05h/06h/08h).
5. **Text rendering (`vdd_video.c`)** — attribute bit 7 was always masked off (no
      bright backgrounds after `1003h BL=0`, no blink with it on); `1112h` — THE 50-line
      call — cleared the user font and changed nothing else (cell height is now
      per-state: 8/14/16, rows = 400/cell_h, cursor emulation and INT 43h answer follow
      it; `1111h`/`1114h`/`1x` user fonts likewise); CRTC `0A/0B/0E/0F` (cursor
      shape/address, how every CRT unit moves the cursor) fell into `default:`; the BDA
      display fields 0449..0489 were never written (rows-1 at 0040:0084 read 0 — a
      one-row screen to anything that sizes itself from it); 40-column text was drawn
      at stride 640 into a 320-wide frame. Video battery T21 a–f.
6. **Alt held forever (`main.c`)** — Windows delivers a key's UP to whichever window
      has focus when it is released, so Alt+Tab away left the guest with Alt down.
      `WM_KILLFOCUS` now releases the modifiers we pushed.

**Evening addendum — THE 17th IS A PORTABLE ZIP (see `docs/PLAN-17th.md`).**
Done today: `NTVDMEX_DIR` is derived from the host's own path (parent of `bm\`;
rig-verified: `STAGE0: root=[...\ntvdmex\]`), `package/` holds install/uninstall/
status/smoke `.bat` + README, `scripts/package.sh` builds `dist/ntvdmex-<date>-<sha>.zip`
(host + selftest.com + empty cfg\ out\; NO Win16 system files -- the WOW half uses
XP's own from system32). A copy sits on the share at `dist\` for the by-hand
fresh-folder install test. Also today: mouse callbacks delivered at our stubs, the
return stub moved 0x5C->0x12 (0x5C was `DPMI_RAW2PM_OFF`, planted later -- "DOS
terminate on the first mouse move"), 8042 transfer hold (900 us), Win key never
forwarded, LASTDRIVE=26 + CDS in a top-of-chain block + real AH=0Eh/19h. PCem 17
(macOS app + source) is under `./pcem` (gitignored) for the BIOS/VGA oracle.
Rig = `b85a8a53…`; rollback = `bf9534a9…` (f79d954). Branch pushed.

**▶ NEXT (needs the user):** deploy on their go, then by hand in QB.EXE: type in the
edit window; Alt then F opens File; click File in the menu bar; the pointer is an
inverted cell; Options > Display for 50 lines (`1112h`). Then edit.com (XP's
`system32\edit.com` — a different code base, 43/50-line and mouse paths both used).
⚠ Re-run Skyroads after (shared keyboard path, standing rule). Then the same
read-the-binary pass over the next text-mode guest rather than another game.

---
