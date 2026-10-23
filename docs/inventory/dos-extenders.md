# Inventory -- DOS extenders: DOS/4GW and DOS/16M

**Spec:** Tenberry (formerly Rational Systems) documentation for DOS/4GW and DOS/16M.
WARNING: **Not held in the repo.** [`../ref/SOURCES.md`](../ref/SOURCES.md) row "DOS
extenders" names it as a source and nothing more. **So nothing below is graded against an
extender's own documentation.** Every row is the host surface an extender reaches, marked
from our code. "Who uses it" comes only from a comment in our code, with the comment
cited. Where the repo has no evidence for a behaviour, it is listed as a question at the
end and not as a row.

**What this file covers.** DOS/4GW (Doom, Heretic, Hexen, Duke3D) and Rational's DOS/16M
(ZAR) are DPMI **clients** here. This file does not repeat the DPMI host. It lists the
host surface the extenders reach: detection, the mode switch, the INT 31h functions our
code says they call, their private INT 21h calls, their interrupt and exception rules,
how the program is launched (argv[0], the environment, the PSP), and the host machinery
that exists only because of them. Per-function DPMI status lives in [dpmi.md](dpmi.md);
VCPI in [xms-ems.md](xms-ems.md) §8; INT 15h `87h`/`88h` in [bios-misc.md](bios-misc.md)
§4; the LE format in [exe-formats.md](exe-formats.md) §4.

WARNING: **The `main.c:` line numbers in dpmi.md, bios-misc.md and exe-formats.md are
older than the `main.c` split.** `src/host/main.c` now has 1045 lines. The citations
below are the current locations, read on 2026-10-23.

**Our implementation:** host code, not a VDD.
- Detection: INT 2Fh `1687h`, V86 `src/host/host_bios.c:1411-1428`, PM
  `src/host/host_dpmi_int.c:7077-7091`.
- Mode switch: `DpmiStartClientSession` (`src/host/host_dpmi_client.c:2821`) ->
  `DpmiSwitchToProtectedMode` (`src/vdm/dpmi.c:93-255`).
- INT 31h: `DpmiServiceInt31` (`host_dpmi_int.c:6179`), one `case` per function
  (`:6302-7031`).
- PM INT 21h: `DpmiServiceInt21` (`host_dpmi_int.c:5633`), the per-function helpers
  `:4426-5127`, and `DpmiServiceClient32Int21` (`:7594-7671`).
- PM interrupt dispatch: `DpmiServicePmIntBody` (`host_dpmi_int.c:8195`),
  `DpmiDispatchToPmHandler` (`src/host/host_dpmi.c:2365`), `DpmiInjectPmIrq`
  (`host_dpmi.c:4034`).
- Launch: argv[0] at start-up (`src/host/host_startup.c:1266-1334`); argv[0] and the
  child environment at EXEC (`src/host/host_dos.c:693-776`).

