/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Port I/O: the trap dispatcher, the fast paths, and the VDD plug-in surface
 *   (third-party VDDs and the ISV I/O hooks).
 *
 * Its own translation unit (#335): declared in host_io.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "host_state.h"
#include "log.h"
#include "host_io.h"
#include "host_timing.h"
#include "host_bios.h"
#include "host_dpmi.h"
#include "host_wow.h"

/* Async-preemption experiment knob, also on the share so it can be changed between
 * runs without a rebuild. One digit: bits 0-1 = the FIXED_NTVDMSTATE pending bits to
 * set before NtVdmControl(VdmQueueInterrupt) (1 = VDM_INT_HARDWARE, 2 = VDM_INT_TIMER,
 * 3 = both), bit 2 = raise a periodic device IRQ 5 for the qirq probe. Absent = the
 * pre-session-11 behaviour (latch the pending bit only, never queue).
 */
/* GH #11: one DLL path per line, '#' comments. See docs/sdk/vdd-sdk.md. */
#define VDDLIST_PATH    CFG_("vdd.txt")

/* -- GH #8 (s91): THE HOST'S NetBIOS, for vdd_net.c. Win32's Netbios() takes an NCB
 * that is the DOS one with a flat buffer pointer -- the same commands, the same
 * return codes -- so this is a field copy. netapi32.dll is loaded on first use (a
 * host that never sees INT 5Ch never loads it).
 *
 * [INFO]: LANA NUMBERS: a DOS program says lana 0 and means "the network". NT's lanas are
 * whatever NCBENUM lists (on the rig: not necessarily 0), so DOS lana N is the Nth
 * enumerated one; past the end is NRC_BRIDGE (23h, invalid adapter).
 *
 * [INFO]: A Win32 lana must be RESET before use and a DOS program often never does (its
 * NetBIOS was initialised at boot), so the first command on a lana resets it with
 * the defaults; the program's own RESET resets it again, its way.
 */
typedef UCHAR (APIENTRY *PNETBIOS_ROUTINE)(PNCB);
static PNETBIOS_ROUTINE g_Netbios;
static LANA_ENUM  g_NetLanas;
static BYTE       g_NetReady[MAX_LANA + 1];
BYTE NetSubmit(PVOID context, NETBIOS_REQUEST *request)
{
    NCB enumBlock;
    UCHAR lana;
    (VOID)context;
    if (!g_Netbios)
    {
        static INT tried;
        HMODULE module;
        if (tried) return NRC_BRIDGE;
        tried = 1;
        module = LoadLibraryA(HOST_MODULE_NETAPI32);
        g_Netbios = module ? (PNETBIOS_ROUTINE)(VOID *)GetProcAddress(module, HOST_EXPORT_NETBIOS) : NULL;
        if (!g_Netbios) return NRC_BRIDGE;
        ZeroMemory(&enumBlock, sizeof enumBlock);
        enumBlock.ncb_command = NCBENUM;
        enumBlock.ncb_buffer  = (PUCHAR)&g_NetLanas;
        enumBlock.ncb_length  = sizeof g_NetLanas;
        if (g_Netbios(&enumBlock) != NRC_GOODRET) g_NetLanas.length = 0;
    }
    if (request->Adapter >= g_NetLanas.length)
    {
        request->ReturnCode = NRC_BRIDGE;
        return NRC_BRIDGE;
    }
    lana = g_NetLanas.lana[request->Adapter];
    if (request->Command != NCBRESET && !g_NetReady[lana])
    {
        ZeroMemory(&enumBlock, sizeof enumBlock);
        enumBlock.ncb_command = NCBRESET;
        enumBlock.ncb_lana_num = lana;
        if (g_Netbios(&enumBlock) == NRC_GOODRET) g_NetReady[lana] = 1;
    }
    ZeroMemory(&enumBlock, sizeof enumBlock);
    enumBlock.ncb_command  = request->Command;
    enumBlock.ncb_lsn      = request->LocalSession;
    enumBlock.ncb_num      = request->NameNumber;
    enumBlock.ncb_buffer   = request->Buffer;
    enumBlock.ncb_length   = request->Length;
    memcpy(enumBlock.ncb_callname, request->CallName, NCBNAMSZ);
    memcpy(enumBlock.ncb_name, request->Name, NCBNAMSZ);
    enumBlock.ncb_rto      = request->ReceiveTimeout;
    enumBlock.ncb_sto      = request->SendTimeout;
    enumBlock.ncb_lana_num = lana;
    if (request->Command == NCBRESET)
    {
        /* DOS: lsn = sessions, num = names (0 = default). Win32 reads them from
         * callname[0..1] -- and [2] nonzero asks for the first name number too.
         */
        enumBlock.ncb_callname[0] = request->LocalSession; enumBlock.ncb_callname[1] = request->NameNumber; enumBlock.ncb_callname[2] = 0;
        enumBlock.ncb_lsn = 0; enumBlock.ncb_num = 0;
    }
    request->ReturnCode = g_Netbios(&enumBlock);
    if (request->Command == NCBRESET && request->ReturnCode == NRC_GOODRET) g_NetReady[lana] = 1;
    request->LocalSession = enumBlock.ncb_lsn; request->NameNumber = enumBlock.ncb_num; request->Length = enumBlock.ncb_length;
    memcpy(request->CallName, enumBlock.ncb_callname, NCBNAMSZ);
    return request->ReturnCode;
}

static DWORD          g_PitReloadLog = 0;
WORD g_IoLastPort = 0;      /* port the last serviced access touched */
static DWORD g_IoSiteLogged = 0;
/* THIRD-PARTY VDDs: LOAD THEM. (GH #11, ADR-0008) (Importance = 4):
 * ADR-0008 chose a "clean internal plugin ABI that all our own devices use"
 * and a third-party hook point on top of it. Every device in src/vdd has been
 * using that ABI since M3; what was missing was the loader, so the model was
 * pluggable in shape and closed in fact.
 *
 * [INFO]: THE DRIVER IMPORTS NOTHING FROM US. Microsoft's ABI has a VDD import
 * VDDInstallIOHook and friends from `NTVDM.EXE` BY NAME, which binds a driver
 * to the FILENAME of its host -- and ours is ntvdmhost.exe, so an MS-shaped
 * import would not resolve at all. We hand over a table of function pointers
 * instead: versioned, appendable, and testable off-VM with a stub table.
 * See sdk/include/ntvdmex-vdd.h.
 *
 * [CAUTION]: A REFUSED CLAIM AND A FAILED LOAD ARE BOTH LOUD. A device that is not on
 * the bus reads as an empty ISA slot (0xFF) from the guest, which is
 * indistinguishable from hardware that is simply absent -- that is exactly how
 * a full port table cost this project an investigation once. Every step of
 * this says what happened.
 *
 * [CAUTION]: THE LIST IS A FILE, NOT THE REGISTRY, AND THAT IS THE PROJECT'S OWN RULE:
 * the share's text knobs override the registry ON PURPOSE, because a test rig
 * needs to change one without an installer. The registry path
 * (HKLM\...\VirtualDeviceDrivers, MS's own) belongs with the vddsvc veneer,
 * which ADR-0008 defers.
 */
static VOID VddLogLine(PCSTR message)
{
    CHAR lineBuffer[512], *cursor = lineBuffer;
    UINT index;
    cursor = LogPut(cursor, "  VDD: ");
    for (index = 0; message && message[index] && index < 400; ++index) *cursor++ = message[index];
    cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
}

static const ntvdmex_vdd_api g_VddApi = {
    (UINT32)sizeof(ntvdmex_vdd_api), NTVDMEX_VDD_ABI_VERSION,
    (INT  (*)(ntvdmex_vdd_bus *, WORD, WORD, ntvdmex_in_fn, ntvdmex_out_fn, VOID *))VddClaimPorts,
    (INT  (*)(ntvdmex_vdd_bus *, UINT32, UINT32, ntvdmex_rd_fn, ntvdmex_wr_fn, VOID *))VddClaimMemory,
    (INT  (*)(ntvdmex_vdd_bus *, BYTE, ntvdmex_int_fn, VOID *))VddClaimInterrupt,
    (INT  (*)(ntvdmex_vdd_bus *, ntvdmex_frame_fn, VOID *))VddOnFrame,
    (VOID (*)(ntvdmex_vdd_bus *, BYTE))VddRaiseIrq,
    (PVOID (*)(ntvdmex_vdd_bus *, WORD, WORD))VddMapFlat,
    (PVOID (*)(ntvdmex_vdd_bus *, UINT32))VddMapLinear,
    VddLogLine
};

VOID VddLoadThirdParty(VOID)
{
    HANDLE handle;
    CHAR buffer[4096];
    DWORD got = 0, index = 0, entryCount = 0, loaded = 0;
    if (g_Safe.VddPlugins) return;                 /* s90 #132: SAFE MODE */
    handle = CreateFileA(VDDLIST_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE) return;          /* no list is not an error */
    ReadFile(handle, buffer, sizeof buffer - 1, &got, NULL);
    CloseHandle(handle);
    buffer[got] = 0;
    while (index < got)
    {
        CHAR path[MAX_PATH]; DWORD length = 0;
        while (index < got && (buffer[index] == '\r' || buffer[index] == '\n')) ++index;
        while (index < got && buffer[index] != '\r' && buffer[index] != '\n' && length < MAX_PATH - 1)
            path[length++] = buffer[index++];
        while (length && (path[length-1] == ' ' || path[length-1] == '\t')) --length;
        path[length] = 0;
        if (!length || path[0] == '#' || path[0] == ';') continue;
        ++entryCount;
        {   HMODULE module = LoadLibraryA(path);
            CHAR lineBuffer[MAX_PATH + 200], *cursor = lineBuffer;
            NtvdmexVddInitFn initialize;
            INT status;
            if (!module)
            {
                cursor = LogPut(cursor, "  VDD: LoadLibrary FAILED err=0x"); cursor = LogHex(cursor, GetLastError());
                cursor = LogPut(cursor, " for ["); cursor = LogPut(cursor, path); cursor = LogPut(cursor, "]\r\n");
                LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
                continue;
            }
            initialize = (NtvdmexVddInitFn)(ULONG_PTR)GetProcAddress(module, NTVDMEX_VDD_INIT_NAME);
            if (!initialize)
            {
                cursor = LogPut(cursor, "  VDD: ["); cursor = LogPut(cursor, path);
                cursor = LogPut(cursor, "] loaded but exports no "); cursor = LogPut(cursor, NTVDMEX_VDD_INIT_NAME);
                cursor = LogPut(cursor, " -- not a VDD; unloaded\r\n");
                LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
                FreeLibrary(module);
                continue;
            }
            status = initialize(&g_VddApi, (ntvdmex_vdd_bus *)&g_Bus);
            cursor = lineBuffer;
            cursor = LogPut(cursor, "  VDD: ["); cursor = LogPut(cursor, path);
            if (status == 0)
            {
                cursor = LogPut(cursor, "] initialised\r\n");
                ++loaded;
            }
            else
            {
                /* Unload a driver that refused. A half-armed device the guest
                 * can still reach is worse than no device: it answers.
                 */
                cursor = LogPut(cursor, "] NtvdmexVddInit returned 0x"); cursor = LogHex(cursor, (DWORD)status);
                cursor = LogPut(cursor, " -- REFUSED, unloading\r\n");
                FreeLibrary(module);
            }
            LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
        }
    }
    {   CHAR lineBuffer[160], *cursor = lineBuffer;
        cursor = LogPut(cursor, "  VDD: third-party list ["); cursor = LogPut(cursor, VDDLIST_PATH);
        cursor = LogPut(cursor, "] -- 0x"); cursor = LogHex(cursor, entryCount); cursor = LogPut(cursor, " entr(ies), 0x");
        cursor = LogHex(cursor, loaded); cursor = LogPut(cursor, " initialised\r\n");
        LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor); }
}

