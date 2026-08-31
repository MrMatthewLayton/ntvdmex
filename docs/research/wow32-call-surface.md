# The WOW32 call surface krnl386 requires

**82 function IDs, 29 of them NAMED by krnl386's own export table.** GH #128.

krnl386.exe cannot call Win32, so it reaches a 32-bit companion (real Windows:
wow32.dll inside ntvdm.exe) through a native `C4 C4 51` BOP. Every call goes through
ONE thunk at `seg1:0x2bb6`, so the whole interface is a small integer namespace.

## The frame — pinned, and confirmed against the live rig

```
[bp+0]      saved BP
[bp+2/+4]   near-return into the stub, and the stub's pushed CS
[bp+6]      ★ THE FUNCTION ID
[bp+8]      the pushed 0
[bp+10]     ★ the ARGUMENT BYTE COUNT
[bp+12/+14] the CALLER's far return address
[bp+16...]  ★ THE ARGUMENTS
[bp-16]     ★ THE RETURN VALUE (low word, then high at bp-14)
```

⚠ **Arguments start at `bp+16`, not `bp+12`.** Session 30 recorded VirtualAlloc's
argument order as "not pinned down, two readings possible". There was only ever one
reading — the host's trace read four bytes low and printed the caller's far return
address as the first two argument words. Proof it is `+16` is krnl386's own return
path at `seg1:0x2c1d`: the argument count indexes a table of
`pop bx / pop bp / add sp,0xA / retf N` stubs, so the arguments are the N bytes
above the far return address.

⚠ **The return value is NOT a register.** The thunk does `sub sp,4` *before* the BOP
and `pop ax / pop dx` after it, so anything left in AX/DX is overwritten before the
caller sees it. The 32-bit side must write the DWORD into that stack hole.

★ **Argument order is PASCAL** — pushed left to right, so the first declared argument
is at the *highest* address. Agreed by three independent call sites: VirtualAlloc's
`MEM_COMMIT|MEM_RESERVE` + `PAGE_EXECUTE_READWRITE`, VirtualFree's `MEM_RELEASE`, and
GlobalMemoryStatus's 32-byte buffer with `dwLength` pre-set to `0x20`.

A far-pointer argument is a normal 16:16 — offset low, **selector** high.

## Declining

krnl386 hooks INT 21h in PM and offers some functions here first. On `0xFFFF` it
chains to `cs:[0x3c]`, the vector it saved before hooking — **real DOS**, which in
this host is our own working INT 21h layer. `tools/ne/wowdecline.py` checks each call
site, because three of them treat `0xFFFF` as a plain error and report failure to the
app instead of chaining; declining there would be a *wrong* answer rather than a
missing one.

## How the names were obtained

- **DIRECT** — an entry-table export points *at* a stub, so the export's name in the
  (non-)resident name table **is** the function's name. No inference.
- **WRAPPER** — an export's body calls exactly one stub before returning. An
  inference, labelled as one.
- **—** — reached only from internal code. `tools/ne/nedis.py --wowfunc <id>` prints
  the stub, its callers and the code that builds the arguments. This is the work list.

Cross-checked before being trusted: `0xcf` was worked out from its call site alone
(the caller compares the result against `0x411/0x412/0x404/0x804/0x0c04`, the Far-East
LANGIDs) and the export table then said `GETSYSTEMDEFAULTLANGID`. Two methods, one
answer.

## Implemented so far

| ID | What | Evidence |
|---|---|---|
| `0xb8` | `VirtualAlloc` | call site guarded by `cmp ax,0x501` — krnl386 services **DPMI 0501** with it |
| `0xb9` | `VirtualFree` | pushes `MEM_RELEASE` |
| `0xbc` | `GlobalMemoryStatus` | 32-byte buffer, `dwAvailPhys+dwAvailPageFile` vs `dwAvailVirtual` |
| `0xcf` | `GetSystemDefaultLangID` | export table **and** call site |
| `0x78` | record the DOS data area | far pointer to the `SysVars+0x6A` table |
| `0x80` | `GetPrivateProfileString` | its own arguments: `"BOOT"`, `"WOWSHELL"`, `"WOWEXEC.EXE"`, `"SYSTEM.INI"` — **and answering it is the program launch** |
| `0x39` | `GetProfileInt` | the same shape with no filename; `("KERNEL","GPCONTINUE")` and `("ModuleCompatibility",<module>)` |
| `0x88` | `GetDriveType` | export table, and its one caller does `cmp al,2` (`DRIVE_REMOVABLE`) |
| `0x7d` | approve a task-database selector — **echo the argument** | both call sites are inside the TDB creator `seg2:0x2984`; the retry offers **aliases of the same memory**, so the question is about the selector's value, and `seg2:0x2a22` uses the return **as** the selector |
| `0xc9` | `GetCurrentDirectory` | serviced in `main.c`, because it needs the DOS machine |
| `0xc1 0x97 0xc2 0xc7 0x7e 0x89 0x6f` | **declined** → real DOS | `wowdecline.py`, spot-checked by hand |

