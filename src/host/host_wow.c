/* host_wow.c -- Win16: the WOW glue -- module loading and anchors, the scheduler, WOW32 calls,
 *   the shims, 16-bit callbacks and owner-draw.
 *
 * Part of the host's single translation unit: #included by main.c after host_internal.h. */

static INT   g_WowFoldMute;
static DWORD g_WowFoldDropped;      /* dumps folded away, reported in WOWPERF */
static DWORD g_IcaRaised = 0, g_IcaDelivered = 0, g_IcaNoHandler = 0;
static DWORD g_ShimState[WOW_SHIMS], g_ShimError[WOW_SHIMS];  /* 0 not tried, 1 loaded+init, 2 no load, 3 init refused */
/* ── THE WOW32 BOP BLOCK'S ONLY WAY OUT. (session 56) ────────────────────────
     Identical to LogAppend + SerialOut, except that it drops the buffer when
     the current call is a folded repeat. Deliberately NOT a change to
     LogAppend itself, and deliberately used only by the sites inside the WOW32
     BOP handler: g_WowFoldMute stays set for as long as a run continues, so a
     global mute would also swallow anything else logged BETWEEN those BOPs --
     a fault, a PM interrupt, an LDT sync -- and hiding one of those to save disk
     would be trading the trace for the thing the trace exists to catch.
     `p` is reset either way, so a muted block cannot leak into the next one. */
static VOID WowLogFlush(PSTR base, PSTR *logCursor)
{
    PSTR end = *logCursor;
    if (g_WowFoldMute == WOWFOLD_MUTE_DROP) { ++g_WowFoldDropped; *logCursor = base; return; }
    if (g_WowFoldMute && end > base) {
        /* ── ⚠⚠ KEEP THE VERDICT LINE. DROPPING IT WOULD BREAK THE GATE. ──
             bmwow.sh's signature is a COUNT of `-> SERVICED` / `-> DECLINED` /
             `-> UNIMPLEMENTED` lines, and the baseline it compares against
             (85/113/57) is a count of the same. Folding whole blocks away would
             have moved those numbers with no change in behaviour whatsoever --
             a silent, self-inflicted regression signal, which is precisely the
             class of instrument fault this fold exists to fix. So drop the
             DUMP -- the register line, the two stack windows, the arguments --
             and keep the one line that says what happened. That is ~90 bytes of
             a ~1 KB block, and it leaves every count in the log exact. */
        PSTR lineStart = end - 1;
        while (lineStart > base && (lineStart[-1] != '\n')) --lineStart;
        /* `ls` is now the start of the final line. If the block is one line
           already there is nothing to save, and emitting it is correct. */
        ++g_WowFoldDropped;
        LogAppend(LOG_PATH, lineStart, end); SerialOut(lineStart, end);
        *logCursor = base;
        return;
    }
    LogAppend(LOG_PATH, base, end); SerialOut(base, end); *logCursor = base;
}
/* ── ★★★★ THE Win16 COMM API, ON THE REAL UART. (GH #9 + #128, session 56) ──
     wowuser.h answered the whole comm family with IE_BADID, and its note gave
     the reason plainly: "the equipment word claims none, on purpose". THAT WAS
     TRUE WHEN IT WAS WRITTEN AND I MADE IT FALSE THIS SESSION -- the equipment
     word now reports SER=2 and the BDA carries 0x03F8/0x02F8, with a real 8250
     behind them. A host that advertises two serial ports and then refuses to
     open either is the same two-layers-disagreeing fault the equipment word
     itself was fixed for, pointing the other way.
   ⇒ So the Win16 side is wired to the same vdd_comm the ports and INT 14h use.
     One device, three ways in, and none of them can now contradict the others.
   ⚠ THESE LIVE HERE, NOT IN wowuser.h, because that header must not see host
     internals -- it gets declarations only. Same rule as the rest of the WOW
     layer. */
#define WOWCOMM_MAX 4   /* #245: COM1-COM4, as the equipment word now says */
static INT  g_WowCommOpen[WOWCOMM_MAX];        /* 1 = this port is open to Win16 */
static WORD g_WowCommEvent[WOWCOMM_MAX];         /* the event word SetCommEventMask points at */

INT WowCommOpen(PCSTR device)
{
    INT index;
    /* "COM1".."COM4", case-insensitive, exactly as Windows parses it. Anything
       else -- including the LPTn a guest may legally pass -- is not ours. */
    if (!device) return WOWUSER_COMM_FAILED;                                        /* IE_BADID     */
    if ((device[0] != 'C' && device[0] != 'c') || (device[1] != 'O' && device[1] != 'o')
        || (device[2] != 'M' && device[2] != 'm')) return WOWUSER_COMM_FAILED;
    index = device[3] - '1';
    if (index < 0 || index >= WOWCOMM_MAX) return WOWUSER_COMM_FAILED;               /* no such port */
    if (!VddCommIsFitted(&g_Comm, index))  return WOWUSER_COMM_FAILED;
    if (g_WowCommOpen[index]) return WOWUSER_COMM_ALREADY_OPEN;                              /* IE_OPEN      */
    g_WowCommOpen[index] = 1; g_WowCommEvent[index] = 0;
    return index;                                                 /* the comm id  */
}
static INT WowCommValid(INT port)
{ return port >= 0 && port < WOWCOMM_MAX && g_WowCommOpen[port]; }
INT WowCommClose(INT port)
{ if (!WowCommValid(port)) return WOWUSER_COMM_FAILED; g_WowCommOpen[port] = 0; return 0; }
INT WowCommRead(INT port, BYTE *buffer, INT count)
{
    INT got = 0;
    if (!WowCommValid(port)) return WOWUSER_COMM_FAILED;
    /* Straight off the same receive ring the guest would see through RBR. */
    while (got < count && g_Comm.Ports[port].ReceiveLength) {
        UINT32 value = 0;
        VddBusIo(&g_Bus, (WORD)(g_Comm.Ports[port].BasePort + COMM_RBR), 1, VDD_IO_IN, &value);
        buffer[got++] = (BYTE)value;
    }
    return got;
}
INT WowCommWrite(INT port, const BYTE *buffer, INT count)
{
    INT index;
    if (!WowCommValid(port)) return WOWUSER_COMM_FAILED;
    for (index = 0; index < count; ++index) {
        UINT32 value = buffer[index];
        VddBusIo(&g_Bus, (WORD)(g_Comm.Ports[port].BasePort + COMM_RBR), 1, VDD_IO_OUT, &value);
    }
    return count;
}
INT WowCommInqueue(INT port)
{ return WowCommValid(port) ? (INT)g_Comm.Ports[port].ReceiveLength : 0; }
/* SETDTR/CLRDTR/SETRTS/CLRRTS and the two break calls all land on MCR, which is
   where they land on real hardware -- so a guest that asserts DTR and then reads
   MSR in loopback sees DSR come back, exactly as the port test does. */
static VOID WowCommMcr(INT port, UINT set, UINT clear)
{
    UINT32 value = 0;
    if (!WowCommValid(port)) return;
    VddBusIo(&g_Bus, (WORD)(g_Comm.Ports[port].BasePort + COMM_MCR), 1, VDD_IO_IN, &value);
    value = (value | set) & ~clear;
    VddBusIo(&g_Bus, (WORD)(g_Comm.Ports[port].BasePort + COMM_MCR), 1, VDD_IO_OUT, &value);
}
/* Named rather than exposing MCR bit numbers to the WOW layer: that header must
   not need a VDD header to compile, and "DTR" is the thing the caller means. */
VOID WowCommDtr(INT port, INT isOn) { WowCommMcr(port, isOn ? COMM_MCR_DTR : 0u, isOn ? 0u : COMM_MCR_DTR); }
VOID WowCommRts(INT port, INT isOn) { WowCommMcr(port, isOn ? COMM_MCR_RTS : 0u, isOn ? 0u : COMM_MCR_RTS); }

/* Is `-w` present as a WHOLE TOKEN? Substring matching would be wrong: a DOS
   program's own path can contain "-w" (…\my-widget\game.exe) and would then be
   handed silently to stock ntvdm -- a worse failure than the one this guard exists
   to prevent, because the program WOULD run and we would never hear about it. */
static INT LaunchIsWow(PCSTR command)
{
    PCSTR cursor = CommandLineAfterArgv0(command);
    while (*cursor) {
        if ((cursor[0] == '-' || cursor[0] == '/') && cursor[1] == 'w' && (cursor[2] == 0 || cursor[2] == ' '))
            return 1;
        if (*cursor == '"') { ++cursor; while (*cursor && *cursor != '"') ++cursor; if (*cursor) ++cursor; }
        else           { while (*cursor && *cursor != ' ') ++cursor; }
        while (*cursor == ' ') ++cursor;
    }
    return 0;
}

static CHAR      g_WowName[WOW_MAX_MOD][16];
/* ── ★ WHICH MODULE OWNS THIS SELECTOR? (GH #128, session 38) ─────────────────
     Needed because the WOW32 id space is per module: a call's stub segment names
     the table it belongs to, and the table decides whose numbering applies. The
     loader already records every module's runtime selectors, so this is a lookup
     rather than an inference. -1 = not one of ours. */
static INT WowModuleOfSelector(WORD selector)
{
    INT module, segment;
    if (!selector) return -1;
    for (module = 0; module < g_WowModuleCount; ++module)
        for (segment = 0; segment < (INT)g_WowModule[module].SegmentCount; ++segment)
            if (g_WowModule[module].Segments[segment].Selector == selector) return module;
    return -1;
}

/* ⛔ s88: NotifyWow (0x217, 6 args, retstub 0x12ea) too -- USER's OWN INIT calls it
     (observed: wKind 4, the DefWindowProc forward table) BEFORE any RegisterClass,
     so with only the two anchors above that call was "?'s table" and stepped over,
     the table stayed empty, and DefWindowProc forwarded nothing for the whole run.
     The triple is USER's: later calls carrying it were already dispatched as USER. */
static INT WowUserAnchor(WORD thunkId, WORD argumentBytes, WORD returnStub)
{
    return (thunkId == 0x190 && argumentBytes == 0 && returnStub == 0x0659)
        || (thunkId == 0x039 && argumentBytes == 4 && returnStub == 0x0c25)
        || (thunkId == 0x217 && argumentBytes == 6 && returnStub == 0x12ea);
}

static INT WowShellAnchor(WORD thunkId, WORD argumentBytes, WORD returnStub)
{
    return WowAnchorHit(g_WowShellAnchors,
                          (INT)(sizeof g_WowShellAnchors / sizeof g_WowShellAnchors[0]),
                          thunkId, argumentBytes, returnStub);
}

static INT WowCommonDialogAnchor(WORD thunkId, WORD argumentBytes, WORD returnStub)
{
    /* s89: the whole table (wowanchors.h) -- two rows left FindText unidentified. */
    return WowAnchorHit(g_WowCommdlgAnchors,
                          (INT)(sizeof g_WowCommdlgAnchors / sizeof g_WowCommdlgAnchors[0]),
                          thunkId, argumentBytes, returnStub);
}

static INT WowKeyboardAnchor(WORD thunkId, WORD argumentBytes, WORD returnStub)
{
    /* s89: the whole table (wowanchors.h), for the same reason as COMMDLG's. */
    return WowAnchorHit(g_WowKeyboardAnchors,
                          (INT)(sizeof g_WowKeyboardAnchors / sizeof g_WowKeyboardAnchors[0]),
                          thunkId, argumentBytes, returnStub);
}

static INT WowSoundAnchor(WORD thunkId, WORD argumentBytes, WORD returnStub)
{
    return WowAnchorHit(g_WowSoundAnchors,
                          (INT)(sizeof g_WowSoundAnchors / sizeof g_WowSoundAnchors[0]),
                          thunkId, argumentBytes, returnStub);
}

/* ⚠ WIDENED FOR THE SAME REASON SHELL'S WAS, and before it could cost anything:
     this used to be the three calls wowgdi.h services (`GetDeviceCaps`,
     `DeleteObject`, `DeleteDC`), so GDI's segment would only ever have been
     learned from a guest that happened to DESTROY something before it drew
     anything. Paint's first GDI calls are `CreateCompatibleDC`/`SelectObject`.
     The table is now all 367 stubs in GDI.EXE -- see src/wow/wowanchors.h. */
static INT WowGdiAnchor(WORD thunkId, WORD argumentBytes, WORD returnStub)
{
    return WowAnchorHit(g_WowGdiAnchors,
                          (INT)(sizeof g_WowGdiAnchors / sizeof g_WowGdiAnchors[0]),
                          thunkId, argumentBytes, returnStub);
}

enum { WOW_K2_STUB_LENGTH = 8 };   /* WowKernel2Stub: push imm16 ; call far -- the bytes before the return */
static INT WowKernel2Stub(WORD thunkId, WORD returnStub)
{
    const NE_SEGMENT *segment;
    const BYTE *image;
    DWORD offset;
    if (g_WowModuleCount < 1 || !g_WowImage[0] || g_WowModule[0].SegmentCount < 2) return 0;
    segment = &g_WowModule[0].Segments[1];
    if (!segment->Sector || returnStub < WOW_K2_STUB_LENGTH || (DWORD)returnStub > segment->Length) return 0;
    image = g_WowImage[0] + segment->FileOffset;
    offset   = (DWORD)returnStub - WOW_K2_STUB_LENGTH;
    return image[offset] == X86_OP_PUSH_IMM
        && (WORD)(image[offset + 1] | (image[offset + 2] << BYTE_SHIFT)) == thunkId
        && image[offset + 3] == X86_OP_CALL_FAR;
}
/* The PM->V86 transfer buffer for pointer-taking INT 21h calls; allocated in
   WowPlaceV86 and used by PmInt21Transfer. Declared here because the allocation
   site comes long before the use site. 0 = absent, and every arm checks. */
/* The PSP/arena block krnl386 is entered with. It must be the LAST thing allocated:
   krnl386 carves from ES+0x10 upward without asking DOS (see WowPlaceV86), so
   anything handed out after it would be memory krnl386 already believes it owns. */
/* ── THE HOST POOL, AND WHY THERE HAS TO BE ONE. ────────────────────────────────
     WowPlaceV86 hands krnl386 every remaining paragraph of conventional memory,
     because krnl386 carves from ES+0x10 upward without asking DOS and anything left
     free would be handed out twice. The consequence is that EVERY host structure
     allocated after that point finds nothing -- and two are, both lazily at the
     mode switch:
       * the INT 2Fh 168A vendor-API stub  -> "no memory for the stub", and krnl386
         prints "Inadequate DPMI Server" and exits (error #1 of its table);
       * the 256-vector default PM handler table -> INT 21h AH=35h then reports
         vector 0x21 as 0000:0000, so krnl386 saves a null previous-handler and its
         chain-to-DOS path calls into nothing.
     Both were found this way, one run apart. So: reserve a pool BEFORE the arena
     goes, and bump-allocate host structures out of it.
   ▸ THE RULE FOR ANYTHING ADDED LATER: on the WOW path, host memory comes from
     WowHostAllocate(), not DosMcbAllocate(). A DosMcbAllocate() after WowPlaceV86 will fail,
     and the failure will look like the guest's fault. */
/* 16 KB. Was 4 KB (0x100) with 0x41 in use, and the SFT block is 0x1D9 paragraphs on
   its own -- a 128-entry table of 59-byte entries. Sized at 0x200 first, and THAT IS
   THE REGRESSION THIS COMMENT EXISTS FOR: the SFT is claimed in WowPlaceV86 and the
   256-vector handler table lazily at the mode switch, so the SFT fitted, the handler
   table did not, and WowHostAllocate's failure is SILENT at that call site. The run
   died at PM step 0x0d with no error -- precisely the failure the note above predicts,
   walked into one release after writing it down. Leave the slack. */
#define WOW_HOSTPOOL_PARAS 0x400          /* 16 KB: SFT 0x1d9, handler table 0x40  */
static WORD      g_WowPoolSegment  = 0;
static WORD      g_WowPoolNext = 0;     /* paragraphs handed out so far          */
#define WOW_PSP_BLOCK_PARAS 0x40               /* a WOW launch keeps only the PSP's block   */
static WORD WowHostAllocate(WORD paras)
{
    WORD segment;
    if (!g_WowPoolSegment || g_WowPoolNext + paras > WOW_HOSTPOOL_PARAS) return 0;
    segment = (WORD)(g_WowPoolSegment + g_WowPoolNext);
    g_WowPoolNext = (WORD)(g_WowPoolNext + paras);
    return segment;
}
/* The `-a` argument of the WOW launch: the full path of krnl386.exe. krnl386 reads
   it back out of the DOS environment block to find its own file -- see WowPlaceV86. */
static CHAR      g_WowKernelPath[512];
/* Bytes of usable memory above krnl386's stack, handed to it in CX at entry. See
   the note at the entry setup and WowPlaceV86. */
static WORD      g_WowEntryCx = 0;
static WORD      g_WowPspSegment  = 0;
/* Where WOW32 0xc5 puts a resolved module path so the guest can point at it. Its own
   paragraph, and separate from the transfer buffer above on purpose -- see the
   allocation site and the 0xc5 service. */
static WORD      g_WowPathSegment = 0;
/* ── ★ WHERE A LAUNCHED TASK'S ENVIRONMENT LIVES. (seg2 0xd1, session 39 part 8) ──
     A COPY, and the copy is the whole point: the parent hands its child an
     environment through its own PSP and then FREES that block the moment
     LoadModule returns -- measured, `LDTSYNC idx 0x15f <- base=0 acc=0x00` one line
     before `PSPENV CHANGED: sel 0x03bf +0x2c 0x0aff -> 0x03c7`. A child pointed at
     the parent's block therefore holds a selector that stops being present a few
     calls later, which is exactly the `#GP` the first cut of the service produced.
     Its own paragraph for the same reason as the path scratch: the child keeps this
     pointer for its whole life, so it cannot share a buffer anything else reuses. */
static WORD      g_WowEnvironmentSegment  = 0;
/* ── ★ THE WAY BACK OUT OF 16-BIT CODE. (GH #128, session 40) ─────────────────────
     One paragraph holding `C4 C4 57`, and a 16-bit CODE selector over it. It is the
     far return address every host-made call to a Win16 procedure is given, and it is
     dispatched by its LINEAR ADDRESS rather than by the BOP code byte -- the byte is
     for the reader, the address is ours by construction and cannot be collided with
     by our own INT-site patcher, which also writes `C4 C4`. See src/wow/wowcall.h. */
static WORD      g_WowCallbackSegment = 0;
static DWORD     g_WowCallbackLinear = 0;
static WORD      g_WowCallbackSelector = 0;          /* built at the first callback */
/* Report any change to a tracked PSP's environment field. Called at every WOW32 BOP
   AND every protected-mode INT 21h, because the resolution of the answer is exactly
   the spacing of the sampler: sampling only at WOW32 calls put the whole of WOWEXEC's
   launcher -- LoadModule included -- inside one window, which names a suspect rather
   than a writer. */
static PSTR WowPspEnvironmentCheck(PSTR cursor, PCSTR where)
{
    INT index;
    for (index = 0; index < g_WowPspCount; ++index) {
        /* ⚠⚠ RESOLVE THE SELECTOR EVERY TIME. A LINEAR ADDRESS IS NOT A PSP.
             krnl386 RE-BASES these selectors -- measured, twice in three log lines
             for 0x0adf (`INT31h 04F2 ... base=0x000297c0` then `base=0x000299e0`)
             -- so a frozen linear address stops pointing at the guest's PSP the
             moment it moves, and this instrument would then report whatever now
             lives at the old address as if it were the field. Watch what the GUEST
             watches: the selector. */
        DWORD linear = DpmiSelectorBase(g_WowPspSelector[index]);
        const volatile BYTE *psp = (const volatile BYTE *)(ULONG_PTR)linear;
        WORD now;
        if (!linear || !HostReadable((const VOID *)psp, DOS_PSP_ENVIRONMENT + sizeof(WORD))) continue;
        if (linear != g_WowPspLinear[index]) {
            cursor = LogPut(cursor, "PSPENV REBASED: sel 0x"); cursor = LogHex(cursor, g_WowPspSelector[index]);
            cursor = LogPut(cursor, " 0x"); cursor = LogHex(cursor, g_WowPspLinear[index]);
            cursor = LogPut(cursor, " -> 0x"); cursor = LogHex(cursor, linear);
            cursor = LogPut(cursor, " (the PSP we built is at the OLD address)\r\n");
            g_WowPspLinear[index] = linear;
        }
        now = (WORD)(psp[DOS_PSP_ENVIRONMENT] | (psp[DOS_PSP_ENVIRONMENT + 1] << BYTE_SHIFT));
        if (g_WowPspEnvironment[index] == now) continue;
        cursor = LogPut(cursor, "PSPENV CHANGED: sel 0x"); cursor = LogHex(cursor, g_WowPspSelector[index]);
        cursor = LogPut(cursor, " +0x2c 0x"); cursor = LogHex(cursor, g_WowPspEnvironment[index]);
        cursor = LogPut(cursor, " -> 0x"); cursor = LogHex(cursor, now);
        cursor = LogPut(cursor, " (seen at "); cursor = LogPut(cursor, where); cursor = LogPut(cursor, ")\r\n");
        g_WowPspEnvironment[index] = now;
    }
    return cursor;
}
/* Pull the `-a <path>` argument out of a WOW command line. Measured shape:
     "…\ntvdm.exe" -f -i1 -w -a C:\WINDOWS\system32\krnl386.exe
   That path is the module WOW is asked to bootstrap, so it is the loader's input. */
static INT WowArgumentA(PCSTR command, PSTR out, INT cap)
{
    PCSTR cursor = CommandLineAfterArgv0(command);
    INT index = 0;
    out[0] = 0;
    while (*cursor) {
        if ((cursor[0] == '-' || cursor[0] == '/') && cursor[1] == 'a' && (cursor[2] == 0 || cursor[2] == ' ')) {
            cursor += 2;
            while (*cursor == ' ') ++cursor;
            if (*cursor == '"') { ++cursor; while (*cursor && *cursor != '"' && index < cap - 1) out[index++] = *cursor++; }
            else           { while (*cursor && *cursor != ' '  && index < cap - 1) out[index++] = *cursor++; }
            out[index] = 0;
            return index ? 0 : -1;
        }
        if (*cursor == '"') { ++cursor; while (*cursor && *cursor != '"') ++cursor; if (*cursor) ++cursor; }
        else           { while (*cursor && *cursor != ' ') ++cursor; }
        while (*cursor == ' ') ++cursor;
    }
    return -1;
}

/* Replace the file name in `path` with `leaf`, so the modules that live beside
   krnl386.exe can be found without hard-coding %SystemRoot%\system32. */
static VOID WowSibling(PCSTR path, PCSTR leaf, PSTR out, INT cap)
{
    INT index, cut = 0;
    for (index = 0; path[index] && index < cap - 1; ++index) {
        out[index] = path[index];
        if (path[index] == '\\' || path[index] == '/') cut = index + 1;
    }
    for (index = 0; leaf[index] && cut < cap - 1; ++index) out[cut++] = leaf[index];
    out[cut] = 0;
}

/* Parse one module and give its segments memory. NO relocation -- see the two-phase
   note above. Returns the g_WowModule index, or -1. */
