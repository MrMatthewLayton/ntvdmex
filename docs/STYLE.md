# NTVDMEX code style

NTVDMEX is a Windows system component written in C, and its code reads like Windows code: a
name, a type or a call looks the same whether it is ours or comes from `windows.h`.

The style follows Microsoft's NT and driver-kit conventions — the lineage NTVDM itself comes
from — rather than the Win32 SDK samples: Windows types, PascalCase functions, and **no Hungarian
notation** (Microsoft's own guidance: "Internally, the Windows team no longer uses it"). Where
this document is silent, do what the surrounding code does.

*Agreed in #333. The migration of the existing code is described in section 8; until a module
has been migrated it still uses the old style, and new code in it follows this document.*

---

## 1. Types

Use the Windows types. Never `uint8_t`/`int32_t`, never bare `int`/`unsigned`/`void`.

| What | Type |
|---|---|
| Unsigned 8 / 16 / 32 bits | `BYTE` / `WORD` / `DWORD` |
| Signed 8 / 16 / 32 / 64 bits | `INT8` / `INT16` / `INT32` / `INT64` |
| Unsigned 64 bits | `UINT64` |
| A natural `int` / `unsigned` (counts and sizes that are not a hardware width) | `INT` / `UINT` |
| A byte count or index that must span memory | `SIZE_T` |
| Truth value | `BOOL`, with `TRUE` / `FALSE` |
| ANSI text | `CHAR`, `PSTR`, `PCSTR` (`WCHAR` / `PWSTR` for wide) |
| No value / untyped pointer | `VOID`, `PVOID`, `PCVOID` |
| Floating point | `FLOAT`, `double` (Windows defines no base `DOUBLE`) |

- **`BYTE`/`WORD`/`DWORD` are for data with a hardware width** — registers, guest memory,
  ports, bit fields. That is most of this codebase.
- **Signedness is written in the type, never implied.** `CHAR` is text only: whether `char` is
  signed depends on the compiler, so a signed byte is `INT8`.
- **Avoid `LONG` / `ULONG`** except where a Windows API demands them: 32-bit on Windows, 64-bit
  in the Mac's off-VM build — a portability trap.
- **Pointers use the `P` form; never `LP`.** `PBYTE`, `PDWORD`, `PVOID`; `PC…` for a pointer to
  const. The SDK has no `PC` form for the basic types, so `ntvdmex_types.h` adds `PCVOID`,
  `PCBYTE`, `PCWORD` and `PCDWORD`.
- **Never compare a `BOOL` with `TRUE`.** Write `if (isVisible)` / `if (!isVisible)`: a `BOOL`
  is any non-zero value.

### Structures and enums

```c
typedef struct _DOS_DISK_GEOMETRY
{
    WORD  BytesPerSector;
    WORD  SectorsPerTrack;
    WORD  Heads;
    DWORD TotalSectors;
    BOOL  IsValid;
} DOS_DISK_GEOMETRY, *PDOS_DISK_GEOMETRY;

typedef const DOS_DISK_GEOMETRY *PCDOS_DISK_GEOMETRY;
```

- Type names are `UPPER_SNAKE`, with the tag `_NAME`, plus `PNAME` and `PCNAME`.
- **Members are `PascalCase`**, so a field (`geometry->Heads`) never reads like a local
  (`heads`).
- Enum values are `UPPER_SNAKE` with the module prefix, like any constant.

### Portability: `src/ntvdmex_types.h`

`windows.h` is the source of these types on Windows. The off-VM tests build the device models on
macOS, where there is no `windows.h`, so `src/ntvdmex_types.h` supplies the same names there with
**exactly the Windows widths** — `DWORD` is 32-bit even though `unsigned long` is 64-bit on the
Mac. A unit test asserts every width on both platforms. Source files include it instead of
`<stdint.h>`.

---

## 2. Names

| Kind | Form | Example |
|---|---|---|
| Function | `PascalCase`, with module prefix | `DosDiskChsToLba`, `VddEmu8kReset` |
| Local variable, parameter | `camelCase` | `sectorsPerTrack`, `voiceIndex` |
| Structure member | `PascalCase` | `geometry->BytesPerSector` |
| Global variable | `g_` + `PascalCase` | `g_VideoState` |
| Constant, macro, enum value | `UPPER_SNAKE`, with module prefix | `EMU8K_POINTER_PORT_OFFSET` |
| Type | `UPPER_SNAKE` | `EMU8K_STATE`, `PEMU8K_STATE` |
| File | lowercase, unchanged | `vdd_emu8k.c` |

### Every name is descriptive

- **No single-letter names. None** — not even `i`. A loop index is named for what it indexes:
  `voiceIndex`, `row`, `column`, `byteOffset`.
- **No private abbreviations.** `state`, not `st`; `machine`, not `mp`; `presenter`, not `pd`.
  An abbreviation the domain itself uses stays: `Lba`, `Chs`, `Dac`, `Crtc`, `Bios`, `Psp`.
- **No Hungarian notation.** The types say what something *is*; the name says what it is *for*.
- **Booleans read as predicates:** `isBlank`, `hasGlyph`, `wasDrawn`.

### Module prefixes

A function's prefix names the module it belongs to. File-static helpers use it too, so a name
says where to look.

| Prefix | Module |
|---|---|
| `Dos` | `src/dos/` — the DOS kernel (`DosInt21…`, `DosMcb…`) |
| `Bios` | `src/dos/bios_*` |
| `Vdd` + device | `src/vdd/` — `VddVideo…`, `VddPit…`, `VddEmu8k…`, `VddSb…` |
| `Present` | the presenter (`present_*`) |
| `Wow` | `src/wow/` — `WowUser…`, `WowGdi…`, `WowSched…` |
| `Dpmi` | the DPMI host services |
| `Vdm` | `src/vdm/` — the V86 / `NtVdmControl` layer |
| `Host` | `src/host/main.c` and its helpers (`HostApplyScale`) |
| `SysFont`, `Settings`, `Pif`, `Log`, `Ne`, `X86`, … | the single-purpose headers, named for what they do |

---

## 3. No magic values

**No meaningful value appears inline.** Every such value — number, string, boolean, bit mask — is
assigned to a named constant and used by that name, so the code says what each value *means*
and the definition says what it *is* and, for hardware, where it comes from.

```c
/* Before */
case 0x802: st->ptr = w; break;

/* After */
#define EMU8K_POINTER_PORT_OFFSET 0x802   /* §2: base + 802h, the Pointer register */
...
case EMU8K_POINTER_PORT_OFFSET: state->Pointer = value; break;
```

- **Name what a value means, not its digits.** Ports, masks, sizes, addresses, timeouts and
  protocol codes get names. Arithmetic identities stay literal — `index = 0`, `+ 1`, `/ 2` —
  because a name like `ZERO` only repeats the digit.
- **One name per meaning, not per value.** `0xFFFF` as a 16-bit offset mask, as a segment limit
  and as "no handle" is three names, even though the values are equal.
- **A value the code does not explain is named for its origin, never a guessed purpose**
  (`KRNL386_OFFSET_2F`, not `KRNL386_TASK_LIST`).
- **Constants live in shared area headers** (`vga_ports.h`, `bios_data.h`, …), so a port or mask
  is defined once and every module uses the same name.
- **The byte/word/dword masks are shared** (`src/ntvdmex_bits.h`: `BYTE_MASK`, `WORD_MASK`,
  `HIGH_BYTE_MASK`, ...); no module defines its own copy. A name keeps its literal's C type:
  the plain name is the `int` literal (`0xFFFF`), the `_U` name the `unsigned` one (`0xFFFFu`).
- **A buffer's size is named when it is a format or shared**: a structure's size (a 0x40-byte
  EXE header), a protocol limit, or a size two places must agree on. A one-off scratch buffer
  (`CHAR lineBuffer[160]`) keeps its literal -- the declaration says it is a size, and the code
  bounds it with `sizeof`.
- **Diagnostic display choices stay literal, like scratch buffers**: how many bytes a log line
  dumps (`LogDump(cursor, code, 8)`), how many times a message may be logged
  (`static INT ioBudget = 6`), how many histogram buckets a report prints. They are choices
  about the log, not facts about the machine. When the value IS a real size -- a stub, a
  frame, a table's length -- it uses that size's name.
- **Positions inside a fixed pattern stay literal**: the bytes of `"X:\"` (`root[1] = ':'`),
  of a signature (`buffer[0] == 'V' && buffer[1] == 'B'`), the slots of an argument array built
  line by line, the bytes of a DWORD written one at a time (`bytes[2] = value >> WORD_SHIFT`).
  The character or the shift beside the index already says what it is.
- **Data tables stay data**: a table of the period's choices (`{ 0x300, 0x310, 0x320 }`), of
  register defaults, of measured signatures or of an EDID block is read as a table, with its
  comment; naming each entry only repeats it. The table's own size and index are named when
  the code uses them.
- **A repeated idiom gets an accessor**, not just a named constant: a 16-bit register out of
  the VDM state is `VDM_REG16(tib, VTIB_CS)`, not `VDM_REG(tib, VTIB_CS) & WORD_MASK`.
- **Hardware values cite their source** — the datasheet section, the specification, or the
  oracle measurement.
- **Booleans at call sites are named too.** `Emu8kDataWrite(state, 0, TRUE, value)` hides what
  `TRUE` means; use a named constant (`EMU8K_HIGH_WORD`) or a flag.
- **Strings are constants** — registry value names, `cfg\` knob and file names, window classes
  and titles, paths, and message text.
- **Constants that must match hardware are `#define` or `enum`**, not `static const` objects: a
  `#define` or `enum` cannot change the generated code, which is what keeps the migration
  provable (section 8).

---

## 4. Functions

```c
/* Read the geometry out of a boot sector's BPB. ... */
static BOOL DosDiskGeometryFromBpb(
    _In_reads_bytes_opt_(DOS_SECTOR_SIZE) PCBYTE bootSector,
    _In_ DWORD imageSize,
    _Out_ PDOS_DISK_GEOMETRY geometry)
{
    DWORD totalSectors;
    geometry->IsValid = FALSE;
    if (!bootSector) return FALSE;
    ...
}
```

- **Success or failure is a `BOOL`**, or a meaningful status value — never `INT` −1/0.
- **SAL annotations** (`_In_`, `_Out_`, `_Inout_`, `_In_reads_bytes_(n)`, `_Out_writes_(n)`, …)
  go on every function declared in a header; on file-static functions they are optional.
  `ntvdmex_types.h` defines them as empty for the Mac build.
- **No function-like macros that evaluate an argument twice.** Use a `static FORCEINLINE`
  function. Never use `windows.h`'s `min`/`max`.

---

## 5. Layout

- 4 spaces, no tabs, lines up to 100 columns.
- **Braces, Allman:** every block brace on its own line, at the indentation of the statement
  that owns it -- functions, `if`/`else`/`for`/`while`/`do`/`switch`, structures and enums.
  `else` starts its own line too. Initialiser braces (`= { ... }`) are not blocks and stay
  where they are.

  ```c
  if (!frequency.QuadPart)
  {
      QueryPerformanceFrequency(&frequency);
  }
  else
  {
      ...
  }
  ```
- **A single-statement body may stay unbraced, on the next line**, one level in. A body of
  two or more statements is always braced.

  ```c
  if (!job->IsActive)
      return BIOS_PRINT_SCREEN_STEP_DONE;
  ```
- **One statement per line.** A `case` label stands alone on its line; its statements follow,
  one level in. The exception is a line that only writes the log -- one field per line,
  `cursor = LogPut(cursor, " io_r=");  cursor = LogHex(cursor, g_Gus.IoReads);` -- which reads
  as a table and stays together.
- **One declaration per line**, for structure members, locals and file-scope variables alike:
  `INT64 era;` / `INT64 dayOfEra;`, never `INT64 era, dayOfEra;`. A comment that described the
  whole list goes on its own line above the group. (The `typedef ... NAME, *PNAME;` pattern is
  one declaration and stays.)
- **A signature fits on one line, or takes one parameter per line**, one level in, the closing
  parenthesis after the last parameter:

  ```c
  static inline VOID DosClockSetDate(
      _In_ PCDOS_CLOCK_TIME hostNow,
      _Inout_ PINT64 offset,
      _In_ UINT year)
  ```
- **A wrapped expression** (by hand -- this one needs judgement): break before an operator,
  never align under an opening parenthesis. Keep each top-level term whole on its own line, one
  level in; a term that itself wraps continues one level deeper, so the indentation shows what
  binds to what. A wrapped `&&`/`||` chain takes one condition per line; a wrapped ternary
  takes three lines (the condition, `? a`, `: b`). `=` stays on the first line.

  ```c
  INT64 dayOfEra = yearOfEra * DOS_CLOCK_DAYS_PER_YEAR
      + yearOfEra / DOS_CLOCK_LEAP_YEAR_INTERVAL
      - yearOfEra / DOS_CLOCK_YEARS_PER_CENTURY
      + dayOfYear;
  ```
- **Blank lines:** one after every function; one after the file header; one after a function's
  local declarations; one between the groups of a `switch`; one between the steps of a
  function where it reads as a sequence (by hand); never two in a row.
- **A block stands apart:** an `if` / `for` / `while` / `do` / `switch` -- braced or not --
  has a blank line after it, and one before it. Not where the blank would split what belongs
  together: before a `}`, an `else` or a do-loop's `while`; after a `{`, a `case` label, or the
  comment that introduces the block. As a general rule, space is better for readability.

  ```c
  header = NeRead32(image + NE_MZ_LFANEW);

  if (!NeInBounds(module, header, NE_HEADER_SIZE))
  {
      module->Error = __LINE__;
      return -1;
  }

  module->Header = header;
  ```
- **Constants before code** in a header: its `#define`s and types come before its functions.
- **`#define` values line up** within a block of definitions, on a 4-column stop, and so do
  their trailing comments; a definition whose comment would pass 120 columns keeps one space.
  A comment that opens a new group of definitions has a blank line above it. The trailing
  comments of a run of `#include` lines line up the same way.
- Include guards are `NTVDMEX_<PATH>_H`, e.g. `NTVDMEX_DOS_DISK_H`.

## 5a. The order of a file

**A header (`.h`):**

1. the file header (section 6a);
2. the include guard, `#ifndef` / `#define NTVDMEX_<PATH>_H`;
3. `#include`s -- system (`<windows.h>`) first, then the project's (`"dos_mcb.h"`);
4. `#define`s: constants, then macros;
5. types -- `enum`s, `struct`s, `typedef`s -- each after the types it uses; a compile-time check
   on a type (`C_ASSERT`) right after it;
6. `extern` variables;
7. function prototypes;
8. `static inline` functions;
9. `#endif /* NTVDMEX_<PATH>_H */`.

**A source file (`.c`):**

1. the file header;
2. `#include`s -- **its own header first** (which proves the header compiles on its own), then
   system headers, then the project's;
3. private `#define`s;
4. private types;
5. forward declarations -- only the ones still needed: for two functions that call each other,
   and for a function a variable's initialiser names (a table of handlers);
6. file-scope variables -- exported first, then `static`;
7. functions, **each `static` helper above its first caller**, so no other forward declaration
   is needed.

An `#include` that has to come after code stays where it must be: the interpreter templates
(`v86interp.h`, `pm32interp.h`) after the host hooks they call, a header that needs a macro set
before it.

A comment travels with the item it describes; an `#if` block moves as one unit. A header that
grows too big to read in this order is split by what it holds: the Win16 call tables (thunk ids
and argument offsets, with their notes) live in `wowuser_calls.h` and `wowgdi_calls.h`, and
`wowuser.h` / `wowgdi.h` hold the host's own constants, types and prototypes.

## 6. Comments

Say *why*, cite the specification or the measurement. Comments refer to identifiers by their
current names. Under `CLEAN-ROOM.md`, comments never quote third-party code.

- **ASCII only** in comments (string literals are program output and are a separate matter):
  `--` not an em dash, `->` not an arrow, `x` not a times sign, `section 2` not `§2`.
- **A multi-line comment has the ` * ` gutter**, with its text starting on the opening line
  and `*/` on a line of its own:

  ```c
  /* WATCH DMX'S TASK TABLE FROM OUTSIDE:
   * The SB interrupt only ARMS the mixer (sets next_due = now, DOOM.EXE 0x571b4);
   * the TIMER services it, ...
   */
  ```
- **Markers** start a paragraph, with a blank comment line above it:
  `[INFO]:` (worth knowing), `[CAUTION]:` (a trap), `[WARNING]:` (has cost real damage or a
  session; do not do this).
- **A section title** is a line ending in a colon. Its importance, where it has one, is written
  out: `TWO DOORS ONTO ONE VALUE, AGAIN (Importance = 3):`, 1 to 5. A title can carry a marker:
  `[CAUTION]: SAY IT BEFORE IT BLOCKS, NOT AFTER (Importance = 1):`.
- **No decoration:** no rule lines, no boxes.

## 6a. The file header

Every source file opens with the same header, then one blank line:

```c
/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * What this file is, in one line.
 *
 * Anything else a reader needs first: what it is for, the decisions that shaped it.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */
```

The file's name is not repeated in it (it would drift when a file is renamed). The licence's
full text is in `LICENSE`.

---

## 7. Names that never change

These are fixed by something outside this repository, and the style does not apply to them:

- **Exported functions** a real program or DLL links to by name: the VDD API
  (`VDDInstallIOHook`, `getAX`, …) and the WOW shim's exports (`WOWGetVDMPointer`, …).
- **The shim API version**, which the host and `bin\wowshim\` must agree on.
- **Registry value names** under `HKCU\Software\NTVDMEX`.
- **`cfg\` file-knob names.**
- **Log line formats** the test scripts parse (`STAGE1:`, `STAGE2:`, `FULLSCREEN:`, …).
- **Win16 export names and thunk ids.**

---

## 8. How the existing code is migrated

Every file changes, so the migration is made in passes, and **each pass is proven not to change
behaviour**:

| Pass | What changes | Proof |
|---|---|---|
| 1. Types | `uint32_t` → `DWORD`, etc. | the built host is **byte-identical** (timestamp aside) |
| 2. Function, type, member and global names | `sysfont_fit` → `SysFontFitGlyph` | byte-identical; string literals untouched (a data-section compare) |
| 3. Local names | `i` → `voiceIndex` | byte-identical |
| 4. Magic values → named constants | `0x802` → `EMU8K_POINTER_PORT_OFFSET` | byte-identical |

- **"Byte-identical" means the STRIPPED image.** The unstripped host carries a COFF symbol
  table, and a renamed file-static function changes its entry there (the pilot's
  `vdd_emu8k.c` moved 26,000 bytes of it). Strip both (`i686-w64-mingw32-strip -o`) and
  `cmp`: the loaded image — code, data, imports, resources, relocations — must not differ in
  one byte.
- **A converted test proves itself by its OUTPUT.** Run the old test against the old code
  and the new against the new; the two transcripts, measured values included, must `diff`
  clean.

- **A pass substitutes tokens; it never reshapes an expression.** A name replaces a name, and a
  named constant replaces a literal of the same value and type, in the same place. Rewriting
  `b[11] | (b[12] << 8)` as `MAKEWORD(...)`, or `x == 0` as `!x`, is equivalent C but the
  compiler may allocate registers differently, and then the byte-identical proof is lost
  (seen in the pilot: the rewrite moved 441 bytes; the pure rename moved none). Improvements
  to the code itself are separate commits, proven by tests rather than by identity.
- **Line counts stay put where `__LINE__` is used** (`src/host/main.c`, `src/wow/ne.h`): it is an
  identity there, so a moved line would change the binary and lose the proof. Elsewhere a pass
  may add lines -- a named constant's `#define`, say -- and the binary still matches.
- **Module by module**, each on its own short branch, with its off-VM tests converted in the same
  commit; `main.c` last. Each module's commit passes the battery; the full test-machine gate runs per
  batch.
- **A pilot first:** `dos_disk.h` and `vdd_emu8k.c`, fully converted and reviewed before the
  rest.

---

## Before and after

```c
/* Before */
static void sysfont_fit(const sysfont_face_t *f, unsigned char ch, int boxblock,
                        unsigned char *cell, int H, int crop_top)
{
    const unsigned char *g = f->g[ch];
    int fh = f->h, off, y;
    for (y = 0; y < H; ++y) cell[y] = 0;

/* After */
static VOID SysFontFitGlyph(PCSYSFONT_FACE face, BYTE character, BOOL isBoxDrawing,
                            PBYTE cell, INT cellHeight, INT blankRowsToCrop)
{
    PCBYTE glyph = face->Glyphs[character];
    INT glyphHeight = face->Height, verticalOffset, row;
    for (row = SYSFONT_FIRST_ROW; row < cellHeight; ++row) cell[row] = SYSFONT_BLANK_ROW;
```
