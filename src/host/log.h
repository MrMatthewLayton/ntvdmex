/* log.h -- tiny no-CRT logging helpers for the host.
 *
 * LogPut/LogHex/LogDump build text into a caller-owned buffer; LogWrite/LogAppend flush
 * it to a file. Header-only static-inline (no shared state); the writers take the
 * log path explicitly rather than hard-coding it (the spike hard-coded
 * C:\ntvdmex\vdmhost.log). Ported from tools/vdmhost/vdmhost.c. No CRT -- only
 * kernel32 file APIs, so it loads on XP.
 */
#ifndef HOST_LOG_H
#define HOST_LOG_H

#include <windows.h>
#include "../ntvdmex_bits.h"     /* defines only: NIBBLE_SHIFT/MASK */
#include "../ntvdmex_ascii.h"    /* defines only: DECIMAL_RADIX     */

/* Append the ASCIIZ string s to p; return the new end (NUL-terminated). */
/* The digits these helpers write. */
#define LOG_HEX_DIGITS       8
#define LOG_HEX_BYTE_DIGITS  2
#define LOG_HEX_TOP_SHIFT    28      /* the first of a DWORD's eight digits     */
#define LOG_DECIMAL_DIGITS   10
#define LOG_DUMP_ROW_MASK    0xF     /* LogDump: sixteen bytes a line           */

static inline PSTR LogPut(PSTR cursor, PCSTR text) {
    while (*text) *cursor++ = *text++;
    *cursor = 0;
    return cursor;
}

/* Append v as 8 lowercase hex digits. */
static inline PSTR LogHex(PSTR cursor, UINT value) {
    INT index; CHAR digits[LOG_HEX_DIGITS + 1]; digits[LOG_HEX_DIGITS] = 0;
    for (index = LOG_HEX_DIGITS - 1; index >= 0; --index) { digits[index] = "0123456789abcdef"[value & NIBBLE_MASK]; value >>= NIBBLE_SHIFT; }
    return LogPut(cursor, digits);
}

/* Append v as 2 lowercase hex digits. For byte-sized things -- interrupt numbers,
   AH values, mode numbers -- where LogHex's 8 digits turn a list into a wall. */
static inline PSTR LogHexByte(PSTR cursor, UINT value) {
    CHAR digits[LOG_HEX_BYTE_DIGITS + 1]; digits[0] = "0123456789abcdef"[(value >> NIBBLE_SHIFT) & NIBBLE_MASK];
    digits[1] = "0123456789abcdef"[value & NIBBLE_MASK]; digits[LOG_HEX_BYTE_DIGITS] = 0;
    return LogPut(cursor, digits);
}

/* Append a raw hex dump of n bytes at b (space-separated, newline every 16). */
/* Plain decimal. Everything else here is hex because it is describing machine state,
   where hex is the readable form -- but a SCREEN RESOLUTION is not machine state, and
   "0xa00 x 0x640" is not a thing anyone can check against their display settings. */
static inline PSTR LogDecimal(PSTR cursor, UINT value) {
    CHAR digits[LOG_DECIMAL_DIGITS + 1]; INT count = 0;
    if (!value) { *cursor++ = '0'; *cursor = 0; return cursor; }
    while (value && count < LOG_DECIMAL_DIGITS) { digits[count++] = (CHAR)('0' + value % DECIMAL_RADIX); value /= DECIMAL_RADIX; }
    while (count) *cursor++ = digits[--count];
    *cursor = 0;
    return cursor;
}

static inline PSTR LogDump(PSTR cursor, LPCVOID bytes, UINT length) {
    const BYTE *source = (const BYTE *)bytes; UINT index;
    for (index = 0; index < length; ++index) {
        *cursor++ = "0123456789abcdef"[source[index] >> NIBBLE_SHIFT];
        *cursor++ = "0123456789abcdef"[source[index] & NIBBLE_MASK];
        *cursor++ = ((index & LOG_DUMP_ROW_MASK) == LOG_DUMP_ROW_MASK) ? '\n' : ' ';
    }
    *cursor = 0;
    return cursor;
}

/* Runaway-log guard. An infinite guest loop that logs each serviced INT can grow
   the log without bound -- a headless pm32irq (an infinite mode-13h animation demo)
   flooded 148 MB, thrashed the disk, and wedged the SMB result-copy. Cap the total
   appended bytes; past the cap LogAppend silently drops (writing one truncation
   marker). LogWrite (the STAGE0 truncate that starts a fresh run) resets the count. */
