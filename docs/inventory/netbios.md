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
| DPMI clients | ❌ | INT 5Ch from protected mode is not reflected |

## Measured against stock (rig, s91, `scripts/dospair.sh tests/probes/dos/p_netb.com`)

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
