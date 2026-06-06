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

/* Overwrite `path` with [buf..end). */
static inline void log_write(const char *path, const char *buf, const char *end) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr; WriteFile(h, buf, (DWORD)(end - buf), &wr, NULL); CloseHandle(h);
    }
}

/* Append [buf..end) to `path` -- used inside the service loop so a host crash still
   leaves the trace-so-far on disk and the in-memory buffer can be reset each pass. */
static inline void log_append(const char *path, const char *buf, const char *end) {
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr; WriteFile(h, buf, (DWORD)(end - buf), &wr, NULL); CloseHandle(h);
    }
}

#endif /* HOST_LOG_H */