/* ── THE LOG'S PATH LIVES WITH THE LOG. (session 56) ─────────────────────────
     It used to be defined in main.c AFTER the headers that want it, so anything
     included earlier -- wow32.h, for one -- could call LogAppend but had no
     name for the file to pass it. Two copies of a path is how one of them goes
     stale; one definition, beside the function that opens it. */
#ifndef LOG_PATH
#define LOG_PATH    "C:\\ntvdmex\\ntvdmhost.log"
#endif

#define LOG_MAX_BYTES (256u * 1024u * 1024u)  /* 4 MB -> 32 MB -> 256 MB. A client that RUNS
                                                 produces a long trace, and truncating it hides
                                                 exactly the part that matters. Measured: with
                                                 pmverbose.flag, Doom past the D/B fix fills 32 MB
                                                 BEFORE it dies, so the cap looked like the
                                                 stopping point -- an instrument lying by
                                                 omission. On a silent VDM teardown nothing runs
                                                 afterwards, so anything not already flushed is
                                                 gone: the ceiling has to clear the whole run. */
static unsigned long g_LogTotal  = 0;
static INT           g_LogIsCapped = 0;

/* ── ★★★★ ONE HANDLE, KEPT OPEN -- AND THIS IS A PERFORMANCE FIX, NOT TIDYING.
     LogAppend used to CreateFile + WriteFile + CloseHandle on EVERY line. An
     open/close pair is two kernel transitions plus filesystem metadata work, and
     it is the dominant cost of a log line by a wide margin -- the payload is
     usually under a hundred bytes.
   ★ THIS PROJECT HAS ALREADY PAID FOR THIS ONCE. See host_irq_sink's note in
     main.c: per-line LogAppend under the device lock cost SKYROADS 24% OF ITS
     DELIVERED TIMER TICKS and 34% of its I/O, and only a player's ear caught it.
     It came back on the WOW path, where every WOW32 BOP writes a multi-line
     block -- a Solitaire startup is 2.7 MB of log -- and the symptom this time
     was a user reporting both games as "laggy, like an early 486" and GDI redraw
     as visibly slow when windows overlap.
   ⚠ DURABILITY IS UNCHANGED. Every line is still handed to the OS at the moment
     it is written -- there is no user-space buffering here -- so a host crash
     still leaves the trace-so-far on disk, which is the property the whole
     debugging method rests on. The share flags are unchanged too, so the log is
     still readable from outside while a run is in progress.
   ⚠ The handle is keyed by PATH: LogWrite truncates and starts a new run, so it
     must drop the cached handle rather than keep appending to the old file.
   ⚠⚠ KEYED BY A COPY OF THE PATH, NOT BY THE CALLER'S POINTER (s73). Since s71 every
     path is composed at the call site into one of ntvdmex_path()'s SIXTEEN ring
     slots, so a cached pointer names whatever that slot holds NOW. Measured on the
     Win16 path, which alternates LOG_PATH with LDTLOG_PATH: the slot cached for
     ldtprobe.log was later refilled with ntvdmhost.log's text, LogIsSamePath said
     "same file", and every STAGE line of the run went into ldtprobe.log while
     ntvdmhost.log stayed empty. A DOS run never showed it -- it uses one path. */
#define LOG_PATH_SLACK       96      /* room past MAX_PATH for a composed path  */
#define LOG_MESSAGE_SIZE     128
#define LOG_MAX_RANGE        (16u << 20)   /* a line longer than 16 MB is a bad call */
#define LOG_ROTATION_SLACK   16
#define LOG_ROTATION_SUFFIX  8       /* "-k" and the rest                       */
#define LOG_EXTENSION_LENGTH 4       /* ".log"                                  */
static HANDLE g_LogHandle = INVALID_HANDLE_VALUE;
static CHAR   g_LogHandlePathBuffer[MAX_PATH + LOG_PATH_SLACK];
static PCSTR g_LogHandlePath = 0;        /* -> g_LogHandlePathBuffer when a handle is open */

static inline INT LogIsSamePath(PCSTR first, PCSTR second) {
    if (first == second) return 1;
    if (!first || !second) return 0;
    while (*first && *first == *second) { ++first; ++second; }
    return *first == *second;
}

static inline VOID LogClose(VOID) {
    if (g_LogHandle != INVALID_HANDLE_VALUE) CloseHandle(g_LogHandle);
    g_LogHandle = INVALID_HANDLE_VALUE;
    g_LogHandlePath = 0;
}