/* Count-register reads split by whether an ASYNC injection was in flight. Note the
 * pair does NOT have to sum to the device's own rd_count[1]: this sees only reads
 * dispatched through HostIoDo, and a gap between the two is itself informative.
 */
UINT32 g_DmaPollInAsync, g_DmaPollMainline;
/* Record the first few DISTINCT ports the guest touches that no VDD claims. A game
 * hunting for hardware probes a fixed set of addresses, so this names the device it
 * wants -- e.g. 0x220-0x22F is a Sound Blaster looking for its DSP. Distinct-only
 * and budgeted, so it cannot flood a run.
 */
/* WHERE THE TIME GOES. 4.5M trapped port accesses in the first 6 s of a Skyroads run is
 * ~750k/s, and at ~1.5 us of round trip each that is the entire CPU -- which is exactly what
 * the game looks like on screen: everything correct, everything far too slow. Counting by
 * PORT says which device is being hammered; capturing the guest CS:IP and the bytes there
 * says which INSTRUCTION IDIOM it is, and that is what a fast path has to match. (The
 * existing burst only collapses `IN/OUT` + `LOOP rel8`, and io_burst was 4828 of 4.5M, so
 * Skyroads' delay loop is plainly a different shape.)
 */
VOID IoHotNote(WORD port, DWORD cs, DWORD ip)
{
    INT index;
    for (index = 0; index < g_IoHotCount; ++index)
        if (g_IoHot[index].Port == port)
        {
            g_IoHot[index].Count++;
            goto sited;
        }
    if (g_IoHotCount < IO_HOT_MAX)
    {
        g_IoHot[g_IoHotCount].Port = port; g_IoHot[g_IoHotCount].Count = 1; g_IoHotCount++;
    }
sited:
    if (g_IoSiteLogged < 6 && cs)
    {
        static DWORD seen[6];
        DWORD slot, key = (cs << WORD_SHIFT) ^ ip;
        for (slot = 0; slot < g_IoSiteLogged; ++slot) if (seen[slot] == key) return;
        seen[g_IoSiteLogged++] = key;
        { CHAR buffer[192], *cursor = buffer;
          const volatile BYTE *code = (const volatile BYTE *)((cs << PARAGRAPH_SHIFT) + ((ip - 6) & WORD_MASK));
          BYTE temporary[16]; UINT byteIndex;
          for (byteIndex = 0; byteIndex < 16; ++byteIndex) temporary[byteIndex] = code[byteIndex];
          cursor = LogPut(cursor, "IO-SITE port=0x"); cursor = LogHex(cursor, port);
          cursor = LogPut(cursor, " cs:ip=0x");       cursor = LogHex(cursor, cs);
          cursor = LogPut(cursor, ":0x");             cursor = LogHex(cursor, ip);
          cursor = LogPut(cursor, " bytes[ip-6..]: "); cursor = LogDump(cursor, temporary, 16);
          cursor = LogPut(cursor, "\r\n");
          LogAppend(LOG_PATH, buffer, cursor); SerialOut(buffer, cursor); }
    }
}

