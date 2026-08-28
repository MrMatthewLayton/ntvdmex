/*
 * vdmdump.c - read a running VDM's memory from outside it.  GH #128.
 *
 * WHY THIS EXISTS
 * ---------------
 * Our WOW loader gives krnl386's segment 1 a real GlobalAlloc but segments 2-4 a
 * size-0 placeholder that nothing ever fills, so LoadSegment(2) fails. The
 * loader contract we are missing is implemented, correctly, exactly once on this
 * machine: by stock ntvdm.exe, against the *same* krnl386.exe. So ask it.
 *
 * This is a plain Win32 process. It attaches to a *running* VDM (stock
 * `ntvdm.exe`, or our own `ntvdmhost.exe` -- same shape, which is the point: the
 * two dumps are directly diffable) and records:
 *
 *   1. the region map of its whole user address space (VirtualQueryEx),
 *   2. the guest's low megabyte + HMA, raw, from process linear 0 (a VDM maps the
 *      V86 address space at linear 0 -- see src/host/main.c's
 *      `g_in.bda = (uint8_t *)0x400`, which is only legal because of that),
 *   3. every hit for a set of byte needles (segment images cut from the NE file by
 *      tools/ne/needles.py) anywhere in the target's committed memory, since a
 *      Win16 segment need NOT live under 1MB,
 *   4. 64KB of context around each hit, plus any --grab ranges asked for,
 *   5. the target's LDT, if the kernel will hand it over.
 *
 * It only ever READS the target (PROCESS_VM_READ | PROCESS_QUERY_INFORMATION).
 *
 * No CRT (XP has no UCRT -- see scripts/check-imports.sh), console subsystem so a
 * .bat can sequence it, imports kernel32 + ntdll only.
 *
 *   vdmdump <outprefix> [--proc NAME] [--pid N] [--needles FILE]
 *           [--grab HEXADDR:HEXLEN] [--nolow]
 *
 * Writes <outprefix>.txt (report), <outprefix>.bin (low 1MB) and
 * <outprefix>.blk (grabbed blocks; see BLOCK FORMAT below).
 */
#include <windows.h>
#include <tlhelp32.h>

#define LOW_LEN      0x110000u          /* 1MB + HMA: the whole V86 range */
#define MAX_NEEDLES  24
#define NEEDLE_MAX   64
#define MAX_HITS     8                  /* recorded hits per needle */
#define MAX_BLOCKS   24
#define BLOCK_LEN    0x10000u
#define MAX_GRABS    8
#define SCAN_CAP     0x30000000u        /* give up after scanning 768MB */
#define REPORT_MAX   (1u << 18)

/* ---- freestanding runtime (local on purpose; this tool links -nostdlib) ------ */

void *memset(void *dst, int c, size_t n)
{
    unsigned char *p = (unsigned char *)dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

static int beq(const unsigned char *a, const unsigned char *b, unsigned n)
{
    while (n--) if (*a++ != *b++) return 0;
    return 1;
}

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

static char *zput(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }

static char *zhex(char *p, unsigned v, int digits)
{
    static const char H[] = "0123456789abcdef";
    int i;
    for (i = digits - 1; i >= 0; i--) *p++ = H[(v >> (i * 4)) & 0xf];
    *p = 0;
    return p;
}

static char *zdec(char *p, unsigned v)
{
    char t[12];
    int n = 0;
    if (!v) { *p++ = '0'; *p = 0; return p; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n--) *p++ = t[n];
    *p = 0;
    return p;
}

static int ieq(const char *a, const char *b)
{
    for (;;) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static unsigned decnum(const char *s)
{
    unsigned v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (unsigned)(*s - '0'); s++; }
    return v;
}

static unsigned hexnum(const char *s, const char **end)
{
    unsigned v = 0;
    int d;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while ((d = hexval(*s)) >= 0) { v = (v << 4) | (unsigned)d; s++; }
    if (end) *end = s;
    return v;
}

/* ---- output ------------------------------------------------------------------ */

static char  g_rep[REPORT_MAX];
static char *g_p = g_rep;

static void say(const char *s)
{
    DWORD  w;
    HANDLE h;
    unsigned n = slen(s);
    if (g_p + n + 2 < g_rep + sizeof(g_rep)) g_p = zput(g_p, s);
    h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, s, n, &w, NULL);
}

/* say() a line assembled in `buf`, appending CRLF. */
static void sayline(char *buf)
{
    char *e = buf + slen(buf);
    *e++ = '\r'; *e++ = '\n'; *e = 0;
    say(buf);
}