## The full surface

Regenerate with `tools/ne/wowmap.py guest/ne/krnl386.exe --md`.

| ID | args | stub | name | evidence |
|---|---|---|---|---|
| `0x00` | 1 | `seg1:0x7f51` | — | internal only — read the call site |
| `0x01` | 2 | `seg1:0xb5c6` | **FATALEXIT** | WRAPPER |
| `0x02` | 2 | `seg1:0xb5b9` | **EXITKERNELTHUNK** | DIRECT |
| `0x03` | 0 | `seg1:0xb5ac` | **WRITEOUTPROFILES** | DIRECT |
| `0x1a` | 6 | `seg1:0xb4e9` | **GETVDMPOINTER32W** | DIRECT |
| `0x1c` | 0 | `seg1:0xb4f6` | **_CALLPROCEX32W** | WRAPPER |
| `0x1d` | 0 | `seg1:0xb503` | **YIELD** | DIRECT |
| `0x1e` | 2 | `seg1:0xb578` | **WAITEVENT** | DIRECT |
| `0x1f` | 2 | `seg1:0xb56b` | **POSTEVENT** | DIRECT |
| `0x20` | 4 | `seg1:0xb585` | **SETPRIORITY** | DIRECT |
| `0x21` | 2 | `seg1:0xb59f` | **LOCKCURRENTTASK** | DIRECT |
| `0x29` | 10 | `seg1:0xb2ba` | — | internal only — read the call site |
| `0x2a` | 4 | `seg1:0xb2c7` | — | internal only — read the call site |
| `0x2d` | 12 | `seg1:0xb40c` | **WOWLOADMODULE** | DIRECT |
| `0x2f` | 4 | `seg1:0xb5e0` | — | internal only — read the call site |
| `0x31` | 8 | `seg1:0xb5d3` | — | internal only — read the call site |
| `0x39` | 10 | `seg1:0xb52a` | — | internal only — read the call site |
| `0x3a` | 18 | `seg1:0xb51d` | — | internal only — read the call site |
| `0x3b` | 12 | `seg1:0xb55e` | — | internal only — read the call site |
| `0x6e` | 0 | `seg1:0xb1dd` | — | internal only — read the call site |
| `0x6f` | 18 | `seg1:0xb204` | — | internal only — read the call site |
| `0x70` | 4 | `seg1:0xb3f2` | **WOWGETNEXTVDMCOMMAND** | DIRECT |
| `0x71` | 20 | `seg1:0xb2ad` | — | internal only — read the call site |
| `0x72` | 2 | `seg1:0xb1ea` | — | internal only — read the call site |
| `0x73` | 4 | `seg1:0xb22b` | — | internal only — read the call site |
| `0x74` | 4 | `seg1:0xb1d0` | — | internal only — read the call site |
| `0x75` | 0 | `seg1:0xb510` | **OLDYIELD** | DIRECT |
| `0x76` | 14 | `seg1:0xb33c` | — | internal only — read the call site |
| `0x77` | 14 | `seg1:0xb349` | — | internal only — read the call site |
| `0x78` | 4 | `seg1:0xb356` | — | internal only — read the call site |
| `0x79` | 4 | `seg1:0xb363` | — | internal only — read the call site |
| `0x7a` | 8 | `seg1:0xb370` | — | internal only — read the call site |
| `0x7b` | 10 | `seg1:0xb37d` | **GETSHORTPATHNAME** | DIRECT |
| `0x7c` | 4 | `seg1:0xb38a` | — | internal only — read the call site |
| `0x7d` | 2 | `seg1:0xb397` | — | internal only — read the call site |
| `0x7e` | 6 | `seg1:0xb32f` | — | internal only — read the call site |
| `0x7f` | 14 | `seg1:0xb537` | — | internal only — read the call site |
| `0x80` | 22 | `seg1:0xb544` | — | internal only — read the call site |
| `0x81` | 16 | `seg1:0xb551` | — | internal only — read the call site |
| `0x82` | 4 | `seg1:0xb2ee` | — | internal only — read the call site |
| `0x83` | 2 | `seg1:0xb2fb` | **WOWWAITFORMSGANDEVENT** | DIRECT |
| `0x84` | 12 | `seg1:0xb308` | **WOWMSGBOX** | DIRECT |
| `0x85` | 0 | `seg1:0xb6a1` | — | internal only — read the call site |
| `0x86` | 0 | `seg1:0xb315` | — | internal only — read the call site |
| `0x87` | 4 | `seg1:0xb322` | — | internal only — read the call site |
| `0x88` | 2 | `seg1:0xb4b5` | **GETDRIVETYPE** | DIRECT |
| `0x89` | 10 | `seg1:0xb2a0` | — | internal only — read the call site |
| `0x8a` | 2 | `seg1:0xb3cb` | — | internal only — read the call site |
| `0x8b` | 8 | `seg1:0xb3ff` | **WOWREGISTERSHELLWINDOWHANDLE** | DIRECT |
| `0x8c` | 4 | `seg1:0xb4cf` | **FREELIBRARY32W** | DIRECT |
| `0x8d` | 8 | `seg1:0xb4dc` | **GETPROCADDRESS32W** | DIRECT |
| `0x96` | 2 | `seg1:0xb592` | **DIRECTEDYIELD** | DIRECT |
| `0x97` | 18 | `seg1:0xb1f7` | — | internal only — read the call site |
| `0x98` | 16 | `seg1:0xb211` | — | internal only — read the call site |
| `0x99` | 10 | `seg1:0xb21e` | — | internal only — read the call site |
| `0x9a` | 12 | `seg1:0xb4c2` | **LOADLIBRARYEX32W** | DIRECT |
| `0x9b` | 8 | `seg1:0xb426` | **WOWQUERYPERFORMANCECOUNTER** | DIRECT |
| `0x9c` | 4 | `seg1:0xb238` | **WOWCURSORICONOP** | DIRECT |
| `0x9d` | 0 | `seg1:0xb245` | **WOWFAILEDEXEC** | DIRECT |
| `0x9e` | 0 | `seg1:0xb433` | — | internal only — read the call site |
| `0x9f` | 2 | `seg1:0xb252` | **WOWCLOSECOMPORT** | DIRECT |
| `0xb7` | 4 | `seg1:0xb481` | — | internal only — read the call site |
| `0xb8` | 16 | `seg1:0xb48e` | — | internal only — read the call site |
| `0xb9` | 12 | `seg1:0xb49b` | — | internal only — read the call site |
| `0xbc` | 4 | `seg1:0xb4a8` | — | internal only — read the call site |
| `0xbd` | 0 | `seg1:0xb440` | — | internal only — read the call site |
| `0xbe` | 4 | `seg1:0xb44d` | — | internal only — read the call site |
| `0xbf` | 4 | `seg1:0xb45a` | **WOWKILLREMOTETASK** | DIRECT |
| `0xc0` | 28 | `seg1:0xb467` | — | internal only — read the call site |
| `0xc1` | 14 | `seg1:0xb25f` | — | internal only — read the call site |
| `0xc2` | 10 | `seg1:0xb26c` | — | internal only — read the call site |
| `0xc3` | 0 | `seg1:0xb419` | — | internal only — read the call site |
| `0xc4` | 14 | `seg1:0xb474` | — | internal only — read the call site |
| `0xc5` | 8 | `seg1:0xb279` | — | internal only — read the call site |
| `0xc6` | 2 | `seg1:0xb286` | — | internal only — read the call site |
| `0xc7` | 4 | `seg1:0xb293` | — | internal only — read the call site |
| `0xc8` | 2 | `seg1:0xb2d4` | — | internal only — read the call site |
| `0xc9` | 6 | `seg1:0xb2e1` | — | internal only — read the call site |
| `0xcc` | 4 | `seg1:0xb3a4` | — | internal only — read the call site |
| `0xcd` | 2 | `seg1:0xb3b1` | **WOWSHUTDOWNTIMER** | DIRECT |
| `0xce` | 0 | `seg1:0xb3be` | — | internal only — read the call site |
| `0xcf` | 0 | `seg1:0xb3d8` | **GETSYSTEMDEFAULTLANGID** | DIRECT |
| `0xd0` | 6 | `seg1:0xb3e5` | — | internal only — read the call site |
