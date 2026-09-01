# The WOW32 call surface of USER.EXE — GH #128

> Regenerate with `tools/ne/wowmap.py guest/ne/user.exe --md`.

**This is a DIFFERENT ID SPACE from krnl386's.** Every id in
[`wow32-call-surface.md`](wow32-call-surface.md) was read out of krnl386's own stub table;
USER has its own, and the numbers collide. `0x39` is `GetProfileInt` in krnl386's table and
**`RegisterClass`** in USER's — and for one run this host serviced the second with the first,
which is what put this file here.

## How a module's table is told apart at run time

The BOP lives inside krnl386's common thunk (`seg1:0x2bf1`), so the executing `CS` at a BOP
is krnl386's code segment. A stub in that same segment is krnl386's; anything else belongs to
another table. The host logs `[krnl]` or `[OTHER TABLE ...] stub=0x....` on every call, and
services only the first.

## The tables, measured

| module | table | stubs |
|---|---|---|
| krnl386 | `seg1 -> own thunk 0x2bb6` | **82** — the surface `wow32.h` implements |
| krnl386 | `seg1 -> own thunk 0xaae8` | 6 |
| krnl386 | `seg2 -> imported thunk` | 121 |
| USER | `seg1 -> imported thunk` | **457** (441 distinct ids) |
| GDI | `seg1 -> imported thunk` | 367 (365 distinct ids) |

USER's stubs are the same three pushes as krnl386's but end in a real far call, because they
IMPORT the thunk rather than owning it. Its exports reach them by **tail-jump**, not by call:

```
1dbd  push bp / mov bp,sp
1dc0  push 0x1dc8 / pop dx      ; the return trampoline, in DX
1dc4  pop bp
1dc5  jmp 0x0c18                ; ★ the stub -- a JUMP
1dc8  retf 4
```

That is ordinal 57, `REGISTERCLASS`, and `seg1:0x0c18` is id `0x39` with 4 argument bytes.

## The surface

262 of 441 ids are named by USER's own export table (190 DIRECT, 72 WRAPPER). The rest are
reached only from internal code and need their call sites read, the same way krnl386's were.

