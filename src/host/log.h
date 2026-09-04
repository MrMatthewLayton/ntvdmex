/* log.h -- tiny no-CRT logging helpers for the host.
 *
 * zput/zhex/zdump build text into a caller-owned buffer; log_write/log_append flush
 * it to a file. Header-only static-inline (no shared state); the writers take the
 * log path explicitly rather than hard-coding it (the spike hard-coded
 * C:\ntvdmex\vdmhost.log). Ported from tools/vdmhost/vdmhost.c. No CRT -- only
 * kernel32 file APIs, so it loads on XP.
 */
#ifndef HOST_LOG_H
#define HOST_LOG_H

#include <windows.h>

/* Append the ASCIIZ string s to p; return the new end (NUL-terminated). */
static inline char *zput(char *p, const char *s) {
    while (*s) *p++ = *s++;
    *p = 0;
    return p;
}

/* Append v as 8 lowercase hex digits. */
static inline char *zhex(char *p, unsigned v) {
    int i; char t[9]; t[8] = 0;
    for (i = 7; i >= 0; --i) { t[i] = "0123456789abcdef"[v & 0xf]; v >>= 4; }
    return zput(p, t);
}

/* Append v as 2 lowercase hex digits. For byte-sized things -- interrupt numbers,
   AH values, mode numbers -- where zhex's 8 digits turn a list into a wall. */
static inline char *zhexb(char *p, unsigned v) {
    char t[3]; t[0] = "0123456789abcdef"[(v >> 4) & 0xf];
    t[1] = "0123456789abcdef"[v & 0xf]; t[2] = 0;
    return zput(p, t);
}

/* Append a raw hex dump of n bytes at b (space-separated, newline every 16). */
static inline char *zdump(char *p, const void *b, unsigned n) {
    const unsigned char *q = (const unsigned char *)b; unsigned i;
    for (i = 0; i < n; ++i) {
        *p++ = "0123456789abcdef"[q[i] >> 4];
        *p++ = "0123456789abcdef"[q[i] & 0xf];
        *p++ = ((i & 0xf) == 0xf) ? '\n' : ' ';
    }
    *p = 0;
    return p;
}

/* Runaway-log guard. An infinite guest loop that logs each serviced INT can grow
   the log without bound -- a headless pm32irq (an infinite mode-13h animation demo)
   flooded 148 MB, thrashed the disk, and wedged the SMB result-copy. Cap the total
   appended bytes; past the cap log_append silently drops (writing one truncation
   marker). log_write (the STAGE0 truncate that starts a fresh run) resets the count. */
#define LOG_MAX_BYTES (256u * 1024u * 1024u)  /* 4 MB -> 32 MB -> 256 MB. A client that RUNS
                                                 produces a long trace, and truncating it hides
                                                 exactly the part that matters. Measured: with
                                                 pmverbose.flag, Doom past the D/B fix fills 32 MB
                                                 BEFORE it dies, so the cap looked like the
                                                 stopping point -- an instrument lying by
                                                 omission. On a silent VDM teardown nothing runs
                                                 afterwards, so anything not already flushed is
                                                 gone: the ceiling has to clear the whole run. */
static unsigned long g_log_total  = 0;
static int           g_log_capped = 0;

/* ── ★★★★ ONE HANDLE, KEPT OPEN -- AND THIS IS A PERFORMANCE FIX, NOT TIDYING.
     log_append used to CreateFile + WriteFile + CloseHandle on EVERY line. An
     open/close pair is two kernel transitions plus filesystem metadata work, and
     it is the dominant cost of a log line by a wide margin -- the payload is
     usually under a hundred bytes.
   ★ THIS PROJECT HAS ALREADY PAID FOR THIS ONCE. See host_irq_sink's note in
     main.c: per-line log_append under the device lock cost SKYROADS 24% OF ITS
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
   ⚠ The handle is keyed by PATH: log_write truncates and starts a new run, so it
     must drop the cached handle rather than keep appending to the old file. */
static HANDLE g_log_h = INVALID_HANDLE_VALUE;
static const char *g_log_h_path = 0;

static inline int log_path_eq(const char *a, const char *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static inline void log_close(void) {
    if (g_log_h != INVALID_HANDLE_VALUE) CloseHandle(g_log_h);
    g_log_h = INVALID_HANDLE_VALUE;
    g_log_h_path = 0;
}

/* Overwrite `path` with [buf..end). Resets the runaway guard -- a new run starts here. */
static inline void log_write(const char *path, const char *buf, const char *end) {
    HANDLE h;
    log_close();                       /* the cached handle names the OLD file */
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    g_log_total = (unsigned long)(end - buf); g_log_capped = 0;
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr; WriteFile(h, buf, (DWORD)(end - buf), &wr, NULL); CloseHandle(h);
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
static int g_log_quiet = 0;

static inline void log_append(const char *path, const char *buf, const char *end) {
    DWORD n = (DWORD)(end - buf);
    HANDLE h;
    if (g_log_quiet) return;
    if (g_log_capped) return;
    if (g_log_total + n > LOG_MAX_BYTES) {
        static const char mark[] = "\r\n[log capped at LOG_MAX_BYTES: runaway guest output suppressed]\r\n";
        h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wr; WriteFile(h, mark, (DWORD)(sizeof(mark) - 1), &wr, NULL); CloseHandle(h);
        }
        g_log_capped = 1;
        return;
    }
    g_log_total += n;
    if (g_log_h == INVALID_HANDLE_VALUE || !log_path_eq(g_log_h_path, path)) {
        log_close();
        /* ⚠ FILE_SHARE_DELETE IS LOAD-BEARING NOW THAT THE HANDLE IS HELD OPEN.
             The harness deletes this log between runs (`del C:\ntvdmex\
             ntvdmhost.log` in wowlive.bat) to guarantee a fresh trace. With an
             open handle and no delete-sharing, that `del` FAILS -- silently, in
             a batch file -- and the next run reads as an enormous log full of
             the previous guest's output. That is the `stale artefact worse than
             missing` trap, arriving by a new route. */
        h = CreateFileA(path, FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return;
        g_log_h = h; g_log_h_path = path;
    }
    {   DWORD wr; WriteFile(g_log_h, buf, n, &wr, NULL); }
}

#endif /* HOST_LOG_H */