static INT WowLoadOne(PCSTR path)
{
    CHAR message[700], *cursor;
    HANDLE file;
    DWORD size = 0, got = 0;
    BYTE *image;
    NE_MODULE *module;
    INT slot = g_WowModuleCount, index;

    if (slot >= WOW_MAX_MOD) return -1;
    module = &g_WowModule[slot];

    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        cursor = message; cursor = LogPut(cursor, "WOWTRY: cannot open "); cursor = LogPut(cursor, path);
        cursor = LogPut(cursor, " err=0x"); cursor = LogHex(cursor, GetLastError());
        cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, message, cursor); return -1;
    }
    size = GetFileSize(file, NULL);
    image = (BYTE *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!image || !ReadFile(file, image, size, &got, NULL) || got != size) {
        CloseHandle(file);
        cursor = message; cursor = LogPut(cursor, "WOWTRY: read failed for "); cursor = LogPut(cursor, path);
        cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, message, cursor); return -1;
    }
    CloseHandle(file);

    if (NeParse(module, image, size) != 0) {
        cursor = message; cursor = LogPut(cursor, "WOWTRY: ne_parse REJECTED "); cursor = LogPut(cursor, path);
        cursor = LogPut(cursor, " at ne.h line "); cursor = LogHex(cursor, (DWORD)module->Error);
        cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, message, cursor); return -1;
    }
    if (NeOwnName(module, g_WowName[slot], sizeof g_WowName[slot]) != 0)
        g_WowName[slot][0] = 0;

    cursor = message;
    cursor = LogPut(cursor, "WOWTRY: "); cursor = LogPut(cursor, path);
    cursor = LogPut(cursor, "\r\n  name=");                cursor = LogPut(cursor, g_WowName[slot]);
    cursor = LogPut(cursor, (module->ProgramFlags & NE_PROG_LIBRARY) ? " LIBRARY" : " PROGRAM");
    cursor = LogPut(cursor, " segs=");                     cursor = LogHex(cursor, module->SegmentCount);
    cursor = LogPut(cursor, " imports-from=");             cursor = LogHex(cursor, module->ModuleCount);
    cursor = LogPut(cursor, " movable=");                  cursor = LogHex(cursor, module->MovableCount);
    cursor = LogPut(cursor, "\r\n  CS:IP=");               cursor = LogHex(cursor, module->CsIp >> WORD_SHIFT);
    cursor = LogPut(cursor, ":");                          cursor = LogHex(cursor, module->CsIp & WORD_MASK);
    cursor = LogPut(cursor, "  SS:SP=");                   cursor = LogHex(cursor, module->SsSp >> WORD_SHIFT);
    cursor = LogPut(cursor, ":");                          cursor = LogHex(cursor, module->SsSp & WORD_MASK);
    cursor = LogPut(cursor, "  autodata=");                cursor = LogHex(cursor, module->AutoData);
    cursor = LogPut(cursor, "  heap=0x");                  cursor = LogHex(cursor, module->Heap);
    cursor = LogPut(cursor, "  stack=0x");                 cursor = LogHex(cursor, module->Stack);
    cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, message, cursor);

    for (index = 0; index < (INT)module->SegmentCount; ++index) {
        NE_SEGMENT *segment = &module->Segments[index];
        UINT32 need = NeSegmentAllocSize(segment), byteIndex;
        segment->Memory = (BYTE *)VirtualAlloc(NULL, need, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_READWRITE);
        segment->Selector = 0;                          /* NO selector yet -- phase 2 assigns it */
        if (!segment->Memory) {
            cursor = message; cursor = LogPut(cursor, "WOWTRY: seg alloc failed\r\n");
            LogAppend(LOG_PATH, message, cursor); return -1;
        }
        if (segment->Sector) for (byteIndex = 0; byteIndex < segment->Length; ++byteIndex) segment->Memory[byteIndex] = image[segment->FileOffset + byteIndex];
    }
    g_WowImage[slot] = image;
    ++g_WowModuleCount;
    return slot;
}

/* ── EXPERIMENTAL: load the WOW module set and report the layout. ───────────────────
     Gated on WOWTRY_FLAG, and it does NOT execute anything. It answers "does the
     loader work on the real binaries, inside the real host, against real memory",
     which is a different question from "does it parse on the build machine", and a
     much smaller one than "does krnl386 initialise". Answering them one at a time is
     how the DPMI work got anywhere.

     krnl386 alone is not enough any more: it imports from nothing, so it exercises
     none of the import machinery. user.exe and gdi.exe both import from KERNEL by
     ordinal (and user once BY NAME), which is the path every real program takes.
   ⚠ Segments get HOST memory here and nothing else -- which is NOT where execution
     will want them. See WowProbeSelectors() for the correction: krnl386's init entry
     runs in V86, so a real load puts its segments in CONVENTIONAL memory and relocates
     against real-mode paragraphs. */
static VOID WowProbeLoad(PCSTR command)
{
    /* The whole graph, in dependency order. Everything imports from KERNEL, USER
       also needs SYSTEM, and wowexec needs KEYBOARD -- so the drivers come before
       the modules that bind to them. Measured: with these eleven present, every
       import in the set resolves and nothing is left dangling. */
    static PCSTR sibling[] = {
        "system.drv", "keyboard.drv", "mouse.drv", "sound.drv", "comm.drv",
        "gdi.exe", "user.exe", "shell.dll", "toolhelp.dll", "wowexec.exe"
    };
    CHAR path[512], siblingPath[512], message[700], *cursor;
    SIZE_T index;

    cursor = message; cursor = LogPut(cursor, "WOWTRY: probe begins\r\n"); LogAppend(LOG_PATH, message, cursor);

    if (WowArgumentA(command, path, sizeof path) != 0) {
        cursor = message; cursor = LogPut(cursor, "WOWTRY: no -a argument on the command line\r\n");
        LogAppend(LOG_PATH, message, cursor); return;
    }
    {   INT length = 0;                       /* keep it: krnl386 needs its own path */
        while (path[length] && length < (INT)sizeof g_WowKernelPath - 1) {
            g_WowKernelPath[length] = path[length]; ++length;
        }
        g_WowKernelPath[length] = 0;
    }
    if (WowLoadOne(path) < 0) return;
    for (index = 0; index < sizeof sibling / sizeof sibling[0]; ++index) {
        WowSibling(path, sibling[index], siblingPath, sizeof siblingPath);
        WowLoadOne(siblingPath);                    /* logs its own failure; keep going */
    }

    cursor = message; cursor = LogPut(cursor, "WOWTRY: modules loaded=");  cursor = LogHex(cursor, (DWORD)g_WowModuleCount);
    cursor = LogPut(cursor, "; NOT relocated yet (selectors first)\r\n");
    LogAppend(LOG_PATH, message, cursor);
}

static INT WowRefuse(PCSTR command)
{
    CHAR message[256], *cursor = message;
    cursor = LogPut(cursor, "STAGE0: WIN16/WOW -- NOT SUPPORTED and cannot be handed back "
                "(see GH #129). Refusing loudly.\r\n");
    LogAppend(LOG_PATH, message, cursor);
#define HOST_WOW16_REFUSED_TEXT \
        "NTVDMEX cannot run 16-bit Windows programs.\n\n" \
        "It replaces the DOS half of NTVDM only. Because Windows starts 16-bit " \
        "Windows programs through the same ntvdm.exe, this launch reached NTVDMEX " \
        "as well -- and it cannot be passed back to the original.\n\n" \
        "To run this program, remove NTVDMEX's interception:\n\n" \
        "    reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion" \
        "\\Image File Execution Options\\ntvdm.exe\" /v Debugger /f\n\n" \
        "DOS programs will then use the original NTVDM too."
    MessageBoxA(NULL, HOST_WOW16_REFUSED_TEXT, HOST_WOW16_REFUSED_TITLE, MB_OK | MB_ICONERROR);
    return 1;
}

/* ── ★ IS THIS CODE SELECTOR NOT PRESENT? (session 57) ─────────────────────────────
     Declared in wowdlg.h and answered here, because the LDT is the host's. A Win16
     code segment is loaded on demand and its descriptor is marked not-present until
     it is, so a call into one must go through the RETF trampoline rather than by
     writing CS -- see WOWCALL_RETF_OFF in wowcall.h and the CARDFILE run that named
     it. The same three lines the BOP handler's own callback path computes inline;
     one day both should read this. */
INT WowDlgIsSelectorAbsent(WORD selector)
{
    WORD index = (WORD)(DPMI_SELECTOR_INDEX(selector));
    return index && index < DPMI_LDT_MAX && !(g_Ldt[index].Access & X86_DESCRIPTOR_PRESENT);
}
enum { WOW_CALLBACK_STUB_LIMIT = 0x0F };   /* the callback stub's 16-byte segment */
/* ── ★★★ A 16-BIT CODE SELECTOR OVER THE CALLBACK RETURN STUB. (session 40) ────────
     Not `DpmiSegmentToDescriptor`: that builds a DATA descriptor, and the guest has to
     EXECUTE these three bytes. The limit is one paragraph on purpose -- the stub is
     three bytes and nothing may ever be reached past it, so a stray branch into this
     selector faults here rather than running off into whatever follows. */
static WORD WowCallbackSelector(VOID)
{
    INT index;
    if (g_WowCallbackSelector) return g_WowCallbackSelector;
    if (!g_WowCallbackSegment) return 0;
    index = DpmiHostIndex();       /* host-private: the guest returns THROUGH this */
    if (index < 0) return 0;
    g_Ldt[index].Base   = g_WowCallbackLinear;
    g_Ldt[index].Limit  = WOW_CALLBACK_STUB_LIMIT;
    g_Ldt[index].Access = DPMI_ACCESS_CODE;                /* present, DPL3, code, readable */
    g_Ldt[index].Flags  = 0;                   /* 16-bit                        */
    DpmiInstall(index);
    g_WowCallbackSelector = (WORD)DPMI_LDT_SELECTOR(index);
    return g_WowCallbackSelector;
}

/* ── CAN THIS PROCESS INSTALL AN LDT DESCRIPTOR AT ALL, RIGHT HERE? ─────────────────
     Every descriptor the WOW stage tried was refused with STATUS_INVALID_PARAMETER_1
     (0xC00000EF), and varying index / access / limit / base changed nothing -- so the
     objection is not to the descriptor, it is to the CALL CONTEXT. The DPMI path
     installs descriptors happily, but it does so much later, with the VDM fully
     established and a guest running.

     So run the IDENTICAL matrix from the IDENTICAL point in WinMain on a DOS launch
     and compare. Same code, same moment, one variable: which kind of launch this is.
     That is the only way to tell "NtSetLdtEntries needs more VDM setup" apart from
     "a WOW launch leaves the process in a different state". */
static VOID WowProbeLdtMatrix(PCSTR tag)
{
    static const struct { PCSTR Description; DWORD Base, Limit; BYTE Access, Flags; INT Index; }
    cases[] = {
        { "idx3  code small",  0x10000, 0x0FFF, 0xFA, 0, 3  },
        { "idx8  code small",  0x10000, 0x0FFF, 0xFA, 0, 8  },
        { "idx8  data small",  0x10000, 0x0FFF, 0xF2, 0, 8  },
        { "idx9  data 64K",    0x10000, 0xFFFF, 0xF2, 0, 9  },
    };
    CHAR message[300], *cursor;
    SIZE_T caseIndex;
    for (caseIndex = 0; caseIndex < sizeof cases / sizeof cases[0]; ++caseIndex) {
        DWORD low, high; LONG status;
        WORD selector = (WORD)DPMI_LDT_SELECTOR(cases[caseIndex].Index);
        DpmiBuildDescriptor(cases[caseIndex].Base, cases[caseIndex].Limit, cases[caseIndex].Access, cases[caseIndex].Flags, &low, &high);
        status = VdmInstallLdtEntries(selector, low, high, selector, low, high);
        cursor = message;
        cursor = LogPut(cursor, "LDTPROBE["); cursor = LogPut(cursor, tag);
        cursor = LogPut(cursor, "] ");       cursor = LogPut(cursor, cases[caseIndex].Description);
        cursor = LogPut(cursor, " sel=0x");  cursor = LogHex(cursor, selector);
        cursor = LogPut(cursor, " -> st=0x");cursor = LogHex(cursor, (DWORD)status);
        cursor = LogPut(cursor, "\r\n"); LogAppend(LDTLOG_PATH, message, cursor);
    }
}

/* ── WOW STAGE 2: give the loaded module REAL SELECTORS. (GH #128) ──────────────────
     The load stage put each segment in host memory and fixed it up. This gives every
     segment an LDT descriptor, which is why it lives here, after the DPMI LDT pool
     exists and the VDM is registered.

   ⚠⚠ THIS IS NOT THE ENTRY PATH, AND THE COMMENT THAT USED TO SIT HERE SAID IT WAS.
     It claimed "krnl386 is the 386 ENHANCED-mode kernel: it runs in 16-bit PROTECTED
     mode, not V86". That is a plausible inference from what krnl386 IS, it survived
     four sessions in these comments, and krnl386's own instructions refute it:

       c045  mov ax,es / shl ax,4      ES is a real-mode PARAGRAPH. Shifting a selector
                                       by 4 is meaningless; this computes the right
                                       answer against two independent oracle dumps.
       c03b  mov word [cs:0x30], ds    A store through CS. Never legal in protected
                                       mode -- a code selector is not writable.
       c0c2  call <2F/1687 check>      It finds the DPMI host and SWITCHES ITSELF, with
                                       AX=0 (16-bit client), then checks cs&7==7, then
                                       INT 31h 000A for a data alias of CS -- and redoes
                                       the SAME cs:0x30 store through that alias.

     The identical store, once per mode, is the binary telling you where the boundary
     is. krnl386 is entered in V86 and becomes a 16-bit DPMI client; that is why
     INT 31h 0002 (paragraph -> selector) is the function it calls most, twelve times.
     So real execution needs the segments in CONVENTIONAL memory with paragraph
     relocations. What is below still earns its keep -- it proved LDT installation
     works on a WOW launch, proved relocation against real descriptors, and caught the
     force-typed-index bug -- but it belongs AFTER the switch, not before it.

   ⚠ THE QUESTION THIS ANSWERS, AND IT IS NOT RHETORICAL: XP's LDT validator caps
     base + limit at MmHighestUserAddress (~2GB) -- a true 4GB flat selector is refused
     outright (Kernel RE session 7, and it is why Doom's DOS/4GW selector has to be
     clamped). Our segments come from VirtualAlloc, which normally lands well under 2GB,
     so these SHOULD install. "Should" is not "does", and VdmInstallLdtEntries returns an
     NTSTATUS, so ask it rather than assume. A descriptor that silently fails to install
     leaves a selector that faults on first use -- a silent death, far from the cause. */
/* ── WHERE IS OUR DESCRIPTOR TABLE? ─────────────────────────────────────────────────
     krnl386 wants what NTVDM's "MS-DOS" vendor API gives it: a WRITABLE SELECTOR ONTO
     THE DESCRIPTOR TABLE, so it can edit descriptors without a DPMI call each time.
     Measured off stock (tests/probes/dos/vendprobe.asm): selector 0x0137, writable,
     base 0x001140B0, limit 0x5FFF -- and the descriptor at window[CS & 0xFFF8] reads
         FF FF D0 6D 00 FA 00 00   -> base 0x00006DD0, access 0xFA (code)
     while DPMI 0006 reports CS's base as 0x00006DD0. Two independent routes, same
     base, right type. It is the real table, and it lives at a USER-MODE address.

   ⚠ WE CANNOT ASK THE CPU WHERE OURS IS. `SLDT` yields the GDT selector, and the GDT
     is not readable from ring 3. So find it the only way available from user mode:
     install descriptors whose contents are unique, then search our own address space
     for those exact eight bytes. A hit is that descriptor's slot; the table base is
     the hit minus (index * 8).

     Clean-room by construction -- it uses only our own memory and the documented
     descriptor encoding, and depends on no Windows internal we would have to be told.

   ⚠ AND IT MUST NOT LIE, IN EITHER DIRECTION:
       * one magic value could occur by chance in an unrelated buffer, so a candidate
         is confirmed by a SECOND descriptor at a different index appearing at the
         predicted offset. One match is a coincidence; two at the right stride is a
         table.
       * a "not found" is worthless unless the things searched for exist, so LAR/LSL
         verify both probes installed before the search runs. Otherwise a refused
         descriptor and an unmappable table produce the same log line -- and one of
         those is a conclusion about Windows while the other is a bug in here. */
static DWORD g_WowLdtBase = 0;

static INT WowLdtPeek(DWORD linear, DWORD low, DWORD high)
{
    const volatile DWORD *descriptor = (const volatile DWORD *)(ULONG_PTR)linear;
    return descriptor[0] == low && descriptor[1] == high;
}
enum { WOW_LDT_PROBES = 2, WOW_LDT_PROBE_GAP = 4, WOW_LDT_PROBE1_BASE = 0x5A5A1000, WOW_LDT_PROBE1_LIMIT = 0x0123, WOW_LDT_PROBE2_BASE = 0x3C3C2000, WOW_LDT_PROBE2_LIMIT = 0x0456 };   /* WowFindLdtBase: two distinctive descriptors */
static DWORD WowFindLdtBase(VOID)
{
    MEMORY_BASIC_INFORMATION memoryInfo;
    DWORD low1, high1, low2, high2;
    INT index1, index2;
    BYTE *address = NULL;
    CHAR message[320], *cursor;

    if (g_WowLdtBase) return g_WowLdtBase;
    if (g_LdtNext + WOW_LDT_PROBES >= DPMI_LDT_MAX) return 0;

    /* Two probes, distinctive and different. ⚠ BOTH bases must sit under
       XP_LDT_MAX_LINEAR (~2GB) or the validator refuses the descriptor outright and
       the search can never find what was never installed. The first cut used
       0xA5A52000 -- 2.77GB -- and reported "not found" for that reason alone: a false
       negative dressed as a finding, with the cap documented a few hundred lines up. */
    index1 = g_LdtNext++;
    g_Ldt[index1].Base = WOW_LDT_PROBE1_BASE; g_Ldt[index1].Limit = WOW_LDT_PROBE1_LIMIT;
    g_Ldt[index1].Access = DPMI_ACCESS_DATA;     g_Ldt[index1].Flags = 0;
    DpmiInstall(index1);
    /* ⚠ NOT CONSECUTIVE, AND THAT IS THE WHOLE POINT. The first cut used i1 and i1+1,
       "confirmed" a hit, and reported a table base -- which MOVED between two runs
       while the winning address stayed at 0x02219018. Two consecutive descriptors sit
       8 bytes apart both in a real table AND in the buffer NtSetLdtEntries marshals
       its two entries through, so that check could not tell them apart and was
       matching the buffer. A five-slot gap is 40 bytes in a table and still 8 in the
       buffer, so only a real table can satisfy it. */
    g_LdtNext += WOW_LDT_PROBE_GAP;                        /* leave a gap between the two probes */
    index2 = g_LdtNext++;
    g_Ldt[index2].Base = WOW_LDT_PROBE2_BASE; g_Ldt[index2].Limit = WOW_LDT_PROBE2_LIMIT;
    g_Ldt[index2].Access = DPMI_ACCESS_DATA;     g_Ldt[index2].Flags = 0;
    DpmiInstall(index2);
    DpmiBuildDescriptor(g_Ldt[index1].Base, g_Ldt[index1].Limit, DPMI_ACCESS_DATA, 0, &low1, &high1);
    DpmiBuildDescriptor(g_Ldt[index2].Base, g_Ldt[index2].Limit, DPMI_ACCESS_DATA, 0, &low2, &high2);

    {   WORD selector1 = (WORD)DPMI_LDT_SELECTOR(index1), selector2 = (WORD)DPMI_LDT_SELECTOR(index2);
        DWORD accessRights1 = 0, accessRights2 = 0;
        BYTE isValid1 = 0, isValid2 = 0;
        __asm__ __volatile__("lar %2, %0\n\tsetz %1" : "=r"(accessRights1), "=q"(isValid1) : "r"(selector1) : "cc");
        __asm__ __volatile__("lar %2, %0\n\tsetz %1" : "=r"(accessRights2), "=q"(isValid2) : "r"(selector2) : "cc");
        cursor = message;
        cursor = LogPut(cursor, "WOWLDT: probes idx 0x"); cursor = LogHex(cursor, (DWORD)index1);
        cursor = LogPut(cursor, isValid1 ? " LAR ok ar=0x" : " LAR FAILED ar=0x"); cursor = LogHex(cursor, accessRights1);
        cursor = LogPut(cursor, " | idx 0x"); cursor = LogHex(cursor, (DWORD)index2);
        cursor = LogPut(cursor, isValid2 ? " LAR ok ar=0x" : " LAR FAILED ar=0x"); cursor = LogHex(cursor, accessRights2);
        cursor = LogPut(cursor, "\r\n"); LogAppend(LDTLOG_PATH, message, cursor);
        if (!isValid1 || !isValid2) {
            cursor = message; cursor = LogPut(cursor, "WOWLDT: a probe did NOT install -- search skipped, a "
                               "'not found' would have meant nothing\r\n");
            LogAppend(LDTLOG_PATH, message, cursor);
            return 0;
        }
    }

    while (VirtualQuery(address, &memoryInfo, sizeof memoryInfo) == sizeof memoryInfo) {
        DWORD protection = memoryInfo.Protect & BYTE_MASK;
        INT readable = memoryInfo.State == MEM_COMMIT && !(memoryInfo.Protect & PAGE_GUARD)
            && (protection == PAGE_READONLY || protection == PAGE_READWRITE
                || protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ
                || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY);
        if (readable) {
            DWORD regionBase = (DWORD)(ULONG_PTR)memoryInfo.BaseAddress, regionSize = (DWORD)memoryInfo.RegionSize, offset;
            for (offset = 0; offset + X86_DESCRIPTOR_SIZE <= regionSize; offset += X86_DESCRIPTOR_SIZE) {          /* descriptors are 8-aligned */
                DWORD cand = regionBase + offset;
                if (!WowLdtPeek(cand, low1, high1)) continue;
                {   DWORD tableBase = cand - (DWORD)index1 * X86_DESCRIPTOR_SIZE;
                    DWORD other = tableBase + (DWORD)index2 * X86_DESCRIPTOR_SIZE;
                    if (other < regionBase || other + X86_DESCRIPTOR_SIZE > regionBase + regionSize) continue;   /* same region only */
                    if (!WowLdtPeek(other, low2, high2)) continue;   /* one hit is chance */
                    g_WowLdtBase = tableBase;
                    cursor = message;
                    cursor = LogPut(cursor, "WOWLDT: descriptor table FOUND at linear 0x");
                    cursor = LogHex(cursor, tableBase);
                    cursor = LogPut(cursor, " (idx 0x"); cursor = LogHex(cursor, (DWORD)index1);
                    cursor = LogPut(cursor, " at 0x"); cursor = LogHex(cursor, cand);
                    cursor = LogPut(cursor, ", confirmed idx 0x"); cursor = LogHex(cursor, (DWORD)index2);
                    cursor = LogPut(cursor, " at 0x"); cursor = LogHex(cursor, other);
                    cursor = LogPut(cursor, ")\r\n"); LogAppend(LDTLOG_PATH, message, cursor);
                    return tableBase;
                }
            }
        }
        address = (BYTE *)memoryInfo.BaseAddress + memoryInfo.RegionSize;
        if ((DWORD)(ULONG_PTR)address >= NT_USER_SPACE_END_U) break;
    }
    cursor = message; cursor = LogPut(cursor, "WOWLDT: descriptor table NOT in our own address space "
                       "(probes verified installed) -- a real LDT window is not "
                       "available this way\r\n");
    LogAppend(LDTLOG_PATH, message, cursor);
    return 0;
}
#define WOW_ENTRY_CX_FULL_U 0xF880u   /* krnl386's entry CX: its 64 KB selector less our header image */
static NE_REGISTRY g_WowRegistry;
enum { WOW_FILLER_SLACK_PARAS = 2 };   /* WowPlaceV86: the MCB headers a filler allocation adds */
/* ── WOW ENTRY STAGE: put krnl386 in CONVENTIONAL memory and relocate to PARAGRAPHS. ─
     This, not the selector stage below, is how krnl386 is actually entered -- see the
     refutation there. It runs in V86 first and switches itself to protected mode via
     INT 2Fh 1687, so what it needs at entry is exactly what a DOS program needs: real
     memory under 1MB and real-mode segment values.

   ⚠ THE SEGMENT BYTES ARE RE-COPIED FROM THE FILE IMAGE, NOT MOVED FROM s->mem.
     s->mem may already have been relocated once (against LDT selectors), and
     relocation is NOT idempotent -- a chained record's next site is the word AT the
     current site, which the first pass overwrote with an address. Copying fresh from
     the untouched image is what makes a second, differently-based relocation legal at
     all. Same rule as ne.h's registry note; this is the place it is easiest to break.

   Returns 0 and fills the entry registers. krnl386 imports from nothing, so no
   importer callback is needed here; user/gdi come later and will need one. */