static int write_file(const char *path, const void *data, DWORD len)
{
    DWORD  w = 0;
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    WriteFile(h, data, len, &w, NULL);
    CloseHandle(h);
    return w == len;
}

/* ---- needles ------------------------------------------------------------------ */

typedef struct {
    char          name[40];
    unsigned char bytes[NEEDLE_MAX];
    unsigned      len;
    unsigned      nhit;
    unsigned      hit[MAX_HITS];
} needle_t;

static needle_t g_nd[MAX_NEEDLES];
static unsigned g_nneedle;

/* needles.txt: one per line, "<name> <hex>". '#' comments; blank lines ignored. */
static int load_needles(const char *path)
{
    static char buf[1 << 16];
    DWORD    got = 0;
    unsigned i = 0;
    HANDLE   h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;

    while (i < got && g_nneedle < MAX_NEEDLES) {
        needle_t *n;
        unsigned  s, e;

        while (i < got && (buf[i] == '\r' || buf[i] == '\n' ||
                           buf[i] == ' '  || buf[i] == '\t')) i++;
        if (i >= got) break;
        if (buf[i] == '#') { while (i < got && buf[i] != '\n') i++; continue; }

        n = &g_nd[g_nneedle];
        s = i;
        while (i < got && buf[i] != ' ' && buf[i] != '\t' &&
               buf[i] != '\r' && buf[i] != '\n') i++;
        e = i;
        if (e - s > sizeof(n->name) - 1) e = s + sizeof(n->name) - 1;
        memcpy(n->name, buf + s, e - s);
        n->name[e - s] = 0;

        while (i < got && (buf[i] == ' ' || buf[i] == '\t')) i++;
        n->len = 0;
        while (i + 1 < got && n->len < NEEDLE_MAX) {
            int hi = hexval(buf[i]), lo = hexval(buf[i + 1]);
            if (hi < 0 || lo < 0) break;
            n->bytes[n->len++] = (unsigned char)((hi << 4) | lo);
            i += 2;
        }
        while (i < got && buf[i] != '\n') i++;
        /* A short needle matches everywhere and tells us nothing. */
        if (n->len >= 8) g_nneedle++;
    }
    return 1;
}

/* ---- blocks ------------------------------------------------------------------- */
/*
 * BLOCK FORMAT (<outprefix>.blk): a flat sequence of
 *     "BLK1" | u32 address | u32 length | length bytes
 * so the offline reader needs no index and a truncated file still parses up to
 * the truncation. tools/ne/dumpscan.py is the reader.
 */
static unsigned char *g_blk;
static unsigned       g_blklen;
static unsigned       g_blkcap;
static unsigned       g_nblock;
static unsigned       g_blkaddr[MAX_BLOCKS];

static int block_have(unsigned addr)
{
    unsigned i;
    for (i = 0; i < g_nblock; i++) if (g_blkaddr[i] == addr) return 1;
    return 0;
}

static unsigned grab_block(HANDLE proc, unsigned addr, unsigned len)
{
    SIZE_T got = 0;
    unsigned char *hdr;

    if (g_nblock >= MAX_BLOCKS) return 0;
    if (block_have(addr)) return 0;
    if (g_blklen + 12 + len > g_blkcap) return 0;

    hdr = g_blk + g_blklen;
    if (!ReadProcessMemory(proc, (LPCVOID)addr, hdr + 12, len, &got) && !got)
        return 0;
    memcpy(hdr, "BLK1", 4);
    *(unsigned *)(hdr + 4) = addr;
    *(unsigned *)(hdr + 8) = (unsigned)got;
    g_blklen += 12 + (unsigned)got;
    g_blkaddr[g_nblock++] = addr;
    return (unsigned)got;
}

/* ---- the needle scan ---------------------------------------------------------- */

static unsigned char g_chunk[0x10000 + NEEDLE_MAX];

static void scan_region(HANDLE proc, unsigned base, unsigned size)
{
    unsigned off  = 0;
    unsigned tail = NEEDLE_MAX - 1;     /* overlap, so a needle may straddle chunks */

    while (off < size) {
        unsigned want = size - off;
        SIZE_T   got  = 0;
        unsigned i, k;

        if (want > sizeof(g_chunk)) want = sizeof(g_chunk);
        if (!ReadProcessMemory(proc, (LPCVOID)(base + off), g_chunk, want, &got) && !got)
            return;

        for (k = 0; k < g_nneedle; k++) {
            needle_t *n = &g_nd[k];
            if (n->nhit >= MAX_HITS || got < n->len) continue;
            for (i = 0; i + n->len <= got; i++) {
                if (g_chunk[i] == n->bytes[0] && beq(g_chunk + i, n->bytes, n->len)) {
                    n->hit[n->nhit++] = base + off + i;
                    if (n->nhit >= MAX_HITS) break;
                }
            }
        }
        if (got <= tail) return;
        off += (unsigned)got - tail;
    }
}

