/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The exec loop: NTVDM's own commands and BOP service, the V86 guest's run, its I/O events and interrupts, and HostRunExecLoop.
 *
 * Its own translation unit (#335): declared in host_exec.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */
#include "host_exec.h"
#include "host_state.h"
#include "bios_kbdact.h"
#include "main.h"
#include "host_audio.h"
#include "host_bios.h"
#include "host_dos.h"
#include "host_dpmi_client.h"
#include "host_input.h"
#include "host_io.h"
#include "host_irq.h"
#include "host_mouse.h"
#include "host_timing.h"
#include "host_video.h"
#include "host_window.h"

#define NTVDM_BOP_DOS   0x50                /* XP's COMMAND.COM: 1 site, in its version-refusal path */

/* How to answer an NTVDM BOP we have not implemented yet: contents of cfg\bop54.txt.
 * A knob because the right answer is UNKNOWN and is being measured -- see the handler.
 */
#define BOP54_PATH      CFG_("bop54.txt")

/* A command line to hand the shell ONCE through BOP 0x54 sub 01, so the path can be
 * tested end-to-end rather than only 'it accepted an empty answer'.
 */
#define BOPCMD_PATH     CFG_("bopcmd.txt")

/* The startup batch file BOP 0x54 sub 0x0D hands the shell. Stock NTVDM names
 * AUTOEXEC.NT here; ours defaults to the DOS-native AUTOEXEC.BAT. See the handler.
 */
#define BOPAUTO_PATH    CFG_("autoexec.txt")

/* The mode-12h trap-storm escape hatch. By default V86 runs on the real CPU and
 * each VGA access (memory OR port) is emulated one-at-a-time as a device access
 * -- pure device virtualization. But QuickBasic plots pixels one at a time and
 * reprograms a VGA register via OUT *between* pixels, so a fill is hundreds of
 * thousands of fault round-trips (port faults AND memory faults) and crawls.
 *
 * HostInterp() is the opt-in batching interpreter: load the V86 register file,
 * run up to `cap` instructions in the host (the inner loop -- planar A0000
 * access, IN/OUT through the bus, ALU, CALL/RET, branches), then write the
 * architectural state back. The caller (the service loop) engages it ONLY on a
 * detected trap-storm (the same tight PC window faulting repeatedly), so it's a
 * measured fallback for proven pathological video loops, not a blanket policy.
 * Returns the number of instructions executed (0 if the faulting instruction
 * itself is unmodeled -> caller falls through).
 */
#define STORM_WINDOW    128                 /* Faults within this PC span count as "the same loop" */

#define STORM_GATE      8                   /* Consecutive in-window faults -> escalate to the interpreter */

#define TIER1_CAP       2000000L            /* Interpreter iteration ceiling once escalated */

#define P12_SLICE       20000L              /* Planar mode: instructions per interpreter slice */

#define UNIMPLEMENTED_BOP_EXIT_CODE 0xBD    /* A guest killed by an unanswered BOP: never a clean 0 */

enum
{
    ENVIRONMENT_SCAN_MAX = 900, PATH_VALUE_MAX = 250
};   /* sub 0Fh's environment snapshot */

static INT NtvdmCommandStartupBatch(
    PSTR *cursorIo,
    PSTR const base,
    const DWORD bopNumber,
    const DWORD sub,
    volatile BYTE * const tib,
    const INT quiet)
{
    PSTR cursor = *cursorIo;

    /* sub 0D = "GIVE ME A PATH TO OPEN" -- THE STARTUP BATCH FILE (Importance = 1):
     * The guest named this gap itself. With `/p` on its command line COMMAND.COM
     * allocates a 7-paragraph block (`AH=48h -> 0x0D6D`), issues this BOP with
     * `DS:DX` pointing into it, and its very next DOS call is `AH=3Dh` (open)
     * on that buffer (our INT 21h log). We wrote nothing, so it opened "" and
     * our DOS answered "path not found".
     * - Stock answers with a path, as an OEM string at DS:DX, of at most
     *   **0x40** bytes.
     * - Which path: stock's permanent shell runs AUTOEXEC.NT at start-up, and
     *   this is the `/p` (permanent shell) startup path. [CAUTION] That last step is an
     *   INFERENCE from context -- but it is verifiable by behaviour, because
     *   whatever we write here is the path the guest opens next.
     *
     * [WARNING]: WE DO NOT DEFAULT TO XP's AUTOEXEC.NT, deliberately. The real one loads
     * `mscdexnt.exe`, `redir` and **`dosx`** -- NT's DPMI host, which we provide
     * ourselves and which has no business being loaded into our VDM. The
     * DOS-native `C:\AUTOEXEC.BAT` is the honest default for a DOS that is
     * ours; `cfg\autoexec.txt` points it anywhere, including at NT's.
     *
     * [CAUTION]: A path that does not exist is FINE and is the normal case -- a DOS with no
     * AUTOEXEC.BAT simply has none. What was broken was the empty string.
     */
    if (bopNumber == NTVDM_BOP_CMD && sub == NTVDM_CMD_STARTUP_BATCH)
    {
        DWORD nameBase = (VDM_REG16(tib, VTIB_DS) << PARAGRAPH_SHIFT)
                 + VDM_REG16(tib, VTIB_EDX);
        volatile BYTE *nameBytes = (volatile BYTE *)(ULONG_PTR)nameBase;
        CHAR autoText[80];
        DWORD autoLength = 0;
        DWORD item;
        HANDLE autoHandle = CreateFileA(BOPAUTO_PATH, GENERIC_READ, FILE_SHARE_READ,
                                NULL, OPEN_EXISTING, 0, NULL);

        if (autoHandle != INVALID_HANDLE_VALUE)
        {
            ReadFile(autoHandle, autoText, sizeof(autoText) - 1, &autoLength, NULL);
            CloseHandle(autoHandle);

            while (autoLength && (autoText[autoLength-1] == '\r' || autoText[autoLength-1] == '\n'
                          || autoText[autoLength-1] == ' ' || autoText[autoLength-1] == '\t'))
                --autoLength;
        }

        /* s92 (#316): AND IT RUNS WITH ECHO OFF, AS STOCK's DOES. XP leaves an
         * EMPTY C:\AUTOEXEC.BAT on every machine; the shell runs it with echo ON,
         * and the end of a batch with echo on is a blank line and the PROMPT --
         * which `prog > file` from cmd captured ahead of the program's own output
         * (dospair LM6: ours "\r\nC:\...\DOS>\n\r\n" first; stock nothing). Stock's
         * AUTOEXEC.NT begins `@echo off`. So the default is now a two-line batch
         * the host writes -- `@echo off` and a CALL of C:\AUTOEXEC.BAT if there
         * is one -- so the user's batch still runs, silently. Its short path must
         * fit XP's 0x3F cap; if it cannot be made, the old answer stands.
         */
        if (!autoLength)
        {
            static CHAR wrap[MAX_PATH];

            if (!wrap[0])
            {
                CHAR tempDirectory[MAX_PATH];
                CHAR full[MAX_PATH];
                CHAR shortPath[MAX_PATH];
                DWORD tempLength = GetTempPathA(sizeof tempDirectory, tempDirectory);
                DWORD shortTempLength;
                HANDLE writeHandle;

                if (tempLength && tempLength < sizeof tempDirectory - 16)
                {
                    wsprintfA(full, HOST_STARTUP_BATCH_FORMAT, tempDirectory);
                    writeHandle = CreateFileA(full, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

                    if (writeHandle != INVALID_HANDLE_VALUE)
                    {
                        static const CHAR body[] =
                            HOST_STARTUP_BATCH_BODY;
                        DWORD bytesWritten = 0;
                        WriteFile(writeHandle, body, sizeof body - 1, &bytesWritten, NULL);
                        CloseHandle(writeHandle);
                        shortTempLength = GetShortPathNameA(full, shortPath, sizeof shortPath);

                        if (shortTempLength && shortTempLength <= NTVDM_CMD_STARTUP_PATH_MAX && bytesWritten == sizeof body - 1)
                            lstrcpynA(wrap, shortPath, sizeof wrap);
                    }
                }

                if (!wrap[0])
                    lstrcpynA(wrap, HOST_AUTOEXEC_PATH, sizeof wrap);
            }

            for (autoLength = 0; wrap[autoLength] && autoLength < sizeof autoText - 1; ++autoLength)
                autoText[autoLength] = wrap[autoLength];
        }

        if (autoLength > NTVDM_CMD_STARTUP_PATH_MAX)
            autoLength = NTVDM_CMD_STARTUP_PATH_MAX;                                                   /* XP's own cap */

        for (item = 0; item < autoLength; ++item)
            nameBytes[item] = (BYTE)autoText[item];

        nameBytes[autoLength] = 0;

        if (!quiet)
        {
            cursor = LogPut(cursor, "         sub 0D answered: startup batch [");

            for (item = 0; item < autoLength; ++item)
            {
                CHAR piece[2];
                piece[0]=autoText[item];
                piece[1]=0;
                cursor = LogPut(cursor, piece);
            }

            cursor = LogPut(cursor, "] at 0x"); cursor = LogHex(cursor, nameBase); cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }

        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_SUBFUNCTION_LENGTH;
        {
            *cursorIo = cursor;
            return HOST_FLOW_CONTINUE;
        }
    }

    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

static INT NtvdmCommandPrompt(
    PSTR *cursorIo,
    PSTR const base,
    const DWORD bopNumber,
    const DWORD sub,
    volatile BYTE * const tib,
    const INT quiet)
{
    PSTR cursor = *cursorIo;

    /* sub 0F = THE HOST'S `PROMPT`, AND IT ANSWERS IN BX (Importance = 1):
     * Stock passes the HOST's `PROMPT` environment variable through to the DOS
     * shell here, and reports in BX; with nothing to pass it answers BX = 0.
     * - BX = 0, which is exactly stock's answer when it has nothing to pass. We were
     * setting no register at all, so the guest read whatever
     * BX happened to hold -- an unimplemented call answering at random again.
     */
    /* sub 0F = "GIVE ME THE INITIAL ENVIRONMENT". (s81: the `C>` prompt) (Importance = 1):
     * Under /P, XP's COMMAND.COM builds a FRESH environment, as DOS's primary
     * shell does -- `PATH=` and a COMSPEC, nothing else -- and asks NTVDM for the
     * rest here (stock hands it the Win32 environment, PROMPT included). We said
     * "none", so the user's shell came up `C>`: DOS's default, with no PROMPT.
     * The protocol, as the shell drives it (observed, two calls):
     * call 1: BX=0 in  -> BX out = EXTRA paragraphs needed (0 = keep the old
     *         environment); the shell grows its block by that much
     * call 2: ES:0 = the new block, BX = its size in paragraphs
     *         -> the variables, double-NUL ended; BX out = paragraphs used,
     *         which must not exceed what came in (else it gives up)
     * We answer with the environment we built for it (seg DOS_ENV_SEG: PROMPT,
     * PATH, BLASTER, ULTRASND...), snapshotted on call 1 while it is intact, with
     * COMSPEC pointed at the real shell.
     */
    if (bopNumber == NTVDM_BOP_CMD && sub == NTVDM_CMD_PROMPT)
    {
        static CHAR environmentSnapshot[1024];
        static DWORD environmentSnapshotLength;
        DWORD initialBx = VDM_REG16(tib, VTIB_EBX);

        if (initialBx == 0)
        {
            const volatile BYTE *environment0 = (const volatile BYTE *)(ULONG_PTR)((DWORD)DOS_ENV_SEG << PARAGRAPH_SHIFT);
            DWORD inputIndex = 0;
            DWORD outputIndex = 0;

            while (inputIndex < ENVIRONMENT_SCAN_MAX && !(environment0[inputIndex] == 0 && environment0[inputIndex + 1] == 0))
            {
                DWORD environmentStart = inputIndex;

                while (environment0[inputIndex] && inputIndex < ENVIRONMENT_SCAN_MAX)
                    ++inputIndex;

                /* PATH IS WINDOWS' PATH, IN 8.3. (s81, user: "mem" -> "Bad command
                 * or file name") We handed the shell `PATH=C:\`, so nothing in
                 * SYSTEM32 -- MEM, EDIT, DEBUG, every XP DOS tool -- could be run by
                 * name. Stock passes the Win32 environment; this passes its PATH,
                 * each entry shortened (a DOS program cannot open a long name) and
                 * the whole kept under 250 characters, dropping entries past that
                 * rather than cutting one in half.
                 */
                if ((environment0[environmentStart] | ASCII_CASE_BIT) == 'p' && (environment0[environmentStart + 1] | ASCII_CASE_BIT) == 'a' &&
                    (environment0[environmentStart + 2] | ASCII_CASE_BIT) == 't' && (environment0[environmentStart + 3] | ASCII_CASE_BIT) == 'h' &&
                    environment0[environmentStart + 4] == '=')
                {
                    CHAR writeBuffer[2048];
                    CHAR shellPath[MAX_PATH];
                    DWORD writeLength;
                    DWORD start = outputIndex;
                    DWORD length0 = 0;
                    PSTR start0 = writeBuffer;
                    PSTR limit;
                    writeLength = GetEnvironmentVariableA(HOST_ENV_PATH, writeBuffer, sizeof writeBuffer);
                    outputIndex = (DWORD)(LogPut(environmentSnapshot + outputIndex, HOST_ENV_PATH_ASSIGN) - environmentSnapshot);

                    if (writeLength && writeLength < sizeof writeBuffer)
                    {
                        while (*start0)
                        {
                            DWORD shortLength;
                            limit = start0;

                            while (*limit && *limit != ';')
                                ++limit;

                            if (*limit)
                                *limit++ = 0;
                            else
                                limit = start0 + lstrlenA(start0);

                            shortLength = *start0 ? GetShortPathNameA(start0, shellPath, sizeof shellPath) : 0;

                            if (shortLength && shortLength < sizeof shellPath && (outputIndex - start) + shortLength + 1 < PATH_VALUE_MAX)
                            {
                                if (length0++)
                                    environmentSnapshot[outputIndex++] = ';';

                                outputIndex = (DWORD)(LogPut(environmentSnapshot + outputIndex, shellPath) - environmentSnapshot);
                            }

                            start0 = limit;
                        }
                    }

                    if (!length0)
                        outputIndex = (DWORD)(LogPut(environmentSnapshot + outputIndex, HOST_DEFAULT_DRIVE_ROOT) - environmentSnapshot);
                }
                else
                if ((environment0[environmentStart] | ASCII_CASE_BIT) == 'c' && environment0[environmentStart + 7] == '=' &&
                    (environment0[environmentStart + 1] | ASCII_CASE_BIT) == 'o' && (environment0[environmentStart + 2] | ASCII_CASE_BIT) == 'm')
                {
                    outputIndex = (DWORD)(LogPut(environmentSnapshot + outputIndex, HOST_ENV_COMSPEC_ASSIGN) - environmentSnapshot);
                    outputIndex = (DWORD)(LogPut(environmentSnapshot + outputIndex, g_ShellPath[0] ? g_ShellPath
                                                   : HOST_DEFAULT_SHELL_PATH) - environmentSnapshot);
                }
                else
                {
                    DWORD snapshotIndex;

                    for (snapshotIndex = environmentStart; snapshotIndex < inputIndex; ++snapshotIndex)
                        environmentSnapshot[outputIndex++] = (CHAR)environment0[snapshotIndex];
                }

                environmentSnapshot[outputIndex++] = 0;
                ++inputIndex;
            }

            environmentSnapshot[outputIndex++] = 0;
            environmentSnapshotLength = outputIndex;
            VDM_SET16(tib, VTIB_EBX, (WORD)((environmentSnapshotLength + PARAGRAPH_LAST_BYTE) / PARAGRAPH_SIZE + 1));
        }
        else
        {
            volatile BYTE *environment1 = (volatile BYTE *)(ULONG_PTR)(VDM_REG16(tib, VTIB_ES) << PARAGRAPH_SHIFT);
            DWORD paragraph;
            DWORD used = (environmentSnapshotLength + PARAGRAPH_LAST_BYTE) / PARAGRAPH_SIZE;

            if (environmentSnapshotLength && used <= initialBx)
            {
                for (paragraph = 0; paragraph < environmentSnapshotLength; ++paragraph)
                    environment1[paragraph] = (BYTE)environmentSnapshot[paragraph];

                VDM_SET16(tib, VTIB_EBX, (WORD)used);
            }
            else
                VDM_SET16(tib, VTIB_EBX, 0);
        }

        if (!quiet)
        {
            cursor = LogPut(cursor, initialBx ? "         sub 0F (2/2): environment written, paras=0x"
                            : "         sub 0F (1/2): environment needs extra paras=0x");
            cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX)); cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }

        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_SUBFUNCTION_LENGTH;
        {
            *cursorIo = cursor;
            return HOST_FLOW_CONTINUE;
        }
    }

    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* An NTVDM BOP from the guest's own code (XP's COMMAND.COM is NTVDM-aware): service its command-shell
 * sub-functions -- the next command to run, termination, the startup batch file, the prompt, the
 * query bits and the keyboard configuration.
 */
static INT NtvdmServiceGuestBop(
    PSTR *cursorIo,
    PSTR const base,
    volatile BYTE * const tib,
    DOS_MACHINE *machine,
    CHAR *programPathBuffer)
{
    PSTR cursor = *cursorIo;
        DWORD bopNumber  = VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK;
        DWORD codeSegment = VDM_REG16(tib, VTIB_CS);
        DWORD instructionPointer = VDM_REG16(tib, VTIB_EIP);
        const volatile BYTE *isvBopBytes = (const volatile BYTE *)(ULONG_PTR)((codeSegment << PARAGRAPH_SHIFT) + instructionPointer);
        DWORD sub = isvBopBytes[VDM_BOP_LENGTH];
        static INT  carryPolicy = -1;                 /* -1 = not yet read */
        INT quiet = 0;                           /* rate-limit the LOG, never the answer */

        if (carryPolicy < 0)
        {
            CHAR text[16];
            DWORD bytesRead = 0;
            HANDLE handle = CreateFileA(BOP54_PATH, GENERIC_READ, FILE_SHARE_READ,
                                   NULL, OPEN_EXISTING, 0, NULL);
            /* DEFAULT CF=0, and that is measured rather than chosen: with CF=1 the
             * sub-01 site's `jnc` falls into a retry and COMMAND.COM POLLS the call
             * forever (first run: 268,435,180 bytes of log, one line per spin). With
             * CF=0 it proceeds to a second, different call -- sub 0x0E at 0x5E6.
             * Neither is known to be RIGHT; one is known to be a dead end.
             */
            carryPolicy = 0;

            if (handle != INVALID_HANDLE_VALUE)
            {
                ReadFile(handle, text, sizeof text - 1, &bytesRead, NULL);
                CloseHandle(handle);

                if (bytesRead >= 3 && text[0] == 'c' && text[1] == 'f' && text[2] == '1')
                    carryPolicy = 1;
            }
        }

        /* [WARNING]: RATE-LIMITED, AND IT COST 256 MB TO LEARN (Importance = 1):
         * The first run of this instrument answered CF=1, COMMAND.COM POLLED the
         * call, and the unlimited log reached 268,435,180 bytes -- one line per
         * iteration of a loop that never ended. An instrument that scales with a
         * guest's spin rate is a denial-of-service on the thing you are trying to
         * read. 16 in full, then count only; the total goes in the summary.
         *
         * [CAUTION]: The 17th call is not less interesting than the 16th -- if the values
         * ever CHANGE after the cap this will not show it. It logs a resumed line
         * when the register signature differs from the last one printed, so a
         * state change still surfaces while a spin does not.
         */
        {   DWORD signature = VDM_REG16(tib, VTIB_EAX)
                      ^ (VDM_REG16(tib, VTIB_EBX) << PARAGRAPH_SHIFT)
                      ^ (VDM_REG16(tib, VTIB_ECX) << BYTE_SHIFT)
                      ^ (VDM_REG16(tib, VTIB_EDX) << 12) ^ (sub << 20);
            static DWORD lastSignature = 0xFFFFFFFFu;
            INT novel = (signature != lastSignature);
            lastSignature = signature;
            ++g_NtvdmBopCount;
            /* [WARNING] A NOVELTY FILTER IS NOT A CAP, AND IT COST A SECOND 256 MB.
             * The first rate-limit logged 16 in full and then only when the
             * register signature CHANGED. That is the right shape for a spin on
             * identical values -- and no defence at all against a LOOP, where
             * every pass differs slightly: once COMMAND.COM reached its command
             * loop the log hit 268,435,219 bytes again, 285,698 BOPs.
             *
             * A hard ceiling as well as a novelty test. Past it, count only.
             */
            /* [WARNING]: AND IT MUST GATE THE LOG ONLY, NEVER THE HANDLING. The first cut
             * `continue`d out of the rate-limit arm, which SKIPPED the sub 01 and
             * sub 0E handlers below and answered with the generic CF instead --
             * so past the 17th call the guest was being told something different
             * from what the first sixteen were told, silently. A quiet instrument
             * that also changes behaviour is not an instrument.
             */
            quiet = (g_NtvdmBopCount > 64) || (g_NtvdmBopCount > 16 && !novel);
        }

        if (quiet)
            goto ntvdmBopDispatch;

        cursor = LogPut(cursor, "STAGE2: NTVDM BOP from guest: bop=0x"); cursor = LogHexByte(cursor, bopNumber);
        cursor = LogPut(cursor, " sub=0x"); cursor = LogHexByte(cursor, sub);
        cursor = LogPut(cursor, " at 0x"); cursor = LogHex(cursor, codeSegment); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, instructionPointer);
        cursor = LogPut(cursor, " ax=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EAX));
        cursor = LogPut(cursor, " bx=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX));
        cursor = LogPut(cursor, " cx=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ECX));
        cursor = LogPut(cursor, " dx=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EDX));
        cursor = LogPut(cursor, " si=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ESI));
        cursor = LogPut(cursor, " di=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EDI));
        cursor = LogPut(cursor, " ds=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_DS));
        cursor = LogPut(cursor, " es=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ES));
        cursor = LogPut(cursor, " ss:sp=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_SS));
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ESP));
        cursor = LogPut(cursor, " -> UNIMPLEMENTED, answering CF="); cursor = LogDecimal(cursor, (UINT)carryPolicy);
        cursor = LogPut(cursor, "\r\n");
        /* And the bytes it is about to run either way -- the branch is right there,
         * and which way it goes is the whole question.
         */
        cursor = LogPut(cursor, "         next="); cursor = LogDump(cursor, (const VOID *)(isvBopBytes + VDM_BOP_SUBFUNCTION_LENGTH), 12);
        /* COMMAND.COM's STATE BLOCK, WHOLE, RATHER THAN ONE BYTE AT A TIME.
         * Its decisions about being a shell follow a handful of bytes in its
         * RESIDENT data -- around 0x2B0 and 0x320..0x333 of the
         * resident segment, 0x0100 for a .COM: the banner, "ask for a command"
         * vs "prompt", and the keyboard read are all gated in that block.
         *
         * Printing all of them at once turns "find the next gate, answer it,
         * re-run" into one reading. Five turns of that pattern produced one
         * caveat; this is the instrument that should have come first.
         *
         * [CAUTION]: The segment is ASSUMED to be 0x0100 (a .COM's PSP). If COMMAND.COM is
         * ever loaded elsewhere these rows are somebody else's memory -- the
         * `@0100:` in the trace's call sites is the check that it is not.
         */
        { const volatile BYTE *lowPspBytes = (const volatile BYTE *)(ULONG_PTR)(0x0100u << PARAGRAPH_SHIFT);
          cursor = LogPut(cursor, "\r\n         cc[0x2B0..0x2BF]="); cursor = LogDump(cursor, (const VOID *)(lowPspBytes + 0x2B0), 16);
          cursor = LogPut(cursor, "\r\n         cc[0x320..0x333]="); cursor = LogDump(cursor, (const VOID *)(lowPspBytes + 0x320), 20);
          /* s81: the environment the shell ACTUALLY has (PSP:2Ch), as text -- the
           * prompt came up `C>` under /P, i.e. without the PROMPT we passed.
           */
          { WORD pspEnvironmentSegment = *(const volatile WORD *)(lowPspBytes + DOS_PSP_ENVIRONMENT);
          INT scan;
            const volatile BYTE *environment2 = (const volatile BYTE *)(ULONG_PTR)((DWORD)pspEnvironmentSegment << PARAGRAPH_SHIFT);
            cursor = LogPut(cursor, "\r\n         shell env seg=0x"); cursor = LogHex(cursor, pspEnvironmentSegment); cursor = LogPut(cursor, " [");

            for (scan = 0; scan < 160 && !(environment2[scan] == 0 && environment2[scan + 1] == 0); ++scan)
            {
                CHAR character1[2];
                character1[0] = environment2[scan] ? (CHAR)environment2[scan] : '|';
                character1[1] = 0;

                if (character1[0] < 0x20 || character1[0] > 0x7e)
                    character1[0] = '.';

                cursor = LogPut(cursor, character1); }

            cursor = LogPut(cursor, "]"); } }
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    ntvdmBopDispatch:
        /* sub 01 = "WHAT SHOULD I RUN NEXT?" -- ANSWER "NOTHING" (Importance = 1):
         * On XP this is answered from CSRSS's GetNextVDMCommand (VDM_COMMAND_INFO).
         * The guest's request block is at DS:DX -- (DS<<4)+DX, exactly as we
         * compute it here -- and the answer comes back in the same block.
         * - The fields (see docs/inventory/bop.md):
         *   +0x10 w   OUT  flags; zero is "nothing to do" (observed: the shell
         *                  then goes to its prompt).
         *   +0x12 dw  IN+OUT a cookie. COMMAND.COM fills it before the call and
         *                  takes back whatever is there afterwards -- so it must
         *                  ROUND-TRIP.
         *   +0x1A w   OUT  COMMAND.COM keeps the low byte.
         *   +0x02 +0x04 +0x06 +0x16 +0x20   OUT
         *   +0x22 w   OUT  status; stock writes 4, 8 or 9 here.
         *
         * [CAUTION]: WHAT WE WRITE IS A DEFINED "NO COMMAND", NOT A DECODED ONE. That is a
         * real claim and it may be wrong -- but the alternative is not neutral:
         * leaving the block ALONE hands COMMAND.COM whatever was in its own
         * memory, which is how it got a garbage answer and spun. A defined answer
         * is falsifiable; an uninitialised one is not.
         *
         * [WARNING]: The cookie is preserved rather than zeroed, because the guest reloads
         * it into its own state unconditionally -- zeroing it would destroy
         * something we were only asked to carry.
         */
        if (bopNumber == NTVDM_BOP_CMD && sub == NTVDM_CMD_NEXT_COMMAND)
        {
            DWORD block = (VDM_REG16(tib, VTIB_DS) << PARAGRAPH_SHIFT)
                      + VDM_REG16(tib, VTIB_EDX);
            volatile BYTE *blockBytes = (volatile BYTE *)(ULONG_PTR)block;
            /* THE SECOND "WHAT NEXT?" IS THE SHELL LEAVING. (s81 sweep: `exit`) (Importance = 1):
             * The block is NT's CMDINFO -- +04 CurDrive, +0C CmdLineSize, +0E
             * ReturnCode, +18 fTSRExit, +1C:1E/+20 ExecPath -- every measured value
             * lines up. Sub 01 is GetNextVDMCommand: the DOS side has finished and
             * asks the Win32 side for work. XP's permanent shell asks once at start-
             * up and again when the user types EXIT (never after an internal or an
             * EXEC'd command -- measured: `dir` does not call it). A bare-launched
             * session has no Win32 side to hand anything back, so the second ask
             * means "we are done": end the VDM with the shell's ReturnCode, as stock
             * ends a command.com window.
             */
            /* #208: the routed program was ENDED BY CLOSE PROGRAM -- the user asked for the
             * prompt, not for the window to close. Go interactive from here (AH=53h
             * AL=2 CF=1, the shell's own prompt path) and answer "nothing", once; the
             * shell's NEXT sub 01 is then an ordinary EXIT.
             */
            if (g_GuestNtAware && g_BackToPrompt && g_ShellGetNextCount >= 1)
            {
                g_BackToPrompt = 0;
                g_DosInt53Answers[DOS_INT53_SHELL_LOOP].IsCarry = 1;
                cursor = LogPut(cursor, "         sub 01 after Close Program: back to the prompt (#208)\r\n");
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            }
            else
            if (g_GuestNtAware && ++g_ShellGetNextCount > 1)
            {
                cursor = LogPut(cursor, "         sub 01 again: the shell is handing back control (EXIT)"
                            " -- ending the VDM, rc=0x");
                cursor = LogHex(cursor, *(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_EXIT_CODE)); cursor = LogPut(cursor, "\r\n");
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
                machine->ExitCode = *(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_EXIT_CODE) & BYTE_MASK;
                {
                    *cursorIo = cursor;
                    return HOST_FLOW_BREAK;
                }
            }
            #define BW(o, v) (*(volatile WORD *)(blockBytes + (o)) = (WORD)(v))
            /* [CAUTION]: DUMPED BEFORE WE WRITE ANYTHING. Logging it after the BW()s below
             * would show our own zeros back and read as the guest's input --
             * which is the whole class of mistake this file keeps catching.
             */
            if (quiet)
                goto blockWritten;

            cursor = LogPut(cursor, "         blk in="); cursor = LogDump(cursor, (const VOID *)blockBytes, 0x28);
            cursor = LogPut(cursor, "\r\n         +1C:1E=0x");
            cursor = LogHex(cursor, *(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_NAME_SEGMENT)); cursor = LogPut(cursor, ":0x");
            cursor = LogHex(cursor, *(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_NAME_OFFSET));
            cursor = LogPut(cursor, " +20=0x"); cursor = LogHex(cursor, *(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_NAME_CAPACITY));
            cursor = LogPut(cursor, "\r\n");
            blockWritten: ;
            /* TOUCH AS LITTLE AS POSSIBLE (Importance = 2):
             * The first cut zeroed every field that XP's handler writes. One of
             * them, `+0x04`, is the DEFAULT DRIVE -- COMMAND.COM selects it with
             * `AH=0Eh` immediately after the call (our INT 21h log shows the DL it
             * passes) -- so a zero meant drive 0 = A:, and our own handler
             * said so in the same log ("drive A: exists but is not ready ...
             * selected as the DOS current drive anyway") while the shell went
             * quiet.
             *
             * [INFO]: Dumping the block BEFORE writing it settled the design: the guest
             * already supplies `+0x04 = 02` (C:). It was never ours to fill in.
             * Measured, one run:
             *   blk in = 26 02 | 00 01 | 02 00 | 00 00 | 42 93 | 27 93 | 80 00 ...
             *
             * `+0x08:+0x0A = 0x9342:0x9327` -- and `0x9327` is EXACTLY the
             * buffer COMMAND.COM reads its command line from (it is also the
             * DS:DX it later hands to INT 21h AH=0Ah, in our log). Two
             * independent sources, same address.
             *
             * So we now write only what a "no command" answer really is: the
             * empty command tail, and the flags word. Everything else is left as
             * the guest set it, because a field we cannot name is not ours.
             */
            {   DWORD codeBase = ((DWORD)(*(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_TAIL_SEGMENT)) << PARAGRAPH_SHIFT)
                         + *(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_TAIL_OFFSET);
                volatile BYTE *codeBlock = (volatile BYTE *)(ULONG_PTR)codeBase;
                /* AN EMPTY DOS COMMAND TAIL, AND THE CR IS THE POINT:
                 * COMMAND.COM scans this buffer for the terminating CR (observed).
                 * With no CR in the buffer the scan walks the WHOLE 64K segment and
                 * never leaves -- which is precisely the "spinning in
                 * V86 with no traps" the headless deadline was killing.
                 *
                 * [WARNING]: AND `[0]` IS NOT OURS. IT IS DOS'S AH=0Ah MAXIMUM, AND
                 * WRITING IT COST THE INTERACTIVE PROMPT. (s79)
                 * This used to write `c[0] = length`, on the reading that the
                 * shell copies [0]+3 bytes of it. That was not wrong about the copy
                 * and was completely wrong about the buffer, because **the same
                 * buffer is handed to INT 21h AH=0Ah** (DX=0x9327 in our log), and
                 * the shell sets its [0] to 0x80 ONCE, at start-up (observed in the
                 * block before our first answer). 0x80 is the buffered-input
                 * MAXIMUM, set a single time and never re-set. Our "empty tail" answer
                 * zeroed it on the first BOP, so
                 * every later AH=0Ah saw a zero-capacity buffer, returned an empty
                 * line immediately, and COMMAND.COM printed its prompt again --
                 * 991 prompts in one 30-second run, with `INT21 AH=0A line max=00`
                 * in the log the whole time. The shell was AT the keyboard read;
                 * we were answering it with EOF.
                 *
                 * [CAUTION]: The `+0x0C` field of the request block is 0x0080 -- the guest
                 * tells us the capacity there. Two independent sources, and the
                 * one we overwrote was the same number.
                 *
                 * WRITE THE LENGTH AT [1], WHERE DOS PUTS IT, AND NEVER TOUCH [0].
                 * Nothing downstream needs the length: the shell copies [0]+3 =
                 * 0x83 bytes (a superset) and finds the end of the text by its CR
                 * (observed), so the CR is the only load-bearing byte.
                 * Layout: [0] = max (the GUEST's, leave alone), [1] = length,
                 *         [2..] = text, then CR.
                 */
                /* ONE COMMAND, ONCE, FROM cfg\bopcmd.txt:
                 * An empty answer proves only that the guest accepted one. This
                 * hands it a REAL command line exactly once and empties every
                 * reply after -- so the log shows whether the whole path works
                 * (does it execute and PRINT?) without the command repeating for
                 * ever in a loop that already runs 1.25M times a run.
                 *
                 * [CAUTION]: One-shot on purpose: a guest that asks again must not be given
                 * the same command again. That is the difference between testing
                 * the mechanism and building a fork bomb.
                 */
                static INT commandDone = 0;
                CHAR commandBuffer[128];
                DWORD commandLength = 0;

                if (!commandDone && g_Routed)
                {
                    /* #208: the program's arguments, as the shell's tail -- it passes
                     * them on as the program's own PSP command tail.
                     */
                    /* [CAUTION]: THE TAIL IS THE WHOLE COMMAND LINE, VERB FIRST; the NAME field
                     * is only the already-resolved path to run it with. Measured two
                     * ways: tail "hello" + name COMMAND.COM EXEC'd COMMAND.COM, and
                     * tail " " + name HELLO.COM ran NOTHING (a blank line is "no
                     * command" and the shell went to its prompt).
                     */
                    commandDone = 1;
                    {   PSTR writeCursor = commandBuffer, limit = commandBuffer + sizeof(commandBuffer) - 2;
                        PCSTR firstProgram = g_FirstProgram;

                        while (*firstProgram && writeCursor < limit)
                            *writeCursor++ = *firstProgram++;

                        if (g_FirstTail[0] && writeCursor < limit)
                        {
                            PCSTR firstTail = g_FirstTail;

                            if (*firstTail != ' ')
                                *writeCursor++ = ' ';

                            while (*firstTail && writeCursor < limit)
                                *writeCursor++ = *firstTail++;
                        }

                        commandLength = (DWORD)(writeCursor - commandBuffer); }
                }

                if (!commandDone)
                {
                    HANDLE bopCommandFile = CreateFileA(BOPCMD_PATH, GENERIC_READ, FILE_SHARE_READ,
                                             NULL, OPEN_EXISTING, 0, NULL);
                    commandDone = 1;

                    if (bopCommandFile != INVALID_HANDLE_VALUE)
                    {
                        ReadFile(bopCommandFile, commandBuffer, sizeof(commandBuffer) - 1, &commandLength, NULL);
                        CloseHandle(bopCommandFile);

                        while (commandLength && (commandBuffer[commandLength-1] == '\r' || commandBuffer[commandLength-1] == '\n'))
                            --commandLength;
                    }
                }

                if (commandLength)
                {
                    DWORD item;
                    codeBlock[DOS_LINE_INPUT_LENGTH] = (BYTE)commandLength;                /* [0] is the guest's AH=0Ah max */

                    for (item = 0; item < commandLength; ++item)
                        codeBlock[DOS_LINE_INPUT_TEXT + item] = (BYTE)commandBuffer[item];

                    codeBlock[DOS_LINE_INPUT_TEXT + commandLength] = ASCII_CR;
                }
                else
                {
                    codeBlock[DOS_LINE_INPUT_LENGTH] = 0;
                    codeBlock[DOS_LINE_INPUT_TEXT] = ASCII_CR;          /* ditto: [0] is NOT ours */
                }

                if (!quiet) { cursor = LogPut(cursor, "         cmdline buf 0x"); cursor = LogHex(cursor, codeBase);

                              if (commandLength) { cursor = LogPut(cursor, " <- [");
                                        { DWORD item;

                                        for (item = 0; item < commandLength; ++item)
                                              {
                                                  CHAR piece[2];
                                                  piece[0]=commandBuffer[item];
                                                  piece[1]=0;
                                                  cursor = LogPut(cursor, piece);
                                              }
                                              }
                                        cursor = LogPut(cursor, "] ONE SHOT"); }
                              else
                                  cursor = LogPut(cursor, " <- empty tail (len=0, CR)");

                              cursor = LogPut(cursor, "\r\n"); }
            }
            BW(NTVDM_CMD_BLOCK_REDIRECTION, 0);                       /* flags: nothing was redirected */
            /* AND THE OTHER TWO HALVES OF THE ANSWER (Importance = 1):
             * sub 01 returns THREE things, not one: a command TAIL (+0x08:+0x0A,
             * written above), a program NAME (+0x1C:+0x1E, capacity +0x20), and
             * that program's TYPE at +0x22. Stock picks the type from the name's
             * extension (observed per program type):
             *   `.EXE` -> 4   `.COM` -> 8   `.BAT` -> 2   shorter than 7 -> 9
             * -- a file-extension dispatch and not a status word. [WARNING] I had guessed
             * "status enumeration"; it is not.
             * - A zero-length name therefore means type 9, and the guest agrees:
             *   on the no-program path it writes a 0 to the FIRST BYTE of the name
             *   buffer at 0x9473 (observed) -- which is exactly the +0x1C:+0x1E we
             *   are handed (`+1C:1E=0x9342:0x9473`).
             *
             * [CAUTION]: We had been leaving both alone, i.e. handing the shell whatever was
             * in its own memory. `ver` and `dir` worked anyway -- a builtin needs
             * only the tail -- but that was luck, not an answer.
             */
            {   DWORD nameBase = ((DWORD)(*(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_NAME_SEGMENT)) << PARAGRAPH_SHIFT)
                         + *(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_NAME_OFFSET);
                DWORD capacity = *(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_NAME_CAPACITY);
                volatile BYTE *nameBytes = (volatile BYTE *)(ULONG_PTR)nameBase;
                /* HAND BACK WHAT CSRSS NAMED -- THE SAME SOURCE XP USES (Importance = 1):
                 * On XP this buffer is filled from the VDM_COMMAND_INFO that
                 * GetNextVDMCommand returned, and our STAGE1 already made that
                 * exact call: `STAGE1: command fetch ... app=[...]` lands in
                 * g_Application2. Returning it is not a guess about what the shell wants;
                 * it is the same answer from the same place.
                 * - ONCE. The first call is the VDM's reason for existing; after
                 *   that there is nothing more to run and the name is empty. A
                 *   shell that is handed the same program every time it asks would
                 *   exec it for ever.
                 *
                 * [CAUTION]: The type is stock's own rule (see above): the last four
                 * characters, `.EXE`->4 `.COM`->8 `.BAT`->2, and anything
                 * shorter than 7 characters -> 9. Mirrored, not invented.
                 */
                static INT namedOnce = 0;
                /* [CAUTION]: programPathBuffer FIRST, not g_Application2. On the rig CSRSS names
                 * `dosstub.com` -- the harness stub -- and `target.txt` names the
                 * real program, so g_Application2 would hand the shell the stub. programPathBuffer
                 * is what we actually LOADED, which is the program either way.
                 */
                PCSTR programPath = g_Routed ? g_FirstProgram      /* #208: the program */
                               : programPathBuffer[0] ? programPathBuffer : (g_Application2[0] ? g_Application2 : "");
                DWORD al = 0;
                DWORD valueType = NTVDM_CMD_TYPE_OTHER;

                if (!namedOnce && programPath[0])
                {
                    while (programPath[al] && al + 1 < capacity && al < MAX_PATH)
                        ++al;

                    namedOnce = 1;
                }

                if (al > NTVDM_CMD_SHORT_NAME_MAX)
                {
                    CHAR extension[DOS_DOT_EXTENSION_LENGTH + 1];
                    INT item;

                    for (item = 0; item < DOS_DOT_EXTENSION_LENGTH; ++item)
                    {
                        CHAR character = programPath[al - DOS_DOT_EXTENSION_LENGTH + item];
                        extension[item] = (character >= 'a' && character <= 'z') ? (CHAR)(character - ASCII_CASE_BIT) : character;
                    }

                    extension[DOS_DOT_EXTENSION_LENGTH] = 0;

                    if      (extension[1]=='E' && extension[2]=='X' && extension[3]=='E' && extension[0]=='.')
                        valueType = NTVDM_CMD_TYPE_EXE;
                    else if (extension[1]=='C' && extension[2]=='O' && extension[3]=='M' && extension[0]=='.')
                        valueType = NTVDM_CMD_TYPE_COM;
                    else if (extension[1]=='B' && extension[2]=='A' && extension[3]=='T' && extension[0]=='.')
                        valueType = NTVDM_CMD_TYPE_BAT;
                }

                {
                    DWORD item;

                    for (item = 0; item < al; ++item)
                        nameBytes[item] = (BYTE)programPath[item];

                    nameBytes[al] = 0;
                }
                BW(NTVDM_CMD_BLOCK_PROGRAM_TYPE, valueType);
            /* +0x1A GATES THE INTERACTIVE PATH, so it is not a field we may
             * leave alone. COMMAND.COM keeps its low byte, and a non-zero value
             * takes it AWAY from the prompt. Zero.
             */
            BW(NTVDM_CMD_BLOCK_KEYBOARD_GATE, 0);

                if (!quiet)
                {
                    cursor = LogPut(cursor, "         prog name <- [");
                    {
                        DWORD item;

                        for (item = 0; item < al; ++item)
                        {
                            CHAR piece[2];
                            piece[0]=programPath[item];
                            piece[1]=0;
                            cursor = LogPut(cursor, piece);
                        }
                    }
                    cursor = LogPut(cursor, "] type="); cursor = LogDecimal(cursor, valueType); cursor = LogPut(cursor, "\r\n");
                }
            }
            #undef BW
            /* +0x12 IS NOT A COOKIE. IT IS THE INTERACTIVE SWITCH (Importance = 3):
             * I called it a cookie because COMMAND.COM fills it from its own
             * state before the call and takes it straight back after -- which
             * looks exactly like carrying an opaque handle. It is not: with it
             * zero the shell goes to its keyboard prompt (AH=0Ah), and with it
             * non-zero it does not (observed).
             *
             * a ZERO dword means "nothing is driving me: read from the keyboard".
             *
             * [INFO]: And stock answers zero there in exactly this case -- when nothing is
             * redirected (flags at +0x10 zero).
             *
             * [WARNING]: PRESERVING IT WAS THE BUG. "Round-trip the value you were only
             * asked to carry" is a good instinct and it was wrong here: the guest
             * re-loads its own non-zero state, we hand it straight back, and it
             * concludes it is being driven -- for ever. 1.25M calls a run.
             *
             * Zero, and only because the flags are zero: the two move together
             * in stock's answers and must stay tied together here.
             */
            *(volatile DWORD *)(blockBytes + NTVDM_CMD_BLOCK_INTERACTIVE) = 0;
            /* +0x14 is the high half of that dword and is covered by the store above.
             * +0x02 +0x04 +0x06 +0x16 +0x1A +0x20 +0x22 likewise: XP writes them,
             * but we cannot yet say WHAT, and a named wrong value is worse than an
             * unchanged right one. Revisit each as its meaning is earned.
             */
            VDM_REG(tib, VTIB_EAX) &= HIGH_WORD_MASK_U;   /* AX = 0 */

            if (!quiet)
            {
                cursor = LogPut(cursor, "         sub 01 answered: no command (flags=0, cookie and "
                            "the guest's own fields left alone), blk=0x");
                            cursor = LogHex(cursor, block);
                cursor = LogPut(cursor, "\r\n");
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            }

            VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;  /* success */
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_SUBFUNCTION_LENGTH;
            {
                *cursorIo = cursor;
                return HOST_FLOW_CONTINUE;
            }
        }

        {
            INT flow = NtvdmCommandPrompt(&cursor, base, bopNumber, sub, tib, quiet);

            if (flow == HOST_FLOW_CONTINUE)
            {
                *cursorIo = cursor;
                return HOST_FLOW_CONTINUE;
            }
        }
        {
            INT flow = NtvdmCommandStartupBatch(&cursor, base, bopNumber, sub, tib, quiet);

            if (flow == HOST_FLOW_CONTINUE)
            {
                *cursorIo = cursor;
                return HOST_FLOW_CONTINUE;
            }
        }
        /* sub 10 = A ONE-BIT QUERY, AND IT GATES THE PROMPT (Importance = 1):
         * The answer is AL, a yes/no flag and nothing more.
         *
         * [INFO]: It sits ON the interactive path. COMMAND.COM issues it just before its
         * prompt, and a non-zero AL takes it AWAY from the prompt: it
         * means "do not read the keyboard". We were not setting AL at all, leaving
         * whatever the guest happened to have there -- which is how an
         * unimplemented call still ANSWERS, at random.
         * - AL = 0. Whatever the flag tracks, it is not set in a plain VDM that
         *   has been asked to run a shell, and 0 is the value that lets the shell
         *   be a shell. [CAUTION] Recorded as a reading of ONE flag we have not named,
         *   not as a decode of what it means.
         */
        if (bopNumber == NTVDM_BOP_CMD && sub == NTVDM_CMD_QUERY_BIT)
        {
            VDM_REG(tib, VTIB_EAX) &= ~BYTE_MASK_U;   /* AL = 0 */

            if (!quiet)
            {
                cursor = LogPut(cursor, "         sub 10 answered: AL=0\r\n");
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            }

            VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_SUBFUNCTION_LENGTH;
            {
                *cursorIo = cursor;
                return HOST_FLOW_CONTINUE;
            }
        }

        /* sub 0E = THE KEYBOARD / CODE-PAGE CONFIGURATION (Importance = 1):
         * On stock this describes the keyboard / code-page setup for a KEYB
         * command line, written into the guest's buffer.
         *
         * DS:SI is a buffer of CX bytes; DX is the ANSWER -- non-zero = "there is
         * a keyboard driver to set up" (the shell goes on to set one up), zero =
         * skip it (observed).
         *
         * [WARNING]: I HAD THIS WRONG ONCE. Reading only the guest side, DX looked like a
         * leftover from the `INT 2Fh AX=AD80h` KEYB check just before it, and I
         * wrote that the BOP "probably does not touch DX". Stock sets DX on every
         * answer. **A register set by the callee is not distinguishable from a
         * leftover by looking at the caller alone.**
         * - We answer 0 = no keyboard driver, which is TRUE of us: we do not load
         *   KB16.COM or KEYBOARD.SYS. Explicitly, rather than by leaving DX alone
         *   and getting 0 because that is what happened to be in it.
         */
        /* sub 00 = VDDTerminateVDM: THE PERMANENT SHELL'S EXIT. (s81 sweep):
         * XP's COMMAND.COM ends the VDM through here on EXIT when it is the
         * permanent shell (started with /P) -- observed.
         * It had no arm, so it fell to the generic "skip the BOP" below and EXIT
         * did nothing. End the run exactly as a top-level AH=4Ch does.
         */
        if (bopNumber == NTVDM_BOP_CMD && sub == NTVDM_CMD_TERMINATE)
        {
            cursor = LogPut(cursor, "         sub 00: the shell asked to END THE VDM (EXIT) -- ending the run\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            machine->ExitCode = 0;
            {
                *cursorIo = cursor;
                return HOST_FLOW_BREAK;
            }
        }

        if (bopNumber == NTVDM_BOP_CMD && sub == NTVDM_CMD_KEYBOARD_CONFIG)
        {
            VDM_REG(tib, VTIB_EDX) &= HIGH_WORD_MASK_U;   /* DX = 0: no KEYB to run */

            if (!quiet)
            {
                cursor = LogPut(cursor, "         sub 0E answered: no keyboard driver (DX=0)\r\n");
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            }

            VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_SUBFUNCTION_LENGTH;
            {
                *cursorIo = cursor;
                return HOST_FLOW_CONTINUE;
            }
        }

        if (carryPolicy)
            VDM_REG(tib, VTIB_EFLAGS) |=  EFLAGS_CF_U;
        else
            VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;

        VDM_REG(tib, VTIB_EIP) += VDM_BOP_SUBFUNCTION_LENGTH;             /* C4 C4 <bop> <sub> -- see above */
        {
            *cursorIo = cursor;
            return HOST_FLOW_CONTINUE;
        }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* INT 20h, 22h (BOP 30h) and INT 27h end the program: terminate it -- or, with a parent waiting, return to the parent. */
static INT DosServiceTerminateBop(
    PSTR *cursorIo,
    PSTR const base,
    volatile BYTE * const tib,
    DOS_MACHINE *machine)
{
    PSTR cursor = *cursorIo;

    {   /* ---- INT 20h / 22h (BOP 30h) and INT 27h: they END the program, so they stay
           here -- only the exec loop can terminate a run or return to a parent.
           The rest of the BIOS block moved to V86BiosBop() (GH #247). */
        UINT bopNumber = VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK;
        INT handled = 1;

        if (bopNumber == DOS_BOP_INT20)                 /* INT 20h: terminate */
        {
            /* INT 20h is AH=4Ch with an exit code of 0. Routing it here
             * rather than leaving an IRET means a program that exits this
             * way actually exits, instead of returning into itself.
             */
            VDM_REG(tib, VTIB_EAX) &= HIGH_WORD_MASK_U;
            machine->TraceCursor = cursor;
            DosInt21(machine);
            cursor = machine->TraceCursor;   /* AH=00 -> terminate */
            /* [INFO]: AND IF THIS WAS A CHILD, GO BACK TO ITS PARENT. (GH #134)
             * This used to set handled = 2, which `break`s the exec loop
             * and ends the whole VDM -- so a .COM child exiting the normal
             * .COM way took the host down with it.
             */
            handled = DosTerminate(machine, tib, &cursor, base) ? EXEC_HANDLED_CHILD_EXITED : EXEC_HANDLED_RUN_OVER;
        }
        else if (bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_TERMINATE_RESIDENT))                 /* TSR, CP/M style */
        {
            /* THE OLD FORM OF AH=31h, AND IT KEEPS MEMORY TOO. (GH #49):
             * DX is a BYTE OFFSET past the PSP here, not a paragraph
             * count -- that is the one thing this call does differently
             * and the easy thing to get wrong. Round UP to paragraphs so
             * the last partial one is kept rather than cut off.
             * CS is the resident program's PSP for an INT 27h caller.
             */
            DWORD int27Dx = VDM_REG16(tib, VTIB_EDX);
            machine->TsrKeep = (WORD)((int27Dx + PARAGRAPH_LAST_BYTE) >> PARAGRAPH_SHIFT);
            machine->IsTsrPending = 1;
            cursor = LogPut(cursor, "  INT27 TSR: keep 0x"); cursor = LogHex(cursor, machine->TsrKeep);
            cursor = LogPut(cursor, " paras (DX=0x"); cursor = LogHex(cursor, int27Dx);
            cursor = LogPut(cursor, " bytes), vectors LEFT INSTALLED\r\n");
            VDM_REG(tib, VTIB_EAX) &= HIGH_WORD_MASK_U;
            machine->TraceCursor = cursor;
            DosInt21(machine);
            cursor = machine->TraceCursor;
            handled = DosTerminate(machine, tib, &cursor, base) ? EXEC_HANDLED_CHILD_EXITED : EXEC_HANDLED_RUN_OVER;
        }
        else
            handled = 0;

        if (handled == EXEC_HANDLED_RUN_OVER) /* terminate: the run is over */
        {
            *cursorIo = cursor;
            return HOST_FLOW_BREAK;
        }

        if (handled == EXEC_HANDLED_CHILD_EXITED) /* a child exited: parent is back */
        {
            *cursorIo = cursor;
            return HOST_FLOW_CONTINUE;
        }

        if (handled)
        {
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
            {
                *cursorIo = cursor;
                return HOST_FLOW_CONTINUE;
            }
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* The BIOS INT 08h timer tick, serviced in the host. */
static INT V86ServiceTimerBop(volatile BYTE * const tib)
{
    if ((VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK) == VECTOR_TIMER)     /* INT 08h timer tick */
    {
        NTVDD_REGISTERS registers;
        RegistersLoad(&registers, tib);
        HOST_LOCK();
        VddBusDeliverInterrupt(&g_Bus, VECTOR_TIMER, &registers);  /* bump BIOS tick at 0040:006C */
        /* The real BIOS timer ISR ends with `mov al,20h; out 20h,al`. Ours is a BOP
         * with nowhere to put one, so issue the EOI here -- without it the PIC's
         * in-service bit for IRQ0 latches on the first tick and the timer stops dead
         * (measured: exactly one tick delivered in a 30 s run).
         */
        VddPicEndOfInterrupt(&g_Pic, PIC_IRQ_TIMER);
        HOST_UNLOCK();
        RegistersStore(&registers, tib);
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;            /* -> CD 1C (chain user timer) */
        {
            return HOST_FLOW_CONTINUE;
        }
    }

    return HOST_FLOW_NEXT;
}

/* The BIOS INT 09h keyboard handler, serviced in the host. */
static INT V86ServiceKeyboardBop(volatile BYTE * const tib)
{
    if ((VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK) == VECTOR_KEYBOARD)     /* INT 09h: BIOS keyboard */
    {
        INT keyAction;
        /* #244: THE SECOND HALF, AFTER INT 15h AH=4Fh SAID "PROCESS IT" (CF=1).
         * We are at bios_kbdact.asm k4f's BOP: AL is the scancode as the hook left
         * it (possibly changed), the interrupted code's AX is on the stack above
         * the INT 09h frame. Translate AL, EOI, pop that AX ourselves, and resume
         * where the byte's action says -- a side-call, or brk's bare IRET.
         */
        if (VDM_REG16(tib, VTIB_CS) == DOS_CTAB_SEG
            && VDM_REG16(tib, VTIB_EIP) == DOS_KBDACT_OFF + BIOS_KEYBOARD_ACTION_INTERCEPT_BOP)
        {
            DWORD ss = VDM_REG16(tib, VTIB_SS);
            DWORD stackPointer = VDM_REG16(tib, VTIB_ESP);
            INT keyboardAction;
            HOST_LOCK();
            keyAction = VddInputBiosTranslate(&g_Input, (BYTE)VDM_REG(tib, VTIB_EAX));
            VddPicEndOfInterrupt(&g_Pic, PIC_IRQ_KEYBOARD);

            if (keyAction == INPUT_ACTION_PAUSE && KeyboardActionEntry(keyAction) < 0)
                VddInputPauseCancel(&g_Input);

            HOST_UNLOCK();
            ++g_Kb4FTranslate;
            VDM_SET16(tib, VTIB_EAX, PeekWord((ss << PARAGRAPH_SHIFT) + stackPointer));          /* pop ax */
            VDM_REG(tib, VTIB_ESP) = (VDM_REG(tib, VTIB_ESP) & HIGH_WORD_MASK_U) | ((stackPointer + X86_WORD_SIZE) & WORD_MASK);
            keyboardAction = KeyboardActionEntry(keyAction);
            VDM_REG(tib, VTIB_EIP) = (DWORD)(DOS_KBDACT_OFF + (keyboardAction >= 0 ? keyboardAction : BIOS_KEYBOARD_ACTION_IRET));
            {
                return HOST_FLOW_CONTINUE;
            }
        }

        /* #244: THE FIRST HALF, WHEN SOMETHING HAS HOOKED INT 15h. Take the byte
         * out of the controller now (it is the BIOS's `in al,60h`), push the
         * interrupted code's AX, load AX = 4F00h | byte, and run k4f: `stc / int
         * 15h` in the guest, then back to the arm above -- or, on CF=0, k4f's own
         * EOI and IRET (swallowed). The EOI waits until then, as the BIOS's does.
         * Not hooked (the normal case, every game included): straight on below,
         * exactly as before -- not one extra instruction on the default path.
         */
        if (Int15Hooked())
        {
            INT scanCode;
            HOST_LOCK();
            scanCode = VddInputBiosFetch(&g_Input);
            HOST_UNLOCK();

            if (scanCode >= 0)
            {
                DWORD ss = VDM_REG16(tib, VTIB_SS);
                WORD  stackPointer = (WORD)((VDM_REG(tib, VTIB_ESP) - X86_WORD_SIZE) & WORD_MASK);
                PokeWord((ss << PARAGRAPH_SHIFT) + stackPointer, (WORD)VDM_REG(tib, VTIB_EAX));     /* push ax */
                VDM_REG(tib, VTIB_ESP) = (VDM_REG(tib, VTIB_ESP) & HIGH_WORD_MASK_U) | stackPointer;
                VDM_SET16(tib, VTIB_EAX, (WORD)((BIOS_SYSTEM_KEYBOARD_INTERCEPT << BYTE_SHIFT) | (UINT)scanCode));
                VDM_SET16(tib, VTIB_CS, DOS_CTAB_SEG);
                VDM_REG(tib, VTIB_EIP) = (DWORD)(DOS_KBDACT_OFF + BIOS_KEYBOARD_ACTION_INTERCEPT);
                ++g_Kb4FCalls;
                {
                    return HOST_FLOW_CONTINUE;
                }
            }

            /* nothing presented: a spurious IRQ1 -- EOI and IRET, as below */
            HOST_LOCK();
            VddPicEndOfInterrupt(&g_Pic, PIC_IRQ_KEYBOARD);
            HOST_UNLOCK();
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
            {
                return HOST_FLOW_CONTINUE;
            }
        }

        HOST_LOCK();
        keyAction = VddInputBiosConsume(&g_Input);   /* take the byte, re-arm if more queued */
        /* THE BIOS INT 09h ENDS WITH AN EOI, AND SO MUST THIS (Importance = 1):
         * A guest that hooks INT 09h keeps IRQ1 in service until it EOIs (strict
         * acknowledge above). QB.EXE's hook EOIs only the keys it swallows; for
         * every ordinary key it CHAINS to the BIOS handler (`int 0EFh`) and leaves
         * the EOI to it -- as the real one does with `mov al,20h / out 20h,al`.
         * This arm never sent one, so IRQ1's in-service bit stayed set after the
         * first key and no keyboard interrupt was ever delivered again: measured
         * keyirq=1 for a whole QBasic run of ~19 key presses. Same shape as the
         * INT 08h arm's EOI below, for the same reason. Harmless when the guest
         * EOI'd before chaining: the bit is already clear.
         */
        VddPicEndOfInterrupt(&g_Pic, PIC_IRQ_KEYBOARD);
        HOST_UNLOCK();
        /* #254: AND WHAT THE BIOS CALLS FROM IT. Ctrl-Break -> INT 1Bh, Print
         * Screen -> INT 05h, SysReq -> INT 15h AH=85h, Pause -> the spin loop:
         * resume the guest in bios_kbdact.asm's routine (after the EOI, as the
         * BIOS does), which IRETs to the interrupted code. Only guest-side
         * calls -- nothing here reaches the host.
         */
        {   INT keyboardAction = KeyboardActionEntry(keyAction);

            if (keyboardAction >= 0)
            {
                VDM_SET16(tib, VTIB_CS, DOS_CTAB_SEG);
                VDM_REG(tib, VTIB_EIP) = (DWORD)(DOS_KBDACT_OFF + keyboardAction);
                {
                    return HOST_FLOW_CONTINUE;
                }
            }

            if (keyAction == INPUT_ACTION_PAUSE)
            {
                HOST_LOCK();
                VddInputPauseCancel(&g_Input);
                HOST_UNLOCK();
            }
        }
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;        /* -> the IRET */
        {
            return HOST_FLOW_CONTINUE;
        }
    }

    return HOST_FLOW_NEXT;
}

/* The guest stopped for something that is neither I/O nor a BOP: log where, with the bytes before and at CS:IP. */
static INT V86ReportUnexpectedStop(
    PSTR *cursorIo,
    PSTR const base,
    const DWORD event,
    volatile BYTE * const tib,
    const LONG vdmStatus)
{
    PSTR cursor = *cursorIo;

    if (event != VDM_EVENT_BOP)
    {
        DWORD currentCs = VDM_REG16(tib, VTIB_CS);
        DWORD currentIp = VDM_REG16(tib, VTIB_EIP);
        volatile BYTE *codeBytes = (volatile BYTE *)((currentCs << PARAGRAPH_SHIFT) + currentIp);
        BYTE instructionBytes[8];
        BYTE pageBytes[8];
        UINT item;

        for (item = 0; item < 8; ++item)
            instructionBytes[item] = codeBytes[item];

        for (item = 0; item < 8; ++item)
            pageBytes[item] = (currentIp >= 8) ? codeBytes[(INT)item - 8] : 0;                               /* 8 bytes BEFORE CS:IP */

        cursor = LogPut(cursor, "STAGE2: stop event=0x"); cursor = LogHex(cursor, event);
        cursor = LogPut(cursor, " status=0x"); cursor = LogHex(cursor, (UINT)vdmStatus);
        cursor = LogPut(cursor, " info=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EVENT_INFO));
        cursor = LogPut(cursor, " CS:IP=0x"); cursor = LogHex(cursor, currentCs);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, currentIp); cursor = LogPut(cursor, "\r\n");
        cursor = LogPut(cursor, "  bytes[CS:IP-8]: "); cursor = LogDump(cursor, pageBytes, 8);
        cursor = LogPut(cursor, "  bytes@CS:IP: "); cursor = LogDump(cursor, instructionBytes, 8);
        cursor = LogPut(cursor, "  VTIB[5A8..]: "); cursor = LogDump(cursor, (const VOID *)(tib + 0x5A8), 0x20);
        LogAppend(LOG_PATH, base, cursor); cursor = base;
        {
            *cursorIo = cursor;
            return HOST_FLOW_BREAK;
        }
    }

    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* A port access (or REP INS/OUTS, or a #GP on one) the kernel reflected: emulate it on the device bus and resume the guest after the instruction. */
static INT V86ServiceIoEvent(
    PSTR *cursorIo,
    PSTR const base,
    const DWORD event,
    volatile BYTE * const tib)
{
    PSTR cursor = *cursorIo;
    static UINT32 lastFault = 0;
    static INT stormCount = 0;   /* the I/O-fault storm detector's memory, across events */

    /* I/O port trap (event 0; VM-confirmed) or a generic GP fault (event 2):
     * if the faulting instruction is an IN/OUT we can decode, service it via
     * the VDD bus and resume; otherwise fall through to the stop dump.
     */
    if (event == VDM_EVENT_IO || event == VDM_EVENT_IO_HW ||
        event == VDM_EVENT_GPFAULT || event == VDM_EVENT_IO_STRING)
    {
        INT handled;
        {
            static INT ioBudget = 6;
            VdmStateSample("io-reflect", tib, &ioBudget);
        }
        /* REP INS/OUTS arrives as its own event with CS:IP ON the instruction. */
        if (event == VDM_EVENT_IO_STRING)
        {
            HOST_LOCK();
            handled = HostTryIoString(tib, &g_Bus);
            HOST_UNLOCK();

            if (handled)
            {
                g_EventIoString++;
                {
                    *cursorIo = cursor;
                    return HOST_FLOW_CONTINUE;
                }
            }
        }

        /* Trap-storm detection over ALL faults (port + A0000 memory): the
         * per-pixel VGA loop faults repeatedly in a tight PC window. Once a
         * storm is established in mode 12h, escalate to the batching
         * interpreter so the whole inner loop (OUTs + pixel writes) runs in
         * one shot; otherwise emulate the single faulting access.
         */
        DWORD currentCs2 = VDM_REG16(tib, VTIB_CS);
        DWORD currentIp2 = VDM_REG16(tib, VTIB_EIP);
        UINT32 current = (currentCs2 << PARAGRAPH_SHIFT) + currentIp2;
        UINT32 distance = (current > lastFault) ? (current - lastFault) : (lastFault - current);
        stormCount = (distance <= STORM_WINDOW) ? (stormCount + 1) : 0;
        lastFault = current;

        if ((g_A000Protection || (g_Interp12 && VddVideoIsPlanarActive(&g_Video)))
            && stormCount >= STORM_GATE)
        {
            DWORD breakCs = VDM_REG16(tib, VTIB_CS);
            DWORD breakIp = VDM_REG16(tib, VTIB_EIP);
            INT32 ran = HostInterpPaced(tib, TIER1_CAP);

            if (ran > 0)
            {
                static INT batchBudget = 10;

                if (batchBudget > 0)              /* is the batch ADVANCING the guest? */
                {
                    --batchBudget;
                    cursor = LogPut(cursor, "BATCH ran="); cursor = LogHex(cursor, (DWORD)ran);
                    cursor = LogPut(cursor, " from 0x"); cursor = LogHex(cursor, breakCs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, breakIp);
                    cursor = LogPut(cursor, " to 0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
                    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EIP));
                    cursor = LogPut(cursor, "\r\n");
                    LogAppend(LOG_PATH, base, cursor); cursor = base;
                }

                { /* batched the hot loop */
                    *cursorIo = cursor;
                    return HOST_FLOW_CONTINUE;
                }
            }
        }

        HOST_LOCK();
        handled = HostTryIo(tib, &g_Bus);     /* single port op (no logging) */
        HOST_UNLOCK();
        RetraceIdle();                              /* #183: outside the lock */

        if (handled)
        {
            g_EventIo++;
            g_IoViaDirect++;
            IoHotNote(g_IoLastPort, VDM_REG16(tib, VTIB_CS), VDM_REG16(tib, VTIB_EIP));
            {
                *cursorIo = cursor;
                return HOST_FLOW_CONTINUE;
            }
        }

        /* real-HW event 3 reports CS:IP AFTER the faulting IN/OUT -> retro-decode the
         * I/O instruction ending at CS:IP and service it (Skyroads' vblank IN AL,DX).
         */
        if (event == VDM_EVENT_IO_HW)
        {
            HOST_LOCK();
            handled = HostTryIoRetro(tib, &g_Bus);
            HOST_UNLOCK();
            RetraceIdle();                          /* #183: outside the lock */

            if (handled)
            {
                g_EventIo++;
                g_IoViaRetro++;
                IoHotNote(g_IoLastPort, VDM_REG16(tib, VTIB_CS), VDM_REG16(tib, VTIB_EIP));
                {
                    *cursorIo = cursor;
                    return HOST_FLOW_CONTINUE;
                }
            }
        }

        if ((g_A000Protection || (g_Interp12 && VddVideoIsPlanarActive(&g_Video)))
            && HostInterpPaced(tib, 1) > 0) /* single A0000 access */
        {
            *cursorIo = cursor;
            return HOST_FLOW_CONTINUE;
        }

        /* The interpreter refused the very first opcode. With A0000 trapped that
         * is a LIVELOCK, not a miss: we resume at the same EIP, the guest
         * re-faults on the same store, forever. Name the opcode -- this is the
         * "mode-12h MOV-store decoder gap" from the M3 notes, and it is why
         * mode 12h has never rendered. Budgeted so it cannot flood the log.
         */
        if (g_A000Protection || g_Interp12)
        {
            static INT declineBudget = 8;

            if (declineBudget > 0)
            {
                DWORD codeSegment2 = VDM_REG16(tib, VTIB_CS);
                DWORD eip2 = VDM_REG16(tib, VTIB_EIP);
                const volatile BYTE *ip2 = (const volatile BYTE *)((codeSegment2 << PARAGRAPH_SHIFT) + eip2);
                UINT index4;
                --declineBudget;
                cursor = LogPut(cursor, "INTERP-REFUSED at 0x"); cursor = LogHex(cursor, codeSegment2);
                cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, eip2); cursor = LogPut(cursor, " bytes:");

                for (index4 = 0; index4 < 8; ++index4)
                {
                    cursor = LogPut(cursor, " ");
                    cursor = LogHexByte(cursor, ip2[index4]);
                }

                cursor = LogPut(cursor, "\r\n");
                LogAppend(LOG_PATH, base, cursor); cursor = base;
            }

            g_InterpRefused++;
        }

        /* Not an I/O instruction and not an A0000 touch. On this hardware event 3
         * is OVERLOADED: besides the I/O reflect, it is how the kernel says "a
         * hardware interrupt is pending and the VDM has interrupts enabled" -- the
         * interrupt assist we thought we lacked. It only ever fires now that the
         * guest runs with IF=1; with IF=0 the kernel had nothing to notify us about
         * and simply left VDM_INT_TIMER pending forever. Distinguish it from a
         * genuine GP fault by the kernel's own pending bits in FIXED_NTVDMSTATE:
         * clear them (so it stops re-notifying), latch the timer IRQ, and resume at
         * the SAME EIP -- no instruction faulted, so nothing must be stepped over.
         * Delivery itself is left to the loop-top gate, which owns the IF and
         * re-entrancy checks. (Session 9 met this same event in protected mode and
         * cleared the bits there; see dpmi_enter.S label 2.)
         */
        if (event == VDM_EVENT_IO_HW)
        {
            volatile DWORD *vdmStateWord = (volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR;
            DWORD pend = *vdmStateWord & VDM_INT_PENDING;

            if (pend)
            {
                if (pend & VDM_INT_TIMER)
                    Irq0Latch();                               /* VDM_INT_TIMER -> IRQ0 */

                *vdmStateWord &= ~VDM_INT_PENDING;
                g_EventIntPending++;
                {
                    *cursorIo = cursor;
                    return HOST_FLOW_CONTINUE;
                }
            }
        }
    }

    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* Run the guest in V86 until its next event, and time the stretch (#238). */
static VOID V86RunGuestTimed(volatile BYTE * const tib, DWORD *eventIo, LONG *vdmStatusIo)
{
    DWORD event = *eventIo;
    LONG vdmStatus = *vdmStatusIo;

    {   LARGE_INTEGER runStart, runEnd;
    DWORD runMilliseconds;
        DWORD eventCs = VDM_REG16(tib, VTIB_CS);
        DWORD eip = VDM_REG16(tib, VTIB_EIP);
        QueryPerformanceCounter(&runStart);

        if (g_HostTimeLast)                                   /* #238: see g_HostMicrosecondsEvent */
            g_HostMicrosecondsEvent[g_HostEventLast] += QpcMicroseconds(runStart.QuadPart - g_HostTimeLast);

        event = VdmRunGuest(tib, &vdmStatus);
        QueryPerformanceCounter(&runEnd);
        g_V86MicrosecondsTotal += QpcMicroseconds(runEnd.QuadPart - runStart.QuadPart);
        g_HostTimeLast = runEnd.QuadPart;
        g_HostEventLast = event < EV_HIST_MAX ? event : EV_HIST_MAX - 1;
        {   DWORD now = GetTickCount();                     /* #238: g_XsSnapshot */

            if (!g_XsStart)
                g_XsStart = now;

            while (g_XsSeconds < XS_SECS && now - g_XsStart >= g_XsSeconds * MILLISECONDS_PER_SECOND_U)
            {
                DWORD *snapshot = g_XsSnapshot[g_XsSeconds++];
                DWORD hostUs = 0;
                INT eventIndex;

                for (eventIndex = 0; eventIndex < EV_HIST_MAX; ++eventIndex)
                    hostUs += g_HostMicrosecondsEvent[eventIndex] / MICROSECONDS_PER_MILLISECOND_U;

                snapshot[XS_RAISE] = g_IrqRaised[0];
                snapshot[XS_ASYNC] = g_AsyncInjected;
                snapshot[XS_COOP] = g_Irq0Injected;
                snapshot[XS_NIE] = g_AsyncWhyHistogram[0][ASYNC_WHY_NOT_IN_EXEC];
                snapshot[XS_BOP] = g_EventHistogram[VDM_EVENT_BOP];
                snapshot[XS_IO] = g_EventHistogram[VDM_EVENT_IO];
                snapshot[XS_HOSTMS] = hostUs;
                snapshot[XS_PACE] = g_PitPaceCalls;
            } }

        /* s82: the entry trampoline borrowed DPMI callback slot 0/1's bytes (#248: the
         * slots have since moved to 0x90; the bytes are spare now). The first
         * exit that is not inside it hands them back -- long before any client can
         * have allocated, let alone called, a callback.
         */
        if (g_TrampolineSaved)
        {
            DWORD trampolineCs = VDM_REG16(tib, VTIB_CS);
            DWORD trampolineIp = VDM_REG16(tib, VTIB_EIP);

            if (!(trampolineCs == DOS_HDLR_SEG && trampolineIp >= DOS_HDLR_TRAMPOLINE_OFF && trampolineIp < DOS_HDLR_TRAMPOLINE_OFF + DOS_HDLR_TRAMPOLINE_SIZE))
            {
                volatile BYTE *trampoline = (volatile BYTE *)(ULONG_PTR)(((DWORD)DOS_HDLR_SEG << PARAGRAPH_SHIFT) + DOS_HDLR_TRAMPOLINE_OFF);
                INT item;

                for (item = 0; item < DOS_HDLR_TRAMPOLINE_SIZE; ++item)
                    trampoline[item] = g_TrampolineSave[item];

                g_TrampolineSaved = 0;
                {   CHAR trampolineLine[200], *trampolineCursor = LogPut(trampolineLine, "STAGE2: entry trampoline done -- first exit ev=0x");
                    trampolineCursor = LogHex(trampolineCursor, (DWORD)event); trampolineCursor = LogPut(trampolineCursor, " at 0x"); trampolineCursor = LogHex(trampolineCursor, trampolineCs);
                    trampolineCursor = LogPut(trampolineCursor, ":0x"); trampolineCursor = LogHex(trampolineCursor, trampolineIp); trampolineCursor = LogPut(trampolineCursor, " VTIB EFLAGS=0x");
                    trampolineCursor = LogHex(trampolineCursor, VDM_REG(tib, VTIB_EFLAGS)); trampolineCursor = LogPut(trampolineCursor, " (IF=bit 9, VIF=bit 19, VIP=bit 20)\r\n");
                    LogAppend(LOG_PATH, trampolineLine, trampolineCursor); }
            }
        }

        /* HOW LONG DID ONE V86 RUN LAST WITHOUT GIVING US A TURN? (Skyroads
         * wobble, s61.) The big timer gaps are low-I/O, so the guest is not
         * hammering ports -- it is inside ONE long VdmRunGuest stretch (a spin, a
         * cli section, or slow interpreted VGA). This is the PM stretch
         * instrument for the V86 path: bucket the durations and keep the worst,
         * with the ENTRY cs:ip (where the stretch began) and the exit event. A
         * stretch >8 ms at native speed is 30M+ instructions -- so it is a WAIT
         * or a slow op, and its entry names the routine.
         */
        {   UINT bucket = 0;
            runMilliseconds = QpcMicroseconds(runEnd.QuadPart - runStart.QuadPart) / MICROSECONDS_PER_MILLISECOND_U;   /* ms */

            while (bucket < 7 && runMilliseconds >= (1u << bucket))
                ++bucket;

            g_V86StringHistogram[bucket]++;

            if (runMilliseconds >= 8u)
                ++g_V86StringCount8;

            if (runMilliseconds > g_V86StringMaximumMs)
            {
                g_V86StringMaximumMs = runMilliseconds;
                g_V86StringMaximumCs = eventCs;
                g_V86StringMaximumIp = eip;
                g_V86StringMaximumEvent = event;
            } } }

    *eventIo = event;
    *vdmStatusIo = vdmStatus;
}

/* While Mode Y needs it, run a slice of the guest in the interpreter rather than in V86. */
static INT V86RunModeYSlice(volatile BYTE * const tib)
{
    if (!g_P12Interp && ModeYNeedsInterp())
    {
        INT32 ran;
        UINT64 cyclesStart = ModeYTimelineRdtsc();
        g_ModeYInterp = 1;
        ran = HostInterpPaced(tib, P12_SLICE);
        g_ModeYInterp = 0;

        if (g_ModeYTimelineT0) { DWORD second = (GetTickCount() - g_ModeYTimelineT0) / MILLISECONDS_PER_SECOND_U;

                        if (second < YTL_SECS) { if (ran > 0)
                            g_ModeYTimelineIns[second] += (DWORD)ran;

                                              g_ModeYTimelineInterpreterCycles[second] += ModeYTimelineRdtsc() - cyclesStart; } }

        if (ran > 0)
        {
            g_ModeYSlices++;
            g_ModeYInstructions += (DWORD)ran;
            {
                return HOST_FLOW_CONTINUE;
            }
        }

        if (VDM_REG16(tib, VTIB_CS) == 0)
            ModeYRingDump("guest at CS=0 after a slice");

        /* Declined (a BOP, or an opcode it does not model): that ONE instruction
         * runs natively. Under a multi-plane mask a native A0000 store reaches only
         * the first selected plane -- so count these separately; they are the
         * remaining way this window can be wrong.
         */
        { DWORD codeSegment3 = VDM_REG16(tib, VTIB_CS), eip3 = VDM_REG16(tib, VTIB_EIP);
          const volatile BYTE *ip3 = (const volatile BYTE *)((codeSegment3 << PARAGRAPH_SHIFT) + eip3);

          if (!(ip3[0] == VDM_BOP0 && ip3[1] == VDM_BOP1))
          {
              BYTE mapMask = (BYTE)(g_Video.MapMask & VIDEO_ALL_PLANES);
              ++g_ModeYBails;

              if (mapMask & (BYTE)(mapMask - 1))
                  ++g_ModeYBailMp;

              ModeYBailNote(codeSegment3, eip3, ip3);
          } }
    }

    return HOST_FLOW_NEXT;
}

/* Mode 12h (GH #55): while a planar mode is current, the host is the CPU -- run a slice in the interpreter, whose A0000 accesses go through the planar write engine. */
static INT V86RunPlanarSlice(volatile BYTE * const tib)
{
    /* MODE 12h: THE HOST IS THE CPU (GH #55):
     * While a planar mode is current we do not hand the guest to V86 at all,
     * because on real hardware there is no way to see its A0000 writes there:
     * the page trap that would show them freezes the VDM (see VideoTrapSync).
     * Run a slice in the interpreter instead -- its A0000 accesses go through
     * the planar write engine -- then loop, which re-runs the IRQ delivery gate
     * above so timer and keyboard interrupts reach the guest between slices.
     * A slice ends early the moment an IRQ is pending, so the slice size is a
     * ceiling on lock-hold time, not on responsiveness.
     * If the interpreter declines the instruction we are ON (a BOP, or an
     * opcode it does not model), ran == 0 and we fall through to V86 exactly as
     * before -- so a DOS call still reaches the kernel as a BOP event, and an
     * unmodeled opcode still executes on the real CPU.
     */
    if (g_P12Interp && !g_DpmiPm)
    {
        INT32 ran = HostInterpPaced(tib, P12_SLICE);

        if (ran > 0)
        {
            g_P12Batches++;
            g_P12Instructions += (DWORD)ran;
            {
                return HOST_FLOW_CONTINUE;
            }
        }

        /* NAME THE OPCODE. Every bail is guest execution we cannot see, so the
         * list of declined opcodes IS the to-do list for this path (#27).
         *
         * [CAUTION]: NOT ON A BOP: our own `C4 C4 nn` stubs are the overwhelming majority
         * of bails and are not unmodelled opcodes. Per-site table, reported at
         * exit -- see g_P12Site.
         */
        { DWORD codeSegment3 = VDM_REG16(tib, VTIB_CS), eip3 = VDM_REG16(tib, VTIB_EIP);
          const volatile BYTE *ip3 = (const volatile BYTE *)((codeSegment3 << PARAGRAPH_SHIFT) + eip3);

          if (!(ip3[0] == VDM_BOP0 && ip3[1] == VDM_BOP1))
          {
            UINT index5;

            for (index5 = 0; index5 < g_P12SiteCount; ++index5)
                if (g_P12Site[index5].Cs == codeSegment3 && g_P12Site[index5].Ip == eip3)
                    break;

            if (index5 < g_P12SiteCount)
                g_P12Site[index5].Count++;
            else if (g_P12SiteCount < P12_SITE_MAX)
            {
                UINT index6;
                g_P12Site[index5].Cs = codeSegment3;
                g_P12Site[index5].Ip = eip3;
                g_P12Site[index5].Count = 1;

                for (index6 = 0; index6 < 8; ++index6)
                    g_P12Site[index5].Bytes[index6] = ip3[index6];

                ++g_P12SiteCount;
            }
            else
                ++g_P12SiteLost;
          } }

        g_P12Bails++;
    }

    return HOST_FLOW_NEXT;
}

/* Deliver a pending IRQ1 (the keyboard) to the guest when it can take an interrupt. */
static VOID V86DeliverKeyboardIrq(volatile BYTE * const tib)
{
    /* Deliver a pending keyboard IRQ1 as INT 09h, same IF-gating as IRQ0. The
     * scancode is already queued for port 0x60; INT 09h vectors through IVT[9]
     * to the game's own handler (or our IRET stub if it hasn't hooked one).
     * Excludes re-entry into our INT 08h (0x34) and INT 09h (0x4C) stubs.
     */
    if (g_Irq1Pending > 0)
    {
        DWORD cs = VDM_REG16(tib, VTIB_CS);
        DWORD ip = VDM_REG16(tib, VTIB_EIP);
        DWORD flags2;

        if (IsOurStubCsIp(cs, ip))     /* #206: BIOS stubs too */
        {
            DWORD ss = VDM_REG16(tib, VTIB_SS);
            DWORD stackPointer = VDM_REG16(tib, VTIB_ESP);
            flags2 = PeekWord((ss << PARAGRAPH_SHIFT) + ((stackPointer + X86_FRAME16_FLAGS) & WORD_MASK));
        }
        else
        {
            flags2 = VDM_REG(tib, VTIB_EFLAGS);
            IfvNote(IFV_PATH_VTIB_IRQ01, flags2);
        }

        /* WHY IS THIS REFUSED? MEASURED, NOT ASSUMED:
         * KEYLAT says 90% of keystrokes take >64 ms to reach INT 09h (max 9.6 s)
         * while the UI thread hands them over in 0 ms, so the refusal is here and
         * it is not rare -- it is the normal case. There are exactly three ways
         * out of this gate, and guessing which one has already cost four rounds.
         */
        ++g_Irq1Checks;

        if (!IfOrVif(flags2))
            ++g_Irq1NoIf;
        else if (cs == DOS_HDLR_SEG && ip >= DOS_HDLR_INT08_STUB_OFF && ip < DOS_HDLR_INT08_STUB_END)
            ++g_Irq1In08;
        else if (cs == DOS_HDLR_SEG && ip >= DOS_HDLR_INT09_STUB_OFF && ip < DOS_HDLR_INT09_STUB_END)
            ++g_Irq1In09;

        if (IfOrVif(flags2) && !(cs == DOS_HDLR_SEG &&
                              ((ip >= DOS_HDLR_INT08_STUB_OFF && ip < DOS_HDLR_INT08_STUB_END) || (ip >= DOS_HDLR_INT09_STUB_OFF && ip < DOS_HDLR_INT09_STUB_END))))
        {
            InterlockedDecrement(&g_Irq1Pending);   /* one INT 09h per queued scancode byte */
            VddPicAcknowledge(&g_Pic, 1);

            if (AsyncVectorIsOurStub(PIC_IRQ_KEYBOARD))
                VddPicEndOfInterrupt(&g_Pic, PIC_IRQ_KEYBOARD);

            g_Irq1Injected++;
            InjectInt(tib, VECTOR_KEYBOARD);
            KeyLatencyPop();               /* the guest is now IN its INT 09h */
        }
    }
}

/* Deliver a pending IRQ0 (the timer) to the guest when it can take an interrupt. */
static VOID V86DeliverTimerIrq(volatile BYTE * const tib)
{
    /* Deliver a pending PIT IRQ0 as INT 08h when the guest's main-line
     * interrupts are enabled. We regain control at event boundaries, almost
     * always inside a BOP stub (CS == DOS_HDLR_SEG) where the LIVE IF is the
     * handler's (cleared by the CD nn that vectored in) -- the guest's real
     * IF is the FLAGS the stub will IRET to, at SS:SP+4. Outside a stub
     * (e.g. an I/O fault from main-line) the live EFLAGS IF applies. Skip if
     * we're inside our own INT 08h stub, to avoid timer re-entrancy.
     */
    if (g_Irq0Pending)
    {
        DWORD cs = VDM_REG16(tib, VTIB_CS);
        DWORD ip = VDM_REG16(tib, VTIB_EIP);
        DWORD flags2;

        if (IsOurStubCsIp(cs, ip))     /* #206: BIOS stubs too */
        {
            DWORD ss = VDM_REG16(tib, VTIB_SS);
            DWORD stackPointer = VDM_REG16(tib, VTIB_ESP);
            flags2 = PeekWord((ss << PARAGRAPH_SHIFT) + ((stackPointer + X86_FRAME16_FLAGS) & WORD_MASK));   /* main-line FLAGS the stub returns to */
        }
        else
        {
            flags2 = VDM_REG(tib, VTIB_EFLAGS);
            IfvNote(IFV_PATH_VTIB_IRQ01, flags2);
        }

        if (IfOrVif(flags2) && Irq0CanDeliver()
            && !(cs == DOS_HDLR_SEG && ip >= DOS_HDLR_INT08_STUB_OFF && ip < DOS_HDLR_INT08_STUB_END))
        {
            InterlockedDecrement(&g_Irq0Pending);
            Irq0Ack();                     /* in service until the guest EOIs (s70) */
            g_Irq0Injected++;
            g_Irq0NoteCs = cs;
            g_Irq0NoteIp = ip;   /* where IF re-opened */
            Irq0DeliveredNote();          /* the guest's clock, as a timeline */
            InjectInt(tib, VECTOR_TIMER);
        }
        else
        {
            static INT skipBudget = 6;
            g_Irq0Skip++;                  /* IF=0 or inside our own INT 08h */

            if (cs == DOS_HDLR_SEG && ip >= DOS_HDLR_INT08_STUB_OFF && ip < DOS_HDLR_INT08_STUB_END)
                g_Irq0SkipStub++;
            else if (!IfOrVif(flags2))
            {
                g_Irq0SkipIf++;

                if (IsOurStubCsIp(cs, ip))     /* #238: who called our stub with IF off */
                {
                    DWORD ss = VDM_REG16(tib, VTIB_SS);
                    DWORD stackPointer = VDM_REG16(tib, VTIB_ESP);
                    SkipIfSiteNote(PeekWord((ss << PARAGRAPH_SHIFT) + ((stackPointer + X86_FRAME16_CS) & WORD_MASK)),
                                     PeekWord((ss << PARAGRAPH_SHIFT) + (stackPointer & WORD_MASK)), ip);
                }
            }

            VdmStateSample("irq0-skip", tib, &skipBudget);
        }
    }
}

/* The exec loop: run the guest until it terminates, a hard stop, or the window closes -- deliver pending IRQs, run it (in V86 or, for planar and Mode Y video, in the interpreter), and service what stopped it: port I/O, the DPMI switch, BIOS and DOS BOPs, the guest's own NTVDM BOPs, INT 21h. */
VOID HostRunExecLoop(
    PSTR *cursorIo,
    PSTR const base,
    DOS_MACHINE *machine,
    volatile BYTE * const tib,
    LONG *vdmStatusIo,
    CHAR *programPathBuffer)
{
    PSTR cursor = *cursorIo;
    DWORD event;
    LONG vdmStatus = *vdmStatusIo;
    DWORD rmStartTick = GetTickCount();   /* headless wall-clock cap origin (real-mode) */

    g_RunStartTick = rmStartTick;       /* published for the STAGE2 vsync rate line */

    while (g_Running)
    {
        /* Headless safety (session-9): the real-mode loop has no iteration cap so
         * interactive/animated programs run free -- but under the SMB auto-exit harness
         * a hung or infinite real-mode program (or a host bug) would run forever and
         * wedge rt.bat's `start /wait`, and the box is not easily accessible to unwedge.
         * So in headless mode bound it by wall clock: the host self-exits, rt.bat returns,
         * the watcher survives. (The PM loop got this in v69; the real-mode loop needs it
         * too -- a real-mode hang was the one path that could still permanently wedge the
         * rig.) GetTickCount per iteration is cheap; the loop runs once per event/BOP.
         */
        if (g_Headless && GetTickCount() - rmStartTick > PM_HEADLESS_MS)
        {
            cursor = LogPut(cursor, "STAGE2: headless time cap (");
            cursor = LogHex(cursor, PM_HEADLESS_MS); cursor = LogPut(cursor, " ms) reached -> exiting"
                     " (long/hung real-mode run; screenshots captured if graphical)\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            break;
        }

        /* File > Close Program (#152). Here, not mid-dispatch: the guest is stopped at
         * an event boundary, which is also where every injected IRQ is delivered.
         */
        if (g_CloseRequest && !g_WowLaunch)
        {
            InterlockedExchange(&g_CloseRequest, 0);

            if (g_ExecDepth > 0 || !g_TopIsShell)
            {
                if (CloseProgramNow(machine, (VOID *)tib, &cursor, base))
                    continue;

                break;
            }
        }

        /* Pump the PIT tick from THIS thread's wall clock. Normally the UI thread
         * raises IRQ0, but a heavy I/O-trap loop (e.g. Skyroads' OPL/timer delay poll
         * that faults on every IN 388h) starves the UI thread, so the guest's timer
         * would crawl (~100x too slow) and any tick-paced delay appears to hang. Set
         * the pending flag at the ~18.2 Hz BIOS rate from here; it coalesces with the
         * UI thread's flag (both just set it to 1) so there's no double-count.
         */
        /* Advance the PIT from the real clock every iteration. This is also what generates
         * IRQ0 now, at whatever rate the GUEST programmed into channel 0 -- the old fixed
         * 55 ms pump hard-wired 18.2 Hz, so a game that reprograms the timer for its music
         * (as this one does) had its sequencer clocked far too slowly no matter what.
         */
        HostPitSync();
        OplPumpTime();            /* keep the OPL timers current for the guest */
        HostKeyTypematic();       /* the keyboard repeats even when the UI stalls */
        HostKeyPresent();         /* ...and presents the next byte after its transfer time */
        V86DeliverTimerIrq(tib);
        V86DeliverKeyboardIrq(tib);
        V86DeliverDeviceIrq(tib);   /* see the helper: shared with the nested 0301/0302 loop */
        MouseCallbackTry(tib);          /* INT 33h 0Ch events, under the same gate as an IRQ */
        /* Mirror the guest's IF into EFLAGS.VIF before handing the context back. On VME
         * hardware the kernel's deliverability test reads VIF, and VIF is lost every time
         * we synthesise an interrupt frame ourselves -- so a guest that has interrupts
         * enabled still looks disabled to the kernel, which then just sets VIP and defers.
         * With VIP set and VIF clear the guest's next IRET faults into a dispatch that
         * refuses to deliver and re-arms VIP: a livelock, measured on the rig as the guest
         * frozen on the IRET at DOS_HDLR_SEG:0x0003. Keeping the two flags in step is what
         * lets the kernel dispatch instead of deferring.
         */
        /* NOTE, measured: do NOT touch bit 9 (0x200) of FIXED_NTVDMSTATE. It is the VDM's
         * virtual interrupt flag and the KERNEL already maintains it -- it read 0x...3230
         * (bit set) from the first instruction. Mirroring our own IF into it only clobbered
         * correct state (the word went 0x3230 -> 0x3030) and changed nothing else.
         */
        if (g_QiVif)
        {
            if (VDM_REG(tib, VTIB_EFLAGS) & EFLAGS_IF)
                VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_VIF;
            else
                VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_VIF;
        }

        {
            INT flow = V86RunPlanarSlice(tib);

            if (flow == HOST_FLOW_CONTINUE)
                continue;
        }
        {
            INT flow = V86RunModeYSlice(tib);

            if (flow == HOST_FLOW_CONTINUE)
                continue;
        }

        while (g_PauseWant && g_Running) /* #219 */
        {
            ++g_PauseCooperative;
            Sleep(PAUSE_POLL_MS);
        }

        CpuSpeedCooperativePark();                                                  /* #225 */
        InterlockedExchange(&g_InExec, 1);
        ExecEnterMark();               /* guest-execution clock starts (throttle) */
        V86RunGuestTimed(tib, &event, &vdmStatus);
        g_EventHistogram[event < EV_HIST_MAX ? event : EV_HIST_MAX - 1]++;
        ExecLeaveMark();               /* ...and stops. Our servicing is not its */
        InterlockedExchange(&g_InExec, 0);
        /* The VM events that follow a mouse-callback injection, verbatim. See
         * g_MouseCallbackTrace. Logged before any arm below acts on the event, so what is
         * recorded is what the kernel handed back, not what we made of it.
         */
        if (g_MouseCallbackTrace > 0)
        {
            CHAR tibLine2[384];
            CHAR *tibCursor2 = tibLine2;
            DWORD trampolineCs = VDM_REG16(tib, VTIB_CS);
            DWORD trampolineIp = VDM_REG16(tib, VTIB_EIP);
            DWORD tibSs = VDM_REG16(tib, VTIB_SS);
            DWORD tibSp = VDM_REG16(tib, VTIB_ESP);
            --g_MouseCallbackTrace;
            tibCursor2 = LogPut(tibCursor2, "MOUSECB-TRACE ev=0x"); tibCursor2 = LogHex(tibCursor2, event);
            tibCursor2 = LogPut(tibCursor2, " info=0x"); tibCursor2 = LogHex(tibCursor2, VDM_REG(tib, VTIB_EVENT_INFO));
            tibCursor2 = LogPut(tibCursor2, " st=0x");   tibCursor2 = LogHex(tibCursor2, (DWORD)vdmStatus);
            tibCursor2 = LogPut(tibCursor2, " cs:ip=0x"); tibCursor2 = LogHex(tibCursor2, trampolineCs); tibCursor2 = LogPut(tibCursor2, ":0x"); tibCursor2 = LogHex(tibCursor2, trampolineIp);
            tibCursor2 = LogPut(tibCursor2, " efl=0x");  tibCursor2 = LogHex(tibCursor2, VDM_REG(tib, VTIB_EFLAGS));
            tibCursor2 = LogPut(tibCursor2, " ss:sp=0x"); tibCursor2 = LogHex(tibCursor2, tibSs); tibCursor2 = LogPut(tibCursor2, ":0x"); tibCursor2 = LogHex(tibCursor2, tibSp);
            tibCursor2 = LogPut(tibCursor2, " ax=0x");   tibCursor2 = LogHex(tibCursor2, VDM_REG16(tib, VTIB_EAX));
            tibCursor2 = LogPut(tibCursor2, " [714]=0x"); tibCursor2 = LogHex(tibCursor2, *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR);
            tibCursor2 = LogPut(tibCursor2, " active="); tibCursor2 = LogHex(tibCursor2, (DWORD)g_MouseCallbackActive);
            tibCursor2 = LogPut(tibCursor2, " code=");   tibCursor2 = LogDump(tibCursor2, (const VOID *)(ULONG_PTR)((trampolineCs << PARAGRAPH_SHIFT) + trampolineIp), 8);
            tibCursor2 = LogPut(tibCursor2, " stack=");  tibCursor2 = LogDump(tibCursor2, (const VOID *)(ULONG_PTR)((tibSs << PARAGRAPH_SHIFT) + tibSp), 12);
            tibCursor2 = LogPut(tibCursor2, "\r\n");
            LogAppend(LOG_PATH, tibLine2, tibCursor2);
        }

        /* WHEN VdmStartExecution RETURNS A FAILURE STATUS, SAY SO. (session 62):
         * The V86 path ignored `st` entirely -- only the DPMI branch below ever
         * read it -- so a guest that executes an INT3 (or any fault the kernel
         * turns into a returned NTSTATUS rather than a serviceable event) died
         * with the host process exit code equal to that status and NOTHING in the
         * log. That is exactly Mario's flaky ~2s death: exit 0x80000003
         * (STATUS_BREAKPOINT), reached in the MAIN LOOP well past the joystick
         * poll, with the last heartbeat the only witness. Name the guest cs:ip
         * and the bytes there so the INT3's origin is visible. Bounded; the top
         * bit of an NTSTATUS is set for both warning (0x8...) and error (0xC...).
         */
        if ((UINT)vdmStatus & NT_STATUS_NOT_SUCCESS_BIT_U)
        {
            static INT stateBudget = 16;

            if (stateBudget > 0)
            {
                DWORD stateCs = VDM_REG16(tib, VTIB_CS);
                DWORD si = VDM_REG16(tib, VTIB_EIP);
                const volatile BYTE *sp3 = (const volatile BYTE *)((stateCs << PARAGRAPH_SHIFT) + si);
                UINT index7;
                --stateBudget;
                cursor = LogPut(cursor, "V86-STATUS 0x"); cursor = LogHex(cursor, (UINT)vdmStatus);
                cursor = LogPut(cursor, " ev=0x"); cursor = LogHex(cursor, event);
                cursor = LogPut(cursor, " at 0x"); cursor = LogHex(cursor, stateCs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, si);
                cursor = LogPut(cursor, " bytes:");

                for (index7 = 0; index7 < 8; ++index7)
                {
                    cursor = LogPut(cursor, " ");
                    cursor = LogHexByte(cursor, sp3[index7]);
                }

                cursor = LogPut(cursor, "\r\n");
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            }
        }

        /* SPIKE: once in protected mode, stop at the FIRST PM event and dump the raw
         * taxonomy (event/info/selectors) -- this is how the spike learns how the
         * monitor reflects a PM INT 31h / fault. Later increments replace this with
         * a real INT 31h dispatch.
         */
        if (g_DpmiPm)
        {
            DWORD currentCs = VDM_REG16(tib, VTIB_CS);
            DWORD currentIp = VDM_REG16(tib, VTIB_EIP);
            cursor = LogPut(cursor, "STAGE3-DPMI: PM stop event=0x"); cursor = LogHex(cursor, event);
            cursor = LogPut(cursor, " status=0x"); cursor = LogHex(cursor, (UINT)vdmStatus);
            cursor = LogPut(cursor, " info=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EVENT_INFO));
            cursor = LogPut(cursor, " CS:IP=0x"); cursor = LogHex(cursor, currentCs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, currentIp);
            cursor = LogPut(cursor, " EFL=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EFLAGS));
            cursor = LogPut(cursor, " SS:SP=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_SS));
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ESP));
            cursor = LogPut(cursor, "\r\n  VTIB[5A8..]: "); cursor = LogDump(cursor, (const VOID *)(tib + 0x5A8), 0x20);
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            break;
        }

        {
            INT flow = V86ServiceIoEvent(&cursor, base, event, tib);

            if (flow == HOST_FLOW_CONTINUE)
                continue;
        }
        {
            INT flow = V86ReportUnexpectedStop(&cursor, base, event, tib, vdmStatus);

            if (flow == HOST_FLOW_BREAK)
                break;
        }
        {
            static INT bopBudget = 6;
            VdmStateSample("bop", tib, &bopBudget);
        }
        /* WHOSE BOP IS THIS? THE NUMBER DOES NOT SAY. (s78) (Importance = 3):
         * `C4 C4 nn` is NTVDM's call-out instruction and the number space is NTVDM's,
         * not ours. Ours were assigned freely and two of them are already taken by a
         * guest that ships with the OS: XP's COMMAND.COM issues `BOP 0x54` fifteen
         * times and `BOP 0x50` once, while ours are DPMI_RMRET and the DPMI entry.
         *
         * [INFO]: THE DISCRIMINATOR IS THE ADDRESS, NOT THE NUMBER. Every BOP we plant, we
         * plant at an address we own -- DOS_HDLR_SEG for the INT stubs, the DPMI
         * entry/return catchers and the callback slots, DOS_CTAB_SEG for the BIOS
         * stubs. A BOP executing anywhere else is the GUEST's own code calling
         * NTVDM, and must not be answered as if it were one of ours.
         *
         * [CAUTION]: This is cheaper AND safer than renumbering ours out of the way: the numbers
         * we would move to are equally NTVDM's, so renumbering only relocates the
         * collision, while the origin check is exact. See docs/inventory/bop.md.
         */
        {
            DWORD bopCs = VDM_REG16(tib, VTIB_CS);
            g_BopFromGuest = (bopCs != DOS_HDLR_SEG && bopCs != DOS_CTAB_SEG);
            ++g_BopHistogram[VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK];     /* #238 */
        }
        /* Route the BOP by its number.
         * -- The BOP numbers our stubs share with the nested DPMI loop: INT 10h/16h/33h,
         * the BIOS block (11h-17h, 25h/26h, 28h/29h), 1Ah, 2Fh, the XMS entry and
         * INT 67h. One copy, in V86BiosBop() (GH #247).
         */
        {   INT biosResult = V86BiosBop(tib, VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK, &cursor, base);

            if (biosResult != V86BOP_NONE)
                continue;                                     /* DONE or RERUN: both resume the guest */
        }

        if ((VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK) == MS_CB_BOP)     /* INT 33h handler returned */
        {
            MouseCallbackReturn(tib);
            continue;
        }

        {
            INT flow = V86ServiceKeyboardBop(tib);

            if (flow == HOST_FLOW_CONTINUE)
                continue;
        }
        {
            INT flow = V86ServiceTimerBop(tib);

            if (flow == HOST_FLOW_CONTINUE)
                continue;
        }
        {
            INT flow = DosServiceTerminateBop(&cursor, base, tib, machine);

            if (flow == HOST_FLOW_BREAK)
                break;

            if (flow == HOST_FLOW_CONTINUE)
                continue;
        }
        /* [CAUTION]: `!g_BopFromGuest`: XP's COMMAND.COM issues a `BOP 0x50` of its own (one
         * site, in its "Incorrect DOS version" path). Without the origin test that
         * would be serviced as a DPMI real-to-protected mode switch.
         */
        if (!g_BopFromGuest
            && (VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK) == DPMI_BOP)    /* DPMI real->PM switch */
        {
            {
                INT flow = DpmiStartClientSession(&cursor, base, tib, machine);

                if (flow == HOST_FLOW_BREAK)
                    break;

                if (flow == HOST_FLOW_CONTINUE)
                    continue;
            }
        }

        /* THIS IS THE FALL-THROUGH, AND IT ANSWERS FOR EVERY BOP IT WAS NEVER
         *   GIVEN. (s78) ------------------------------------------------------
         * ev is already known to be VDM_EVENT_BOP here, and every arm above matches
         * an EXACT code -- INT 21h's own stub is `C4 C4 20` (see the bop[] table at
         * the install). So anything that reaches this line is a BOP we do not
         * implement, and we hand it to the DOS INT 21h handler anyway, where the
         * guest's AH decides what it "asked" for.
         *
         * [INFO]: XP's COMMAND.COM is the guest that made this visible. It is NTVDM-AWARE:
         * its image contains sixteen `C4 C4` sites -- fifteen of them BOP 0x54 with
         * a sub-function byte after it, one BOP 0x50. Our 0x54 is DPMI_RMRET_BOP and
         * our 0x50 is the DPMI entry, so the NUMBERS COLLIDE with the ones a real
         * NTVDM guest uses. The one at 0x9342:0x03ce arrives with AX=0x0002, falls
         * through here, is read as INT 21h AH=00, and terminates the guest -- which
         * we then report as a CLEAN EXIT, CODE 0. Four sessions read that as
         * "COMMAND.COM gives up"; it never called DOS at all.
         *
         * [CAUTION]: THE GATE IS MEASURED, NOT ASSUMED. The arms above are long and one of them
         * -- the BIOS block -- deliberately sets `handled = 0` and falls through, so
         * "require 0x20" needed evidence rather than a reading of the control flow.
         * The diagnostic below shipped first (92e2136) and the battery was run with
         * it: 17 DOS guests (Doom, Hexen, Duke3D, Heretic, heaven7, Skyroads, Wolf3D,
         * Mario, Lemmings, Chasm, Gothica, Radiance, Fusion, Skyxmas, vesacube, MEM,
         * 6.22's COMMAND.COM) and 3 Win16 apps (Notepad, Paint, WinMine) -- ALL of
         * them confirmed loaded, **zero** fall-throughs. Only XP's COMMAND.COM
         * reaches here. ([CAUTION] Lemmings' first row was a NO SUCH TARGET and its zero was
         * not evidence; re-run under its real entry, `lemvga.com`.)
         *
         * [CAUTION]: WE STOP, we do not skip. Skipping needs the BOP's encoded LENGTH, and that
         * is per-call: `0x54` carries a sub-function byte, so `EIP += 3` would leave
         * that byte to execute as an instruction. A refusal we cannot encode is
         * better reported than faked -- see the standing note that an unimplemented
         * call which still ANSWERS is worse than one that does not.
         */
        /* AN NTVDM BOP FROM THE GUEST'S OWN CODE. (s78) (Importance = 3):
         * XP's COMMAND.COM is NTVDM-aware and this is how it talks to the 32-bit side.
         * Both of its numbers carry a SUB-FUNCTION BYTE after the BOP, so the
         * instruction is FOUR bytes, not three -- read off the guest's bytes at the
         * BOP site (logged), not assumed: what follows decodes as a sensible
         * instruction at +4 and as junk at +3, for both 0x54 and 0x50.
         *
         * [CAUTION]: That is a claim about THESE TWO numbers, from this one guest. It is not a
         * general rule about BOP encoding, and must be re-derived for any other
         * number that turns up here.
         * - WHAT TO ANSWER IS NOT KNOWN YET, so it is a knob rather than a guess:
         *   cfg\bop54.txt = "cf1" (default) or "cf0". What sub 01 does next depends on
         *   carry, so the two settings take COMMAND.COM down different paths and the
         *   difference is the measurement. Every call is logged with full
         *   registers so the two runs can be diffed.
         *
         * [WARNING]: A BOP IS NOT AN INT: nothing was pushed, so CF goes in the live EFLAGS.
         */
        /* s91 (#11): a guest's own `C4 C4 58 nn` is the third-party BOP -- see IsvBop.
         * Four bytes (the sub-function follows), like 50h/54h below.
         */
        if (g_BopFromGuest && (VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK) == NTVDM_BOP_ISV)
        {
            DWORD codeSegment = VDM_REG16(tib, VTIB_CS);
            DWORD instructionPointer = VDM_REG16(tib, VTIB_EIP);
            const volatile BYTE *isvBopBytes = (const volatile BYTE *)(ULONG_PTR)((codeSegment << PARAGRAPH_SHIFT) + instructionPointer);
            IsvBop(tib, isvBopBytes[VDM_BOP_LENGTH], &cursor);
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_SUBFUNCTION_LENGTH;
            continue;
        }

        if (g_BopFromGuest
            && ((VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK) == NTVDM_BOP_CMD
                || (VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK) == NTVDM_BOP_DOS))
        {
            {
                INT flow = NtvdmServiceGuestBop(&cursor, base, tib, machine, programPathBuffer);

                if (flow == HOST_FLOW_BREAK)
                    break;

                if (flow == HOST_FLOW_CONTINUE)
                    continue;
            }
        }

        if ((VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK) != DOS_BOP_INT21)
        {
            DWORD codeSegment = VDM_REG16(tib, VTIB_CS);
            DWORD instructionPointer = VDM_REG16(tib, VTIB_EIP);
            const volatile BYTE *isvBopBytes = (const volatile BYTE *)(ULONG_PTR)((codeSegment << PARAGRAPH_SHIFT) + instructionPointer);
            cursor = LogPut(cursor, "STAGE2: UNIMPLEMENTED BOP -- refusing (NOT an INT 21h call): bop=0x");
            cursor = LogHexByte(cursor, VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK);
            cursor = LogPut(cursor, " next=0x"); cursor = LogHexByte(cursor, isvBopBytes[VDM_BOP_LENGTH]);   /* the sub-function byte */
            cursor = LogPut(cursor, " at 0x");  cursor = LogHex(cursor, codeSegment); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, instructionPointer);
            cursor = LogPut(cursor, " ax=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EAX));
            cursor = LogPut(cursor, " bx=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX));
            cursor = LogPut(cursor, " dx=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EDX));
            cursor = LogPut(cursor, "\r\n         see docs/inventory/bop.md -- the C4 C4 number space is NTVDM's\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            /* A DISTINCT, NON-ZERO exit code. The whole defect this closes was a guest
             * killed by an unimplemented call and reported as a clean exit 0; reusing 0
             * here would leave the lie in place with better logging on top of it.
             */
            machine->ExitCode = UNIMPLEMENTED_BOP_EXIT_CODE;
            break;
        }

        /* #34: THE GUEST'S INT 24h HAS ANSWERED. Recognised by ADDRESS: BOP 20h is
         * also the INT 21h BOP, and this one sits at DOS_CRIT_RETURN, where only
         * CriticalRaise ever sends the guest.
         */
        if (!g_BopFromGuest && VDM_REG16(tib, VTIB_CS) == DOS_CTAB_SEG
            && VDM_REG16(tib, VTIB_EIP) == DOS_CRIT_RETURN)
        {
            INT criticalAction = CriticalReturn(machine, tib, &cursor);
            LogAppend(LOG_PATH, base, cursor); cursor = base;

            if (criticalAction)                                  /* ABORT: end the program, as 4Ch */
            {
                if (DosTerminate(machine, tib, &cursor, base))
                    continue;

                break;
            }

            continue;                                /* RETRY re-runs it; FAIL/IGNORE resume */
        }

        if (!machine->IsCritActive)
            CriticalSnapshot(tib);                              /* #34: the call's INPUT registers (not the handler's own calls) */

        machine->TraceCursor = cursor;
        machine->IsRetry = 0;
        machine->CanTrampoline = 1;                         /* #251: we can resume elsewhere */
        machine->CanRaiseCrit = 1;                        /* #275: ...and raise INT 24h (below) */

        if (!DosInt21(machine))                         /* AH=4Ch -> terminate */
        {
            machine->CanTrampoline = 0;
            machine->CanRaiseCrit = 0;
            cursor = machine->TraceCursor;

            if (DosTerminate(machine, tib, &cursor, base))
                continue;

            break;
        }

        machine->CanTrampoline = 0;
        machine->CanRaiseCrit = 0;
        cursor = machine->TraceCursor;

        if (machine->Trampoline)                            /* #251: into DOS's AUX/PRN driver code */
        {
            VDM_SET16(tib, VTIB_CS, DOS_CTAB_SEG);
            VDM_REG(tib, VTIB_EIP) = machine->Trampoline;
            machine->Trampoline = 0;
            LogAppend(LOG_PATH, base, cursor); cursor = base;
            continue;
        }

        if (machine->IsCritPending)                         /* #34: call the guest's INT 24h */
        {
            CriticalRaise(machine, tib, &cursor);
            LogAppend(LOG_PATH, base, cursor); cursor = base;
            continue;                               /* CS:IP is now the INT 24h site */
        }

        if (machine->IsExecPending)                         /* GH #30: AH=4Bh */
        {
            machine->IsExecPending = 0;
            cursor = ExecBegin(machine, tib, cursor);
            LogAppend(LOG_PATH, base, cursor); cursor = base;
            continue;                               /* CS:IP now points at the child */
        }

        /* A blocking read with nothing to return leaves EIP ON the BOP, so the guest
         * re-executes the INT and keeps running -- and keeps taking timer interrupts, so
         * its music and animation carry on while it waits for a key, as on real hardware.
         */
        if (!machine->IsRetry)
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;                     /* past the 3-byte BOP -> the IRET */

        LogAppend(LOG_PATH, base, cursor); cursor = base;
    }

    *cursorIo = cursor;
    *vdmStatusIo = vdmStatus;
}