**Off-VM:** `tests/unit/exec_test.c:143-146` (an LE/LX file is a DOS program, not a
Windows one); `tests/unit/mcb_test.c:470-508` (`DOS4GVM=@ZAR.VMC` reaches the environment
block); `tests/unit/dpmisvc_test.c` and `tests/unit/rmcs_test.c` for the DPMI rules
underneath. **No off-VM test covers the 64-byte argv[0] rule.** A grep of `tests/` for
`ARGV0`/`argv[0]` finds none.
**Probes:** `tests/probes/dos/p_child.asm` (the child's environment name slot, the
DOS4GW.EXE case), `p_env.asm` (the environment as text, DOS4GVM named in its header),
`pm32flat.asm` (the base-0 flat-selector model), `pm32gfx.asm`, `pmfault.asm` (a PM
`STI`), `p_dpmi31.asm` (INT 31h). WARNING: `pm32sw.asm`'s header still describes the run-81
behaviour (a D/B=1 CS straight after the switch). `src/vdm/dpmi.c:125-159` reverted that
behaviour.
**Oracle:** none for this surface. dpmi.md's header notes that MS-DOS 6.22, DOSBox-X and
PCem have no DPMI host. Stock NTVDM is the only one, and no probe here has been run against
it. The guest shelf is the integration test.
**Marked:** 2026-10-23, **from the code** (#257).

---

## Headline

**Every host service that a comment in our code says an extender calls has an arm.** No
row below is MISS for a call our code records an extender making. What stays open:

1. **INT 31h `0306h` hands out two raw-switch entries that nothing services.**
   `host_dpmi_int.c:6858-6875` returns `DOS_HDLR_SEG:DPMI_RAW2PM_OFF` and
   `selector:DPMI_RAW2RM_OFF`. `host_startup.c:4241-4246` plants BOP `58h`/`59h` there.
   No dispatcher in `src/` tests for `DPMI_RAW2PM_BOP` or `DPMI_RAW2RM_BOP`: a grep finds
   only the defines, the plant and the `0306h` arm. In V86, BOP `58h` from a guest is
   also NTVDM's third-party BOP (`NTVDM_BOP_ISV`, `src/vdm/ntvdm.h:257`,
   `host_exec.c:2211`). dpmi.md §5 and [bop.md](bop.md) both mark this IMPL. **Here it is
   PART.** Whether an extender ever jumps to these entries is not known (§9).
2. **Only INT 21h goes to an extender's own PM handler.** DOS/4GW also installs handlers
   for other vectors. The comment that scopes routing to INT 21h (`host_dpmi_int.c:8360-8369`)
   gives as its reason that "`0300` ... currently only implements INT 21h". #247 removed
   that limit (dpmi.md §5), so the comment is out of date and the scope is a choice that
   can now be revisited.
3. **argv[0] is limited to 64 bytes at start-up but not always at EXEC.** At EXEC the
   path is shortened only when the program sits in the current directory
   (`host_dos.c:764-773`).
4. **A 4 GB flat selector cannot exist on XP.** The host clamps it to the largest
   descriptor the kernel accepts (`host_dpmi.c:618-640`). That is a platform limit, not a
   defect, but a client that relies on the top 2 GB will fault.

| Group | Units | IMPL | PART | MISS | N/A | UNMEASURED |
|---|---|---|---|---|---|---|
| §1 Detecting a host | 7 | 3 | 1 | 2 | -- | 1 |
| §2 The mode switch and the initial state | 4 | 4 | -- | -- | -- | -- |
| §3 INT 31h services the extenders call | 15 | 10 | 5 | -- | -- | -- |
| §4 Extender-private and PM INT 21h | 8 | 6 | 1 | -- | -- | 1 |
| §5 Interrupts | 7 | 6 | 1 | -- | -- | -- |
| §6 Exceptions | 4 | 2 | 1 | -- | -- | 1 |
| §7 Selectors and memory | 5 | 3 | 2 | -- | -- | -- |
| §8 Launch: the program file, argv[0], the environment, the PSP | 8 | 6 | 1 | -- | 1 | -- |
| **Total** | **58** | **40** | **12** | **2** | **1** | **3** |

UNMEASURED means the code gives an answer, but nothing in the repo says whether it is the
answer the extender expects.

---

## 1. Detecting a host

The extenders ask how they can reach protected mode. These are the questions they could
ask. Only `1687h` and the mode switch have a recorded extender caller (§2). For the
others, which ones DOS/4GW or DOS/16M actually ask under a DPMI host is not evidenced (§9).