static VOID IoUnclaimedNote(WORD port, INT isIn)
{
    INT index;
    (VOID)isIn;
    for (index = 0; index < g_UnclaimedCount; ++index) if (g_Unclaimed[index] == port) return;
    if (g_UnclaimedCount >= IO_UNCLAIMED_MAX) return;
    g_Unclaimed[g_UnclaimedCount++] = port;
}

static DWORD g_SoundIoLogged = 0;    /* bounded SNDIO trace; see the note below */
static VOID HostIoDo(volatile BYTE *tib, VDD_BUS *bus, WORD port,
                       INT isIn, INT width)
{
    UINT32 value, eax = VDM_REG(tib, VTIB_EAX);
    g_IoLastPort = port;              /* for the hot-port histogram */
    /* - THE LAST FORK IN THE ECHO. 91% of DMX's 8237 count reads happen outside any
     * COOPERATIVE injection -- but "outside cooperative" is two very different
     * places: inside an ASYNC-delivered ISR (the async path does something the
     * cooperative one does not, and making them equivalent is the fix), or in
     * Doom's MAINLINE code (DMX polls at its own rate, the tick path is irrelevant,
     * and the fix is somewhere else entirely). The async ISR cannot be bracketed
     * the way the cooperative one was -- the guest runs it on its own thread -- but
     * g_AsyncPmActive is set for exactly its duration, from the injection to the
     * catcher. Read it here, where the port access is dispatched.
     */
    if (port == DMA_PORT_CHANNEL1_COUNT) { if (g_AsyncPmActive) ++g_DmaPollInAsync;
                        else                   ++g_DmaPollMainline; }
    /* Sync the counter before the guest looks at it, so a poll always reads real time. */
    if (port >= PIT_PORT_COUNTER0 && port <= PIT_PORT_CONTROL) HostPitSync();
    /* Report the rate the guest programs. Skyroads divides its own fast timer down to the
     * BIOS 18.2 Hz (519 injected IRQ0 vs 32 BIOS ticks last run => ~16:1), so the reload it
     * writes is the tempo the music actually wants.
     */
    if (port == PIT_PORT_COUNTER0 && !isIn && g_PitReloadLog < 8)
    {
        static WORD previous = 0;
        if (g_Pit.Reload != previous)
        {
            CHAR buffer[128], *cursor = buffer;
            previous = g_Pit.Reload; g_PitReloadLog++;
            cursor = LogPut(cursor, "PIT-RELOAD 0x"); cursor = LogHex(cursor, (DWORD)g_Pit.Reload);
            cursor = LogPut(cursor, " (hz=0x");
            cursor = LogHex(cursor, g_Pit.Reload ? (PIT_INPUT_HZ / g_Pit.Reload) : 18u);
            cursor = LogPut(cursor, ")\r\n");
            LogAppend(LOG_PATH, buffer, cursor); SerialOut(buffer, cursor);
        }
    }
    /* Reached from BOTH port paths (this reflected one and the interpreter's
     * V86HostOut); Lemmings' calibration latches from whichever the guest is in.
     */
    if (port == PIT_PORT_CONTROL && !isIn) PitLatchNote((BYTE)eax);
    if (isIn)
    {
        /* An unclaimed ISA port floats high: real hardware reads 0xFF, not 0x00.
         * This matters for device detection -- a probe that reads 0x00 from an
         * absent card can conclude it IS present and then wait forever for a
         * response that will never come.
         */
        value = 0;
        if (!VddBusIo(bus, port, (BYTE)width, VDD_IO_IN, &value))
        {
            value = VDD_UNCLAIMED_READ_U;
            IoUnclaimedNote(port, VDD_IO_IN);
        }
        if (width == 1)      VDM_REG(tib, VTIB_EAX) = (eax & ~BYTE_MASK_U) | (value & BYTE_MASK);
        else if (width == X86_WORD_SIZE) VDM_REG(tib, VTIB_EAX) = (eax & HIGH_WORD_MASK_U) | (value & WORD_MASK);
        else                 VDM_REG(tib, VTIB_EAX) = value;
    }
    else
    {
        value = (width == 1) ? (eax & BYTE_MASK) : (width == X86_WORD_SIZE) ? (eax & WORD_MASK) : eax;
        if (!VddBusIo(bus, port, (BYTE)width, VDD_IO_OUT, &value)) IoUnclaimedNote(port, VDD_IO_OUT);
        if (port == PIT_PORT_COUNTER0) HostPitResyncCheck();   /* see HostPitResyncCheck */
    }
    /* THE SOUND-CARD HANDSHAKE, IN FULL, FOR AS LONG AS IT LASTS:
     * "SB isn't responding at p=0x220, i=7, d=1" is Doom's verdict, not a
     * measurement: it says the probe failed, not which step of it did. The DSP reset
     * is a four-step conversation (write 1 to base+6, write 0, poll base+0xE for
     * bit 7, read base+0xA for 0xAA) and any one of them can be the miss.
     * Bounded to the first 300 accesses, which is far more than a probe needs and far
     * less than a playing game produces -- the point is the OPENING of the
     * conversation, and after that the per-block counters in STAGE2 take over.
     * `IoHotNote` cannot serve here: it is only called on the V86 arm of the exec
     * loop, so for a protected-mode client like Doom it records nothing at all --
     * which is why STAGE2's "hot ports:" line came back empty from a run that had
     * plainly done thousands of port accesses.
     */
    if (g_SoundIoLogged < 300
        && ((port >= SB_DEFAULT_BASE && port <= SB_DEFAULT_BASE + SB_PORT_LAST)      /* Sound Blaster */
         || (port >= OPL_PORT_FIRST && port <= OPL_PORT_FIRST + 1)      /* AdLib / OPL */
         || (port >= MPU_DEFAULT_BASE && port <= MPU_DEFAULT_BASE + 1)))    /* MPU-401 MIDI */
    {
        CHAR soundIoLine[128], *lineCursor = soundIoLine;
        ++g_SoundIoLogged;
        lineCursor = LogPut(lineCursor, "SNDIO "); lineCursor = LogPut(lineCursor, isIn ? "in  0x" : "out 0x");
        lineCursor = LogHex(lineCursor, port);
        lineCursor = LogPut(lineCursor, isIn ? " -> 0x" : " <- 0x");
        lineCursor = LogHex(lineCursor, value);
        lineCursor = LogPut(lineCursor, " w="); lineCursor = LogHexByte(lineCursor, (UINT)width);
        lineCursor = LogPut(lineCursor, " ms="); lineCursor = LogHex(lineCursor, GetTickCount());
        lineCursor = LogPut(lineCursor, "\r\n"); LogAppend(LOG_PATH, soundIoLine, lineCursor); SerialOut(soundIoLine, lineCursor);
    }
}