static INT WowPlaceV86(DOS_MACHINE *machine, WORD *entryCs, WORD *eip,
                         WORD *entryDs, WORD *entrySs, WORD *esp)
{
    /* krnl386 is a LIBRARY: SS:SP = 0:0 and stack = 0 in the header, so nothing tells
       us how big a stack it wants. It pushes on its second instruction. 4K of
       paragraphs is generous for an init path and cheap; if it ever overruns, that is
       a stack overflow into the block below and it will look like one.
     ★ IT OVERRAN, AND IT LOOKED EXACTLY LIKE ONE. (GH #128, session 37) The first
       run in which krnl386 got far enough to OPEN the Win16 program ended in a #SS
       at `ss:sp = 0x1f:0x0002` with `bytes@fault = 66 55` -- a `push ebp` two bytes
       from the bottom. 4 KB covered the bring-up and does not cover loading a
       program: that path recurses through the module loader, the relocation walk and
       the descriptor commit. 32 KB, because the block is conventional memory we hand
       to krnl386 anyway and the header image sits immediately above it either way.
     ⚠ SP IS 16-BIT, so this cannot exceed 0xFFF paragraphs -- the entry SP is
       `WOW_STACK_PARAS << 4` and the NE header image is placed at
       `stackBlockSegment + WOW_STACK_PARAS` precisely so that `base(SS) + SP` lands on it. Both
       derive from this constant; do not pin either by hand. */
    enum { WOW_STACK_PARAS = 0x800 };            /* 32 KB */
    /* ── ★★★ THE WHOLE FILE IMAGE IS STAGED, NOT JUST THE HEADER. ─────────────────
         This used to be `enum { WOW_HDRIMG_PARAS = 0x400 }` -- 16 KB, "comfortably
         more than krnl386's tables need". It is comfortably more than the TABLES need
         and far less than the LOADER needs, and session 33 measured the difference in
         stock ntvdm's own memory:

             stock: the file from the NE header on, 0x16440 bytes, resident at linear
                    0x899f0, with a 64 KB selector (0x01ef) over it -- and segments 2,
                    3 and 4 recoverable there byte-for-byte at 0x895f0 + file_offset.
             ours:  the first 0x4000 bytes, and nothing else.

         krnl386's own LoadSegment reads a segment's image out of memory (it issues no
         file open while loading them -- confirmed, session 32). Segment 1 lives at
         file 0x2040, inside what we kept; segments 2, 3 and 4 live at 0xf880, 0x137a0
         and 0x14a60 -- ALL PAST THE CUT. So LoadSegment(1) worked and LoadSegment(2)
         read whatever followed and decoded it as relocation records, which is exactly
         the failure session 32 bracketed to the instruction.
       ⇒ Stage all of it. The size is the file's, so it is computed, not a constant. */
    WORD  headerImageParagraphs;                          /* whole image, in paragraphs */
    WORD  windowParas;                          /* what the block must really cover */
    /* ── ★★★ AND THE WHOLE 64 KB OF IT MUST BE OURS TO GIVE. ───────────────────────
         krnl386 builds a SIXTY-FOUR KILOBYTE selector over `base(SS) + SP` (observed in
         the LDT) and treats everything above the header image as its scratch arena -- we even
         hand it the size in CX (0xFFF0 - 0x4000 = 0xBFF0 bytes). But the block was
         allocated as stack + header image only, 0x500 paragraphs, so the selector ran
         0xBFFF bytes PAST the end of what DOS had given us:

             SS                      para 0x1abf
             header selector base    para 0x1bbf   (64 KB window -> para 0x2bbf)
             our block ENDS          para 0x1fbf
             krnl386's PSP/arena     para 0x1fc0   ★ inside the window

         i.e. the scratch arena we promised it was, for 48 KB of its length, its OWN
         PSP and arena block -- which it is independently carving from. Two owners of
         one region, which is the exact failure the memory-model note above was written
         about, reappearing one allocation later.

       ⚠ HOW IT SURFACED. krnl386 COMPACTS that arena with a block copy -- paragraphs
         from higher in the selector are copied down to offset 0 inside the very same
         selector. Walking the guest with breakpoints bracketed the death to a single
         call between two armed sites, and that call is the copy.
         A block copy striding through memory a second owner is using explains a host
         that dies with no VDM fault, no reflect and no crash record.

       ⇒ So allocate the window, not just the header. This is the size of the SELECTOR,
         and it is the thing that has to be real memory; the staged image may now be
         LARGER than it (krnl386's is 0x1644 paragraphs), in which case the image wins. */
    enum { WOW_SELECTOR_PARAS = 0x1000 };        /* 64 KB -- the whole scratch selector */
    NE_MODULE *module = &g_WowModule[0];
    BYTE *image = g_WowImage[0];
    CHAR message[400], *cursor;
    WORD stackBlockSegment = 0, stackBlockMaximum = 0;
    DWORD imageLength;
    INT index;

    if (!g_WowModuleCount || !image) return -1;

    imageLength        = module->ImageLength > module->Header ? module->ImageLength - module->Header : 0;
    headerImageParagraphs  = (WORD)((imageLength + PARAGRAPH_LAST_BYTE_U) >> PARAGRAPH_SHIFT);
    windowParas  = headerImageParagraphs > (WORD)WOW_SELECTOR_PARAS
                  ? headerImageParagraphs : (WORD)WOW_SELECTOR_PARAS;

    /* ── SHRINK THE PROGRAM BLOCK FIRST, OR THERE IS NOTHING TO ALLOCATE. ───────────
         DosMcbInitialize lays out one 'Z' block covering all 0x9F00 paragraphs of
         conventional memory, OWNED by the PSP -- which is faithful: real DOS gives a
         .COM the entire arena and the program shrinks it before allocating. The first
         run of this stage asked for krnl386's 0xD80 paragraphs and got
             "no conventional memory for seg 1, largest free 0x0 paras"
         which reads like exhaustion and is actually "everything is owned, nothing is
         free". Exactly what a DOS program sees before it calls AH=4Ah.
       On a WOW launch there is no DOS program -- the image loaded above is discarded
       -- so the block can go back to just the PSP. Doing this through DosMcbResize()
       rather than poking the chain keeps the MCB invariants (and DosMcbCheckChain) true. */
    {   WORD pspMaximum = 0;
        INT resizeResult = DosMcbResize(NULL, DOS_PSP_SEG, WOW_PSP_BLOCK_PARAS, &pspMaximum);
        cursor = message;
        cursor = LogPut(cursor, "WOWV86: shrink PSP block to 0x40 paras -> rc=");
        cursor = LogHex(cursor, (DWORD)resizeResult); cursor = LogPut(cursor, " max=0x"); cursor = LogHex(cursor, pspMaximum);
        cursor = LogPut(cursor, "\r\n"); LogAppend(LDTLOG_PATH, message, cursor);
    }

    /* ── THE PM->V86 TRANSFER BUFFER, BEFORE krnl386 TAKES THE REST. ───────────────
         krnl386 chains its declined INT 21h file calls to real DOS, and those calls
         carry protected-mode pointers the V86 DOS layer cannot resolve. PmInt21Transfer()
         copies through this block. It is claimed HERE, between the shrink and
         krnl386's own segments, because after those allocations krnl386 owns the arena
         -- and it is a plain DOS allocation so the MCB chain (and DosMcbCheckChain) stay
         true. 16 KB because that is the largest read krnl386 asks for; a bigger request
         is clamped and SAID SO rather than silently short-read. */
    /* ⚠ THE HOST POOL GOES FIRST, i.e. as LOW as possible. krnl386 builds a 64 KB
         scratch selector over `base(SS) + SP` (observed) -- a window over everything
         above its own stack -- and uses it as a working block. Allocated late, the pool
         landed at 0x1ab9, INSIDE that window, so the host's own PM stub table appeared
         in the middle of krnl386's scratch and was read back as an NE header. Host
         memory belongs below the guest's, not in the middle of it. */
    {   WORD hostPoolSegment = 0, hostPoolMaximum = 0;
        if (DosMcbAllocate(NULL, machine->FirstMcb, WOW_HOSTPOOL_PARAS, &hostPoolSegment, &hostPoolMaximum) == 0 && hostPoolSegment)
            g_WowPoolSegment = hostPoolSegment;
        cursor = message; cursor = LogPut(cursor, "WOWV86: host pool reserved at para 0x");
        cursor = LogHex(cursor, g_WowPoolSegment); cursor = LogPut(cursor, " size 0x");
        cursor = LogHex(cursor, (DWORD)WOW_HOSTPOOL_PARAS);
        cursor = LogPut(cursor, " paras (see wow_host_alloc)\r\n");
        LogAppend(LDTLOG_PATH, message, cursor);
    }
    /* ── ★ THE SFT CHAIN, OR krnl386 WALKS THE IVT FOREVER. (GH #128) ─────────────
         See the DOS_SFT_* block in dos_layout.h for what this is built against.
         One block, `next` = FFFF:FFFF so the walk terminates, and an entry
         count equal to what our INT 21h layer can really open. The entries
         themselves are zeroed -- which is not a stub, it is what a free SFT entry
         looks like; nothing has been opened yet at this point in the bootstrap.
       ▸ WOW-only on purpose. It comes out of the host pool because by here krnl386
         owns every remaining paragraph (see WowHostAllocate), and a DOS guest still
         gets SysVars+4 = 0 -- unchanged, and the reason no DOS guest has ever been
         affected by this. When a DOS program needs the SFT, this moves. */
    {   WORD sft = WowHostAllocate(DOS_SFT_PARAS);
        volatile BYTE *sysVars = (volatile BYTE *)(ULONG_PTR)
                            (((DWORD)DOS_SYSVARS_SEG << PARAGRAPH_SHIFT) + DOS_SYSVARS_OFF);
        cursor = message; cursor = LogPut(cursor, "WOWV86: SFT ");
        if (!sft) {
            cursor = LogPut(cursor, "NOT ALLOCATED -- host pool exhausted; krnl386 will spin on "
                        "SysVars+4 = 0000:0000\r\n");
        } else {
            volatile BYTE *sftBlock = (volatile BYTE *)(ULONG_PTR)((DWORD)sft << PARAGRAPH_SHIFT);
            UINT byteIndex;
            for (byteIndex = 0; byteIndex < (UINT)DOS_SFT_BYTES; ++byteIndex) sftBlock[byteIndex] = 0;
            *(volatile WORD *)(sftBlock + DOS_SFT_NEXT_OFFSET) = DOS_SFT_LAST;              /* next offset: last block */
            *(volatile WORD *)(sftBlock + DOS_SFT_NEXT_SEGMENT) = DOS_SFT_LAST;              /* next segment            */
            *(volatile WORD *)(sftBlock + DOS_SFT_COUNT) = DOS_SFT_ENTRIES;     /* entries in this block   */
            *(volatile WORD *)(sysVars + DOS_SYSVARS_SFT) = 0;                  /* SysVars+4 = offset      */
            *(volatile WORD *)(sysVars + DOS_SYSVARS_SFT + X86_FAR_POINTER_SEGMENT) = sft;                /* SysVars+6 = segment     */
            cursor = LogPut(cursor, "at 0x"); cursor = LogHex(cursor, sft);
            cursor = LogPut(cursor, ":0000, "); cursor = LogHex(cursor, (DWORD)DOS_SFT_ENTRIES);
            cursor = LogPut(cursor, " entries, next=FFFF:FFFF -> SysVars+4\r\n");
        }
        LogAppend(LDTLOG_PATH, message, cursor);
    }
    {   WORD transferSegment = 0, transferMaximum = 0;
        if (DosMcbAllocate(NULL, machine->FirstMcb, PM_TRANSFER_PARAGRAPHS, &transferSegment, &transferMaximum) == 0 && transferSegment) {
            g_PmTransferSegment = transferSegment; g_PmTransferParagraphs = PM_TRANSFER_PARAGRAPHS;
        }
        cursor = message;
        cursor = LogPut(cursor, "WOWV86: PM->V86 transfer buffer at para 0x"); cursor = LogHex(cursor, g_PmTransferSegment);
        cursor = LogPut(cursor, " (0x"); cursor = LogHex(cursor, (DWORD)g_PmTransferParagraphs * PARAGRAPH_SIZE_U);
        cursor = LogPut(cursor, " bytes; largest free was 0x"); cursor = LogHex(cursor, transferMaximum);
        cursor = LogPut(cursor, ")\r\n"); LogAppend(LDTLOG_PATH, message, cursor);
    }
    /* ── ★ A SEPARATE PARAGRAPH FOR RESOLVED MODULE PATHS (WOW32 0xc5). ─────────
         Deliberately NOT the transfer buffer above. 0xc5 hands krnl386 a far
         pointer that it keeps across a resolve/release window -- and inside that
         window it opens the file, which is a PM->V86 INT 21h, which reuses the
         transfer buffer. Sharing them would hand the guest a pointer to a path
         that the very next call overwrites, and the corruption would surface as
         a wrong filename somewhere far from here. One paragraph, one purpose. */
    {   WORD pathSegment = 0, pathMaximum = 0;
        if (DosMcbAllocate(NULL, machine->FirstMcb, WOW_PATH_PARAS, &pathSegment, &pathMaximum) == 0 && pathSegment)
            g_WowPathSegment = pathSegment;
        cursor = message;
        cursor = LogPut(cursor, "WOWV86: module-path scratch at para 0x"); cursor = LogHex(cursor, g_WowPathSegment);
        cursor = LogPut(cursor, " (0x200 bytes; largest free was 0x"); cursor = LogHex(cursor, pathMaximum);
        cursor = LogPut(cursor, ")\r\n"); LogAppend(LDTLOG_PATH, message, cursor);
    }
    /* ── ★ AND ONE FOR A LAUNCHED TASK'S ENVIRONMENT (WOW32 seg2 0xd1). ────────
         Allocated here, with the others, because after this function hands the
         arena to krnl386 there is nothing left to allocate FROM -- see
         WowHostAllocate. Separate again: this one is held by the child for its
         whole life, and both buffers above are reused on the very next call. */
    {   WORD environmentSegment = 0, environmentMaximum = 0;
        if (DosMcbAllocate(NULL, machine->FirstMcb, WOW_ENV_PARAS, &environmentSegment, &environmentMaximum) == 0 && environmentSegment)
            g_WowEnvironmentSegment = environmentSegment;
        cursor = message;
        cursor = LogPut(cursor, "WOWV86: task-environment block at para 0x"); cursor = LogHex(cursor, g_WowEnvironmentSegment);
        cursor = LogPut(cursor, " (0x"); cursor = LogHex(cursor, (DWORD)WOW_ENV_PARAS * PARAGRAPH_SIZE_U);
        cursor = LogPut(cursor, " bytes; largest free was 0x"); cursor = LogHex(cursor, environmentMaximum);
        cursor = LogPut(cursor, ")\r\n"); LogAppend(LDTLOG_PATH, message, cursor);
    }
    /* ── ★★★ THREE BYTES THAT LET THE HOST CALL 16-BIT CODE. (session 40) ──────
         The whole of the return path is `C4 C4 57`: we push its address as the
         far return address of a window procedure, the procedure's own `retf`
         lands on it, and the BOP arrives here like any other. It needs a
         paragraph of its own -- not a corner of one of the buffers above -- for
         the plainest reason there is: a callback can be in flight across any
         number of other calls, and both buffers above are overwritten by the
         next one. See src/wow/wowcall.h.
       ⚠ The SELECTOR is not made here. This runs before the guest exists, and a
         code descriptor over it is only useful once there is an LDT to put it
         in, so it is built at the first callback and cached. */
    {   WORD callStubSegment = 0, callStubMaximum = 0;
        if (DosMcbAllocate(NULL, machine->FirstMcb, 1, &callStubSegment, &callStubMaximum) == 0 && callStubSegment) {
            volatile BYTE *callStub = (volatile BYTE *)(ULONG_PTR)((DWORD)callStubSegment << PARAGRAPH_SHIFT);
            g_WowCallbackSegment = callStubSegment;
            g_WowCallbackLinear = (DWORD)callStubSegment << PARAGRAPH_SHIFT;
            callStub[0] = VDM_BOP0; callStub[1] = VDM_BOP1; callStub[VDM_BOP_NUMBER_OFFSET] = WOWCALL_BOP_CODE;
            /* ★ And the RETF trampoline, four bytes along -- the paragraph has
                 sixteen and we were using three. See WOWCALL_RETF_OFF: it is how
                 a call into a NOT-YET-LOADED code segment gets that segment
                 loaded, by faulting the way the guest's own far calls do. */
            callStub[WOWCALL_RETF_OFF] = WOWCALL_RETF_BYTE;
        }
        cursor = message;
        cursor = LogPut(cursor, "WOWV86: 16-bit callback return stub at para 0x");
        cursor = LogHex(cursor, g_WowCallbackSegment);
        cursor = LogPut(cursor, " (C4 C4 "); cursor = LogHexByte(cursor, WOWCALL_BOP_CODE);
        cursor = LogPut(cursor, "; largest free was 0x"); cursor = LogHex(cursor, callStubMaximum);
        cursor = LogPut(cursor, ")\r\n"); LogAppend(LDTLOG_PATH, message, cursor);
    }

    for (index = 0; index < (INT)module->SegmentCount; ++index) {
        NE_SEGMENT *neSegment = &module->Segments[index];
        UINT32 need = NeSegmentAllocSize(neSegment), byteIndex;
        WORD segment = 0, maximum = 0;
        volatile BYTE *destination;
        /* Room for the relocation records copied in below -- krnl386 reads them back
           out of the loaded segment, so they are part of what has to be resident. */
        if (neSegment->Sector && (neSegment->Flags & NE_SEG_RELOCS)) {
            UINT32 relocationOffset = neSegment->FileOffset + neSegment->Length;
            if (relocationOffset + NE_RELOC_COUNT_SIZE <= module->ImageLength) {
                UINT32 relocationBytes = NE_RELOC_COUNT_SIZE + (UINT32)(image[relocationOffset] | (image[relocationOffset + 1] << BYTE_SHIFT)) * NE_RELOC_SIZE;
                if (neSegment->Length + relocationBytes > need) need = neSegment->Length + relocationBytes;
            }
        }
        /* ── DGROUP IS BIGGER THAN THE SEGMENT IN THE FILE. ────────────────────────
             For the automatic data segment a Win16 loader allocates the segment's
             length PLUS ne_heap PLUS ne_stack -- the local heap and stack live above
             the initialised data, and nothing in the segment table says so.
             NeSegmentAllocSize() only knows about the segment, so the header's own
             fields have to be added here. krnl386 asks for heap 0x200 and got none,
             which left its DGROUP exactly as large as its initialised data: any local
             allocation would have run off the end of the segment. */
        if (module->AutoData && index == (INT)module->AutoData - 1) {
            UINT32 dgroupSize = need + module->Heap + module->Stack;
            if (dgroupSize > X86_SEGMENT_SIZE_U) dgroupSize = X86_SEGMENT_SIZE_U;
            if (dgroupSize > need) {
                cursor = message;
                cursor = LogPut(cursor, "WOWV86: DGROUP (seg "); cursor = LogHex(cursor, (DWORD)(index + 1));
                cursor = LogPut(cursor, ") 0x");        cursor = LogHex(cursor, need);
                cursor = LogPut(cursor, " + heap 0x");  cursor = LogHex(cursor, module->Heap);
                cursor = LogPut(cursor, " + stack 0x"); cursor = LogHex(cursor, module->Stack);
                cursor = LogPut(cursor, " = 0x");       cursor = LogHex(cursor, dgroupSize);
                cursor = LogPut(cursor, "\r\n"); LogAppend(LDTLOG_PATH, message, cursor);
                need = dgroupSize;
            }
        }
        if (DosMcbAllocate(NULL, machine->FirstMcb, (WORD)((need + PARAGRAPH_LAST_BYTE) >> PARAGRAPH_SHIFT), &segment, &maximum) || !segment) {
            cursor = message; cursor = LogPut(cursor, "WOWV86: no conventional memory for seg ");
            cursor = LogHex(cursor, (DWORD)(index + 1)); cursor = LogPut(cursor, ", largest free 0x"); cursor = LogHex(cursor, maximum);
            cursor = LogPut(cursor, " paras\r\n"); LogAppend(LDTLOG_PATH, message, cursor);
            return -1;
        }
        destination = (volatile BYTE *)(ULONG_PTR)((DWORD)segment << PARAGRAPH_SHIFT);
        for (byteIndex = 0; byteIndex < need; ++byteIndex) destination[byteIndex] = 0;
        if (neSegment->Sector) for (byteIndex = 0; byteIndex < neSegment->Length; ++byteIndex) destination[byteIndex] = image[neSegment->FileOffset + byteIndex];
        /* ── ★ AND THE RELOCATION RECORDS, WHICH LIVE AFTER THE SEGMENT DATA. ──────
             An NE segment with NE_SEG_RELOCS is followed in the FILE by a WORD count
             and that many 8-byte records. A conventional loader applies them and
             throws them away -- ours does too (NeApplyRelocations reads them straight
             out of the image). But krnl386 relocates its own copy AGAIN, reading the
             count and the records from the LOADED SEGMENT in memory, just past its
             `length` bytes (observed). With only `length` bytes copied it was reading
             whatever followed, decoding it as relocation records, and taking them for
             IMPORTED fixups -- which sent it into the module-reference table of a
             module that imports from nothing, so the import lookup failed and
             LoadSegment failed. Bracketed with breakpoints; see session 31 part 20.
           So copy them too, and size the block to hold them. */
        if (neSegment->Sector && (neSegment->Flags & NE_SEG_RELOCS)) {
            UINT32 relocationOffset = neSegment->FileOffset + neSegment->Length;
            if (relocationOffset + NE_RELOC_COUNT_SIZE <= module->ImageLength) {
                UINT32 relocationCount = (UINT32)(image[relocationOffset] | (image[relocationOffset + 1] << BYTE_SHIFT));
                UINT32 relocationBytes   = NE_RELOC_COUNT_SIZE + relocationCount * NE_RELOC_SIZE;
                if (relocationOffset + relocationBytes <= module->ImageLength && neSegment->Length + relocationBytes <= need)
                    for (byteIndex = 0; byteIndex < relocationBytes; ++byteIndex) destination[neSegment->Length + byteIndex] = image[relocationOffset + byteIndex];
                cursor = message;
                cursor = LogPut(cursor, "WOWV86:   + "); cursor = LogHex(cursor, relocationCount);
                cursor = LogPut(cursor, " relocation records ("); cursor = LogHex(cursor, relocationBytes);
                cursor = LogPut(cursor, " bytes) at segment offset 0x"); cursor = LogHex(cursor, neSegment->Length);
                cursor = LogPut(cursor, (neSegment->Length + relocationBytes <= need) ? "\r\n"
                                                     : " -- ⚠ DOES NOT FIT\r\n");
                LogAppend(LDTLOG_PATH, message, cursor);
            }
        }
        neSegment->Memory = (BYTE *)(ULONG_PTR)((DWORD)segment << PARAGRAPH_SHIFT);   /* relocate in place */
        neSegment->Selector = segment;                                        /* a PARAGRAPH now     */

        cursor = message;
        cursor = LogPut(cursor, "WOWV86: seg ");   cursor = LogHex(cursor, (DWORD)(index + 1));
        cursor = LogPut(cursor, (neSegment->Flags & NE_SEG_DATA) ? " DATA" : " CODE");
        cursor = LogPut(cursor, " len=0x");        cursor = LogHex(cursor, neSegment->Length);
        cursor = LogPut(cursor, " alloc=0x");      cursor = LogHex(cursor, need);
        cursor = LogPut(cursor, " -> para 0x");    cursor = LogHex(cursor, segment);
        cursor = LogPut(cursor, " (linear 0x");    cursor = LogHex(cursor, (DWORD)segment << PARAGRAPH_SHIFT);
        cursor = LogPut(cursor, ")\r\n"); LogAppend(LDTLOG_PATH, message, cursor);
    }

    /* ── ★★★ WE DO NOT RELOCATE krnl386. IT RELOCATES ITSELF, AND ONLY IT CAN. ──────
         This is the reversal of the obvious thing, so here is the whole argument.

       ★ A CHAINED NE FIXUP CAN ONLY BE APPLIED ONCE. A non-additive record names one
         site; the word AT that site is the offset of the next site, 0xFFFF ending it.
         Segment 1's four records expand to 80 sites that way (3 -> seg 1, 58 -> seg 2,
         8 -> seg 3, 11 -> seg 4). Applying them WRITES the target over the link, so
         the chain is gone and a second pass is not "wasteful", it is impossible.

       ★★ AND krnl386 RUNS THAT SECOND PASS ON PURPOSE, BECAUSE IT IS THE PASS THAT
         MATTERS. Its bring-up calls LoadSegment on its OWN segments (segments 1 and 4
         observed), and each fixup resolves to the target segment's HANDLE in the
         in-memory segment table, i.e. a PROTECTED-MODE SELECTOR. We enter krnl386 in V86 where
         a segment is a
         paragraph; it switches itself to PM, moves segment 1 to linear 0x20760 and
         builds a code selector (0x0207) over it. Every far reference inside that copy
         has to become a selector, and LoadSegment is what does it.

       ⚠ MEASURED, AND IT IS WHAT REFUTED THE EASY FIX. Clearing NE_SEG_RELOCS in the
         placed header makes LoadSegment skip the pass and "succeed": the run cleared
         the ExitKernelThunk(1) wall and got 10 PM steps further, into extended-memory
         setup. But at step 0x44, code executing from selector 0x0207 pushed its own
         far pointers as `0x0643:0x4ff2` and `0x18e2:0x024a` -- OUR PARAGRAPHS, in
         protected mode, where 0x0643 and 0x18e2 are not valid selectors. The image
         had been moved to PM with real-mode fixups baked in, and the 58 far calls to
         segment 2 were all pointed at a paragraph. That run went further while being
         MORE wrong, which is the reason this note is long.

       ⇒ So the loader's job here is to LOAD, not to relocate: the segment bytes and
         the relocation records go into memory verbatim (above), the header describes
         them (below), and krnl386 does the one pass that is legal to do. Entry CS/DS/SS
         are handed over directly and are not fixups, so nothing in the entry path
         depends on this.

       ⚠ NeApplyRelocations is UNCHANGED and still used for the selector-stage load and by
         all 209 NE checks; it is only this V86 entry stage that must not run it. */
    module->Sites = 0;
    cursor = message;
    cursor = LogPut(cursor, "WOWV86: NOT relocating -- the chains are left intact for krnl386's own "
                "LoadSegment pass, which resolves to SELECTORS (see the note here)\r\n");
    LogAppend(LDTLOG_PATH, message, cursor);

    /* ⚠ ONE BLOCK FOR BOTH, because DOS puts a one-paragraph MCB header between any
         two allocations and krnl386 needs the header image to be EXACTLY at
         `base(SS) + 0x1000`. Allocated separately they came out one paragraph apart
         and the log said so ("NOT ADJACENT") rather than leaving it to be discovered
         downstream. Stack occupies [0, 0x1000); the NE header image follows it. */
    /* ── ★★★ PUT THE STACK + WINDOW AT THE **TOP** OF CONVENTIONAL MEMORY. ─────────
         krnl386's conventional arena is the region from `base(SS) + SP` to the 640 KB
         line -- measured: its arena block is base=0x1bbe0 limit=0x8441f, i.e. its DATA
         starts at exactly our header image and runs to 0xA0000. That is 542 KB, so
         segment 1 (0xd7fa bytes) fits in it comfortably, which is why krnl386 puts its
         own code copy at 0x2c760 -- INSIDE the block it later compacts across (part 14).
       ⚠ Neither the arena size we declare (part 15) nor the size of the PSP block we
         build (part 16) moves that: the block reaches 0xA0000 because 0xA0000 is where
         conventional memory ends, not because of anything we hand over. The ONE input we
         control is WHERE `base(SS) + SP` is.
       ⇒ So allocate this block high. With the window at the top, the arena is the window
         itself -- 64 KB, of which 48 KB is free after the header image -- and 48 KB is
         SMALLER THAN SEGMENT 1. krnl386 then cannot place its code there and must use the
         0x88080-byte global heap it already VirtualAlloc'd at 0x03a70000, which no
         conventional compaction can reach.
       DOS has no "allocate high" here, so do it the way a DOS program would: take a
         filler that leaves exactly this block's worth at the top, allocate, free the
         filler. The PSP block below then takes the freed region, as before. */
    {   WORD fillerSegment = 0, fillerMaximum = 0, want;
        (VOID)DosMcbAllocate(NULL, machine->FirstMcb, DOS_MCB_LARGEST_REQUEST, &fillerSegment, &fillerMaximum);   /* ask -> largest free */
        want = (WORD)(WOW_STACK_PARAS + windowParas);
        if (fillerMaximum > want + WOW_FILLER_SLACK_PARAS) {
            /* -1 for the MCB header DOS puts in front of the second allocation: without
               it the filler eats the paragraph the stack+window block needs and the
               allocation fails outright ("no memory for the stack + header image"). */
            WORD fill = (WORD)(fillerMaximum - want - 1);
            if (DosMcbAllocate(NULL, machine->FirstMcb, fill, &fillerSegment, &fillerMaximum) == 0 && fillerSegment) {
                cursor = message; cursor = LogPut(cursor, "WOWV86: filler 0x"); cursor = LogHex(cursor, fill);
                cursor = LogPut(cursor, " paras at 0x"); cursor = LogHex(cursor, fillerSegment);
                cursor = LogPut(cursor, " so the stack+window lands HIGH (freed again below)\r\n");
                LogAppend(LDTLOG_PATH, message, cursor);
            } else fillerSegment = 0;
        } else fillerSegment = 0;
        if (DosMcbAllocate(NULL, machine->FirstMcb, (WORD)(WOW_STACK_PARAS + windowParas),
                      &stackBlockSegment, &stackBlockMaximum) || !stackBlockSegment) stackBlockSegment = 0;
        if (fillerSegment) DosMcbFree(NULL, fillerSegment);          /* give the low region back */
    }
    if (!stackBlockSegment) {
        cursor = message; cursor = LogPut(cursor, "WOWV86: no memory for the stack + header image\r\n");
        LogAppend(LDTLOG_PATH, message, cursor); return -1;
    }
    /* ZERO IT. A loader hands out clean memory, and here it is load-bearing rather
       than tidy: krnl386's scratch selector starts inside this block, and whatever
       was left in it is read back as structured data. Uninitialised memory that gets
       PARSED is a bug that reads like a guest fault. */
    {   volatile BYTE *stackBytes = (volatile BYTE *)(ULONG_PTR)((DWORD)stackBlockSegment << PARAGRAPH_SHIFT);
        DWORD byteIndex; for (byteIndex = 0; byteIndex < (DWORD)WOW_STACK_PARAS * PARAGRAPH_SIZE_U; ++byteIndex) stackBytes[byteIndex] = 0; }

    /* ── ★ THE NE HEADER GOES IMMEDIATELY ABOVE THE STACK. ─────────────────────────
         krnl386 builds a selector over `base(SS) + SP` (observed in the LDT) and parses
         the KERNEL EXE's NE header through it -- the documented NE fields (`ne_enttab`,
         `ne_cseg`, ...) are read at their NE offsets from that selector's base.
         NOTHING in its bring-up reads that header from disk: its only file call there
         is an OpenFile(..., OF_EXIST), and the selector is never replaced (both
         measured). So the LOADER has to put the header there.

       ★ AND THE ADDRESS IS FIXED, WHICH IS WHAT MAKES THIS POSSIBLE. Measured on the
         rig: SS:SP is 0x1f:0x0FFE at three breakpoints across its bring-up -- the entry SP,
         unchanged, because krnl386's calls up to that point are balanced. (An earlier
         reading of a different layout suggested the base was call-depth dependent and
         therefore unplaceable; it was not, and the three-point measurement is what
         settled it.) So `base(SS) + SP` is the top of this stack block, and the header
         belongs in the paragraph immediately after it -- with SP entering at the very
         top rather than top-2, so the two coincide exactly.

       ⚠ Copy from the NE HEADER, not from the start of the file: every table offset in
         an NE (`enttab`, `segtab`, `rsrctab`, `restab`, `modtab`, `imptab`) is relative
         to the header, so the header must land at offset 0 of the selector for any of
         them to resolve. */
    {   WORD hostPoolSegment = (WORD)(stackBlockSegment + WOW_STACK_PARAS);      /* same block, no MCB between */
        DWORD loadLength = module->ImageLength > module->Header ? module->ImageLength - module->Header : 0;
        volatile BYTE *headerBytes = (volatile BYTE *)(ULONG_PTR)((DWORD)hostPoolSegment << PARAGRAPH_SHIFT);
        DWORD byteIndex;
        /* Zero the WHOLE block, not just what the image fills: anything the image
           does not cover is krnl386's scratch, it is parsed as structured data, and
           uninitialised memory that gets parsed is a bug that reads like a guest
           fault. Then stage the file -- ALL of it; see the note on hdrimg_paras. */
        for (byteIndex = 0; byteIndex < (DWORD)windowParas * PARAGRAPH_SIZE_U; ++byteIndex) headerBytes[byteIndex] = 0;
        for (byteIndex = 0; byteIndex < loadLength; ++byteIndex) headerBytes[byteIndex] = image[module->Header + byteIndex];

        /* ── ⚠ DO NOT WIDEN THE SEGMENT TABLE HERE. TRIED, MEASURED, REFUTED. ──────
             krnl386's in-memory segment table has TEN-byte entries, the handle at +8 --
             the two bytes the file's 8-byte entry does not have. Stock's KERNEL module
             database shows it (stock-VDM dump): at linear
             0x196c0, read with stride 10, it reproduces krnl386's file segment table
             sector-for-sector and carries 0x01ff / 0x0206 / 0x020e / 0x0217 at +8 --
             its four segment selectors.

           ⇒ The obvious conclusion is that the LOADER must widen the entries. It is
             wrong, and one run said so. krnl386 BUILDS ITS OWN MODULE DATABASE from the
             header we stage: it allocates a block, copies the header in, and widens the
             table itself. The number that proves it is the selector limit --

                 without widening   idx 0x38 base=0x0001ad00 limit=0x00000a5f
                 stock              sel 0x01f7 base=0x000196c0 limit=0x00000a5f  ★ same
                 with widening      idx 0x38 base=0x0001ad00 limit=0x00000a7f

             -- our module block is already byte-identical in size to stock's, and
             pre-widening made krnl386 widen an already-widened table: the run died
             EARLIER (PM step 0x32 against 0x4f) with no error message at all.

             Same shape as the relocation lesson above: the loader's job is to stage the
             file's header, not to pre-chew it. */
        /* ⚠ NE_SEG_RELOCS IS LEFT SET HERE, AND THAT IS A CORRECTION. Clearing it in
             this copy makes LoadSegment return success without relocating (measured)
             -- which DID clear the
             ExitKernelThunk(1) wall and is NOT the right answer; see the refutation
             above NeApplyRelocations's call site. Recorded so it is not re-tried. */

        cursor = message;
        cursor = LogPut(cursor, "WOWV86: NE header image at para 0x"); cursor = LogHex(cursor, hostPoolSegment);
        cursor = LogPut(cursor, " = SS 0x");  cursor = LogHex(cursor, stackBlockSegment);
        cursor = LogPut(cursor, " + 0x1000 (0x"); cursor = LogHex(cursor, loadLength);
        cursor = LogPut(cursor, " bytes from file offset 0x"); cursor = LogHex(cursor, module->Header);
        cursor = LogPut(cursor, ", ne_cseg=0x"); cursor = LogHex(cursor, module->SegmentCount);
        cursor = LogPut(cursor, ")\r\n  staged 0x"); cursor = LogHex(cursor, loadLength);
        cursor = LogPut(cursor, " of 0x");            cursor = LogHex(cursor, imageLength);
        cursor = LogPut(cursor, " bytes = 0x");       cursor = LogHex(cursor, (DWORD)windowParas);
        cursor = LogPut(cursor, " paras");
        cursor = LogPut(cursor, loadLength == imageLength ? " (WHOLE FILE)\r\n" : " -- ⚠ STILL TRUNCATED\r\n");
        LogAppend(LDTLOG_PATH, message, cursor);

        /* Say where each segment's image landed inside the staged copy: that is the
           address krnl386's own LoadSegment has to be able to reach, and the old
           truncation was INVISIBLE in the line above, which only ever printed the
           already-clamped length. A number that cannot show the fault it is there to
           catch is not an instrument. */
        for (index = 0; index < (INT)module->SegmentCount; ++index) {
            UINT32 fileOffset = module->Segments[index].FileOffset;
            INT resident = fileOffset >= module->Header && (DWORD)(fileOffset - module->Header) < loadLength;
            cursor = message;
            cursor = LogPut(cursor, "WOWV86:   seg "); cursor = LogHex(cursor, (DWORD)(index + 1));
            cursor = LogPut(cursor, " file 0x");       cursor = LogHex(cursor, fileOffset);
            cursor = LogPut(cursor, " -> staged linear 0x");
            cursor = LogHex(cursor, ((DWORD)hostPoolSegment << PARAGRAPH_SHIFT) + (fileOffset - module->Header));
            cursor = LogPut(cursor, resident ? " RESIDENT\r\n" : " ⚠ NOT RESIDENT\r\n");
            LogAppend(LDTLOG_PATH, message, cursor);
        }

        cursor = message;
        /* ── ★★★ CX IS THE WHOLE WINDOW, NOT "THE PART ABOVE THE HEADER". ────────────
             This used to subtract the header image, on the reading that the header is
             not krnl386's to allocate from. That reading is what kills the run:

                 0x0bff   the arena size we declare, in paragraphs (CX >> 4)
                 0x0f88   the size of the selector krnl386 actually built (LDT)
                 0x0389   the difference -- i.e. OUR HEADER IMAGE

             krnl386 treats that difference as dead space at the BOTTOM of its arena and
             reclaims it by block-copying everything above it DOWN by 0x389 paragraphs.
             Measured at a breakpoint on the copy: ECX=0x202e0 dwords (514 KB)
             through selector 0x01b7 (base 0x1bbe0, limit 0x8441f, i.e. up to the 640 KB
             line). The copy is entirely in bounds -- and its destination range
             0x1bbe0..0x9c760 CONTAINS krnl386's own segment-1 PM copy at 0x2c760, the
             code executing the copy. It overwrites itself mid-instruction: no
             fault to reflect, no crash record, no surviving thread.

           ⇒ Declaring the FULL window makes total == free, so the gap is zero and the
             reclaim becomes a copy of zero bytes instead of a fatal one. The space is
             krnl386's either way -- reclaiming it is precisely what it was trying to do;
             we were just describing it in a way that made the reclaim run over live code.
           ⚠ The header image still has to survive being PARSED before anything is
             allocated over it. If that turns out to be the next wall it will show up as
             a header-parse failure, which is a different and much louder failure than
             this one. */
        /* ⚠ 0xFFF0 (the full window) was tried and OVERSHOOTS: krnl386's own size came
             back 0x0f88 paragraphs in BOTH runs -- it is computed from its arena, not
             from CX -- so declaring 0x0fff made the gap NEGATIVE (-0x77 paragraphs)
             and the copy's source addressed 0xFF890, far outside the selector. A
             negative gap is not a smaller bug than a positive one, it is a wilder one.
           So declare exactly what it believes it has: gap = its size - ours = 0,
             which makes the reclaim copy src==dst and therefore harmless. */
        /* ── ★★★ THE GAP IS NOT WASTE. IT IS HOW THE SEGMENT IMAGES ADVANCE. ────────
             Session 33 part 11. The note above declares the FULL window so the gap is
             zero and krnl386's reclaim copies nothing -- which fixed a crash and broke
             the load, because that reclaim is the mechanism that walks the staged image
             (observed per segment: compute the gap, copy everything above it DOWN, then
             LoadSegment copies the segment from the bottom of the block).

             With the gap zero the block never advances, so every segment is copied from
             OFFSET ZERO of the staged image -- which is the NE header. Measured directly
             at a breakpoint on LoadSegment's copy: the source base was 0x00089bc0, and
             our staged header is at para 0x89bc.
             LoadSegment(2) was copying 0x3ee2 bytes of header and calling it segment 2.

           ⇒ Declare the window MINUS the header and tables, so the first reclaim
             discards exactly them and segment 1's image lands at offset 0; krnl386's own
             bookkeeping then advances the block by each segment it consumes.
           ⚠ The crash that motivated gap=0 was the reclaim's destination range containing
             krnl386's own PM code copy. That was with the block spanning the whole arena
             (base 0x1bbe0, limit 0x8441f). It no longer does: the block is the staged
             image (0x89bc0 + 0xf880), and the PM copy is at 0x1ad00 -- outside it. */
        /* ⚠ AND THE GAP IS MEASURED TO THE FIRST SEGMENT THE **LOOP** LOADS, WHICH IS
             SEGMENT 2. Segment 1 is loaded earlier, separately and without the
             compaction step, so it never consumes from the staged block: with the gap
             set to the header alone, the loop's first iteration copied segment 1's image
             and called it segment 2 (measured -- `@ds:si` was seg1+0x4a). krnl386's own
             per-segment bookkeeping advances it after that. */
        {   DWORD first = module->SegmentCount > 1 ? module->Segments[1].FileOffset
                                        : module->Segments[0].FileOffset;
            DWORD gap   = (first > module->Header) ? ((first - module->Header) + PARAGRAPH_LAST_BYTE_U) & ~PARAGRAPH_LAST_BYTE_U : 0;
            DWORD full  = WOW_ENTRY_CX_FULL_U;
            g_WowEntryCx = (WORD)(gap < full ? full - gap : full);
            cursor = LogPut(cursor, "WOWV86: arena gap = 0x"); cursor = LogHex(cursor, gap);
            cursor = LogPut(cursor, " bytes (the header and tables, header+0x0 .. +0x");
            cursor = LogHex(cursor, first - module->Header);
            cursor = LogPut(cursor, ") -- the first reclaim discards them so segment 1 lands at "
                        "offset 0\r\n");
            LogAppend(LDTLOG_PATH, message, cursor);
            cursor = message;
        }
        cursor = LogPut(cursor, "WOWV86: arena declared to krnl386 = 0x"); cursor = LogHex(cursor, g_WowEntryCx);
        cursor = LogPut(cursor, " bytes -> CX at entry");
        /* ⚠ UNCHANGED WHILE THE STAGED IMAGE GREW, DELIBERATELY -- one change per run.
             It is now a claim about memory the staged image also occupies (the image is
             0x16440 bytes and this declares 0xf880 of scratch above the header), so if
             the run gets past LoadSegment and then dies in the arena, THIS is the next
             thing to look at, not a new mystery. Stock's arena is a separate block
             below its staged image (session 33 part 7). */
        cursor = LogPut(cursor, (DWORD)g_WowEntryCx > loadLength ? "\r\n"
                                                 : " -- ⚠ OVERLAPS THE STAGED IMAGE\r\n");
        LogAppend(LDTLOG_PATH, message, cursor);
    }

    if (!module->AutoData || module->AutoData > module->SegmentCount) {
        cursor = message; cursor = LogPut(cursor, "WOWV86: autodata segment out of range\r\n");
        LogAppend(LDTLOG_PATH, message, cursor); return -1;
    }

    /* ── ★ krnl386 CARVES FROM `ES + 0x10` AND NEVER ASKS DOS. ────────────────────
         Observed at its DPMI bring-up: after `INT 2Fh AX=1687h` it places the DPMI
         host's private data block at the paragraph ES + 0x10 (ES as we entered it),
         then calls the mode-switch entry -- and everything it allocates afterwards
         grows upward from there, WITHOUT a single INT 21h AH=48h. So whatever sits above `ES +
         0x10` is memory krnl386
         believes is its own.

       ⚠ WE WERE PUTTING ITS OWN CODE THERE. Entered with ES = DOS_PSP_SEG (0x100),
         krnl386 carved from paragraph 0x110 — and DosMcbAllocate had already handed out
         0x141 (the transfer buffer) and 0x542, 0x12c3, 0x16b3, 0x17dc (krnl386's own
         four segments) out of that same region. Measured, not deduced: the descriptor
         it commits through 04F2 is base=0x1100 limit=0xaf9f, a window that spans its
         own code segment at 0x5420.

       ⇒ So ES must name a block whose NEXT paragraph-plus-0x10 is genuinely free.
         Allocate everything else FIRST (above), then claim ALL remaining conventional
         memory here and hand krnl386 that block. Its own carving then happens inside
         an allocation DOS knows about, so nothing else can be given the same memory —
         which is exactly the relationship a real DOS program has with its PSP block.
       ⚠ It must be a REAL PSP, not a bare block: krnl386 writes PSP+0x42 and reads
         PSP+0x02 (top of memory) during bring-up. DosPspBuild fills both. */
    {   WORD pathSegment = 0, pathMaximum = 0;
        (VOID)DosMcbAllocate(NULL, machine->FirstMcb, DOS_MCB_LARGEST_REQUEST, &pathSegment, &pathMaximum);  /* ask -> get max */
        if (!pathMaximum || DosMcbAllocate(NULL, machine->FirstMcb, pathMaximum, &pathSegment, &pathMaximum) || !pathSegment) {
            cursor = message; cursor = LogPut(cursor, "WOWV86: no arena left for krnl386's PSP block\r\n");
            LogAppend(LDTLOG_PATH, message, cursor); return -1;
        }
        /* ── ★ ZERO THE ARENA. The same rule the header-image block already states,
             applied where it was measurably needed: krnl386 carves everything from
             `ES + 0x10` upward without asking DOS, and it PARSES what it finds there
             as structured data -- so uninitialised memory here is a bug that reads
             like a guest fault, and did.
           ★ MEASURED (session 37): krnl386's own boot task database lands in this
             arena, and `pmchg` says the instance-handle field at `TDB+0x1c` is NEVER
             written for a whole run while `TDB+0x1e` holds `0xFF` from the earliest
             PM events. krnl386 fills only the fields it thinks it needs and leaves
             the rest as whatever the block already held. `GetExePtr` (observed)
             matches `+0x1c` and returns `+0x1e`, so a NULL instance -- which is the
             DOCUMENTED way to ask for a system cursor, and exactly what WOWEXEC's
             `LoadCursor(NULL, IDC_ARROW)` passes -- matched that task and got
             `0xFFFF` back, which USER loaded into ES. Zeroed, the same lookup yields
             `0`, which is a failure the caller already handles.
           ⚠ AND IT DID NOT MOVE THAT WALL, which is worth more than the change is.
             The `pmchg` line said `CHANGED 0x00 -> 0xff`, and a CHANGE is a WRITE --
             read as "the block simply contains it", which was wrong. With the arena
             zeroed the field is still `0xFFFF`, so krnl386 puts it there on purpose
             for its own boot task, and the defect is the OTHER field: `+0x1c`, the
             instance handle, which is never written by anything.
           ▸ Kept anyway, on its own merits and not as a fix: this project has already
             paid for uninitialised memory that gets parsed (see the header-image block
             a few lines up), and a deterministic arena makes the next such bug
             reproducible instead of intermittent.
           ▸ Skips the PSP itself: DosPspBuild lays that down immediately after. */
        {   volatile BYTE *arena = (volatile BYTE *)(ULONG_PTR)((DWORD)pathSegment << PARAGRAPH_SHIFT);
            DWORD arenaBytes = (DWORD)pathMaximum * PARAGRAPH_SIZE_U, index2;
            for (index2 = DOS_PSP_SIZE; index2 < arenaBytes; ++index2) arena[index2] = 0;
            cursor = message; cursor = LogPut(cursor, "WOWV86: arena zeroed, 0x"); cursor = LogHex(cursor, arenaBytes - DOS_PSP_SIZE);
            cursor = LogPut(cursor, " bytes above the PSP\r\n"); LogAppend(LDTLOG_PATH, message, cursor);
        }
        DosPspBuild(NULL, pathSegment, DOS_ENV_SEG, (WORD)(pathSegment + pathMaximum));
        /* ★ AND REBUILD THE ENVIRONMENT, because DosPspBuild ZEROES ITS FIRST
             THREE BYTES -- correct when it is laying down a fresh PSP with a fresh
             env block, destructive here, where the env was already built and is
             shared. It matters more than it looks:
           ★★ krnl386 FINDS ITS OWN EXECUTABLE THROUGH THE ENVIRONMENT -- the
             documented MS-DOS 3.0+ convention: the environment segment at PSP+0x2Ch,
             past the strings to the double NUL, the count WORD, then the full
             pathname of the program. Observed: it is the ONLY way it learns the path. With the
             env wiped, the
             scan walked off into whatever followed and OpenFile got nothing, which is
             reported as "Unable to open KERNEL executable" -- error #3 of its table.
           ⚠ And the path must be KRNL386's, not the Win16 app's: this is krnl386
             asking where IT lives. g_WowKernelPath is the `-a` argument the WOW
             launch already carries. */
        /* ★★ AND THE PATH, because krnl386 SEARCHES IT for the Win16 program.
             `[boot] WOWSHELL` in SYSTEM.INI yields a bare "WOWEXEC.EXE" with no
             directory, and krnl386's search walks `PATH=` out of this very block
             (observed). With the DOS default of `C:\` it can never find it, and says
             so: "Missing 16-bit system module ... WOWEXEC.EXE". Ask the host where
             Windows actually is rather than hardcoding it -- a real WOW launch
             inherits the NT environment, which is exactly these two directories. */
        {   CHAR pathValue[MAX_PATH * 2 + 2]; UINT length;
            /* s89 (#270): ★ AND SYSTEMROOT, which a real WOW VDM inherits from
                 NT's environment. krnl386 builds its SYSTEM directory from it
                 (observed; see WOW32 0x7b): without it, 16-bit
                 GetSystemDirectory answered "\SYSTEM" with no drive. One entry,
                 through `extra`, whose cap note keeps the tail krnl386 reads. */
            CHAR systemRootVariable[MAX_PATH + 16] = HOST_ENV_SYSTEMROOT_ASSIGN;
            if (!GetWindowsDirectoryA(systemRootVariable + sizeof HOST_ENV_SYSTEMROOT_ASSIGN - 1, MAX_PATH)) systemRootVariable[0] = 0;
            length = GetSystemDirectoryA(pathValue, MAX_PATH);
            if (length && length < MAX_PATH) { pathValue[length] = ';';
                if (!GetWindowsDirectoryA(pathValue + length + 1, MAX_PATH)) pathValue[length] = 0; }
            else pathValue[0] = 0;
            /* ⚠ NO dosenv.txt ON THE WOW PATH, deliberately. This block is krnl386's,
                 it is rebuilt after the fact, and the note above records how narrowly
                 it fits: krnl386 finds its own executable by scanning past the strings
                 to the double NUL, so anything added here moves the tail it reads. */
            DosEnvBuildWithCard(NULL, DOS_ENV_SEG,
                          g_WowKernelPath[0] ? g_WowKernelPath
                                             : HOST_DEFAULT_KRNL386_PATH,
                          pathValue[0] ? pathValue : HOST_DEFAULT_WIN16_PATH, &g_SbConfig,
                          systemRootVariable[0] ? systemRootVariable : NULL);
            cursor = message; cursor = LogPut(cursor, "WOWV86: env rebuilt, PATH=");
            cursor = LogPut(cursor, pathValue[0] ? pathValue : "(fallback)");
            cursor = LogPut(cursor, " program path = ");
            cursor = LogPut(cursor, g_WowKernelPath[0] ? g_WowKernelPath : "(default)");
            cursor = LogPut(cursor, "\r\n"); LogAppend(LDTLOG_PATH, message, cursor);
        }
        g_WowPspSegment = pathSegment;
        cursor = message;
        cursor = LogPut(cursor, "WOWV86: krnl386 PSP/arena block at para 0x"); cursor = LogHex(cursor, pathSegment);
        cursor = LogPut(cursor, " size 0x");        cursor = LogHex(cursor, pathMaximum);
        cursor = LogPut(cursor, " paras -> it will carve from 0x"); cursor = LogHex(cursor, (DWORD)(pathSegment + DOS_PSP_PARAGRAPHS));
        cursor = LogPut(cursor, " upward (top 0x");  cursor = LogHex(cursor, (DWORD)(pathSegment + pathMaximum));
        cursor = LogPut(cursor, ")\r\n"); LogAppend(LDTLOG_PATH, message, cursor);
    }

    *entryCs = module->Segments[(module->CsIp >> WORD_SHIFT) - 1].Selector;
    *eip = (WORD)(module->CsIp & WORD_MASK);
    *entryDs = module->Segments[module->AutoData - 1].Selector;
    *entrySs = stackBlockSegment;
    /* ⚠ THE VERY TOP, not top-2. `base(SS) + SP` is what krnl386 turns into its
         header selector, and it must land exactly on the header image placed in the
         next paragraph -- two bytes low and every table offset is two bytes out.
         An empty 16-bit stack conventionally has SP at the top of the segment anyway;
         the -2 was a nicety that is now load-bearing in the wrong direction. */
    *esp = (WORD)(WOW_STACK_PARAS << PARAGRAPH_SHIFT);

    cursor = message;
    cursor = LogPut(cursor, "WOWV86: relocated to paragraphs, sites=0x"); cursor = LogHex(cursor, module->Sites);
    cursor = LogPut(cursor, "\r\n  ENTRY CS:IP=");  cursor = LogHex(cursor, *entryCs); cursor = LogPut(cursor, ":"); cursor = LogHex(cursor, *eip);
    cursor = LogPut(cursor, "  DS=");               cursor = LogHex(cursor, *entryDs);
    cursor = LogPut(cursor, "  SS:SP=");            cursor = LogHex(cursor, *entrySs); cursor = LogPut(cursor, ":"); cursor = LogHex(cursor, *esp);
    cursor = LogPut(cursor, "  AX=4b4f ('OK')\r\n");
    LogAppend(LDTLOG_PATH, message, cursor);
    return 0;
}

