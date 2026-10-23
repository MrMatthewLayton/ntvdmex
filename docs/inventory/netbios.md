# NetBIOS (INT 5Ch / INT 2Ah) — the DOS network interface

GH #8 · s91 · device `src/vdd/vdd_net.c` · host backend `net_submit` in `src/host/main.c` ·
probe `tests/probes/dos/p_netb.asm` · off-VM `tests/unit/net_test.c`

## Why NetBIOS and not a packet driver

#8 asked for "packet driver / NDIS-era interface as appropriate". The reference decides:
stock XP NTVDM answers **INT 5Ch** (its NetBIOS VDD hands the program's NCB to the NT
NetBIOS driver) and **INT 2Ah AH=00h** (installed), and it has no packet driver — raw
Ethernet frames need a capture driver a user-mode process on XP does not have. So the
period interface a DOS program under NT gets is NetBIOS, and that is what this is.

## What is implemented

| Surface | State | Notes |
|---|---|---|
| INT 5Ch, every NCB command (wait form) | ✅ | the 64-byte DOS NCB is converted field by field to Win32's `NCB` and run through `Netbios()` (netapi32, loaded on first use); retcode, lsn, num, length, callname and cmd_cplt written back |
| DOS lana N → NT lana | ✅ | the Nth entry of `NCBENUM`; past the end → 23h (invalid adapter) |
| implicit RESET | ✅ | a Win32 lana must be reset before use and a DOS program often never does; the first command on a lana resets it with defaults |
| RESET's sessions/names | ✅ | DOS lsn/num → Win32 callname[0]/[1] |
| no-wait commands (bit 7) | ✅ | completed before INT 5Ch returns: AL = immediate 00h, the final code already in retcode/cmd_cplt (never FFh). A polling program sees a command that finished at once |
| POST routines | ✅ | run as INT 5Ch returns: the host pushes one more interrupt frame so the stub's IRET enters POST with ES:BX = the NCB and POST's IRET returns to the caller (`p_netb`: called, ES:BX = NCB, = stock — stock calls it seconds later, when the name registration completes) |
| INT 2Ah AH=00h | ✅ | AH=01h, as stock |
| INT 2Ah AH=01h/04h | ✅ | execute the NCB at ES:BX; AL = retcode, AH = 0/1 |
| INT 2Ah AH=80h-82h | ✅ | critical sections: nothing to serialise |
| DPMI clients: INT 5Ch / INT 2Ah | ✅ (unmeasured) | #315: the PM dispatcher resolves ES:(E)BX and the NCB's buffer pointer through the client's selectors and runs the same service (`VddNetBiosCall`, `host_dpmi_int.c`). It used to end the run ("unexpected PM stop") |
| DPMI clients: POST routines | ⚠ not called | a PM client's no-wait NCB completes before INT 5Ch returns, like V86, but its POST (a selector:offset) is not called -- the V86 trick pushes a real-mode frame; a PM one of the client's width is not built. Counted (`PostsSkippedProtectedMode`), logged once |

## Measured against stock (test machine, s91)

14/14 agree (the 10 below plus a no-wait ADD NAME with a POST routine): invalid command → 03h in AL and retcode; RESET 00h; ADAPTER STATUS of `*`
00h with 60 (3Ch) bytes and a non-zero adapter address; ADD NAME 00h with a name number;
DELETE NAME 00h; INT 2Ah AH=00h → AH=01h.

## Also fixed on the way: a claimed vector must have a stub

`vdd_claim_int` only reached a device for vectors the host had wired a BOP stub for by
number (10h, 14h, 16h, 1Ah …). INT 2Ah and 5Ch now have BIOS stubs beside INT 11h-29h
(`bios_ints[]`) and `v86_bios_bop` hands them to the bus — from our own stub only (the
s78 origin rule). Any OTHER claimed user vector (60h-66h, 68h-6Fh, 78h-FEh) gets a
generic stub (`DOS_GENSTUB_OFF`, 16 slots; #315), which is what a third-party driver's
`claim_int` uses (`sdk/sample/intecho.c`, INT 61h).

## IPX (#315)

Stock XP NTVDM offers IPX through `vwipxspx.dll` when the **NWLink IPX/SPX** protocol is
installed; without it `INT 2Fh AX=7A00h` answers "not installed". Ours follows the same
split, through Winsock:

| Surface | State | Notes |
|---|---|---|
| `INT 2Fh AX=7A00h` (V86) | ✅ | ws2_32.dll is **loaded** (never imported) on the first ask; if an `AF_IPX` socket can be made (NWLink), AL=FFh and ES:DI = `DOS_HDLR_SEG:0084h` (`BOP 7Ah ; RETF`); otherwise nothing is touched -- AL stays 00h, "not installed" (RBIL INT 2F/7A00) |
| `INT 2Fh AX=7A00h` from PM | deliberate "no" | the entry and its ECBs are real-mode; a DPMI program uses 0300h/0301h, as its extender does |
| `00h` open socket (DX big-endian, 0 = dynamic 4000h-7FFFh) | ✅ | AL 00h / FEh table full (20 sockets) / FFh already open |
| `01h` close | ✅ | its listening ECBs end with FCh |
| `02h` get local target | ✅ | one network: the immediate address is the destination node; CX = 1 tick |
| `03h` send | ✅ | fragments gathered (30..576 bytes); IPX fills checksum FFFFh, length, transport control, source; Winsock sends the data with the packet type set through `IPX_PTYPE`. Completion 00h / FDh malformed / FEh undeliverable / FFh socket not open |
| `04h` listen | ✅ | FIFO per socket; a packet is scattered into the first listener's fragments with its IPX header (`IPX_RECVHDR`), immediate address = the sender's node; FDh when it overflows |
| `06h` cancel, `08h` interval marker, `09h` internetwork address, `0Ah` relinquish | ✅ | 08h = 0040:006Ch; 09h from the bound socket's name |
| Event service routines | ⚠ not called | in-use and completion are final when the event happens, so a POLLING program works (Doom's IPXSETUP polls); one that relies only on its ESR does not. Counted (`EsrsSkipped`) |
| `05h`/`07h` IPX/AES events, SPX (`10h`-`16h`), `INT 7Ah` | ❌ | AL as passed, counted (`UnsupportedCalls`) |
| delivery | | on each IPX call and each BIOS tick (the INT 08h BOP) -- not the instant a packet lands |

Off-VM: `tests/unit/ipx_test.c` (loopback backend: open/close, dynamic sockets, the header
IPX fills, send-to-self through a listening ECB, drop with no listener, overflow,
malformed, cancel, interval marker). **Unmeasured on the rig**: it needs NWLink installed
there, and an IPX program (IPXSETUP for Doom) against a second machine or a second VDM.