/* Overwrite `path` with [buf..end). Resets the runaway guard -- a new run starts here. */
/* ── ⚠ A CALLER WITH end < buf IS A BUG IN THE CALLER, AND IT USED TO BE FATAL TO THE
     INSTRUMENT (s73). (DWORD)(end - buf) wraps to ~4 GB, the runaway guard trips on
     that ONE call, and every later line of the run is suppressed -- the file holds the
     cap marker and nothing else, which reads as "runaway guest" when it is a stale
     pointer in the host. Measured on the Win16 path: a 66-byte log and a host that
     died in under a second, with no way to see where. Now the bad call is reported,
     in the file, with both pointers, and the run goes on logging. */
static inline INT LogIsBadRange(PCSTR path, PCSTR buffer, PCSTR end) {
    HANDLE file; CHAR message[LOG_MESSAGE_SIZE], *cursor = message; DWORD written; UINT value; INT index;
    if (end >= buffer && (unsigned long)(end - buffer) < LOG_MAX_RANGE) return 0;
    for (index = 0; "\r\n[log: BAD RANGE from a caller: buf=0x"[index]; ++index) *cursor++ = "\r\n[log: BAD RANGE from a caller: buf=0x"[index];
    for (value = (UINT)(ULONG_PTR)buffer, index = LOG_HEX_TOP_SHIFT; index >= 0; index -= NIBBLE_SHIFT) *cursor++ = "0123456789abcdef"[(value >> index) & NIBBLE_MASK];
    for (index = 0; " end=0x"[index]; ++index) *cursor++ = " end=0x"[index];
    for (value = (UINT)(ULONG_PTR)end, index = LOG_HEX_TOP_SHIFT; index >= 0; index -= NIBBLE_SHIFT) *cursor++ = "0123456789abcdef"[(value >> index) & NIBBLE_MASK];
    for (index = 0; " -- line dropped, run continues]\r\n"[index]; ++index) *cursor++ = " -- line dropped, run continues]\r\n"[index];
    file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) { WriteFile(file, message, (DWORD)(cursor - message), &written, NULL); CloseHandle(file); }
    return 1;
}

/* ── KEEP THE LAST FEW RUNS. (2026-09-22, the first field report) ─────────────────
     The zip went to a machine we cannot reach. Its first session was broken -- every
     DOS/4GW game crawled and stalled -- and its second was fine, and by the time the
     report arrived the only log on the box was the LAST run's, because every start
     truncates ntvdmhost.log. The one file that explains a failure had been overwritten
     by the runs that worked. So the first truncate of a process first shifts what is
     there: ntvdmhost.log -> ntvdmhost-1.log -> ... -> ntvdmhost-LOG_KEEP.log, the
     oldest dropped. A field machine then holds the last LOG_KEEP+1 runs for someone to
     copy back by hand; nothing here can read them for us.
   ⚠ ONCE PER PROCESS. LogWrite is called more than once in a run (the STAGE0 line,
     then the preamble re-truncates), and a rotation on each would shift one run into
     several files. The flag is per-process, which is per-run.
   ⚠ MoveFileEx over an existing target: on a share the target can be open elsewhere
     (the rig's watcher tails the log), in which case the shift fails and the run
     simply overwrites as it always did -- rotation is best-effort and must never
     stop the log itself from being written. */
#define LOG_KEEP 5
static INT g_LogIsRotated = 0;

/* "<stem>.log" + k -> "<stem>-k.log" (a name without .log gets the suffix at its end). */
static inline VOID LogRotationName(PSTR out, PCSTR path, INT length, INT stem, INT number) {
    INT index; PSTR cursor = out;
    for (index = 0; index < stem; ++index) *cursor++ = path[index];
    *cursor++ = '-'; *cursor++ = (CHAR)('0' + number);
    for (index = stem; index < length; ++index) *cursor++ = path[index];
    *cursor = 0;
}

static inline VOID LogRotateOnce(PCSTR path) {
    CHAR from[MAX_PATH + LOG_ROTATION_SLACK], to[MAX_PATH + LOG_ROTATION_SLACK];
    INT length = 0, stem, number;
    if (g_LogIsRotated) return;
    g_LogIsRotated = 1;
    while (path[length]) ++length;
    if (length < LOG_EXTENSION_LENGTH || length + LOG_ROTATION_SUFFIX >= (INT)sizeof from) return;
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;   /* nothing to keep */
    stem = length;
    if (path[length-LOG_EXTENSION_LENGTH] == '.' && (path[length-3] | ASCII_CASE_BIT) == 'l' && (path[length-2] | ASCII_CASE_BIT) == 'o'
        && (path[length-1] | ASCII_CASE_BIT) == 'g') stem = length - LOG_EXTENSION_LENGTH;
    LogRotationName(to, path, length, stem, LOG_KEEP);
    DeleteFileA(to);                                        /* the oldest falls off */
    for (number = LOG_KEEP - 1; number >= 1; --number) {
        LogRotationName(from, path, length, stem, number);
        LogRotationName(to,   path, length, stem, number + 1);
        MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING);
    }
    LogRotationName(to, path, length, stem, 1);
    MoveFileExA(path, to, MOVEFILE_REPLACE_EXISTING);       /* last run -> -1 */
}