static VOID WowProbeSelectors(VOID)
{
    CHAR message[600], *cursor;
    INT moduleIndex, index;
    if (!g_WowModuleCount) return;
    cursor = message; cursor = LogPut(cursor, "WOWTRY: selector stage, modules="); cursor = LogHex(cursor, (DWORD)g_WowModuleCount);
    cursor = LogPut(cursor, "\r\n"); LogAppend(LDTLOG_PATH, message, cursor);
    WowProbeLdtMatrix("wow-late");

    /* ── STEP PAST THE RESERVED LDT INDICES BEFORE ALLOCATING ANYTHING. ─────────────
         `g_LdtNext` starts at 3, and DpmiInstall() FORCES indices 2 and 3 to
         writable data (0xF2) no matter what access we asked for -- a deliberate hack
         for the DPMI path, where a client's first allocation is typically its stack
         and i310102's C runtime retypes sel 0x1F to code and then #GPs on it.
         WOW's first allocation is not a stack. It is krnl386's CODE segment 1, and it
         was silently landing on index 3 and becoming a DATA descriptor.
       ★ THE LOG SAID "CODE ... LAR ok". It was the LAR READBACK that caught it, by
         reporting ar=0xf200 where segments 2 and 3 of the same module reported
         0xfa00 -- i.e. the CPU disagreeing with our bookkeeping, which is the entire
         reason that readback exists. The failure it prevents is the next step of this
         work: jumping to sel 0x1F:0xC02B would have #GP'd instantly, with a log
         claiming a code selector had installed cleanly.
         6 is the same floor INT 31h 0001 calls "reserved", so a client cannot free
         these either; keeping the two in step is why the constant is shared.
       ★ SINCE SESSION 48 the floor is `DPMI_LDT_FIRSTFREE`, above the host's own
         private pool, so a client allocation can never land on a selector the host
         has handed out and the guest is holding forever. */
    if (g_LdtNext < DPMI_LDT_FIRSTFREE) {
        cursor = message; cursor = LogPut(cursor, "  skipping reserved + host-pool LDT indices, next 0x");
        cursor = LogHex(cursor, (DWORD)g_LdtNext); cursor = LogPut(cursor, " -> 0x");
        g_LdtNext = DPMI_LDT_FIRSTFREE;
        cursor = LogHex(cursor, (DWORD)g_LdtNext);
        cursor = LogPut(cursor, " (2 and 3 are forced to data by dpmi_install)\r\n");
        LogAppend(LDTLOG_PATH, message, cursor);
    }

    /* Can krnl386 be given a real descriptor-table window? Asked AFTER the reserved
       skip above, so the throwaway probes cannot land on indices 2/3 -- which
       DpmiInstall() force-types to data, i.e. the exact interaction that silently
       turned krnl386's code segment into a data descriptor earlier this session. */
    WowFindLdtBase();

    /* ── PHASE 2a: a selector for EVERY segment of EVERY module, before any
         relocation runs. NeRegistryResolve refuses a target whose selector is still
         0, so getting this order wrong fails loudly instead of writing 0000:xxxx. */
    for (moduleIndex = 0; moduleIndex < g_WowModuleCount; ++moduleIndex) {
        NE_MODULE *module = &g_WowModule[moduleIndex];
        for (index = 0; index < (INT)module->SegmentCount; ++index) {
            NE_SEGMENT *segment = &module->Segments[index];
            INT isCode = !(segment->Flags & NE_SEG_DATA);
            UINT32 need = NeSegmentAllocSize(segment);
            INT ldtIndex;
            if (g_LdtNext >= DPMI_LDT_MAX) {
                cursor = message; cursor = LogPut(cursor, "  LDT POOL EXHAUSTED\r\n");
                LogAppend(LDTLOG_PATH, message, cursor); return;
            }
            ldtIndex = g_LdtNext++;
            g_Ldt[ldtIndex].Base   = (DWORD)(ULONG_PTR)segment->Memory;
            g_Ldt[ldtIndex].Limit  = need - 1;
            g_Ldt[ldtIndex].Access = (BYTE)(isCode ? DPMI_ACCESS_CODE : DPMI_ACCESS_DATA);
            g_Ldt[ldtIndex].Flags  = 0;                       /* 16-bit segments */
            DpmiInstall(ldtIndex);
            segment->Selector = (WORD)DPMI_LDT_SELECTOR(ldtIndex);            /* the REAL selector now */

            cursor = message;
            cursor = LogPut(cursor, "  "); cursor = LogPut(cursor, g_WowName[moduleIndex]);
            cursor = LogPut(cursor, " seg ");      cursor = LogHex(cursor, (DWORD)(index + 1));
            cursor = LogPut(cursor, isCode ? " CODE" : " DATA");
            cursor = LogPut(cursor, " base=0x");   cursor = LogHex(cursor, g_Ldt[ldtIndex].Base);
            cursor = LogPut(cursor, " limit=0x");  cursor = LogHex(cursor, g_Ldt[ldtIndex].Limit);
            cursor = LogPut(cursor, " -> sel 0x"); cursor = LogHex(cursor, segment->Selector);
            /* Read the descriptor back through the CPU. LAR only succeeds on a
               selector the processor can actually see, so this is the hardware
               confirming the install rather than us believing our own bookkeeping. */
            {   DWORD accessRights = 0; BYTE isValid = 0; WORD selector = segment->Selector;
                __asm__ __volatile__("lar %2, %0\n\tsetz %1"
                                     : "=r"(accessRights), "=q"(isValid) : "r"(selector) : "cc");
                cursor = LogPut(cursor, isValid ? "  LAR ok ar=0x" : "  LAR FAILED ar=0x");
                cursor = LogHex(cursor, isValid ? accessRights : 0);
            }
            cursor = LogPut(cursor, "\r\n"); LogAppend(LDTLOG_PATH, message, cursor);
        }
        NeRegistryAdd(&g_WowRegistry, module);
    }

    /* ── PHASE 2b: relocate ONCE, resolving imports across the registry. ──────────
         KERNEL imports from nothing, so it only proves the internal fixups. GDI and
         USER are the real test: every call they make into KERNEL is patched here.
         WOWEXEC is expected to STOP at KEYBOARD, which is simply not loaded yet --
         a stop that names its module is a to-do list, not a failure. */
    for (moduleIndex = 0; moduleIndex < g_WowModuleCount; ++moduleIndex) {
        NE_MODULE *module = &g_WowModule[moduleIndex];
        INT status = 0;
        module->Sites = 0;
        for (index = 0; index < (INT)module->SegmentCount && status == 0; ++index)
            status = NeApplyRelocations(module, index, NeRegistryResolve, &g_WowRegistry);
        cursor = message;
        cursor = LogPut(cursor, "  RELOC "); cursor = LogPut(cursor, g_WowName[moduleIndex]);
        if (status == 0) {
            cursor = LogPut(cursor, " ALL RESOLVED sites=0x"); cursor = LogHex(cursor, module->Sites);
        } else {
            cursor = LogPut(cursor, " STOPPED after 0x");     cursor = LogHex(cursor, module->Sites);
            cursor = LogPut(cursor, " sites, at ne.h line "); cursor = LogHex(cursor, (DWORD)module->Error);
            cursor = LogPut(cursor, ", needed ");             cursor = LogPut(cursor, g_WowRegistry.FailedModule);
            cursor = LogPut(cursor, ".");
            if (g_WowRegistry.FailedFunction[0]) cursor = LogPut(cursor, g_WowRegistry.FailedFunction);
            else                    { cursor = LogPut(cursor, "@"); cursor = LogHex(cursor, g_WowRegistry.FailedOrdinal); }
        }
        cursor = LogPut(cursor, "\r\n"); LogAppend(LDTLOG_PATH, message, cursor);
    }

    cursor = message;
    cursor = LogPut(cursor, "WOWTRY: bind stage done.\r\n  KERNEL init entry would be CS:IP = sel 0x");
    cursor = LogHex(cursor, g_WowModule[0].Segments[(g_WowModule[0].CsIp >> WORD_SHIFT) - 1].Selector);
    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, g_WowModule[0].CsIp & WORD_MASK);
    cursor = LogPut(cursor, "\r\nWOWTRY: NOT entering PM yet -- next step.\r\n");
    LogAppend(LDTLOG_PATH, message, cursor);
}
/* ── ★ ANSWER AN UNIMPLEMENTED WOW32 CALL DIFFERENTLY, WITHOUT CLAIMING TO KNOW
     WHAT IT MEANS. (GH #128, session 37) ──────────────────────────────────────────
     53 of the 82 IDs are not named by krnl386's export table, and the sentinel we
     answer them with is load-bearing: `0` is right for "declined / not present" and
     WRONG for a caller that loops until the answer is non-zero. krnl386 has one
     (observed): it allocates, asks WOW32 0x7d whether the result is acceptable, and
     on 0 allocates another and asks again. It ran 1884 times and took the stack out.
     Guessing the semantics and writing a `case` for it is what this project keeps
     paying for. So: a FILE, like pmbp.txt -- one `<hex id> <hex dword>` per line --
     that changes the answer for one run. The log marks every overridden call as an
     EXPERIMENT rather than a service, so no reader can mistake a measurement for an
     implementation. Absent file = the sentinel, unchanged, and no cost. */