/* Burst fast path for the `<I/O insn>; LOOP <back to it>` idiom -- the same
 * trick as the mode-12h fill-loop interpreter below, applied to port I/O.
 *
 * Every guest IN/OUT is an IOPL-0 #GP reflected out to this user-mode monitor,
 * which measures ~40x the cost of the ISA access it stands in for (iobench.com).
 * DOS sound code leans on that idiom hard: an AdLib register write is `OUT` the
 * address, 6 dummy reads, `OUT` the data, then `mov cx,35 / in al,dx / loop $-1`
 * to satisfy the OPL's write-settling time -- 43 trapped accesses per register,
 * which is what left Skyroads grinding in its OPL init instead of reaching video.
 * Since the loop body is EXACTLY the one I/O instruction, we can run the rest of
 * the iterations here, one bus call each, and pay a single trap for all of them.
 *
 * `io_start` is the offset of the I/O instruction and `ip_next` the offset of the
 * insn after it; we only fire when a LOOP sits at `ip_next` and jumps back to
 * precisely `io_start`, so the body cannot contain anything we would skip.
 *
 * Accounting is exact and needs NO EIP fixup: the guest is left about to execute
 * the LOOP, so if it enters with CX=c the body still runs c-1 more times. We run
 * n of those here and subtract n from CX, leaving c-n; the guest then runs
 * (c-n)-1 itself, for n + (c-n-1) = c-1 total. Capping n (rather than draining
 * the loop) also keeps IRQ latency bounded: a `mov cx,0xFFFF` delay loop still
 * re-enters this monitor every IO_BURST_MAX accesses instead of once at the end.
 * Returns the number of extra accesses performed (0 = idiom not present).
 */
#define IO_BURST_MAX    4096
static DWORD HostIoLoopBurst(volatile BYTE *tib, VDD_BUS *bus,
                                volatile const BYTE *segment, DWORD ipNext,
                                DWORD ioStart, WORD port,
                                INT isIn, INT width, INT is32)
{
    /* A 16-bit segment wraps offsets (and LOOP counts) at 64K; a 32-bit flat one
     * does not, and there LOOP counts in ECX. One mask drives both.
     */
    DWORD cx, iterations, mask = is32 ? DWORD_MASK_U : WORD_MASK_U;
    INT displacement;
    if (segment[ipNext & mask] != X86_OP_LOOP) return 0;           /* LOOP rel8 only */
    displacement = (INT8)segment[(ipNext + 1) & mask];
    if (((ipNext + X86_JCC_SHORT_LENGTH + displacement) & mask) != (ioStart & mask)) return 0;
    cx = VDM_REG(tib, VTIB_ECX) & mask;
    iterations  = (cx - 1) & mask;                                /* body runs c-1 more */
    if (iterations > IO_BURST_MAX) iterations = IO_BURST_MAX;
    if (!iterations) return 0;
    VDM_REG(tib, VTIB_ECX) = is32 ? (cx - iterations)
                                  : ((VDM_REG(tib, VTIB_ECX) & HIGH_WORD_MASK_U) |
                                     ((cx - iterations) & WORD_MASK));
    {
        DWORD index;
        for (index = 0; index < iterations; ++index) HostIoDo(tib, bus, port, isIn, width);
    }
    g_IoExtra += iterations;
    return iterations;
}

INT HostTryIo(volatile BYTE *tib, VDD_BUS *bus)
{
    DWORD cs = VDM_REG16(tib, VTIB_CS);
    DWORD ip = VDM_REG16(tib, VTIB_EIP);
    volatile BYTE *code = (volatile BYTE *)((cs << PARAGRAPH_SHIFT) + ip);   /* absolute V86 */
    INT index = 0, operandSize = X86_WORD_SIZE, isIn, width, usedDx, length;
    BYTE opcode; WORD port;

    while (code[index] == X86_PREFIX_OPERAND_SIZE || code[index] == X86_PREFIX_ADDRESS_SIZE ||
           code[index] == X86_PREFIX_REPNE || code[index] == X86_PREFIX_REP)          /* prefixes */
    {
        if (code[index] == X86_PREFIX_OPERAND_SIZE) operandSize = X86_DWORD_SIZE;
        if (++index > X86_PREFIXES_MAX) return 0;
    }
    opcode = code[index];
    switch (opcode)
    {
    case X86_OP_IN_IMM_BYTE: isIn = 1; width = 1;      usedDx = 0; break;  /* IN  AL,ib */
    case X86_OP_IN_IMM: isIn = 1; width = operandSize; usedDx = 0; break;  /* IN  eAX,ib */
    case X86_OP_OUT_IMM_BYTE: isIn = 0; width = 1;      usedDx = 0; break;  /* OUT ib,AL */
    case X86_OP_OUT_IMM: isIn = 0; width = operandSize; usedDx = 0; break;  /* OUT ib,eAX */
    case X86_OP_IN_DX_BYTE: isIn = 1; width = 1;      usedDx = 1; break;  /* IN  AL,DX */
    case X86_OP_IN_DX: isIn = 1; width = operandSize; usedDx = 1; break;  /* IN  eAX,DX */
    case X86_OP_OUT_DX_BYTE: isIn = 0; width = 1;      usedDx = 1; break;  /* OUT DX,AL */
    case X86_OP_OUT_DX: isIn = 0; width = operandSize; usedDx = 1; break;  /* OUT DX,eAX */
    default:   return 0;                       /* not an I/O op -> real fault */
    }
    if (usedDx)
    {
        port = (WORD)VDM_REG(tib, VTIB_EDX);
        length = index + 1;
    }
    else
    {
        port = code[index + 1];
        length = index + 2;
    }

    HostIoDo(tib, bus, port, isIn, width);
    VDM_REG(tib, VTIB_EIP) = (ip + length) & WORD_MASK;          /* step past I/O */
    RetraceNote(tib, port, isIn, cs, (ip + length) & WORD_MASK);    /* #183 */
    /* ...and if a LOOP over this very instruction follows, drain it here. */
    HostIoLoopBurst(tib, bus, (volatile const BYTE *)(cs << PARAGRAPH_SHIFT),
                       (ip + length) & WORD_MASK, ip, port, isIn, width, X86_OPERAND_16);
    return 1;
}