static inline VOID LogWrite(PCSTR path, PCSTR buffer, PCSTR end) {
    HANDLE file;
    if (LogIsBadRange(path, buffer, end)) return;
    LogRotateOnce(path);
    LogClose();                       /* the cached handle names the OLD file */
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    g_LogTotal = (unsigned long)(end - buffer); g_LogIsCapped = 0;
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written; WriteFile(file, buffer, (DWORD)(end - buffer), &written, NULL); CloseHandle(file);
    }
}

/* Append [buf..end) to `path` -- used inside the service loop so a host crash still
   leaves the trace-so-far on disk and the in-memory buffer can be reset each pass.
   Bounded by LOG_MAX_BYTES so a runaway guest can never flood the disk. */
/* ── ★★ THE A/B SWITCH FOR "IS THE INSTRUMENT THE PROBLEM?" ──────────────────
     Set from `wowquiet.txt` on the share (see main.c). It silences the trace
     ENTIRELY, which is the point: this project cannot measure feel from the dev
     machine -- the headless rig cannot see input lag -- so the only honest
     instrument for "does it feel slow" is a one-file A/B in the user's hands.
   ⚠ IT IS A MEASUREMENT MODE, NOT A PRODUCT MODE. Every session's debugging
     rests on the trace, so this is opt-in and off by default. If it turns out to
     be the whole difference, the ANSWER is not to ship it on -- it is to stop
     writing a kilobyte per BOP in the first place. */
static INT g_LogIsQuiet = 0;

/* ── ★ THE COST OF THE INSTRUMENT, MEASURED BY THE INSTRUMENT. ───────────────
     Two sessions have now blamed the trace for the guests feeling slow, and both
     times it was a guess. These are the numbers that settle it: ticks spent
     inside LogAppend, and how many calls and bytes that was. QPC, not
     GetTickCount -- a 15 ms clock cannot see a call that costs microseconds, and
     summing 15 ms quanta over 100k calls is how a measurement invents a
     bottleneck. */
static LONGLONG g_LogQpc = 0;
static DWORD    g_LogCalls = 0, g_LogBytes = 0;

static inline VOID LogAppend(PCSTR path, PCSTR buffer, PCSTR end) {
    DWORD length = (DWORD)(end - buffer);
    HANDLE file;
    LARGE_INTEGER start, stop;
    if (g_LogIsQuiet) return;
    if (LogIsBadRange(path, buffer, end)) return;
    QueryPerformanceCounter(&start);
    ++g_LogCalls; g_LogBytes += length;
    if (g_LogIsCapped) return;
    if (g_LogTotal + length > LOG_MAX_BYTES) {
        static const CHAR mark[] = "\r\n[log capped at LOG_MAX_BYTES: runaway guest output suppressed]\r\n";
        file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written; WriteFile(file, mark, (DWORD)(sizeof(mark) - 1), &written, NULL); CloseHandle(file);
        }
        g_LogIsCapped = 1;
        return;
    }
    g_LogTotal += length;
    if (g_LogHandle == INVALID_HANDLE_VALUE || !LogIsSamePath(g_LogHandlePath, path)) {
        LogClose();
        /* ⚠ FILE_SHARE_DELETE IS LOAD-BEARING NOW THAT THE HANDLE IS HELD OPEN.
             The harness deletes this log between runs (`del C:\ntvdmex\
             ntvdmhost.log` in wowlive.bat) to guarantee a fresh trace. With an
             open handle and no delete-sharing, that `del` FAILS -- silently, in
             a batch file -- and the next run reads as an enormous log full of
             the previous guest's output. That is the `stale artefact worse than
             missing` trap, arriving by a new route. */
        file = CreateFileA(path, FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) return;
        {   UINT index = 0;
            while (path[index] && index < sizeof g_LogHandlePathBuffer - 1) { g_LogHandlePathBuffer[index] = path[index]; ++index; }
            g_LogHandlePathBuffer[index] = 0; }
        g_LogHandle = file; g_LogHandlePath = g_LogHandlePathBuffer;
    }
    {   DWORD written; WriteFile(g_LogHandle, buffer, length, &written, NULL); }
    QueryPerformanceCounter(&stop);
    g_LogQpc += stop.QuadPart - start.QuadPart;
}

#endif /* HOST_LOG_H */
