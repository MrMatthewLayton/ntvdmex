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

/* Overwrite `path` with [buf..end). Resets the runaway guard -- a new run starts here. */
static inline void log_write(const char *path, const char *buf, const char *end) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    g_log_total = (unsigned long)(end - buf); g_log_capped = 0;
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr; WriteFile(h, buf, (DWORD)(end - buf), &wr, NULL); CloseHandle(h);
    }
}

/* Append [buf..end) to `path` -- used inside the service loop so a host crash still
   leaves the trace-so-far on disk and the in-memory buffer can be reset each pass.
   Bounded by LOG_MAX_BYTES so a runaway guest can never flood the disk. */
static inline void log_append(const char *path, const char *buf, const char *end) {
    DWORD n = (DWORD)(end - buf);
    HANDLE h;
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
    h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr; WriteFile(h, buf, n, &wr, NULL); CloseHandle(h);
    }
}

#endif /* HOST_LOG_H */
