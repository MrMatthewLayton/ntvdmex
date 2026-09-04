# Session 48 — two allocators, one LDT

> Branch `m9/completeness`. Follows [session 47](session-47.md).

## ★★★★★ THE HEADLINE

**`File > Save As` no longer crashes in krnl386, and MS Paint's save now runs far
enough to read its whole canvas** — `GetDIBits(dc 0x20c8, bm 0x20c0, scan
0000+03ce) -> 03ce scan lines`, 974 rows out of the image, which had never
happened. The remaining failure is a **different wall in a different module**
(`OLESVR.DLL at 0003:1548`) and the file is still not written.

⚠ **Verified three ways, because the change is in a shared path:**

| | result |
|---|---|
| WOW regression gate | **`82 / 122 / 60 · 0001:229C` — unchanged** |
| **Doom** | **all eleven startup stages `V_Init`…`ST_Init`, 3.51 MB — the known-good signature** |
| host LDT pool | never spilled in any run |

---

## ★★★★★ THE BUG: TWO ALLOCATORS OVER ONE DESCRIPTOR TABLE

The user's report is what named it: *"the same happened in Notepad"*. That takes
it out of MS Paint entirely — it is **krnl386's**, so it is **every guest's**, and
any new program hits it the first time it touches a file.

krnl386 caches the DOS structures at boot (`seg1:0xc033`: `INT 21h AH=52h`, six
pointers copied out of the list of lists) and fills their segment halves at
`seg1:0xc0fd` with `mov ax,2 / int 31h` — DPMI Segment-to-Descriptor. That
selector has to stay valid for the life of the VDM. It did not:

```
INT31h AX=0002 BX=0x50    -> sel 0x018f              ; we mint it, idx 0x31
INT31h AX=000C BX=0x018f  <- base=0x0002a800 lim=0x031f   ; THE GUEST REPOINTS IT
INT31h AX=000C BX=0x018f  <- base=0x03b4c1c0 lim=0x003f
LDTSYNC idx 0x31 <- guest wrote ... INSTALL FAILED
```

and `seg1:0x5343` (`les di,[0x275] / mov al,es:[di]`, reading the current-drive
byte) then indexes 1952 bytes into a 64-byte selector. #GP.

⇒ **krnl386 keeps its own idea of which LDT entries are free** — it reaches them
through `DPMI 000C` and by writing the descriptor shadow directly — and cannot
know `g_ldt_next` had already handed 0x31 out. Our counter started at 6 and grew
upward; krnl386's arena starts at 0x30. **They grew into each other**, and the
collision window is the handful of host allocations made while the counter is
passing through the low 0x30s. `seg 0x1ef3 -> sel 0x17f` (idx 0x2f) missed by one.

### ⚠⚠ THE FIRST TWO EXPLANATIONS WERE BOTH WRONG

1. *"Declining `0x82`/`0xc1` will chain to real DOS."* Tried through
   `wow32ret.txt`: the fault **moved** to `0001:53DB` — the same broken cache one
   arm further on.
2. *"The selector is freed and then recycled to somebody else."* Written up as
   fact and checked afterwards. It does not survive the check: **`grep -c
   recycled` is 0 across three full runs and there is no `DPMI 0001` on that
   selector anywhere.** Nothing was ever freed.

⇒ Both were reasoning from a mechanism that fit rather than from the log. The
third explanation is the one the log states outright, in two independent forms
(`AX=000C` *and* a direct shadow write to the same index).

---

## ★★★ THE POOL IS MEASURED, NOT CHOSEN — AND THE DATA WAS ALREADY ON DISK

No new instrument was needed. Every LDT index the guest touches was harvested out
of three logs already on the share (a WOW bootstrap, a Paint session, a Save As):

* **348 distinct indices, `0x001..0x189`**
* dense from **`0x30` upward**, plus singletons at `0x1`, `0x2`, `0x3`, `0x8`, `0x2c`
* **largest untouched run: `0x09..0x2b`, 35 entries**
* a whole run mints only **11** host selectors → **3× headroom**

So `dpmi_seg_to_desc`, `dpmi_hdlr_code_sel` and `wow_callback_selector` now
allocate from a host-private pool at `0x09..0x2b`, and the client-facing counter
starts at `DPMI_LDT_FIRSTFREE = 0x2c` — **above** it, so a client allocation can
never land on a selector the host has handed out and a guest is holding forever.
⚠ On exhaustion it falls back to the client arena **and says so in the log**,
because a host that stops minting selectors is worse than one that risks the old
bug quietly.

★ Same family as [[instrument-must-not-infer-its-own-frame]] and
[[stale-artefact-worse-than-missing]]: the answer was in data already written, and
two sessions were spent inferring instead of grepping it.

---

## ▶ RESUME HERE

### The next wall on Save As: `OLESVR.DLL at 0003:1548`

Paint registers itself as an OLE server (session 45) and notifies its clients when
the document changes. ⚠ All 8 of its OLESVR imports are **OLESVR's own 16-bit
code**, so nothing here is a missing WOW32 service — the fault is inside OLESVR
running on something it was handed. Read `OLESVR.DLL seg3` around `0x1548` and
find what reaches it.

### Then broaden — the user's call, and the numbers back it

Measured with the session-47 enumerator:

| app | new services | notes |
|---|---|---|
| **Solitaire** | **9** | 1 GDI + 8 USER |
| **Minesweeper** | **15** | 6 are the optional SOUND driver |
| Clock | 7 | |
| Charmap | 16 | |
| **Media Player** | **20** | 3 GDI + 17 USER — **and 0 from MMSYSTEM** |
| Sound Recorder | 26 | + OLESVR |
| Program Manager | 45 | it is the shell |

★★ **Media Player's MMSYSTEM imports all resolve to 16-bit code inside
MMSYSTEM.DLL — none reach a WOW32 thunk.** WinMM is therefore not a wall in front
of it; whatever is needed sits *below* MMSYSTEM.DLL and only a run can name it.
That is the difference from the games: their cost is known and small, Media
Player's is unknown but not obviously large.

★★★ **The guests overlap heavily** — `SetTimer`, `DrawText`, `FrameRect`,
`GetParent`, `GetCurrentTime`, `IsDialogMessage`, `DefDlgProc`, `ExtTextOut`,
`AppendMenu`/`DeleteMenu`/`ModifyMenu` recur across nearly all of them. **~35
distinct services covers Solitaire, Minesweeper, Clock, Charmap, Media Player and
Sound Recorder together**, which is why breadth is cheaper here than depth.
⚠ `SetTimer` is the one that is not a pass-through: it needs `WM_TIMER` posted
into the Win16 queue, and both games want it (card animation, the clock).

### Ruled out — do not re-try

* **Declining krnl386 `0x82`/`0xc1`** (moves the fault, does not fix it).
* **"the selector was freed and recycled"** — refuted from the logs.
* **A host selector pool chosen by reasoning about "low" or "high".** krnl386 was
  observed at `0x30`, `0x31`, `0x34` *and* `0x122`; only the harvest of every
  touched index identifies a safe range.