#define WOW32RET_PATH CFG_("wow32ret.txt")
#define WOW32RET_MAX 16
static WORD  g_Wow32ReturnId[WOW32RET_MAX];
static DWORD g_Wow32ReturnValue[WOW32RET_MAX];
static INT   g_Wow32ReturnCount = 0;

static DWORD Wow32ReturnOverride(WORD thunkId)
{
    INT index;
    for (index = 0; index < g_Wow32ReturnCount; ++index) if (g_Wow32ReturnId[index] == thunkId) return g_Wow32ReturnValue[index];
    return (DWORD)WOW32_UNIMPL_RET;
}

/* ── ★★★ AND THE SAME LEVER FOR THE EPILOGUE MODE. (GH #128, session 38) ──────
     `wow32ret.txt` chooses what a call ANSWERS. `wowmode.txt` chooses HOW IT
     RETURNS -- the word at bp-24 that picks one of krnl386's 38 return paths (see
     WOW32_OFF_MODE in wow32.h). krnl386 pushes 0 there and never sets it, so all
     37 non-zero modes exist for the 32-bit side and none of them has ever been
     exercised on this host. Mode 25 is the task switch-back.
   ⚠ THIS IS THE MOST DANGEROUS KNOB IN THE TREE. A mode selects a code path that
     pops a specific stack shape; the wrong one at the wrong call site resumes the
     guest with SS:SP loaded from whatever two registers happened to hold, which is
     not a crash so much as a random jump. One line, one id, one run, and read the
     log -- the same discipline wow32ret.txt earned the hard way, for higher stakes.
   Format: `<hex id> <hex mode>`, data lines first, `#` comments below. */
#define WOWMODE_PATH CFG_("wowmode.txt")
#define WOWMODE_MAX 8
static WORD g_Wow32ModeId[WOWMODE_MAX];
static WORD g_Wow32ModeValue[WOWMODE_MAX];
static INT  g_Wow32ModeCount = 0;

/* -1 = no override for this id (0 is a legal mode, so it cannot be the sentinel). */
static INT Wow32ModeOverride(WORD thunkId)
{
    INT index;
    for (index = 0; index < g_Wow32ModeCount; ++index)
        if (g_Wow32ModeId[index] == thunkId) return (INT)g_Wow32ModeValue[index];
    return -1;
}
enum { WOW32_KNOB_COLUMNS = 2 };   /* wow32mode.txt / wow32ret.txt: two hex columns a line */
static VOID Wow32ModeLoad(VOID)
{
    HANDLE handle = CreateFileA(WOWMODE_PATH, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHAR buffer[8192]; DWORD bytesRead = 0, index = 0;
    CHAR lineBuffer[256], *cursor = lineBuffer;
    if (handle == INVALID_HANDLE_VALUE) return;
    ReadFile(handle, buffer, sizeof buffer - 1, &bytesRead, NULL);
    CloseHandle(handle);
    if (bytesRead >= sizeof buffer - 1) {
        cursor = LogPut(cursor, "WOWMODE: !! wowmode.txt TRUNCATED AT THE BUFFER -- LINES DROPPED\r\n");
        LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor); cursor = lineBuffer;
    }
    while (index < bytesRead && g_Wow32ModeCount < WOWMODE_MAX) {
        DWORD values[WOW32_KNOB_COLUMNS] = { 0, 0 }; INT column = 0;
        while (index < bytesRead && (buffer[index] == '\r' || buffer[index] == '\n')) ++index;
        if (index >= bytesRead) break;
        if (buffer[index] == '#') { while (index < bytesRead && buffer[index] != '\n') ++index; continue; }
        while (index < bytesRead && buffer[index] != '\r' && buffer[index] != '\n' && column < WOW32_KNOB_COLUMNS) {
            INT digits = 0;
            while (index < bytesRead && (buffer[index] == ' ' || buffer[index] == '\t')) ++index;
            if (index >= bytesRead || buffer[index] == '\r' || buffer[index] == '\n' || buffer[index] == '#') break;
            while (index < bytesRead) {
                CHAR character = buffer[index];
                INT digit = (character >= '0' && character <= '9') ? character - '0'
                      : (character >= 'a' && character <= 'f') ? character - 'a' + HEX_DIGIT_A_VALUE
                      : (character >= 'A' && character <= 'F') ? character - 'A' + HEX_DIGIT_A_VALUE : -1;
                if (digit < 0) break;
                values[column] = (values[column] << NIBBLE_SHIFT) | (DWORD)digit; ++digits; ++index;
            }
            if (digits) ++column; else ++index;
        }
        while (index < bytesRead && buffer[index] != '\n') ++index;
        if (column >= WOW32_KNOB_COLUMNS) {
            g_Wow32ModeId[g_Wow32ModeCount]  = (WORD)values[0];
            g_Wow32ModeValue[g_Wow32ModeCount] = (WORD)values[1];
            ++g_Wow32ModeCount;
        }
    }
    if (g_Wow32ModeCount) {
        INT entry;
        cursor = LogPut(cursor, "WOWMODE: ** EXPERIMENT ** returning");
        for (entry = 0; entry < g_Wow32ModeCount; ++entry) {
            cursor = LogPut(cursor, " 0x"); cursor = LogHexByte(cursor, (BYTE)g_Wow32ModeId[entry]);
            cursor = LogPut(cursor, " through epilogue mode "); cursor = LogHex(cursor, g_Wow32ModeValue[entry]);
        }
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
    }
}