| ID | args | stub | name | evidence |
|---|---|---|---|---|
| `0x00` | 0 | `seg1:0x4c3f` | — | internal only — read the call site |
| `0x01` | 12 | `seg1:0x0b62` | — | internal only — read the call site |
| `0x03` | 0 | `seg1:0x053e` | **ENABLEOEMLAYER** | DIRECT |
| `0x04` | 0 | `seg1:0x0488` | **DISABLEOEMLAYER** | DIRECT |
| `0x06` | 2 | `seg1:0x0bf1` | **POSTQUITMESSAGE** | DIRECT |
| `0x07` | 6 | `seg1:0x05e7` | **EXITWINDOWS** | DIRECT |
| `0x08` | 2 | `seg1:0x15f7` | **BEAR8** | DIRECT |
| `0x0a` | 10 | `seg1:0x0f39` | — | internal only — read the call site |
| `0x0b` | 10 | `seg1:0x0f2c` | **BEAR11** | DIRECT |
| `0x0c` | 4 | `seg1:0x0a84` | **KILLTIMER** | WRAPPER |
| `0x0d` | 0 | `seg1:0x507e` | **GETTICKCOUNT** | DIRECT |
| `0x0e` | 0 | `seg1:0x08c9` | **GETTIMERRESOLUTION** | DIRECT |
| `0x0f` | 0 | `seg1:0x508b` | **GETCURRENTTIME** | DIRECT |
| `0x10` | 4 | `seg1:0x02ca` | — | internal only — read the call site |
| `0x11` | 4 | `seg1:0x4f53` | — | internal only — read the call site |
| `0x12` | 2 | `seg1:0x0cf8` | **SETCAPTURE** | WRAPPER |
| `0x13` | 0 | `seg1:0x0c42` | **RELEASECAPTURE** | DIRECT |
| `0x14` | 2 | `seg1:0x0e69` | **SETDOUBLECLICKTIME** | DIRECT |
| `0x15` | 0 | `seg1:0x079e` | **GETDOUBLECLICKTIME** | DIRECT |
| `0x16` | 2 | `seg1:0x0e83` | **SETFOCUS** | WRAPPER |
| `0x17` | 0 | `seg1:0x07b8` | **GETFOCUS** | DIRECT |
| `0x18` | 6 | `seg1:0x0c69` | — | internal only — read the call site |
| `0x19` | 6 | `seg1:0x087b` | — | internal only — read the call site |
| `0x1a` | 8 | `seg1:0x0eeb` | — | internal only — read the call site |
| `0x1b` | 6 | `seg1:0x05a6` | — | internal only — read the call site |
| `0x1c` | 6 | `seg1:0x4f2c` | — | internal only — read the call site |
| `0x1d` | 6 | `seg1:0x5030` | — | internal only — read the call site |
| `0x1e` | 4 | `seg1:0x10ef` | **WINDOWFROMPOINT** | DIRECT |
| `0x1f` | 2 | `seg1:0x504a` | **ISICONIC** | DIRECT |
| `0x20` | 6 | `seg1:0x5016` | — | internal only — read the call site |
| `0x21` | 6 | `seg1:0x4f46` | — | internal only — read the call site |
| `0x22` | 4 | `seg1:0x054b` | **ENABLEWINDOW** | WRAPPER |
| `0x23` | 2 | `seg1:0x5057` | **ISWINDOWENABLED** | DIRECT |
| `0x24` | 8 | `seg1:0x0932` | — | internal only — read the call site |
| `0x25` | 6 | `seg1:0x0f87` | — | internal only — read the call site |
| `0x26` | 2 | `seg1:0x093f` | **GETWINDOWTEXTLENGTH** | WRAPPER |
| `0x27` | 6 | `seg1:0x0207` | — | internal only — read the call site |
| `0x28` | 6 | `seg1:0x057f` | — | internal only — read the call site |
| `0x29` | 30 | `seg1:0x038d` | — | internal only — read the call site |
| `0x2a` | 4 | `seg1:0x0fd5` | — | internal only — read the call site |
| `0x2b` | 2 | `seg1:0x02f1` | **CLOSEWINDOW** | WRAPPER |
| `0x2c` | 2 | `seg1:0x0ba3` | **OPENICON** | WRAPPER |
| `0x2d` | 2 | `seg1:0x0214` | **BRINGWINDOWTOTOP** | WRAPPER |
| `0x2e` | 2 | `seg1:0x4fc8` | **GETPARENT** | WRAPPER |
| `0x2f` | 2 | `seg1:0x5023` | **ISWINDOW** | DIRECT |
| `0x30` | 4 | `seg1:0x503d` | **ISCHILD** | DIRECT |
| `0x31` | 2 | `seg1:0x5064` | **ISWINDOWVISIBLE** | DIRECT |
| `0x32` | 8 | `seg1:0x0659` | — | internal only — read the call site |
| `0x33` | 2 | `seg1:0x0a5d` | **BEAR51** | DIRECT |
| `0x34` | 0 | `seg1:0x01d3` | **ANYPOPUP** | DIRECT |
| `0x35` | 2 | `seg1:0x046e` | **DESTROYWINDOW** | WRAPPER |
| `0x36` | 8 | `seg1:0x05c0` | — | internal only — read the call site |
| `0x37` | 10 | `seg1:0x058c` | — | internal only — read the call site |
| `0x38` | 12 | `seg1:0x0b7c` | **MOVEWINDOW** | WRAPPER |
| `0x39` | 4 | `seg1:0x0c18` | **REGISTERCLASS** | WRAPPER |
| `0x3a` | 8 | `seg1:0x4f39` | — | internal only — read the call site |
| `0x3b` | 2 | `seg1:0x0ceb` | **SETACTIVEWINDOW** | WRAPPER |
| `0x3c` | 0 | `seg1:0x068d` | **GETACTIVEWINDOW** | DIRECT |
| `0x3d` | 14 | `seg1:0x0caa` | — | internal only — read the call site |
| `0x3e` | 8 | `seg1:0x0ef8` | — | internal only — read the call site |
| `0x3f` | 4 | `seg1:0x0895` | — | internal only — read the call site |
| `0x40` | 10 | `seg1:0x0f05` | — | internal only — read the call site |
| `0x41` | 12 | `seg1:0x08a2` | — | internal only — read the call site |
| `0x42` | 2 | `seg1:0x075d` | **GETDC** | WRAPPER |
| `0x43` | 2 | `seg1:0x08fd` | **GETWINDOWDC** | WRAPPER |
| `0x44` | 4 | `seg1:0x0c4f` | **RELEASEDC** | WRAPPER |
| `0x45` | 2 | `seg1:0x0e1b` | — | internal only — read the call site |
| `0x46` | 4 | `seg1:0x0e28` | **SETCURSORPOS** | DIRECT |
| `0x47` | 2 | `seg1:0x0fae` | **SHOWCURSOR** | DIRECT |
| `0x51` | 8 | `seg1:0x0632` | — | internal only — read the call site |
| `0x52` | 6 | `seg1:0x09f5` | — | internal only — read the call site |
| `0x53` | 8 | `seg1:0x0680` | — | internal only — read the call site |
| `0x54` | 8 | `seg1:0x04fd` | — | internal only — read the call site |
| `0x55` | 14 | `seg1:0x0517` | — | internal only — read the call site |
| `0x56` | 0 | `seg1:0x09b4` | **BEAR86** | DIRECT |
| `0x58` | 4 | `seg1:0x0565` | **ENDDIALOG** | WRAPPER |
| `0x5a` | 6 | `seg1:0x0a43` | — | internal only — read the call site |
| `0x5b` | 4 | `seg1:0x4f7a` | **GETDLGITEM** | WRAPPER |
| `0x5c` | 8 | `seg1:0x0e5c` | — | internal only — read the call site |
| `0x5d` | 10 | `seg1:0x0791` | **GETDLGITEMTEXT** | WRAPPER |
| `0x5e` | 8 | `seg1:0x0e4f` | **SETDLGITEMINT** | WRAPPER |
| `0x5f` | 10 | `seg1:0x0784` | — | internal only — read the call site |
| `0x60` | 8 | `seg1:0x02a3` | **CHECKRADIOBUTTON** | WRAPPER |
| `0x61` | 6 | `seg1:0x0289` | **CHECKDLGBUTTON** | WRAPPER |
| `0x62` | 4 | `seg1:0x0a50` | **ISDLGBUTTONCHECKED** | WRAPPER |
| `0x63` | 8 | `seg1:0x04bc` | — | internal only — read the call site |
| `0x64` | 12 | `seg1:0x04a2` | — | internal only — read the call site |
| `0x65` | 12 | `seg1:0x0cc4` | **SENDDLGITEMMESSAGE** | WRAPPER |
| `0x66` | 10 | `seg1:0x016b` | — | internal only — read the call site |
| `0x67` | 6 | `seg1:0x0b2e` | — | internal only — read the call site |
| `0x68` | 2 | `seg1:0x0b55` | **MESSAGEBEEP** | DIRECT |
| `0x69` | 4 | `seg1:0x0666` | **FLASHWINDOW** | WRAPPER |
| `0x6a` | 2 | `seg1:0x4f12` | **GETKEYSTATE** | DIRECT |
| `0x6b` | 10 | `seg1:0x03eb` | — | internal only — read the call site |
| `0x6c` | 10 | `seg1:0x0813` | — | internal only — read the call site |
| `0x6d` | 12 | `seg1:0x0bbd` | — | internal only — read the call site |
| `0x6e` | 10 | `seg1:0x0bd7` | **POSTMESSAGE** | WRAPPER |
| `0x6f` | 10 | `seg1:0x0cd1` | **SENDMESSAGE** | WRAPPER |
| `0x70` | 0 | `seg1:0x10e2` | **WAITMESSAGE** | DIRECT |
| `0x71` | 4 | `seg1:0x106d` | — | internal only — read the call site |
| `0x72` | 4 | `seg1:0x0495` | — | internal only — read the call site |
| `0x73` | 4 | `seg1:0x0c83` | **REPLYMESSAGE** | DIRECT |
| `0x74` | 10 | `seg1:0x0bca` | — | internal only — read the call site |
| `0x75` | 2 | `seg1:0x173c` | **WINDOWFROMDC** | DIRECT |
| `0x76` | 4 | `seg1:0x0c35` | — | internal only — read the call site |
| `0x77` | 0 | `seg1:0x082d` | **GETMESSAGEPOS** | DIRECT |
| `0x78` | 0 | `seg1:0x083a` | **GETMESSAGETIME** | DIRECT |
| `0x79` | 8 | `seg1:0x0f7a` | — | internal only — read the call site |
| `0x7a` | 14 | `seg1:0x0255` | — | internal only — read the call site |
| `0x7b` | 6 | `seg1:0x0248` | — | internal only — read the call site |
| `0x7c` | 2 | `seg1:0x10ae` | **UPDATEWINDOW** | WRAPPER |
| `0x7d` | 8 | `seg1:0x09db` | — | internal only — read the call site |
| `0x7e` | 6 | `seg1:0x09e8` | **INVALIDATERGN** | WRAPPER |
| `0x7f` | 6 | `seg1:0x10c8` | — | internal only — read the call site |
| `0x80` | 4 | `seg1:0x10d5` | **VALIDATERGN** | WRAPPER |
| `0x81` | 4 | `seg1:0x06e8` | **GETCLASSWORD** | WRAPPER |
| `0x82` | 6 | `seg1:0x0d2c` | **SETCLASSWORD** | WRAPPER |
| `0x83` | 4 | `seg1:0x06db` | **GETCLASSLONG** | WRAPPER |
| `0x84` | 8 | `seg1:0x0d1f` | **SETCLASSLONG** | WRAPPER |
| `0x85` | 4 | `seg1:0x094c` | **GETWINDOWWORD** | DIRECT |
| `0x86` | 6 | `seg1:0x0f94` | **SETWINDOWWORD** | WRAPPER |
| `0x87` | 4 | `seg1:0x090a` | **GETWINDOWLONG** | DIRECT |
| `0x88` | 8 | `seg1:0x0f60` | — | internal only — read the call site |
| `0x89` | 2 | `seg1:0x0b89` | **OPENCLIPBOARD** | WRAPPER |
| `0x8a` | 0 | `seg1:0x02d7` | **CLOSECLIPBOARD** | DIRECT |
| `0x8b` | 0 | `seg1:0x0524` | **EMPTYCLIPBOARD** | DIRECT |
| `0x8c` | 0 | `seg1:0x070f` | **GETCLIPBOARDOWNER** | DIRECT |
| `0x8d` | 4 | `seg1:0x0d39` | — | internal only — read the call site |
| `0x8e` | 2 | `seg1:0x06f5` | **GETCLIPBOARDDATA** | DIRECT |
| `0x8f` | 0 | `seg1:0x0332` | **COUNTCLIPBOARDFORMATS** | DIRECT |
| `0x90` | 2 | `seg1:0x0599` | **ENUMCLIPBOARDFORMATS** | DIRECT |
| `0x91` | 4 | `seg1:0x0c25` | — | internal only — read the call site |
| `0x92` | 8 | `seg1:0x0702` | — | internal only — read the call site |
| `0x93` | 2 | `seg1:0x0d46` | **SETCLIPBOARDVIEWER** | WRAPPER |
| `0x94` | 0 | `seg1:0x071c` | **GETCLIPBOARDVIEWER** | DIRECT |
| `0x95` | 4 | `seg1:0x026f` | **CHANGECLIPBOARDCHAIN** | WRAPPER |
| `0x96` | 16 | `seg1:0x0ad2` | — | internal only — read the call site |
| `0x97` | 0 | `seg1:0x0373` | **CREATEMENU** | DIRECT |
| `0x98` | 2 | `seg1:0x0461` | **DESTROYMENU** | WRAPPER |
| `0x99` | 12 | `seg1:0x027c` | — | internal only — read the call site |
| `0x9a` | 6 | `seg1:0x0296` | — | internal only — read the call site |
| `0x9b` | 6 | `seg1:0x4f05` | — | internal only — read the call site |
| `0x9c` | 4 | `seg1:0x08af` | **GETSYSTEMMENU** | WRAPPER |
| `0x9d` | 2 | `seg1:0x4f87` | **GETMENU** | WRAPPER |
| `0x9e` | 4 | `seg1:0x0ec4` | **SETMENU** | WRAPPER |
| `0x9f` | 4 | `seg1:0x4fd5` | **GETSUBMENU** | WRAPPER |
| `0xa0` | 2 | `seg1:0x050a` | **DRAWMENUBAR** | WRAPPER |
| `0xa1` | 12 | `seg1:0x0806` | — | internal only — read the call site |
| `0xa2` | 8 | `seg1:0x09a7` | — | internal only — read the call site |
| `0xa3` | 8 | `seg1:0x033f` | **CREATECARET** | WRAPPER |
| `0xa4` | 0 | `seg1:0x043a` | **DESTROYCARET** | DIRECT |
| `0xa5` | 4 | `seg1:0x0d12` | **SETCARETPOS** | DIRECT |
| `0xa6` | 2 | `seg1:0x099a` | **HIDECARET** | WRAPPER |
| `0xa7` | 2 | `seg1:0x0fa1` | **SHOWCARET** | WRAPPER |
| `0xa8` | 2 | `seg1:0x0d05` | **SETCARETBLINKTIME** | DIRECT |
| `0xa9` | 0 | `seg1:0x06b4` | **GETCARETBLINKTIME** | DIRECT |
| `0xaa` | 2 | `seg1:0x01ed` | **ARRANGEICONICWINDOWS** | WRAPPER |
| `0xac` | 4 | `seg1:0x1009` | **SWITCHTOTHISWINDOW** | DIRECT |
| `0xad` | 20 | `seg1:0x0ab8` | — | internal only — read the call site |
| `0xaf` | 14 | `seg1:0x0aab` | — | internal only — read the call site |
| `0xb2` | 8 | `seg1:0x104d` | — | internal only — read the call site |
| `0xb3` | 2 | `seg1:0x4fef` | — | internal only — read the call site |
| `0xb4` | 2 | `seg1:0x4fe2` | — | internal only — read the call site |
| `0xb5` | 10 | `seg1:0x0f12` | — | internal only — read the call site |
| `0xb6` | 4 | `seg1:0x0a77` | **BEAR182** | DIRECT |
| `0xb7` | 4 | `seg1:0x06c1` | **GETCARETPOS** | DIRECT |
| `0xb8` | 10 | `seg1:0x1164` | **QUERYSENDMESSAGE** | DIRECT |
| `0xb9` | 22 | `seg1:0x098d` | — | internal only — read the call site |
| `0xba` | 2 | `seg1:0x0ffc` | **SWAPMOUSEBUTTON** | DIRECT |
| `0xbb` | 0 | `seg1:0x0572` | **ENDMENU** | DIRECT |
| `0xbe` | 8 | `seg1:0x08d6` | — | internal only — read the call site |
| `0xbf` | 6 | `seg1:0x02b0` | **CHILDWINDOWFROMPOINT** | WRAPPER |
| `0xc0` | 0 | `seg1:0x09c1` | **INSENDMESSAGE** | DIRECT |
| `0xc1` | 2 | `seg1:0x0a36` | **ISCLIPBOARDFORMATAVAILABLE** | DIRECT |
| `0xc2` | 8 | `seg1:0x04c9` | — | internal only — read the call site |
| `0xc3` | 12 | `seg1:0x04af` | — | internal only — read the call site |
| `0xc4` | 20 | `seg1:0x1023` | — | internal only — read the call site |
| `0xc5` | 14 | `seg1:0x08bc` | — | internal only — read the call site |
| `0xc8` | 12 | `seg1:0x0b96` | — | internal only — read the call site |
| `0xc9` | 4 | `seg1:0x0e0e` | — | internal only — read the call site |
| `0xca` | 6 | `seg1:0x0743` | — | internal only — read the call site |
| `0xcb` | 6 | `seg1:0x0729` | — | internal only — read the call site |
| `0xcc` | 8 | `seg1:0x0bfe` | — | internal only — read the call site |
| `0xcd` | 8 | `seg1:0x1116` | — | internal only — read the call site |
| `0xce` | 4 | `seg1:0x107a` | — | internal only — read the call site |
| `0xcf` | 6 | `seg1:0x02e4` | — | internal only — read the call site |
| `0xd0` | 4 | `seg1:0x0e01` | — | internal only — read the call site |
| `0xd1` | 4 | `seg1:0x0736` | — | internal only — read the call site |
| `0xd2` | 2 | `seg1:0x0df4` | — | internal only — read the call site |
| `0xd3` | 2 | `seg1:0x02bd` | — | internal only — read the call site |
| `0xd4` | 4 | `seg1:0x1087` | — | internal only — read the call site |
| `0xd5` | 8 | `seg1:0x022e` | — | internal only — read the call site |
| `0xd6` | 4 | `seg1:0x05cd` | — | internal only — read the call site |
| `0xd7` | 4 | `seg1:0x0673` | — | internal only — read the call site |
| `0xd8` | 8 | `seg1:0x1171` | **USERSEEUSERDO** | DIRECT |
| `0xd9` | 4 | `seg1:0x0af9` | **LOOKUPMENUHANDLE** | DIRECT |
| `0xdc` | 4 | `seg1:0x0adf` | — | internal only — read the call site |
| `0xdd` | 20 | `seg1:0x0c9d` | — | internal only — read the call site |
| `0xde` | 4 | `seg1:0x4f1f` | — | internal only — read the call site |
| `0xdf` | 4 | `seg1:0x0eb7` | — | internal only — read the call site |
| `0xe0` | 2 | `seg1:0x0917` | **GETWINDOWTASK** | WRAPPER |
| `0xe1` | 10 | `seg1:0x05b3` | — | internal only — read the call site |
| `0xe2` | 6 | `seg1:0x117e` | **LOCKINPUT** | DIRECT |
| `0xe3` | 6 | `seg1:0x0847` | **GETNEXTDLGGROUPITEM** | WRAPPER |
| `0xe4` | 6 | `seg1:0x0854` | **GETNEXTDLGTABITEM** | WRAPPER |
| `0xe5` | 2 | `seg1:0x4ffc` | **GETTOPWINDOW** | WRAPPER |
| `0xe6` | 4 | `seg1:0x4fbb` | **GETNEXTWINDOW** | DIRECT |
| `0xe8` | 14 | `seg1:0x0f6d` | — | internal only — read the call site |
| `0xe9` | 4 | `seg1:0x0ede` | **SETPARENT** | WRAPPER |
| `0xea` | 6 | `seg1:0x1094` | — | internal only — read the call site |
| `0xeb` | 12 | `seg1:0x4ef8` | — | internal only — read the call site |
| `0xec` | 0 | `seg1:0x06a7` | **GETCAPTURE** | DIRECT |
| `0xed` | 6 | `seg1:0x08e3` | **GETUPDATERGN** | WRAPPER |
| `0xee` | 4 | `seg1:0x05da` | **EXCLUDEUPDATERGN** | WRAPPER |
| `0xef` | 22 | `seg1:0x047b` | — | internal only — read the call site |
| `0xf3` | 0 | `seg1:0x076a` | **GETDIALOGBASEUNITS** | DIRECT |
| `0xf5` | 8 | `seg1:0x118b` | — | internal only — read the call site |
| `0xf6` | 8 | `seg1:0x1198` | — | internal only — read the call site |
| `0xf7` | 0 | `seg1:0x11a5` | **GETCURSOR** | DIRECT |
| `0xf8` | 0 | `seg1:0x11b2` | **GETOPENCLIPBOARDWINDOW** | DIRECT |
| `0xf9` | 2 | `seg1:0x069a` | **GETASYNCKEYSTATE** | DIRECT |
| `0xfa` | 6 | `seg1:0x4fae` | — | internal only — read the call site |
| `0x102` | 10 | `seg1:0x11bf` | — | internal only — read the call site |
| `0x103` | 2 | `seg1:0x01fa` | **BEGINDEFERWINDOWPOS** | DIRECT |
| `0x104` | 16 | `seg1:0x03c1` | — | internal only — read the call site |
| `0x105` | 2 | `seg1:0x0558` | **ENDDEFERWINDOWPOS** | WRAPPER |
| `0x106` | 4 | `seg1:0x5009` | — | internal only — read the call site |
| `0x107` | 2 | `seg1:0x4f94` | **GETMENUITEMCOUNT** | WRAPPER |
| `0x108` | 4 | `seg1:0x4fa1` | **GETMENUITEMID** | WRAPPER |
| `0x109` | 4 | `seg1:0x0fbb` | **SHOWOWNEDPOPUPS** | WRAPPER |
| `0x10b` | 6 | `seg1:0x0fc8` | — | internal only — read the call site |
| `0x10c` | 4 | `seg1:0x0959` | **GLOBALADDATOM** | DIRECT |
| `0x10d` | 2 | `seg1:0x0966` | **GLOBALDELETEATOM** | DIRECT |
| `0x10e` | 4 | `seg1:0x0973` | — | internal only — read the call site |
| `0x10f` | 8 | `seg1:0x0980` | **GLOBALGETATOMNAME** | DIRECT |
| `0x110` | 2 | `seg1:0x5071` | **ISZOOMED** | DIRECT |
| `0x111` | 8 | `seg1:0x0318` | **CONTROLPANELINFO** | DIRECT |
| `0x112` | 4 | `seg1:0x0861` | **GETNEXTQUEUEWINDOW** | WRAPPER |
| `0x113` | 0 | `seg1:0x0c76` | **REPAINTSCREEN** | DIRECT |
| `0x114` | 2 | `seg1:0x0aec` | **LOCKMYTASK** | DIRECT |
| `0x115` | 2 | `seg1:0x0777` | **GETDLGCTRLID** | WRAPPER |
| `0x116` | 0 | `seg1:0x4f60` | **GETDESKTOPHWND** | DIRECT |
| `0x117` | 4 | `seg1:0x0e35` | **OLDSETDESKPATTERN** | DIRECT |
| `0x118` | 4 | `seg1:0x0f1f` | **SETSYSTEMMENU** | WRAPPER |
| `0x119` | 2 | `seg1:0x0fef` | **GETSYSCOLORBRUSH** | DIRECT |
| `0x11a` | 6 | `seg1:0x0cb7` | **SELECTPALETTE** | WRAPPER |
| `0x11b` | 2 | `seg1:0x0c0b` | **REALIZEPALETTE** | WRAPPER |
| `0x11c` | 0 | `seg1:0x0e9d` | **GETFREESYSTEMRESOURCES** | DIRECT |
| `0x11d` | 4 | `seg1:0x0e42` | **BEAR285** | DIRECT |
| `0x11e` | 0 | `seg1:0x4f6d` | **GETDESKTOPWINDOW** | DIRECT |
| `0x11f` | 2 | `seg1:0x07ec` | **GETLASTACTIVEPOPUP** | WRAPPER |
| `0x120` | 0 | `seg1:0x11f3` | **GETMESSAGEEXTRAINFO** | DIRECT |
| `0x122` | 10 | `seg1:0x120d` | — | internal only — read the call site |
| `0x123` | 10 | `seg1:0x113d` | — | internal only — read the call site |
| `0x124` | 4 | `seg1:0x114a` | **UNHOOKWINDOWSHOOKEX** | WRAPPER |
| `0x125` | 12 | `seg1:0x1157` | — | internal only — read the call site |
| `0x126` | 2 | `seg1:0x121a` | **LOCKWINDOWUPDATE** | WRAPPER |
| `0x12c` | 2 | `seg1:0x1722` | **UNLOADINSTALLABLEDRIVERS** | DIRECT |
| `0x131` | 10 | `seg1:0x042d` | — | internal only — read the call site |
| `0x134` | 10 | `seg1:0x03b4` | **DEFDLGPROC** | WRAPPER |
| `0x135` | 4 | `seg1:0x1234` | — | internal only — read the call site |
| `0x136` | 10 | `seg1:0x0325` | — | internal only — read the call site |
| `0x137` | 10 | `seg1:0x0262` | — | internal only — read the call site |
| `0x138` | 10 | `seg1:0x0cde` | — | internal only — read the call site |
| `0x139` | 10 | `seg1:0x0be4` | — | internal only — read the call site |
| `0x13a` | 10 | `seg1:0x0fe2` | **SIGNALPROC** | DIRECT |
| `0x13b` | 0 | `seg1:0x1123` | — | internal only — read the call site |
| `0x13c` | 10 | `seg1:0x02fe` | — | internal only — read the call site |
| `0x13d` | 8 | `seg1:0x030b` | — | internal only — read the call site |
| `0x13e` | 4 | `seg1:0x08f0` | — | internal only — read the call site |
| `0x13f` | 0 | `seg1:0x0f53` | — | internal only — read the call site |
| `0x140` | 14 | `seg1:0x0de4` | — | internal only — read the call site |
| `0x141` | 4 | `seg1:0x0e76` | — | internal only — read the call site |
| `0x142` | 4 | `seg1:0x10fc` | **WINOLDAPPHACKOMATIC** | DIRECT |
| `0x143` | 14 | `seg1:0x0820` | **GETMESSAGE2** | DIRECT |
| `0x144` | 8 | `seg1:0x063f` | **FILLWINDOW** | DIRECT |
| `0x145` | 12 | `seg1:0x0bb0` | **PAINTRECT** | DIRECT |
| `0x146` | 6 | `seg1:0x0750` | **GETCONTROLBRUSH** | DIRECT |
| `0x147` | 4 | `seg1:0x0a91` | — | internal only — read the call site |
| `0x148` | 10 | `seg1:0x0f46` | — | internal only — read the call site |
| `0x149` | 6 | `seg1:0x0b48` | — | internal only — read the call site |
| `0x14a` | 4 | `seg1:0x0e90` | — | internal only — read the call site |
| `0x14b` | 2 | `seg1:0x0531` | **ENABLEHARDWAREINPUT** | DIRECT |
| `0x14c` | 0 | `seg1:0x10bb` | **USERYIELD** | DIRECT |
| `0x14d` | 0 | `seg1:0x0a6a` | **ISUSERIDLE** | DIRECT |
| `0x14e` | 2 | `seg1:0x0888` | — | internal only — read the call site |
| `0x14f` | 0 | `seg1:0x07d2` | **GETINPUTSTATE** | DIRECT |
| `0x155` | 0 | `seg1:0x1130` | — | internal only — read the call site |
| `0x157` | 4 | `seg1:0x07ab` | — | internal only — read the call site |
| `0x15a` | 10 | `seg1:0x0625` | — | internal only — read the call site |
| `0x15b` | 10 | `seg1:0x1016` | — | internal only — read the call site |
| `0x162` | 22 | `seg1:0x1030` | — | internal only — read the call site |
| `0x163` | 10 | `seg1:0x0221` | — | internal only — read the call site |
| `0x166` | 2 | `seg1:0x124e` | **ISMENU** | DIRECT |
| `0x167` | 8 | `seg1:0x125b` | — | internal only — read the call site |
| `0x16a` | 12 | `seg1:0x03a7` | **DCHOOK** | DIRECT |
| `0x16c` | 12 | `seg1:0x15a9` | **LOOKUPICONIDFROMDIRECTORYEX** | DIRECT |
| `0x172` | 6 | `seg1:0x1268` | — | internal only — read the call site |
| `0x173` | 6 | `seg1:0x1275` | — | internal only — read the call site |
| `0x174` | 8 | `seg1:0x1282` | — | internal only — read the call site |
| `0x176` | 16 | `seg1:0x13ae` | **DLLENTRYPOINT** | DIRECT |
| `0x177` | 20 | `seg1:0x1430` | **DRAWTEXTEX** | DIRECT |
| `0x178` | 4 | `seg1:0x1693` | **SETMESSAGEEXTRAINFO** | DIRECT |
| `0x17a` | 10 | `seg1:0x16a0` | **SETPROPEX** | DIRECT |
| `0x17b` | 6 | `seg1:0x1500` | **GETPROPEX** | DIRECT |
| `0x17c` | 6 | `seg1:0x1645` | **REMOVEPROPEX** | DIRECT |
| `0x17e` | 6 | `seg1:0x16c7` | **SETWINDOWCONTEXTHELPID** | DIRECT |
| `0x17f` | 2 | `seg1:0x1534` | **GETWINDOWCONTEXTHELPID** | DIRECT |
| `0x180` | 6 | `seg1:0x166c` | **SETMENUCONTEXTHELPID** | DIRECT |
| `0x181` | 2 | `seg1:0x14bf` | **GETMENUCONTEXTHELPID** | DIRECT |
| `0x182` | 0 | `seg1:0x3ee8` | — | internal only — read the call site |
| `0x185` | 14 | `seg1:0x158f` | **LOADIMAGE** | DIRECT |
| `0x186` | 12 | `seg1:0x136d` | **COPYIMAGE** | DIRECT |
| `0x187` | 14 | `seg1:0x16e1` | **SIGNALPROC32** | DIRECT |
| `0x18a` | 18 | `seg1:0x1409` | **DRAWICONEX** | DIRECT |
| `0x18b` | 6 | `seg1:0x148b` | **GETICONINFO** | DIRECT |
| `0x18d` | 4 | `seg1:0x1638` | **REGISTERCLASSEX** | DIRECT |
| `0x18e` | 10 | `seg1:0x1471` | **GETCLASSINFOEX** | DIRECT |
| `0x18f` | 8 | `seg1:0x1346` | **CHILDWINDOWFROMPOINTEX** | DIRECT |
| `0x190` | 0 | `seg1:0x064c` | **FINALUSERINIT** | DIRECT |
| `0x192` | 6 | `seg1:0x086e` | — | internal only — read the call site |
| `0x193` | 6 | `seg1:0x10a1` | — | internal only — read the call site |
| `0x194` | 10 | `seg1:0x06ce` | — | internal only — read the call site |
| `0x196` | 18 | `seg1:0x034c` | — | internal only — read the call site |
| `0x197` | 18 | `seg1:0x0366` | — | internal only — read the call site |
| `0x198` | 14 | `seg1:0x0359` | **CREATECURSORICONINDIRECT** | DIRECT |
| `0x199` | 10 | `seg1:0x0b3b` | **INITTHREADINPUT** | DIRECT |
| `0x19a` | 12 | `seg1:0x09ce` | — | internal only — read the call site |
| `0x19b` | 10 | `seg1:0x01e0` | — | internal only — read the call site |
| `0x19c` | 6 | `seg1:0x0c5c` | — | internal only — read the call site |
| `0x19d` | 6 | `seg1:0x0420` | — | internal only — read the call site |
| `0x19e` | 12 | `seg1:0x0b6f` | — | internal only — read the call site |
| `0x19f` | 0 | `seg1:0x0380` | **CREATEPOPUPMENU** | DIRECT |
| `0x1a0` | 16 | `seg1:0x103d` | — | internal only — read the call site |
| `0x1a1` | 0 | `seg1:0x07f9` | **GETMENUCHECKMARKDIMENSIONS** | DIRECT |
| `0x1a2` | 10 | `seg1:0x0ed1` | — | internal only — read the call site |
| `0x1a6` | 10 | `seg1:0x128f` | — | internal only — read the call site |
| `0x1a7` | 10 | `seg1:0x129c` | — | internal only — read the call site |
| `0x1ab` | 12 | `seg1:0x1457` | **FINDWINDOWEX** | DIRECT |
| `0x1ac` | 14 | `seg1:0x16ee` | **TILEWINDOWS** | DIRECT |
| `0x1ad` | 14 | `seg1:0x131f` | **CASCADEWINDOWS** | DIRECT |
| `0x1ae` | 8 | `seg1:0x0b06` | — | internal only — read the call site |
| `0x1af` | 4 | `seg1:0x01b9` | — | internal only — read the call site |
| `0x1b0` | 4 | `seg1:0x0185` | — | internal only — read the call site |
| `0x1b1` | 2 | `seg1:0x0a02` | — | internal only — read the call site |
| `0x1b2` | 2 | `seg1:0x0a0f` | — | internal only — read the call site |
| `0x1b3` | 2 | `seg1:0x0a29` | — | internal only — read the call site |
| `0x1b4` | 2 | `seg1:0x0a1c` | — | internal only — read the call site |
| `0x1b5` | 6 | `seg1:0x01c6` | — | internal only — read the call site |
| `0x1b6` | 6 | `seg1:0x0192` | — | internal only — read the call site |
| `0x1b9` | 10 | `seg1:0x1568` | **INSERTMENUITEM** | DIRECT |
| `0x1bb` | 10 | `seg1:0x14d9` | **GETMENUITEMINFO** | DIRECT |
| `0x1bd` | 12 | `seg1:0x03ce` | **DEFFRAMEPROC** | WRAPPER |
| `0x1be` | 10 | `seg1:0x1686` | **SETMENUITEMINFO** | DIRECT |
| `0x1bf` | 10 | `seg1:0x03db` | **DEFMDICHILDPROC** | WRAPPER |
| `0x1c0` | 12 | `seg1:0x13c8` | **DRAWANIMATEDRECTS** | DIRECT |
| `0x1c1` | 24 | `seg1:0x1423` | **DRAWSTATE** | DIRECT |
| `0x1c2` | 20 | `seg1:0x137a` | **CREATEICONFROMRESOURCEEX** | DIRECT |
| `0x1c3` | 6 | `seg1:0x105d` | — | internal only — read the call site |
| `0x1c4` | 34 | `seg1:0x039a` | — | internal only — read the call site |
| `0x1c5` | 10 | `seg1:0x0a9e` | — | internal only — read the call site |
| `0x1c6` | 14 | `seg1:0x0178` | — | internal only — read the call site |
| `0x1c7` | 6 | `seg1:0x07c5` | **GETICONID** | DIRECT |
| `0x1c8` | 4 | `seg1:0x0ac5` | **LOADICONHANDLER** | DIRECT |
| `0x1c9` | 2 | `seg1:0x0454` | — | internal only — read the call site |
| `0x1ca` | 2 | `seg1:0x0447` | — | internal only — read the call site |
| `0x1cc` | 10 | `seg1:0x07df` | **GETINTERNALWINDOWPOS** | DIRECT |
| `0x1cd` | 12 | `seg1:0x0eaa` | **SETINTERNALWINDOWPOS** | DIRECT |
| `0x1ce` | 4 | `seg1:0x023b` | **CALCCHILDSCROLL** | DIRECT |
| `0x1cf` | 10 | `seg1:0x0c90` | **SCROLLCHILDREN** | DIRECT |
| `0x1d0` | 12 | `seg1:0x04e3` | — | internal only — read the call site |
| `0x1d1` | 6 | `seg1:0x04d6` | **DRAGDETECT** | WRAPPER |
| `0x1d2` | 6 | `seg1:0x04f0` | — | internal only — read the call site |
| `0x1d7` | 8 | `seg1:0x0b13` | — | internal only — read the call site |
| `0x1d8` | 4 | `seg1:0x019f` | — | internal only — read the call site |
| `0x1d9` | 8 | `seg1:0x01ac` | — | internal only — read the call site |
| `0x1db` | 10 | `seg1:0x16ad` | **SETSCROLLINFO** | DIRECT |
| `0x1dc` | 8 | `seg1:0x150d` | **GETSCROLLINFO** | DIRECT |
| `0x1dd` | 4 | `seg1:0x14b2` | **GETKEYBOARDLAYOUTNAME** | DIRECT |
| `0x1de` | 6 | `seg1:0x159c` | **LOADKEYBOARDLAYOUT** | DIRECT |
| `0x1df` | 8 | `seg1:0x15b6` | **MENUITEMFROMPOINT** | DIRECT |
| `0x1e0` | 2 | `seg1:0x12a9` | **GETUSERLOCALOBJTYPE** | DIRECT |
| `0x1e1` | 0 | `seg1:0x12b6` | **HARDWARE_EVENT** | DIRECT |
| `0x1e2` | 6 | `seg1:0x12c3` | — | internal only — read the call site |
| `0x1e3` | 10 | `seg1:0x12d0` | — | internal only — read the call site |
| `0x1f2` | 0 | `seg1:0x1464` | **BEAR498** | DIRECT |
| `0x1f4` | 0 | `seg1:0x0618` | — | internal only — read the call site |
| `0x215` | 0 | `seg1:0x1749` | **WNETINITIALIZE** | DIRECT |
| `0x216` | 6 | `seg1:0x1756` | **WNETLOGON** | DIRECT |
| `0x217` | 6 | `seg1:0x12dd` | **NOTIFYWOW** | DIRECT |
| `0x219` | 10 | `seg1:0x1109` | **WOWWORDBREAKPROC** | DIRECT |
| `0x21a` | 12 | `seg1:0x1227` | **MOUSEEVENT** | DIRECT |
| `0x21b` | 8 | `seg1:0x1200` | **KEYBDEVENT** | DIRECT |
| `0x21c` | 0 | `seg1:0x151a` | **GETSHELLWINDOW** | DIRECT |
| `0x21d` | 4 | `seg1:0x13bb` | **DOHOTKEYSTUFF** | DIRECT |
| `0x21e` | 2 | `seg1:0x1652` | **SETCHECKCURSORTIMER** | DIRECT |
| `0x21f` | 6 | `seg1:0x1679` | **SETMENUDEFAULTITEM** | DIRECT |
| `0x229` | 4 | `seg1:0x1387` | **DESTROYICON32** | DIRECT |
| `0x22a` | 16 | `seg1:0x1305` | **BROADCASTSYSTEMMESSAGE** | DIRECT |
| `0x22b` | 2 | `seg1:0x154e` | **HACKTASKMONITOR** | DIRECT |
| `0x22d` | 8 | `seg1:0x132c` | **CHANGEDISPLAYSETTINGS** | DIRECT |
| `0x22e` | 0 | `seg1:0x147e` | **GETFOREGROUNDWINDOW** | DIRECT |
| `0x22f` | 2 | `seg1:0x165f` | **SETFOREGROUNDWINDOW** | DIRECT |
| `0x230` | 12 | `seg1:0x143d` | **ENUMDISPLAYSETTINGS** | DIRECT |
| `0x231` | 18 | `seg1:0x15d0` | **MSGWAITFORMULTIPLEOBJECTS** | DIRECT |
| `0x232` | 6 | `seg1:0x12f8` | **ACTIVATEKEYBOARDLAYOUT** | DIRECT |
| `0x233` | 4 | `seg1:0x1498` | **GETKEYBOARDLAYOUT** | DIRECT |
| `0x234` | 6 | `seg1:0x14a5` | **GETKEYBOARDLAYOUTLIST** | DIRECT |
| `0x235` | 4 | `seg1:0x172f` | **UNLOADKEYBOARDLAYOUT** | DIRECT |
| `0x236` | 0 | `seg1:0x1611` | **POSTPOSTEDMESSAGES** | DIRECT |
| `0x237` | 10 | `seg1:0x13fc` | **DRAWFRAMECONTROL** | DIRECT |
| `0x238` | 18 | `seg1:0x13e2` | **DRAWCAPTIONTEMP** | DIRECT |
| `0x239` | 0 | `seg1:0x1394` | **DISPATCHINPUT** | DIRECT |
| `0x23a` | 10 | `seg1:0x13ef` | **DRAWEDGE** | DIRECT |
| `0x23b` | 10 | `seg1:0x13d5` | **DRAWCAPTION** | DIRECT |
| `0x23c` | 10 | `seg1:0x16ba` | **SETSYSCOLORSTEMP** | DIRECT |
| `0x23d` | 12 | `seg1:0x1416` | **DRAWMENUBARTEMP** | DIRECT |
| `0x23e` | 6 | `seg1:0x14cc` | **GETMENUDEFAULTITEM** | DIRECT |
| `0x23f` | 10 | `seg1:0x14e6` | **GETMENUITEMRECT** | DIRECT |
| `0x240` | 10 | `seg1:0x1339` | **CHECKMENURADIOITEM** | DIRECT |
| `0x241` | 14 | `seg1:0x16fb` | **TRACKPOPUPMENUEX** | DIRECT |
| `0x242` | 6 | `seg1:0x16d4` | **SETWINDOWRGN** | DIRECT |
| `0x243` | 4 | `seg1:0x1541` | **GETWINDOWRGN** | DIRECT |
| `0x244` | 10 | `seg1:0x1360` | **CHOOSEFONT_CALLBACK16** | DIRECT |
| `0x245` | 10 | `seg1:0x144a` | **FINDREPLACE_CALLBACK16** | DIRECT |
| `0x246` | 10 | `seg1:0x15dd` | **OPENFILENAME_CALLBACK16** | DIRECT |
| `0x247` | 10 | `seg1:0x162b` | **PRINTDLG_CALLBACK16** | DIRECT |
| `0x248` | 10 | `seg1:0x1353` | **CHOOSECOLOR_CALLBACK16** | DIRECT |
| `0x249` | 14 | `seg1:0x15ea` | **PEEKMESSAGE32** | DIRECT |
| `0x24a` | 12 | `seg1:0x14f3` | **GETMESSAGE32** | DIRECT |
| `0x24b` | 6 | `seg1:0x1708` | **TRANSLATEMESSAGE32** | DIRECT |
| `0x24c` | 6 | `seg1:0x13a1` | **DISPATCHMESSAGE32** | DIRECT |
| `0x24d` | 8 | `seg1:0x1312` | **CALLMSGFILTER32** | DIRECT |
| `0x24e` | 8 | `seg1:0x1582` | **ISDIALOGMESSAGE32** | DIRECT |
| `0x24f` | 12 | `seg1:0x1604` | **POSTMESSAGE32** | DIRECT |
| `0x250` | 14 | `seg1:0x161e` | **POSTTHREADMESSAGE32** | DIRECT |
| `0x251` | 4 | `seg1:0x15c3` | **MESSAGEBOXINDIRECT** | DIRECT |
| `0x252` | 12 | `seg1:0x1575` | **INSTALLIMT** | DIRECT |
| `0x253` | 12 | `seg1:0x1715` | **UNINSTALLIMT** | DIRECT |
| `0x254` | 12 | `seg1:0x12ea` | — | internal only — read the call site |
| `0x2080` | 0 | `seg1:0x4e1f` | — | internal only — read the call site |
