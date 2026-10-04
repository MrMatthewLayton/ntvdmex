# Inventory — keyboard BIOS: INT 16h, INT 09h and the BDA keyboard fields

**Spec:** IBM PC/AT and PS/2 BIOS Technical References (the INT 09h/INT 16h listings,
including the K1S compatibility translation); Ralf Brown's Interrupt List (INT 16h,
INT 09h, BDA `0040:0017`–`0040:0097`). ⚠ **Not held in the repo** —
[`../ref/SOURCES.md`](../ref/SOURCES.md) names them. No `docs/ref/keyboard.md` yet.
**The controller and the keyboard device** (ports `60h`/`64h`/`92h`, the 8042 command
set, keyboard-side ACKs) are **[kbc.md](kbc.md)**. This file is the firmware on top.
**Our implementation:** `src/vdd/vdd_input.c` (788 lines), `src/vdd/vdd_input.h`; the
INT 09h/16h stubs and their BOP arms in `src/host/main.c`; host keys enter at
`host_key_scancode` (`main.c:6598`).
**Off-VM:** `tools/dostest/input_test.c`, `kbdact_test.c`, `prtsc_test.c`. **DOS probes:** `tools/dostest/p_kbd.asm`, `p_kbd2.asm`, `p_kbd3.asm` (#244/#274).
**Oracles:** MS-DOS 6.22 under QEMU (SeaBIOS — a reimplementation, so **provisional** for a
BIOS row), PCem with a genuine AMI 486 BIOS (**oracle**), DOSBox-X.
**Marked:** 2026-10-01, **from the code**, with citations. Carried over from
`docs/PARITY.md` (retired 2026-09-23) and re-marked into the current vocabulary.
⚠ `src/host/main.c` line numbers drift; search for the symbol beside each one.

⚠ **How the verification column was set.** `p_kbd` was compared against 6.22/QEMU
(32 fields, all AGREE, 2026-09-15). For a BIOS row that is SeaBIOS's opinion, so those
rows are **provisional**. In s84 (#188, `9a91149`) `p_kbd` was asked of PCem; the commit
names the rows that changed (`16.01.enh`, `16.09.support`). The rest are kept
**provisional** until a PCem run is read row by row.

---

## Headline

**The ring and the INT 16h read/peek/push calls are right and measured. What is
missing is everything the BIOS INT 09h handler does besides "store a key":** the
extended flag byte `0040:0018`, the LED byte, Ctrl-Break, Pause, Print Screen, SysReq,
Alt+keypad entry, the Insert toggle, and the INT 15h `AH=4Fh` intercept. And the
enhanced keyboard's **grey-key `E0` forms are never produced** — so `AH=10h/11h`, the
calls that exist to tell a grey arrow from a keypad arrow, cannot.

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 INT 16h functions | 14 | 10 | — | — | 1 | 3 |
| §2 BDA keyboard fields | 10 | 8 | — | — | 1 | 1 |
| §3 INT 09h BIOS handler | 15 | 12 | 2 | — | — | 1 |
| **Total** | **39** | **30** | **2** | **—** | **2** | **5** |

★ **#244 / #274 (2026-10-04)** closed the INT 15h `AH=4Fh` intercept, Pause from the
host, Alt+keypad (`0019h`), the ring bounds (`0080h`/`0082h`) and put a BIOS print-screen
routine behind INT 05h. **All from the documentation, none measured yet** — `p_kbd3` is
the probe that asks the oracles (see each row).

---

## 1. INT 16h

The vector is planted at `DOS_HDLR_SEG:0028` as `BOP 16h; IRET` (`main.c:26985`). V86
reaches the VDD through the BOP arm (`main.c:28980-28995`); protected mode through the
PM BOP arm (`main.c:22240-22257`). Dispatcher: `int16` (`vdd_input.c:664-738`).

| AH | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `00h` | read key, 83-key form | **IMPL** | `vdd_input.c:675-679`; IBM's K1S filter `kb_compat` (`:648-659`) discards enhanced-only codes and rewrites `E0` forms. Empty ring: V86 leaves EIP on the BOP so the guest re-executes and keeps taking IRQs (`main.c:28990`); PM waits on `g_key_event` (`main.c:22242-22249`) | provisional (`16.00.read`, `.head`, `.drained`, `.arrow`); input_test `:72-76`, `:342-345` |
| `01h` | check key, 83-key form | **IMPL** | `:684-690`; a discarded code is **consumed**, head moved on (the PCem answer) | **oracle** for `16.01.enh` (PCem, #188); provisional for `16.01.peek`, `.nocons`, `.empty` |
| `02h` | shift flags → AL | **IMPL** | `:695-697`, reads `0040:0017` | provisional (`16.02.shift`); input_test `:133` |
| `03h` | typematic rate / delay | **N/A** | `:703-710`: answered `CF=0`, nothing stored. The host OS owns key repeat (the host repeats keys itself, `main.c:6611`) and the BIOS keeps no readable copy. `AL=06h` (get) is not claimed by `AH=09h` | provisional (`16.03.typematic`) |
| `04h` | keyclick on/off | **N/A** | PCjr / Convertible only; not in the AT or PS/2 BIOS. Falls to `default:` (`:734-736`) | — |
| `05h` | store keystroke CH:CL | **IMPL** | `:711-720`; `AL=1` when full (`vdd_input_push` `:19-42`) | provisional (`16.05.push`, `.tail`, `.readback`, `.full`); input_test `:295-313` |
| `09h` | supported-function mask | **IMPL** | `:721-727`, `AL=B1h` | **oracle** (PCem + DOSBox-X, #188); input_test `:322` |
| `0Ah` | keyboard ID → BX | **IMPL** | `:728-733`, `41ABh` (MF2 behind a translating 8042), as `09h` bit 4 promises | untested — never compared; input_test `:325` |
| `10h` | read key, enhanced | **IMPL** | `:680-683`, unfiltered. Same blocking arms as `00h`. ⚠ But see §3: the grey-key `E0` forms it should return are never stored | provisional (`16.10.enh`); input_test `:151-152` |
| `11h` | check key, enhanced | **IMPL** | `:691-694` | provisional (`16.11.enh`, `.empty`); input_test `:144-148` |
| `12h` | extended shift flags → AX | **IMPL** | #254: `AH` is built in its own layout from `0040:0018` (bits 0/1/4–6; SysReq bit 2 → `AH` bit 7) and `0040:0096` bits 2/3 (right Ctrl/Alt), which INT 09h now maintains. It used to copy `0018` whole | **oracle** (`p_kbd2`: the probe writes the BDA bytes and asks) |
| `20h`–`22h` | 122-key keyboard calls | **N/A** | not claimed (`09h` bit 6 clear); the AMI BIOS does not offer them | — |
| — | unknown `AH` | **IMPL** | `:734-736`: `ZF=1`, registers untouched — never a phantom key | input_test `:155` |
| — | the wait loop calls INT 15h `AH=90h` (device busy) | **MISS** | the AT BIOS calls it while `00h`/`10h` waits so a multitasker can switch tasks; ours never does | — |

## 2. BDA keyboard fields

The BDA is guest memory at `0040:0000`; the VDD writes it through `g_in.bda`
(`main.c:27845`). Offsets are named in `vdd_input.h:28-33`.

| Offset | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `0017h` | shift flags | **IMPL** | bits 0–6 by `bios_translate`; Ctrl/Alt (bits 2/3) follow BOTH sides' held bits (#254: releasing left Ctrl while right is held no longer clears Ctrl); lock keys toggle once per press, not per typematic repeat; bit 7 Insert toggles on an Insert press (#254) | provisional (`16.02.shift`); input_test T12 |
| `0018h` | extended shift flags (left Ctrl/Alt held, SysReq, Pause, keys held) | **IMPL** | #254: all eight bits maintained by `bios_translate` | input_test T12; `p_kbd2` (via `AH=12h`) |
| `0019h` | Alt+keypad accumulator | **IMPL** | #274: `bios_translate` — Alt + a non-E0 keypad digit does `0019h = 0019h*10 + d` (a byte, wraps), stores nothing; the last Alt up stores `00xxh` if non-zero and clears it; any other key under Alt clears it. IBM AT TR KB_INT, RBIL — unmeasured (no oracle can hold Alt) | input_test T13(b) |
| `001Ah`/`001Ch` | ring head / tail | **IMPL** | `:9-67`; a pointer pair the guest has scribbled on is reset rather than trusted (`:27-31`) | provisional (`bda.buffer`) |
| `001Eh`–`003Dh` | the 16-slot ring | **IMPL** | `BDA_KB_START`/`END` (`vdd_input.h:30-31`); 15 usable slots, wrap at the end (`:16-17`) | provisional (`ring.wrap.*`); input_test `:84-86` |
| `0071h` | Ctrl-Break flag (bit 7) | **IMPL** | #254: set by Ctrl-Break | input_test T12 |
| `0072h` | reset flag (`1234h` = warm boot) | **N/A** | a VDM is never rebooted; nothing reads it | — |
| `0080h`/`0082h` | ring start / end pointers | **IMPL** | #274: `vdd_input_reset` writes POST's `001Eh`/`003Eh`; push/pop/peek (so INT 09h and every INT 16h call) read them each time (`kb_bounds`). A pair that cannot be a ring (odd, inverted, < 2 slots) falls back to `001E`/`003E`. Was: nothing read or wrote them | input_test T13(a); `p_kbd3 kbuf.small.*` (a 4-slot ring at `001E`–`0026`: full at 3, wraps at `0026h`) — **owed** an oracle run |
| `0096h` | keyboard mode / type | **IMPL** | bit 4 (enhanced) at reset; #254: bits 0/1 (last code E1/E0) and 2/3 (right Ctrl/Alt held) maintained | provisional (`bda.enhanced`); input_test T12 |
| `0097h` | LED / keyboard status | **MISS** | no writer; LED commands to the keyboard are not sent either ([kbc.md](kbc.md) §5) | — |

## 3. The BIOS INT 09h handler

Default vector `DOS_HDLR_SEG:004C` = `BOP 09h; IRET` (`main.c:25790`, planted `:26997`).
V86 arm: `main.c:29005-29020` (consume, then the EOI the BIOS ends with). PM default
handler reflected to it: `main.c:22303-22319`.

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| Consume the presented byte and translate it | **IMPL** | `vdd_input_bios_consume` `vdd_input.c:453-469` | untested |
| A guest hook that read `60h` itself and then chains: the BIOS re-reads the **same** byte | **IMPL** | `sc_bios_owed` (`:459-464`, set at `:560`) | untested |
| The EOI at the end of the handler | **IMPL** | `vdd_pic_eoi(&g_pic, 1)` (`main.c:29018`); measured as the QBasic keyboard fix | by hand (QBasic) |
| Make / break, the `E0` prefix, the fake shifts `E0 2A`/`E0 AA` | **IMPL** | `:394-406` | input_test |
| Modifier and lock state into `0040:0017` | **IMPL** | `:404-412` (Insert excepted, §2) | provisional |
| Four columns (plain / Shift / Ctrl / Alt), Caps for letters, NumLock for the keypad | **IMPL** | `sc_key` (`:154-246`), precedence `:419-443`. From the IBM table, not from a machine | untested; input_test T10 (`:187`) pins the table |
| Grey keys: Alt forms (`9B00h` …) | **IMPL** | `sc_ext_alt` (`:250-260`) | untested |
| Grey keys: plain / Shift / Ctrl **`E0` forms** (`4BE0h`, keypad Enter `E00Dh`, keypad `/` `E02Fh`, Ctrl+grey Left `73E0h`) | **IMPL** | #254: `sc_ext_plain`/`sc_ext_ctrl`; `kb_compat` folds them back for `AH=00h/01h`, and DOS's CON reads through `vdd_input_dos_key` (same fold), so INT 21h input is unchanged | input_test T7/T10/T12 — ⚠ not oracle-measured: a headless oracle cannot press a key |
| Keyboard layouts (UK / German / French) | **IMPL** | #136, `:266-343`, `:430-442`; dead keys not composed. An extension, not a BIOS unit | untested |
| INT 15h `AH=4Fh` keyboard intercept | **PART** | #244: `bios_kbdact.asm` `k4f` — `stc / int 15h` with `AL` = the byte; CF=1 → the (possibly changed) `AL` is translated; CF=0 → swallowed, the BIOS's own EOI. Made **only while IVT[15h] is hooked**: our default answers CF=1 with AL untouched, so the call would change nothing and cost two VM exits per byte on the latency-fragile path. `C0h` feature 1 bit 4 now set (`74h`). ⚠ PART: the **PM-reflected** default INT 09h (a DPMI client with no PM keyboard hook, `main.c` "reflected to the BIOS (IVT is ours)") consumes host-side and does not call it | kbdact_test, input_test T13(c); `p_kbd3 int09.4f.*` (injects with 8042 `D2h`) — **owed** |
| Ctrl-Break → INT 1Bh, `0000h` in the ring, `0071h` bit 7 | **IMPL** | #254: Ctrl + `46h` (E0 or not): ring emptied, `0000h` stored, `0071h` bit 7, then INT 1Bh run as guest code (`bios_kbdact.asm`) — skipped if IVT[1Bh] lands on a ROM BOP | input_test T12, kbdact_test |
| Pause (`E1 1D 45`) → BIOS pause loop, `0018h` bit 3 | **IMPL** | #254: the BIOS side (E1 sequence, Ctrl+NumLock, the spin loop with IF=1, the next keystroke ends it and is discarded). #274: **the host now sends it** — `host_key_special` / `vdd_input_host_key_bytes`: Win32 scan `45h` *without* the extended bit is Pause → `E1 1D 45 E1 9D C5` on the press, nothing on the release, no typematic. Ctrl+Break (`46h` extended) → `E0 46 E0 C6` on the press likewise (it used to repeat INT 1Bh at the typematic rate while held). ⚠ The Win32 half (Pause = `45h` not extended) is from documentation, unmeasured on the rig | input_test T12/T13(e), kbdact_test; by hand: Pause in `edit.com` |
| Print Screen → INT 05h | **PART** | #254: `E0 37` calls INT 05h (guest code) and stores nothing (was `3700h`); Ctrl+PrtSc stores `7200h`. #274: **IVT[05h] is ours now** — `bios_kbdact.asm` `p5` + `bios_prtsc.h`: LF CR, then every cell (NUL → space) and LF CR per row, through the **guest's** INT 17h; a printer error (`AH & 29h`) after a cell stops it. ⛔ PART because the **status byte is not at `0050:0000`**: that byte is the first byte of our own INT 21h stub (`DOS_HDLR_SEG:0000`), so it is kept host-side (`g_prtsc_status`). ⚠ Windows delivers only the key-UP for PrtSc, and we deliberately do NOT synthesise a press from it (every Windows screenshot would also print the DOS screen) | prtsc_test, kbdact_test; `p_kbd3 int05.*` — **owed** (SeaBIOS has no print-screen routine; PCem's AMI is the reference) |
| SysReq → INT 15h `AH=85h` | **IMPL** | #254: `54h` press → `AX=8500h`, release → `8501h`, held bit `0018h` bit 2; our INT 15h `85h` default answers `AH=0` CF=0 | input_test T12, kbdact_test; `p_kbd2` (`int15.85.*`) |
| Ctrl+Alt+Del → reboot | **N/A** | a VDD cannot reboot its host — the same reasoning as 8042 `FEh` ([kbc.md](kbc.md)) | — |

---

## Measured history (kept)

### Gaps the probe found and closed (s72, `c5c3e60`)

* **`AH=05h` did nothing at all.** It fell into the `default` arm, which sets ZF and
  leaves AX exactly as passed — so a key-stuffing program read its **own** byte back
  out of AL and took it for success, and nothing was ever queued. This is how DOSKEY,
  installers that pre-answer their own prompts, and every TSR that drives another
  program put keys in.
* **`AH=09h` likewise.** Caught only once the probe poisoned AL.
* **`AH=03h`** was answered by accident (the default arm happens not to touch CF).

### #188 (s84, `9a91149`) — what PCem settled

PCem's genuine AMI BIOS **discards** an enhanced-only key for `AH=01h` (`AX=0000`,
ZF=1, head advanced `001E→0020`) — the IBM rule. QEMU's SeaBIOS returns `8500h`; that
was the provisional row we used to match. `kb_compat` now applies the K1S translation
for `AH=00h/01h`; `AH=10h/11h` are unfiltered. `AH=09h` became `B1h` (PCem + DOSBox-X;
SeaBIOS said `30h`) and `AH=0Ah` answers `41ABh`, which bit 4 promises.

## What to fix, in order

1. ~~Grey-key `E0` forms; `0040:0018`/`0096`/Insert; `AH=12h`; Ctrl-Break, Print Screen,
   SysReq, Pause~~ — #254 (BIOS side). ~~The INT 15h `AH=4Fh` intercept; the host sending
   Pause as `E1 1D 45`; a print-screen routine behind INT 05h~~ — #244/#274.
   Left: `0050:0000` (needs our INT 21h stub moved off it); the 4Fh call from the
   PM-reflected INT 09h; a `p_kbd3` run on all four oracles.
4. ~~Honour `0040:0080/0082` as the ring bounds~~ — #274.
5. A PCem row-by-row read of `p_kbd`, to move the provisional rows.