static VOID Wow32ReturnLoad(VOID)
{
    HANDLE handle = CreateFileA(WOW32RET_PATH, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    /* ⚠⚠ 8 KB, AND SAY SO WHEN IT IS NOT ENOUGH. This was `char buf[1024]` for
         exactly as long as it took to write a file with a comment block explaining
         why the one data line matters -- 1375 bytes, data line last, silently
         truncated, and the run regressed to the stack overflow with NOTHING in the
         log to say why. That is DpmiBreakpointLoad's bug from earlier the same session,
         reproduced in the loader written after fixing it. The rule is the same in
         both places and it is now enforced in both: read enough, and shout when the
         file was longer than the buffer. */
    CHAR buffer[8192]; DWORD bytesRead = 0, index = 0;
    CHAR lineBuffer[256], *cursor = lineBuffer;
    if (handle == INVALID_HANDLE_VALUE) return;
    ReadFile(handle, buffer, sizeof buffer - 1, &bytesRead, NULL);
    CloseHandle(handle);
    if (bytesRead >= sizeof buffer - 1) {
        cursor = LogPut(cursor, "WOW32RET: !! wow32ret.txt TRUNCATED AT THE BUFFER -- LINES DROPPED\r\n");
        LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor); cursor = lineBuffer;
    }
    while (index < bytesRead && g_Wow32ReturnCount < WOW32RET_MAX) {
        DWORD values[WOW32_KNOB_COLUMNS] = { 0, 0 }; INT column = 0;
        while (index < bytesRead && (buffer[index] == '\r' || buffer[index] == '\n')) ++index;
        if (index >= bytesRead) break;
        if (buffer[index] == '#') { while (index < bytesRead && buffer[index] != '\n') ++index; continue; }
        while (index < bytesRead && buffer[index] != '\r' && buffer[index] != '\n' && column < WOW32_KNOB_COLUMNS) {
            INT digits = 0;
            while (index < bytesRead && (buffer[index] == ' ' || buffer[index] == '\t')) ++index;
            if (index >= bytesRead || buffer[index] == '\r' || buffer[index] == '\n' || buffer[index] == '#') break;
            while (index < bytesRead) {
                CHAR character = buffer[index];
                INT digit = (character >= '0' && character <= '9') ? character - '0'
                      : (character >= 'a' && character <= 'f') ? character - 'a' + HEX_DIGIT_A_VALUE
                      : (character >= 'A' && character <= 'F') ? character - 'A' + HEX_DIGIT_A_VALUE : -1;
                if (digit < 0) break;
                values[column] = (values[column] << NIBBLE_SHIFT) | (DWORD)digit; ++digits; ++index;
            }
            if (digits) ++column; else ++index;
        }
        while (index < bytesRead && buffer[index] != '\n') ++index;
        if (column >= WOW32_KNOB_COLUMNS) {
            g_Wow32ReturnId[g_Wow32ReturnCount]  = (WORD)values[0];
            g_Wow32ReturnValue[g_Wow32ReturnCount] = values[1];
            ++g_Wow32ReturnCount;
        }
    }
    if (g_Wow32ReturnCount) {
        INT entry;
        cursor = LogPut(cursor, "WOW32RET: ** EXPERIMENT ** overriding");
        for (entry = 0; entry < g_Wow32ReturnCount; ++entry) {
            cursor = LogPut(cursor, " 0x"); cursor = LogHexByte(cursor, (BYTE)g_Wow32ReturnId[entry]);
            cursor = LogPut(cursor, "->0x"); cursor = LogHex(cursor, g_Wow32ReturnValue[entry]);
        }
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
    }
}

/* ── ★★★ THE WIN16 TASK SCHEDULER (GH #128, session 38). ──────────────────────
     The mechanism, the evidence for it and what this first cut does NOT do are
     all in src/wow/wowsched.h; this is the state and the wiring.
   ⚠ OPT-IN, and deliberately so. Turning it on changes the ORDER in which two
     16-bit tasks run, which is the largest behavioural change this host has made
     since it started executing Win16 code at all. A default run must still
     reproduce the committed result exactly, so the switch is a file on the share
     and its absence costs nothing. */
#define WOWSCHED_PATH CFG_("wowsched.txt")
static WORD  g_WowDgroupSelector   = 0;      /* krnl386's DGROUP selector, learned at a BOP */
static INT   g_WowSchedRoundRobin = 0;            /* round-robin cursor for the yields            */
static INT WowSchedFree(VOID)
{
    INT index;
    for (index = 0; index < WOWSCHED_MAX; ++index) if (!g_WowSchedSlots[index].IsUsed) return index;
    return -1;
}
/* The next parked task other than `cur`, round robin; -1 if none. */
static INT WowSchedPick(WORD current)
{
    INT index, step;
    for (step = 1; step <= WOWSCHED_MAX; ++step) {
        index = (g_WowSchedRoundRobin + step) % WOWSCHED_MAX;
        if (g_WowSchedSlots[index].IsUsed && g_WowSchedSlots[index].Task != current) { g_WowSchedRoundRobin = index; return index; }
    }
    return -1;
}
/* A task parked at its launch that has never run -- or, with `mid`, one parked
   mid-work by the launch-first yield below; -1 if none. */
static INT WowSchedFresh(WORD current)
{
    INT index;
    for (index = 0; index < WOWSCHED_MAX; ++index)
        if (g_WowSchedSlots[index].IsUsed && g_WowSchedSlots[index].IsFresh && g_WowSchedSlots[index].Task != current) return index;
    return -1;
}
/* A task that may take the CPU from one idling at callback depth `depth`: a fresh one
   only from the top (depth 0); a parent parked mid-work (F) only at the depth it left,
   so the host's callback frames stay last-in first-out across the swap. */
static INT WowSchedRunnable(WORD current, INT depth)
{
    INT index;
    for (index = 0; index < WOWSCHED_MAX; ++index)
        if (g_WowSchedSlots[index].IsUsed && g_WowSchedSlots[index].Task != current
            && ((g_WowSchedSlots[index].IsRunnable && g_WowSchedSlots[index].CallbackDepth == depth)
                || (g_WowSchedSlots[index].IsFresh && depth == 0)
                || (g_WowSchedSlots[index].IsWaitingForMessage && WowMsgCountFor(g_WowSchedSlots[index].Task)
                    && (g_WowSchedSlots[index].CallbackDepth == g_WowSchedSlots[index].BaseDepth
                        || g_WowSchedSlots[index].CallbackDepth == depth)))) return index;
    return -1;
}
/* Parked holding no host callback frame: launched and never run, or idle in its own
   top-level GetMessage. Such a task is re-based at whatever depth resumes it. */
static INT WowSchedTopLevel(const WOWSCHED_SLOT *slot)
{
    return slot->IsFresh || (slot->IsWaitingForMessage && slot->CallbackDepth == slot->BaseDepth);
}
/* krnl386's current-task word. 0xFFFF means "we do not know yet", which is NOT
   the same as 0 -- 0 is krnl386 saying "no task is current", and acting on the
   two as if they were the same would switch tasks before the guest has one. */
static WORD WowSchedCurrentTask(VOID)
{
    DWORD base;
    const volatile BYTE *dgroup;
    if (!g_WowDgroupSelector) return WOWUSER_TASK_NONE16;
    base = DpmiSelectorBase(g_WowDgroupSelector);
    if (!base) return WOWUSER_TASK_NONE16;
    dgroup = (const volatile BYTE *)(ULONG_PTR)base;
    return (WORD)(dgroup[WOWUSER_KRNL_CURRENT_TASK] | (dgroup[WOWUSER_KRNL_CURRENT_TASK + 1] << BYTE_SHIFT));
}

/* ── #164: A TASK RUNS IN ITS OWN CURRENT DIRECTORY. (s85) ──────────────────────────
     DOS has one current directory and Win16 one per task. On NT the 32-bit side keeps
     them (krnl386's TDB holds only the drive -- measured: TDB+0x66 = 0x82, +0x67
     empty, for WOWEXEC and the task alike) and puts a task's back when it runs. Here
     OUR scheduler switches tasks, so this is that table. Measured with
     tests/probes/win16/w_cwd: the launched task parked at its first WaitEvent, WOWEXEC
     changed back to C:\WINDOWS, and the task resumed there -- its relative CreateFile
     landed in C:\WINDOWS, where stock puts it in the launch folder.
   ► A task's entry is written when it parks at its launch (it inherits the directory
     its creator chose for LoadModule -- WOWEXEC sets it from the launch's cur= just
     before) and whenever it calls WOW32 0x82; it is restored when it is resumed. */
#define WOW_TASK_DIRS 8
static struct { WORD Task; CHAR Directory[MAX_PATH]; } g_WowTaskDirectory[WOW_TASK_DIRS];
static VOID WowTaskDirectoryNote(WORD task, PCSTR directory)
{
    INT index, freeK = -1;
    if (!task || task == WOWUSER_TASK_NONE16) return;
    for (index = 0; index < WOW_TASK_DIRS; ++index) {
        if (g_WowTaskDirectory[index].Task == task) break;
        if (!g_WowTaskDirectory[index].Task && freeK < 0) freeK = index;
    }
    if (index == WOW_TASK_DIRS) { if (freeK < 0) return; index = freeK; }
    g_WowTaskDirectory[index].Task = task;
    lstrcpynA(g_WowTaskDirectory[index].Directory, directory, sizeof g_WowTaskDirectory[index].Directory);
}
static VOID WowTaskDirectoryHere(WORD task)          /* record the host's directory now */
{
    CHAR directory[MAX_PATH];
    if (GetCurrentDirectoryA(sizeof directory, directory)) WowTaskDirectoryNote(task, directory);
}
static VOID Wow32CurrentDirectorySet(PCSTR directory)    /* WOW32 0x82 succeeded (wow32.h) */
{
    WowTaskDirectoryNote(WowSchedCurrentTask(), directory);
}
static VOID WowTaskChdir(WORD task, PSTR *logCursor)
{
    INT index;
    static INT logged;
    for (index = 0; index < WOW_TASK_DIRS; ++index) if (g_WowTaskDirectory[index].Task == task) break;
    if (index == WOW_TASK_DIRS) return;
    {   INT isOk = SetCurrentDirectoryA(g_WowTaskDirectory[index].Directory) != 0;
        if (logged < 12) {
            ++logged;
            *logCursor = LogPut(*logCursor, "  WOWSCHED: task 0x"); *logCursor = LogHex(*logCursor, task);
            *logCursor = LogPut(*logCursor, " resumes in ["); *logCursor = LogPut(*logCursor, g_WowTaskDirectory[index].Directory);
            *logCursor = LogPut(*logCursor, isOk ? "]\r\n" : "] -- SetCurrentDirectory FAILED\r\n");
        }
    }
}

/* ── ★★ [0x228] IS PART OF THE CONTEXT, NOT A LEVER. (session 39) ─────────────
     The register file is not the whole of a task switch: krnl386 keeps "who is
     current" in one word of its DGROUP (offset 0x228 -- observed to hold the
     current TDB), and the launch sequence has already set it to the new task when
     the 0x74 BOP we park at arrives. Mode 25's epilogue then puts it back to the
     creator (observed). So a frame parked at that BOP is
     only self-consistent if the word is put back with it -- restoring registers
     alone resumes the new task's code with krnl386 still believing the creator
     is current.
   ⚠ THIS IS NOT "WRITE [0x228] TO YIELD", WHICH IS RULED OUT AND STAYS RULED
     OUT. That was using the word as a lever to make krnl386 switch, and it does
     not work -- measured: krnl386 simply carries on as the caller's own task.
     This is the opposite direction: the host has already
     switched, and this makes the guest's own bookkeeping agree with the context
     it is about to run. The value is not invented; it is the one the word held
     when that frame was parked (`slot->task`). */
static VOID WowSchedSetCurrent(WORD task)
{
    DWORD base;
    volatile BYTE *dgroup;
    if (!g_WowDgroupSelector) return;
    base = DpmiSelectorBase(g_WowDgroupSelector);
    if (!base) return;
    dgroup = (volatile BYTE *)(ULONG_PTR)base;
    dgroup[WOWUSER_KRNL_CURRENT_TASK] = (BYTE)(task & BYTE_MASK);
    dgroup[WOWUSER_KRNL_CURRENT_TASK + 1] = (BYTE)(task >> BYTE_SHIFT);
}
enum { WOW_RETARGET_HEADROOM = 0x40, WOW_RETARGET_STACK_MIN = 0x200 };   /* WowSchedRetarget: below the lowest live frame */
/* ── s92 (#306): THE RECEIVER'S STACK FOR AN INTER-TASK MESSAGE -- see
     g_WowCallRetarget in wowcall.h. The window's owner (wowuser.h records its
     creator) must not be the running task, and must be somewhere the host knows
     its free stack: parked in a run-queue slot, or blocked in a host callback
     frame it entered (the newest such frame's saved SP; everything below is
     free). Anything else runs where it always did. 0x40 bytes are left below the
     parked SP for the frame the guest itself may still think is live. */
static WORD WowSchedOwnerOf(WORD hwnd) { return WowUserOwner16(hwnd); }
static DWORD g_WowSchedInterTask;
static INT WowSchedRetarget(WORD hwnd, WORD *stackSegment, WORD *stackPointer, DWORD *stackSegmentBase, WORD *prev)
{
    WORD owner = WowSchedOwnerOf(hwnd), current = WowSchedCurrentTask();
    const BYTE *contexts[WOWSCHED_MAX + WOWCALL_MAX_DEPTH];
    WORD foundStackSegment = 0, lowestStackPointer = WORD_MASK;
    INT index, count = 0;
    if (!owner || !current || current == WOWUSER_TASK_NONE16 || owner == current) return 0;
    for (index = 0; index < WOWSCHED_MAX; ++index)
        if (g_WowSchedSlots[index].IsUsed && g_WowSchedSlots[index].Task == owner) contexts[count++] = g_WowSchedSlots[index].Context;
    for (index = g_WowCallDepth - 2; index >= 0; --index)         /* the newest frame is the one being built */
        if (g_WowCallFrames[index].EnteredTask == owner) contexts[count++] = g_WowCallFrames[index].Saved.Context;
    /* ⚠ THE DEEPEST ONE. Calls that bounce between two tasks leave the owner's
         stack in use below its parked SP; the lowest SP known is the free edge. */
    for (index = 0; index < count; ++index) {
        WORD contextSs = (WORD)(contexts[index][VTIB_SS - WOWSCHED_CTX_LO] | (contexts[index][VTIB_SS - WOWSCHED_CTX_LO + 1] << BYTE_SHIFT));
        WORD contextSp = (WORD)(contexts[index][VTIB_ESP - WOWSCHED_CTX_LO] | (contexts[index][VTIB_ESP - WOWSCHED_CTX_LO + 1] << BYTE_SHIFT));
        if (foundStackSegment && (contextSs & ~X86_SELECTOR_RPL_MASK_U) != (foundStackSegment & ~X86_SELECTOR_RPL_MASK_U)) return 0;   /* two stacks: do not guess */
        foundStackSegment = contextSs;
        if (contextSp < lowestStackPointer) lowestStackPointer = contextSp;
    }
    if (!count) return 0;
    *stackSegment = foundStackSegment;
    *stackPointer = (WORD)((lowestStackPointer - WOW_RETARGET_HEADROOM) & ~X86_WORD_ALIGN_MASK_U);
    if (*stackPointer < WOW_RETARGET_STACK_MIN) return 0;                    /* no room: run where it always did */
    *stackSegmentBase = DpmiSelectorBase(*stackSegment);
    if (!*stackSegmentBase) return 0;
    *prev = current;
    WowSchedSetCurrent(owner);
    ++g_WowSchedInterTask;
    return 1;
}
static VOID WowSchedUntarget(WORD prev) { WowSchedSetCurrent(prev); }
/* An inter-task call is in flight: the owner's parked context is BORROWED, so no
   yield may park or resume anything until it returns. */
static INT WowSchedInterTaskLive(VOID)
{
    INT index;
    for (index = 0; index < g_WowCallDepth; ++index) if (g_WowCallFrames[index].PreviousTask) return 1;
    return 0;
}

