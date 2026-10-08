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
typedef struct _DOS_DISK_GEOMETRY {
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
- **Braces, the NT layout:** a function's opening brace on its own line; `if`/`else`/`for`/
  `while`/`switch` braces on the same line as the statement.
- **A one-line body may stay unbraced** (`if (!bootSector) return FALSE;`).
- Include guards are `NTVDMEX_<PATH>_H`, e.g. `NTVDMEX_DOS_DISK_H`.

## 6. Comments

Unchanged: say *why*, cite the specification or the measurement, keep the ⚠ warnings. Comments
refer to identifiers by their current names. Under `CLEAN-ROOM.md`, comments never quote
third-party code.

---

## 7. Names that never change

These are fixed by something outside this repository, and the style does not apply to them:

- **Exported functions** a real program or DLL links to by name: the VDD API
  (`VDDInstallIOHook`, `getAX`, …) and the WOW shim's exports (`WOWGetVDMPointer`, …).
- **The shim API version**, which the host and `bin\wowshim\` must agree on.
- **Registry value names** under `HKCU\Software\NTVDMEX`.
- **`cfg\` file-knob names.**
- **Log line formats** the rig scripts parse (`STAGE1:`, `STAGE2:`, `FULLSCREEN:`, …).
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
  commit; `main.c` last. Each module's commit passes the battery; the full rig gate runs per
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
