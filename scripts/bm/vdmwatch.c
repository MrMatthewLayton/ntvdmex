/*
 * vdmwatch.c -- see WHAT THE KERNEL SAW when it killed the VDM.
 *
 * WHY THIS EXISTS. Doom (E1M1, kill the distant imp), Mario (s62) and one Lemmings
 * run (s69) all died the same way: no HOSTFAULT from our vectored handler, no
 * Dr Watson entry, no WER event, not one line of the host's own shutdown path. The
 * process simply stopped existing. Mario's exit code was measured once, headless,
 * as STATUS_BREAKPOINT -- and everything since has been theory, because nothing we
 * own runs after the kernel decides to terminate a process whose exception it
 * cannot deliver.
 *
 * The one thing that DOES run is a debugger. KiDispatchException hands every
 * exception that reaches it to the debug port FIRST, before it tries (and, in this
 * family, fails) to write the exception frame onto the user stack. So a debugger
 * attached to the host sees the exception code, the faulting CS:EIP and the whole
 * register file of the thread at the instant the kernel gave up -- and, at the end,
 * the exit code, from the EXIT_PROCESS event. That is a measurement where before
 * there was a fingerprint.
 *
 * What it deliberately does NOT do: handle anything. Every exception is answered
 * DBG_EXCEPTION_NOT_HANDLED, which is exactly the path the kernel takes with no
 * debugger present (the host's own VEH/SEH still run first-chance as before). The
 * one exception is the attach breakpoint DebugActiveProcess itself injects, which
 * is answered DBG_CONTINUE. Kill-on-exit is switched off, so closing this window
 * leaves the guest running.
 *
 * Faults that never reach KiDispatchException -- V86 reflections to the IVT, PM
 * faults absorbed by the VdmTib fault table -- are invisible here BY DESIGN: those
 * are the ones the host already logs. What arrives here is what the host could not
 * see, which is what we are after.
 *
 *   vdmwatch [procname]     wait for <procname> (default ntvdmhost.exe), attach,
 *                           log every debug event until it exits, then wait for
 *                           the next one. Leave the window open and play.
 *
 * Output: the console, and SHARE\out\vdmwatch.txt (appended, timestamped).
 *
 * XP-safe build: no-CRT (reuses src/runtime.c), subsystem 5.01, console, imports
 * only KERNEL32. See scripts/build-vdmwatch.sh.
 */
#include <windows.h>
#include <tlhelp32.h>

#define SHARE "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex"
#define LOGF  SHARE "\\out\\vdmwatch.txt"