#define WOWQUIET_PATH CFG_("wowquiet.txt")
static VOID WowQuietLoad(VOID)
{
    HANDLE handle = CreateFileA(WOWQUIET_PATH, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHAR lineBuffer[192], *cursor = lineBuffer;
    if (handle == INVALID_HANDLE_VALUE) return;
    CloseHandle(handle);
    /* Say so BEFORE going quiet, or the run leaves no evidence of why it is
       silent -- a log that just stops looks like a crash. */
    cursor = LogPut(cursor, "WOWQUIET: ** ON ** -- the trace is SILENCED from here for an A/B "
                "measurement. This is not a product mode; delete wowquiet.txt to "
                "get the instrumented run back.\r\n");
    LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
    g_LogIsQuiet = 1;
}

static VOID WowSchedLoad(VOID)
{
    HANDLE handle = CreateFileA(WOWSCHED_PATH, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHAR lineBuffer[192], *cursor = lineBuffer;
    if (handle == INVALID_HANDLE_VALUE) return;
    CloseHandle(handle);
    g_WowSchedOn = 1;
    cursor = LogPut(cursor, "WOWSCHED: ** ON ** -- Win16 tasks will be interleaved by the host "
                "(0x74 saves the launch frame, WaitEvent hands the creator back "
                "through epilogue mode 25, [0x228]==0 starts the parked task)\r\n");
    LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
}

/* ── ★★★★★ CALLING 16-BIT CODE: THE SWITCH. (GH #128, session 40) ─────────────
     Opt-in for the same reason the scheduler is, and the reason is stronger here:
     this makes the host execute guest code that nothing has ever executed, on a
     stack it did not build the frame for. A default run must still reproduce the
     committed baseline (270 / 44 / 122 / 98) count for count, so the switch is a
     file on the share and its absence costs nothing. See src/wow/wowcall.h. */
#define WOWCALL_PATH CFG_("wowcall.txt")
static INT g_WowCallOn = 0;

static VOID WowCallLoad(VOID)
{
    HANDLE handle = CreateFileA(WOWCALL_PATH, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHAR lineBuffer[224], *cursor = lineBuffer;
    if (handle == INVALID_HANDLE_VALUE) return;
    CloseHandle(handle);
    g_WowCallOn = 1;
    cursor = LogPut(cursor, "WOWCALL: ** ON ** -- the host will CALL 16-bit code: CreateWindow "
                "sends WM_CREATE to the window's own procedure and waits for it to "
                "return through the C4 C4 stub\r\n");
    LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
}

/* ── THE "MS-DOS" VENDOR-SPECIFIC DPMI API (INT 2Fh 168A). GH #128. ─────────────────
     krnl386 will not run without this. It asks with DS:SI -> "MS-DOS", and if AL
     comes back 0x8A (unchanged: "not supported") it aborts with
     "NTVDM KERNEL: Inadequate DPMI Server" (observed). Leaving AL alone -- the correct answer for
     a host with no vendor API -- is therefore fatal to this one guest.

   ★ WHAT STOCK NTVDM ACTUALLY RETURNS, measured (tests/probes/dos/vendprobe.com under
     `stock`): in REAL mode AL=8A (not supported); in PROTECTED mode AL=00 and
     ES:DI = 00C7:2037, a readable code selector (LAR=0xFB00). Called (same probe):

         AX=0x0000  ->  AX=0x0100, CF=0
         AX=0x0100  ->  AX=0x0137, CF=0
         anything else  ->  CF=1

     A two-function entry returning constants. So the API is PM-ONLY, which is
     consistent: krnl386 only ever asks after it has switched.

   ★ AND krnl386 ONLY NEEDS IT TO EXIST. It calls the entry once, with AX=0x0100,
     and keeps the AX it gets back only when CF is clear AND it is a WRITABLE
     selector; otherwise it carries on normally (observed: CF=1 here costs nothing).
     So an honest "that function is not provided" (CF=1) is tolerated by the guest.

   ⚠ WHICH IS WHY FUNCTION 0x0100 RETURNS CF=1 HERE AND NOT A SELECTOR. Stock hands
     back 0x0137, a writable data selector (VERW, measured) onto something
     ntvdm owns -- we do not know what, and a selector onto an empty block of ours
     would pass that check, get stored, and be read later as if it were that something.
     That is the "runs but lies" failure this project treats as the most expensive
     kind. Declining is truthful and costs nothing today. Function 0 mirrors the
     oracle exactly, because there we are copying a measured answer rather than
     inventing one. */
static WORD g_WowVendorSelector = 0;

/* Translate a guest selector to a host linear base, for far-pointer arguments.
   ⚠ Goes through DpmiSelectorBase rather than g_Ldt[] directly so that a selector
     krnl386 created by writing the descriptor shadow resolves the same way here
     as everywhere else in the host. */
static DWORD Wow32HostSelectorToLinear(WORD selector, PVOID context)
{
    (VOID)context;
    return DpmiSelectorBase(selector);
}

static WORD  g_WowShadowSelector = 0;
/* ── "WHAT DID THE SHADOW LAST LOOK LIKE" IS A DIFFERENT QUESTION FROM "WHAT IS
     IN THE REAL LDT", AND CONFLATING THEM IS A BUG WE HAVE NOW MADE TWICE.
     The sync wants to know which entries the GUEST changed, so it must diff the
     shadow against the last shadow it saw -- not against g_Ldt[], which is the
     host's record of the REAL LDT.
     Diffing against g_Ldt[] forces a lie whenever the two cannot be made equal:
     krnl386 writes free-list links with access byte 0x0F, the kernel rejects them
     (`INSTALL FAILED st=0xc000011a`, four per run on the rig), and the old code
     then updated g_Ldt[] to match the shadow ANYWAY -- purely so the next pass
     would not retry forever. From that moment g_Ldt[] claimed a base and limit
     the CPU had never been told about, and DpmiSelectorBase() -- which every part of
     the host uses to resolve a guest pointer, including the WOW32 argument
     translation added this session -- would have answered from it.
     With a separate `seen` copy, a failed install stops being retried without
     anybody having to pretend it succeeded. */
static BYTE *g_WowSeen = NULL;          /* last shadow contents we processed */

static VOID WowShadowPut(INT index)      /* g_Ldt[idx] -> shadow */
{
    DWORD low, high, *entry;
    if (!g_WowShadow || index < 0 || index >= WOW_SHADOW_ENTRIES) return;
    DpmiBuildDescriptor(g_Ldt[index].Base, g_Ldt[index].Limit,
                    g_Ldt[index].Access, g_Ldt[index].Flags, &low, &high);
    entry = (DWORD *)(g_WowShadow + index * X86_DESCRIPTOR_SIZE);
    entry[0] = low; entry[1] = high;
    if (g_WowSeen) { entry = (DWORD *)(g_WowSeen + index * X86_DESCRIPTOR_SIZE); entry[0] = low; entry[1] = high; }
}
enum { WOW_SHADOW_SCAN_SLACK = 8 };   /* WowShadowSync looks a few entries past g_LdtNext */
/* Push anything krnl386 changed in the shadow into the real LDT. Returns the count. */
static INT WowShadowSync(PSTR *logCursor)
{
    INT index, top = g_LdtNext + WOW_SHADOW_SCAN_SLACK, count = 0;
    PSTR cursor = logCursor ? *logCursor : NULL;
    if (!g_WowShadow || !g_WowSeen) return 0;
    if (top > WOW_SHADOW_ENTRIES) top = WOW_SHADOW_ENTRIES;
    for (index = DPMI_LDT_RESERVED; index < top; ++index) {
        DWORD low, high;
        const DWORD *entry = (const DWORD *)(g_WowShadow + index * X86_DESCRIPTOR_SIZE);
        DWORD *seen = (DWORD *)(g_WowSeen + index * X86_DESCRIPTOR_SIZE);
        low = seen[0]; high = seen[1];
        if (entry[0] == low && entry[1] == high) continue;      /* the guest did not touch it */
        seen[0] = entry[0]; seen[1] = entry[1];                  /* acknowledged either way */
        {   WORD selector = (WORD)DPMI_LDT_SELECTOR(index);
            DWORD descriptorLow = entry[0], descriptorHigh = entry[1];
            /* Install EXACTLY the bytes the guest wrote -- decoding and re-encoding
               would quietly normalise anything we got wrong. Then decode purely for
               our own bookkeeping so later host-side reads of g_Ldt[] agree. */
            LONG status = VdmInstallLdtEntries(selector, descriptorLow, descriptorHigh, selector, descriptorLow, descriptorHigh);
            DWORD descriptorBase = (descriptorHigh & X86_DESCRIPTOR_BASE_HIGH_U) | ((descriptorHigh & BYTE_MASK_U) << WORD_SHIFT) | (descriptorLow >> WORD_SHIFT);
            DWORD descriptorLimit  = (descriptorLow & WORD_MASK_U) | (descriptorHigh & X86_DESCRIPTOR_LIMIT_HIGH_U);
            /* ★ ONLY RECORD IT IF THE CPU ACTUALLY TOOK IT. g_Ldt[] is the host's
                 record of the REAL LDT, and DpmiSelectorBase() answers every guest
                 pointer translation from it. Writing a descriptor the kernel just
                 rejected would make it a record of what the guest WANTED, which is
                 a different thing and silently wrong exactly where it matters. */
            if (!status) {
                g_Ldt[index].Base   = descriptorBase;
                g_Ldt[index].Limit  = descriptorLimit;
                g_Ldt[index].Access = (BYTE)((descriptorHigh >> BYTE_SHIFT) & BYTE_MASK);
                g_Ldt[index].Flags  = (BYTE)((descriptorHigh >> X86_DESCRIPTOR_FLAGS_SHIFT) & DPMI_DESCRIPTOR_FLAGS_MASK);
            }
            ++count; ++g_WowSyncWrites;
            if (cursor && count <= 6) {
                cursor = LogPut(cursor, "  LDTSYNC idx 0x"); cursor = LogHex(cursor, (DWORD)index);
                cursor = LogPut(cursor, " <- guest wrote base=0x"); cursor = LogHex(cursor, descriptorBase);
                cursor = LogPut(cursor, " limit=0x");               cursor = LogHex(cursor, descriptorLimit);
                cursor = LogPut(cursor, " acc=0x");                 cursor = LogHexByte(cursor, (BYTE)((descriptorHigh >> BYTE_SHIFT) & BYTE_MASK));
                cursor = LogPut(cursor, status ? " INSTALL FAILED (g_ldt NOT updated) st=0x" : " ok st=0x");
                cursor = LogHex(cursor, (DWORD)status);
                cursor = LogPut(cursor, "\r\n");
            }
        }
    }
    if (logCursor) *logCursor = cursor;
    return count;
}

static WORD WowShadowSelector(VOID)
{
    INT ldtIndex, index;
    if (g_WowShadowSelector) return g_WowShadowSelector;
    if (g_LdtNext >= DPMI_LDT_MAX) return 0;
    g_WowShadow = (BYTE *)VirtualAlloc(NULL, WOW_SHADOW_ENTRIES * X86_DESCRIPTOR_SIZE,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_WowShadow) return 0;
    /* The `seen` copy is host-private and is NEVER handed to the guest -- that is
       the point of it. See WowShadowSync for what went wrong without it. */
    g_WowSeen = (BYTE *)VirtualAlloc(NULL, WOW_SHADOW_ENTRIES * X86_DESCRIPTOR_SIZE,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_WowSeen) { VirtualFree(g_WowShadow, 0, MEM_RELEASE);
                       g_WowShadow = NULL; return 0; }
    for (index = 0; index < WOW_SHADOW_ENTRIES * X86_DESCRIPTOR_SIZE; ++index) { g_WowShadow[index] = 0; g_WowSeen[index] = 0; }
    for (index = 0; index < WOW_SHADOW_ENTRIES; ++index) WowShadowPut(index);
    ldtIndex = g_LdtNext++;
    g_Ldt[ldtIndex].Base   = (DWORD)(ULONG_PTR)g_WowShadow;
    g_Ldt[ldtIndex].Limit  = WOW_SHADOW_ENTRIES * X86_DESCRIPTOR_SIZE - 1;
    g_Ldt[ldtIndex].Access = DPMI_ACCESS_DATA;            /* present, DPL3, data R/W -- verw must pass */
    g_Ldt[ldtIndex].Flags  = 0;               /* 16-bit                                    */
    DpmiInstall(ldtIndex);
    g_WowShadowSelector = (WORD)DPMI_LDT_SELECTOR(ldtIndex);
    return g_WowShadowSelector;
}
enum { WOW_VENDOR_STUB_SELECTOR = 0x10, WOW_VENDOR_STUB_LIMIT = 0x1F };   /* WowVendorApiEntry: the mov ax,imm16's operand; the segment */
static INT WowVendorApiEntry(DOS_MACHINE *machine, WORD *selector, WORD *offset)
{
    /* The DPMI vendor-specific API entry krnl386 asks for (INT 2Fh AX=168Ah). Its
       contract, as stock answers it: AX=0 -> AX=0x0100; AX=0x0100 -> AX=a selector;
       anything else -> CF=1, AX unchanged. Our own encoding of that contract; the
       immediate at +0x10 is patched with our shadow selector below. */
    static const BYTE stub[] = {
        X86_OP_CMP_AX_IMM, 0x00, 0x01,        /* +00  cmp ax,0x0100            */
        X86_OP_JZ_SHORT, 0x0A,              /* +03  je   +0x0F               */
        X86_OP_TEST, 0xC0,              /* +05  test ax,ax  (CF=0)       */
        X86_OP_STC,                    /* +07  stc                      */
        X86_OP_JNZ_SHORT, 0x04,              /* +08  jnz  +0x0E  -- not ours  */
        X86_OP_MOV_AH_IMM, 0x01,              /* +0A  mov ah,1    (AX=0x0100)  */
        X86_OP_CLC,                    /* +0C  clc                      */
        X86_OP_RETF,                    /* +0D  retf                     */
        X86_OP_RETF,                    /* +0E  retf        (CF=1)       */
        X86_OP_MOV_IMM_FIRST, 0x00, 0x00,        /* +0F  mov ax,<shadow selector> */
        X86_OP_CLC,                    /* +12  clc                      */
        X86_OP_RETF                     /* +13  retf                     */
    };
    WORD segment = 0, maximum = 0, shadow;
    volatile BYTE *bytes;
    INT ldtIndex, index;

    *offset = 0;
    if (g_WowVendorSelector) { *selector = g_WowVendorSelector; return 0; }
    shadow = WowShadowSelector();
    if (!shadow) return -1;
    /* Its own paragraph rather than a corner of DOS_HDLR_SEG: that segment is at
       linear 0x500 and DOS_DEV_SEG (was DOS_ENV_SEG) starts at 0x600, so it has 0x100 bytes total and
       the map in the header shows them nearly all spoken for. */
    /* Prefer the paragraph WowPlaceV86 set aside. On a WOW launch krnl386 owns
       every other free paragraph by the time this runs, so the fallback below can
       only succeed on a non-WOW path -- and failing here is not cosmetic: krnl386
       treats a missing vendor API as "Inadequate DPMI Server" and exits. */
    segment = WowHostAllocate(1);
    if (!segment && (DosMcbAllocate(NULL, machine->FirstMcb, 1, &segment, &maximum) || !segment)) return -1;
    bytes = (volatile BYTE *)(ULONG_PTR)((DWORD)segment << PARAGRAPH_SHIFT);
    for (index = 0; index < (INT)sizeof stub; ++index) bytes[index] = stub[index];
    bytes[WOW_VENDOR_STUB_SELECTOR] = (BYTE)shadow; bytes[WOW_VENDOR_STUB_SELECTOR + 1] = (BYTE)(shadow >> BYTE_SHIFT);   /* the returned selector */
    if (g_LdtNext >= DPMI_LDT_MAX) return -1;
    ldtIndex = g_LdtNext++;
    g_Ldt[ldtIndex].Base   = (DWORD)segment << PARAGRAPH_SHIFT;
    g_Ldt[ldtIndex].Limit  = WOW_VENDOR_STUB_LIMIT;
    g_Ldt[ldtIndex].Access = DPMI_ACCESS_CODE;            /* present, DPL3, code, readable */
    g_Ldt[ldtIndex].Flags  = 0;               /* 16-bit                        */
    DpmiInstall(ldtIndex);
    g_WowVendorSelector = (WORD)DPMI_LDT_SELECTOR(ldtIndex);
    *selector = g_WowVendorSelector;
    return 0;
}

/* ── s89 (#302): …and WITH A STRUCTURE. `blob` (blobn bytes) is placed on the guest's
     stack below the arguments and its far pointer written into args[blobarg..+1]
     (high word first, as WowCallEnter does for WM_CREATE). After the procedure
     returns the same bytes are copied back into `blob`: WM_MEASUREITEM's answer is
     written INTO the structure. They are intact -- the callee's frame lives below
     the arguments, and nothing runs between the return and the read. */
/* ★ POINTERS INSIDE THE STRUCTURE (`fix`, `nfix`): a CREATESTRUCT names its window
     text and class by far pointer, and the strings travel in the same block. Each
     `fix[i]` is an offset in `blob` holding a WORD offset WITHIN the blob; it is
     rewritten as the far pointer ss:(where the blob landed + that offset) -- known
     only here, because only here is SS:SP known. */
static INT WowCall16Sync(DWORD proc, WORD ds, const WORD *args, INT argumentCount,
                           WORD hwnd, WORD message, WORD *result)
{
    return WowCall16SyncEx(proc, ds, args, argumentCount, hwnd, message, result, NULL, 0, -1, NULL, 0);
}
/* ── ★★ THE WOW32.DLL / NTVDM.EXE STAND-INS (s90, #5/#278). See src/shim/wowshim.c.
     Loaded once, at WOW start-up, by FULL PATH from bin\wowshim\ -- a module of that
     name must be in the process before any 32-bit thunk DLL asks for it by name
     (winmm asks in NotifyCallbackData, the first thing MMSYSTEM calls). */
typedef struct {
    DWORD  Version;
    PVOID (*GetVdmPointer)(DWORD segmentedAddress, DWORD byteCount, BOOL isProtectedMode);
    HANDLE (*Handle32)(WORD handle16, DWORD handleType);
    WORD   (*Handle16)(HANDLE handle32, DWORD handleType);
    BOOL   (*Callback16Ex)(DWORD segmentedFunction, DWORD flags, DWORD argumentBytes, PVOID arguments, DWORD *returnValue);
    VOID   (*IcaInterrupt)(INT picAdapter, BYTE line, INT count);
    VOID   (*Yield16)(VOID);
    VOID   (*Log)(PCSTR message);
    /* version 2 (s91, #309): krnl386's global heap -- see ShimGlobal16 */
    DWORD  (*Global16)(INT operation, DWORD firstArgument, DWORD secondArgument);
    /* version 3 (s91, #11): the VDD service API -- see "THIRD-PARTY VDDs" below */
    DWORD  (*GetRegister)(INT registerIndex);
    VOID   (*SetRegister)(INT registerIndex, DWORD value);
    PVOID (*MapFlat)(WORD segment, DWORD offset, INT isProtectedMode);
    BOOL   (*InstallIoHook)(HANDLE vddHandle, WORD rangeCount, PCVOID ranges, PCVOID handlers);
    VOID   (*RemoveIoHook)(HANDLE vddHandle, WORD rangeCount, PCVOID ranges);
} NTVDMEX_SHIM_API;
static VOID ShimLog(PCSTR what)
{
    CHAR buffer[200], *cursor = buffer;
    cursor = LogPut(cursor, "WOWSHIM: "); cursor = LogPut(cursor, what); cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, buffer, cursor);
}
/* WOWGetVDMPointer(vp, cb, fProtectedMode): a 16:16 PM address through our LDT, or
   seg:off in real mode (which sits at linear 0 of this process, as in NTVDM). */
static PVOID ShimGetVdmPointer(DWORD segmentedAddress, DWORD byteCount, BOOL isProtectedMode)
{
    (VOID)byteCount;
    if (!segmentedAddress) return NULL;
    if (isProtectedMode) {
        DWORD base = DpmiSelectorBase((WORD)(segmentedAddress >> WORD_SHIFT));
        return base ? (VOID *)(ULONG_PTR)(base + (segmentedAddress & WORD_MASK)) : NULL;
    }
    return (VOID *)(ULONG_PTR)(((segmentedAddress >> WORD_SHIFT) << PARAGRAPH_SHIFT) + (segmentedAddress & WORD_MASK));
}
/* WOW_TYPE_*: 0 HWND, 1 HMENU, 4 HDC, 5 HFONT, 6 HMETAFILE, 7 HRGN, 8 HBITMAP,
   9 HBRUSH, 10 HPALETTE, 11 HPEN, 14 FULLHWND. The rest answer 0 and say so. */
static HANDLE ShimHandle32(WORD handle16, DWORD type)
{
    INT kind = -1;
    if (!handle16) return NULL;
    switch (type) {
    case WOW_TYPE_HWND: case WOW_TYPE_FULLHWND: { WOWUSER_WINDOW *window = WowUserFindWindow(handle16); return window ? (HANDLE)window->Window32 : NULL; }
    case WOW_TYPE_HMENU:  return (HANDLE)WowUserMenu32(handle16);
    case WOW_TYPE_HDC: case WOW_TYPE_HFONT: case WOW_TYPE_HMETAFILE: case WOW_TYPE_HRGN: case WOW_TYPE_HBITMAP: case WOW_TYPE_HBRUSH: case WOW_TYPE_HPALETTE: case WOW_TYPE_HPEN:
        return (HANDLE)WowGdiH32(handle16, &kind);
    }
    ShimLog("WOWHandle32: a handle type this host does not map -- 0");
    return NULL;
}
static WORD ShimHandle16(HANDLE handle, DWORD type)
{
    if (!handle) return 0;
    switch (type) {
    case WOW_TYPE_HWND: case WOW_TYPE_FULLHWND: return WowWinHwnd16((HWND)handle);
    case WOW_TYPE_HDC: return WowGdiH16((HGDIOBJ)handle, WOWGDI_KIND_DC);
    case WOW_TYPE_HFONT: case WOW_TYPE_HMETAFILE: case WOW_TYPE_HRGN: case WOW_TYPE_HBITMAP: case WOW_TYPE_HBRUSH: case WOW_TYPE_HPALETTE: case WOW_TYPE_HPEN:
        return WowGdiH16((HGDIOBJ)handle, WOWGDI_KIND_OBJ);
    }
    ShimLog("WOWHandle16: a handle type this host does not map -- 0");
    return 0;
}
static VOID ShimIcaInterrupt(INT isSlave, BYTE line, INT count)
{
    (VOID)count;
    if (line >= PIC_LINES_PER_CHIP) return;
    InterlockedOr(&g_IcaPending, (LONG)(1u << ((isSlave ? PIC_LINES_PER_CHIP : 0) + line)));
    if (++g_IcaRaised <= 8) {
        CHAR buffer[96], *cursor = buffer;
        cursor = LogPut(cursor, "WOWSHIM: call_ica_hw_interrupt ms="); cursor = LogHex(cursor, (DWORD)isSlave);
        cursor = LogPut(cursor, " line="); cursor = LogHex(cursor, line); cursor = LogPut(cursor, " tid=0x");
        cursor = LogHex(cursor, GetCurrentThreadId()); cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, buffer, cursor);
    }
    /* wake a GetMessage wait parked in MsgWaitForMultipleObjects */
    if (g_GuestThreadId && GetCurrentThreadId() != g_GuestThreadId)
        PostThreadMessageA(g_GuestThreadId, WM_NULL, 0, 0);
}
static VOID ShimYield(VOID) { Sleep(0); }
/* ── s91 (#309): WOWCallback16Ex -- a 32-bit thunk DLL calling 16-bit code. pArgs is
     the 16-bit STACK IMAGE, cbArgs bytes, copied as it is (wownt32.h; Wine's
     K32WOWCallback16Ex does the same memcpy): its lowest word is what SP points at,
     i.e. a PASCAL function's LAST argument. WowCall16Sync takes arguments in
     declared order (first pushed first), so the buffer is read from its top down.
     WCB16_CDECL needs nothing more: the nested run restores SS:SP itself. DS is the
     calling task's, as it was when the guest called into the thunk. Only on the
     guest thread -- from any other, FALSE (the nested run refuses). */
static BOOL ShimCallback16Ex(DWORD targetProcedure, DWORD flags, DWORD byteCount, PVOID arguments, DWORD *result)
{
    WORD args[WOWCALL_MAX_ARGW], callResult = 0;
    const BYTE *argumentBytes = (const BYTE *)arguments;
    INT wordCount = (INT)(byteCount / WOW_WORD_BYTES), index;
    (VOID)flags;
    if ((byteCount & 1) || wordCount > WOWCALL_MAX_ARGW || (byteCount && !argumentBytes) || !g_TibDebug) {
        ShimLog("WOWCallback16Ex: refused (odd or > 64 argument bytes)");
        return FALSE;
    }
    for (index = 0; index < wordCount; ++index)
        args[index] = (WORD)(argumentBytes[byteCount - WOW_WORD_BYTES - WOW_WORD_BYTES * index] | (argumentBytes[byteCount - 1 - WOW_WORD_BYTES * index] << BYTE_SHIFT));
    if (!WowCall16Sync(targetProcedure, (WORD)VDM_REG16(g_TibDebug, VTIB_DS),
                         args, wordCount, 0, 0, &callResult)) {
        ShimLog("WOWCallback16Ex: the nested run could not make the call");
        return FALSE;
    }
    if (result) *result = g_WowCallLastResult;
    return TRUE;
}
/* ── s91 (#309): WOWGlobal*16 -- krnl386's OWN global heap, through its exports
     (offsets in its segment 1, read off guest/win16/krnl386.exe's entry table:
     15 GlobalAlloc 3ac3, 17 GlobalFree 3adf, 18 GlobalLock 3b10, 19 GlobalUnlock
     3b63, 20 GlobalSize 3b4f, 21 GlobalHandle 3afc). The shim composes the
     AllocLock/UnlockFree/LockSize forms from these. 0 when the call cannot be made. */
static DWORD ShimGlobal16(INT operation, DWORD firstArgument, DWORD secondArgument)
{
    static const WORD offset[SHIM_GLOBAL_OPERATIONS] = { 0x3ac3, 0x3adf, 0x3b10, 0x3b63, 0x3b4f, 0x3afc };
    WORD args[3], result = 0;
    INT argumentCount;
    if (operation < 0 || operation >= SHIM_GLOBAL_OPERATIONS || !g_WowUserKernelSegment || !g_TibDebug) return 0;
    if (operation == SHIM_GLOBAL_ALLOC) { args[0] = (WORD)firstArgument; args[1] = (WORD)(secondArgument >> WORD_SHIFT); args[2] = (WORD)secondArgument; argumentCount = 3; }
    else         { args[0] = (WORD)firstArgument; argumentCount = 1; }
    if (!WowCall16Sync(((DWORD)g_WowUserKernelSegment << WORD_SHIFT) | offset[operation],
                         (WORD)VDM_REG16(g_TibDebug, VTIB_DS), args, argumentCount, 0, 0, &result))
        return 0;
    /* GlobalLock / GlobalSize / GlobalHandle answer in DX:AX; the rest in AX */
    return (operation == SHIM_GLOBAL_LOCK || operation == SHIM_GLOBAL_SIZE || operation == SHIM_GLOBAL_HANDLE) ? g_WowCallLastResult : (DWORD)(WORD)g_WowCallLastResult;
}
enum { WOW_WINDOW_NESTING_MAX = 6, WOW_CALL16_PHASE_MAX = 500000 };   /* WowCall16SyncEx */
/* ══ THIRD-PARTY VDDs, MICROSOFT ABI (s91, #11). ════════════════════════════════════
     A DOS program with a VDD of its own registers it with the third-party BOP,
     `C4 C4 58 nn` (the DDK's isvbop.inc): nn=0 RegisterModule (DS:SI the DLL, DS:DI
     its init routine's name, DS:BX its dispatch routine's name; CF clear + AX = a
     handle, or CF set + AX = 1 no DLL / 2 no dispatch routine / 3 no init routine),
     nn=1 UnRegisterModule (AX = handle), nn=2 DispatchCall (AX = handle; the VDD
     reads and writes the caller's registers). The VDD calls back through what
     NTVDM.EXE exports -- getAX/setAX..., VdmMapFlat, VDDInstallIOHook -- which in
     this process is bin\wowshim\NTVDM.EXE (src/shim/wowshim.c), loaded before the VDD
     so its import of "NTVDM.EXE" resolves to it by name. These are the host halves. */
enum { SHIM_R_EAX, SHIM_R_EBX, SHIM_R_ECX, SHIM_R_EDX, SHIM_R_ESI, SHIM_R_EDI, SHIM_R_EBP,
       SHIM_R_ESP, SHIM_R_EIP, SHIM_R_CS, SHIM_R_DS, SHIM_R_ES, SHIM_R_SS, SHIM_R_FS,
       SHIM_R_GS, SHIM_R_EFL, SHIM_R_MSW };
static const INT g_ShimTibOffsets[16] = { VTIB_EAX, VTIB_EBX, VTIB_ECX, VTIB_EDX, VTIB_ESI,
    VTIB_EDI, VTIB_EBP, VTIB_ESP, VTIB_EIP, VTIB_CS, VTIB_DS, VTIB_ES, VTIB_SS, VTIB_FS,
    VTIB_GS, VTIB_EFLAGS };
static DWORD ShimGetRegister(INT registerIndex)
{
    volatile BYTE *tib = g_TibDebug;
    if (!tib) return 0;
    if (registerIndex == SHIM_R_MSW) return g_DpmiPm ? 1u : 0u;
    if (registerIndex < 0 || registerIndex > SHIM_R_EFL) return 0;
    if (registerIndex >= SHIM_R_CS && registerIndex <= SHIM_R_GS) return VDM_REG16(tib, g_ShimTibOffsets[registerIndex]);
    return VDM_REG(tib, g_ShimTibOffsets[registerIndex]);
}
static VOID ShimSetRegister(INT registerIndex, DWORD value)
{
    volatile BYTE *tib = g_TibDebug;
    if (!tib || registerIndex < 0 || registerIndex > SHIM_R_EFL) return;
    if (registerIndex >= SHIM_R_CS && registerIndex <= SHIM_R_GS) VDM_SET16(tib, g_ShimTibOffsets[registerIndex], (WORD)value);
    else VDM_REG(tib, g_ShimTibOffsets[registerIndex]) = value;
}
PVOID ShimMapFlat(WORD segment, DWORD offset, INT isProtectedMode)
{
    if (isProtectedMode) { DWORD base = DpmiSelectorBase(segment); return base ? (VOID *)(ULONG_PTR)(base + offset) : NULL; }
    return (VOID *)(ULONG_PTR)(((DWORD)segment << PARAGRAPH_SHIFT) + (offset & WORD_MASK));
}
static BOOL ShimInstallIoHook(HANDLE vddHandle, WORD rangeCount, PCVOID ranges, PCVOID handlers)
{
    const WORD *ranges16 = (const WORD *)ranges;
    WORD index2;
    CHAR buffer[160], *cursor;
    if (!ranges || !handlers) return FALSE;
    for (index2 = 0; index2 < rangeCount; ++index2) {
        INT index, slot = -1;
        for (index = 0; index < ISV_MAX_HOOKS; ++index)
            if (!g_IsvHooks[index].VddHandle || (g_IsvHooks[index].VddHandle == vddHandle && !g_IsvHooks[index].IsLive
                                        && g_IsvHooks[index].FirstPort == ranges16[index2 * 2])) { slot = index; break; }
        if (slot < 0) return FALSE;
        if (!g_IsvHooks[slot].VddHandle
            && VddClaimPorts(&g_Bus, ranges16[index2 * 2], ranges16[index2 * 2 + 1], IsvIoIn, IsvIoOut,
                               (VOID *)(ULONG_PTR)slot) != 0) return FALSE;
        g_IsvHooks[slot].VddHandle = vddHandle;
        g_IsvHooks[slot].FirstPort = ranges16[index2 * 2]; g_IsvHooks[slot].LastPort = ranges16[index2 * 2 + 1];
        g_IsvHooks[slot].Handlers = *(const ISV_IO_HANDLERS *)handlers;
        g_IsvHooks[slot].IsLive = 1;
        cursor = buffer; cursor = LogPut(cursor, "ISVVDD: I/O hook 0x"); cursor = LogHex(cursor, ranges16[index2 * 2]);
        cursor = LogPut(cursor, "-0x"); cursor = LogHex(cursor, ranges16[index2 * 2 + 1]); cursor = LogPut(cursor, " installed\r\n");
        LogAppend(LOG_PATH, buffer, cursor);
    }
    return TRUE;
}
static VOID ShimRemoveIoHook(HANDLE vddHandle, WORD rangeCount, PCVOID ranges)
{
    const WORD *ranges16 = (const WORD *)ranges;
    WORD index2; INT index;
    if (!ranges) return;
    for (index2 = 0; index2 < rangeCount; ++index2)
        for (index = 0; index < ISV_MAX_HOOKS; ++index)
            if (g_IsvHooks[index].VddHandle == vddHandle && g_IsvHooks[index].FirstPort == ranges16[index2 * 2]) g_IsvHooks[index].IsLive = 0;
}
enum { WOW_WNDPROC_ARGUMENTS = 5 };   /* a Win16 window procedure: hwnd, msg, wParam, lParam high, low */
VOID WowShimsLoad(VOID)
{
    static INT done;
    static NTVDMEX_SHIM_API shimApi;
    static PCSTR const names[] = { "WOW32.DLL", "NTVDM.EXE" };
    CHAR path[MAX_PATH], buffer[MAX_PATH + 160], *cursor;
    UINT index;
    if (done) return;
    done = 1;
    /* SAFE MODE (#132): read the counter here -- this runs before the recovery
       decision is taken further down WinMain, and must not wait for it. */
    if (DosRecoveryGetSafeSkips(DosRecoveryDecideStartMode(RecoveryRead())).WowShims) return;
    shimApi.Version = SHIM_API_VERSION; shimApi.GetVdmPointer = ShimGetVdmPointer; shimApi.Handle32 = ShimHandle32;
    shimApi.Handle16 = ShimHandle16; shimApi.Callback16Ex = ShimCallback16Ex; shimApi.IcaInterrupt = ShimIcaInterrupt;
    shimApi.Yield16 = ShimYield; shimApi.Log = ShimLog; shimApi.Global16 = ShimGlobal16;
    shimApi.GetRegister = ShimGetRegister; shimApi.SetRegister = ShimSetRegister; shimApi.MapFlat = ShimMapFlat;
    shimApi.InstallIoHook = ShimInstallIoHook; shimApi.RemoveIoHook = ShimRemoveIoHook;
    for (index = 0; index < WOW_SHIMS; ++index) {
        HMODULE module;
        BOOL (WINAPI *initialize)(const NTVDMEX_SHIM_API *);
        INT length = 0;
        PCSTR source = NTVDMEX_DIR;
        while (*source && length < MAX_PATH - 40) path[length++] = *source++;
        source = HOST_SHIM_SUBDIRECTORY; while (*source) path[length++] = *source++;
        source = names[index]; while (*source) path[length++] = *source++;
        path[length] = 0;
        module = LoadLibraryExA(path, NULL, 0);
        initialize = module ? (BOOL (WINAPI *)(const NTVDMEX_SHIM_API *))GetProcAddress(module, HOST_EXPORT_SHIM_INIT)
                 : NULL;
        cursor = buffer; cursor = LogPut(cursor, "WOWSHIM: "); cursor = LogPut(cursor, names[index]);
        if (!module) { g_ShimState[index] = SHIM_NO_LOAD; g_ShimError[index] = GetLastError();
                  cursor = LogPut(cursor, " NOT LOADED from ["); cursor = LogPut(cursor, path);
                  cursor = LogPut(cursor, "] err=0x"); cursor = LogHex(cursor, g_ShimError[index]); }
        else if (!initialize || !initialize(&shimApi)) { g_ShimState[index] = SHIM_INIT_REFUSED;
                  cursor = LogPut(cursor, " loaded but its init REFUSED the table"); }
        else { g_ShimState[index] = SHIM_LOADED; cursor = LogPut(cursor, " loaded at 0x"); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)module); }
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, buffer, cursor);
    }
}
static INT WowCall16SyncEx(DWORD proc, WORD ds, const WORD *args, INT argumentCount,
                              WORD hwnd, WORD message, WORD *result,
                              BYTE *blob, INT blobLength, INT blobArgument,
                              const INT *fix, INT fixupCount)
{
    volatile BYTE *tib = g_TibDebug;
    INT depthBefore = g_WowCallDepth, isOk;
    WORD sink = 0, callbackSelector, blobStackPointer = 0;
    DWORD stackSegmentBase;
    UINT phase;
    if (!tib || !g_DpmiPm || !g_WowLaunch || !g_DosMachine || !g_Running) return 0;
    if (GetCurrentThreadId() != g_GuestThreadId) return 0;
    if (g_WowCallDepth >= WOWCALL_MAX_DEPTH - 1 || g_WowWindowNested >= WOW_WINDOW_NESTING_MAX || !(proc >> WORD_SHIFT)) return 0;
    callbackSelector = WowCallbackSelector();
    stackSegmentBase  = DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_SS));
    if (!callbackSelector || !stackSegmentBase) return 0;
    if (blob && blobLength > 0) {
        WORD ss = (WORD)VDM_REG16(tib, VTIB_SS);
        INT fixIndex;
        blobStackPointer = (WORD)(VDM_REG16(tib, VTIB_ESP) - ((blobLength + 1) & ~1));
        for (fixIndex = 0; fix && fixIndex < fixupCount; ++fixIndex) {
            INT fixOffset = fix[fixIndex];
            WORD fixupValue;
            if (fixOffset < 0 || fixOffset + X86_FAR_POINTER_SIZE > blobLength) continue;
            fixupValue = (WORD)(blob[fixOffset] | (blob[fixOffset + 1] << BYTE_SHIFT));
            fixupValue = (WORD)(blobStackPointer + fixupValue);
            blob[fixOffset] = (BYTE)fixupValue; blob[fixOffset + 1] = (BYTE)(fixupValue >> BYTE_SHIFT);
            blob[fixOffset + X86_FAR_POINTER_SEGMENT] = (BYTE)ss; blob[fixOffset + X86_FAR_POINTER_SEGMENT + 1] = (BYTE)(ss >> BYTE_SHIFT);
        }
    }
    if (!WowCallEnter(tib, stackSegmentBase, callbackSelector, proc, ds, args, argumentCount, 0, WOWCALL_RET_KEEP, &sink,
                       hwnd, message, blob, blob ? blobLength : 0, blob ? blobArgument : -1,
                       WowDlgIsSelectorAbsent((WORD)(proc >> WORD_SHIFT))))
        return 0;
    ++g_WowWindowNested;
    for (phase = 0; phase < WOW_CALL16_PHASE_MAX && g_WowCallDepth > depthBefore && g_Running; ++phase) {
        DWORD event, eip, vector; INT status;
        DpmiArmFaultTrampoline(tib, 0);
        DpmiEnterProtectedMode(tib);
        event  = VDM_REG(tib, VTIB_EVENT);
        eip = DpmiPmEip(tib);
        if (event == 3) continue;
        if (event == VDM_EVENT_IO || event == VDM_EVENT_IO_HW || event == VDM_EVENT_GPFAULT) {
            INT ioHandled;
            HOST_LOCK();
            ioHandled = HostTryIoPm(tib, &g_Bus);
            HOST_UNLOCK();
            if (ioHandled) continue;
        }
        if (DpmiNestedFault(tib, event, eip)) continue;      /* s90: see the helper */
        vector = (event == VDM_EVENT_BOP) ? DpmiBopVector(VDM_REG16(tib, VTIB_CS), eip) : 0;
        status = DpmiServicePmInt(g_DosMachine, tib, vector, 0);
        if (status <= 0) break;
    }
    --g_WowWindowNested;
    isOk = (g_WowCallDepth == depthBefore);
    if (!isOk) {
        CHAR buffer[160], *cursor = buffer;
        while (g_WowCallDepth > depthBefore) WowCallLeave(tib, 0);
        cursor = LogPut(cursor, "WOWNEST: ★ the nested call to 0x"); cursor = LogHex(cursor, proc);
        cursor = LogPut(cursor, " (msg 0x"); cursor = LogHex(cursor, message);
        cursor = LogPut(cursor, ") did not return -- frame unwound, Windows' default used\r\n");
        LogAppend(LOG_PATH, buffer, cursor);
    }
    if (isOk && blob && blobLength > 0) {
        INT index;
        for (index = 0; index < blobLength; ++index)
            blob[index] = *(volatile BYTE *)(ULONG_PTR)(stackSegmentBase + (DWORD)(WORD)(blobStackPointer + index));
    }
    if (result) *result = sink;
    return isOk;
}
/* ── WM_CTLCOLOR, ANSWERED BY THE PROGRAM (s89, #162). Win32's seven WM_CTLCOLOR*
     are Win16's one WM_CTLCOLOR (0x0019) with the type in lParam's HIGH word
     (MSGBOX 0 .. STATIC 6, in the same order). The program gets a DC token for the
     real DC -- SetTextColor/SetBkColor on it land on the control's own DC -- and
     answers a brush token, mapped back to the real brush. 0 or a refusal leaves
     Windows' default. Calc's display, Cardfile's card bar and Packager's headers are
     drawn in colours their programs chose here. */
