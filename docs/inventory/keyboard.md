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
**Off-VM:** `tools/dostest/input_test.c`. **DOS probe:** `tools/dostest/p_kbd.asm`.
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
| §1 INT 16h functions | 14 | 9 | 1 | — | 1 | 3 |
| §2 BDA keyboard fields | 10 | 2 | 2 | — | 5 | 1 |
| §3 INT 09h BIOS handler | 15 | 8 | 1 | — | 5 | 1 |
| **Total** | **39** | **19** | **4** | **—** | **11** | **5** |

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
| `12h` | extended shift flags → AX | **PART** | `:698-702`: AL right; **AH is the raw byte at `0040:0018`**, which nothing writes (§2), and whose layout is not `AH`'s anyway — `AH` bit 2/3 are **right** Ctrl/Alt and bit 7 SysReq, where `0040:0018` has SysReq at bit 2, Pause at bit 3 and Insert at bit 7. So `AH` is always `00h` | provisional (`16.12.ext`) — ⚠ **agrees by luck**: nothing is held when the probe asks |
| `20h`–`22h` | 122-key keyboard calls | **N/A** | not claimed (`09h` bit 6 clear); the AMI BIOS does not offer them | — |
| — | unknown `AH` | **IMPL** | `:734-736`: `ZF=1`, registers untouched — never a phantom key | input_test `:155` |
| — | the wait loop calls INT 15h `AH=90h` (device busy) | **MISS** | the AT BIOS calls it while `00h`/`10h` waits so a multitasker can switch tasks; ours never does | — |

## 2. BDA keyboard fields

The BDA is guest memory at `0040:0000`; the VDD writes it through `g_in.bda`
(`main.c:27845`). Offsets are named in `vdd_input.h:28-33`.

| Offset | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `0017h` | shift flags | **PART** | bits 0–6 (shifts, Ctrl, Alt, Scroll/Num/Caps toggles) are maintained by `bios_translate` (`vdd_input.c:404-412`). **Bit 7, Insert active, is never toggled** — there is no flag for it (`:372-378`); `52h` only stores a keystroke | provisional (`16.02.shift`) |
| `0018h` | extended shift flags (left Ctrl/Alt held, SysReq, Pause, keys held) | **MISS** | only cleared at reset (`:769`); no writer anywhere | — |
| `0019h` | Alt+keypad accumulator | **MISS** | the keypad rows' Alt column stores nothing (`:150-152`, `:227-237`), so Alt+0+6+5 does not type `A` | — |
| `001Ah`/`001Ch` | ring head / tail | **IMPL** | `:9-67`; a pointer pair the guest has scribbled on is reset rather than trusted (`:27-31`) | provisional (`bda.buffer`) |
| `001Eh`–`003Dh` | the 16-slot ring | **IMPL** | `BDA_KB_START`/`END` (`vdd_input.h:30-31`); 15 usable slots, wrap at the end (`:16-17`) | provisional (`ring.wrap.*`); input_test `:84-86` |
| `0071h` | Ctrl-Break flag (bit 7) | **MISS** | no writer (§3) | — |
| `0072h` | reset flag (`1234h` = warm boot) | **N/A** | a VDM is never rebooted; nothing reads it | — |
| `0080h`/`0082h` | ring start / end pointers | **MISS** | **nothing in `src/` writes or reads them**: the ring wraps at the constants. A TSR that enlarges the keyboard buffer by moving these is ignored. The probe's `001E`/`003E` agreed because the values were already there, not because we model them | provisional (`bda.buffer`) — the value, not the mechanism |
| `0096h` | keyboard mode / type | **PART** | bit 4 (enhanced keyboard) is set at reset (`:774`). Bits 0/1 (last code was `E1`/`E0`) are never written | provisional (`bda.enhanced`) |
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
| Grey keys: plain / Shift **`E0` forms** (`4BE0h`, keypad Enter `E00Dh`, keypad `/` `E02Fh`) | **PART** | ⛔ `sc_ext_plain` (`:262-265`) stores `4B00h`, `1C0Dh`, `352Fh` — the **83-key** forms. `kb_compat` (`:651-657`) rewrites `E0` forms for `AH=00h/01h`, but nothing ever produces one, so `AH=10h/11h` cannot tell grey Left from keypad Left. input_test `:339` pushes `4BE0h` by hand, so it does not catch this | untested |
| Keyboard layouts (UK / German / French) | **IMPL** | #136, `:266-343`, `:430-442`; dead keys not composed. An extension, not a BIOS unit | untested |
| INT 15h `AH=4Fh` keyboard intercept | **MISS** | never called. The `C0h` system table says so honestly (feature bit 4 clear, `main.c:27524`) | — |
| Ctrl-Break → INT 1Bh, `0000h` in the ring, `0071h` bit 7 | **MISS** | scan `46h` only toggles Scroll Lock (`:412`), with or without Ctrl or `E0` | — |
| Pause (`E1 1D 45`) → BIOS pause loop, `0018h` bit 3 | **MISS** | `E1` is dropped (`:401`); the host never sends one (`main.c:6601-6602` sends only `E0` prefixes) | — |
| Print Screen → INT 05h | **MISS** | INT 05h is never called. Alt+SysRq `54h` stores nothing (`:239`); `E0 37` goes through `sc_ext_plain` and stores **`3700h`**, a keystroke the BIOS never stores | — |
| SysReq → INT 15h `AH=85h` | **MISS** | | — |
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

1. Store the grey-key `E0` forms (`sc_ext_plain`), so `AH=10h/11h` can tell the two
   key sets apart — `kb_compat` already undoes them for the 83-key calls.
2. Maintain `0040:0018` and the Insert bit, and build `AH=12h`'s `AH` from the BIOS
   layout rather than copying the byte.
3. Ctrl-Break (INT 1Bh + `0000h`), then the INT 15h `AH=4Fh` intercept (set `C0h`
   feature bit 4 in the same commit), then Print Screen / SysReq / Pause.
4. Honour `0040:0080/0082` as the ring bounds.
5. A PCem row-by-row read of `p_kbd`, to move the provisional rows.
