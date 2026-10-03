# Win16 call surface — every 16→32 call, library by library

> **Generated** by `tools/ne/wowinventory.py --md --logs=…`. Do not edit by hand — regenerate after any dispatcher change. GH #128 / the s89 inventory. Messages are a separate, hand-kept table: [`win16-messages.md`](win16-messages.md).

One row is one numbered WOW call a system module's 16-bit code makes to the 32-bit side — the whole of what this host has to answer. Exports that are 16-bit code inside the module are not rows: they run on the real CPU.

- **handled** = the module's dispatcher has a `case` that does more than name the id, **or** the rig log shows it answered (main.c services some ids in front of the dispatchers). *Not* that the answer is right — a row is *verified* only once its batch is checked against stock.
- **rig** = outcomes in the host logs read (110 logs): `ok` answered, `part` answered but the host's own note says part is not implemented (usually a message — see the message table), **STEPPED** = unimplemented, stepped over, the guest got a sentinel.
- **kind** (Wine's argument types): `values` = words/longs only (handles still need mapping); `pointer` = reads/writes guest memory; `callback` = takes or installs 16-bit code (needs `wow_call16_sync()`); `?` = no Wine entry.
- **shelf** = programs in `guest/win16/` that import the call; `NAME*` = only through one of the shelf's 16-bit DLLs (an over-count: the program may not reach every call its DLL makes).
- `(internal)` / `(HOST_NAME)` = a stub no export maps to; the module reaches it from its own 16-bit code (e.g. `LoadIcon` → USER `0xad`). **0 shelf users does not mean unused** — the rig column is the evidence for these.
- MMSYSTEM has no WOW stubs at all (#5); its rows are its exports.

## Summary

| module | ids | handled | used by shelf | used + handled | **gaps** | partial |
|---|---:|---:|---:|---:|---:|---:|
| KERNEL | 202 | 32 | 9 | 6 | **12** | 0 |
| USER | 441 | 203 | 197 | 188 | **13** | 4 |
| GDI | 365 | 98 | 102 | 93 | **9** | 0 |
| KEYBOARD | 11 | 5 | 6 | 5 | **1** | 0 |
| SHELL | 34 | 16 | 18 | 16 | **2** | 0 |
| COMMDLG | 8 | 7 | 5 | 4 | **1** | 0 |
| SOUND | 17 | 0 | 6 | 0 | **6** | 0 |
| SYSTEM | 1 | 0 | 0 | 0 | **0** | 0 |
| MOUSE | 0 | 0 | 0 | 0 | **0** | 0 |
| COMM | 0 | 0 | 0 | 0 | **0** | 0 |
| TOOLHELP | 3 | 0 | 0 | 0 | **0** | 0 |
| WINNLS | 8 | 0 | 0 | 0 | **0** | 0 |
| MMSYSTEM | 166 | 0 | 31 | 0 | **31** | 0 |
| **all** | 1256 | 361 | 374 | 312 | **75** | 4 |

Shelf (21): CALC.EXE, CARDFILE.EXE, CHARMAP.EXE, CLOCK.EXE, DDEML.DLL, LZEXPAND.DLL, MPLAYER.EXE, NOTEPAD.EXE, PACKAGER.EXE, PBRUSH.DLL, PBRUSH.EXE, PROGMAN.EXE, RECORDER.EXE, SOL.EXE, SOUNDREC.EXE, SYSEDIT.EXE, TASKMAN.EXE, TERMINAL.EXE, WINFILE.EXE, WINMINE.EXE, WRITE.EXE

## The gap list — the work, most-used first

Used by the shelf and unanswered, or stepped over on the rig.

| module | id | name | Wine signature | kind | rig | shelf |
|---|---|---|---|---|---|---|
| KERNEL | `0x01e` | WAITEVENT | `(word)` | values | **STEPPED 198** | 18: CALC CARDFILE CHARMAP CLOCK MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC SYSEDIT TASKMAN TERMINAL WINFILE WINMINE WRITE |
| KERNEL | `0x001` | FATALEXIT | `()` | values |  | 16: CALC CARDFILE CHARMAP LZEXPAND MPLAYER NOTEPAD PACKAGER PBRUSH RECORDER SOL SOUNDREC SYSEDIT TERMINAL WINMINE WRITE WINFILE* |
| COMMDLG | `0x014` | PRINTDLG | `(ptr)` | callback | **STEPPED 7** | 5: CARDFILE NOTEPAD PBRUSH TERMINAL WRITE |
| GDI | `0x07b` | PLAYMETAFILE | `(word word)` | values |  | 4: PBRUSH WRITE CARDFILE* PACKAGER* |
| KERNEL | `0x01d` | YIELD | `()` | values |  | 4: DDEML CARDFILE* PACKAGER* WRITE* |
| USER | `0x01b` | ENUMPROPS | `(word segptr)` | callback | **STEPPED 16** | 3: PACKAGER* PBRUSH* SOUNDREC* |
| GDI | `0x05e` | GETVIEWPORTEXT | `(word)` | values |  | 3: CARDFILE* PACKAGER* WRITE* |
| GDI | `0x060` | GETWINDOWEXT | `(word)` | values |  | 3: CARDFILE* PACKAGER* WRITE* |
| GDI | `0x0a2` | GETBITMAPDIMENSION | `(word)` | values |  | 3: CARDFILE* PACKAGER* WRITE* |
| GDI | `0x0af` | ENUMMETAFILE | `(word word segptr long)` | callback |  | 3: CARDFILE* PACKAGER* WRITE* |
| GDI | `0x0b0` | PLAYMETAFILERECORD | `(word ptr ptr word)` | pointer |  | 3: CARDFILE* PACKAGER* WRITE* |
| USER | `0x092` | GETCLIPBOARDFORMATNAME | `(word ptr s_word)` | pointer |  | 3: CARDFILE* PACKAGER* WRITE* |
| GDI | `0x046` | ENUMFONTS | `(word str segptr long)` | callback | **STEPPED 24** | 2: TERMINAL WRITE |
| MMSYSTEM | `0x191` | WAVEOUTGETNUMDEVS | `()` | values |  | 2: MPLAYER SOUNDREC |
| MMSYSTEM | `0x1f5` | WAVEINGETNUMDEVS | `()` | values |  | 2: MPLAYER SOUNDREC |
| USER | `0x063` | DLGDIRSELECT | `(word ptr word)` | pointer |  | 2: SYSEDIT TERMINAL |
| USER | `0x1d0` | DRAGOBJECT | `(word word word word word word)` | values |  | 2: PROGMAN WINFILE |
| GDI | `0x047` | ENUMOBJECTS | `(word word segptr long)` | callback |  | 1: PBRUSH |
| GDI | `0x16c` | SETPALETTEENTRIES | `(word word word ptr)` | pointer |  | 1: DDEML |
| KEYBOARD | `0x084` | GETKBCODEPAGE | `()` | values |  | 1: DDEML |
| MMSYSTEM | `0x0c9` | MIDIOUTGETNUMDEVS | `()` | values |  | 1: MPLAYER |
| MMSYSTEM | `0x12d` | MIDIINGETNUMDEVS | `()` | values |  | 1: MPLAYER |
| MMSYSTEM | `0x15e` | AUXGETNUMDEVS | `()` | values |  | 1: MPLAYER |
| MMSYSTEM | `0x194` | WAVEOUTOPEN | `(ptr word ptr long long long)` | pointer |  | 1: SOUNDREC |
| MMSYSTEM | `0x195` | WAVEOUTCLOSE | `(word)` | values |  | 1: SOUNDREC |
| MMSYSTEM | `0x196` | WAVEOUTPREPAREHEADER | `(word segptr word)` | pointer |  | 1: SOUNDREC |
| MMSYSTEM | `0x197` | WAVEOUTUNPREPAREHEADER | `(word segptr word)` | pointer |  | 1: SOUNDREC |
| MMSYSTEM | `0x198` | WAVEOUTWRITE | `(word segptr word)` | pointer |  | 1: SOUNDREC |
| MMSYSTEM | `0x19b` | WAVEOUTRESET | `(word)` | values |  | 1: SOUNDREC |
| MMSYSTEM | `0x19c` | WAVEOUTGETPOSITION | `(word ptr word)` | pointer |  | 1: SOUNDREC |
| MMSYSTEM | `0x1f8` | WAVEINOPEN | `(ptr word ptr long long long)` | pointer |  | 1: SOUNDREC |
| MMSYSTEM | `0x1f9` | WAVEINCLOSE | `(word)` | values |  | 1: SOUNDREC |
| MMSYSTEM | `0x1fa` | WAVEINPREPAREHEADER | `(word segptr word)` | pointer |  | 1: SOUNDREC |
| MMSYSTEM | `0x1fb` | WAVEINUNPREPAREHEADER | `(word segptr word)` | pointer |  | 1: SOUNDREC |
| MMSYSTEM | `0x1fc` | WAVEINADDBUFFER | `(word segptr word)` | pointer |  | 1: SOUNDREC |
| MMSYSTEM | `0x1fd` | WAVEINSTART | `(word)` | values |  | 1: SOUNDREC |
| MMSYSTEM | `0x1ff` | WAVEINRESET | `(word)` | values |  | 1: SOUNDREC |
| MMSYSTEM | `0x200` | WAVEINGETPOSITION | `(word ptr word)` | pointer |  | 1: SOUNDREC |
| MMSYSTEM | `0x2bd` | MCISENDCOMMAND | `(word word long long)` | values |  | 1: MPLAYER |
| MMSYSTEM | `0x2be` | MCISENDSTRING | `(str ptr word word)` | pointer |  | 1: MPLAYER |
| MMSYSTEM | `0x2c2` | MCIGETERRORSTRING | `(long ptr word)` | pointer |  | 1: MPLAYER |
| MMSYSTEM | `0x4ba` | MMIOOPEN | `(str ptr long)` | pointer |  | 1: SOUNDREC |
| MMSYSTEM | `0x4bb` | MMIOCLOSE | `(word word)` | values |  | 1: SOUNDREC |
| MMSYSTEM | `0x4bc` | MMIOREAD | `(word ptr long)` | pointer |  | 1: SOUNDREC |
| MMSYSTEM | `0x4bd` | MMIOWRITE | `(word ptr long)` | pointer |  | 1: SOUNDREC |
| MMSYSTEM | `0x4bf` | MMIOGETINFO | `(word ptr word)` | pointer |  | 1: SOUNDREC |
| MMSYSTEM | `0x4c7` | MMIODESCEND | `(word ptr ptr word)` | pointer |  | 1: SOUNDREC |
| MMSYSTEM | `0x4c8` | MMIOASCEND | `(word ptr word)` | pointer |  | 1: SOUNDREC |
| MMSYSTEM | `0x4c9` | MMIOCREATECHUNK | `(word ptr word)` | pointer |  | 1: SOUNDREC |
| SHELL | `0x026` | FINDENVIRONMENTSTRING | `(ptr)` | pointer |  | 1: WINFILE |
| SHELL | `0x027` | INTERNALEXTRACTICON | `(word ptr s_word word)` | pointer |  | 1: PROGMAN |
| SOUND | `0x001` | OPENSOUND | `()` | values |  | 1: WINMINE |
| SOUND | `0x002` | CLOSESOUND | `()` | values |  | 1: WINMINE |
| SOUND | `0x004` | SETVOICENOTE | `(word word word word)` | values |  | 1: WINMINE |
| SOUND | `0x005` | SETVOICEACCENT | `(word word word word word)` | values |  | 1: WINMINE |
| SOUND | `0x009` | STARTSOUND | `()` | values |  | 1: WINMINE |
| SOUND | `0x00a` | STOPSOUND | `()` | values |  | 1: WINMINE |
| USER | `0x07b` | CALLMSGFILTER | `(ptr s_word)` | pointer |  | 1: DDEML |
| USER | `0x0bf` | CHILDWINDOWFROMPOINT | `(word long)` | values |  | 1: WINFILE* |
| USER | `0x0e9` | SETPARENT | `(word word)` | values |  | 1: DDEML |
| USER | `0x174` | GETINTERNALICONHEADER | `()` | values |  | 1: PROGMAN |
| USER | `0x194` | GETCLASSINFO | `(word segstr ptr)` | pointer |  | 1: WINFILE* |
| KERNEL | `0x0c6` | (internal) |  | ? | **STEPPED 1504** | 0 |
| USER | `0x13a` | SIGNALPROC | `(word word word word word)` | callback | **STEPPED 524** | 0 |
| KERNEL | `0x08a` | (internal) |  | ? | **STEPPED 198** | 0 |
| KERNEL | `0x02f` | (internal) |  | ? | **STEPPED 124** | 0 |
| KERNEL | `0x087` | (internal) |  | ? | **STEPPED 99** | 0 |
| KERNEL | `0x08b` | WOWREGISTERSHELLWINDOWHANDLE |  | ? | **STEPPED 99** | 0 |
| KERNEL | `0x09d` | WOWFAILEDEXEC | `()` | values | **STEPPED 99** | 0 |
| KERNEL | `0x0be` | (internal) |  | ? | **STEPPED 99** | 0 |
| KERNEL | `0x0c0` | (internal) |  | ? | **STEPPED 99** | 0 |
| USER | `0x190` | FINALUSERINIT | `()` | values | **STEPPED 99** | 0 |
| KERNEL | `0x09a` | LOADLIBRARYEX32W | `(ptr long long)` | pointer | **STEPPED 82** | 0 |
| USER | `0x079` | (internal) |  | ? | **STEPPED 14** | 0 |
| USER | `0x140` | (internal) |  | ? | **STEPPED 4** | 0 |

## Answered, but partly not implemented (from the host's own notes)

| module | id | name | rig |
|---|---|---|---|
| USER | `0x065` | SENDDLGITEMMESSAGE | ok 1537 part 216 |
| USER | `0x06f` | SENDMESSAGE | ok 310 part 43 |
| USER | `0x072` | DISPATCHMESSAGE | ok 1860 part 16 |
| USER | `0x1e3` | SYSTEMPARAMETERSINFO | ok 6 part 8 |

## KERNEL — 202 ids, 32 handled, 12 gaps

Unhandled first, then most-used.

| id | table | args | name | Wine signature | kind | handled | rig | shelf |
|---|---|---:|---|---|---|---|---|---|
| `0x01e` | seg1→own thunk 0x2bb6 | 2 | WAITEVENT | `(word)` | values | — | **STEPPED 198** | 18: CALC CARDFILE CHARMAP CLOCK MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC SYSEDIT TASKMAN TERMINAL WINFILE WINMINE WRITE |
| `0x001` | seg1→own thunk 0x2bb6 | 2 | FATALEXIT | `()` | values | — |  | 16: CALC CARDFILE CHARMAP LZEXPAND MPLAYER NOTEPAD PACKAGER PBRUSH RECORDER SOL SOUNDREC SYSEDIT TERMINAL WINMINE WRITE WINFILE* |
| `0x01d` | seg1→own thunk 0x2bb6 | 0 | YIELD | `()` | values | — |  | 4: DDEML CARDFILE* PACKAGER* WRITE* |
| `0x002` | seg1→own thunk 0x2bb6 | 2 | EXITKERNELTHUNK | `()` | values | — |  | 0 |
| `0x01a` | seg1→own thunk 0x2bb6 | 6 | GETVDMPOINTER32W | `(segptr word)` | pointer | — |  | 0 |
| `0x01c` | seg1→own thunk 0x2bb6 | 0 | _CALLPROCEX32W | `(long long long)` | values | — |  | 0 |
| `0x01f` | seg1→own thunk 0x2bb6 | 2 | POSTEVENT | `(word)` | values | — |  | 0 |
| `0x020` | seg1→own thunk 0x2bb6 | 4 | SETPRIORITY | `(word s_word)` | values | — |  | 0 |
| `0x021` | seg1→own thunk 0x2bb6 | 2 | LOCKCURRENTTASK | `(word)` | values | — |  | 0 |
| `0x029` | seg1→own thunk 0x2bb6 | 10 | (internal) |  | ? | — |  | 0 |
| `0x02a` | seg1→own thunk 0x2bb6 | 4 | (internal) |  | ? | — |  | 0 |
| `0x02d` | seg1→own thunk 0x2bb6 | 12 | WOWLOADMODULE | `()` | values | — |  | 0 |
| `0x02f` | seg1→own thunk 0x2bb6 | 4 | (internal) |  | ? | — | **STEPPED 124** | 0 |
| `0x031` | seg1→own thunk 0x2bb6 | 8 | (internal) |  | ? | — |  | 0 |
| `0x06e` | seg1→own thunk 0x2bb6 | 0 | (internal) |  | ? | — |  | 0 |
| `0x06f` | seg1→own thunk 0x2bb6 | 18 | (internal) |  | ? | — |  | 0 |
| `0x071` | seg1→own thunk 0x2bb6 | 20 | (internal) |  | ? | — |  | 0 |
| `0x072` | seg1→own thunk 0x2bb6 | 2 | (internal) |  | ? | — |  | 0 |
| `0x073` | seg1→own thunk 0x2bb6 | 4 | (internal) |  | ? | — |  | 0 |
| `0x074` | seg1→own thunk 0x2bb6 | 4 | (internal) |  | ? | — |  | 0 |
| `0x075` | seg1→own thunk 0x2bb6 | 0 | OLDYIELD | `()` | values | — |  | 0 |
| `0x076` | seg1→own thunk 0x2bb6 | 14 | (internal) |  | ? | — |  | 0 |
| `0x079` | seg1→own thunk 0x2bb6 | 4 | (internal) |  | ? | — |  | 0 |
| `0x07a` | seg1→own thunk 0x2bb6 | 8 | (internal) |  | ? | — |  | 0 |
| `0x07c` | seg1→own thunk 0x2bb6 | 4 | (internal) |  | ? | — |  | 0 |
| `0x07e` | seg1→own thunk 0x2bb6 | 6 | (internal) |  | ? | — |  | 0 |
| `0x083` | seg1→own thunk 0x2bb6 | 2 | WOWWAITFORMSGANDEVENT | `()` | values | — |  | 0 |
| `0x085` | seg1→own thunk 0x2bb6 | 0 | (internal) |  | ? | — |  | 0 |
| `0x087` | seg1→own thunk 0x2bb6 | 4 | (internal) |  | ? | — | **STEPPED 99** | 0 |
| `0x08a` | seg1→own thunk 0x2bb6 | 2 | (internal) |  | ? | — | **STEPPED 198** | 0 |
| `0x08b` | seg1→own thunk 0x2bb6 | 8 | WOWREGISTERSHELLWINDOWHANDLE |  | ? | — | **STEPPED 99** | 0 |
| `0x08c` | seg1→own thunk 0x2bb6 | 4 | FREELIBRARY32W | `(long)` | values | — |  | 0 |
| `0x08d` | seg1→own thunk 0x2bb6 | 8 | GETPROCADDRESS32W | `(long str)` | pointer | — |  | 0 |
| `0x096` | seg1→own thunk 0x2bb6 | 2 | DIRECTEDYIELD | `(word)` | values | — |  | 0 |
| `0x099` | seg1→own thunk 0x2bb6 | 10 | (internal) |  | ? | — |  | 0 |
| `0x09a` | seg1→own thunk 0x2bb6 | 12 | LOADLIBRARYEX32W | `(ptr long long)` | pointer | — | **STEPPED 82** | 0 |
| `0x09b` | seg1→own thunk 0x2bb6 | 8 | WOWQUERYPERFORMANCECOUNTER | `()` | values | — |  | 0 |
| `0x09c` | seg1→own thunk 0x2bb6 | 4 | WOWCURSORICONOP | `()` | values | — |  | 0 |
| `0x09d` | seg1→own thunk 0x2bb6 | 0 | WOWFAILEDEXEC | `()` | values | — | **STEPPED 99** | 0 |
| `0x09e` | seg1→own thunk 0x2bb6 | 0 | (internal) |  | ? | — |  | 0 |
| `0x09f` | seg1→own thunk 0x2bb6 | 2 | WOWCLOSECOMPORT |  | ? | — |  | 0 |
| `0x0bd` | seg1→own thunk 0x2bb6 | 0 | (internal) |  | ? | — |  | 0 |
| `0x0be` | seg1→own thunk 0x2bb6 | 4 | (internal) |  | ? | — | **STEPPED 99** | 0 |
| `0x0bf` | seg1→own thunk 0x2bb6 | 4 | WOWKILLREMOTETASK | `()` | values | — |  | 0 |
| `0x0c0` | seg1→own thunk 0x2bb6 | 28 | (internal) |  | ? | — | **STEPPED 99** | 0 |
| `0x0c3` | seg1→own thunk 0x2bb6 | 0 | (internal) |  | ? | — |  | 0 |
| `0x0c4` | seg1→own thunk 0x2bb6 | 14 | (WOW32_MESSAGEBOX) |  | ? | — |  | 0 |
| `0x0c6` | seg1→own thunk 0x2bb6 | 2 | (internal) |  | ? | — | **STEPPED 1504** | 0 |
| `0x0cc` | seg1→own thunk 0x2bb6 | 4 | (internal) |  | ? | — |  | 0 |
| `0x0cd` | seg1→own thunk 0x2bb6 | 2 | WOWSHUTDOWNTIMER |  | ? | — |  | 0 |
| `0x0ce` | seg1→own thunk 0x2bb6 | 0 | (internal) |  | ? | — |  | 0 |
| `0x000` | seg1→own thunk 0xaae8 | 1 | (internal) |  | ? | — |  | 0 |
| `0x000` | seg2→imported thunk | 9 | (internal) |  | ? | — |  | 0 |
| `0x007` | seg2→imported thunk | 10 | OPENFILEEX | `(str ptr word)` | pointer | — |  | 0 |
| `0x008` | seg2→imported thunk | 4 | GlobalChangeLockCount | `(word word)` | values | — |  | 0 |
| `0x009` | seg2→imported thunk | 18 | WRITEPRIVATEPROFILESTRUCT | `(str str ptr word str)` | pointer | — |  | 0 |
| `0x00a` | seg2→imported thunk | 18 | GETPRIVATEPROFILESTRUCT | `(str str ptr word str)` | pointer | — |  | 0 |
| `0x00b` | seg2→imported thunk | 8 | GETCURRENTDIRECTORY | `(long ptr)` | pointer | — |  | 0 |
| `0x00c` | seg2→imported thunk | 4 | SETCURRENTDIRECTORY | `(ptr)` | pointer | — |  | 0 |
| `0x00d` | seg2→imported thunk | 8 | FINDFIRSTFILE | `(ptr ptr)` | pointer | — |  | 0 |
| `0x00e` | seg2→imported thunk | 8 | FINDNEXTFILE | `(word ptr)` | pointer | — |  | 0 |
| `0x00f` | seg2→imported thunk | 4 | FINDCLOSE | `(word)` | values | — |  | 0 |
| `0x010` | seg2→imported thunk | 12 | WRITEPRIVATEPROFILESECTION | `(str str str)` | pointer | — |  | 0 |
| `0x011` | seg2→imported thunk | 8 | WRITEPROFILESECTION | `(str str)` | pointer | — |  | 0 |
| `0x012` | seg2→imported thunk | 14 | GETPRIVATEPROFILESECTION | `(str ptr word str)` | pointer | — |  | 0 |
| `0x013` | seg2→imported thunk | 10 | GETPROFILESECTION | `(str ptr word)` | pointer | — |  | 0 |
| `0x014` | seg2→imported thunk | 4 | GETFILEATTRIBUTES | `(ptr)` | pointer | — |  | 0 |
| `0x015` | seg2→imported thunk | 8 | SETFILEATTRIBUTES | `(ptr long)` | pointer | — |  | 0 |
| `0x016` | seg2→imported thunk | 20 | GETDISKFREESPACE | `(ptr ptr ptr ptr ptr)` | pointer | — |  | 0 |
| `0x017` | seg2→imported thunk | 6 | IsPeFormat | `(str word)` | pointer | — |  | 0 |
| `0x018` | seg2→imported thunk | 8 | FILETIMETOLOCALFILETIME | `()` | values | — |  | 0 |
| `0x019` | seg2→imported thunk | 10 | UnicodeToAnsi | `(ptr ptr word)` | pointer | — |  | 0 |
| `0x01b` | seg2→imported thunk | 24 | CreateThread16 | `(ptr long segptr segptr long ptr)` | pointer | — |  | 0 |
| `0x022` | seg2→imported thunk | 0 | WIN32_OldYield | `()` | values | — |  | 0 |
| `0x023` | seg2→imported thunk | 12 | REGLOADKEY | `()` | values | — |  | 0 |
| `0x024` | seg2→imported thunk | 8 | REGUNLOADKEY | `()` | values | — |  | 0 |
| `0x025` | seg2→imported thunk | 12 | REGSAVEKEY | `()` | values | — |  | 0 |
| `0x026` | seg2→imported thunk | 0 | GetpWin16Lock | `()` | values | — |  | 0 |
| `0x027` | seg2→imported thunk | 4 | LoadLibrary32 | `(str)` | pointer | — |  | 0 |
| `0x028` | seg2→imported thunk | 8 | GetProcAddress32 | `(long str)` | pointer | — |  | 0 |
| `0x02b` | seg2→imported thunk | 8 | CreateW32Event | `(long long)` | values | — |  | 0 |
| `0x02c` | seg2→imported thunk | 4 | SetW32Event | `(long)` | values | — |  | 0 |
| `0x02e` | seg2→imported thunk | 4 | ResetW32Event | `(long)` | values | — |  | 0 |
| `0x030` | seg2→imported thunk | 8 | WaitForSingleObject | `(long long)` | values | — |  | 0 |
| `0x032` | seg2→imported thunk | 16 | WaitForMultipleObjects | `(long ptr long long)` | pointer | — |  | 0 |
| `0x033` | seg2→imported thunk | 0 | GetCurrentThreadId | `()` | values | — |  | 0 |
| `0x034` | seg2→imported thunk | 6 | SetThreadQueue | `(long word)` | values | — |  | 0 |
| `0x036` | seg2→imported thunk | 4 | GetThreadQueue | `(long)` | values | — |  | 0 |
| `0x037` | seg2→imported thunk | 10 | NukeProcess | `()` | values | — |  | 0 |
| `0x038` | seg2→imported thunk | 2 | ExitProcess | `(word)` | values | — |  | 0 |
| `0x03c` | seg2→imported thunk | 0 | GetCurrentProcessId | `()` | values | — |  | 0 |
| `0x03d` | seg2→imported thunk | 0 | MapHInstLS | `()` | values | — |  | 0 |
| `0x03e` | seg2→imported thunk | 0 | MapHInstSL | `()` | values | — |  | 0 |
| `0x03f` | seg2→imported thunk | 4 | CloseW32Handle | `(long)` | values | — |  | 0 |
| `0x040` | seg2→imported thunk | 4 | LoadSystemLibrary32 | `(str)` | pointer | — |  | 0 |
| `0x041` | seg2→imported thunk | 4 | FreeLibrary32 | `(long)` | values | — |  | 0 |
| `0x042` | seg2→imported thunk | 10 | GetModuleFileName32 | `(long str word)` | pointer | — |  | 0 |
| `0x043` | seg2→imported thunk | 4 | GetModuleHandle32 | `(str)` | pointer | — |  | 0 |
| `0x044` | seg2→imported thunk | 8 | REGISTERSERVICEPROCESS | `(long long)` | values | — |  | 0 |
| `0x046` | seg2→imported thunk | 8 | SetThunkletCallbackGlue | `(long segptr)` | callback | — |  | 0 |
| `0x047` | seg2→imported thunk | 8 | AllocLSThunkletCallback | `(segptr long)` | callback | — |  | 0 |
| `0x048` | seg2→imported thunk | 8 | AllocSLThunkletCallback | `(long long)` | callback | — |  | 0 |
| `0x049` | seg2→imported thunk | 8 | FindLSThunkletCallback | `(segptr long)` | callback | — |  | 0 |
| `0x04a` | seg2→imported thunk | 8 | FindSLThunkletCallback | `(long long)` | callback | — |  | 0 |
| `0x04b` | seg2→imported thunk | 8 | KERNEL_566 | `()` | values | — |  | 0 |
| `0x04c` | seg2→imported thunk | 10 | AllocLSThunkletCallbackEx | `(segptr long word)` | callback | — |  | 0 |
| `0x04d` | seg2→imported thunk | 10 | AllocSLThunkletCallbackEx | `(long long word)` | callback | — |  | 0 |
| `0x04e` | seg2→imported thunk | 2 | ? |  | ? | — |  | 0 |
| `0x04f` | seg2→imported thunk | 4 | ? |  | ? | — |  | 0 |
| `0x050` | seg2→imported thunk | 6 | ? |  | ? | — |  | 0 |
| `0x051` | seg2→imported thunk | 8 | ? |  | ? | — |  | 0 |
| `0x052` | seg2→imported thunk | 10 | ? |  | ? | — |  | 0 |
| `0x053` | seg2→imported thunk | 12 | ? |  | ? | — |  | 0 |
| `0x054` | seg2→imported thunk | 14 | ? |  | ? | — |  | 0 |
| `0x055` | seg2→imported thunk | 16 | ? |  | ? | — |  | 0 |
| `0x056` | seg2→imported thunk | 18 | ? |  | ? | — |  | 0 |
| `0x057` | seg2→imported thunk | 20 | ? |  | ? | — |  | 0 |
| `0x058` | seg2→imported thunk | 22 | ? |  | ? | — |  | 0 |
| `0x059` | seg2→imported thunk | 24 | ? |  | ? | — |  | 0 |
| `0x05a` | seg2→imported thunk | 26 | ? |  | ? | — |  | 0 |
| `0x05b` | seg2→imported thunk | 28 | ? |  | ? | — |  | 0 |
| `0x05c` | seg2→imported thunk | 30 | ? |  | ? | — |  | 0 |
| `0x05d` | seg2→imported thunk | 32 | ? |  | ? | — |  | 0 |
| `0x05e` | seg2→imported thunk | 34 | ? |  | ? | — |  | 0 |
| `0x05f` | seg2→imported thunk | 36 | ? |  | ? | — |  | 0 |
| `0x060` | seg2→imported thunk | 38 | ? |  | ? | — |  | 0 |
| `0x061` | seg2→imported thunk | 40 | ? |  | ? | — |  | 0 |
| `0x062` | seg2→imported thunk | 42 | ? |  | ? | — |  | 0 |
| `0x063` | seg2→imported thunk | 44 | ? |  | ? | — |  | 0 |
| `0x064` | seg2→imported thunk | 46 | ? |  | ? | — |  | 0 |
| `0x065` | seg2→imported thunk | 48 | ? |  | ? | — |  | 0 |
| `0x066` | seg2→imported thunk | 50 | ? |  | ? | — |  | 0 |
| `0x067` | seg2→imported thunk | 52 | ? |  | ? | — |  | 0 |
| `0x068` | seg2→imported thunk | 54 | ? |  | ? | — |  | 0 |
| `0x069` | seg2→imported thunk | 56 | ? |  | ? | — |  | 0 |
| `0x06a` | seg2→imported thunk | 58 | ? |  | ? | — |  | 0 |
| `0x06b` | seg2→imported thunk | 60 | ? |  | ? | — |  | 0 |
| `0x06c` | seg2→imported thunk | 62 | ? |  | ? | — |  | 0 |
| `0x06d` | seg2→imported thunk | 64 | ? |  | ? | — |  | 0 |
| `0x08f` | seg2→imported thunk | 10 | GETPRIVATEPROFILESECTIONNAMES | `(ptr word str)` | pointer | — |  | 0 |
| `0x090` | seg2→imported thunk | 8 | CREATEDIRECTORY | `(ptr ptr)` | pointer | — |  | 0 |
| `0x091` | seg2→imported thunk | 4 | REMOVEDIRECTORY | `(ptr)` | pointer | — |  | 0 |
| `0x092` | seg2→imported thunk | 4 | DELETEFILE | `(ptr)` | pointer | — |  | 0 |
| `0x093` | seg2→imported thunk | 4 | SETLASTERROR | `(long)` | values | — |  | 0 |
| `0x094` | seg2→imported thunk | 0 | GETLASTERROR | `()` | values | — |  | 0 |
| `0x095` | seg2→imported thunk | 4 | GETVERSIONEX | `(ptr)` | pointer | — |  | 0 |
| `0x0a0` | seg2→imported thunk | 14 | K208 | `(word long long long)` | values | — |  | 0 |
| `0x0a1` | seg2→imported thunk | 12 | K209 | `(long long word long)` | values | — |  | 0 |
| `0x0a2` | seg2→imported thunk | 18 | K210 | `(long long word long long)` | values | — |  | 0 |
| `0x0a3` | seg2→imported thunk | 10 | K211 | `(long long word)` | values | — |  | 0 |
| `0x0a4` | seg2→imported thunk | 12 | K213 | `(long long word word)` | values | — |  | 0 |
| `0x0a5` | seg2→imported thunk | 10 | K214 | `(long long word)` | values | — |  | 0 |
| `0x0a6` | seg2→imported thunk | 6 | K215 | `(long word)` | values | — |  | 0 |
| `0x0a7` | seg2→imported thunk | 16 | REGENUMKEY | `(long long ptr long)` | callback | — |  | 0 |
| `0x0a8` | seg2→imported thunk | 12 | REGOPENKEY | `(long str ptr)` | pointer | — |  | 0 |
| `0x0a9` | seg2→imported thunk | 12 | REGCREATEKEY | `(long str ptr)` | pointer | — |  | 0 |
| `0x0aa` | seg2→imported thunk | 8 | REGDELETEKEY | `(long str)` | pointer | — |  | 0 |
| `0x0ab` | seg2→imported thunk | 4 | REGCLOSEKEY | `(long)` | values | — |  | 0 |
| `0x0ac` | seg2→imported thunk | 20 | REGSETVALUE | `(long str long ptr long)` | pointer | — |  | 0 |
| `0x0ad` | seg2→imported thunk | 8 | REGDELETEVALUE | `(long str)` | pointer | — |  | 0 |
| `0x0ae` | seg2→imported thunk | 32 | REGENUMVALUE | `(long long ptr ptr ptr ptr ptr ptr)` | callback | — |  | 0 |
| `0x0af` | seg2→imported thunk | 16 | REGQUERYVALUE | `(long str ptr ptr)` | pointer | — |  | 0 |
| `0x0b0` | seg2→imported thunk | 24 | REGQUERYVALUEEX | `(long str ptr ptr ptr ptr)` | pointer | — |  | 0 |
| `0x0b1` | seg2→imported thunk | 24 | REGSETVALUEEX | `(long str long long ptr long)` | pointer | — |  | 0 |
| `0x0b2` | seg2→imported thunk | 4 | REGFLUSHKEY | `(long)` | values | — |  | 0 |
| `0x0b3` | seg2→imported thunk | 2 | K228 | `(word)` | values | — |  | 0 |
| `0x0b4` | seg2→imported thunk | 4 | K229 | `(long)` | values | — |  | 0 |
| `0x0b6` | seg2→imported thunk | 0 | INVALIDATENLSCACHE | `()` | values | — |  | 0 |
| `0x0ca` | seg2→imported thunk | 6 | GETPRODUCTNAME | `()` | values | — |  | 0 |
| `0x0cb` | seg2→imported thunk | 0 | K237 | `()` | values | — |  | 0 |
| `0x03a` | seg1→own thunk 0x2bb6 | 18 | GETPROFILESTRING | `(str str str ptr word)` | pointer | ✅ | ok 480 | 14: CALC CARDFILE CHARMAP CLOCK DDEML MPLAYER NOTEPAD PBRUSH PROGMAN SOUNDREC SYSEDIT TERMINAL WINFILE WRITE |
| `0x039` | seg1→own thunk 0x2bb6 | 10 | GETPROFILEINT | `(str str s_word)` | pointer | ✅ | ok 1922 | 10: CALC CARDFILE CLOCK NOTEPAD PBRUSH RECORDER SOUNDREC TERMINAL WINFILE WRITE |
| `0x03b` | seg1→own thunk 0x2bb6 | 12 | WRITEPROFILESTRING | `(str str str)` | pointer | ✅ | ok 4 | 8: CALC CARDFILE CHARMAP PBRUSH RECORDER TERMINAL WINFILE WRITE |
| `0x07f` | seg1→own thunk 0x2bb6 | 14 | GETPRIVATEPROFILEINT | `(str str s_word str)` | pointer | ✅ | ok 635 | 6: CLOCK MPLAYER PROGMAN SOL WINFILE WINMINE |
| `0x081` | seg1→own thunk 0x2bb6 | 16 | WRITEPRIVATEPROFILESTRING | `(str str str str)` | pointer | ✅ | ok 6 | 6: CLOCK MPLAYER PROGMAN SOL WINFILE WINMINE |
| `0x088` | seg1→own thunk 0x2bb6 | 2 | GETDRIVETYPE | `(word)` | values | ✅ | ok 1285 | 5: CARDFILE PROGMAN WINFILE PACKAGER* WRITE* |
| `0x003` | seg1→own thunk 0x2bb6 | 0 | WRITEOUTPROFILES | `()` | values | ✅ |  | 0 |
| `0x070` | seg1→own thunk 0x2bb6 | 4 | WOWGETNEXTVDMCOMMAND | `()` | values | ✅ | ok 99 | 0 |
| `0x077` | seg1→own thunk 0x2bb6 | 14 | (internal) |  | ? | ✅ | ok 24 | 0 |
| `0x078` | seg1→own thunk 0x2bb6 | 4 | (WOW32_REGISTERDOSDATA) |  | ? | ✅ | ok 99 | 0 |
| `0x07b` | seg1→own thunk 0x2bb6 | 10 | GETSHORTPATHNAME | `(str ptr word)` | pointer | ✅ | ok 198 | 0 |
| `0x07d` | seg1→own thunk 0x2bb6 | 2 | (WOW32_ACCEPTTASKSELECTOR) |  | ? | ✅ | ok 198 | 0 |
| `0x080` | seg1→own thunk 0x2bb6 | 22 | (WOW32_GETPRIVATEPROFILESTRING) |  | ? | ✅ | ok 832 | 0 |
| `0x082` | seg1→own thunk 0x2bb6 | 4 | (WOW32_SETCURRENTDIR) |  | ? | ✅ | ok 236 | 0 |
| `0x084` | seg1→own thunk 0x2bb6 | 12 | WOWMSGBOX | `()` | values | ✅ |  | 0 |
| `0x086` | seg1→own thunk 0x2bb6 | 0 | (WOW32_GETDATETIME) |  | ? | ✅ | ok 525 | 0 |
| `0x089` | seg1→own thunk 0x2bb6 | 10 | (internal) |  | ? | ✅ | ok 543 | 0 |
| `0x097` | seg1→own thunk 0x2bb6 | 18 | (internal) |  | ? | ✅ | ok 10252 | 0 |
| `0x098` | seg1→own thunk 0x2bb6 | 16 | (internal) |  | ? | ✅ | ok 8425 | 0 |
| `0x0b7` | seg1→own thunk 0x2bb6 | 4 | (internal) |  | ? | ✅ | ok 8 | 0 |
| `0x0b8` | seg1→own thunk 0x2bb6 | 16 | (WOW32_VIRTUALALLOC) |  | ? | ✅ | ok 297 | 0 |
| `0x0b9` | seg1→own thunk 0x2bb6 | 12 | (WOW32_VIRTUALFREE) |  | ? | ✅ |  | 0 |
| `0x0bc` | seg1→own thunk 0x2bb6 | 4 | (WOW32_GLOBALMEMORYSTATUS) |  | ? | ✅ | ok 313 | 0 |
| `0x0c1` | seg1→own thunk 0x2bb6 | 14 | (internal) |  | ? | ✅ | ok 3856 | 0 |
| `0x0c2` | seg1→own thunk 0x2bb6 | 10 | (internal) |  | ? | ✅ | ok 1619 | 0 |
| `0x0c5` | seg1→own thunk 0x2bb6 | 8 | (WOW32_RESOLVEMODULEPATH) |  | ? | ✅ | ok 1238 | 0 |
| `0x0c7` | seg1→own thunk 0x2bb6 | 4 | (internal) |  | ? | ✅ | ok 4 | 0 |
| `0x0c8` | seg1→own thunk 0x2bb6 | 2 | (WOW32_SETCURRENTDRIVE) |  | ? | ✅ | ok 2475 | 0 |
| `0x0c9` | seg1→own thunk 0x2bb6 | 6 | (WOW32_GETCURDIR) |  | ? | ✅ | ok 1592 | 0 |
| `0x0cf` | seg1→own thunk 0x2bb6 | 0 | GETSYSTEMDEFAULTLANGID |  | ? | ✅ | ok 297 | 0 |
| `0x0d0` | seg1→own thunk 0x2bb6 | 6 | (WOW32_GETWINDOWSDIRECTORY) |  | ? | ✅ | ok 224 | 0 |
| `0x0d1` | seg2→imported thunk | 8 | (internal) |  | ? | ✅ | ok 99 | 0 |

## USER — 441 ids, 203 handled, 13 gaps

Unhandled first, then most-used.

| id | table | args | name | Wine signature | kind | handled | rig | shelf |
|---|---|---:|---|---|---|---|---|---|
| `0x01b` | seg1→imported thunk | 6 | ENUMPROPS | `(word segptr)` | callback | — | **STEPPED 16** | 3: PACKAGER* PBRUSH* SOUNDREC* |
| `0x092` | seg1→imported thunk | 8 | GETCLIPBOARDFORMATNAME | `(word ptr s_word)` | pointer | — |  | 3: CARDFILE* PACKAGER* WRITE* |
| `0x063` | seg1→imported thunk | 8 | DLGDIRSELECT | `(word ptr word)` | pointer | — |  | 2: SYSEDIT TERMINAL |
| `0x1d0` | seg1→imported thunk | 12 | DRAGOBJECT | `(word word word word word word)` | values | — |  | 2: PROGMAN WINFILE |
| `0x07b` | seg1→imported thunk | 6 | CALLMSGFILTER | `(ptr s_word)` | pointer | — |  | 1: DDEML |
| `0x0bf` | seg1→imported thunk | 6 | CHILDWINDOWFROMPOINT | `(word long)` | values | — |  | 1: WINFILE* |
| `0x0e9` | seg1→imported thunk | 4 | SETPARENT | `(word word)` | values | — |  | 1: DDEML |
| `0x174` | seg1→imported thunk | 8 | GETINTERNALICONHEADER | `()` | values | — |  | 1: PROGMAN |
| `0x194` | seg1→imported thunk | 10 | GETCLASSINFO | `(word segstr ptr)` | pointer | — |  | 1: WINFILE* |
| `0x000` | seg1→imported thunk | 0 | (internal) |  | ? | — |  | 0 |
| `0x003` | seg1→imported thunk | 0 | ENABLEOEMLAYER | `()` | values | — |  | 0 |
| `0x004` | seg1→imported thunk | 0 | DISABLEOEMLAYER | `()` | values | — |  | 0 |
| `0x008` | seg1→imported thunk | 2 | BEAR8 |  | ? | — |  | 0 |
| `0x014` | seg1→imported thunk | 2 | SETDOUBLECLICKTIME | `(word)` | values | — |  | 0 |
| `0x02b` | seg1→imported thunk | 2 | CLOSEWINDOW | `(word)` | values | — |  | 0 |
| `0x02c` | seg1→imported thunk | 2 | OPENICON | `(word)` | values | — |  | 0 |
| `0x033` | seg1→imported thunk | 2 | BEAR51 | `()` | values | — |  | 0 |
| `0x034` | seg1→imported thunk | 0 | ANYPOPUP | `()` | values | — |  | 0 |
| `0x056` | seg1→imported thunk | 0 | BEAR86 | `()` | values | — |  | 0 |
| `0x070` | seg1→imported thunk | 0 | WAITMESSAGE | `()` | values | — |  | 0 |
| `0x073` | seg1→imported thunk | 4 | REPLYMESSAGE | `(long)` | values | — |  | 0 |
| `0x074` | seg1→imported thunk | 10 | POSTAPPMESSAGE | `(word word word long)` | values | — |  | 0 |
| `0x075` | seg1→imported thunk | 2 | WINDOWFROMDC | `(word)` | values | — |  | 0 |
| `0x078` | seg1→imported thunk | 0 | GETMESSAGETIME | `()` | values | — |  | 0 |
| `0x079` | seg1→imported thunk | 8 | (internal) |  | ? | — | **STEPPED 14** | 0 |
| `0x080` | seg1→imported thunk | 4 | VALIDATERGN | `(word word)` | values | — |  | 0 |
| `0x084` | seg1→imported thunk | 8 | SETCLASSLONG | `(word s_word long)` | callback | — |  | 0 |
| `0x08f` | seg1→imported thunk | 0 | COUNTCLIPBOARDFORMATS | `()` | values | — |  | 0 |
| `0x093` | seg1→imported thunk | 2 | SETCLIPBOARDVIEWER | `(word)` | values | — |  | 0 |
| `0x094` | seg1→imported thunk | 0 | GETCLIPBOARDVIEWER | `()` | values | — |  | 0 |
| `0x095` | seg1→imported thunk | 4 | CHANGECLIPBOARDCHAIN | `(word word)` | values | — |  | 0 |
| `0x097` | seg1→imported thunk | 0 | CREATEMENU | `()` | values | — |  | 0 |
| `0x0a8` | seg1→imported thunk | 2 | SETCARETBLINKTIME | `(word)` | values | — |  | 0 |
| `0x0b5` | seg1→imported thunk | 10 | SETSYSCOLORS | `(word ptr ptr)` | pointer | — |  | 0 |
| `0x0b6` | seg1→imported thunk | 4 | BEAR182 | `(word word)` | values | — |  | 0 |
| `0x0b7` | seg1→imported thunk | 4 | GETCARETPOS | `(ptr)` | pointer | — |  | 0 |
| `0x0b8` | seg1→imported thunk | 10 | QUERYSENDMESSAGE | `()` | callback | — |  | 0 |
| `0x0ba` | seg1→imported thunk | 2 | SWAPMOUSEBUTTON | `(word)` | values | — |  | 0 |
| `0x0bb` | seg1→imported thunk | 0 | ENDMENU | `()` | values | — |  | 0 |
| `0x0c2` | seg1→imported thunk | 8 | DLGDIRSELECTCOMBOBOX | `(word ptr word)` | pointer | — |  | 0 |
| `0x0c3` | seg1→imported thunk | 12 | DLGDIRLISTCOMBOBOX | `(word ptr word word word)` | pointer | — |  | 0 |
| `0x0c5` | seg1→imported thunk | 14 | GETTABBEDTEXTEXTENT | `(word ptr word word ptr)` | pointer | — |  | 0 |
| `0x0d1` | seg1→imported thunk | 4 | GETCOMMEVENTMASK | `(word word)` | values | — |  | 0 |
| `0x0d4` | seg1→imported thunk | 4 | UNGETCOMMCHAR | `(word word)` | values | — |  | 0 |
| `0x0d5` | seg1→imported thunk | 8 | BUILDCOMMDCB | `(ptr ptr)` | pointer | — |  | 0 |
| `0x0d8` | seg1→imported thunk | 8 | USERSEEUSERDO | `(word word word word)` | values | — |  | 0 |
| `0x0d9` | seg1→imported thunk | 4 | LOOKUPMENUHANDLE | `(word s_word)` | values | — |  | 0 |
| `0x0dc` | seg1→imported thunk | 4 | LOADMENUINDIRECT | `(ptr)` | pointer | — |  | 0 |
| `0x0e2` | seg1→imported thunk | 6 | LOCKINPUT | `()` | values | — |  | 0 |
| `0x0e3` | seg1→imported thunk | 6 | GETNEXTDLGGROUPITEM | `(word word word)` | values | — |  | 0 |
| `0x0ed` | seg1→imported thunk | 6 | GETUPDATERGN | `(word word word)` | values | — |  | 0 |
| `0x0ee` | seg1→imported thunk | 4 | EXCLUDEUPDATERGN | `(word word)` | values | — |  | 0 |
| `0x0f5` | seg1→imported thunk | 8 | ENABLECOMMNOTIFICATION | `(s_word word s_word s_word)` | values | — |  | 0 |
| `0x0f6` | seg1→imported thunk | 8 | EXITWINDOWSEXEC | `(str str)` | pointer | — |  | 0 |
| `0x0f7` | seg1→imported thunk | 0 | GETCURSOR | `()` | values | — |  | 0 |
| `0x0f8` | seg1→imported thunk | 0 | GETOPENCLIPBOARDWINDOW | `()` | values | — |  | 0 |
| `0x102` | seg1→imported thunk | 10 | MAPWINDOWPOINTS | `(word word ptr word)` | pointer | — |  | 0 |
| `0x103` | seg1→imported thunk | 2 | BEGINDEFERWINDOWPOS | `(s_word)` | values | — |  | 0 |
| `0x104` | seg1→imported thunk | 16 | DEFERWINDOWPOS | `(word word word s_word s_word s_word s_word word)` | values | — |  | 0 |
| `0x105` | seg1→imported thunk | 2 | ENDDEFERWINDOWPOS | `(word)` | values | — |  | 0 |
| `0x109` | seg1→imported thunk | 4 | SHOWOWNEDPOPUPS | `(word word)` | values | — |  | 0 |
| `0x111` | seg1→imported thunk | 8 | CONTROLPANELINFO | `(word word str)` | pointer | — |  | 0 |
| `0x112` | seg1→imported thunk | 4 | GETNEXTQUEUEWINDOW | `()` | values | — |  | 0 |
| `0x113` | seg1→imported thunk | 0 | REPAINTSCREEN | `()` | values | — |  | 0 |
| `0x114` | seg1→imported thunk | 2 | LOCKMYTASK | `()` | values | — |  | 0 |
| `0x116` | seg1→imported thunk | 0 | GETDESKTOPHWND | `()` | values | — |  | 0 |
| `0x117` | seg1→imported thunk | 4 | OLDSETDESKPATTERN | `()` | values | — |  | 0 |
| `0x118` | seg1→imported thunk | 4 | SETSYSTEMMENU | `(word word)` | values | — |  | 0 |
| `0x119` | seg1→imported thunk | 2 | GETSYSCOLORBRUSH | `(word)` | values | — |  | 0 |
| `0x11c` | seg1→imported thunk | 0 | GETFREESYSTEMRESOURCES | `(word)` | values | — |  | 0 |
| `0x11d` | seg1→imported thunk | 4 | BEAR285 | `(str)` | pointer | — |  | 0 |
| `0x122` | seg1→imported thunk | 10 | REDRAWWINDOW | `(word ptr word word)` | pointer | — |  | 0 |
| `0x123` | seg1→imported thunk | 10 | SETWINDOWSHOOKEX | `(s_word segptr word word)` | callback | — |  | 0 |
| `0x124` | seg1→imported thunk | 4 | UNHOOKWINDOWSHOOKEX | `(segptr)` | callback | — |  | 0 |
| `0x125` | seg1→imported thunk | 12 | CALLNEXTHOOKEX | `(segptr s_word word long)` | callback | — |  | 0 |
| `0x126` | seg1→imported thunk | 2 | LOCKWINDOWUPDATE | `(word)` | values | — |  | 0 |
| `0x12c` | seg1→imported thunk | 2 | UNLOADINSTALLABLEDRIVERS | `()` | values | — |  | 0 |
| `0x131` | seg1→imported thunk | 10 | (internal) |  | ? | — |  | 0 |
| `0x135` | seg1→imported thunk | 4 | GETCLIPCURSOR | `(ptr)` | pointer | — |  | 0 |
| `0x136` | seg1→imported thunk | 10 | (internal) |  | ? | — |  | 0 |
| `0x137` | seg1→imported thunk | 10 | (internal) |  | ? | — |  | 0 |
| `0x138` | seg1→imported thunk | 10 | (internal) |  | ? | — |  | 0 |
| `0x139` | seg1→imported thunk | 10 | (internal) |  | ? | — |  | 0 |
| `0x13a` | seg1→imported thunk | 10 | SIGNALPROC | `(word word word word word)` | callback | — | **STEPPED 524** | 0 |
| `0x13b` | seg1→imported thunk | 0 | (internal) |  | ? | — |  | 0 |
| `0x13c` | seg1→imported thunk | 10 | (internal) |  | ? | — |  | 0 |
| `0x13d` | seg1→imported thunk | 8 | (internal) |  | ? | — |  | 0 |
| `0x13e` | seg1→imported thunk | 4 | (internal) |  | ? | — |  | 0 |
| `0x13f` | seg1→imported thunk | 0 | SCROLLWINDOWEX | `(word s_word s_word ptr ptr word ptr word)` | pointer | — |  | 0 |
| `0x140` | seg1→imported thunk | 14 | (internal) |  | ? | — | **STEPPED 4** | 0 |
| `0x141` | seg1→imported thunk | 4 | SETEVENTHOOK | `(segptr)` | callback | — |  | 0 |
| `0x142` | seg1→imported thunk | 4 | WINOLDAPPHACKOMATIC | `()` | values | — |  | 0 |
| `0x143` | seg1→imported thunk | 14 | GETMESSAGE2 | `()` | values | — |  | 0 |
| `0x144` | seg1→imported thunk | 8 | FILLWINDOW | `(word word word word)` | values | — |  | 0 |
| `0x145` | seg1→imported thunk | 12 | PAINTRECT | `(word word word word ptr)` | pointer | — |  | 0 |
| `0x146` | seg1→imported thunk | 6 | GETCONTROLBRUSH | `(word word word)` | values | — |  | 0 |
| `0x147` | seg1→imported thunk | 4 | (internal) |  | ? | — |  | 0 |
| `0x148` | seg1→imported thunk | 10 | (internal) |  | ? | — |  | 0 |
| `0x149` | seg1→imported thunk | 6 | (internal) |  | ? | — |  | 0 |
| `0x14a` | seg1→imported thunk | 4 | (internal) |  | ? | — |  | 0 |
| `0x14b` | seg1→imported thunk | 2 | ENABLEHARDWAREINPUT | `(word)` | values | — |  | 0 |
| `0x14c` | seg1→imported thunk | 0 | USERYIELD | `()` | values | — |  | 0 |
| `0x14d` | seg1→imported thunk | 0 | ISUSERIDLE | `()` | values | — |  | 0 |
| `0x14e` | seg1→imported thunk | 2 | GETQUEUESTATUS | `(word)` | values | — |  | 0 |
| `0x14f` | seg1→imported thunk | 0 | GETINPUTSTATE | `()` | values | — |  | 0 |
| `0x155` | seg1→imported thunk | 0 | (internal) |  | ? | — |  | 0 |
| `0x157` | seg1→imported thunk | 4 | (internal) |  | ? | — |  | 0 |
| `0x15a` | seg1→imported thunk | 10 | (internal) |  | ? | — |  | 0 |
| `0x15b` | seg1→imported thunk | 10 | (internal) |  | ? | — |  | 0 |
| `0x162` | seg1→imported thunk | 22 | (internal) |  | ? | — |  | 0 |
| `0x163` | seg1→imported thunk | 10 | (internal) |  | ? | — |  | 0 |
| `0x166` | seg1→imported thunk | 2 | ISMENU | `(word)` | values | — |  | 0 |
| `0x167` | seg1→imported thunk | 8 | GETDCEX | `(word word long)` | values | — |  | 0 |
| `0x16a` | seg1→imported thunk | 12 | DCHOOK | `(word word long long)` | callback | — |  | 0 |
| `0x176` | seg1→imported thunk | 16 | DLLENTRYPOINT | `(long word word word long word)` | values | — |  | 0 |
| `0x177` | seg1→imported thunk | 20 | DRAWTEXTEX | `()` | values | — |  | 0 |
| `0x178` | seg1→imported thunk | 4 | SETMESSAGEEXTRAINFO | `()` | values | — |  | 0 |
| `0x17a` | seg1→imported thunk | 10 | SETPROPEX | `()` | values | — |  | 0 |
| `0x17b` | seg1→imported thunk | 6 | GETPROPEX | `()` | values | — |  | 0 |
| `0x17c` | seg1→imported thunk | 6 | REMOVEPROPEX | `()` | values | — |  | 0 |
| `0x17e` | seg1→imported thunk | 6 | SETWINDOWCONTEXTHELPID | `()` | values | — |  | 0 |
| `0x17f` | seg1→imported thunk | 2 | GETWINDOWCONTEXTHELPID | `()` | values | — |  | 0 |
| `0x180` | seg1→imported thunk | 6 | SETMENUCONTEXTHELPID | `(word word)` | values | — |  | 0 |
| `0x181` | seg1→imported thunk | 2 | GETMENUCONTEXTHELPID | `(word)` | values | — |  | 0 |
| `0x182` | seg1→imported thunk | 0 | (internal) |  | ? | — |  | 0 |
| `0x185` | seg1→imported thunk | 14 | LOADIMAGE | `(word str word word word word)` | pointer | — |  | 0 |
| `0x186` | seg1→imported thunk | 12 | COPYIMAGE | `(word word word word word)` | values | — |  | 0 |
| `0x187` | seg1→imported thunk | 14 | SIGNALPROC32 | `(long long long word)` | values | — |  | 0 |
| `0x18a` | seg1→imported thunk | 18 | DRAWICONEX | `(word word word word word word word word word)` | values | — |  | 0 |
| `0x18b` | seg1→imported thunk | 6 | GETICONINFO | `(word ptr)` | pointer | — |  | 0 |
| `0x18d` | seg1→imported thunk | 4 | REGISTERCLASSEX | `(ptr)` | pointer | — |  | 0 |
| `0x18e` | seg1→imported thunk | 10 | GETCLASSINFOEX | `(word segstr ptr)` | pointer | — |  | 0 |
| `0x18f` | seg1→imported thunk | 8 | CHILDWINDOWFROMPOINTEX | `(word long word)` | values | — |  | 0 |
| `0x190` | seg1→imported thunk | 0 | FINALUSERINIT | `()` | values | — | **STEPPED 99** | 0 |
| `0x192` | seg1→imported thunk | 6 | GETPRIORITYCLIPBOARDFORMAT | `(ptr s_word)` | pointer | — |  | 0 |
| `0x193` | seg1→imported thunk | 6 | UNREGISTERCLASS | `(str word)` | pointer | — |  | 0 |
| `0x196` | seg1→imported thunk | 18 | CREATECURSOR | `(word word word word word ptr ptr)` | pointer | — |  | 0 |
| `0x197` | seg1→imported thunk | 18 | CREATEICON | `(word word word word word ptr ptr)` | pointer | — |  | 0 |
| `0x198` | seg1→imported thunk | 14 | CREATECURSORICONINDIRECT | `(word ptr ptr ptr)` | pointer | — |  | 0 |
| `0x199` | seg1→imported thunk | 10 | INITTHREADINPUT | `(word word)` | values | — |  | 0 |
| `0x19c` | seg1→imported thunk | 6 | REMOVEMENU | `(word word word)` | values | — |  | 0 |
| `0x1a0` | seg1→imported thunk | 16 | TRACKPOPUPMENU | `(word word s_word s_word s_word word ptr)` | pointer | — |  | 0 |
| `0x1a1` | seg1→imported thunk | 0 | GETMENUCHECKMARKDIMENSIONS | `()` | values | — |  | 0 |
| `0x1a2` | seg1→imported thunk | 10 | SETMENUITEMBITMAPS | `(word word word word word)` | values | — |  | 0 |
| `0x1a6` | seg1→imported thunk | 10 | DLGDIRSELECTEX | `(word ptr word word)` | pointer | — |  | 0 |
| `0x1a7` | seg1→imported thunk | 10 | DLGDIRSELECTCOMBOBOXEX | `(word ptr word word)` | pointer | — |  | 0 |
| `0x1ab` | seg1→imported thunk | 12 | FINDWINDOWEX | `(word word str str)` | pointer | — |  | 0 |
| `0x1ac` | seg1→imported thunk | 14 | TILEWINDOWS | `()` | values | — |  | 0 |
| `0x1ad` | seg1→imported thunk | 14 | CASCADEWINDOWS | `()` | values | — |  | 0 |
| `0x1ae` | seg1→imported thunk | 8 | (internal) |  | ? | — |  | 0 |
| `0x1af` | seg1→imported thunk | 4 | (internal) |  | ? | — |  | 0 |
| `0x1b0` | seg1→imported thunk | 4 | (internal) |  | ? | — |  | 0 |
| `0x1b1` | seg1→imported thunk | 2 | (internal) |  | ? | — |  | 0 |
| `0x1b2` | seg1→imported thunk | 2 | (internal) |  | ? | — |  | 0 |
| `0x1b3` | seg1→imported thunk | 2 | (internal) |  | ? | — |  | 0 |
| `0x1b4` | seg1→imported thunk | 2 | (internal) |  | ? | — |  | 0 |
| `0x1b5` | seg1→imported thunk | 6 | (internal) |  | ? | — |  | 0 |
| `0x1b6` | seg1→imported thunk | 6 | (internal) |  | ? | — |  | 0 |
| `0x1b9` | seg1→imported thunk | 10 | INSERTMENUITEM | `(word word word ptr)` | pointer | — |  | 0 |
| `0x1bb` | seg1→imported thunk | 10 | GETMENUITEMINFO | `()` | values | — |  | 0 |
| `0x1be` | seg1→imported thunk | 10 | SETMENUITEMINFO | `()` | values | — |  | 0 |
| `0x1c0` | seg1→imported thunk | 12 | DRAWANIMATEDRECTS | `(word word ptr ptr)` | pointer | — |  | 0 |
| `0x1c1` | seg1→imported thunk | 24 | DRAWSTATE | `(word word segptr long word s_word s_word s_word s_word word)` | pointer | — |  | 0 |
| `0x1c2` | seg1→imported thunk | 20 | CREATEICONFROMRESOURCEEX | `(ptr long word long word word word)` | pointer | — |  | 0 |
| `0x1c5` | seg1→imported thunk | 10 | (internal) |  | ? | — |  | 0 |
| `0x1c6` | seg1→imported thunk | 14 | ADJUSTWINDOWRECTEX | `(ptr long word long)` | pointer | — |  | 0 |
| `0x1c7` | seg1→imported thunk | 6 | GETICONID | `(word long)` | values | — |  | 0 |
| `0x1c8` | seg1→imported thunk | 4 | LOADICONHANDLER | `(word word)` | values | — |  | 0 |
| `0x1ca` | seg1→imported thunk | 2 | DESTROYCURSOR | `(word)` | values | — |  | 0 |
| `0x1cc` | seg1→imported thunk | 10 | GETINTERNALWINDOWPOS | `(word ptr ptr)` | pointer | — |  | 0 |
| `0x1cd` | seg1→imported thunk | 12 | SETINTERNALWINDOWPOS | `(word word ptr ptr)` | pointer | — |  | 0 |
| `0x1cf` | seg1→imported thunk | 10 | SCROLLCHILDREN | `(word word word long)` | values | — |  | 0 |
| `0x1d1` | seg1→imported thunk | 6 | DRAGDETECT | `(word long)` | values | — |  | 0 |
| `0x1d7` | seg1→imported thunk | 8 | (internal) |  | ? | — |  | 0 |
| `0x1d8` | seg1→imported thunk | 4 | (internal) |  | ? | — |  | 0 |
| `0x1d9` | seg1→imported thunk | 8 | (internal) |  | ? | — |  | 0 |
| `0x1db` | seg1→imported thunk | 10 | SETSCROLLINFO | `(word s_word ptr word)` | pointer | — |  | 0 |
| `0x1dc` | seg1→imported thunk | 8 | GETSCROLLINFO | `(word s_word ptr)` | pointer | — |  | 0 |
| `0x1dd` | seg1→imported thunk | 4 | GETKEYBOARDLAYOUTNAME | `(ptr)` | pointer | — |  | 0 |
| `0x1de` | seg1→imported thunk | 6 | LOADKEYBOARDLAYOUT | `()` | values | — |  | 0 |
| `0x1df` | seg1→imported thunk | 8 | MENUITEMFROMPOINT | `()` | values | — |  | 0 |
| `0x1e0` | seg1→imported thunk | 2 | GETUSERLOCALOBJTYPE | `()` | values | — |  | 0 |
| `0x1e1` | seg1→imported thunk | 0 | HARDWARE_EVENT |  | ? | — |  | 0 |
| `0x1f2` | seg1→imported thunk | 0 | BEAR498 | `()` | values | — |  | 0 |
| `0x1f4` | seg1→imported thunk | 0 | (internal) |  | ? | — |  | 0 |
| `0x215` | seg1→imported thunk | 0 | WNETINITIALIZE | `()` | values | — |  | 0 |
| `0x216` | seg1→imported thunk | 6 | WNETLOGON | `()` | values | — |  | 0 |
| `0x219` | seg1→imported thunk | 10 | WOWWORDBREAKPROC | `()` | callback | — |  | 0 |
| `0x21a` | seg1→imported thunk | 12 | MOUSEEVENT | `()` | values | — |  | 0 |
| `0x21b` | seg1→imported thunk | 8 | KEYBDEVENT |  | ? | — |  | 0 |
| `0x21c` | seg1→imported thunk | 0 | GETSHELLWINDOW | `()` | values | — |  | 0 |
| `0x21d` | seg1→imported thunk | 4 | DOHOTKEYSTUFF | `()` | values | — |  | 0 |
| `0x21e` | seg1→imported thunk | 2 | SETCHECKCURSORTIMER | `()` | values | — |  | 0 |
| `0x21f` | seg1→imported thunk | 6 | SETMENUDEFAULTITEM | `()` | values | — |  | 0 |
| `0x229` | seg1→imported thunk | 4 | DESTROYICON32 | `(word word)` | values | — |  | 0 |
| `0x22a` | seg1→imported thunk | 16 | BROADCASTSYSTEMMESSAGE | `()` | values | — |  | 0 |
| `0x22b` | seg1→imported thunk | 2 | HACKTASKMONITOR | `()` | values | — |  | 0 |
| `0x22d` | seg1→imported thunk | 8 | CHANGEDISPLAYSETTINGS | `(ptr long)` | pointer | — |  | 0 |
| `0x22e` | seg1→imported thunk | 0 | GETFOREGROUNDWINDOW | `()` | values | — |  | 0 |
| `0x22f` | seg1→imported thunk | 2 | SETFOREGROUNDWINDOW | `(word)` | values | — |  | 0 |
| `0x230` | seg1→imported thunk | 12 | ENUMDISPLAYSETTINGS | `(str long ptr)` | callback | — |  | 0 |
| `0x231` | seg1→imported thunk | 18 | MSGWAITFORMULTIPLEOBJECTS | `(long ptr long long long)` | pointer | — |  | 0 |
| `0x232` | seg1→imported thunk | 6 | ACTIVATEKEYBOARDLAYOUT | `()` | values | — |  | 0 |
| `0x233` | seg1→imported thunk | 4 | GETKEYBOARDLAYOUT | `()` | values | — |  | 0 |
| `0x234` | seg1→imported thunk | 6 | GETKEYBOARDLAYOUTLIST | `()` | values | — |  | 0 |
| `0x235` | seg1→imported thunk | 4 | UNLOADKEYBOARDLAYOUT | `()` | values | — |  | 0 |
| `0x236` | seg1→imported thunk | 0 | POSTPOSTEDMESSAGES | `()` | values | — |  | 0 |
| `0x237` | seg1→imported thunk | 10 | DRAWFRAMECONTROL | `(word ptr word word)` | pointer | — |  | 0 |
| `0x238` | seg1→imported thunk | 18 | DRAWCAPTIONTEMP | `(word word ptr word word ptr word)` | pointer | — |  | 0 |
| `0x239` | seg1→imported thunk | 0 | DISPATCHINPUT | `()` | values | — |  | 0 |
| `0x23a` | seg1→imported thunk | 10 | DRAWEDGE | `(word ptr word word)` | pointer | — |  | 0 |
| `0x23b` | seg1→imported thunk | 10 | DRAWCAPTION | `(word word ptr word)` | pointer | — |  | 0 |
| `0x23c` | seg1→imported thunk | 10 | SETSYSCOLORSTEMP | `()` | values | — |  | 0 |
| `0x23d` | seg1→imported thunk | 12 | DRAWMENUBARTEMP | `()` | values | — |  | 0 |
| `0x23e` | seg1→imported thunk | 6 | GETMENUDEFAULTITEM | `()` | values | — |  | 0 |
| `0x23f` | seg1→imported thunk | 10 | GETMENUITEMRECT | `(word word word ptr)` | pointer | — |  | 0 |
| `0x240` | seg1→imported thunk | 10 | CHECKMENURADIOITEM | `(word word word word word)` | values | — |  | 0 |
| `0x241` | seg1→imported thunk | 14 | TRACKPOPUPMENUEX | `()` | values | — |  | 0 |
| `0x242` | seg1→imported thunk | 6 | SETWINDOWRGN | `(word word word)` | values | — |  | 0 |
| `0x243` | seg1→imported thunk | 4 | GETWINDOWRGN | `()` | values | — |  | 0 |
| `0x244` | seg1→imported thunk | 10 | CHOOSEFONT_CALLBACK16 | `()` | callback | — |  | 0 |
| `0x245` | seg1→imported thunk | 10 | FINDREPLACE_CALLBACK16 | `()` | callback | — |  | 0 |
| `0x246` | seg1→imported thunk | 10 | OPENFILENAME_CALLBACK16 | `()` | callback | — |  | 0 |
| `0x247` | seg1→imported thunk | 10 | PRINTDLG_CALLBACK16 | `()` | callback | — |  | 0 |
| `0x248` | seg1→imported thunk | 10 | CHOOSECOLOR_CALLBACK16 | `()` | callback | — |  | 0 |
| `0x249` | seg1→imported thunk | 14 | PEEKMESSAGE32 | `(ptr word word word word word)` | pointer | — |  | 0 |
| `0x24a` | seg1→imported thunk | 12 | GETMESSAGE32 | `(ptr word word word word)` | pointer | — |  | 0 |
| `0x24b` | seg1→imported thunk | 6 | TRANSLATEMESSAGE32 | `(ptr word)` | pointer | — |  | 0 |
| `0x24c` | seg1→imported thunk | 6 | DISPATCHMESSAGE32 | `(ptr word)` | callback | — |  | 0 |
| `0x24d` | seg1→imported thunk | 8 | CALLMSGFILTER32 | `(ptr word word)` | pointer | — |  | 0 |
| `0x24e` | seg1→imported thunk | 8 | ISDIALOGMESSAGE32 |  | ? | — |  | 0 |
| `0x24f` | seg1→imported thunk | 12 | POSTMESSAGE32 | `()` | values | — |  | 0 |
| `0x250` | seg1→imported thunk | 14 | POSTTHREADMESSAGE32 | `()` | values | — |  | 0 |
| `0x251` | seg1→imported thunk | 4 | MESSAGEBOXINDIRECT | `(ptr)` | pointer | — |  | 0 |
| `0x252` | seg1→imported thunk | 12 | INSTALLIMT | `()` | values | — |  | 0 |
| `0x253` | seg1→imported thunk | 12 | UNINSTALLIMT | `()` | values | — |  | 0 |
| `0x254` | seg1→imported thunk | 12 | (internal) |  | ? | — |  | 0 |
| `0x2080` | seg1→imported thunk | 0 | (internal) |  | ? | — |  | 0 |
| `0x02a` | seg1→imported thunk | 4 | SHOWWINDOW | `(word word)` | values | ✅ | ok 372 | 18: CALC CARDFILE CHARMAP CLOCK MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC SYSEDIT TASKMAN TERMINAL WINFILE WINMINE WRITE |
| `0x039` | seg1→imported thunk | 4 | REGISTERCLASS | `(ptr)` | pointer | ✅ | ok 616 | 18: CALC CARDFILE CHARMAP CLOCK DDEML MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC SYSEDIT TERMINAL WINFILE WINMINE WRITE |
| `0x06e` | seg1→imported thunk | 10 | POSTMESSAGE | `(word word word long)` | values | ✅ | ok 48 | 18: CARDFILE CHARMAP CLOCK DDEML MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC SYSEDIT TASKMAN TERMINAL WINFILE WINMINE WRITE |
| `0x072` | seg1→imported thunk | 4 | DISPATCHMESSAGE | `(ptr)` | callback | ✅ | ok 1860 part 16 | 18: CALC CARDFILE CHARMAP CLOCK DDEML MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC SYSEDIT TERMINAL WINFILE WINMINE WRITE |
| `0x029` | seg1→imported thunk | 30 | CREATEWINDOW | `(str str long s_word s_word s_word s_word word word word segptr)` | pointer | ✅ | ok 486 | 17: CALC CARDFILE CHARMAP CLOCK DDEML NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SYSEDIT TERMINAL WINFILE WINMINE WRITE SOUNDREC* |
| `0x06b` | seg1→imported thunk | 10 | DEFWINDOWPROC | `(word word word long)` | callback | ✅ | ok 403 | 17: CALC CARDFILE CHARMAP CLOCK DDEML MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC TERMINAL WINFILE WINMINE WRITE |
| `0x06c` | seg1→imported thunk | 10 | GETMESSAGE | `(ptr word word word)` | pointer | ✅ | ok 1910 | 17: CALC CARDFILE CHARMAP CLOCK DDEML MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC SYSEDIT WINFILE WINMINE WRITE |
| `0x06f` | seg1→imported thunk | 10 | SENDMESSAGE | `(word word word long)` | callback | ✅ | ok 310 part 43 | 17: CALC CARDFILE CHARMAP DDEML MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOUNDREC SYSEDIT TASKMAN TERMINAL WINFILE WINMINE WRITE |
| `0x071` | seg1→imported thunk | 4 | TRANSLATEMESSAGE | `(ptr)` | pointer | ✅ | ok 3 | 17: CALC CARDFILE CHARMAP CLOCK MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC SYSEDIT TERMINAL WINFILE WINMINE WRITE |
| `0x07c` | seg1→imported thunk | 2 | UPDATEWINDOW | `(word)` | values | ✅ | ok 117 | 17: CALC CARDFILE CHARMAP CLOCK MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC SYSEDIT TERMINAL WINFILE WINMINE WRITE |
| `0x0b3` | seg1→imported thunk | 2 | GETSYSTEMMETRICS | `(s_word)` | values | ✅ | ok 681 | 17: CARDFILE CHARMAP CLOCK MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC SYSEDIT TASKMAN TERMINAL WINFILE WINMINE WRITE |
| `0x001` | seg1→imported thunk | 12 | MESSAGEBOX | `(word str str word)` | pointer | ✅ | ok 1 | 16: CALC CARDFILE CLOCK MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC SYSEDIT TERMINAL WINFILE WINMINE WRITE |
| `0x006` | seg1→imported thunk | 2 | POSTQUITMESSAGE | `(word)` | values | ✅ | ok 34 | 16: CALC CARDFILE CHARMAP CLOCK MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC SYSEDIT WINFILE WINMINE WRITE |
| `0x035` | seg1→imported thunk | 2 | DESTROYWINDOW | `(word)` | values | ✅ | ok 46 | 16: CALC CARDFILE CHARMAP DDEML MPLAYER NOTEPAD PACKAGER PBRUSH RECORDER SOL SOUNDREC SYSEDIT TERMINAL WINFILE WINMINE WRITE |
| `0x05b` | seg1→imported thunk | 4 | GETDLGITEM | `(word word)` | values | ✅ | ok 444 | 16: CALC CARDFILE CHARMAP MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC SYSEDIT TASKMAN TERMINAL WINFILE WRITE |
| `0x07d` | seg1→imported thunk | 8 | INVALIDATERECT | `(word ptr word)` | pointer | ✅ | ok 173 | 16: CALC CARDFILE CHARMAP CLOCK MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC TERMINAL WINFILE WINMINE WRITE |
| `0x016` | seg1→imported thunk | 2 | SETFOCUS | `(word)` | values | ✅ | ok 74 | 15: CALC CARDFILE CHARMAP MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC SYSEDIT TERMINAL WINFILE WRITE |
| `0x022` | seg1→imported thunk | 4 | ENABLEWINDOW | `(word word)` | values | ✅ | ok 71 | 15: CALC CARDFILE CHARMAP MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC SYSEDIT TERMINAL WINFILE WRITE |
| `0x042` | seg1→imported thunk | 2 | GETDC | `(word)` | values | ✅ | ok 441 | 15: CALC CARDFILE CHARMAP CLOCK DDEML MPLAYER PACKAGER PBRUSH PROGMAN SOL SOUNDREC TERMINAL WINFILE WINMINE WRITE |
| `0x044` | seg1→imported thunk | 4 | RELEASEDC | `(word word)` | values | ✅ | ok 438 | 15: CALC CARDFILE CHARMAP CLOCK DDEML MPLAYER PACKAGER PBRUSH PROGMAN SOL SOUNDREC TERMINAL WINFILE WINMINE WRITE |
| `0x09b` | seg1→imported thunk | 6 | ENABLEMENUITEM | `(word word word)` | values | ✅ | ok 163 | 15: CALC CARDFILE CLOCK NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC SYSEDIT TERMINAL WINFILE WINMINE WRITE |
| `0x021` | seg1→imported thunk | 6 | GETCLIENTRECT | `(word ptr)` | pointer | ✅ | ok 306 | 14: CARDFILE CHARMAP CLOCK MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN SOL SOUNDREC SYSEDIT TERMINAL WINFILE WRITE |
| `0x025` | seg1→imported thunk | 6 | SETWINDOWTEXT | `(word segstr)` | pointer | ✅ | ok 96 | 14: CALC CARDFILE CLOCK MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOUNDREC SYSEDIT TERMINAL WINFILE WRITE |
| `0x027` | seg1→imported thunk | 6 | BEGINPAINT | `(word ptr)` | pointer | ✅ | ok 548 | 14: CALC CARDFILE CHARMAP CLOCK MPLAYER PACKAGER PBRUSH PROGMAN SOL SOUNDREC TERMINAL WINFILE WINMINE WRITE |
| `0x028` | seg1→imported thunk | 6 | ENDPAINT | `(word ptr)` | pointer | ✅ | ok 548 | 14: CALC CARDFILE CHARMAP CLOCK MPLAYER PACKAGER PBRUSH PROGMAN SOL SOUNDREC TERMINAL WINFILE WINMINE WRITE |
| `0x038` | seg1→imported thunk | 12 | MOVEWINDOW | `(word word word word word word)` | values | ✅ | ok 313 | 14: CARDFILE CHARMAP MPLAYER NOTEPAD PBRUSH PROGMAN RECORDER SOL SYSEDIT TASKMAN TERMINAL WINFILE WINMINE WRITE |
| `0x058` | seg1→imported thunk | 4 | ENDDIALOG | `(word s_word)` | values | ✅ | ok 2 | 14: CARDFILE DDEML NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SYSEDIT TASKMAN TERMINAL WINFILE WINMINE WRITE |
| `0x05c` | seg1→imported thunk | 8 | SETDLGITEMTEXT | `(word word segstr)` | pointer | ✅ | ok 422 | 14: CALC CARDFILE MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOUNDREC SYSEDIT TERMINAL WINFILE WINMINE WRITE |
| `0x06d` | seg1→imported thunk | 12 | PEEKMESSAGE | `(ptr word word word word)` | pointer | ✅ | ok 410 | 14: CARDFILE DDEML MPLAYER NOTEPAD PBRUSH PROGMAN RECORDER SOL SYSEDIT TERMINAL WINFILE WINMINE WRITE PACKAGER* |
| `0x09d` | seg1→imported thunk | 2 | GETMENU | `(word)` | values | ✅ | ok 110 | 14: CALC CARDFILE CLOCK MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN SOL SOUNDREC SYSEDIT TERMINAL WINFILE WRITE |
| `0x00a` | seg1→imported thunk | 10 | SETTIMER | `(word word word segptr)` | callback | ✅ | ok 24 | 13: CLOCK DDEML MPLAYER RECORDER SOL SOUNDREC TERMINAL WINFILE WINMINE WRITE CARDFILE* PACKAGER* PBRUSH* |
| `0x00c` | seg1→imported thunk | 4 | KILLTIMER | `(word word)` | callback | ✅ | **STEPPED 16** | 13: CLOCK DDEML MPLAYER RECORDER SOL SOUNDREC TERMINAL WINFILE WINMINE WRITE CARDFILE* PACKAGER* PBRUSH* |
| `0x045` | seg1→imported thunk | 2 | SETCURSOR | `(word)` | values | ✅ | ok 125 | 13: CALC CARDFILE CLOCK MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOUNDREC TERMINAL WINFILE WRITE |
| `0x09a` | seg1→imported thunk | 6 | CHECKMENUITEM | `(word word word)` | values | ✅ | ok 112 | 13: CALC CARDFILE CLOCK MPLAYER NOTEPAD PBRUSH PROGMAN RECORDER SYSEDIT TERMINAL WINFILE WINMINE WRITE |
| `0x0b2` | seg1→imported thunk | 8 | TRANSLATEACCELERATOR | `(word word ptr)` | pointer | ✅ | ok 1 | 13: CALC CARDFILE MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOUNDREC SYSEDIT WINFILE WINMINE WRITE |
| `0x012` | seg1→imported thunk | 2 | SETCAPTURE | `(word)` | values | ✅ | ok 3 | 12: CARDFILE CHARMAP CLOCK MPLAYER PACKAGER PBRUSH SOL SOUNDREC TERMINAL WINFILE WINMINE WRITE |
| `0x013` | seg1→imported thunk | 0 | RELEASECAPTURE | `()` | values | ✅ | ok 1 | 12: CARDFILE CHARMAP CLOCK MPLAYER PACKAGER PBRUSH SOL SOUNDREC TERMINAL WINFILE WINMINE WRITE |
| `0x01f` | seg1→imported thunk | 2 | ISICONIC | `(word)` | values | ✅ | ok 56 | 12: CALC CLOCK NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SYSEDIT TERMINAL WINFILE WINMINE |
| `0x020` | seg1→imported thunk | 6 | GETWINDOWRECT | `(word ptr)` | pointer | ✅ | ok 97 | 12: CALC CARDFILE CHARMAP CLOCK PACKAGER PBRUSH PROGMAN RECORDER TASKMAN TERMINAL WINFILE WRITE |
| `0x051` | seg1→imported thunk | 8 | FILLRECT | `(word ptr word)` | pointer | ✅ | ok 638 | 12: CALC CARDFILE CHARMAP CLOCK MPLAYER PACKAGER PBRUSH PROGMAN SOUNDREC TERMINAL WINFILE WRITE |
| `0x065` | seg1→imported thunk | 12 | SENDDLGITEMMESSAGE | `(word word word word long)` | callback | ✅ | ok 1537 part 216 | 12: CARDFILE CHARMAP MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SYSEDIT TERMINAL WINFILE WRITE |
| `0x068` | seg1→imported thunk | 2 | MESSAGEBEEP | `(word)` | values | ✅ |  | 12: CALC CARDFILE MPLAYER NOTEPAD PBRUSH PROGMAN SOUNDREC SYSEDIT TASKMAN TERMINAL WINFILE WRITE |
| `0x087` | seg1→imported thunk | 4 | GETWINDOWLONG | `(word s_word)` | values | ✅ | ok 206 | 12: CARDFILE CLOCK DDEML MPLAYER PBRUSH PROGMAN RECORDER SOUNDREC TASKMAN WINFILE WRITE PACKAGER* |
| `0x0b4` | seg1→imported thunk | 2 | GETSYSCOLOR | `(word)` | values | ✅ | ok 1502 | 12: CALC CARDFILE CHARMAP CLOCK MPLAYER PACKAGER PBRUSH PROGMAN SOL SOUNDREC WINFILE WRITE |
| `0x02f` | seg1→imported thunk | 2 | ISWINDOW | `(word)` | values | ✅ | ok 219 | 11: CARDFILE DDEML MPLAYER PACKAGER PBRUSH PROGMAN RECORDER TASKMAN WINFILE WRITE SOUNDREC* |
| `0x05a` | seg1→imported thunk | 6 | ISDIALOGMESSAGE | `(word ptr)` | pointer | ✅ | ok 336 | 11: CALC CARDFILE CHARMAP MPLAYER NOTEPAD PBRUSH SOUNDREC SYSEDIT TERMINAL WINFILE WRITE |
| `0x05d` | seg1→imported thunk | 10 | GETDLGITEMTEXT | `(word word segptr word)` | pointer | ✅ |  | 11: CARDFILE NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SYSEDIT TERMINAL WINFILE WINMINE WRITE |
| `0x061` | seg1→imported thunk | 6 | CHECKDLGBUTTON | `(word word word)` | values | ✅ |  | 11: CALC CARDFILE PACKAGER PBRUSH PROGMAN RECORDER SOL SYSEDIT TERMINAL WINFILE WRITE |
| `0x06a` | seg1→imported thunk | 2 | GETKEYSTATE | `(word)` | values | ✅ | ok 6 | 11: CARDFILE CHARMAP PACKAGER PBRUSH PROGMAN RECORDER SOL TASKMAN TERMINAL WINFILE WRITE |
| `0x085` | seg1→imported thunk | 4 | GETWINDOWWORD | `(word s_word)` | values | ✅ | ok 180 | 11: DDEML MPLAYER PROGMAN RECORDER SOUNDREC SYSEDIT TERMINAL WINFILE WRITE PACKAGER* PBRUSH* |
| `0x088` | seg1→imported thunk | 8 | SETWINDOWLONG | `(word s_word long)` | callback | ✅ | ok 65 | 11: CARDFILE CLOCK DDEML MPLAYER NOTEPAD PBRUSH PROGMAN SOUNDREC WINFILE PACKAGER* WRITE* |
| `0x089` | seg1→imported thunk | 2 | OPENCLIPBOARD | `(word)` | values | ✅ | ok 9 | 11: CALC CARDFILE CHARMAP NOTEPAD PACKAGER PBRUSH SOUNDREC SYSEDIT TERMINAL WINFILE WRITE |
| `0x08a` | seg1→imported thunk | 0 | CLOSECLIPBOARD | `()` | values | ✅ | ok 9 | 11: CALC CARDFILE CHARMAP NOTEPAD PACKAGER PBRUSH SOUNDREC SYSEDIT TERMINAL WINFILE WRITE |
| `0x0e8` | seg1→imported thunk | 14 | SETWINDOWPOS | `(word word word word word word word)` | values | ✅ | ok 85 | 11: CALC CLOCK MPLAYER PACKAGER PBRUSH PROGMAN SOL SYSEDIT TASKMAN TERMINAL WINFILE |
| `0x031` | seg1→imported thunk | 2 | ISWINDOWVISIBLE | `(word)` | values | ✅ | ok 56 | 10: CHARMAP MPLAYER PACKAGER PBRUSH SOUNDREC TASKMAN TERMINAL WRITE CARDFILE* RECORDER* |
| `0x060` | seg1→imported thunk | 8 | CHECKRADIOBUTTON | `(word word word word)` | values | ✅ | ok 79 | 10: CALC CARDFILE PACKAGER PBRUSH PROGMAN RECORDER SOL TERMINAL WINFILE WRITE |
| `0x062` | seg1→imported thunk | 4 | ISDLGBUTTONCHECKED | `(word word)` | values | ✅ |  | 10: CARDFILE PACKAGER PBRUSH PROGMAN RECORDER SOL SYSEDIT TERMINAL WINFILE WRITE |
| `0x09f` | seg1→imported thunk | 4 | GETSUBMENU | `(word word)` | values | ✅ | ok 71 | 10: CALC CARDFILE MPLAYER NOTEPAD PACKAGER PROGMAN SYSEDIT TERMINAL WINFILE WRITE |
| `0x02d` | seg1→imported thunk | 2 | BRINGWINDOWTOTOP | `(word)` | values | ✅ | ok 4 | 9: CARDFILE PACKAGER PBRUSH PROGMAN SOUNDREC TERMINAL WINFILE WINMINE WRITE |
| `0x03b` | seg1→imported thunk | 2 | SETACTIVEWINDOW | `(word)` | values | ✅ |  | 9: CLOCK MPLAYER NOTEPAD PBRUSH RECORDER SOL SOUNDREC SYSEDIT WINFILE |
| `0x086` | seg1→imported thunk | 6 | SETWINDOWWORD | `(word s_word word)` | values | ✅ | ok 120 | 9: CLOCK DDEML MPLAYER PROGMAN SOUNDREC SYSEDIT WINFILE PACKAGER* PBRUSH* |
| `0x01d` | seg1→imported thunk | 6 | SCREENTOCLIENT | `(word ptr)` | pointer | ✅ | ok 71 | 8: CHARMAP MPLAYER PACKAGER PBRUSH PROGMAN SOUNDREC WINFILE WRITE |
| `0x02e` | seg1→imported thunk | 2 | GETPARENT | `(word)` | values | ✅ | ok 72 | 8: DDEML MPLAYER RECORDER SOUNDREC WINFILE WRITE PACKAGER* PBRUSH* |
| `0x03c` | seg1→imported thunk | 0 | GETACTIVEWINDOW | `()` | values | ✅ | ok 21 | 8: MPLAYER PBRUSH PROGMAN RECORDER SOUNDREC TERMINAL WINFILE WRITE |
| `0x03e` | seg1→imported thunk | 8 | SETSCROLLPOS | `(word word s_word word)` | values | ✅ | ok 42 | 8: CARDFILE NOTEPAD PACKAGER PBRUSH PROGMAN SOUNDREC TERMINAL WRITE |
| `0x040` | seg1→imported thunk | 10 | SETSCROLLRANGE | `(word word s_word s_word word)` | values | ✅ | ok 54 | 8: CARDFILE NOTEPAD PACKAGER PBRUSH PROGMAN SOUNDREC TERMINAL WRITE |
| `0x047` | seg1→imported thunk | 2 | SHOWCURSOR | `(word)` | values | ✅ | ok 306 | 8: CALC CHARMAP PACKAGER PBRUSH PROGMAN SOL WINFILE WRITE |
| `0x08b` | seg1→imported thunk | 0 | EMPTYCLIPBOARD | `()` | values | ✅ |  | 8: CARDFILE CHARMAP PACKAGER PBRUSH SOUNDREC TERMINAL WINFILE WRITE |
| `0x106` | seg1→imported thunk | 4 | GETWINDOW | `(word word)` | values | ✅ | ok 47 | 8: DDEML PROGMAN SYSEDIT TASKMAN WINFILE PACKAGER* PBRUSH* SOUNDREC* |
| `0x017` | seg1→imported thunk | 0 | GETFOCUS | `()` | values | ✅ | ok 14 | 7: MPLAYER NOTEPAD PROGMAN RECORDER SOUNDREC TERMINAL WINFILE |
| `0x018` | seg1→imported thunk | 6 | REMOVEPROP | `(word ptr)` | pointer | ✅ |  | 7: SYSEDIT CARDFILE* PACKAGER* PBRUSH* SOUNDREC* WINFILE* WRITE* |
| `0x019` | seg1→imported thunk | 6 | GETPROP | `(word str)` | pointer | ✅ |  | 7: SYSEDIT CARDFILE* PACKAGER* PBRUSH* SOUNDREC* WINFILE* WRITE* |
| `0x01a` | seg1→imported thunk | 8 | SETPROP | `(word str word)` | pointer | ✅ |  | 7: SYSEDIT CARDFILE* PACKAGER* PBRUSH* SOUNDREC* WINFILE* WRITE* |
| `0x01c` | seg1→imported thunk | 6 | CLIENTTOSCREEN | `(word ptr)` | pointer | ✅ | ok 71 | 7: CHARMAP PBRUSH SOL SOUNDREC WINFILE WINMINE WRITE |
| `0x023` | seg1→imported thunk | 2 | ISWINDOWENABLED | `(word)` | values | ✅ | ok 55 | 7: MPLAYER PACKAGER PROGMAN RECORDER SOUNDREC WRITE WINFILE* |
| `0x08e` | seg1→imported thunk | 2 | GETCLIPBOARDDATA | `(word)` | values | ✅ |  | 7: CALC CARDFILE PACKAGER PBRUSH SOUNDREC TERMINAL WRITE |
| `0x091` | seg1→imported thunk | 4 | REGISTERCLIPBOARDFORMAT | `(ptr)` | pointer | ✅ | ok 318 | 7: CARDFILE CHARMAP PACKAGER PBRUSH SOUNDREC WINFILE WRITE |
| `0x09c` | seg1→imported thunk | 4 | GETSYSTEMMENU | `(word word)` | values | ✅ | ok 10 | 7: CARDFILE CLOCK NOTEPAD PBRUSH PROGMAN RECORDER WINFILE* |
| `0x0c1` | seg1→imported thunk | 2 | ISCLIPBOARDFORMATAVAILABLE | `(word)` | values | ✅ | ok 7 | 7: CALC CARDFILE PACKAGER PBRUSH SOUNDREC TERMINAL WRITE |
| `0x0e0` | seg1→imported thunk | 2 | GETWINDOWTASK | `(word)` | values | ✅ |  | 7: DDEML TASKMAN CARDFILE* PACKAGER* PBRUSH* SOUNDREC* WRITE* |
| `0x10c` | seg1→imported thunk | 4 | GLOBALADDATOM | `(str)` | pointer | ✅ | ok 762 | 7: DDEML PACKAGER PBRUSH PROGMAN SOUNDREC CARDFILE* WRITE* |
| `0x10d` | seg1→imported thunk | 2 | GLOBALDELETEATOM | `(word)` | values | ✅ | ok 46 | 7: DDEML PACKAGER PBRUSH PROGMAN SOUNDREC CARDFILE* WRITE* |
| `0x10e` | seg1→imported thunk | 4 | GLOBALFINDATOM | `(str)` | pointer | ✅ |  | 7: DDEML PROGMAN CARDFILE* PACKAGER* PBRUSH* SOUNDREC* WRITE* |
| `0x11e` | seg1→imported thunk | 0 | GETDESKTOPWINDOW | `()` | values | ✅ | ok 103 | 7: PBRUSH TASKMAN WINFILE WINMINE PACKAGER* RECORDER* SOUNDREC* |
| `0x11f` | seg1→imported thunk | 2 | GETLASTACTIVEPOPUP | `(word)` | values | ✅ |  | 7: CLOCK PROGMAN SOL SYSEDIT TASKMAN WINFILE WINMINE |
| `0x19d` | seg1→imported thunk | 6 | DELETEMENU | `(word word word)` | values | ✅ | ok 25 | 7: CARDFILE MPLAYER PACKAGER PROGMAN SOUNDREC WINFILE WRITE |
| `0x00f` | seg1→imported thunk | 0 | GETCURRENTTIME | `()` | values | ✅ | ok 2 | 6: CARDFILE DDEML MPLAYER TERMINAL WINMINE WRITE |
| `0x03a` | seg1→imported thunk | 8 | GETCLASSNAME | `(word ptr word)` | pointer | ✅ | ok 8 | 6: RECORDER WINFILE WRITE PACKAGER* PBRUSH* SOUNDREC* |
| `0x03f` | seg1→imported thunk | 4 | GETSCROLLPOS | `(word word)` | values | ✅ | ok 20 | 6: MPLAYER PACKAGER PBRUSH PROGMAN SOUNDREC TERMINAL |
| `0x053` | seg1→imported thunk | 8 | FRAMERECT | `(word ptr word)` | pointer | ✅ | ok 48 | 6: CARDFILE MPLAYER SOL SOUNDREC TERMINAL WINFILE |
| `0x055` | seg1→imported thunk | 14 | DRAWTEXT | `(word str s_word ptr word)` | pointer | ✅ | ok 26 | 6: MPLAYER PACKAGER PROGMAN SOL SOUNDREC TERMINAL |
| `0x076` | seg1→imported thunk | 4 | REGISTERWINDOWMESSAGE | `(str)` | pointer | ✅ | ok 135 | 6: CARDFILE NOTEPAD PROGMAN SOL WINFILE WRITE |
| `0x090` | seg1→imported thunk | 2 | ENUMCLIPBOARDFORMATS | `(word)` | callback | ✅ | ok 29 | 6: CARDFILE NOTEPAD SOUNDREC SYSEDIT WRITE PACKAGER* |
| `0x0eb` | seg1→imported thunk | 12 | DEFHOOKPROC | `(s_word word long ptr)` | callback | ✅ |  | 6: DDEML MPLAYER PROGMAN SOUNDREC WINFILE RECORDER* |
| `0x10f` | seg1→imported thunk | 8 | GLOBALGETATOMNAME | `(word ptr s_word)` | pointer | ✅ |  | 6: DDEML CARDFILE* PACKAGER* PBRUSH* SOUNDREC* WRITE* |
| `0x026` | seg1→imported thunk | 2 | GETWINDOWTEXTLENGTH | `(word)` | values | ✅ |  | 5: CARDFILE CHARMAP PROGMAN RECORDER SYSEDIT |
| `0x03d` | seg1→imported thunk | 14 | SCROLLWINDOW | `(word s_word s_word ptr ptr)` | pointer | ✅ | ok 8 | 5: CARDFILE PACKAGER PBRUSH PROGMAN TERMINAL |
| `0x046` | seg1→imported thunk | 4 | SETCURSORPOS | `(word word)` | values | ✅ |  | 5: PBRUSH SOL WINFILE WRITE RECORDER* |
| `0x052` | seg1→imported thunk | 6 | INVERTRECT | `(word ptr)` | pointer | ✅ | ok 4 | 5: MPLAYER PBRUSH SOL TERMINAL WRITE |
| `0x054` | seg1→imported thunk | 8 | DRAWICON | `(word s_word s_word word)` | values | ✅ |  | 5: PACKAGER PROGMAN SOUNDREC TERMINAL WINFILE |
| `0x011` | seg1→imported thunk | 4 | GETCURSORPOS | `(ptr)` | pointer | ✅ |  | 4: PBRUSH WINFILE WRITE RECORDER* |
| `0x037` | seg1→imported thunk | 10 | ENUMCHILDWINDOWS | `(word segptr long)` | callback | ✅ | ok 9 | 4: WRITE PACKAGER* PBRUSH* SOUNDREC* |
| `0x043` | seg1→imported thunk | 2 | GETWINDOWDC | `(word)` | values | ✅ | ok 17 | 4: CHARMAP PACKAGER PBRUSH TERMINAL |
| `0x05e` | seg1→imported thunk | 8 | SETDLGITEMINT | `(word word word word)` | values | ✅ |  | 4: NOTEPAD PBRUSH TERMINAL WINMINE |
| `0x077` | seg1→imported thunk | 0 | GETMESSAGEPOS | `()` | values | ✅ |  | 4: CHARMAP MPLAYER PBRUSH WINFILE* |
| `0x0a3` | seg1→imported thunk | 8 | CREATECARET | `(word word word word)` | values | ✅ |  | 4: MPLAYER PBRUSH PROGMAN WINFILE* |
| `0x0a4` | seg1→imported thunk | 0 | DESTROYCARET | `()` | values | ✅ |  | 4: MPLAYER PBRUSH PROGMAN WINFILE* |
| `0x0a5` | seg1→imported thunk | 4 | SETCARETPOS | `(word word)` | values | ✅ |  | 4: MPLAYER PBRUSH PROGMAN WINFILE* |
| `0x0a6` | seg1→imported thunk | 2 | HIDECARET | `(word)` | values | ✅ |  | 4: MPLAYER PBRUSH PROGMAN WINFILE* |
| `0x0a7` | seg1→imported thunk | 2 | SHOWCARET | `(word)` | values | ✅ |  | 4: MPLAYER PBRUSH PROGMAN WINFILE* |
| `0x0b9` | seg1→imported thunk | 22 | GRAYSTRING | `(word word segptr segptr s_word s_word s_word s_word s_word)` | callback | ✅ | ok 45 | 4: MPLAYER PROGMAN SOUNDREC TERMINAL |
| `0x0ea` | seg1→imported thunk | 6 | UNHOOKWINDOWSHOOK | `(s_word segptr)` | callback | ✅ |  | 4: DDEML MPLAYER SOUNDREC RECORDER* |
| `0x0ec` | seg1→imported thunk | 0 | GETCAPTURE | `()` | values | ✅ |  | 4: MPLAYER RECORDER SOUNDREC WINFILE |
| `0x11a` | seg1→imported thunk | 6 | SELECTPALETTE | `(word word word)` | values | ✅ |  | 4: PBRUSH CARDFILE* PACKAGER* WRITE* |
| `0x11b` | seg1→imported thunk | 2 | REALIZEPALETTE | `(word)` | values | ✅ |  | 4: PBRUSH CARDFILE* PACKAGER* WRITE* |
| `0x19a` | seg1→imported thunk | 12 | INSERTMENU | `(word word word word segptr)` | pointer | ✅ | ok 1 | 4: CARDFILE PACKAGER WINFILE WRITE |
| `0x1d2` | seg1→imported thunk | 6 | DRAWFOCUSRECT | `(word ptr)` | pointer | ✅ | ok 15 | 4: CHARMAP PACKAGER PROGMAN WINFILE |
| `0x00d` | seg1→imported thunk | 0 | GETTICKCOUNT | `()` | values | ✅ | **STEPPED 15** | 3: CALC RECORDER WINFILE |
| `0x036` | seg1→imported thunk | 8 | ENUMWINDOWS | `(segptr long)` | callback | ✅ | ok 4 | 3: DDEML RECORDER WRITE |
| `0x041` | seg1→imported thunk | 12 | GETSCROLLRANGE | `(word word ptr ptr)` | pointer | ✅ |  | 3: CARDFILE PACKAGER PROGMAN |
| `0x05f` | seg1→imported thunk | 10 | GETDLGITEMINT | `(word s_word ptr word)` | pointer | ✅ |  | 3: PBRUSH TERMINAL WINMINE |
| `0x064` | seg1→imported thunk | 12 | DLGDIRLIST | `(word str word word word)` | pointer | ✅ |  | 3: RECORDER SYSEDIT TERMINAL |
| `0x069` | seg1→imported thunk | 4 | FLASHWINDOW | `(word word)` | values | ✅ |  | 3: RECORDER TERMINAL WRITE |
| `0x07f` | seg1→imported thunk | 6 | VALIDATERECT | `(word ptr)` | pointer | ✅ |  | 3: PROGMAN TERMINAL WRITE |
| `0x082` | seg1→imported thunk | 6 | SETCLASSWORD | `(word s_word word)` | values | ✅ | ok 36 | 3: CARDFILE PBRUSH PROGMAN |
| `0x099` | seg1→imported thunk | 12 | CHANGEMENU | `(word word segstr word word)` | pointer | ✅ | ok 1 | 3: RECORDER TERMINAL WRITE |
| `0x0a0` | seg1→imported thunk | 2 | DRAWMENUBAR | `(word)` | values | ✅ | ok 7 | 3: PBRUSH SOUNDREC WINFILE |
| `0x0e1` | seg1→imported thunk | 10 | ENUMTASKWINDOWS | `(word segptr long)` | callback | ✅ |  | 3: PACKAGER CARDFILE* WRITE* |
| `0x0fa` | seg1→imported thunk | 6 | GETMENUSTATE | `(word word word)` | values | ✅ |  | 3: PBRUSH WINFILE WRITE |
| `0x110` | seg1→imported thunk | 2 | ISZOOMED | `(word)` | values | ✅ | ok 2 | 3: CLOCK PROGMAN TERMINAL |
| `0x115` | seg1→imported thunk | 2 | GETDLGCTRLID | `(word)` | values | ✅ | ok 50 | 3: CHARMAP SOUNDREC WINFILE* |
| `0x134` | seg1→imported thunk | 10 | DEFDLGPROC | `(word word word long)` | callback | ✅ | ok 289 | 3: CHARMAP MPLAYER SOUNDREC |
| `0x172` | seg1→imported thunk | 6 | GETWINDOWPLACEMENT | `(word ptr)` | pointer | ✅ |  | 3: MPLAYER PROGMAN WINFILE |
| `0x19b` | seg1→imported thunk | 10 | APPENDMENU | `(word word word segptr)` | pointer | ✅ | ok 20 | 3: CARDFILE CLOCK MPLAYER |
| `0x19e` | seg1→imported thunk | 12 | MODIFYMENU | `(word word word word segptr)` | pointer | ✅ | ok 2 | 3: PBRUSH PROGMAN SOUNDREC |
| `0x19f` | seg1→imported thunk | 0 | CREATEPOPUPMENU | `()` | values | ✅ |  | 3: CARDFILE PACKAGER WRITE |
| `0x1bd` | seg1→imported thunk | 12 | DEFFRAMEPROC | `(word word word word long)` | callback | ✅ | ok 8 | 3: PROGMAN SYSEDIT WINFILE |
| `0x1bf` | seg1→imported thunk | 10 | DEFMDICHILDPROC | `(word word word long)` | callback | ✅ | ok 448 | 3: PROGMAN SYSEDIT WINFILE |
| `0x1c3` | seg1→imported thunk | 6 | TRANSLATEMDISYSACCEL | `(word ptr)` | pointer | ✅ |  | 3: PROGMAN SYSEDIT WINFILE |
| `0x007` | seg1→imported thunk | 6 | EXITWINDOWS | `(long word)` | values | ✅ |  | 2: PROGMAN WINFILE |
| `0x010` | seg1→imported thunk | 4 | CLIPCURSOR | `(ptr)` | pointer | ✅ |  | 2: PBRUSH WINFILE |
| `0x015` | seg1→imported thunk | 0 | GETDOUBLECLICKTIME | `()` | values | ✅ | ok 2 | 2: WINFILE RECORDER* |
| `0x01e` | seg1→imported thunk | 4 | WINDOWFROMPOINT | `(long)` | values | ✅ |  | 2: CHARMAP RECORDER* |
| `0x030` | seg1→imported thunk | 4 | ISCHILD | `(word word)` | values | ✅ |  | 2: TERMINAL RECORDER* |
| `0x032` | seg1→imported thunk | 8 | FINDWINDOW | `(str str)` | pointer | ✅ | ok 99 | 2: RECORDER WINMINE |
| `0x07a` | seg1→imported thunk | 14 | CALLWINDOWPROC | `(segptr word word word long)` | callback | ✅ |  | 2: CARDFILE WINFILE* |
| `0x08c` | seg1→imported thunk | 0 | GETCLIPBOARDOWNER | `()` | values | ✅ |  | 2: SOUNDREC WRITE |
| `0x098` | seg1→imported thunk | 2 | DESTROYMENU | `(word)` | values | ✅ |  | 2: PACKAGER WRITE |
| `0x09e` | seg1→imported thunk | 4 | SETMENU | `(word word)` | values | ✅ | ok 2 | 2: WINMINE WINFILE* |
| `0x0a9` | seg1→imported thunk | 0 | GETCARETBLINKTIME | `()` | values | ✅ |  | 2: TERMINAL WRITE |
| `0x0be` | seg1→imported thunk | 8 | GETUPDATERECT | `(word ptr word)` | pointer | ✅ | ok 60 | 2: CHARMAP WRITE |
| `0x0c4` | seg1→imported thunk | 20 | TABBEDTEXTOUT | `(word s_word s_word ptr s_word s_word ptr s_word)` | pointer | ✅ |  | 2: CARDFILE NOTEPAD |
| `0x0c9` | seg1→imported thunk | 4 | SETCOMMSTATE | `(ptr)` | pointer | ✅ |  | 2: CARDFILE TERMINAL |
| `0x0ca` | seg1→imported thunk | 6 | GETCOMMSTATE | `(word ptr)` | pointer | ✅ |  | 2: CARDFILE TERMINAL |
| `0x0cb` | seg1→imported thunk | 6 | GETCOMMERROR | `(word ptr)` | pointer | ✅ |  | 2: CARDFILE TERMINAL |
| `0x0cd` | seg1→imported thunk | 8 | WRITECOMM | `(word ptr word)` | pointer | ✅ |  | 2: CARDFILE TERMINAL |
| `0x0d7` | seg1→imported thunk | 4 | FLUSHCOMM | `(word word)` | values | ✅ |  | 2: CARDFILE TERMINAL |
| `0x0de` | seg1→imported thunk | 4 | GETKEYBOARDSTATE | `(ptr)` | pointer | ✅ |  | 2: RECORDER TERMINAL |
| `0x0df` | seg1→imported thunk | 4 | SETKEYBOARDSTATE | `(ptr)` | pointer | ✅ |  | 2: RECORDER TERMINAL |
| `0x0e5` | seg1→imported thunk | 2 | GETTOPWINDOW | `(word)` | values | ✅ | ok 25 | 2: PACKAGER TERMINAL |
| `0x0f9` | seg1→imported thunk | 2 | GETASYNCKEYSTATE | `(word)` | values | ✅ |  | 2: CLOCK RECORDER |
| `0x107` | seg1→imported thunk | 2 | GETMENUITEMCOUNT | `(word)` | values | ✅ |  | 2: TERMINAL WINFILE |
| `0x120` | seg1→imported thunk | 0 | GETMESSAGEEXTRAINFO | `()` | values | ✅ |  | 2: PBRUSH WINFILE |
| `0x173` | seg1→imported thunk | 6 | SETWINDOWPLACEMENT | `(word ptr)` | pointer | ✅ |  | 2: PROGMAN WINFILE |
| `0x1c9` | seg1→imported thunk | 2 | DESTROYICON | `(word)` | values | ✅ |  | 2: PACKAGER PROGMAN |
| `0x1e3` | seg1→imported thunk | 10 | SYSTEMPARAMETERSINFO | `(word word ptr word)` | pointer | ✅ | ok 6 part 8 | 2: MPLAYER PROGMAN |
| `0x066` | seg1→imported thunk | 10 | ADJUSTWINDOWRECT | `(ptr long word)` | pointer | ✅ | ok 2 | 1: SOL |
| `0x067` | seg1→imported thunk | 6 | MAPDIALOGRECT | `(word ptr)` | pointer | ✅ | ok 288 | 1: CALC |
| `0x07e` | seg1→imported thunk | 6 | INVALIDATERGN | `(word word word)` | values | ✅ |  | 1: PROGMAN |
| `0x081` | seg1→imported thunk | 4 | GETCLASSWORD | `(word s_word)` | values | ✅ |  | 1: PROGMAN |
| `0x0a1` | seg1→imported thunk | 12 | GETMENUSTRING | `(word word ptr s_word word)` | pointer | ✅ |  | 1: WINFILE |
| `0x0a2` | seg1→imported thunk | 8 | HILITEMENUITEM | `(word word word word)` | values | ✅ |  | 1: WRITE |
| `0x0aa` | seg1→imported thunk | 2 | ARRANGEICONICWINDOWS | `(word)` | values | ✅ |  | 1: TASKMAN |
| `0x0ac` | seg1→imported thunk | 4 | SWITCHTOTHISWINDOW | `(word word)` | values | ✅ |  | 1: TASKMAN |
| `0x0c0` | seg1→imported thunk | 0 | INSENDMESSAGE | `()` | callback | ✅ |  | 1: WRITE |
| `0x0cc` | seg1→imported thunk | 8 | READCOMM | `(word ptr word)` | pointer | ✅ |  | 1: TERMINAL |
| `0x0d0` | seg1→imported thunk | 4 | SETCOMMEVENTMASK | `(word word)` | values | ✅ |  | 1: TERMINAL |
| `0x0d2` | seg1→imported thunk | 2 | SETCOMMBREAK | `(word)` | values | ✅ |  | 1: TERMINAL |
| `0x0d3` | seg1→imported thunk | 2 | CLEARCOMMBREAK | `(word)` | values | ✅ |  | 1: TERMINAL |
| `0x0d6` | seg1→imported thunk | 4 | ESCAPECOMMFUNCTION | `(word word)` | values | ✅ |  | 1: TERMINAL |
| `0x0dd` | seg1→imported thunk | 20 | SCROLLDC | `(word s_word s_word ptr ptr word ptr)` | pointer | ✅ |  | 1: WRITE |
| `0x0e4` | seg1→imported thunk | 6 | GETNEXTDLGTABITEM | `(word word word)` | values | ✅ |  | 1: CHARMAP |
| `0x0e6` | seg1→imported thunk | 4 | GETNEXTWINDOW | `(word word)` | values | ✅ |  | 1: TERMINAL |
| `0x0f3` | seg1→imported thunk | 0 | GETDIALOGBASEUNITS | `()` | values | ✅ |  | 1: CLOCK |
| `0x108` | seg1→imported thunk | 4 | GETMENUITEMID | `(word word)` | values | ✅ |  | 1: WINFILE |
| `0x10b` | seg1→imported thunk | 6 | SHOWSCROLLBAR | `(word word word)` | values | ✅ | ok 72 | 1: CARDFILE |
| `0x1c4` | seg1→imported thunk | 34 | CREATEWINDOWEX | `(long str str long s_word s_word s_word s_word word word word segptr)` | pointer | ✅ |  | 1: WINFILE |
| `0x1ce` | seg1→imported thunk | 4 | CALCCHILDSCROLL | `(word word)` | values | ✅ |  | 1: PROGMAN |
| `0x00b` | seg1→imported thunk | 10 | BEAR11 | `(word word word segptr)` | pointer | ✅ |  | 0 |
| `0x00e` | seg1→imported thunk | 0 | GETTIMERRESOLUTION | `()` | values | ✅ |  | 0 |
| `0x024` | seg1→imported thunk | 8 | (WOWUSER_GETWINDOWTEXT) |  | ? | ✅ | ok 95 | 0 |
| `0x083` | seg1→imported thunk | 4 | GETCLASSLONG | `(word s_word)` | values | ✅ |  | 0 |
| `0x08d` | seg1→imported thunk | 4 | (WOWUSER_SETCLIPBOARDDATA) |  | ? | ✅ |  | 0 |
| `0x096` | seg1→imported thunk | 16 | (WOWUSER_LOADMENU) |  | ? | ✅ | ok 4 | 0 |
| `0x0ad` | seg1→imported thunk | 20 | (WOWUSER_LOADSYSOBJ) |  | ? | ✅ | ok 868 | 0 |
| `0x0af` | seg1→imported thunk | 14 | (WOWUSER_LOADBITMAPRES) |  | ? | ✅ | ok 30 | 0 |
| `0x0c8` | seg1→imported thunk | 12 | (WOWUSER_OPENCOMM) |  | ? | ✅ |  | 0 |
| `0x0ce` | seg1→imported thunk | 4 | TRANSMITCOMMCHAR | `(word word)` | values | ✅ |  | 0 |
| `0x0cf` | seg1→imported thunk | 6 | (WOWUSER_CLOSECOMM) |  | ? | ✅ |  | 0 |
| `0x0ef` | seg1→imported thunk | 22 | (WOWUSER_CREATEDIALOG) |  | ? | ✅ | ok 59 | 0 |
| `0x16c` | seg1→imported thunk | 12 | LOOKUPICONIDFROMDIRECTORYEX | `(ptr word word word word)` | pointer | ✅ | ok 213 | 0 |
| `0x1e2` | seg1→imported thunk | 6 | ENABLESCROLLBAR | `(word word word)` | values | ✅ | **STEPPED 16** | 0 |
| `0x217` | seg1→imported thunk | 6 | NOTIFYWOW | `()` | values | ✅ | ok 65 **STEPPED 344** | 0 |

## GDI — 365 ids, 98 handled, 9 gaps

Unhandled first, then most-used.

| id | table | args | name | Wine signature | kind | handled | rig | shelf |
|---|---|---:|---|---|---|---|---|---|
| `0x07b` | seg1→imported thunk | 4 | PLAYMETAFILE | `(word word)` | values | — |  | 4: PBRUSH WRITE CARDFILE* PACKAGER* |
| `0x05e` | seg1→imported thunk | 2 | GETVIEWPORTEXT | `(word)` | values | — |  | 3: CARDFILE* PACKAGER* WRITE* |
| `0x060` | seg1→imported thunk | 2 | GETWINDOWEXT | `(word)` | values | — |  | 3: CARDFILE* PACKAGER* WRITE* |
| `0x0a2` | seg1→imported thunk | 2 | GETBITMAPDIMENSION | `(word)` | values | — |  | 3: CARDFILE* PACKAGER* WRITE* |
| `0x0af` | seg1→imported thunk | 12 | ENUMMETAFILE | `(word word segptr long)` | callback | — |  | 3: CARDFILE* PACKAGER* WRITE* |
| `0x0b0` | seg1→imported thunk | 12 | PLAYMETAFILERECORD | `(word ptr ptr word)` | pointer | — |  | 3: CARDFILE* PACKAGER* WRITE* |
| `0x046` | seg1→imported thunk | 14 | ENUMFONTS | `(word str segptr long)` | callback | — | **STEPPED 24** | 2: TERMINAL WRITE |
| `0x047` | seg1→imported thunk | 12 | ENUMOBJECTS | `(word word segptr long)` | callback | — |  | 1: PBRUSH |
| `0x16c` | seg1→imported thunk | 10 | SETPALETTEENTRIES | `(word word word ptr)` | pointer | — |  | 1: DDEML |
| `0x005` | seg1→imported thunk | 4 | SETRELABS | `(word word)` | values | — |  | 0 |
| `0x008` | seg1→imported thunk | 4 | SETTEXTCHARACTEREXTRA | `(word s_word)` | values | — |  | 0 |
| `0x00f` | seg1→imported thunk | 6 | OFFSETWINDOWORG | `(word s_word s_word)` | values | — |  | 0 |
| `0x010` | seg1→imported thunk | 10 | SCALEWINDOWEXT | `(word s_word s_word s_word s_word)` | values | — |  | 0 |
| `0x011` | seg1→imported thunk | 6 | OFFSETVIEWPORTORG | `(word s_word s_word)` | values | — |  | 0 |
| `0x012` | seg1→imported thunk | 10 | SCALEVIEWPORTEXT | `(word s_word s_word s_word s_word)` | values | — |  | 0 |
| `0x017` | seg1→imported thunk | 18 | ARC | `(word s_word s_word s_word s_word s_word s_word s_word s_word)` | values | — |  | 0 |
| `0x019` | seg1→imported thunk | 10 | FLOODFILL | `(word s_word s_word long)` | values | — |  | 0 |
| `0x01a` | seg1→imported thunk | 18 | PIE | `(word s_word s_word s_word s_word s_word s_word s_word s_word)` | values | — |  | 0 |
| `0x020` | seg1→imported thunk | 6 | OFFSETCLIPRGN | `(word s_word s_word)` | values | — |  | 0 |
| `0x028` | seg1→imported thunk | 6 | FILLRGN | `(word word word)` | values | — |  | 0 |
| `0x029` | seg1→imported thunk | 10 | FRAMERGN | `(word word word word word)` | values | — |  | 0 |
| `0x02a` | seg1→imported thunk | 4 | INVERTRGN | `(word word)` | values | — |  | 0 |
| `0x02b` | seg1→imported thunk | 4 | PAINTRGN | `(word word)` | values | — |  | 0 |
| `0x02e` | seg1→imported thunk | 10 | (internal) |  | ? | — |  | 0 |
| `0x032` | seg1→imported thunk | 4 | CREATEBRUSHINDIRECT | `(ptr)` | pointer | — |  | 0 |
| `0x036` | seg1→imported thunk | 8 | CREATEELLIPTICRGN | `(s_word s_word s_word s_word)` | values | — |  | 0 |
| `0x037` | seg1→imported thunk | 4 | CREATEELLIPTICRGNINDIRECT | `(ptr)` | pointer | — |  | 0 |
| `0x03e` | seg1→imported thunk | 4 | CREATEPENINDIRECT | `(ptr)` | pointer | — |  | 0 |
| `0x048` | seg1→imported thunk | 4 | EQUALRGN | `(word word)` | values | — |  | 0 |
| `0x049` | seg1→imported thunk | 10 | EXCLUDEVISRECT | `(word s_word s_word s_word s_word)` | values | — |  | 0 |
| `0x04e` | seg1→imported thunk | 2 | GETCURRENTPOSITION | `(word)` | values | — |  | 0 |
| `0x056` | seg1→imported thunk | 2 | GETRELABS | `(word)` | values | — |  | 0 |
| `0x059` | seg1→imported thunk | 2 | GETTEXTCHARACTEREXTRA | `(word)` | values | — |  | 0 |
| `0x05f` | seg1→imported thunk | 2 | GETVIEWPORTORG | `(word)` | values | — |  | 0 |
| `0x061` | seg1→imported thunk | 2 | GETWINDOWORG | `(word)` | values | — |  | 0 |
| `0x062` | seg1→imported thunk | 10 | INTERSECTVISRECT | `(word s_word s_word s_word s_word)` | values | — |  | 0 |
| `0x065` | seg1→imported thunk | 6 | OFFSETRGN | `(word s_word s_word)` | values | — |  | 0 |
| `0x066` | seg1→imported thunk | 6 | OFFSETVISRGN | `(word s_word s_word)` | values | — |  | 0 |
| `0x069` | seg1→imported thunk | 4 | SELECTVISRGN | `(word word)` | values | — |  | 0 |
| `0x075` | seg1→imported thunk | 6 | SETDCORG | `(word s_word s_word)` | values | — |  | 0 |
| `0x076` | seg1→imported thunk | 16 | (internal) |  | ? | — |  | 0 |
| `0x077` | seg1→imported thunk | 4 | (internal) |  | ? | — |  | 0 |
| `0x079` | seg1→imported thunk | 2 | DEATH | `(word)` | values | — |  | 0 |
| `0x07a` | seg1→imported thunk | 14 | RESURRECTION | `(word word word word word word word)` | values | — |  | 0 |
| `0x07c` | seg1→imported thunk | 4 | GETMETAFILE | `(str)` | pointer | — |  | 0 |
| `0x081` | seg1→imported thunk | 2 | SAVEVISRGN | `(word)` | values | — |  | 0 |
| `0x082` | seg1→imported thunk | 2 | RESTOREVISRGN | `(word)` | values | — |  | 0 |
| `0x083` | seg1→imported thunk | 2 | INQUIREVISRGN | `(word)` | values | — |  | 0 |
| `0x084` | seg1→imported thunk | 10 | SETENVIRONMENT | `(str str word)` | pointer | — |  | 0 |
| `0x085` | seg1→imported thunk | 10 | GETENVIRONMENT | `(str str word)` | pointer | — |  | 0 |
| `0x086` | seg1→imported thunk | 6 | GETRGNBOX | `(word ptr)` | pointer | — |  | 0 |
| `0x087` | seg1→imported thunk | 12 | SCANLR |  | ? | — |  | 0 |
| `0x088` | seg1→imported thunk | 4 | (internal) |  | ? | — |  | 0 |
| `0x089` | seg1→imported thunk | 2 | (internal) |  | ? | — |  | 0 |
| `0x08a` | seg1→imported thunk | 8 | (internal) |  | ? | — |  | 0 |
| `0x08b` | seg1→imported thunk | 6 | (internal) |  | ? | — |  | 0 |
| `0x08c` | seg1→imported thunk | 10 | (internal) |  | ? | — |  | 0 |
| `0x08d` | seg1→imported thunk | 4 | (internal) |  | ? | — |  | 0 |
| `0x08e` | seg1→imported thunk | 10 | (internal) |  | ? | — |  | 0 |
| `0x08f` | seg1→imported thunk | 6 | (internal) |  | ? | — |  | 0 |
| `0x09d` | seg1→imported thunk | 6 | (internal) |  | ? | — |  | 0 |
| `0x09e` | seg1→imported thunk | 14 | (internal) |  | ? | — |  | 0 |
| `0x0a1` | seg1→imported thunk | 6 | PTINREGION | `(word s_word s_word)` | values | — |  | 0 |
| `0x0a4` | seg1→imported thunk | 16 | (internal) |  | ? | — |  | 0 |
| `0x0a9` | seg1→imported thunk | 6 | ISDCDIRTY | `()` | values | — |  | 0 |
| `0x0aa` | seg1→imported thunk | 8 | SETDCSTATUS | `()` | values | — |  | 0 |
| `0x0ab` | seg1→imported thunk | 10 | (internal) |  | ? | — |  | 0 |
| `0x0ad` | seg1→imported thunk | 2 | GETCLIPRGN | `(word)` | values | — |  | 0 |
| `0x0b1` | seg1→imported thunk | 4 | (internal) |  | ? | — |  | 0 |
| `0x0b2` | seg1→imported thunk | 4 | (internal) |  | ? | — |  | 0 |
| `0x0b3` | seg1→imported thunk | 2 | GETDCSTATE | `(word)` | values | — |  | 0 |
| `0x0b4` | seg1→imported thunk | 4 | SETDCSTATE | `(word word)` | values | — |  | 0 |
| `0x0b5` | seg1→imported thunk | 6 | RECTINREGION | `(word ptr)` | pointer | — |  | 0 |
| `0x0b8` | seg1→imported thunk | 6 | (internal) |  | ? | — |  | 0 |
| `0x0b9` | seg1→imported thunk | 6 | (internal) |  | ? | — |  | 0 |
| `0x0bc` | seg1→imported thunk | 20 | GETTEXTEXTENTEX | `()` | values | — |  | 0 |
| `0x0be` | seg1→imported thunk | 10 | SETDCHOOK | `(word segptr long)` | callback | — |  | 0 |
| `0x0bf` | seg1→imported thunk | 6 | GETDCHOOK | `(word ptr)` | callback | — |  | 0 |
| `0x0c0` | seg1→imported thunk | 4 | SETHOOKFLAGS | `(word word)` | callback | — |  | 0 |
| `0x0c1` | seg1→imported thunk | 8 | SETBOUNDSRECT | `(word ptr word)` | pointer | — |  | 0 |
| `0x0c2` | seg1→imported thunk | 8 | GETBOUNDSRECT | `(word ptr word)` | pointer | — |  | 0 |
| `0x0c3` | seg1→imported thunk | 4 | SELECTBITMAP | `()` | values | — |  | 0 |
| `0x0c9` | seg1→imported thunk | 32 | DMBITBLT | `()` | values | — |  | 0 |
| `0x0ca` | seg1→imported thunk | 12 | DMCOLORINFO | `()` | values | — |  | 0 |
| `0x0ce` | seg1→imported thunk | 16 | DMENUMDFONTS | `()` | callback | — |  | 0 |
| `0x0cf` | seg1→imported thunk | 14 | DMENUMOBJ | `()` | callback | — |  | 0 |
| `0x0d0` | seg1→imported thunk | 28 | DMOUTPUT | `()` | values | — |  | 0 |
| `0x0d1` | seg1→imported thunk | 16 | DMPIXEL | `()` | values | — |  | 0 |
| `0x0d2` | seg1→imported thunk | 18 | DMREALIZEOBJECT | `()` | values | — |  | 0 |
| `0x0d3` | seg1→imported thunk | 30 | DMSTRBLT | `()` | values | — |  | 0 |
| `0x0d4` | seg1→imported thunk | 14 | DMSCANLR | `()` | values | — |  | 0 |
| `0x0d6` | seg1→imported thunk | 40 | DMEXTTEXTOUT | `()` | values | — |  | 0 |
| `0x0d7` | seg1→imported thunk | 24 | DMGETCHARWIDTH | `()` | values | — |  | 0 |
| `0x0d8` | seg1→imported thunk | 40 | DMSTRETCHBLT | `()` | values | — |  | 0 |
| `0x0d9` | seg1→imported thunk | 26 | DMDIBBITS | `()` | values | — |  | 0 |
| `0x0da` | seg1→imported thunk | 50 | DMSTRETCHDIBITS | `()` | values | — |  | 0 |
| `0x0db` | seg1→imported thunk | 32 | DMSETDIBTODEV | `()` | values | — |  | 0 |
| `0x0dc` | seg1→imported thunk | 10 | DMTRANSPOSE | `()` | values | — |  | 0 |
| `0x0f0` | seg1→imported thunk | 10 | OPENJOB | `(str str word)` | pointer | — |  | 0 |
| `0x0f1` | seg1→imported thunk | 8 | WRITESPOOL | `(word ptr word)` | pointer | — |  | 0 |
| `0x0f2` | seg1→imported thunk | 8 | WRITEDIALOG | `(word str word)` | pointer | — |  | 0 |
| `0x0f3` | seg1→imported thunk | 2 | CLOSEJOB | `(word)` | values | — |  | 0 |
| `0x0f4` | seg1→imported thunk | 4 | DELETEJOB | `(word word)` | values | — |  | 0 |
| `0x0f5` | seg1→imported thunk | 6 | (internal) |  | ? | — |  | 0 |
| `0x0f6` | seg1→imported thunk | 2 | STARTSPOOLPAGE | `(word)` | values | — |  | 0 |
| `0x0f7` | seg1→imported thunk | 2 | ENDSPOOLPAGE | `(word)` | values | — |  | 0 |
| `0x0f8` | seg1→imported thunk | 4 | QUERYJOB | `()` | values | — |  | 0 |
| `0x0fa` | seg1→imported thunk | 10 | COPY | `(ptr ptr word)` | pointer | — |  | 0 |
| `0x0fd` | seg1→imported thunk | 2 | DELETESPOOLPAGE | `()` | values | — |  | 0 |
| `0x0fe` | seg1→imported thunk | 16 | SPOOLFILE | `()` | values | — |  | 0 |
| `0x105` | seg1→imported thunk | 4 | (internal) |  | ? | — |  | 0 |
| `0x10a` | seg1→imported thunk | 12 | OPENPRINTERA |  | ? | — |  | 0 |
| `0x10b` | seg1→imported thunk | 12 | STARTDOCPRINTERA | `()` | values | — |  | 0 |
| `0x10c` | seg1→imported thunk | 4 | STARTPAGEPRINTER | `()` | values | — |  | 0 |
| `0x10d` | seg1→imported thunk | 16 | WRITEPRINTER | `()` | values | — |  | 0 |
| `0x10e` | seg1→imported thunk | 4 | ENDPAGEPRINTER | `()` | values | — |  | 0 |
| `0x10f` | seg1→imported thunk | 4 | ABORTPRINTER | `()` | values | — |  | 0 |
| `0x110` | seg1→imported thunk | 4 | ENDDOCPRINTER | `()` | values | — |  | 0 |
| `0x112` | seg1→imported thunk | 4 | CLOSEPRINTER | `()` | values | — |  | 0 |
| `0x118` | seg1→imported thunk | 12 | GETREALDRIVERINFO | `()` | values | — |  | 0 |
| `0x119` | seg1→imported thunk | 20 | DRVSETPRINTERDATA | `(str str long ptr long)` | pointer | — |  | 0 |
| `0x11a` | seg1→imported thunk | 24 | DRVGETPRINTERDATA | `(str str ptr ptr long ptr)` | pointer | — |  | 0 |
| `0x12b` | seg1→imported thunk | 12 | ENGINEGETCHARWIDTHEX | `()` | values | — |  | 0 |
| `0x12c` | seg1→imported thunk | 12 | ENGINEENUMERATEFONT | `(str segptr long)` | callback | — |  | 0 |
| `0x12d` | seg1→imported thunk | 4 | ENGINEDELETEFONT | `(ptr)` | pointer | — |  | 0 |
| `0x12e` | seg1→imported thunk | 12 | ENGINEREALIZEFONT | `(ptr ptr ptr)` | pointer | — |  | 0 |
| `0x12f` | seg1→imported thunk | 12 | ENGINEGETCHARWIDTH | `(ptr word word ptr)` | pointer | — |  | 0 |
| `0x130` | seg1→imported thunk | 6 | ENGINESETFONTCONTEXT | `(ptr word)` | pointer | — |  | 0 |
| `0x131` | seg1→imported thunk | 22 | ENGINEGETGLYPHBMPEXT/ENGINEGETGLYPHBMP | `()` | values | — |  | 0 |
| `0x132` | seg1→imported thunk | 10 | ENGINEMAKEFONTDIR | `(word ptr ptr)` | pointer | — |  | 0 |
| `0x134` | seg1→imported thunk | 8 | GETOUTLINETEXTMETRICS | `(word word ptr)` | pointer | — |  | 0 |
| `0x135` | seg1→imported thunk | 22 | GETGLYPHOUTLINE | `(word word word ptr long ptr ptr)` | pointer | — |  | 0 |
| `0x136` | seg1→imported thunk | 14 | CREATESCALABLEFONTRESOURCE | `(word str str str)` | pointer | — |  | 0 |
| `0x137` | seg1→imported thunk | 18 | GETFONTDATA | `(word long long ptr long)` | pointer | — |  | 0 |
| `0x138` | seg1→imported thunk | 12 | CONVERTOUTLINEFONTFILE | `()` | values | — |  | 0 |
| `0x139` | seg1→imported thunk | 6 | GETRASTERIZERCAPS | `(ptr word)` | pointer | — |  | 0 |
| `0x13a` | seg1→imported thunk | 42 | ENGINEEXTTEXTOUT | `()` | values | — |  | 0 |
| `0x13b` | seg1→imported thunk | 16 | ENGINEREALIZEFONTEXT | `(long long long long)` | values | — |  | 0 |
| `0x13c` | seg1→imported thunk | 14 | ENGINEGETCHARWIDTHSTR | `()` | values | — |  | 0 |
| `0x14c` | seg1→imported thunk | 8 | GETKERNINGPAIRS | `(word word ptr)` | pointer | — |  | 0 |
| `0x159` | seg1→imported thunk | 2 | GETTEXTALIGN | `(word)` | values | — |  | 0 |
| `0x15b` | seg1→imported thunk | 14 | (internal) |  | ? | — |  | 0 |
| `0x15c` | seg1→imported thunk | 18 | CHORD | `(word s_word s_word s_word s_word s_word s_word s_word s_word)` | values | — |  | 0 |
| `0x15d` | seg1→imported thunk | 6 | SETMAPPERFLAGS | `(word long)` | values | — |  | 0 |
| `0x160` | seg1→imported thunk | 2 | GETPHYSICALFONTHANDLE | `()` | values | — |  | 0 |
| `0x161` | seg1→imported thunk | 2 | GETASPECTRATIOFILTER | `()` | values | — |  | 0 |
| `0x162` | seg1→imported thunk | 0 | SHRINKGDIHEAP | `()` | values | — |  | 0 |
| `0x169` | seg1→imported thunk | 6 | GDISELECTPALETTE | `(word word word)` | values | — |  | 0 |
| `0x16a` | seg1→imported thunk | 2 | GDIREALIZEPALETTE | `(word)` | values | — |  | 0 |
| `0x16d` | seg1→imported thunk | 2 | REALIZEDEFAULTPALETTE | `(word)` | values | — |  | 0 |
| `0x16f` | seg1→imported thunk | 10 | ANIMATEPALETTE | `(word word word ptr)` | pointer | — |  | 0 |
| `0x170` | seg1→imported thunk | 4 | RESIZEPALETTE | `(word word)` | values | — |  | 0 |
| `0x175` | seg1→imported thunk | 4 | SETSYSTEMPALETTEUSE | `(word word)` | values | — |  | 0 |
| `0x176` | seg1→imported thunk | 2 | GETSYSTEMPALETTEUSE | `(word)` | values | — |  | 0 |
| `0x177` | seg1→imported thunk | 10 | GETSYSTEMPALETTEENTRIES | `(word word word ptr)` | pointer | — |  | 0 |
| `0x178` | seg1→imported thunk | 6 | RESETDC | `(word ptr)` | pointer | — |  | 0 |
| `0x179` | seg1→imported thunk | 6 | STARTDOC | `(word ptr)` | pointer | — |  | 0 |
| `0x17a` | seg1→imported thunk | 2 | ENDDOC | `(word)` | values | — |  | 0 |
| `0x17b` | seg1→imported thunk | 2 | STARTPAGE | `(word)` | values | — |  | 0 |
| `0x17c` | seg1→imported thunk | 2 | ENDPAGE | `(word)` | values | — |  | 0 |
| `0x17d` | seg1→imported thunk | 6 | SETABORTPROC | `(word segptr)` | callback | — |  | 0 |
| `0x17e` | seg1→imported thunk | 2 | ABORTDOC | `(word)` | values | — |  | 0 |
| `0x190` | seg1→imported thunk | 14 | FASTWINDOWFRAME | `(word ptr s_word s_word long)` | pointer | — |  | 0 |
| `0x191` | seg1→imported thunk | 2 | GDIMOVEBITMAP | `()` | values | — |  | 0 |
| `0x193` | seg1→imported thunk | 4 | GDIINIT2 | `(word word)` | values | — |  | 0 |
| `0x194` | seg1→imported thunk | 6 | GETTTGLYPHINDEXMAP | `()` | values | — |  | 0 |
| `0x195` | seg1→imported thunk | 2 | FINALGDIINIT | `(word)` | values | — |  | 0 |
| `0x196` | seg1→imported thunk | 6 | (internal) |  | ? | — |  | 0 |
| `0x197` | seg1→imported thunk | 12 | CREATEUSERBITMAP | `(word word word word ptr)` | pointer | — |  | 0 |
| `0x198` | seg1→imported thunk | 14 | (internal) |  | ? | — |  | 0 |
| `0x199` | seg1→imported thunk | 6 | CREATEUSERDISCARDABLEBITMAP | `(word word word)` | values | — |  | 0 |
| `0x19a` | seg1→imported thunk | 0 | ISVALIDMETAFILE | `()` | values | — |  | 0 |
| `0x19b` | seg1→imported thunk | 2 | GETCURLOGFONT | `(word)` | values | — |  | 0 |
| `0x19c` | seg1→imported thunk | 2 | ISDCCURRENTPALETTE | `(word)` | values | — |  | 0 |
| `0x1bc` | seg1→imported thunk | 12 | CREATEROUNDRECTRGN | `(s_word s_word s_word s_word s_word s_word)` | values | — |  | 0 |
| `0x1bd` | seg1→imported thunk | 6 | (internal) |  | ? | — |  | 0 |
| `0x1c1` | seg1→imported thunk | 8 | DEVICECOLORMATCH | `()` | values | — |  | 0 |
| `0x1c2` | seg1→imported thunk | 12 | POLYPOLYGON | `(word ptr ptr word)` | pointer | — |  | 0 |
| `0x1c3` | seg1→imported thunk | 12 | CREATEPOLYPOLYGONRGN | `(ptr ptr word word)` | pointer | — |  | 0 |
| `0x1c4` | seg1→imported thunk | 12 | GDISEEGDIDO | `(word word word word)` | values | — |  | 0 |
| `0x1c5` | seg1→imported thunk | 26 | (internal) |  | ? | — |  | 0 |
| `0x1cb` | seg1→imported thunk | 2 | (internal) |  | ? | — |  | 0 |
| `0x1cc` | seg1→imported thunk | 2 | (internal) |  | ? | — |  | 0 |
| `0x1cd` | seg1→imported thunk | 4 | SETOBJECTOWNER | `(word word)` | values | — |  | 0 |
| `0x1ce` | seg1→imported thunk | 2 | ISGDIOBJECT | `(word)` | values | — |  | 0 |
| `0x1cf` | seg1→imported thunk | 4 | MAKEOBJECTPRIVATE | `(word word)` | values | — |  | 0 |
| `0x1d0` | seg1→imported thunk | 6 | FIXUPBOGUSPUBLISHERMETAFILE | `()` | values | — |  | 0 |
| `0x1d1` | seg1→imported thunk | 6 | RECTVISIBLE_EHH | `(word ptr)` | pointer | — |  | 0 |
| `0x1d2` | seg1→imported thunk | 6 | RECTINREGION_EHH | `(word ptr)` | pointer | — |  | 0 |
| `0x1d3` | seg1→imported thunk | 8 | UNICODETOANSI | `()` | values | — |  | 0 |
| `0x1d4` | seg1→imported thunk | 6 | GETBITMAPDIMENSIONEX | `(word ptr)` | pointer | — |  | 0 |
| `0x1d5` | seg1→imported thunk | 6 | GETBRUSHORGEX | `(word ptr)` | pointer | — |  | 0 |
| `0x1d6` | seg1→imported thunk | 6 | GETCURRENTPOSITIONEX | `(word ptr)` | pointer | — |  | 0 |
| `0x1d7` | seg1→imported thunk | 12 | GETTEXTEXTENTPOINT | `(word ptr s_word ptr)` | pointer | — |  | 0 |
| `0x1d8` | seg1→imported thunk | 6 | GETVIEWPORTEXTEX | `(word ptr)` | pointer | — |  | 0 |
| `0x1d9` | seg1→imported thunk | 6 | GETVIEWPORTORGEX | `(word ptr)` | pointer | — |  | 0 |
| `0x1da` | seg1→imported thunk | 6 | GETWINDOWEXTEX | `(word ptr)` | pointer | — |  | 0 |
| `0x1db` | seg1→imported thunk | 6 | GETWINDOWORGEX | `(word ptr)` | pointer | — |  | 0 |
| `0x1dc` | seg1→imported thunk | 10 | OFFSETVIEWPORTORGEX | `(word s_word s_word ptr)` | pointer | — |  | 0 |
| `0x1dd` | seg1→imported thunk | 10 | OFFSETWINDOWORGEX | `(word s_word s_word ptr)` | pointer | — |  | 0 |
| `0x1de` | seg1→imported thunk | 10 | SETBITMAPDIMENSIONEX | `(word s_word s_word ptr)` | pointer | — |  | 0 |
| `0x1df` | seg1→imported thunk | 10 | SETVIEWPORTEXTEX | `(word s_word s_word ptr)` | pointer | — |  | 0 |
| `0x1e0` | seg1→imported thunk | 10 | SETVIEWPORTORGEX | `(word s_word s_word ptr)` | pointer | — |  | 0 |
| `0x1e1` | seg1→imported thunk | 10 | SETWINDOWEXTEX | `(word s_word s_word ptr)` | pointer | — |  | 0 |
| `0x1e2` | seg1→imported thunk | 10 | SETWINDOWORGEX | `(word s_word s_word ptr)` | pointer | — |  | 0 |
| `0x1e3` | seg1→imported thunk | 10 | MOVETOEX | `(word s_word s_word ptr)` | pointer | — |  | 0 |
| `0x1e4` | seg1→imported thunk | 14 | SCALEVIEWPORTEXTEX | `(word s_word s_word s_word s_word ptr)` | pointer | — |  | 0 |
| `0x1e5` | seg1→imported thunk | 14 | SCALEWINDOWEXTEX | `(word s_word s_word s_word s_word ptr)` | pointer | — |  | 0 |
| `0x1e6` | seg1→imported thunk | 6 | GETASPECTRATIOFILTEREX | `(word ptr)` | pointer | — |  | 0 |
| `0x1e7` | seg1→imported thunk | 14 | POLYPOLYLINEWOW |  | ? | — |  | 0 |
| `0x1e9` | seg1→imported thunk | 20 | CREATEDIBSECTION | `(word ptr word ptr long long)` | pointer | — |  | 0 |
| `0x1ea` | seg1→imported thunk | 2 | CLOSEENHMETAFILE | `()` | values | — |  | 0 |
| `0x1eb` | seg1→imported thunk | 6 | COPYENHMETAFILE | `()` | values | — |  | 0 |
| `0x1ec` | seg1→imported thunk | 14 | CREATEENHMETAFILE | `()` | values | — |  | 0 |
| `0x1ed` | seg1→imported thunk | 2 | DELETEENHMETAFILE | `()` | values | — |  | 0 |
| `0x1ef` | seg1→imported thunk | 10 | GDICOMMENT | `()` | values | — |  | 0 |
| `0x1f0` | seg1→imported thunk | 4 | GETENHMETAFILE | `()` | values | — |  | 0 |
| `0x1f1` | seg1→imported thunk | 10 | GETENHMETAFILEBITS | `()` | values | — |  | 0 |
| `0x1f2` | seg1→imported thunk | 10 | GETENHMETAFILEDESCRIPTION | `()` | values | — |  | 0 |
| `0x1f3` | seg1→imported thunk | 10 | GETENHMETAFILEHEADER | `()` | values | — |  | 0 |
| `0x1f5` | seg1→imported thunk | 10 | GETENHMETAFILEPALETTEENTRIES | `()` | values | — |  | 0 |
| `0x1f6` | seg1→imported thunk | 8 | POLYBEZIER | `(word ptr word)` | pointer | — |  | 0 |
| `0x1f7` | seg1→imported thunk | 8 | POLYBEZIERTO | `(word ptr word)` | pointer | — |  | 0 |
| `0x1f8` | seg1→imported thunk | 14 | PLAYENHMETAFILERECORD | `()` | values | — |  | 0 |
| `0x1f9` | seg1→imported thunk | 8 | SETENHMETAFILEBITS | `()` | values | — |  | 0 |
| `0x1fa` | seg1→imported thunk | 2 | SETMETARGN | `()` | values | — |  | 0 |
| `0x1fc` | seg1→imported thunk | 6 | EXTSELECTCLIPRGN | `(word word word)` | values | — |  | 0 |
| `0x1ff` | seg1→imported thunk | 2 | ABORTPATH | `(word)` | values | — |  | 0 |
| `0x200` | seg1→imported thunk | 2 | BEGINPATH | `(word)` | values | — |  | 0 |
| `0x201` | seg1→imported thunk | 2 | CLOSEFIGURE | `(word)` | values | — |  | 0 |
| `0x202` | seg1→imported thunk | 2 | ENDPATH | `(word)` | values | — |  | 0 |
| `0x203` | seg1→imported thunk | 2 | FILLPATH | `(word)` | values | — |  | 0 |
| `0x204` | seg1→imported thunk | 2 | FLATTENPATH | `(word)` | values | — |  | 0 |
| `0x205` | seg1→imported thunk | 14 | GETPATH | `(word ptr ptr word)` | pointer | — |  | 0 |
| `0x206` | seg1→imported thunk | 2 | PATHTOREGION | `(word)` | values | — |  | 0 |
| `0x207` | seg1→imported thunk | 4 | SELECTCLIPPATH | `(word word)` | values | — |  | 0 |
| `0x208` | seg1→imported thunk | 2 | STROKEANDFILLPATH | `(word)` | values | — |  | 0 |
| `0x209` | seg1→imported thunk | 2 | STROKEPATH | `(word)` | values | — |  | 0 |
| `0x20a` | seg1→imported thunk | 2 | WIDENPATH | `(word)` | values | — |  | 0 |
| `0x20b` | seg1→imported thunk | 20 | EXTCREATEPEN | `()` | values | — |  | 0 |
| `0x20c` | seg1→imported thunk | 2 | GETARCDIRECTION | `(word)` | values | — |  | 0 |
| `0x20d` | seg1→imported thunk | 4 | SETARCDIRECTION | `(word word)` | values | — |  | 0 |
| `0x20e` | seg1→imported thunk | 6 | GETMITERLIMIT | `()` | values | — |  | 0 |
| `0x20f` | seg1→imported thunk | 10 | SETMITERLIMIT | `()` | values | — |  | 0 |
| `0x210` | seg1→imported thunk | 10 | GDIPARAMETERSINFO | `()` | values | — |  | 0 |
| `0x211` | seg1→imported thunk | 2 | CREATEHALFTONEPALETTE | `(word)` | values | — |  | 0 |
| `0x25a` | seg1→imported thunk | 10 | SETDIBCOLORTABLE | `(word word word ptr)` | pointer | — |  | 0 |
| `0x25b` | seg1→imported thunk | 10 | GETDIBCOLORTABLE | `(word word word ptr)` | pointer | — |  | 0 |
| `0x25c` | seg1→imported thunk | 6 | SETSOLIDBRUSH | `(word long)` | values | — |  | 0 |
| `0x25d` | seg1→imported thunk | 2 | SYSDELETEOBJECT | `(word)` | values | — |  | 0 |
| `0x25e` | seg1→imported thunk | 8 | SETMAGICCOLORS | `(word long word)` | values | — |  | 0 |
| `0x25f` | seg1→imported thunk | 10 | GETREGIONDATA | `(word long ptr)` | pointer | — |  | 0 |
| `0x260` | seg1→imported thunk | 12 | EXTCREATEREGION | `()` | values | — |  | 0 |
| `0x262` | seg1→imported thunk | 14 | GDISIGNALPROC32 | `(long long long word)` | values | — |  | 0 |
| `0x263` | seg1→imported thunk | 6 | GETRANDOMRGN | `()` | values | — |  | 0 |
| `0x264` | seg1→imported thunk | 2 | GETTEXTCHARSET | `(word)` | values | — |  | 0 |
| `0x265` | seg1→imported thunk | 18 | ENUMFONTFAMILIESEX | `(word ptr segptr long long)` | callback | — |  | 0 |
| `0x266` | seg1→imported thunk | 4 | ADDLPKTOGDI | `()` | values | — |  | 0 |
| `0x267` | seg1→imported thunk | 18 | GETCHARACTERPLACEMENT | `()` | values | — |  | 0 |
| `0x268` | seg1→imported thunk | 2 | GETFONTLANGUAGEINFO | `(word)` | values | — |  | 0 |
| `0x269` | seg1→imported thunk | 8 | BUILDINVERSETABLEDIB | `()` | values | — |  | 0 |
| `0x26a` | seg1→imported thunk | 12 | ICMCREATETRANSFORM | `()` | values | — |  | 0 |
| `0x26b` | seg1→imported thunk | 16 | ICMDELETETRANSFORM | `()` | values | — |  | 0 |
| `0x26c` | seg1→imported thunk | 16 | ICMTRANSLATERGB | `()` | values | — |  | 0 |
| `0x26d` | seg1→imported thunk | 32 | ICMTRANSLATERGBS | `()` | values | — |  | 0 |
| `0x26e` | seg1→imported thunk | 16 | ICMCHECKCOLORSINGAMUT | `()` | values | — |  | 0 |
| `0x2080` | seg1→imported thunk | 0 | (internal) |  | ? | — |  | 0 |
| `0x044` | seg1→imported thunk | 2 | DELETEDC | `(word)` | values | ✅ | ok 172 | 16: CARDFILE CHARMAP CLOCK DDEML MPLAYER NOTEPAD PBRUSH PROGMAN SOL SOUNDREC SYSEDIT TERMINAL WINFILE WINMINE WRITE PACKAGER* |
| `0x045` | seg1→imported thunk | 2 | DELETEOBJECT | `(word)` | values | ✅ | ok 796 | 16: CALC CARDFILE CHARMAP CLOCK DDEML MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN SOL SOUNDREC TERMINAL WINFILE WINMINE WRITE |
| `0x02d` | seg1→imported thunk | 4 | SELECTOBJECT | `(word word)` | values | ✅ | ok 3327 | 15: CALC CARDFILE CHARMAP CLOCK DDEML MPLAYER PACKAGER PBRUSH PROGMAN SOL SOUNDREC TERMINAL WINFILE WINMINE WRITE |
| `0x050` | seg1→imported thunk | 4 | GETDEVICECAPS | `(word s_word)` | values | ✅ | ok 250 | 15: CALC CARDFILE CHARMAP CLOCK NOTEPAD PACKAGER PBRUSH PROGMAN SOL SOUNDREC SYSEDIT TERMINAL WINFILE WINMINE WRITE |
| `0x057` | seg1→imported thunk | 2 | GETSTOCKOBJECT | `(word)` | values | ✅ | ok 1584 | 15: CALC CARDFILE CHARMAP CLOCK MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN SOL SOUNDREC TERMINAL WINFILE WINMINE WRITE |
| `0x05b` | seg1→imported thunk | 8 | GETTEXTEXTENT | `(word ptr s_word)` | pointer | ✅ | ok 958 | 15: CALC CARDFILE CHARMAP CLOCK MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN SOL SOUNDREC SYSEDIT TERMINAL WINFILE WRITE |
| `0x034` | seg1→imported thunk | 2 | CREATECOMPATIBLEDC | `(word)` | values | ✅ | ok 170 | 14: CARDFILE CHARMAP CLOCK DDEML MPLAYER PBRUSH PROGMAN SOL SOUNDREC TERMINAL WINFILE WINMINE WRITE PACKAGER* |
| `0x042` | seg1→imported thunk | 4 | CREATESOLIDBRUSH | `(long)` | values | ✅ | ok 663 | 14: CALC CARDFILE CHARMAP CLOCK MPLAYER PACKAGER PBRUSH PROGMAN SOL SOUNDREC TERMINAL WINFILE WINMINE WRITE |
| `0x009` | seg1→imported thunk | 6 | SETTEXTCOLOR | `(word long)` | values | ✅ | ok 1152 | 13: CALC CARDFILE CHARMAP CLOCK MPLAYER PACKAGER PBRUSH PROGMAN SOL SOUNDREC TERMINAL WINFILE WRITE |
| `0x021` | seg1→imported thunk | 12 | TEXTOUT | `(word s_word s_word str word)` | pointer | ✅ | ok 722 | 12: CALC CARDFILE CHARMAP MPLAYER PACKAGER PBRUSH PROGMAN SOL SYSEDIT TERMINAL WINFILE WRITE |
| `0x022` | seg1→imported thunk | 20 | BITBLT | `(word s_word s_word s_word s_word word s_word s_word long)` | values | ✅ | ok 265 | 12: CARDFILE CHARMAP CLOCK DDEML MPLAYER PBRUSH PROGMAN SOL SOUNDREC WINFILE WINMINE WRITE |
| `0x001` | seg1→imported thunk | 6 | SETBKCOLOR | `(word long)` | values | ✅ | ok 652 | 11: CARDFILE CHARMAP CLOCK MPLAYER PACKAGER PBRUSH PROGMAN SOUNDREC TERMINAL WINFILE WRITE |
| `0x002` | seg1→imported thunk | 4 | SETBKMODE | `(word word)` | values | ✅ | ok 50 | 11: CARDFILE CHARMAP CLOCK MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN SOUNDREC WINFILE WRITE |
| `0x01d` | seg1→imported thunk | 14 | PATBLT | `(word s_word s_word s_word s_word long)` | values | ✅ | ok 765 | 10: CHARMAP CLOCK MPLAYER PACKAGER PBRUSH PROGMAN SOL SOUNDREC WINFILE WRITE |
| `0x052` | seg1→imported thunk | 8 | GETOBJECT | `(word s_word ptr)` | pointer | ✅ | ok 27 | 10: CARDFILE DDEML MPLAYER PBRUSH SOL SOUNDREC TERMINAL WINFILE WRITE PACKAGER* |
| `0x05d` | seg1→imported thunk | 6 | GETTEXTMETRICS | `(word ptr)` | pointer | ✅ | ok 55 | 10: CALC CARDFILE CHARMAP NOTEPAD PBRUSH PROGMAN SOL TERMINAL WINFILE WRITE |
| `0x033` | seg1→imported thunk | 6 | CREATECOMPATIBLEBITMAP | `(word word word)` | values | ✅ | ok 66 | 9: CHARMAP DDEML MPLAYER PBRUSH SOL SOUNDREC TERMINAL WRITE WINFILE* |
| `0x039` | seg1→imported thunk | 4 | CREATEFONTINDIRECT | `(ptr)` | pointer | ✅ | ok 75 | 9: CHARMAP CLOCK MPLAYER PACKAGER PBRUSH PROGMAN TERMINAL WINFILE WRITE |
| `0x15f` | seg1→imported thunk | 22 | EXTTEXTOUT | `(word s_word s_word word ptr str word ptr)` | pointer | ✅ | ok 3525 | 9: CARDFILE CHARMAP CLOCK MPLAYER PACKAGER PROGMAN SOUNDREC TERMINAL WINFILE |
| `0x013` | seg1→imported thunk | 6 | LINETO | `(word s_word s_word)` | values | ✅ | ok 767 | 8: CARDFILE CHARMAP CLOCK PBRUSH SOL TERMINAL WINMINE WRITE |
| `0x014` | seg1→imported thunk | 6 | MOVETO | `(word s_word s_word)` | values | ✅ | ok 723 | 8: CARDFILE CHARMAP CLOCK PBRUSH SOL TERMINAL WINMINE WRITE |
| `0x023` | seg1→imported thunk | 24 | STRETCHBLT | `(word s_word s_word s_word s_word word s_word s_word s_word s_word long)` | values | ✅ | ok 9 | 8: CARDFILE MPLAYER PBRUSH SOL SOUNDREC TERMINAL WRITE PACKAGER* |
| `0x01e` | seg1→imported thunk | 2 | SAVEDC | `(word)` | values | ✅ | ok 49 | 7: CARDFILE MPLAYER PBRUSH SOUNDREC WRITE PACKAGER* WINFILE* |
| `0x027` | seg1→imported thunk | 4 | RESTOREDC | `(word s_word)` | values | ✅ | ok 49 | 7: CARDFILE MPLAYER PBRUSH SOUNDREC WRITE PACKAGER* WINFILE* |
| `0x030` | seg1→imported thunk | 12 | CREATEBITMAP | `(word word word word ptr)` | pointer | ✅ | ok 28 | 7: CARDFILE MPLAYER PBRUSH SOUNDREC WRITE PACKAGER* WINFILE* |
| `0x03d` | seg1→imported thunk | 8 | CREATEPEN | `(s_word s_word long)` | values | ✅ | ok 403 | 7: CALC CHARMAP CLOCK PBRUSH TERMINAL WINMINE WRITE |
| `0x004` | seg1→imported thunk | 4 | SETROP2 | `(word word)` | values | ✅ | ok 34 | 6: CALC CARDFILE CLOCK PBRUSH SOL WINMINE |
| `0x00b` | seg1→imported thunk | 6 | SETWINDOWORG | `(word s_word s_word)` | values | ✅ | ok 54 | 6: CARDFILE PACKAGER PBRUSH SOUNDREC WINFILE* WRITE* |
| `0x016` | seg1→imported thunk | 10 | INTERSECTCLIPRECT | `(word s_word s_word s_word s_word)` | values | ✅ | ok 45 | 6: CARDFILE MPLAYER SOUNDREC WRITE PACKAGER* WINFILE* |
| `0x01b` | seg1→imported thunk | 10 | RECTANGLE | `(word s_word s_word s_word s_word)` | values | ✅ | ok 420 | 6: CALC CARDFILE CLOCK PBRUSH WRITE PACKAGER* |
| `0x026` | seg1→imported thunk | 14 | ESCAPE | `(word word word segptr ptr)` | pointer | ✅ |  | 6: CARDFILE NOTEPAD PBRUSH SYSEDIT TERMINAL WRITE |
| `0x035` | seg1→imported thunk | 16 | CREATEDC | `(str str str ptr)` | pointer | ✅ |  | 6: CARDFILE NOTEPAD PBRUSH SYSEDIT TERMINAL WRITE |
| `0x097` | seg1→imported thunk | 6 | COPYMETAFILE | `(word str)` | pointer | ✅ |  | 6: DDEML PACKAGER CARDFILE* PBRUSH* SOUNDREC* WRITE* |
| `0x00c` | seg1→imported thunk | 6 | SETWINDOWEXT | `(word s_word s_word)` | values | ✅ | ok 4 | 5: CARDFILE PACKAGER PBRUSH SOUNDREC WRITE |
| `0x04a` | seg1→imported thunk | 10 | GETBITMAPBITS | `(word long ptr)` | pointer | ✅ |  | 5: CARDFILE PBRUSH PACKAGER* SOUNDREC* WRITE* |
| `0x06a` | seg1→imported thunk | 10 | SETBITMAPBITS | `(word long ptr)` | pointer | ✅ |  | 5: PBRUSH CARDFILE* PACKAGER* SOUNDREC* WRITE* |
| `0x07d` | seg1→imported thunk | 4 | CREATEMETAFILE | `(str)` | pointer | ✅ |  | 5: PACKAGER PBRUSH SOUNDREC CARDFILE* WRITE* |
| `0x07e` | seg1→imported thunk | 2 | CLOSEMETAFILE | `(word)` | values | ✅ |  | 5: PACKAGER PBRUSH SOUNDREC CARDFILE* WRITE* |
| `0x07f` | seg1→imported thunk | 2 | DELETEMETAFILE | `(word)` | values | ✅ |  | 5: SOUNDREC CARDFILE* PACKAGER* PBRUSH* WRITE* |
| `0x09c` | seg1→imported thunk | 6 | CREATEDISCARDABLEBITMAP | `(word word word)` | values | ✅ | ok 10 | 5: CLOCK PBRUSH PROGMAN WRITE WINFILE* |
| `0x168` | seg1→imported thunk | 4 | CREATEPALETTE | `(ptr)` | pointer | ✅ |  | 5: DDEML PBRUSH CARDFILE* PACKAGER* WRITE* |
| `0x003` | seg1→imported thunk | 4 | SETMAPMODE | `(word word)` | values | ✅ | ok 4 | 4: PBRUSH WRITE CARDFILE* PACKAGER* |
| `0x007` | seg1→imported thunk | 4 | SETSTRETCHBLTMODE | `(word word)` | values | ✅ | ok 17 | 4: MPLAYER PBRUSH SOUNDREC WRITE |
| `0x00d` | seg1→imported thunk | 6 | SETVIEWPORTORG | `(word s_word s_word)` | values | ✅ | ok 4 | 4: PBRUSH WRITE CARDFILE* PACKAGER* |
| `0x00e` | seg1→imported thunk | 6 | SETVIEWPORTEXT | `(word s_word s_word)` | values | ✅ | ok 4 | 4: PBRUSH WRITE CARDFILE* PACKAGER* |
| `0x03c` | seg1→imported thunk | 2 | CREATEPATTERNBRUSH | `(word)` | values | ✅ | ok 13 | 4: MPLAYER PBRUSH SOUNDREC WINFILE* |
| `0x063` | seg1→imported thunk | 8 | LPTODP | `(word ptr s_word)` | pointer | ✅ | ok 2 | 4: PBRUSH WRITE CARDFILE* PACKAGER* |
| `0x068` | seg1→imported thunk | 6 | RECTVISIBLE | `(word ptr)` | pointer | ✅ | ok 50 | 4: MPLAYER PROGMAN SOUNDREC WINFILE* |
| `0x094` | seg1→imported thunk | 6 | SETBRUSHORG | `(word s_word s_word)` | values | ✅ | ok 87 | 4: MPLAYER PBRUSH SOL SOUNDREC |
| `0x09a` | seg1→imported thunk | 6 | GETNEARESTCOLOR | `(word long)` | values | ✅ | ok 18 | 4: CLOCK PBRUSH WRITE WINFILE* |
| `0x0a3` | seg1→imported thunk | 6 | SETBITMAPDIMENSION | `(word s_word s_word)` | values | ✅ |  | 4: PBRUSH WRITE CARDFILE* PACKAGER* |
| `0x15a` | seg1→imported thunk | 4 | SETTEXTALIGN | `(word word)` | values | ✅ | ok 162 | 4: CLOCK PACKAGER PBRUSH WINFILE* |
| `0x1b7` | seg1→imported thunk | 32 | STRETCHDIBITS | `()` | values | ✅ |  | 4: PBRUSH CARDFILE* PACKAGER* WRITE* |
| `0x1b9` | seg1→imported thunk | 18 | GETDIBITS | `(word word word word ptr ptr word)` | pointer | ✅ |  | 4: PBRUSH CARDFILE* PACKAGER* WRITE* |
| `0x01c` | seg1→imported thunk | 14 | ROUNDRECT | `(word s_word s_word s_word s_word s_word s_word)` | values | ✅ | ok 688 | 3: CALC PBRUSH TERMINAL |
| `0x02c` | seg1→imported thunk | 4 | SELECTCLIPRGN | `(word word)` | values | ✅ |  | 3: PBRUSH TERMINAL WRITE |
| `0x038` | seg1→imported thunk | 30 | CREATEFONT | `(s_word s_word s_word s_word s_word word word word word word word word word str)` | pointer | ✅ | ok 15 | 3: CHARMAP PACKAGER WINFILE |
| `0x096` | seg1→imported thunk | 2 | UNREALIZEOBJECT | `(word)` | values | ✅ | ok 97 | 3: PBRUSH SOL SOUNDREC |
| `0x099` | seg1→imported thunk | 16 | CREATEIC | `(str str str ptr)` | pointer | ✅ | ok 16 | 3: CARDFILE PBRUSH WRITE |
| `0x1ba` | seg1→imported thunk | 20 | CREATEDIBITMAP | `(word ptr long ptr ptr word)` | pointer | ✅ | ok 15 | 3: CHARMAP WINFILE WINMINE |
| `0x015` | seg1→imported thunk | 10 | EXCLUDECLIPRECT | `(word s_word s_word s_word s_word)` | values | ✅ | ok 1 | 2: PBRUSH WINFILE* |
| `0x018` | seg1→imported thunk | 10 | ELLIPSE | `(word s_word s_word s_word s_word)` | values | ✅ |  | 2: PBRUSH WRITE |
| `0x01f` | seg1→imported thunk | 10 | SETPIXEL | `(word s_word s_word long)` | values | ✅ | ok 510 | 2: SOL WINMINE |
| `0x040` | seg1→imported thunk | 8 | CREATERECTRGN | `(s_word s_word s_word s_word)` | values | ✅ |  | 2: PBRUSH PROGMAN |
| `0x041` | seg1→imported thunk | 4 | CREATERECTRGNINDIRECT | `(ptr)` | pointer | ✅ |  | 2: CARDFILE TERMINAL |
| `0x043` | seg1→imported thunk | 8 | DPTOLP | `(word ptr s_word)` | pointer | ✅ |  | 2: PBRUSH WRITE |
| `0x04f` | seg1→imported thunk | 2 | GETDCORG | `(word)` | values | ✅ | ok 8 | 2: PBRUSH SOL |
| `0x053` | seg1→imported thunk | 6 | GETPIXEL | `(word s_word s_word)` | values | ✅ | ok 512 | 2: PBRUSH SOL |
| `0x05c` | seg1→imported thunk | 8 | GETTEXTFACE | `(word s_word ptr)` | pointer | ✅ |  | 2: TERMINAL WRITE |
| `0x064` | seg1→imported thunk | 16 | LINEDDA | `(s_word s_word s_word s_word segptr long)` | callback | ✅ |  | 2: PBRUSH SOL |
| `0x067` | seg1→imported thunk | 6 | PTVISIBLE | `(word s_word s_word)` | values | ✅ | ok 14 | 2: PBRUSH SOL |
| `0x15e` | seg1→imported thunk | 10 | GETCHARWIDTH | `(word word word ptr)` | pointer | ✅ | ok 13 | 2: CHARMAP WRITE |
| `0x16b` | seg1→imported thunk | 10 | GETPALETTEENTRIES | `(word word word ptr)` | pointer | ✅ |  | 2: DDEML PBRUSH |
| `0x1b8` | seg1→imported thunk | 18 | SETDIBITS | `(word word word word ptr ptr word)` | pointer | ✅ |  | 2: PBRUSH WINFILE* |
| `0x006` | seg1→imported thunk | 4 | SETPOLYFILLMODE | `(word word)` | values | ✅ |  | 1: PBRUSH |
| `0x00a` | seg1→imported thunk | 6 | SETTEXTJUSTIFICATION | `(word s_word s_word)` | values | ✅ | ok 4 | 1: WRITE |
| `0x024` | seg1→imported thunk | 8 | POLYGON | `()` | values | ✅ |  | 1: PBRUSH |
| `0x025` | seg1→imported thunk | 8 | POLYLINE | `()` | values | ✅ |  | 1: PBRUSH |
| `0x02f` | seg1→imported thunk | 8 | COMBINERGN | `(word word word s_word)` | values | ✅ |  | 1: PROGMAN |
| `0x031` | seg1→imported thunk | 4 | CREATEBITMAPINDIRECT | `(ptr)` | pointer | ✅ |  | 1: WRITE |
| `0x03a` | seg1→imported thunk | 6 | CREATEHATCHBRUSH | `(word long)` | values | ✅ |  | 1: PBRUSH |
| `0x03f` | seg1→imported thunk | 8 | CREATEPOLYGONRGN | `(ptr word word)` | pointer | ✅ |  | 1: PBRUSH |
| `0x04b` | seg1→imported thunk | 2 | GETBKCOLOR | `(word)` | values | ✅ |  | 1: PBRUSH |
| `0x04d` | seg1→imported thunk | 6 | GETCLIPBOX | `(word ptr)` | pointer | ✅ |  | 1: TERMINAL |
| `0x055` | seg1→imported thunk | 2 | GETROP2 | `(word)` | values | ✅ |  | 1: PBRUSH |
| `0x095` | seg1→imported thunk | 2 | GETBRUSHORG | `(word)` | values | ✅ |  | 1: PBRUSH |
| `0x0ac` | seg1→imported thunk | 10 | SETRECTRGN | `(word s_word s_word s_word s_word)` | values | ✅ |  | 1: PROGMAN |
| `0x133` | seg1→imported thunk | 10 | GETCHARABCWIDTHS | `(word word word ptr)` | pointer | ✅ |  | 1: PBRUSH |
| `0x14a` | seg1→imported thunk | 14 | ENUMFONTFAMILIES | `(word str segptr long)` | callback | ✅ | ok 10 **STEPPED 3** | 1: CHARMAP |
| `0x16e` | seg1→imported thunk | 2 | UPDATECOLORS | `(word)` | values | ✅ |  | 1: PBRUSH |
| `0x172` | seg1→imported thunk | 6 | GETNEARESTPALETTEINDEX | `(word long)` | values | ✅ |  | 1: PBRUSH |
| `0x174` | seg1→imported thunk | 12 | EXTFLOODFILL | `(word s_word s_word long word)` | values | ✅ |  | 1: PBRUSH |
| `0x1bb` | seg1→imported thunk | 28 | SETDIBITSTODEVICE | `(word s_word s_word s_word s_word s_word s_word word word ptr ptr word)` | pointer | ✅ | ok 20 | 1: WINMINE |
| `0x04c` | seg1→imported thunk | 2 | GETBKMODE | `(word)` | values | ✅ |  | 0 |
| `0x051` | seg1→imported thunk | 2 | GETMAPMODE | `(word)` | values | ✅ |  | 0 |
| `0x054` | seg1→imported thunk | 2 | GETPOLYFILLMODE | `(word)` | values | ✅ |  | 0 |
| `0x058` | seg1→imported thunk | 2 | GETSTRETCHBLTMODE | `(word)` | values | ✅ |  | 0 |
| `0x05a` | seg1→imported thunk | 2 | GETTEXTCOLOR | `(word)` | values | ✅ |  | 0 |

## KEYBOARD — 11 ids, 5 handled, 1 gaps

Unhandled first, then most-used.

| id | table | args | name | Wine signature | kind | handled | rig | shelf |
|---|---|---:|---|---|---|---|---|---|
| `0x084` | seg1→imported thunk | 0 | GETKBCODEPAGE | `()` | values | — |  | 1: DDEML |
| `0x004` | seg1→imported thunk | 14 | TOASCII | `(word word ptr ptr word)` | pointer | — |  | 0 |
| `0x080` | seg1→imported thunk | 2 | OEMKEYSCAN | `(word)` | values | — |  | 0 |
| `0x082` | seg1→imported thunk | 2 | GETKEYBOARDTYPE | `(word)` | values | — |  | 0 |
| `0x086` | seg1→imported thunk | 10 | ANSITOOEMBUFF | `(ptr ptr word)` | pointer | — |  | 0 |
| `0x087` | seg1→imported thunk | 10 | OEMTOANSIBUFF | `(ptr ptr word)` | pointer | — |  | 0 |
| `0x006` | seg1→imported thunk | 8 | OEMTOANSI | `(str ptr)` | pointer | ✅ | ok 1018 | 9: CARDFILE NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOUNDREC WINFILE WRITE |
| `0x005` | seg1→imported thunk | 8 | ANSITOOEM | `(str ptr)` | pointer | ✅ | ok 1647 | 8: CARDFILE NOTEPAD PACKAGER PBRUSH PROGMAN TERMINAL WINFILE WRITE |
| `0x081` | seg1→imported thunk | 2 | VKKEYSCAN | `(word)` | values | ✅ | ok 6 | 2: CHARMAP WRITE |
| `0x083` | seg1→imported thunk | 4 | MAPVIRTUALKEY | `(word word)` | values | ✅ | ok 2 | 2: CHARMAP PROGMAN |
| `0x085` | seg1→imported thunk | 10 | GETKEYNAMETEXT | `(long ptr word)` | pointer | ✅ |  | 1: PROGMAN |

## SHELL — 34 ids, 16 handled, 2 gaps

Unhandled first, then most-used.

| id | table | args | name | Wine signature | kind | handled | rig | shelf |
|---|---|---:|---|---|---|---|---|---|
| `0x026` | seg1→imported thunk | 0 | FINDENVIRONMENTSTRING | `(ptr)` | pointer | — |  | 1: WINFILE |
| `0x027` | seg1→imported thunk | 0 | INTERNALEXTRACTICON | `(word ptr s_word word)` | pointer | — |  | 1: PROGMAN |
| `0x00c` | seg1→imported thunk | 2 | (internal) |  | ? | — |  | 0 |
| `0x020` | seg1→imported thunk | 0 | WCI |  | ? | — |  | 0 |
| `0x021` | seg1→imported thunk | 0 | ABOUTDLGPROC | `(word word word long)` | callback | — |  | 0 |
| `0x028` | seg1→imported thunk | 16 | EXTRACTICONEX | `(str word ptr ptr word)` | pointer | — |  | 0 |
| `0x029` | seg1→imported thunk | 0 | HERETHARBETYGARS |  | ? | — |  | 0 |
| `0x02a` | seg1→imported thunk | 0 | FINDEXEDLGPROC | `(long word word word long word)` | callback | — |  | 0 |
| `0x02c` | seg1→imported thunk | 0 | SHELLHOOKPROC | `(word word long)` | callback | — |  | 0 |
| `0x02d` | seg1→imported thunk | 10 | RESTARTDIALOG | `()` | values | — |  | 0 |
| `0x02e` | seg1→imported thunk | 12 | PICKICONDLG |  | ? | — |  | 0 |
| `0x02f` | seg1→imported thunk | 2 | DRIVETYPE | `(long)` | values | — |  | 0 |
| `0x030` | seg1→imported thunk | 8 | SH16TO32DRIVEIOCTL |  | ? | — |  | 0 |
| `0x031` | seg1→imported thunk | 14 | SH16TO32INT2526 |  | ? | — |  | 0 |
| `0x032` | seg1→imported thunk | 16 | SHGETFILEINFO |  | ? | — |  | 0 |
| `0x033` | seg1→imported thunk | 8 | SHFORMATDRIVE |  | ? | — |  | 0 |
| `0x034` | seg1→imported thunk | 14 | SHCHECKDRIVE | `()` | values | — |  | 0 |
| `0x035` | seg1→imported thunk | 10 | _RUNDLLCHECKDRIVE |  | ? | — |  | 0 |
| `0x016` | seg1→imported thunk | 12 | SHELLABOUT | `(word ptr ptr word)` | pointer | ✅ |  | 15: CALC CARDFILE CLOCK MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN RECORDER SOL SOUNDREC TERMINAL WINFILE WINMINE WRITE |
| `0x009` | seg1→imported thunk | 4 | DRAGACCEPTFILES | `(word word)` | values | ✅ | ok 34 | 8: CARDFILE MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN SOUNDREC WRITE |
| `0x00b` | seg1→imported thunk | 10 | DRAGQUERYFILE | `(word s_word ptr s_word)` | pointer | ✅ |  | 8: CARDFILE MPLAYER NOTEPAD PACKAGER PBRUSH PROGMAN SOUNDREC WRITE |
| `0x006` | seg1→imported thunk | 16 | REGQUERYVALUE | `(long str ptr ptr)` | pointer | ✅ | ok 25 | 7: CARDFILE MPLAYER PACKAGER PBRUSH SOUNDREC WINFILE WRITE |
| `0x001` | seg1→imported thunk | 12 | REGOPENKEY | `(long str ptr)` | pointer | ✅ |  | 6: CARDFILE PACKAGER WINFILE WRITE PBRUSH* SOUNDREC* |
| `0x003` | seg1→imported thunk | 4 | REGCLOSEKEY | `(long)` | values | ✅ |  | 6: CARDFILE PACKAGER PBRUSH WINFILE WRITE SOUNDREC* |
| `0x005` | seg1→imported thunk | 20 | REGSETVALUE | `(long str long str long)` | pointer | ✅ | ok 8 | 5: MPLAYER PACKAGER PBRUSH SOUNDREC WINFILE |
| `0x007` | seg1→imported thunk | 16 | REGENUMKEY | `(long long ptr long)` | callback | ✅ |  | 4: CARDFILE PACKAGER WINFILE WRITE |
| `0x014` | seg1→imported thunk | 20 | SHELLEXECUTE | `(word str str str str s_word)` | pointer | ✅ |  | 3: PACKAGER PROGMAN WINFILE |
| `0x022` | seg1→imported thunk | 8 | EXTRACTICON | `(word str s_word)` | pointer | ✅ |  | 2: PACKAGER PROGMAN |
| `0x024` | seg1→imported thunk | 10 | EXTRACTASSOCIATEDICON | `(word ptr ptr)` | pointer | ✅ |  | 2: PACKAGER PROGMAN |
| `0x002` | seg1→imported thunk | 12 | REGCREATEKEY | `(long str ptr)` | pointer | ✅ |  | 1: PBRUSH |
| `0x004` | seg1→imported thunk | 8 | REGDELETEKEY | `(long str)` | pointer | ✅ |  | 1: WINFILE |
| `0x015` | seg1→imported thunk | 12 | FINDEXECUTABLE | `(str str ptr)` | pointer | ✅ |  | 1: PROGMAN |
| `0x025` | seg1→imported thunk | 6 | DOENVIRONMENTSUBST | `(ptr word)` | pointer | ✅ |  | 1: PROGMAN |
| `0x02b` | seg1→imported thunk | 0 | REGISTERSHELLHOOK | `(word word)` | callback | ✅ |  | 1: PROGMAN |

## COMMDLG — 8 ids, 7 handled, 1 gaps

Unhandled first, then most-used.

| id | table | args | name | Wine signature | kind | handled | rig | shelf |
|---|---|---:|---|---|---|---|---|---|
| `0x014` | seg1→imported thunk | 4 | PRINTDLG | `(ptr)` | callback | — | **STEPPED 7** | 5: CARDFILE NOTEPAD PBRUSH TERMINAL WRITE |
| `0x001` | seg1→imported thunk | 4 | GETOPENFILENAME | `(segptr)` | callback | ✅ |  | 10: CARDFILE MPLAYER NOTEPAD PACKAGER PBRUSH RECORDER SOUNDREC TERMINAL WINFILE WRITE |
| `0x002` | seg1→imported thunk | 4 | GETSAVEFILENAME | `(segptr)` | callback | ✅ |  | 8: CARDFILE NOTEPAD PACKAGER PBRUSH RECORDER SOUNDREC TERMINAL WRITE |
| `0x00f` | seg1→imported thunk | 4 | CHOOSEFONT | `(ptr)` | callback | ✅ | ok 1 | 4: CLOCK PBRUSH WINFILE WRITE |
| `0x00b` | seg1→imported thunk | 4 | FINDTEXT | `(segptr)` | callback | ✅ | **STEPPED 2** | 2: CARDFILE NOTEPAD |
| `0x005` | seg1→imported thunk | 4 | CHOOSECOLOR | `(ptr)` | callback | ✅ |  | 0 |
| `0x00c` | seg1→imported thunk | 4 | REPLACETEXT | `(segptr)` | callback | ✅ |  | 0 |
| `0x01a` | seg1→imported thunk | 0 | (WOWCDLG_EXTENDEDERROR) |  | ? | ✅ |  | 0 |

## SOUND — 17 ids, 0 handled, 6 gaps

Unhandled first, then most-used.

| id | table | args | name | Wine signature | kind | handled | rig | shelf |
|---|---|---:|---|---|---|---|---|---|
| `0x001` | seg1→imported thunk | 0 | OPENSOUND | `()` | values | — |  | 1: WINMINE |
| `0x002` | seg1→imported thunk | 0 | CLOSESOUND | `()` | values | — |  | 1: WINMINE |
| `0x004` | seg1→imported thunk | 8 | SETVOICENOTE | `(word word word word)` | values | — |  | 1: WINMINE |
| `0x005` | seg1→imported thunk | 10 | SETVOICEACCENT | `(word word word word word)` | values | — |  | 1: WINMINE |
| `0x009` | seg1→imported thunk | 0 | STARTSOUND | `()` | values | — |  | 1: WINMINE |
| `0x00a` | seg1→imported thunk | 0 | STOPSOUND | `()` | values | — |  | 1: WINMINE |
| `0x003` | seg1→imported thunk | 4 | SETVOICEQUEUESIZE | `(word word)` | values | — |  | 0 |
| `0x006` | seg1→imported thunk | 6 | SETVOICEENVELOPE | `(word word word)` | values | — |  | 0 |
| `0x007` | seg1→imported thunk | 4 | SETSOUNDNOISE | `(word word)` | values | — |  | 0 |
| `0x008` | seg1→imported thunk | 8 | SETVOICESOUND | `(word long word)` | values | — |  | 0 |
| `0x00b` | seg1→imported thunk | 2 | WAITSOUNDSTATE | `(word)` | values | — |  | 0 |
| `0x00c` | seg1→imported thunk | 0 | SYNCALLVOICES | `()` | values | — |  | 0 |
| `0x00d` | seg1→imported thunk | 2 | COUNTVOICENOTES | `(word)` | values | — |  | 0 |
| `0x00e` | seg1→imported thunk | 0 | GETTHRESHOLDEVENT | `()` | values | — |  | 0 |
| `0x00f` | seg1→imported thunk | 0 | GETTHRESHOLDSTATUS | `()` | values | — |  | 0 |
| `0x010` | seg1→imported thunk | 4 | SETVOICETHRESHOLD | `(word word)` | values | — |  | 0 |
| `0x011` | seg1→imported thunk | 0 | DOBEEP | `()` | values | — |  | 0 |

## SYSTEM — 1 ids, 0 handled, 0 gaps

Unhandled first, then most-used.

| id | table | args | name | Wine signature | kind | handled | rig | shelf |
|---|---|---:|---|---|---|---|---|---|
| `0x00f` | seg1→imported thunk | 0 | GETSYSTEMMSECCOUNT | `()` | values | — |  | 0 |

## TOOLHELP — 3 ids, 0 handled, 0 gaps

Unhandled first, then most-used.

| id | table | args | name | Wine signature | kind | handled | rig | shelf |
|---|---|---:|---|---|---|---|---|---|
| `0x000` | seg1→imported thunk | 0 | (internal) |  | ? | — |  | 0 |
| `0x001` | seg1→imported thunk | 4 | CLASSFIRST |  | ? | — |  | 0 |
| `0x002` | seg1→imported thunk | 4 | CLASSNEXT |  | ? | — |  | 0 |

## WINNLS — 8 ids, 0 handled, 0 gaps

Unhandled first, then most-used.

| id | table | args | name | Wine signature | kind | handled | rig | shelf |
|---|---|---:|---|---|---|---|---|---|
| `0x006` | seg1→imported thunk | 6 | SENDIMEMESSAGE |  | ? | — |  | 0 |
| `0x007` | seg1→imported thunk | 6 | SENDIMEMESSAGEEX |  | ? | — |  | 0 |
| `0x00f` | seg1→imported thunk | 2 | WINNLSGETIMEHOTKEY |  | ? | — |  | 0 |
| `0x010` | seg1→imported thunk | 4 | WINNLSENABLEIME |  | ? | — |  | 0 |
| `0x012` | seg1→imported thunk | 2 | WINNLSGETENABLESTATUS |  | ? | — |  | 0 |
| `0x016` | seg1→imported thunk | 4 | IMPQUERYIME |  | ? | — |  | 0 |
| `0x017` | seg1→imported thunk | 6 | IMPGETIME |  | ? | — |  | 0 |
| `0x018` | seg1→imported thunk | 6 | IMPSETIME |  | ? | — |  | 0 |

## MMSYSTEM — 166 ids, 0 handled, 31 gaps

Unhandled first, then most-used.

| id | table | args | name | Wine signature | kind | handled | rig | shelf |
|---|---|---:|---|---|---|---|---|---|
| `0x191` | exports |  | WAVEOUTGETNUMDEVS | `()` | values | — |  | 2: MPLAYER SOUNDREC |
| `0x1f5` | exports |  | WAVEINGETNUMDEVS | `()` | values | — |  | 2: MPLAYER SOUNDREC |
| `0x0c9` | exports |  | MIDIOUTGETNUMDEVS | `()` | values | — |  | 1: MPLAYER |
| `0x12d` | exports |  | MIDIINGETNUMDEVS | `()` | values | — |  | 1: MPLAYER |
| `0x15e` | exports |  | AUXGETNUMDEVS | `()` | values | — |  | 1: MPLAYER |
| `0x194` | exports |  | WAVEOUTOPEN | `(ptr word ptr long long long)` | pointer | — |  | 1: SOUNDREC |
| `0x195` | exports |  | WAVEOUTCLOSE | `(word)` | values | — |  | 1: SOUNDREC |
| `0x196` | exports |  | WAVEOUTPREPAREHEADER | `(word segptr word)` | pointer | — |  | 1: SOUNDREC |
| `0x197` | exports |  | WAVEOUTUNPREPAREHEADER | `(word segptr word)` | pointer | — |  | 1: SOUNDREC |
| `0x198` | exports |  | WAVEOUTWRITE | `(word segptr word)` | pointer | — |  | 1: SOUNDREC |
| `0x19b` | exports |  | WAVEOUTRESET | `(word)` | values | — |  | 1: SOUNDREC |
| `0x19c` | exports |  | WAVEOUTGETPOSITION | `(word ptr word)` | pointer | — |  | 1: SOUNDREC |
| `0x1f8` | exports |  | WAVEINOPEN | `(ptr word ptr long long long)` | pointer | — |  | 1: SOUNDREC |
| `0x1f9` | exports |  | WAVEINCLOSE | `(word)` | values | — |  | 1: SOUNDREC |
| `0x1fa` | exports |  | WAVEINPREPAREHEADER | `(word segptr word)` | pointer | — |  | 1: SOUNDREC |
| `0x1fb` | exports |  | WAVEINUNPREPAREHEADER | `(word segptr word)` | pointer | — |  | 1: SOUNDREC |
| `0x1fc` | exports |  | WAVEINADDBUFFER | `(word segptr word)` | pointer | — |  | 1: SOUNDREC |
| `0x1fd` | exports |  | WAVEINSTART | `(word)` | values | — |  | 1: SOUNDREC |
| `0x1ff` | exports |  | WAVEINRESET | `(word)` | values | — |  | 1: SOUNDREC |
| `0x200` | exports |  | WAVEINGETPOSITION | `(word ptr word)` | pointer | — |  | 1: SOUNDREC |
| `0x2bd` | exports |  | MCISENDCOMMAND | `(word word long long)` | values | — |  | 1: MPLAYER |
| `0x2be` | exports |  | MCISENDSTRING | `(str ptr word word)` | pointer | — |  | 1: MPLAYER |
| `0x2c2` | exports |  | MCIGETERRORSTRING | `(long ptr word)` | pointer | — |  | 1: MPLAYER |
| `0x4ba` | exports |  | MMIOOPEN | `(str ptr long)` | pointer | — |  | 1: SOUNDREC |
| `0x4bb` | exports |  | MMIOCLOSE | `(word word)` | values | — |  | 1: SOUNDREC |
| `0x4bc` | exports |  | MMIOREAD | `(word ptr long)` | pointer | — |  | 1: SOUNDREC |
| `0x4bd` | exports |  | MMIOWRITE | `(word ptr long)` | pointer | — |  | 1: SOUNDREC |
| `0x4bf` | exports |  | MMIOGETINFO | `(word ptr word)` | pointer | — |  | 1: SOUNDREC |
| `0x4c7` | exports |  | MMIODESCEND | `(word ptr ptr word)` | pointer | — |  | 1: SOUNDREC |
| `0x4c8` | exports |  | MMIOASCEND | `(word ptr word)` | pointer | — |  | 1: SOUNDREC |
| `0x4c9` | exports |  | MMIOCREATECHUNK | `(word ptr word)` | pointer | — |  | 1: SOUNDREC |
| `0x001` | exports |  | WEP | `(word word word ptr)` | pointer | — |  | 0 |
| `0x002` | exports |  | SNDPLAYSOUND | `(ptr word)` | pointer | — |  | 0 |
| `0x003` | exports |  | PlaySound | `(ptr word long)` | pointer | — |  | 0 |
| `0x004` | exports |  | DllEntryPoint | `(long word word word long word)` | values | — |  | 0 |
| `0x005` | exports |  | MMSYSTEMGETVERSION | `()` | values | — |  | 0 |
| `0x006` | exports |  | DRIVERPROC | `(long word word long long)` | callback | — |  | 0 |
| `0x008` | exports |  | WMMMidiRunOnce | `()` | values | — |  | 0 |
| `0x01e` | exports |  | OUTPUTDEBUGSTR | `(str)` | pointer | — |  | 0 |
| `0x01f` | exports |  | DRIVERCALLBACK | `(long word word word long long long)` | callback | — |  | 0 |
| `0x020` | exports |  | STACKENTER | `()` | values | — |  | 0 |
| `0x021` | exports |  | STACKLEAVE | `()` | values | — |  | 0 |
| `0x022` | exports |  | MMDRVINSTALL | `()` | values | — |  | 0 |
| `0x065` | exports |  | JOYGETNUMDEVS | `()` | values | — |  | 0 |
| `0x066` | exports |  | JOYGETDEVCAPS | `(word ptr word)` | pointer | — |  | 0 |
| `0x067` | exports |  | JOYGETPOS | `(word ptr)` | pointer | — |  | 0 |
| `0x068` | exports |  | JOYGETTHRESHOLD | `(word ptr)` | pointer | — |  | 0 |
| `0x069` | exports |  | JOYRELEASECAPTURE | `(word)` | values | — |  | 0 |
| `0x06a` | exports |  | JOYSETCAPTURE | `(word word word word)` | values | — |  | 0 |
| `0x06b` | exports |  | JOYSETTHRESHOLD | `(word word)` | values | — |  | 0 |
| `0x06d` | exports |  | JOYSETCALIBRATION | `(word)` | values | — |  | 0 |
| `0x06e` | exports |  | joyGetPosEx | `(word ptr)` | pointer | — |  | 0 |
| `0x06f` | exports |  | JOYCONFIGCHANGED | `()` | values | — |  | 0 |
| `0x0ca` | exports |  | MIDIOUTGETDEVCAPS | `(word ptr word)` | pointer | — |  | 0 |
| `0x0cb` | exports |  | MIDIOUTGETERRORTEXT | `(word ptr word)` | pointer | — |  | 0 |
| `0x0cc` | exports |  | MIDIOUTOPEN | `(ptr word long long long)` | pointer | — |  | 0 |
| `0x0cd` | exports |  | MIDIOUTCLOSE | `(word)` | values | — |  | 0 |
| `0x0ce` | exports |  | MIDIOUTPREPAREHEADER | `(word segptr word)` | pointer | — |  | 0 |
| `0x0cf` | exports |  | MIDIOUTUNPREPAREHEADER | `(word segptr word)` | pointer | — |  | 0 |
| `0x0d0` | exports |  | MIDIOUTSHORTMSG | `(word long)` | values | — |  | 0 |
| `0x0d1` | exports |  | MIDIOUTLONGMSG | `(word segptr word)` | pointer | — |  | 0 |
| `0x0d2` | exports |  | MIDIOUTRESET | `(word)` | values | — |  | 0 |
| `0x0d3` | exports |  | MIDIOUTGETVOLUME | `(word ptr)` | pointer | — |  | 0 |
| `0x0d4` | exports |  | MIDIOUTSETVOLUME | `(word long)` | values | — |  | 0 |
| `0x0d5` | exports |  | MIDIOUTCACHEPATCHES | `(word word ptr word)` | pointer | — |  | 0 |
| `0x0d6` | exports |  | MIDIOUTCACHEDRUMPATCHES | `(word word ptr word)` | pointer | — |  | 0 |
| `0x0d7` | exports |  | MIDIOUTGETID | `(word ptr)` | pointer | — |  | 0 |
| `0x0d8` | exports |  | MIDIOUTMESSAGE | `(word word long long)` | values | — |  | 0 |
| `0x0fa` | exports |  | midiStreamProperty | `(word ptr long)` | pointer | — |  | 0 |
| `0x0fb` | exports |  | midiStreamOpen | `(ptr ptr long long long long)` | pointer | — |  | 0 |
| `0x0fc` | exports |  | midiStreamClose | `(word)` | values | — |  | 0 |
| `0x0fd` | exports |  | midiStreamPosition | `(word ptr word)` | pointer | — |  | 0 |
| `0x0fe` | exports |  | midiStreamOut | `(word ptr word)` | pointer | — |  | 0 |
| `0x0ff` | exports |  | midiStreamPause | `(word)` | values | — |  | 0 |
| `0x100` | exports |  | midiStreamRestart | `(word)` | values | — |  | 0 |
| `0x101` | exports |  | midiStreamStop | `(word)` | values | — |  | 0 |
| `0x12e` | exports |  | MIDIINGETDEVCAPS | `(word ptr word)` | pointer | — |  | 0 |
| `0x12f` | exports |  | MIDIINGETERRORTEXT | `(word ptr word)` | pointer | — |  | 0 |
| `0x130` | exports |  | MIDIINOPEN | `(ptr word long long long)` | pointer | — |  | 0 |
| `0x131` | exports |  | MIDIINCLOSE | `(word)` | values | — |  | 0 |
| `0x132` | exports |  | MIDIINPREPAREHEADER | `(word segptr word)` | pointer | — |  | 0 |
| `0x133` | exports |  | MIDIINUNPREPAREHEADER | `(word segptr word)` | pointer | — |  | 0 |
| `0x134` | exports |  | MIDIINADDBUFFER | `(word segptr word)` | pointer | — |  | 0 |
| `0x135` | exports |  | MIDIINSTART | `(word)` | values | — |  | 0 |
| `0x136` | exports |  | MIDIINSTOP | `(word)` | values | — |  | 0 |
| `0x137` | exports |  | MIDIINRESET | `(word)` | values | — |  | 0 |
| `0x138` | exports |  | MIDIINGETID | `(word ptr)` | pointer | — |  | 0 |
| `0x139` | exports |  | MIDIINMESSAGE | `(word word long long)` | values | — |  | 0 |
| `0x15f` | exports |  | AUXGETDEVCAPS | `(word ptr word)` | pointer | — |  | 0 |
| `0x160` | exports |  | AUXGETVOLUME | `(word ptr)` | pointer | — |  | 0 |
| `0x161` | exports |  | AUXSETVOLUME | `(word long)` | values | — |  | 0 |
| `0x162` | exports |  | AUXOUTMESSAGE | `(word word long long)` | values | — |  | 0 |
| `0x192` | exports |  | WAVEOUTGETDEVCAPS | `(word ptr word)` | pointer | — |  | 0 |
| `0x193` | exports |  | WAVEOUTGETERRORTEXT | `(word ptr word)` | pointer | — |  | 0 |
| `0x199` | exports |  | WAVEOUTPAUSE | `(word)` | values | — |  | 0 |
| `0x19a` | exports |  | WAVEOUTRESTART | `(word)` | values | — |  | 0 |
| `0x19d` | exports |  | WAVEOUTGETPITCH | `(word ptr)` | pointer | — |  | 0 |
| `0x19e` | exports |  | WAVEOUTSETPITCH | `(word long)` | values | — |  | 0 |
| `0x19f` | exports |  | WAVEOUTGETVOLUME | `(word ptr)` | pointer | — |  | 0 |
| `0x1a0` | exports |  | WAVEOUTSETVOLUME | `(word long)` | values | — |  | 0 |
| `0x1a1` | exports |  | WAVEOUTGETPLAYBACKRATE | `(word ptr)` | pointer | — |  | 0 |
| `0x1a2` | exports |  | WAVEOUTSETPLAYBACKRATE | `(word long)` | values | — |  | 0 |
| `0x1a3` | exports |  | WAVEOUTBREAKLOOP | `(word)` | values | — |  | 0 |
| `0x1a4` | exports |  | WAVEOUTGETID | `(word ptr)` | pointer | — |  | 0 |
| `0x1a5` | exports |  | WAVEOUTMESSAGE | `(word word long long)` | values | — |  | 0 |
| `0x1f6` | exports |  | WAVEINGETDEVCAPS | `(word ptr word)` | pointer | — |  | 0 |
| `0x1f7` | exports |  | WAVEINGETERRORTEXT | `(word ptr word)` | pointer | — |  | 0 |
| `0x1fe` | exports |  | WAVEINSTOP | `(word)` | values | — |  | 0 |
| `0x201` | exports |  | WAVEINGETID | `(word ptr)` | pointer | — |  | 0 |
| `0x202` | exports |  | WAVEINMESSAGE | `(word word long long)` | values | — |  | 0 |
| `0x259` | exports |  | TIMEGETSYSTEMTIME | `(ptr word)` | pointer | — |  | 0 |
| `0x25a` | exports |  | TIMESETEVENT | `(word word segptr long word)` | pointer | — |  | 0 |
| `0x25b` | exports |  | TIMEKILLEVENT | `(word)` | values | — |  | 0 |
| `0x25c` | exports |  | TIMEGETDEVCAPS | `(ptr word)` | pointer | — |  | 0 |
| `0x25d` | exports |  | TIMEBEGINPERIOD | `(word)` | values | — |  | 0 |
| `0x25e` | exports |  | TIMEENDPERIOD | `(word)` | values | — |  | 0 |
| `0x25f` | exports |  | TIMEGETTIME | `()` | values | — |  | 0 |
| `0x2bf` | exports |  | MCIGETDEVICEID | `(ptr)` | pointer | — |  | 0 |
| `0x2c1` | exports |  | MCILOADCOMMANDRESOURCE | `(word str word)` | pointer | — |  | 0 |
| `0x2c3` | exports |  | MCISETDRIVERDATA | `(word long)` | values | — |  | 0 |
| `0x2c4` | exports |  | MCIGETDRIVERDATA | `(word)` | values | — |  | 0 |
| `0x2c6` | exports |  | MCIDRIVERYIELD | `(word)` | values | — |  | 0 |
| `0x2c7` | exports |  | MCIDRIVERNOTIFY | `(word word word)` | values | — |  | 0 |
| `0x2c8` | exports |  | MCIEXECUTE | `(ptr)` | pointer | — |  | 0 |
| `0x2c9` | exports |  | MCIFREECOMMANDRESOURCE | `(word)` | values | — |  | 0 |
| `0x2ca` | exports |  | MCISETYIELDPROC | `(word ptr long)` | callback | — |  | 0 |
| `0x2cb` | exports |  | MCIGETDEVICEIDFROMELEMENTID | `(long ptr)` | pointer | — |  | 0 |
| `0x2cc` | exports |  | MCIGETYIELDPROC | `(word ptr)` | callback | — |  | 0 |
| `0x2cd` | exports |  | MCIGETCREATORTASK | `(word)` | values | — |  | 0 |
| `0x320` | exports |  | MIXERGETNUMDEVS | `()` | values | — |  | 0 |
| `0x321` | exports |  | MIXERGETDEVCAPS | `(word ptr word)` | pointer | — |  | 0 |
| `0x322` | exports |  | MIXEROPEN | `(ptr word long long long)` | pointer | — |  | 0 |
| `0x323` | exports |  | MIXERCLOSE | `(word)` | values | — |  | 0 |
| `0x324` | exports |  | MIXERMESSAGE | `(word word long long)` | values | — |  | 0 |
| `0x325` | exports |  | MIXERGETLINEINFO | `(word ptr long)` | pointer | — |  | 0 |
| `0x326` | exports |  | MIXERGETID | `(word ptr long)` | pointer | — |  | 0 |
| `0x327` | exports |  | MIXERGETLINECONTROLS | `(word ptr long)` | pointer | — |  | 0 |
| `0x328` | exports |  | MIXERGETCONTROLDETAILS | `(word ptr long)` | pointer | — |  | 0 |
| `0x329` | exports |  | MIXERSETCONTROLDETAILS | `(word ptr long)` | pointer | — |  | 0 |
| `0x384` | exports |  | MMTASKCREATE | `(long ptr long)` | pointer | — |  | 0 |
| `0x386` | exports |  | MMTASKBLOCK | `(word)` | values | — |  | 0 |
| `0x387` | exports |  | MMTASKSIGNAL | `(word)` | values | — |  | 0 |
| `0x388` | exports |  | MMGETCURRENTTASK | `()` | values | — |  | 0 |
| `0x389` | exports |  | MMTASKYIELD | `()` | values | — |  | 0 |
| `0x44c` | exports |  | DRVOPEN | `(str str long)` | pointer | — |  | 0 |
| `0x44d` | exports |  | DRVCLOSE | `(word long long)` | values | — |  | 0 |
| `0x44e` | exports |  | DRVSENDMESSAGE | `(word word long long)` | callback | — |  | 0 |
| `0x44f` | exports |  | DRVGETMODULEHANDLE | `(word)` | values | — |  | 0 |
| `0x450` | exports |  | DRVDEFDRIVERPROC | `(long word word long long)` | callback | — |  | 0 |
| `0x460` | exports |  | mmThreadCreate | `(segptr ptr long long)` | pointer | — |  | 0 |
| `0x461` | exports |  | mmThreadSignal | `(word)` | values | — |  | 0 |
| `0x462` | exports |  | mmThreadBlock | `(word)` | values | — |  | 0 |
| `0x463` | exports |  | mmThreadIsCurrent | `(word)` | values | — |  | 0 |
| `0x464` | exports |  | mmThreadIsValid | `(word)` | values | — |  | 0 |
| `0x465` | exports |  | mmThreadGetTask | `(word)` | values | — |  | 0 |
| `0x47e` | exports |  | mmShowMMCPLPropertySheet | `(word str str str)` | pointer | — |  | 0 |
| `0x4be` | exports |  | MMIOSEEK | `(word long word)` | values | — |  | 0 |
| `0x4c0` | exports |  | MMIOSETINFO | `(word ptr word)` | pointer | — |  | 0 |
| `0x4c1` | exports |  | MMIOSETBUFFER | `(word segptr long word)` | pointer | — |  | 0 |
| `0x4c2` | exports |  | MMIOFLUSH | `(word word)` | values | — |  | 0 |
| `0x4c3` | exports |  | MMIOADVANCE | `(word ptr word)` | pointer | — |  | 0 |
| `0x4c4` | exports |  | MMIOSTRINGTOFOURCC | `(str word)` | pointer | — |  | 0 |
| `0x4c5` | exports |  | MMIOINSTALLIOPROC | `(long ptr long)` | callback | — |  | 0 |
| `0x4c6` | exports |  | MMIOSENDMESSAGE | `(word word long long)` | callback | — |  | 0 |
| `0x4ca` | exports |  | MMIORENAME | `(ptr ptr ptr long)` | pointer | — |  | 0 |
| `0x7ff` | exports |  | __wine_mmThreadEntryPoint | `(long)` | values | — |  | 0 |