/* Retro I/O servicer for the real-hardware event-3 reflect. On this XP box an IOPL-0
 * IN/OUT #GP is reflected as VTIB_EVENT=3 with CS:IP pointing at the instruction AFTER
 * the faulting IN/OUT (EIP already advanced past it) -- so HostTryIo, which decodes
 * AT CS:IP, sees the next op (e.g. a MOV in Skyroads' vblank poll) and declines. Here
 * we decode the IN/OUT that ENDS at CS:IP and service it WITHOUT advancing EIP: a DX-form
 * (1 byte: EC/ED/EE/EF at IP-1, optional 66 prefix at IP-2) or an imm-form (2 bytes:
 * E4-E7 at IP-2, port imm at IP-1). Returns 1 if serviced.
 */
INT HostTryIoRetro(volatile BYTE *tib, VDD_BUS *bus)
{
    DWORD cs = VDM_REG16(tib, VTIB_CS);
    DWORD ip = VDM_REG16(tib, VTIB_EIP);
    volatile BYTE *segment = (volatile BYTE *)(cs << PARAGRAPH_SHIFT);
    BYTE opcode; INT isIn, width, operandSize = X86_WORD_SIZE; WORD port; DWORD ioStart;
    if (ip < 1) return 0;
    opcode = segment[ip - 1];
    if (opcode == X86_OP_IN_DX_BYTE || opcode == X86_OP_IN_DX || opcode == X86_OP_OUT_DX_BYTE || opcode == X86_OP_OUT_DX)     /* DX-form (1 byte) */
    {
        ioStart = ip - 1;
        if (ip >= X86_IN_IMM_LENGTH && segment[ip - X86_IN_IMM_LENGTH] == X86_PREFIX_OPERAND_SIZE)
        {
            operandSize = X86_DWORD_SIZE;
            ioStart = ip - X86_IN_IMM_LENGTH;
        }
        isIn = (opcode == X86_OP_IN_DX_BYTE || opcode == X86_OP_IN_DX);
        width = (opcode == X86_OP_IN_DX_BYTE || opcode == X86_OP_OUT_DX_BYTE) ? 1 : operandSize;
        port  = (WORD)VDM_REG(tib, VTIB_EDX);
    }
    else if (ip >= X86_IN_IMM_LENGTH && ((opcode = segment[ip - X86_IN_IMM_LENGTH]) == X86_OP_IN_IMM_BYTE || opcode == X86_OP_IN_IMM ||
                            opcode == X86_OP_OUT_IMM_BYTE || opcode == X86_OP_OUT_IMM))            /* imm-form (2 byte) */
    {
        ioStart = ip - X86_IN_IMM_LENGTH;
        isIn = (opcode == X86_OP_IN_IMM_BYTE || opcode == X86_OP_IN_IMM);
        width = (opcode == X86_OP_IN_IMM_BYTE || opcode == X86_OP_OUT_IMM_BYTE) ? 1 : operandSize;
        port  = segment[ip - 1];                                      /* imm8 port */
    }
    else
    {
        return 0;                                                 /* no I/O ends here */
    }
    HostIoDo(tib, bus, port, isIn, width);
    RetraceNote(tib, port, isIn, VDM_REG16(tib, VTIB_CS), ip);   /* #183 */
    /* EIP is already past the I/O, so the guest is sitting on whatever follows --
     * if that is a LOOP back to this same I/O (the OPL write-delay idiom), drain
     * the iterations here rather than paying one #GP reflect per read.
     */
    HostIoLoopBurst(tib, bus, segment, ip, ioStart, port, isIn, width, X86_OPERAND_16);
    return 1;                              /* EIP already past the I/O -- do NOT advance */
}

/* Service a REP INS/OUTS reflected as event 1 (string I/O) -- how the VGA palette
 * is loaded (`rep outsb` to 0x3C9) and how several sound drivers push blocks.
 * The kernel hands us a decoded descriptor in the words after VTIB_EVENT, but we
 * deliberately work from the guest's own SI/DI/CX/DF and the instruction bytes at
 * CS:IP instead: those fields are already established, whereas the descriptor's
 * layout is inferred from a single observation. (The two agreed on the run that
 * found this: port 0x3C9, count 0x40, buffer 0100:0355.)
 *
 * Unlike the other servicers, CS:IP points AT the instruction here. We transfer at
 * most IO_BURST_MAX units per reflect and only step past the instruction once CX
 * drains -- leaving EIP on the REP otherwise, exactly as a real CPU resumes an
 * interrupted string op, which keeps a 64K transfer from monopolising the monitor.
 */