static LRESULT WowControlColour(HWND window, WORD window16, UINT message, WPARAM wParam, LPARAM lParam, INT *handled)
{
    WOWUSER_WINDOW *wowWindow = WowUserFindWindow(window16);
    DWORD proc = wowWindow ? WowUserWindowProcedureOf(wowWindow) : 0;
    WORD  deviceContext16, child, args[WOW_WNDPROC_ARGUMENTS], result = 0, type;
    INT   kind = -1, made;
    HGDIOBJ brush;
    *handled = 0;
    if (!proc) return 0;
    child = WowWinHwnd16((HWND)lParam);
    deviceContext16  = WowGdiH16((HGDIOBJ)wParam, WOWGDI_KIND_DC);
    if (!deviceContext16) return 0;
    /* A Win32 read-only edit reports WM_CTLCOLORSTATIC; a Win16 edit is always
       CTLCOLOR_EDIT -- Calc's display is one, and its default is the window colour. */
    {   CHAR className[16];
        type = (WORD)(message - WM_CTLCOLORMSGBOX);
        if (type == CTLCOLOR_STATIC && GetClassNameA((HWND)lParam, className, sizeof className) && !lstrcmpiA(className, WC_EDITA))
            type = 1;
    }
    args[0] = window16; args[1] = WM_CTLCOLOR16;
    args[2] = deviceContext16;
    args[3] = type;                              /* lParam HIGH: the control type */
    args[4] = child;                             /* lParam LOW: the control       */
    made = WowCall16Sync(proc, wowWindow->Instance ? wowWindow->Instance : g_WowUserClasses[wowWindow->Class].Instance,
                           args, WOW_WNDPROC_ARGUMENTS, window16, WM_CTLCOLOR16, &result);
    WowGdiForget(deviceContext16);
    if (made && result) {
        brush = WowGdiH32(result, &kind);
        if (brush && kind == WOWGDI_KIND_OBJ) { *handled = 1; return (LRESULT)brush; }
    }
    /* ── THE DEFAULT A 3.x PROGRAM GETS, as stock's USER32 gives it (measured against
         stock on the rig, s89): edit and list boxes are the WINDOW colour; static
         text and buttons are the 3-D face inside a DIALOG (16-bit dialogs get the 3-D
         look when its template names a font -- Charmap's labels) and the WINDOW
         colour otherwise (Calc's display; Cardfile's card bar, Packager's headers). Scroll bars and the dialog's own
         background keep Windows' default. */
    if (type == CTLCOLOR_EDIT || type == CTLCOLOR_LISTBOX || ((type == CTLCOLOR_BTN || type == CTLCOLOR_STATIC) && !wowWindow->IsDialog3D)) {
        SetTextColor((HDC)wParam, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor((HDC)wParam, GetSysColor(COLOR_WINDOW));
        *handled = 1;
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    return 0;
}
/* ── s89 (#302 M3): OWNER-DRAW, ANSWERED BY THE PROGRAM. Windows SENDS the four
     owner-draw messages to a control's parent and needs the answer before it goes
     on -- the control's size before it is laid out (MEASUREITEM), the pixels before
     the paint ends (DRAWITEM) -- so they go through the nested run like WM_CTLCOLOR,
     each with its structure converted to the Win16 layout on the guest's stack:
       DRAWITEMSTRUCT    26 bytes: 5 WORDs, hwndItem, hDC (a DC token), RECT of 4
                         INT16s, DWORD itemData  (Win32: 48)
       MEASUREITEMSTRUCT 14 bytes: 5 WORDs, DWORD itemData -- width/height COPIED BACK
       DELETEITEMSTRUCT  12 bytes: 3 WORDs, hwndItem, DWORD itemData
       COMPAREITEMSTRUCT 18 bytes: 2 WORDs, hwndItem, id1, DWORD data1, id2, DWORD
                         data2 -- the answer is the return value (-1/0/1)
     Win16 itemState has only the first five ODS_ bits. A refusal (no procedure, no
     nested run possible) leaves Windows' own handling. */
static VOID OwnerDrawPutWord(BYTE *bytes, INT offset, WORD value) { bytes[offset] = (BYTE)value; bytes[offset + 1] = (BYTE)(value >> BYTE_SHIFT); }
static VOID OwnerDrawPutDword(BYTE *bytes, INT offset, DWORD value) { OwnerDrawPutWord(bytes, offset, (WORD)value); OwnerDrawPutWord(bytes, offset + X86_WORD_SIZE, (WORD)(value >> WORD_SHIFT)); }
static WORD OwnerDrawGetWord(const BYTE *bytes, INT offset) { return (WORD)(bytes[offset] | (bytes[offset + 1] << BYTE_SHIFT)); }
static LRESULT WowOwnerDraw(HWND window, WORD window16, UINT message, WPARAM wParam, LPARAM lParam, INT *handled)
{
    WOWUSER_WINDOW *wowWindow = WowUserFindWindow(window16);
    DWORD proc = wowWindow ? WowUserWindowProcedureOf(wowWindow) : 0;
    WORD args[WOW_WNDPROC_ARGUMENTS], result = 0, deviceContext16 = 0;
    BYTE bytes[32];
    INT length = 0, index, made;
    *handled = 0;
    if (!proc || !lParam) return 0;
    for (index = 0; index < (INT)sizeof bytes; ++index) bytes[index] = 0;
    switch (message) {
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *drawItem = (const DRAWITEMSTRUCT *)lParam;
        deviceContext16 = WowGdiH16((HGDIOBJ)drawItem->hDC, WOWGDI_KIND_DC);
        if (!deviceContext16) return 0;
        OwnerDrawPutWord(bytes, WOWUSER_OWNERDRAW16_CTLTYPE, (WORD)drawItem->CtlType); OwnerDrawPutWord(bytes, WOWUSER_OWNERDRAW16_CTLID, (WORD)drawItem->CtlID);
        OwnerDrawPutWord(bytes, WOWUSER_DRAWITEM16_ITEMID, (WORD)drawItem->itemID);  OwnerDrawPutWord(bytes, WOWUSER_DRAWITEM16_ITEMACTION, (WORD)drawItem->itemAction);
        OwnerDrawPutWord(bytes, WOWUSER_DRAWITEM16_ITEMSTATE, (WORD)(drawItem->itemState & WOWUSER_ODS16_MASK));
        OwnerDrawPutWord(bytes, WOWUSER_DRAWITEM16_HWNDITEM, WowWinHwnd16(drawItem->hwndItem)); OwnerDrawPutWord(bytes, WOWUSER_DRAWITEM16_HDC, deviceContext16);
        OwnerDrawPutWord(bytes, WOWUSER_DRAWITEM16_RCITEM_LEFT, (WORD)drawItem->rcItem.left);  OwnerDrawPutWord(bytes, WOWUSER_DRAWITEM16_RCITEM_TOP, (WORD)drawItem->rcItem.top);
        OwnerDrawPutWord(bytes, WOWUSER_DRAWITEM16_RCITEM_RIGHT, (WORD)drawItem->rcItem.right); OwnerDrawPutWord(bytes, WOWUSER_DRAWITEM16_RCITEM_BOTTOM, (WORD)drawItem->rcItem.bottom);
        OwnerDrawPutDword(bytes, WOWUSER_DRAWITEM16_ITEMDATA, (DWORD)drawItem->itemData);
        length = WOWUSER_DRAWITEM16_SIZE; break;
    }
    case WM_MEASUREITEM: {
        const MEASUREITEMSTRUCT *measureItem = (const MEASUREITEMSTRUCT *)lParam;
        OwnerDrawPutWord(bytes, WOWUSER_OWNERDRAW16_CTLTYPE, (WORD)measureItem->CtlType); OwnerDrawPutWord(bytes, WOWUSER_OWNERDRAW16_CTLID, (WORD)measureItem->CtlID);
        OwnerDrawPutWord(bytes, WOWUSER_MEASUREITEM16_ITEMID, (WORD)measureItem->itemID);  OwnerDrawPutWord(bytes, WOWUSER_MEASUREITEM16_ITEMWIDTH, (WORD)measureItem->itemWidth);
        OwnerDrawPutWord(bytes, WOWUSER_MEASUREITEM16_ITEMHEIGHT, (WORD)measureItem->itemHeight); OwnerDrawPutDword(bytes, WOWUSER_MEASUREITEM16_ITEMDATA, (DWORD)measureItem->itemData);
        length = WOWUSER_MEASUREITEM16_SIZE; break;
    }
    case WM_DELETEITEM: {
        const DELETEITEMSTRUCT *drawItem = (const DELETEITEMSTRUCT *)lParam;
        OwnerDrawPutWord(bytes, WOWUSER_OWNERDRAW16_CTLTYPE, (WORD)drawItem->CtlType); OwnerDrawPutWord(bytes, WOWUSER_OWNERDRAW16_CTLID, (WORD)drawItem->CtlID);
        OwnerDrawPutWord(bytes, WOWUSER_DELETEITEM16_ITEMID, (WORD)drawItem->itemID);  OwnerDrawPutWord(bytes, WOWUSER_DELETEITEM16_HWNDITEM, WowWinHwnd16(drawItem->hwndItem));
        OwnerDrawPutDword(bytes, WOWUSER_DELETEITEM16_ITEMDATA, (DWORD)drawItem->itemData);
        length = WOWUSER_DELETEITEM16_SIZE; break;
    }
    case WM_COMPAREITEM: {
        const COMPAREITEMSTRUCT *compareItem = (const COMPAREITEMSTRUCT *)lParam;
        OwnerDrawPutWord(bytes, WOWUSER_OWNERDRAW16_CTLTYPE, (WORD)compareItem->CtlType); OwnerDrawPutWord(bytes, WOWUSER_OWNERDRAW16_CTLID, (WORD)compareItem->CtlID);
        OwnerDrawPutWord(bytes, WOWUSER_COMPAREITEM16_HWNDITEM, WowWinHwnd16(compareItem->hwndItem));
        OwnerDrawPutWord(bytes, WOWUSER_COMPAREITEM16_ITEMID1, (WORD)compareItem->itemID1); OwnerDrawPutDword(bytes, WOWUSER_COMPAREITEM16_ITEMDATA1, (DWORD)compareItem->itemData1);
        OwnerDrawPutWord(bytes, WOWUSER_COMPAREITEM16_ITEMID2, (WORD)compareItem->itemID2); OwnerDrawPutDword(bytes, WOWUSER_COMPAREITEM16_ITEMDATA2, (DWORD)compareItem->itemData2);
        length = WOWUSER_COMPAREITEM16_SIZE; break;
    }
    default: return 0;
    }
    args[0] = window16; args[1] = (WORD)message; args[2] = (WORD)wParam;
    args[3] = 0; args[4] = 0;                    /* lParam: the structure's far pointer */
    made = WowCall16SyncEx(proc, wowWindow->Instance ? wowWindow->Instance : g_WowUserClasses[wowWindow->Class].Instance,
                              args, ARRAYSIZE(args), window16, (WORD)message, &result, bytes, length, WOWUSER_WNDPROC_ARG_LPARAM, NULL, 0);
    if (deviceContext16) WowGdiForget(deviceContext16);
    if (!made) return 0;
    *handled = 1;
    if (message == WM_MEASUREITEM) {
        MEASUREITEMSTRUCT *measureItem = (MEASUREITEMSTRUCT *)lParam;
        measureItem->itemWidth  = OwnerDrawGetWord(bytes, WOWUSER_MEASUREITEM16_ITEMWIDTH);
        measureItem->itemHeight = OwnerDrawGetWord(bytes, WOWUSER_MEASUREITEM16_ITEMHEIGHT);
        return TRUE;
    }
    if (message == WM_COMPAREITEM) return (LRESULT)(INT16)result;
    return (LRESULT)(result ? TRUE : FALSE);
}
/* s89 (#305 M10): a message SENT to a guest window, now -- its own procedure and
   instance chosen exactly as DispatchMessage chooses them. WowUserDestroy uses it
   so WM_DESTROY arrives while the window and its children still exist. */
/* ...and with a structure as lParam (see WowCall16SyncEx for `fix`). */
static INT WowSend16Blob(WORD window16, WORD message, WORD wParam, BYTE *blob, INT blobLength,
                           const INT *fix, INT fixupCount, WORD *result)
{
    WOWUSER_WINDOW *window = WowUserFindWindow(window16);
    DWORD proc = window ? WowUserWindowProcedureOf(window) : 0;
    WORD args[WOW_WNDPROC_ARGUMENTS];
    if (!proc) return 0;
    args[0] = window16; args[1] = message; args[2] = wParam; args[3] = 0; args[4] = 0;
    return WowCall16SyncEx(proc, window->Instance ? window->Instance : g_WowUserClasses[window->Class].Instance,
                              args, WOW_WNDPROC_ARGUMENTS, window16, message, result, blob, blobLength, 3, fix, fixupCount);
}
static INT WowSend16Now(WORD window16, WORD message, WORD wParam, DWORD lParam, WORD *result)
{
    WOWUSER_WINDOW *window = WowUserFindWindow(window16);
    DWORD proc = window ? WowUserWindowProcedureOf(window) : 0;
    WORD args[WOW_WNDPROC_ARGUMENTS];
    if (!proc) return 0;
    args[0] = window16; args[1] = message; args[2] = wParam;
    args[3] = (WORD)(lParam >> WORD_SHIFT); args[4] = (WORD)(lParam & WORD_MASK);
    return WowCall16Sync(proc, window->Instance ? window->Instance : g_WowUserClasses[window->Class].Instance,
                           args, WOW_WNDPROC_ARGUMENTS, window16, message, result);
}
/* ── s90 (#278): deliver IRQs a 32-bit component raised (call_ica_hw_interrupt via
     bin\wowshim\NTVDM.EXE) to the client's PM handlers. Called from the main PM loop
     AND from the WOW GetMessage wait: a Win16 program spends a sound's whole playback
     parked in GetMessage, and on real hardware IRQ 10 would interrupt that idle task,
     MMSYSTEM's handler would post MM_WOM_DONE, and GetMessage would return it -- so
     the wait must deliver too, or the callback arrives never. */
static VOID WowIcaDeliver(DOS_MACHINE *machine, volatile BYTE *tib, UINT steps)
{
    LONG bits;
    UINT line;
    if (!g_IcaPending || g_InPmIrq || g_PmNoIrq || g_AsyncPmActive) return;
    bits = InterlockedExchange(&g_IcaPending, 0);
    for (line = 0; line < PIC_LINES; ++line) {
        UINT interruptVector;
        if (!(bits & (1L << line))) continue;
        interruptVector = (line < PIC_LINES_PER_CHIP) ? PIC_MASTER_VECTOR_BASE + line : PIC_SLAVE_VECTOR_BASE + (line - PIC_LINES_PER_CHIP);
        if (!g_PmInt[interruptVector].Client) { ++g_IcaNoHandler; continue; }
        g_InPmIrq = 1;
        if (DpmiInjectPmIrq(machine, tib, interruptVector, steps)) ++g_IcaDelivered;
        else InterlockedOr(&g_IcaPending, (LONG)(1L << line));       /* retry later */
        g_InPmIrq = 0;
    }
}
