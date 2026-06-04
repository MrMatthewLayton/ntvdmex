# DOS process model — memory map, PSP, `.COM` entry (M2.1 research)

Research backing **M2.1 — Real DOS process setup**. Two parts: the V86 **address-space map** (which
we recovered from `ntvdm` because it's the undocumented bit), and the **PSP + entry conventions**
(documented — codify, don't spike).

## V86 address-space map — `ntvdm` `0xf00ea75`

`ntvdm` builds the guest's first MB+ as four mappings of one section. We currently replicate only
the first two (`tools/vdmhost`), which is why a guest that runs past 64KB faults at linear `0x10000`
(GP fault, event 2).

```
NtCreateSection(&h, 0xA, &oa, maxsize=0xB0000, PAGE_EXECUTE_READWRITE, SEC_RESERVE(0x4000000), NULL)
NtFreeVirtualMemory(self, base=0x00001,  size=0x9FFFF, MEM_RELEASE)   ; drop default 640KB
NtFreeVirtualMemory(self, base=0x100000, size=0x10000, MEM_RELEASE)   ; drop default HMA

; --- Map 1: first 64KB of conventional memory  (we do this)
NtMapViewOfSection(h, self, base=0x00001,  off=0,       size=0xFFFF,  ..., 0x40000000, PAGE_EXECUTE_READWRITE)
; --- Map 2: HMA  (we do this)
NtMapViewOfSection(h, self, base=0x100000, off=0x10000, size=0x10000, ...)
; --- Map 3: REST of conventional memory   (we are MISSING this)
ax   = conv_paragraphs            ; from helper 0xf0134db, stored at [0xf07726c]; 0xA000 for 640KB
size = ax*16 - 0x10000            ; = 0x90000 for 640KB  (0x0FFFF000<<4 + ax<<4, mod 2^32)
NtMapViewOfSection(h, self, base=0x10000, off=0x10000, size, ...)        ; maps 0x10000..0x9FFFF
; if ax < 0xA000: zero/fill the gap up to 0xA0000 via 0xf0447a0
; --- Upper memory: video + ROM   (defer to M3 video)
NtMapViewOfSection(h, self, base=0xA0000, off=0, size=0x60000, commit=0x1000, ...)  ; 0xA0000..0xFFFFF
```

Resulting guest physical layout (host-linear == V86-linear, 1:1):

| Linear | Size | Contents |
|--------|------|----------|
| `0x00000`–`0x003FF` | 1KB | **IVT** (256 × far ptr) |
| `0x00400`–`0x004FF` | 256B | BIOS Data Area (BDA) |
| `0x00500`–… | | DOS data / our real-mode stubs + BOP handlers |
| …–`0x9FFFF` | ≤640KB | conventional memory: PSP + program + free arena |
| `0xA0000`–`0xBFFFF` | 128KB | video memory (M3) |
| `0xC0000`–`0xFFFFF` | 256KB | option ROMs / ROM BIOS (M3) |
| `0x100000`–`0x10FFFF` | 64KB | HMA |

**Note on linear 0:** we free `base=1` (not 0) and map `base=1`, so the very first page stays
unmapped (null-deref guard). The IVT lives at `0x0`–`0x3FF`; entry `0` itself is rarely used, but if
a guest needs it we may need to map from `0`. Watch for this.

**M2.1 fix:** add Map 3 (`base=0x10000`, `off=0x10000`, `size=0x90000`) so conventional memory is the
full 640KB. The `0xA0000` upper map is M3.

## PSP — Program Segment Prefix (documented; 256 bytes at the load segment)

DOS builds this 256-byte block immediately below the program; the program loads at `PSP:0x100`.

| Off | Size | Field | Our value |
|-----|------|-------|-----------|
| `0x00` | 2 | `CD 20` (INT 20h — legacy terminate) | `CD 20` |
| `0x02` | 2 | segment of first byte past the program's memory (top) | top-of-conv segment |
| `0x05` | 5 | far `CALL` to the DOS dispatcher (CP/M legacy) | `9A ...` (or leave) |
| `0x0A` | 4 | saved INT 22h (terminate addr) | parent's |
| `0x0E` | 4 | saved INT 23h (Ctrl-C) | parent's |
| `0x12` | 4 | saved INT 24h (critical error) | parent's |
| `0x16` | 2 | parent PSP segment | 0 (we are the root) |
| `0x18` | 20 | Job File Table (handles 0–4 → stdin/out/err/aux/prn) | `01 01 01 00 02 FF…` |
| `0x2C` | 2 | environment segment | our env block seg |
| `0x2E` | 4 | saved `SS:SP` (on INT 21h entry) | runtime |
| `0x32` | 2 | JFT size (default 20) | `0x14` |
| `0x34` | 4 | far ptr to JFT (default `PSP:0x18`) | `PSP:0x18` |
| `0x38` | 4 | far ptr to previous PSP | `FFFF:FFFF` |
| `0x50` | 3 | `CD 21 CB` (INT 21h ; RETF) | `CD 21 CB` |
| `0x5C` | 16 | FCB #1 (parsed from first arg) | from cmd tail |
| `0x6C` | 16 | FCB #2 (parsed from second arg) | from cmd tail |
| `0x80` | 1 | command-tail length | from launch |
| `0x81` | 127 | command tail (leading space, `CR`-terminated); also default DTA | from launch |

The exit criterion's "reads its PSP command tail" = a guest that inspects `DS:0x80`/`DS:0x81` (DS=PSP)
sees the right argument bytes.

## `.COM` load + entry conventions (documented)

- Load the file image at `PSP:0x100` (we currently load at offset 0; switch to `0x100`).
- `CS = DS = ES = SS = PSP segment`.
- `IP = 0x100`; `SP = 0xFFFE` (top of the 64KB segment), with a `0x0000` word pushed at `[SP]` so a
  near `RET` lands at `PSP:0` → the `INT 20h` terminate.
- `AX`: `AL`/`AH` = `0x00` if the drive letters in FCB1/FCB2 are valid, `0xFF` otherwise.
- `BX:CX` = program size; the other regs are typically 0.
- Interrupt flag set (`IF=1`), direction flag clear.

## M2.1 plan (Spike → Impl → Test)

- **Spike** (in `tools/vdmhost`): add Map 3 for full 640KB; pick a PSP segment (e.g. `0x0080` so the
  program area is well clear of the IVT/BDA/stubs); build the PSP; load the `.COM` at `PSP:0x100`;
  set the entry context per above; keep INT 21h via the IVT→BOP loop.
- **Test:** a `.COM` that reads its command tail at `DS:0x80` prints the right bytes; a program that
  touches memory above 64KB no longer faults.
- **Impl:** fold into the host once M2.2's INT 21h surface lands (avoid churn).