INT HostTryIoString(volatile BYTE *tib, VDD_BUS *bus)
{
    DWORD cs = VDM_REG16(tib, VTIB_CS);
    DWORD ip = VDM_REG16(tib, VTIB_EIP);
    volatile BYTE *segment = (volatile BYTE *)(cs << PARAGRAPH_SHIFT);
    INT index = 0, operandSize = X86_WORD_SIZE, isIn, width, report = 0;
    DWORD sover = 0, count, burst, step, element;
    WORD port;
    BYTE opcode;

    for (;;)                                     /* prefixes */
    {
        opcode = segment[(ip + index) & WORD_MASK];
        if      (opcode == X86_PREFIX_OPERAND_SIZE) operandSize = X86_DWORD_SIZE;
        else if (opcode == X86_PREFIX_REPNE || opcode == X86_PREFIX_REP) report = 1;
        else if (opcode == X86_PREFIX_ES) sover = VTIB_ES;    /* segment overrides on the source */
        else if (opcode == X86_PREFIX_CS) sover = VTIB_CS;
        else if (opcode == X86_PREFIX_SS) sover = VTIB_SS;
        else if (opcode == X86_PREFIX_DS) sover = VTIB_DS;
        else if (opcode != X86_PREFIX_ADDRESS_SIZE) break;
        if (++index > X86_PREFIXES_MAX) return 0;
    }
    switch (opcode)
    {
    case X86_OP_INSB: isIn = 1; width = 1;      break;              /* INSB */
    case X86_OP_INS: isIn = 1; width = operandSize; break;              /* INSW / INSD */
    case X86_OP_OUTSB: isIn = 0; width = 1;      break;              /* OUTSB */
    case X86_OP_OUTS: isIn = 0; width = operandSize; break;              /* OUTSW / OUTSD */
    default:   return 0;                                      /* not a string I/O */
    }
    port  = (WORD)VDM_REG(tib, VTIB_EDX);
    count = report ? VDM_REG16(tib, VTIB_ECX) : 1;
    if (!count)                                               /* REP with CX=0 */
    {
        VDM_REG(tib, VTIB_EIP) = (ip + index + 1) & WORD_MASK;
        return 1;
    }
    step = (VDM_REG(tib, VTIB_EFLAGS) & EFLAGS_DF) ? (DWORD)-width : (DWORD)width;  /* DF */
    burst = (count > IO_BURST_MAX) ? IO_BURST_MAX : count;

    for (element = 0; element < burst; ++element)
    {
        UINT32 value = 0;
        if (isIn)                                            /* port -> ES:DI */
        {
            DWORD di = VDM_REG16(tib, VTIB_EDI);
            DWORD linear = (VDM_REG16(tib, VTIB_ES) << PARAGRAPH_SHIFT) + di;
            VddBusIo(bus, port, (BYTE)width, VDD_IO_IN, &value);
            PokeWidth(linear, value, width);
            VDM_SET16(tib, VTIB_EDI, (WORD)(di + step));
        }
        else                                                /* DS:SI -> port */
        {
            DWORD si = VDM_REG16(tib, VTIB_ESI);
            DWORD segmentRegisterOffset = sover ? sover : VTIB_DS;
            DWORD linear = (VDM_REG16(tib, segmentRegisterOffset) << PARAGRAPH_SHIFT) + si;
            value = PeekWidth(linear, width);
            VddBusIo(bus, port, (BYTE)width, VDD_IO_OUT, &value);
            VDM_SET16(tib, VTIB_ESI, (WORD)(si + step));
        }
    }
    if (report)
    {
        VDM_SET16(tib, VTIB_ECX, (WORD)(count - burst));
        if (count - burst) return 1;                 /* more to go: resume ON the REP */
    }
    VDM_REG(tib, VTIB_EIP) = (ip + index + 1) & WORD_MASK;
    return 1;
}

DWORD g_DmaPollEip[DMAPOLL_MAX], g_DmaPollHits[DMAPOLL_MAX];
UINT g_DmaPollCount = 0, g_DmaPollOverflow = 0;
DWORD g_PollStack[POLLSTK_MAX], g_PollStackHits[POLLSTK_MAX];
DWORD g_PollGap[10], g_PollGapMaximumMicroseconds = 0;
UINT g_PollStackCount = 0, g_PollStackOverflow = 0;
INT HostTryIoPm(volatile BYTE *tib, VDD_BUS *bus)
{
    DWORD csValue = VDM_REG16(tib, VTIB_CS);
    DWORD eip = VDM_REG(tib, VTIB_EIP);
    INT is32 = DpmiSelectorIs32((WORD)csValue);
    DWORD eipOffset = is32 ? eip : (eip & WORD_MASK);
    volatile BYTE *code = (volatile BYTE *)(ULONG_PTR)(DpmiSelectorBase((WORD)csValue) + eipOffset);
    INT index = 0, operandSize = is32 ? X86_DWORD_SIZE : X86_WORD_SIZE, isIn, width, usedDx, length;
    BYTE opcode; WORD port;

    while (code[index] == X86_PREFIX_OPERAND_SIZE || code[index] == X86_PREFIX_ADDRESS_SIZE ||
           code[index] == X86_PREFIX_REPNE || code[index] == X86_PREFIX_REP)          /* prefixes */
    {
        if (code[index] == X86_PREFIX_OPERAND_SIZE) operandSize = is32 ? X86_WORD_SIZE : X86_DWORD_SIZE;     /* 0x66 flips the segment default */
        if (++index > X86_PREFIXES_MAX) return 0;
    }
    opcode = code[index];
    switch (opcode)
    {
    case X86_OP_IN_IMM_BYTE: isIn = 1; width = 1;      usedDx = 0; break;  /* IN  AL,ib */
    case X86_OP_IN_IMM: isIn = 1; width = operandSize; usedDx = 0; break;  /* IN  eAX,ib */
    case X86_OP_OUT_IMM_BYTE: isIn = 0; width = 1;      usedDx = 0; break;  /* OUT ib,AL */
    case X86_OP_OUT_IMM: isIn = 0; width = operandSize; usedDx = 0; break;  /* OUT ib,eAX */
    case X86_OP_IN_DX_BYTE: isIn = 1; width = 1;      usedDx = 1; break;  /* IN  AL,DX */
    case X86_OP_IN_DX: isIn = 1; width = operandSize; usedDx = 1; break;  /* IN  eAX,DX */
    case X86_OP_OUT_DX_BYTE: isIn = 0; width = 1;      usedDx = 1; break;  /* OUT DX,AL */
    case X86_OP_OUT_DX: isIn = 0; width = operandSize; usedDx = 1; break;  /* OUT DX,eAX */
    default:   return 0;                       /* not an I/O op -> real fault */
    }
    if (usedDx)
    {
        port = (WORD)VDM_REG(tib, VTIB_EDX);
        length = index + 1;
    }
    else
    {
        port = code[index + 1];
        length = index + 2;
    }

    /* WHERE IN THE GUEST IS THE DMA POLL? A LOCATOR, NOT A HYPOTHESIS:
     * DMX refills from its timer ISR and steers by the 8237's channel-1 count, and
     * ~23 of the 135 ticks a second we deliver enter its handler and return without
     * ever reading that count. The dispatcher is not what drops them -- it is
     * believed to call the registered handler every tick -- so the decision is
     * inside DMX's own routine, whose address is runtime data and cannot
     * be read out of the image.
     * The host, however, sees the instruction. Record the guest EIP of the reads of
     * port 3, and the map `guest = file + 0x03AEDFEC` (verified on DMX's IRQ0 stub
     * and Doom's keyboard ISR) turns it straight into a file offset to disassemble.
     * The overflow is counted, so a too-small table cannot pass as a complete answer.
     */
    if (isIn && port == DMA_PORT_CHANNEL1_COUNT)
    {
        DWORD site = DpmiSelectorBase((WORD)csValue) + eipOffset;
        UINT pollIndex;
        for (pollIndex = 0; pollIndex < g_DmaPollCount; ++pollIndex) if (g_DmaPollEip[pollIndex] == site) break;
        if (pollIndex < g_DmaPollCount) g_DmaPollHits[pollIndex]++;
        else if (g_DmaPollCount < DMAPOLL_MAX)
        {
            g_DmaPollEip[g_DmaPollCount] = site; g_DmaPollHits[g_DmaPollCount] = 1;
            ++g_DmaPollCount;
        }
        else ++g_DmaPollOverflow;

        /* -- WHY IS THE MIXER RUN ONLY 56 TIMES A SECOND? TWO CAUSES, ONE SHAPE EACH.
         * DMX's scheduler (DOOM.EXE file 0x57224) runs a task when the tick clock
         * reaches its deadline, ABANDONS THE WHOLE PASS if the task is still busy,
         * and recomputes the deadline from NOW so a missed run is never made up.
         * (a) the mixer overruns the 7.4 ms tick period, so the next tick finds it
         *   busy -> the gaps between polls are LONG and irregular.
         * (b) we deliver ticks in bursts; each burst tick advances DMX's clock but
         *   only one task run happens per ISR entry -> the gaps are SHORT and
         *   regular, and the loss is in the bunching, not the duration.
         * A rate cannot tell those apart -- both give 56/s -- so bucket the actual
         * interval. QPC because at these scales GetTickCount's 10-16 ms granularity
         * is the same size as the effect.
         */
        { static LARGE_INTEGER frequency, prev;
          LARGE_INTEGER now;
          if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
          if (frequency.QuadPart && QueryPerformanceCounter(&now))
          {
              if (prev.QuadPart)
              {
                  LONGLONG deltaMicroseconds = ((now.QuadPart - prev.QuadPart) * MICROSECONDS_PER_SECOND) / frequency.QuadPart;
                  UINT bucket = 0;
                  while (bucket < 9 && deltaMicroseconds >= (LONGLONG)MICROSECONDS_PER_MILLISECOND << bucket) ++bucket;   /* 1,2,4..256ms+ */
                  g_PollGap[bucket]++;
                  if (deltaMicroseconds > (LONGLONG)g_PollGapMaximumMicroseconds) g_PollGapMaximumMicroseconds = (DWORD)deltaMicroseconds;
              }
              prev = now;
          } }

        /* The return chain, straight off the guest's stack. */
        { DWORD stackSegmentBase = DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_SS));
          DWORD esp = VDM_REG(tib, VTIB_ESP);
          const volatile DWORD *stack = (const volatile DWORD *)(ULONG_PTR)(stackSegmentBase + esp);
          UINT slot;
          for (slot = 0; slot < 40; ++slot)
          {
              DWORD word = stack[slot], distance = (word > site) ? (word - site) : (site - word);
              UINT entry;
              if (distance > 0x60000u) continue;             /* not a code address */
              for (entry = 0; entry < g_PollStackCount; ++entry) if (g_PollStack[entry] == word) break;
              if (entry < g_PollStackCount) g_PollStackHits[entry]++;
              else if (g_PollStackCount < POLLSTK_MAX)
              {
                  g_PollStack[g_PollStackCount] = word; g_PollStackHits[g_PollStackCount] = 1;
                  ++g_PollStackCount;
              }
              else ++g_PollStackOverflow;
          } }
    }

    HostIoDo(tib, bus, port, isIn, width);
    /* step past the I/O insn. 16-bit client (D=0): advance the low word, keep high.
     * 32-bit client (D=1): advance the full EIP.
     */
    VDM_REG(tib, VTIB_EIP) = is32 ? (eip + length) : ((eip & HIGH_WORD_MASK_U) | ((eip + length) & WORD_MASK));
    /* Same LOOP-drain as V86 (a PM sound driver runs the identical OPL idiom).
     * With D/B=1 the LOOP counts in ECX, so the counter width follows the selector.
     */
    HostIoLoopBurst(tib, bus, (volatile const BYTE *)(ULONG_PTR)DpmiSelectorBase((WORD)csValue),
                       eipOffset + length, eipOffset, port, isIn, width, is32);
    return 1;
}