/* ---- LDT (best effort) --------------------------------------------------------- */

typedef LONG (WINAPI *PFN_NTQIP)(HANDLE, ULONG, PVOID, ULONG, PULONG);

#define ProcessLdtInformation 10

typedef struct {
    ULONG     Start;                    /* byte offset of the first entry wanted */
    ULONG     Length;                   /* bytes of entries wanted */
    LDT_ENTRY LdtEntries[512];
} PROCESS_LDT_INFO;

static PROCESS_LDT_INFO g_ldt;

static void dump_ldt(HANDLE proc)
{
    char      line[256], *p;
    HMODULE   nt = GetModuleHandleA("ntdll.dll");
    PFN_NTQIP q  = nt ? (PFN_NTQIP)(void *)GetProcAddress(nt, "NtQueryInformationProcess")
                      : NULL;
    ULONG     ret = 0;
    LONG      st;
    unsigned  i, n;

    say("\r\n-- LDT --\r\n");
    if (!q) { say("  NtQueryInformationProcess unavailable\r\n"); return; }

    g_ldt.Start  = 0;
    g_ldt.Length = sizeof(g_ldt.LdtEntries);
    st = q(proc, ProcessLdtInformation, &g_ldt, sizeof(g_ldt), &ret);
    if (st < 0) {
        p = zput(line, "  query failed, status 0x");
        p = zhex(p, (unsigned)st, 8);
        p = zput(p, " (not fatal -- the memory dump is the datum)");
        sayline(line);
        return;
    }
    n = (ret > 8 ? (ret - 8) : 0) / sizeof(LDT_ENTRY);
    if (n > 512) n = 512;
    p = zput(line, "  returned "); p = zdec(p, ret);
    p = zput(p, " bytes = "); p = zdec(p, n); p = zput(p, " entries");
    sayline(line);

    for (i = 0; i < n; i++) {
        LDT_ENTRY *e = &g_ldt.LdtEntries[i];
        unsigned base, limit;
        if (!e->HighWord.Bits.Pres && !e->BaseLow && !e->LimitLow) continue;
        base  = (unsigned)e->BaseLow |
                ((unsigned)e->HighWord.Bytes.BaseMid << 16) |
                ((unsigned)e->HighWord.Bytes.BaseHi  << 24);
        limit = (unsigned)e->LimitLow |
                ((unsigned)e->HighWord.Bits.LimitHi << 16);
        if (e->HighWord.Bits.Granularity) limit = (limit << 12) | 0xfff;
        p = zput(line, "  sel 0x");
        p = zhex(p, (i * 8) | 7, 4);
        p = zput(p, "  base 0x"); p = zhex(p, base, 8);
        p = zput(p, "  limit 0x"); p = zhex(p, limit, 8);
        p = zput(p, "  type 0x"); p = zhex(p, e->HighWord.Bits.Type, 2);
        p = zput(p, e->HighWord.Bits.Default_Big ? " big" : " 16b");
        p = zput(p, e->HighWord.Bits.Pres ? " P" : " -");
        sayline(line);
    }
}

/* ---- main ---------------------------------------------------------------------- */

static char g_out[MAX_PATH];
static char g_reportpath[MAX_PATH];

/* The report is written on EVERY exit path, including the failures. A run that
   produced no file is indistinguishable from a run that never happened, and this
   tool runs inside a .bat that also has to put the IFEO key back. */
static void write_file_report(void)
{
    char path[MAX_PATH];
    char *e;
    if (!g_out[0]) return;
    e = zput(path, g_out);
    zput(e, ".txt");
    write_file(path, g_rep, (DWORD)(g_p - g_rep));
    zput(g_reportpath, path);
}

static char g_proc[64];
static char g_needles[MAX_PATH];

static unsigned char g_low[LOW_LEN];

/* Pull the next whitespace-separated (optionally "quoted") token off the line. */
static const char *token(const char *s, char *dst, unsigned cap)
{
    unsigned n = 0;
    char q = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '"') { q = '"'; s++; }
    while (*s && (q ? *s != q : (*s != ' ' && *s != '\t'))) {
        if (n + 1 < cap) dst[n++] = *s;
        s++;
    }
    if (q && *s == q) s++;
    dst[n] = 0;
    return s;
}