/* ── tiny no-CRT helpers (same shapes as rigshot.c) ─────────────────────────── */
static char *sput(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *sputu(char *p, unsigned v)
{
    char t[12]; int n = 0;
    if (!v) { *p++ = '0'; *p = 0; return p; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n--) *p++ = t[n];
    *p = 0; return p;
}
static char *sputx(char *p, unsigned v)            /* 8 hex digits */
{
    static const char H[] = "0123456789abcdef";
    int i;
    for (i = 28; i >= 0; i -= 4) *p++ = H[(v >> i) & 0xF];
    *p = 0; return p;
}
static char *sputx4(char *p, unsigned v)           /* 4 hex digits */
{
    static const char H[] = "0123456789abcdef";
    int i;
    for (i = 12; i >= 0; i -= 4) *p++ = H[(v >> i) & 0xF];
    *p = 0; return p;
}
static char *sputx2(char *p, unsigned v)           /* 2 hex digits */
{
    static const char H[] = "0123456789abcdef";
    *p++ = H[(v >> 4) & 0xF]; *p++ = H[v & 0xF]; *p = 0; return p;
}
static int sieq(const char *a, const char *b)      /* case-insensitive equal */
{
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static HANDLE g_con;

/* One line to the console and the share log, prefixed hh:mm:ss.mmm. */
static void logline(const char *s)
{
    static char lb[1200]; char *p = lb;
    SYSTEMTIME st; DWORD wr; HANDLE h;
    GetLocalTime(&st);
    *p++ = (char)('0' + st.wHour / 10);   *p++ = (char)('0' + st.wHour % 10);   *p++ = ':';
    *p++ = (char)('0' + st.wMinute / 10); *p++ = (char)('0' + st.wMinute % 10); *p++ = ':';
    *p++ = (char)('0' + st.wSecond / 10); *p++ = (char)('0' + st.wSecond % 10); *p++ = '.';
    *p++ = (char)('0' + st.wMilliseconds / 100);
    *p++ = (char)('0' + (st.wMilliseconds / 10) % 10);
    *p++ = (char)('0' + st.wMilliseconds % 10);
    *p++ = ' ';
    p = sput(p, s);
    *p++ = '\r'; *p++ = '\n'; *p = 0;
    if (g_con && g_con != INVALID_HANDLE_VALUE) WriteFile(g_con, lb, (DWORD)(p - lb), &wr, NULL);
    h = CreateFileA(LOGF, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, lb, (DWORD)(p - lb), &wr, NULL);
    CloseHandle(h);
}

/* ── process lookup ─────────────────────────────────────────────────────────── */
static DWORD find_pid(const char *name, DWORD not_this)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe; DWORD pid = 0;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof pe;
    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID != not_this && sieq(pe.szExeFile, name)) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

/* ── thread handle table (the debug API hands us handles once, at creation) ─── */
#define MAXT 64
static struct { DWORD tid; HANDLE h; } g_thr[MAXT];
static void thr_add(DWORD tid, HANDLE h)
{
    int i;
    for (i = 0; i < MAXT; ++i) if (!g_thr[i].tid) { g_thr[i].tid = tid; g_thr[i].h = h; return; }
}
static HANDLE thr_get(DWORD tid)
{
    int i;
    for (i = 0; i < MAXT; ++i) if (g_thr[i].tid == tid) return g_thr[i].h;
    return NULL;
}
static void thr_del(DWORD tid)
{
    int i;
    for (i = 0; i < MAXT; ++i) if (g_thr[i].tid == tid) { g_thr[i].tid = 0; g_thr[i].h = NULL; return; }
}
static void thr_clear(void)
{
    int i;
    for (i = 0; i < MAXT; ++i) { g_thr[i].tid = 0; g_thr[i].h = NULL; }
}

/* ── selector -> linear base, so CS:EIP / SS:ESP can be read as memory ───────
   V86 (EFLAGS.VM): segment<<4. Otherwise the descriptor, which for an LDT selector
   is the DPMI client's own -- GetThreadSelectorEntry exists for exactly this. */
static int sel_base(HANDLE th, DWORD efl, WORD sel, DWORD *base, DWORD *limit, char *desc)
{
    LDT_ENTRY e;
    if (efl & 0x20000) {
        *base = (DWORD)sel << 4; *limit = 0xFFFF;
        sput(desc, "v86");
        return 1;
    }
    if (!GetThreadSelectorEntry(th, sel, &e)) { *base = 0; *limit = 0; sput(desc, "<unresolved>"); return 0; }
    *base  = e.BaseLow | ((DWORD)e.HighWord.Bytes.BaseMid << 16) | ((DWORD)e.HighWord.Bytes.BaseHi << 24);
    *limit = e.LimitLow | ((DWORD)e.HighWord.Bits.LimitHi << 16);
    if (e.HighWord.Bits.Granularity) *limit = (*limit << 12) | 0xFFF;
    {
        char *d = desc;
        d = sput(d, "base=");  d = sputx(d, *base);
        d = sput(d, " limit="); d = sputx(d, *limit);
        d = sput(d, e.HighWord.Bits.Default_Big ? " 32" : " 16");
        d = sput(d, e.HighWord.Bits.Pres ? " P" : " NP");
        d = sput(d, " type="); d = sputx2(d, e.HighWord.Bits.Type);
        d = sput(d, " dpl=");  *d++ = (char)('0' + e.HighWord.Bits.Dpl); *d = 0;
    }
    return 1;
}

static char *hexbytes(char *p, HANDLE hp, DWORD lin, unsigned n)
{
    BYTE b[64]; SIZE_T got = 0; unsigned i;
    if (n > sizeof b) n = sizeof b;
    if (!ReadProcessMemory(hp, (LPCVOID)(ULONG_PTR)lin, b, n, &got) || !got)
        return sput(p, "<unreadable>");
    for (i = 0; i < got; ++i) { p = sputx2(p, b[i]); *p++ = ' '; }
    *p = 0; return p;
}

static const char *exc_name(DWORD code)
{
    switch (code) {
    case 0x80000003: return "BREAKPOINT";
    case 0x80000004: return "SINGLE_STEP";
    case 0x80000001: return "GUARD_PAGE";
    case 0xC0000005: return "ACCESS_VIOLATION";
    case 0xC000001D: return "ILLEGAL_INSTRUCTION";
    case 0xC0000096: return "PRIVILEGED_INSTRUCTION";
    case 0xC00000FD: return "STACK_OVERFLOW";
    case 0xC0000094: return "INTEGER_DIVIDE_BY_ZERO";
    case 0xC0000095: return "INTEGER_OVERFLOW";
    case 0xC000008C: return "ARRAY_BOUNDS_EXCEEDED";
    case 0xC000008E: return "FLOAT_DIVIDE_BY_ZERO";
    case 0xC0000090: return "FLOAT_INVALID_OPERATION";
    case 0xC000008D: return "FLOAT_DENORMAL_OPERAND";
    case 0xC0000091: return "FLOAT_OVERFLOW";
    case 0xC0000092: return "FLOAT_STACK_CHECK";
    case 0xC0000093: return "FLOAT_UNDERFLOW";
    case 0xC000008F: return "FLOAT_INEXACT_RESULT";
    case 0xC0000006: return "IN_PAGE_ERROR";
    case 0xC000013A: return "CONTROL_C_EXIT";
    case 0xC0000194: return "POSSIBLE_DEADLOCK";
    case 0x40010005: return "DBG_CONTROL_C";
    case 0x40010008: return "DBG_CONTROL_BREAK";
    default:         return "?";
    }
}

/* ── the core: guest memory as a flat file per region, so the death can be read
   offline against the LE image. Offsets are linear (addr = base + offset); a page
   ReadProcessMemory cannot read is written as zeros and counted, never skipped, so
   the file stays addressable. Written once per process, at the FIRST exception --
   the process is frozen while the debugger holds the event, so this is the one
   moment the picture is consistent. */
static void dump_region(HANDLE hp, const char *path, DWORD base, DWORD size)
{
    static BYTE buf[0x10000];
    static char b[256]; char *p = b;
    HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD off, wr, bad = 0, t0 = GetTickCount();
    if (f == INVALID_HANDLE_VALUE) { p = sput(p, "core: cannot create "); p = sput(p, path); logline(b); return; }
    for (off = 0; off < size; off += sizeof buf) {
        DWORD n = size - off; SIZE_T got = 0;
        if (n > sizeof buf) n = sizeof buf;
        if (!ReadProcessMemory(hp, (LPCVOID)(ULONG_PTR)(base + off), buf, n, &got) || got != n) {
            /* fall back a page at a time; zero what cannot be read */
            DWORD pg;
            for (pg = 0; pg < n; pg += 0x1000) {
                SIZE_T g2 = 0;
                if (!ReadProcessMemory(hp, (LPCVOID)(ULONG_PTR)(base + off + pg), buf + pg, 0x1000, &g2) || g2 != 0x1000) {
                    unsigned i; for (i = 0; i < 0x1000; ++i) buf[pg + i] = 0;
                    ++bad;
                }
            }
        }
        WriteFile(f, buf, n, &wr, NULL);
    }
    CloseHandle(f);
    p = sput(p, "core: "); p = sput(p, path); p = sput(p, " base=0x"); p = sputx(p, base);
    p = sput(p, " size=0x"); p = sputx(p, size); p = sput(p, " unreadable pages="); p = sputu(p, bad);
    p = sput(p, " ms="); p = sputu(p, GetTickCount() - t0);
    logline(b);
}

static void dump_core(HANDLE hp)
{
    dump_region(hp, SHARE "\\out\\vdmwatch_core_low.bin",  0x00000000u, 0x00110000u);   /* IVT..HMA */
    dump_region(hp, SHARE "\\out\\vdmwatch_core_dpmi.bin", 0x03ff0000u, 0x00b10000u);   /* every DPMI block Doom took */
}

/* The stack, wide: from below ESP (what a `ret` just popped, the callee's locals) up
   to the top of the stack object, so the whole return chain is in the log. */
static void dump_stack(HANDLE hp, DWORD lin_esp)
{
    static char b[256]; char *p;
    DWORD lo = lin_esp - 0x100, hi = lin_esp + 0x600, a;
    for (a = lo; a < hi; a += 32) {
        BYTE x[32]; SIZE_T got = 0; unsigned i;
        p = b;
        p = sput(p, "  st "); p = sputx(p, a); p = sput(p, a == lin_esp ? " >" : "  ");
        if (!ReadProcessMemory(hp, (LPCVOID)(ULONG_PTR)a, x, 32, &got) || got != 32) { p = sput(p, " <unreadable>"); logline(b); continue; }
        for (i = 0; i < 32; i += 4) {
            DWORD v = x[i] | ((DWORD)x[i+1] << 8) | ((DWORD)x[i+2] << 16) | ((DWORD)x[i+3] << 24);
            *p++ = ' '; p = sputx(p, v);
        }
        logline(b);
    }
}

/* ── the exception dump: record, register file, both segments, code and stack ── */
static void dump_exception(HANDLE hp, DWORD tid, const EXCEPTION_DEBUG_INFO *ei)
{
    static char b[1024]; char *p = b;
    const EXCEPTION_RECORD *er = &ei->ExceptionRecord;
    HANDLE th = thr_get(tid);
    CONTEXT cx;
    DWORD i;

    p = sput(p, ei->dwFirstChance ? "EXCEPTION first-chance " : "EXCEPTION *** SECOND CHANCE *** ");
    p = sput(p, "code=0x"); p = sputx(p, er->ExceptionCode);
    p = sput(p, " ("); p = sput(p, exc_name(er->ExceptionCode)); p = sput(p, ")");
    p = sput(p, " addr=0x"); p = sputx(p, (DWORD)(ULONG_PTR)er->ExceptionAddress);
    p = sput(p, " flags=0x"); p = sputx(p, er->ExceptionFlags);
    p = sput(p, " tid="); p = sputu(p, tid);
    if (er->NumberParameters) {
        p = sput(p, " params:");
        for (i = 0; i < er->NumberParameters && i < 4; ++i) { p = sput(p, " 0x"); p = sputx(p, (DWORD)er->ExceptionInformation[i]); }
        if (er->ExceptionCode == 0xC0000005 && er->NumberParameters >= 2)
            p = sput(p, er->ExceptionInformation[0] ? " (write)" : " (read)");
    }
    logline(b); p = b;

    if (!th) { logline("  (no thread handle for this tid -- context unavailable)"); return; }
    cx.ContextFlags = CONTEXT_FULL | CONTEXT_SEGMENTS;
    if (!GetThreadContext(th, &cx)) {
        p = sput(p, "  GetThreadContext failed err="); p = sputu(p, GetLastError()); logline(b); return;
    }
    p = sput(p, "  cs:eip=");  p = sputx4(p, cx.SegCs); *p++ = ':'; p = sputx(p, cx.Eip);
    p = sput(p, " ss:esp=");   p = sputx4(p, cx.SegSs); *p++ = ':'; p = sputx(p, cx.Esp);
    p = sput(p, " efl=");      p = sputx(p, cx.EFlags);
    p = sput(p, (cx.EFlags & 0x20000) ? " V86" : ((cx.SegCs & 4) ? " PM/LDT" : " flat"));
    logline(b); p = b;
    p = sput(p, "  eax="); p = sputx(p, cx.Eax); p = sput(p, " ebx="); p = sputx(p, cx.Ebx);
    p = sput(p, " ecx="); p = sputx(p, cx.Ecx); p = sput(p, " edx="); p = sputx(p, cx.Edx);
    p = sput(p, " esi="); p = sputx(p, cx.Esi); p = sput(p, " edi="); p = sputx(p, cx.Edi);
    p = sput(p, " ebp="); p = sputx(p, cx.Ebp);
    logline(b); p = b;
    p = sput(p, "  ds="); p = sputx4(p, cx.SegDs); p = sput(p, " es="); p = sputx4(p, cx.SegEs);
    p = sput(p, " fs="); p = sputx4(p, cx.SegFs); p = sput(p, " gs="); p = sputx4(p, cx.SegGs);
    p = sput(p, " dr6="); p = sputx(p, cx.Dr6); p = sput(p, " dr7="); p = sputx(p, cx.Dr7);
    logline(b); p = b;

    {
        DWORD cb, cl, sb, sl; char cd[96], sd[96]; DWORD lin;
        sel_base(th, cx.EFlags, (WORD)cx.SegCs, &cb, &cl, cd);
        sel_base(th, cx.EFlags, (WORD)cx.SegSs, &sb, &sl, sd);
        p = sput(p, "  CS "); p = sput(p, cd); logline(b); p = b;
        p = sput(p, "  SS "); p = sput(p, sd); logline(b); p = b;
        lin = cb + cx.Eip;
        p = sput(p, "  code@lin 0x"); p = sputx(p, lin);
        p = sput(p, "  -8: "); p = hexbytes(p, hp, lin - 8, 8);
        p = sput(p, " | "); p = hexbytes(p, hp, lin, 16);
        logline(b); p = b;
        lin = sb + cx.Esp;
        p = sput(p, "  stack@lin 0x"); p = sputx(p, lin); p = sput(p, ": ");
        p = hexbytes(p, hp, lin, 32);
        logline(b); p = b;
        if (ei->dwFirstChance) dump_stack(hp, lin);
    }
}

/* ── one debuggee, attach to exit ───────────────────────────────────────────── */
static void watch(DWORD pid)
{
    static char b[512]; char *p = b;
    DEBUG_EVENT de;
    HANDLE hp = NULL;
    int attach_bp_pending = 1, done = 0, core_written = 0;
    unsigned nexc = 0, nbadclose = 0;

    thr_clear();
    if (!DebugActiveProcess(pid)) {
        p = sput(p, "DebugActiveProcess("); p = sputu(p, pid); p = sput(p, ") FAILED err="); p = sputu(p, GetLastError());
        logline(b); Sleep(2000); return;
    }
    DebugSetProcessKillOnExit(FALSE);           /* closing this window must not take the guest */
    p = sput(p, "attached pid="); p = sputu(p, pid); p = sput(p, " -- every exception the kernel dispatches will be logged; play.");
    logline(b); p = b;

    while (!done) {
        DWORD cont = DBG_EXCEPTION_NOT_HANDLED;
        if (!WaitForDebugEvent(&de, INFINITE)) {
            p = sput(p, "WaitForDebugEvent failed err="); p = sputu(p, GetLastError()); logline(b); p = b;
            break;
        }
        switch (de.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            hp = de.u.CreateProcessInfo.hProcess;
            thr_add(de.dwThreadId, de.u.CreateProcessInfo.hThread);
            if (de.u.CreateProcessInfo.hFile) CloseHandle(de.u.CreateProcessInfo.hFile);
            p = sput(p, "process: base=0x"); p = sputx(p, (DWORD)(ULONG_PTR)de.u.CreateProcessInfo.lpBaseOfImage);
            p = sput(p, " main tid="); p = sputu(p, de.dwThreadId);
            logline(b); p = b;
            cont = DBG_CONTINUE;
            break;
        case CREATE_THREAD_DEBUG_EVENT:
            thr_add(de.dwThreadId, de.u.CreateThread.hThread);
            p = sput(p, "thread+ tid="); p = sputu(p, de.dwThreadId);
            p = sput(p, " start=0x"); p = sputx(p, (DWORD)(ULONG_PTR)de.u.CreateThread.lpStartAddress);
            logline(b); p = b;
            cont = DBG_CONTINUE;
            break;
        case EXIT_THREAD_DEBUG_EVENT:
            p = sput(p, "thread- tid="); p = sputu(p, de.dwThreadId);
            p = sput(p, " exit=0x"); p = sputx(p, de.u.ExitThread.dwExitCode);
            logline(b); p = b;
            thr_del(de.dwThreadId);
            cont = DBG_CONTINUE;
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (de.u.LoadDll.hFile) CloseHandle(de.u.LoadDll.hFile);
            cont = DBG_CONTINUE;
            break;
        case UNLOAD_DLL_DEBUG_EVENT:
            cont = DBG_CONTINUE;
            break;
        case OUTPUT_DEBUG_STRING_EVENT: {
            char s[256]; SIZE_T got = 0; DWORD n = de.u.DebugString.nDebugStringLength;
            if (n > sizeof s - 1) n = sizeof s - 1;
            if (hp && ReadProcessMemory(hp, de.u.DebugString.lpDebugStringData, s, n, &got)) {
                s[got] = 0;
                p = sput(p, "ods: "); p = sput(p, s); logline(b); p = b;
            }
            cont = DBG_CONTINUE;
            break; }
        case RIP_EVENT:
            p = sput(p, "RIP: err="); p = sputu(p, de.u.RipInfo.dwError); p = sput(p, " type="); p = sputu(p, de.u.RipInfo.dwType);
            logline(b); p = b;
            cont = DBG_CONTINUE;
            break;
        case EXCEPTION_DEBUG_EVENT:
            ++nexc;
            /* The break-in DebugActiveProcess injects: a BREAKPOINT on a fresh thread, and
               the first exception we see. Swallow that one only. */
            if (attach_bp_pending && de.u.Exception.ExceptionRecord.ExceptionCode == 0x80000003) {
                attach_bp_pending = 0;
                p = sput(p, "attach breakpoint (tid="); p = sputu(p, de.dwThreadId); p = sput(p, ") -- swallowed");
                logline(b); p = b;
                cont = DBG_CONTINUE;
                break;
            }
            attach_bp_pending = 0;
            /* ⚠ A DEBUGGER-ONLY EXCEPTION. NtClose on a bad handle raises
               STATUS_INVALID_HANDLE in user mode ONLY when a debugger is attached;
               without one it is a silent failure. Passed through as NOT_HANDLED it
               reached the host's VEH, which is fatal on anything unrecognised once in
               PM -- so the instrument killed Doom at the loader screen (s72, run 7).
               Swallow it and count it: the count is a free measurement of how often
               the host closes a handle it does not own. */
            if (de.u.Exception.ExceptionRecord.ExceptionCode == 0xC0000008) {
                ++nbadclose;
                if (nbadclose <= 20) {
                    p = sput(p, "bad CloseHandle #"); p = sputu(p, nbadclose);
                    p = sput(p, " (STATUS_INVALID_HANDLE) tid="); p = sputu(p, de.dwThreadId);
                    p = sput(p, " -- continued, invisible without a debugger");
                    logline(b); p = b;
                }
                cont = DBG_CONTINUE;
                break;
            }
            dump_exception(hp, de.dwThreadId, &de.u.Exception);
            if (!core_written && hp) { core_written = 1; dump_core(hp); }
            cont = DBG_EXCEPTION_NOT_HANDLED;   /* exactly what happens with no debugger present */
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            p = sput(p, "*** EXIT pid="); p = sputu(p, pid);
            p = sput(p, " code=0x"); p = sputx(p, de.u.ExitProcess.dwExitCode);
            p = sput(p, " ("); p = sput(p, exc_name(de.u.ExitProcess.dwExitCode)); p = sput(p, ")");
            p = sput(p, " exceptions seen="); p = sputu(p, nexc);
            p = sput(p, " bad CloseHandle="); p = sputu(p, nbadclose);
            logline(b); p = b;
            done = 1;
            cont = DBG_CONTINUE;
            break;
        default:
            cont = DBG_CONTINUE;
            break;
        }
        ContinueDebugEvent(de.dwProcessId, de.dwThreadId, cont);
    }
    logline("detached; waiting for the next one");
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    static char b[256]; char *p = b;
    static char name[64];
    const char *c = cmdline;
    DWORD me = GetCurrentProcessId();
    (void)inst; (void)prev; (void)show;

    g_con = GetStdHandle(STD_OUTPUT_HANDLE);

    /* argv[1] by hand: skip the (possibly quoted) program token, take the next word. */
    if (*c == '"') { ++c; while (*c && *c != '"') ++c; if (*c) ++c; }
    else while (*c && *c != ' ') ++c;
    while (*c == ' ') ++c;
    if (*c) { int n = 0; while (*c && *c != ' ' && n < 63) name[n++] = *c++; name[n] = 0; }
    else sput(name, "ntvdmhost.exe");

    p = sput(p, "vdmwatch: waiting for "); p = sput(p, name); p = sput(p, " (leave this window open; log: " LOGF ")");
    logline(b); p = b;

    for (;;) {
        DWORD pid = find_pid(name, me);
        if (!pid) { Sleep(300); continue; }
        watch(pid);
        /* The pid is gone; if the same number comes straight back it is a new process. */
        Sleep(500);
    }
    return 0;
}