/* An I/O hook: the VDD's VDD_IO_HANDLERS, claimed on our bus range by range. STDCALL:
 * nt_vdd.h names no convention, and NT and the DDK compile with __stdcall as the
 * default -- measured: stock NTVDM died at the first IN when the test VDD's handlers
 * were cdecl (tests/probes/dos/isvtest). A hook taken down by VDDDeInstallIOHook stays claimed
 * (the bus has no release) and answers as an empty slot: FFh in, writes dropped.
 */
typedef VOID (WINAPI *PISV_IN_BYTE_ROUTINE)(WORD, BYTE *);   typedef VOID (WINAPI *PISV_IN_WORD_ROUTINE)(WORD, WORD *);
typedef VOID (WINAPI *PISV_OUT_BYTE_ROUTINE)(WORD, BYTE);    typedef VOID (WINAPI *PISV_OUT_WORD_ROUTINE)(WORD, WORD);
VOID IsvIoIn(PVOID self, WORD port, BYTE width, UINT32 *value)
{
    INT index = (INT)(ULONG_PTR)self;
    BYTE lowByte = VDD_UNCLAIMED_READ_BYTE, highByte = VDD_UNCLAIMED_READ_BYTE; WORD word = VDD_UNCLAIMED_READ_WORD;
    if (!g_IsvHooks[index].IsLive)
    {
        *value = width == 1 ? VDD_UNCLAIMED_READ_BYTE : width == X86_WORD_SIZE ? VDD_UNCLAIMED_READ_WORD : VDD_UNCLAIMED_READ_U;
        return;
    }
    if (width == 1)
    {
        if (g_IsvHooks[index].Handlers.InByte) ((PISV_IN_BYTE_ROUTINE)g_IsvHooks[index].Handlers.InByte)(port, &lowByte);
        *value = lowByte;
    }
    else if (width == X86_WORD_SIZE && g_IsvHooks[index].Handlers.InWord)
    {
        ((PISV_IN_WORD_ROUTINE)g_IsvHooks[index].Handlers.InWord)(port, &word); *value = word;
    }
    else                                          /* no word handler: two byte reads */
    {
        if (g_IsvHooks[index].Handlers.InByte) { ((PISV_IN_BYTE_ROUTINE)g_IsvHooks[index].Handlers.InByte)(port, &lowByte);
                                   ((PISV_IN_BYTE_ROUTINE)g_IsvHooks[index].Handlers.InByte)((WORD)(port + 1), &highByte); }
        *value = (UINT32)lowByte | ((UINT32)highByte << BYTE_SHIFT);
    }
}

VOID IsvIoOut(PVOID self, WORD port, BYTE width, UINT32 value)
{
    INT index = (INT)(ULONG_PTR)self;
    if (!g_IsvHooks[index].IsLive) return;
    if (width == 1)
    {
        if (g_IsvHooks[index].Handlers.OutByte) ((PISV_OUT_BYTE_ROUTINE)g_IsvHooks[index].Handlers.OutByte)(port, (BYTE)value);
    }
    else if (width == X86_WORD_SIZE && g_IsvHooks[index].Handlers.OutWord) ((PISV_OUT_WORD_ROUTINE)g_IsvHooks[index].Handlers.OutWord)(port, (WORD)value);
    else if (g_IsvHooks[index].Handlers.OutByte)
    {
        ((PISV_OUT_BYTE_ROUTINE)g_IsvHooks[index].Handlers.OutByte)(port, (BYTE)value);
        ((PISV_OUT_BYTE_ROUTINE)g_IsvHooks[index].Handlers.OutByte)((WORD)(port + 1), (BYTE)(value >> BYTE_SHIFT));
    }
}