| Unit | Status | Where / what is missing |
|---|---|---|
| INT 2Fh `1687h` from V86: AX=0, BX bit 0 (32-bit supported), CL = `DPMI_CPU_CLASS`, DX = 0.90, SI = 0, ES:DI = the switch entry | **IMPL** | `host_bios.c:1411-1428`. dpmi.md §1 |
| INT 2Fh `1687h` from PM (a client asking again from inside) | **IMPL** | `host_dpmi_int.c:7077-7091`, same answer. BX is written as a literal `1` here, and as `MULTIPLEX_DPMI_32BIT_SUPPORTED` in the V86 arm |
| INT 2Fh `4300h`/`4310h` XMS | **IMPL** | `host_bios.c:1402-1410`; does not answer while XMS is switched off. xms-ems.md §1. No extender caller is recorded |
| INT 67h `DE00h` VCPI installation check | **MISS** | `HostEms` `default:` returns `AH=84h` (`host_dos.c:2732-2734`), so every client is told consistently that there is no VCPI server. Scope not decided: xms-ems.md §8 |
| VCPI `DE01h`-`DE0Ch` | **MISS** | xms-ems.md §8 (14 rows) |
| INT 15h `88h` extended memory size | **PART** | V86 `host_bios.c:836+`, PM `host_dpmi_int.c:7246-7249`: always `CMOS_EXTENDED_KB`, which is the same memory XMS hands out (bios-misc.md §4). The comment at `host_bios.c:838-846` says DOS/16M (ZAR) never calls the V86 arm: it installs its own PM INT 15h handler. The comment at `host_dpmi_client.c:617-620` says DOS/16M picks its memory manager by a vector byte at `ds:0x34` (`0x15` = "size with INT 15h AH=88h") |
| INT 15h functions we do not implement (`AH=86h` CF=1, logged `INT15 UNIMPL`) | **UNMEASURED** | `host_bios.c:1050-1073`. The comment there: "with an argument the trace is identical until here, DOS/4GW takes this CF=1, and exits CLEANLY". The log line names the function, but the comment does not record it. `E801h` is refused (bios-misc.md §4 MISS); whether that is the call is not known |

## 2. The mode switch and the initial state

| Unit | Status | Where / what is missing |
|---|---|---|
| Far call to the switch entry with AX bit 0 = the client's width (DOS/4GW passes 1) | **IMPL** | `host_dpmi_client.c:2833-2844`. The width is recorded in `g_DpmiIsClient32` (`dpmi.c:162`) and used for API widths and frame widths (§5, §6) |
| The initial CS/DS/SS are 16-bit, based, 64K, **even for a 32-bit client** | **IMPL** | `dpmi.c:125-160`, `host_dpmi_client.c:2866-2873`. The argument in the code: on the failure path, the bytes after the client's `call far` run as real-mode code, so they cannot be 32-bit code on success. The comment records Doom's post-switch bytes decoding as a textbook 16-bit stub (`dpmi.c:137-146`) |
| ES = a selector for the PSP with a `100h` limit | **IMPL** | `host_dpmi_client.c:2909-2924`. The comment at `:2892-2900` says DOS/4GW's PM module loads PSP+`2Ch` through the initial ES as a selector |
| PSP:`2Ch` (environment) converted to a selector; a zero word is left at zero | **IMPL** | `host_dpmi_client.c:2926-2943` |

## 3. INT 31h services the extenders call

This lists the functions a comment in our code ties to DOS/4GW or DOS/16M. The status is
dpmi.md's, re-cited to the current lines. A function missing from this list means only
that no comment names an extender caller, not that the extender never calls it.