static DWORD find_process(const char *name, char *report_into)
{
    PROCESSENTRY32 pe;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD  pid = 0;
    unsigned count = 0;
    char  line[256], *p;

    if (snap == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (ieq(pe.szExeFile, name)) {
                count++;
                if (!pid) pid = pe.th32ProcessID;
                p = zput(line, "  candidate pid ");
                p = zdec(p, pe.th32ProcessID);
                p = zput(p, "  ");
                p = zput(p, pe.szExeFile);
                sayline(line);
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    (void)report_into;
    if (count > 1) say("  ! more than one match -- taking the first; pass --pid to choose\r\n");
    return pid;
}

int main_impl(const char *cmdline)
{
    char        line[256], *p;
    const char *s = cmdline;
    char        tok[MAX_PATH];
    HANDLE      proc;
    DWORD       pid = 0;
    unsigned    grab_a[MAX_GRABS], grab_l[MAX_GRABS], ngrab = 0;
    int         nolow = 0;
    unsigned    scanned = 0, i, k;
    SIZE_T      lowgot = 0;
    char        path[MAX_PATH];
    MEMORY_BASIC_INFORMATION mbi;
    unsigned    addr;

    zput(g_proc, "ntvdm.exe");
    g_out[0] = 0;
    g_needles[0] = 0;

    s = token(s, tok, sizeof(tok));                 /* argv[0] */
    for (;;) {
        s = token(s, tok, sizeof(tok));
        if (!tok[0]) break;
        if (ieq(tok, "--proc"))         { s = token(s, g_proc, sizeof(g_proc)); }
        else if (ieq(tok, "--needles")) { s = token(s, g_needles, sizeof(g_needles)); }
        else if (ieq(tok, "--pid"))     { s = token(s, tok, sizeof(tok)); pid = decnum(tok); }
        else if (ieq(tok, "--nolow"))   { nolow = 1; }
        else if (ieq(tok, "--grab")) {
            const char *e;
            s = token(s, tok, sizeof(tok));
            if (ngrab < MAX_GRABS) {
                grab_a[ngrab] = hexnum(tok, &e);
                grab_l[ngrab] = (*e == ':') ? hexnum(e + 1, NULL) : BLOCK_LEN;
                if (grab_l[ngrab] > BLOCK_LEN) grab_l[ngrab] = BLOCK_LEN;
                ngrab++;
            }
        }
        else if (!g_out[0]) { zput(g_out, tok); }
    }

    if (!g_out[0]) {
        say("usage: vdmdump <outprefix> [--proc NAME] [--pid N] [--needles FILE]"
            " [--grab HEXADDR:HEXLEN] [--nolow]\r\n");
        return 2;
    }

    g_blkcap = MAX_BLOCKS * (BLOCK_LEN + 12);
    g_blk = (unsigned char *)VirtualAlloc(NULL, g_blkcap, MEM_COMMIT | MEM_RESERVE,
                                          PAGE_READWRITE);
    if (!g_blk) g_blkcap = 0;

    say("vdmdump -- reading a live VDM from outside\r\n");
    p = zput(line, "  target image: "); p = zput(p, g_proc);
    sayline(line);

    if (g_needles[0]) {
        if (!load_needles(g_needles)) {
            p = zput(line, "  ! cannot read needles file "); p = zput(p, g_needles);
            sayline(line);
        } else {
            p = zput(line, "  needles loaded: "); p = zdec(p, g_nneedle);
            sayline(line);
        }
    }

    if (!pid) pid = find_process(g_proc, NULL);
    if (!pid) {
        say("  FAIL: no such process is running\r\n");
        write_file_report();
        return 3;
    }
    p = zput(line, "  using pid "); p = zdec(p, pid);
    sayline(line);

    proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) {
        p = zput(line, "  FAIL: OpenProcess, GetLastError "); p = zdec(p, GetLastError());
        sayline(line);
        write_file_report();
        return 4;
    }

    /* 1. the region map -------------------------------------------------------- */
    say("\r\n-- region map (committed/reserved only) --\r\n");
    addr = 0;
    while (addr < 0x7ff00000u) {
        if (!VirtualQueryEx(proc, (LPCVOID)addr, &mbi, sizeof(mbi))) break;
        if (mbi.State != MEM_FREE) {
            p = zput(line, "  0x"); p = zhex(p, (unsigned)(ULONG_PTR)mbi.BaseAddress, 8);
            p = zput(p, "  size 0x"); p = zhex(p, (unsigned)mbi.RegionSize, 8);
            p = zput(p, mbi.State == MEM_COMMIT ? "  commit" : "  reserve");
            p = zput(p, mbi.Type == MEM_IMAGE   ? "  image"   :
                        mbi.Type == MEM_MAPPED  ? "  mapped"  : "  private");
            p = zput(p, "  prot 0x"); p = zhex(p, (unsigned)mbi.Protect, 3);
            sayline(line);
        }
        if (!mbi.RegionSize) break;
        addr = (unsigned)(ULONG_PTR)mbi.BaseAddress + (unsigned)mbi.RegionSize;
    }

    /* 2. the guest's low megabyte ---------------------------------------------- */
    if (!nolow) {
        unsigned pages = 0, off;
        memset(g_low, 0, sizeof(g_low));
        for (off = 0; off < LOW_LEN; off += 0x1000) {
            SIZE_T got = 0;
            if (ReadProcessMemory(proc, (LPCVOID)off, g_low + off, 0x1000, &got) && got)
                pages++;
        }
        lowgot = pages;
        zput(path, g_out); zput(path + slen(path), ".bin");
        write_file(path, g_low, LOW_LEN);
        p = zput(line, "\r\n-- low memory --\r\n  0x00000000..0x00110000 -> ");
        p = zput(p, path);
        p = zput(p, "   readable pages "); p = zdec(p, pages);
        p = zput(p, " of 272");
        sayline(line);
    }

    /* 3. the needle scan -------------------------------------------------------- */
    if (g_nneedle) {
        say("\r\n-- needle scan --\r\n");
        addr = 0x1000;
        while (addr < 0x7ff00000u && scanned < SCAN_CAP) {
            if (!VirtualQueryEx(proc, (LPCVOID)addr, &mbi, sizeof(mbi))) break;
            if (mbi.State == MEM_COMMIT &&
                !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                scan_region(proc, (unsigned)(ULONG_PTR)mbi.BaseAddress,
                            (unsigned)mbi.RegionSize);
                scanned += (unsigned)mbi.RegionSize;
            }
            if (!mbi.RegionSize) break;
            addr = (unsigned)(ULONG_PTR)mbi.BaseAddress + (unsigned)mbi.RegionSize;
        }
        p = zput(line, "  scanned 0x"); p = zhex(p, scanned, 8); p = zput(p, " bytes");
        sayline(line);

        for (k = 0; k < g_nneedle; k++) {
            needle_t *n = &g_nd[k];
            p = zput(line, "  "); p = zput(p, n->name);
            p = zput(p, " ("); p = zdec(p, n->len); p = zput(p, "B): ");
            if (!n->nhit) p = zput(p, "no hit");
            for (i = 0; i < n->nhit; i++) {
                p = zput(p, "0x"); p = zhex(p, n->hit[i], 8); p = zput(p, " ");
            }
            sayline(line);
            /* 4. context: the 64KB block each hit lives in. */
            for (i = 0; i < n->nhit; i++)
                grab_block(proc, n->hit[i] & ~(BLOCK_LEN - 1), BLOCK_LEN);
        }
    }

    /* 4b. explicit grabs -------------------------------------------------------- */
    for (i = 0; i < ngrab; i++) {
        unsigned got = grab_block(proc, grab_a[i], grab_l[i]);
        p = zput(line, "  grab 0x"); p = zhex(p, grab_a[i], 8);
        p = zput(p, " -> "); p = zdec(p, got); p = zput(p, " bytes");
        sayline(line);
    }

    if (g_blklen) {
        zput(path, g_out); zput(path + slen(path), ".blk");
        write_file(path, g_blk, g_blklen);
        p = zput(line, "\r\n-- blocks --\r\n  "); p = zdec(p, g_nblock);
        p = zput(p, " blocks, "); p = zdec(p, g_blklen); p = zput(p, " bytes -> ");
        p = zput(p, path);
        sayline(line);
    }

    /* 5. the LDT ---------------------------------------------------------------- */
    dump_ldt(proc);

    CloseHandle(proc);
    (void)lowgot;
    write_file_report();
    return 0;
}

/* -nostdlib: our own entry. Console subsystem, so a .bat can sequence us -- a GUI
   subsystem exe is not waited for by cmd, and this one runs between "IFEO key
   removed" and "IFEO key restored". */
void mainCRTStartup(void)
{
    ExitProcess((UINT)main_impl(GetCommandLineA()));
}