enum
{
    ISV_STRING_DLL = 0, ISV_STRING_INIT = 1, ISV_STRING_DISPATCH = 2, ISV_STRINGS = 3
};   /* RegisterModule's three names: DS:SI, ES:DI, DS:BX */
/* The third-party BOP's three calls (see "THIRD-PARTY VDDs" above). CF goes in the
 * LIVE flags -- a BOP is not an INT, nothing was pushed. Handles are 1-based.
 */
#define ISV_MAX_MODS    8
static struct
{
    HMODULE Module;
    FARPROC Dispatch;
} g_IsvModules[ISV_MAX_MODS];
VOID IsvBop(volatile BYTE *tib, DWORD subfunction, PSTR *logCursor)
{
    PSTR cursor = *logCursor;
    DWORD ds = VDM_REG16(tib, VTIB_DS);
    INT isProtectedMode = g_DpmiPm;
    WORD error = 0, vddHandle = 0;
    INT index;
    #define ISV_STR(reg) ((PCSTR)ShimMapFlat((WORD)ds, VDM_REG16(tib, reg), isProtectedMode))
    if (subfunction == 0)                                       /* RegisterModule */
    {
        /* [CAUTION]: COPIED OUT FIRST. A V86 string lives below linear 64 KB, and GetProcAddress
         * takes any "name" pointer under 0x10000 for an ORDINAL -- the first rig run
         * answered ERROR_INVALID_ORDINAL for a routine stock found at once.
         */
        CHAR dllBuffer[MAX_PATH], iniBuffer[128], dispatchBuffer[128];
        PCSTR dllName = dllBuffer, iniName = iniBuffer, dispatchName = dispatchBuffer;
        HMODULE module = NULL; FARPROC initProcedure = NULL, dispatchProcedure = NULL;
        {   PCSTR source[ISV_STRINGS]; PSTR destination[ISV_STRINGS]; INT cap[ISV_STRINGS], index2, length;
            source[ISV_STRING_DLL] = ISV_STR(VTIB_ESI); source[ISV_STRING_INIT] = ISV_STR(VTIB_EDI); source[ISV_STRING_DISPATCH] = ISV_STR(VTIB_EBX);
            destination[ISV_STRING_DLL] = dllBuffer; destination[ISV_STRING_INIT] = iniBuffer; destination[ISV_STRING_DISPATCH] = dispatchBuffer;
            cap[ISV_STRING_DLL] = (INT)sizeof dllBuffer; cap[ISV_STRING_INIT] = (INT)sizeof iniBuffer; cap[ISV_STRING_DISPATCH] = (INT)sizeof dispatchBuffer;
            for (index2 = 0; index2 < ISV_STRINGS; ++index2)
            {
                for (length = 0; source[index2] && length < cap[index2] - 1 && source[index2][length]; ++length) destination[index2][length] = source[index2][length];
                destination[index2][length] = 0;
            }
        }
        WowShimsLoad();          /* NTVDM.EXE must be in the process before the VDD */
        for (index = 0; index < ISV_MAX_MODS && g_IsvModules[index].Module; ++index) ;
        if (index == ISV_MAX_MODS) error = NTVDM_ISV_ERROR_NO_MEMORY;
        else if (!dllName || !dllName[0] || !(module = LoadLibraryA(dllName))) error = NTVDM_ISV_ERROR_DLL_NOT_FOUND;
        else if (!dispatchName || !dispatchName[0] || !(dispatchProcedure = GetProcAddress(module, dispatchName))) error = NTVDM_ISV_ERROR_NO_DISPATCH;
        else if (iniName && iniName[0] && !(initProcedure = GetProcAddress(module, iniName))) error = NTVDM_ISV_ERROR_NO_INIT;
        cursor = LogPut(cursor, "  ISVVDD: RegisterModule ["); cursor = LogPut(cursor, dllName ? dllName : "?");
        cursor = LogPut(cursor, "] init ["); cursor = LogPut(cursor, iniName ? iniName : ""); cursor = LogPut(cursor, "] dispatch [");
        cursor = LogPut(cursor, dispatchName ? dispatchName : ""); cursor = LogPut(cursor, "]");
        if (error)
        {
            if (module) FreeLibrary(module);
            cursor = LogPut(cursor, " -> ERROR "); cursor = LogHex(cursor, error);
            cursor = LogPut(cursor, " gle=0x"); cursor = LogHex(cursor, GetLastError()); cursor = LogPut(cursor, "\r\n");
        }
        else
        {
            g_IsvModules[index].Module = module; g_IsvModules[index].Dispatch = dispatchProcedure;
            vddHandle = (WORD)(index + 1);
            cursor = LogPut(cursor, " -> handle "); cursor = LogHex(cursor, vddHandle); cursor = LogPut(cursor, "\r\n");
            *logCursor = cursor;
            if (initProcedure) ((VOID (*)(VOID))initProcedure)();          /* the init routine, in context */
            cursor = *logCursor;
        }
    }
    else if (subfunction == NTVDM_ISV_UNREGISTER_MODULE || subfunction == NTVDM_ISV_DISPATCH_CALL)                    /* UnRegisterModule / DispatchCall */
    {
        WORD ax = (WORD)VDM_REG16(tib, VTIB_EAX);
        if (!ax || ax > ISV_MAX_MODS || !g_IsvModules[ax - 1].Module)
        {
            error = 1;
            cursor = LogPut(cursor, "  ISVVDD: bad handle 0x"); cursor = LogHex(cursor, ax); cursor = LogPut(cursor, "\r\n");
        }
        else if (subfunction == NTVDM_ISV_DISPATCH_CALL)
        {
            ((VOID (*)(VOID))g_IsvModules[ax - 1].Dispatch)();
            *logCursor = cursor;
            return;                       /* the VDD owns the registers and CF now */
        }
        else
        {
            for (index = 0; index < ISV_MAX_HOOKS; ++index)
                if (g_IsvHooks[index].VddHandle == (HANDLE)g_IsvModules[ax - 1].Module) g_IsvHooks[index].IsLive = 0;
            FreeLibrary(g_IsvModules[ax - 1].Module);
            g_IsvModules[ax - 1].Module = NULL;
        }
    }
    else
    {
        error = 1;
    }
    #undef ISV_STR
    if (error)
    {
        VDM_SET16(tib, VTIB_EAX, error);
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
    }
    else
    {
        if (subfunction == 0) VDM_SET16(tib, VTIB_EAX, vddHandle);
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
    }
    *logCursor = cursor;
}