| AX | Caller (per our comment) | Status | Where |
|---|---|---|---|
| `0000h` allocate descriptors | DOS/4GW, to build its own flat selectors (`dpmi.c:154-158`, `:167-169`) | **IMPL** | `host_dpmi_int.c:6314`; dpmi.md §2 |
| `0007h` set base | DOS/4GW (`dpmi.c:168-169`) | **IMPL** | `host_dpmi_int.c:6630`; dpmi.md §2 |
| `0008h` set limit | DOS/4GW (`dpmi.c:168-169`); ZAR (dpmi.md §2, "the ZAR fix") | **PART** | `host_dpmi_int.c:6631`; dpmi.md §2: the host chooses G, `8021h` not checked |
| `0009h` set access rights | DOS/4GW sets D/B on its 32-bit code selector (`host_dpmi_int.c:6686-6687`) and retypes a loaded module to code with `0xFB` (`host/host_types.h:221-223`); ZAR asks for DPL 0 (`host_dpmi.c:584-595`) | **PART** | `host_dpmi_int.c:6632`, `:6680-6694`. DPL is forced to 3 (§7); the access byte is not validated (dpmi.md §2) |
| `000Bh`/`000Ch` get / set descriptor | DOS/4GW clears D/B through them (`host_dpmi_int.c:6743-6752`) | **PART** | `:6800-6834`; dpmi.md §2: `000Ch` rejects indices >= 512 |
| `0200h`/`0201h` get / set real-mode vector | DOS/16M (ZAR): `0200h` x 256, then `0201h` x 39 (`host_dpmi_int.c:6399-6402`) | **IMPL** | `:6418-6445`; the IVT is the store |
| `0202h`/`0203h` get / set exception handler | DOS/4GW: "45 of these calls" in the session-16 trace (`src/host/host_state.c:585-594`) | **IMPL** | `host_dpmi_int.c:6497-6564` |
| `0204h`/`0205h` get / set PM vector | DOS/4GW reads the vector back with `0204h` to walk its chain (`host_dpmi_int.c:6457-6463`, `:116-121`); DOS/16M makes 33 `0205h` calls (`host_bios.c:844-845`) | **IMPL** | `:6446-6496`; the offset is zero-extended for a 32-bit client (`:6450-6467`) |
| `0300h` simulate real-mode interrupt | "a Watcom/DOS4GW program ... calls int86(), and int86 under an extender is 0300" (`host_dpmi_int.c:884-888`, evidence from ZAR's video driver) | **IMPL** | `:6955`; dpmi.md §5 (#247) |
| `0302h` call real-mode procedure with an IRET frame | DOS/4GW's PM INT 21h handler forwards to real-mode DOS this way (`host_dpmi_int.c:383-386`) | **PART** | `:6961`. dpmi.md §5's PART: a procedure that never returns still gets CF=0 after 128 nested events |
| `0305h` state save / restore addresses | not named | **IMPL** | `:6836-6857`; both routines are a `RETF` (`host_startup.c:4247-4248`) |
| `0306h` raw mode-switch addresses | DOS/4GW: "it tests CF from 0306" (`host_startup.c:286-287`, `host_dpmi_int.c:6860-6864`) | **PART** | The addresses are returned with CF=0 (`host_dpmi_int.c:6858-6875`), but nothing services a jump to either entry (Headline 1) |
| `0501h` allocate memory block | DOS/4GW: one block per LE object, sized to the object's page-rounded virtual size (`host_types.h:337-345`, `host_dpmi_int.c:1252-1260`) | **IMPL** | `:6937`; dpmi.md §7 |
| `0701h`-`0703h` paging hints | Doom: "asks 0702 six times" (`host_dpmi_int.c:6561`) | **IMPL** | `:6573-6587`, advisory CF=0 |
| `0A00h` vendor API | DOS/4GW: "asks, is refused, and carries on" (`host_dpmi_int.c:6791-6797`) | **IMPL** | Refused with CF=1. dpmi.md §8 marks it MISS against stock NTVDM's vendor API. For the extenders, our comment records that the refusal is enough. Counted IMPL here because the refusal is the answer this caller is recorded as taking |

## 4. Extender-private and PM INT 21h

| Unit | Status | Where / what is missing |
|---|---|---|
| A PM INT 21h goes **first** to the client's own PM handler, if it installed one | **IMPL** | `host_dpmi_int.c:8426-8434` -> `DpmiDispatchToPmHandler` (`host_dpmi.c:2365`). Why, in the code's words: DOS/4GW installs a PM INT 21h handler and "calls its OWN private extender functions through it" (`host_dpmi.c:279-286`) |
| `AX=FF80h` (DX=`1301h`, ES = region): "lock this memory" | **IMPL** | When it reaches the host it is answered CF=0 (`host_dpmi_int.c:6080-6115`), the same answer INT 31h `0600h` gives. On CF=1 the extender prints `DOS/16M error: [34] DPMI host error (cannot lock stack)` (`:6082-6086`; `host_dpmi.c:281-283`). dpmi.md §10 |
| `AX=FF00h` and any other `FFxxh` | **UNMEASURED** | `host_types.h:296-299` names DOS/4GW's private `FF00h` as a call it passes down its chain. If it reaches the host it falls to `pmInt21Unhandled` and is refused with CF=1 plus a full register dump (`host_dpmi_int.c:6117-6170`). Nothing records what DOS/4GW expects back |
| `25h`/`35h` for vectors `08h`-`0Fh`, 32-bit client: the host owns them | **PART** | `DpmiServiceClient32Int21` (`host_dpmi_int.c:7594-7671`, reason at `:8386-8401`): Doom hooks its timer with `AX=2508h`, and if DOS/4GW services that itself it fails with error 1001. `35h` returns our default stub, not the extender's arming stub (`:7626-7649`). WARNING: the range check is `VECTOR_IRQ0`-`VECTOR_IRQ7` (`:7605`), so the slave PIC's vectors `70h`-`77h` are not covered |
| `25h`/`35h` for every other vector: they act on the **PM** table | **IMPL** | `DpmiInt21SetGetVector` (`host_dpmi_int.c:4624-4702`). The comment at `:4648-4660` derives this from DOS/4GW's own call with DS = its code selector. It is marked there as "forced-by-the-data, not oracle-confirmed" |
| `48h` allocate: a **selector** in AX only (the other registers preserved) | **IMPL** | `DpmiInt21Allocate` (`host_dpmi_int.c:4779-4903`). Why: "DOS/4GW keeps the request's BYTE SIZE in DX" (`:4831-4840`). The block is stamped with the current PSP (`src/dos/dos_int21.c:4914-4923`) |
| `4Ah` resize, from PM (the descriptor's limit updated) | **IMPL** | `DpmiInt21Resize` (`host_dpmi_int.c:4537-4623`). DOS/4GW calls it right after `48h` (`:4549-4552`) |
| `06h` direct console I/O (how DOS/4GW prints its fatal errors) and `44h` AL=`00h` for handles 0-4 | **IMPL** | `host_dpmi_int.c:5884-5915`, handled in the register-only list `:6038-6046` |

## 5. Interrupts

| Unit | Status | Where / what is missing |
|---|---|---|
| Every PM vector starts at a host stub (not `0:0`), so an extender that chains to the "previous" handler lands somewhere | **IMPL** | `host_types.h:293-300`, installed by `DpmiInstallDefaultPmHandlers` (called at `host_dpmi_client.c:2944`) |
| A software INT for a vector other than 21h, raised by the extender, reaches its PM handler | **PART** | Only INT 21h is routed to the client (`host_dpmi_int.c:8360-8369`, `:8426`). Vectors 10h, 16h, 33h, 1Ah and 08h are serviced by the host directly (`:8447-8524`), even when DOS/4GW has installed a handler. The reason the comment gives is out of date (Headline 2) |
| The frame pushed for a PM interrupt follows the **client's** declared width, not the handler selector's D bit | **IMPL** | `host_dpmi.c:2403-2412` (dispatch), `:4107-4115` (injection). DOS/4GW's 16-bit INT 21h handler ends in `66 CF` (IRETD) |
| Hardware IRQs are delivered through the extender's vector table, never straight to the application's ISR | **IMPL** | `host_types.h:363-372`, `host_dpmi.c:4099-4103`. Bypassing it gave `DOS/4GW Professional fatal error (1001): error in interrupt chain` |
| For a 32-bit client, an IRQ is not injected while the CPU is in the extender's own 16-bit code, and IRQ0 waits until the application has hooked INT 08h | **IMPL** | `host_dpmi.c:4081-4103`. This is a host heuristic, not a rule from any spec: "the extender's modules are all 16-bit selectors" (`:4091-4093`) |
| DOS/4GW's PM pass-up handler chains to our default stub, which reflects to real mode: IVT still ours | **IMPL** | `host_dpmi_int.c:8526-8581` (the s80 keyboard case: `DOS/4GW Professional error (2002): transfer stack overflow`) |
| ...and when a guest hooked the real-mode vector: a nested-V86 reflection | **IMPL** | `host_state.c:548-566` (ZAR's real-mode Miles ISR at IVT[0Dh]); `DpmiReflectIrqToRm` (`host_dpmi.c:3397`). dpmi.md §4 |

## 6. Exceptions

| Unit | Status | Where / what is missing |
|---|---|---|
| A fault goes to the client's `0203h` handler, with a 32-bit frame for a 32-bit client | **IMPL** | `DpmiDeliverToClientHandler` (`host_dpmi_client.c:462`); the frame-width rule is at `:473-480` (ZAR, GH #23) |
| The handler's return is read **back from the frame**, so a handler that rewrites it is honoured | **IMPL** | `DpmiResumeAfterClientHandler` (`host_dpmi_client.c:742`), `:784-797`: DOS/4GW's #GP handler resumes at the faulting instruction after zeroing the bad selector on the faulting stack |
| A fault with no client handler | **PART** | `host_dpmi_client.c:1180-1186`: the run stops. dpmi.md §4 has the spec's default |
| A PM `STI`/`CLI` (DOS/4GW "brackets every DPMI call with CLI/STI", `tests/probes/dos/pmfault.asm:8-9`) | **UNMEASURED** | `src/vdm/ntvdm.h:140-163` says that at CPL 3 / IOPL 0 a PM `STI` raises the one #GP XP does not reflect, and that "a PM client that uses CLI/STI cannot run to completion" until the GH #18 reflect exists. `host_dpmi_client.c:967` logs "REAL-CPU PM fault reflect WORKS". Which statement is current was not worked out here (§9) |

## 7. Selectors and memory

| Unit | Status | Where / what is missing |
|---|---|---|
| A base-0, 4 GB, G=1 flat selector | **PART** | XP's LDT check caps base+limit at `MmHighestUserAddress`. The host retries with the largest descriptor that check accepts and logs the clamp (`host_dpmi.c:618-640`). The comment says stock NTVDM has the same cap. A platform limit; there is nothing more to build |
| DPL in a client descriptor | **PART** | Forced to 3 for any present descriptor (`host_dpmi.c:584-596`): NT rejects anything else. The requested byte is kept for `LAR`. ZAR asks for DPL 0 for its LFB selector |
| A client's linear address is a host VA (`0501h` = `VirtualAlloc`) | **IMPL** | `src/host/host_video.c:1634`, `host_types.h:229-233` |
| Raw `CD nn` in extender and application code is patched to a BOP: the LE object table says which `0501h` blocks are code | **IMPL** | `DpmiLeLearn` (`host_dpmi.c:1515`, called at `host_startup.c:4055-4058`), `DpmiScanCodeBlocks` (`:1597`), `DpmiPatchCodeRegion` (`:1007`). Why it is needed: a raw INT in PM is a #GP that XP does not reflect (`host_types.h:331-336`). The patcher's limits: `src/host/x86len.h:105-122` (DOS/4GW's version check follows the string `requires DOS/16M`) |
| A module the extender reads into conventional memory and retypes to code (`48h` + `0009h` access `0xFB`) gets patched too | **IMPL** | The patch map is keyed by linear address (`host_types.h:217-228`); `0009h` marking a region CODE triggers the scan (`host_dpmi_int.c:6694+`) |

## 8. Launch: the program file, argv[0], the environment, the PSP

| Unit | Status | Where / what is missing |
|---|---|---|
| An MZ whose `e_lfanew` points at `LE`/`LX` runs as a DOS program (the stub *is* the extender) | **IMPL** | `src/dos/dos_loader.h:55-75`; `tests/unit/exec_test.c:143-146` |
| Loading the LE image itself | **N/A** | The extender loads its own image through DPMI. exe-formats.md §4 |
| argv[0] (the environment's program-path slot) is the 8.3 short path | **IMPL** | `host_startup.c:1266-1291`. The comment says DOS/4GW re-opens the program by that name and tokenises it, so a path with spaces fails. If `GetShortPathNameA` cannot answer, the long path is kept |
| argv[0] longer than 62 chars at start-up -> the bare file name | **IMPL** | `host_startup.c:1293-1333`, `EXTENDER_ARGV0_SAFE_LENGTH = 62` (`:388`). The comment records "DOS/4GW 1.97 copies argv[0] into a 64-BYTE BUFFER and does not bound it" and three paths of 62, 64 and 68 chars (`:1294-1302`). The bare name works because the current directory is the program's own |
| argv[0] of 64 chars or more at EXEC -> the bare file name | **PART** | `host_dos.c:735-774`, `DOS_EXTENDER_ARGV0_MAX = 64` (`src/dos/dos_sizes.h:19`). Only when the program is in the current directory (`:764-773`); otherwise the long name goes through. #208's Duke3D case is the recorded one (`:739-741`) |
| EXEC gives the child a **fresh** environment block with the name slot, whatever word 0 of the parameter block says | **IMPL** | `host_dos.c:693-713`: Heaven7's separate `DOS4GW.EXE` read its target from that slot, and Duke3D's `SETMAIN.EXE` died with `DOS/16M error: [8] cannot open file ''`. Probe `p_child.asm` |
| The environment block and PSP+2 (top of memory) do not move | **IMPL** | `host_dos.c:370-378`: moving them "BROKE EVERY DOS EXTENDER: Doom, Zar and Wolf3d took a PM #GP at startup ... user-confirmed, and confirmed fixed by rolling that change out". Kept in place: `src/dos/dos_mcb.h:97-100`, `src/dos/dos_layout.h:58-59` |
| Extender settings in the environment (`DOS4GVM=@ZAR.VMC`, ZAR's `RUNZAR.BAT`) | **IMPL** | Through the `dosenv.txt` knob into the fixed block (`src/dos/dos_env.h:230-260`); `tests/unit/mcb_test.c:470-508`. WARNING: this works only if the user puts the variable in `dosenv.txt`. Launching the `.EXE` directly does not set it |

---

## 9. Open questions -- not inventoried

These came up while reading the code. The repo does not answer them, so they are not rows.

1. **Does DOS/4GW or DOS/16M ever jump to the `0306h` raw-switch entries?** If one does,
   what happens when BOP `58h`/`59h` arrives with no arm to service it, and in V86 is BOP
   `58h` taken as NTVDM's third-party BOP (`host_exec.c:2211`)? This also bears on the
   IMPL that dpmi.md §5 and bop.md give `0306h`.
2. **Which INT 15h function does DOS/4GW ask when it is given a command-line argument?**
   `host_bios.c:1068-1073` and `host_startup.c:4382-4386` both record that it quits with
   an argument, and neither names the function. Is that still true after #206 and #244?
3. **Is PM `STI`/`CLI` now handled?** `ntvdm.h:160-163` says no; the fault-reflect path
   says the reflect works (§6). One of the two comments is out of date.
4. **Which detection calls do the extenders make before `1687h`?** VCPI `DE00h`, XMS
   `4300h`, INT 15h `88h`/`E801h`? Nothing in the repo records the order. It matters for
   the VCPI scope decision in xms-ems.md §8.
5. **What answer does DOS/4GW expect for `INT 21h AX=FF00h`** when it reaches the bottom
   of its chain (§4)? We answer CF=1.
6. **Does DOS/4GW's virtual-memory manager (`DOS4GVM`) need anything more from the host**
   than DPMI `0501h`/`0600h` and file I/O (for example the paging calls `0701h`-`0703h`
   or `0500h`'s free-page counts, which dpmi.md §7 marks PART)? ZAR is the shelf guest
   that sets it.
7. **Should the slave-PIC vectors `70h`-`77h` join the host-owned `25h`/`35h` range
   (§4)?** Nothing on the shelf is recorded hooking them through `25h` from a 32-bit
   client.
8. **Should vectors other than 21h be routed to the extender's PM handlers (§5)?** The
   blocker that comment names was removed by #247.
9. **Stale comments found while marking:** `host_state.c:575-577` ("We still service
   patched INT 21h/31h ourselves -- routing to a client-installed PM handler is a deeper
   item") is contradicted by `host_dpmi_int.c:8426-8434`; `host_dpmi_int.c:8363-8364`
   (§5); `pm32sw.asm`'s header (above).
