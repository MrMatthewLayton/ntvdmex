/* host_core.c -- the host's foundations: its folder and paths, the OS imports bound at run time,
 *   the host lock, guest-memory peeks and pokes, and small utilities.
 *
 * Part of the host's single translation unit: #included by main.c after host_internal.h. */

static PFN_ATTACH_CONSOLE           g_PfnAttachConsole;
static VOID OsCompatBind(VOID)
{
    HMODULE kernel32 = GetModuleHandleA(HOST_MODULE_KERNEL32), user32 = GetModuleHandleA(HOST_MODULE_USER32);
    g_OsVersion = GetVersion();
    g_PfnAddVeh        = (PFN_ADD_VECTORED_EXCEPTION_HANDLER)(ULONG_PTR)GetProcAddress(kernel32, HOST_EXPORT_ADD_VECTORED_EXCEPTION_HANDLER);
    g_PfnAttachConsole = (PFN_ATTACH_CONSOLE)(ULONG_PTR)GetProcAddress(kernel32, HOST_EXPORT_ATTACH_CONSOLE);
    g_PfnRegisterRawInput   = user32 ? (PFN_REGISTER_RAW_INPUT_DEVICES)(ULONG_PTR)GetProcAddress(user32, HOST_EXPORT_REGISTER_RAW_INPUT_DEVICES) : 0;
    g_PfnGetRawInput   = user32 ? (PFN_GET_RAW_INPUT_DATA)(ULONG_PTR)GetProcAddress(user32, HOST_EXPORT_GET_RAW_INPUT_DATA) : 0;
}
/* AttachConsole on an OS without it: fail, and let the next route try. */
static BOOL OsCompatAttachConsole(DWORD processId)
{
    return g_PfnAttachConsole ? g_PfnAttachConsole(processId) : FALSE;
}

/* See NTVDMEX_DIR. No CRT here (the host links without one), so the string work is
   spelled out. */
static PCSTR NtvdmexRoot(VOID)
{
    static CHAR root[MAX_PATH + 16];
    static volatile LONG ready;
    if (!ready) {
        CHAR self[MAX_PATH + 16];
        DWORD length = GetModuleFileNameA(NULL, self, sizeof self - 2);
        INT index, last = -1, prev = -1;
        if (length == 0 || length >= sizeof self - 2) {
            for (index = 0; NTVDMEX_DIR_DEFAULT[index]; ++index) root[index] = NTVDMEX_DIR_DEFAULT[index];
            root[index] = 0;
        } else {
            for (index = 0; index < (INT)length; ++index) if (self[index] == '\\') { prev = last; last = index; }
            if (last < 0) { root[0] = '.'; root[1] = '\\'; root[2] = 0; }
            else {
                INT cut = last;                      /* ...\bin\ntvdmhost.exe -> ...\bin */
                /* the exe's directory is "bin" or "bm" (any case) -> the root is its
                   parent. bm is the pre-s73 name; an installed s72 zip still has it. */
                INT directoryLength = last - prev;                /* dir name length + 1 */
                if (prev >= 0 && (self[prev + 1] | ASCII_CASE_BIT) == 'b'
                    && ((directoryLength == 3 && (self[prev + 2] | ASCII_CASE_BIT) == 'm')
                        || (directoryLength == 4 && (self[prev + 2] | ASCII_CASE_BIT) == 'i'
                                    && (self[prev + 3] | ASCII_CASE_BIT) == 'n')))
                    cut = prev;
                for (index = 0; index < cut; ++index) root[index] = self[index];
                root[cut] = '\\'; root[cut + 1] = 0;
            }
        }
        ready = 1;
    }
    return root;
}
/* ── ⛔ THE RING IS PER-THREAD, BECAUSE A SHARED ONE WROTE A LOG LINE INTO A FLAG FILE.
     (s74c) The watchdog thread built WDLOG_PATH here, and before its CreateFile ran the
     other threads had taken 16 more slots -- so the pointer it held now read
     `cfg\pmnoirq.flag` (the PM loop's GetFileAttributes at entry), and the watchdog's
     "started" line CREATED the knob that suppresses every PM IRQ. From that run on,
     Duke3D failed its sound-IRQ test ("Playback failed, possibly due to an invalid or
     conflicting IRQ"), heaven7 and ZAR ran with time stopped, and nothing said why:
     the file's CONTENTS were the only clue. A caller may legitimately hold a few paths
     at once (LOG_PATH inside LogAppend while building another), so keep a small ring,
     but never let another thread's path land in it. */
#define NTVDMEX_PATH_SLOTS 8
#define NTVDMEX_PATH_SLOT  (MAX_PATH + 96)
static volatile LONG g_PathTls = -1;             /* TlsAlloc'd on first use (no __thread: no libgcc) */
static PCSTR NtvdmexPath(PCSTR subdirectory, PCSTR name)
{
    PSTR ring; UINT *next; PSTR slot;
    if (g_PathTls < 0) {
        LONG tlsIndex = (LONG)TlsAlloc();
        if (InterlockedCompareExchange(&g_PathTls, tlsIndex, -1) != -1) TlsFree((DWORD)tlsIndex);
    }
    ring = (PSTR)TlsGetValue((DWORD)g_PathTls);
    if (!ring) {
        ring = (PSTR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                 NTVDMEX_PATH_SLOTS * NTVDMEX_PATH_SLOT + sizeof(UINT));
        if (!ring) return "";                     /* out of memory: an empty path fails loudly downstream */
        TlsSetValue((DWORD)g_PathTls, ring);
    }
    next = (UINT *)(ring + NTVDMEX_PATH_SLOTS * NTVDMEX_PATH_SLOT);
    slot = ring + ((*next)++ & (NTVDMEX_PATH_SLOTS - 1)) * NTVDMEX_PATH_SLOT;
    PCSTR root = NtvdmexRoot();
    INT length = 0, index;
    for (index = 0; root[index] && length < MAX_PATH + 90; ++index) slot[length++] = root[index];
    for (index = 0; subdirectory[index] && length < MAX_PATH + 90; ++index) slot[length++] = subdirectory[index];
    for (index = 0; name[index] && length < MAX_PATH + 94; ++index) slot[length++] = name[index];
    slot[length] = 0;
    return slot;
}
/* Does a newline-separated NAME=VALUE block already set `name` (case-insensitive, at
   the start of a line)? No C runtime here, so no strstr. */
static INT StrStrNoCase(PCSTR block, PCSTR name)
{
    PCSTR line = block;
    while (*line) {
        PCSTR cursor = line, nameCursor = name;
        while (*nameCursor && *cursor && ((*cursor | ASCII_CASE_BIT) == (*nameCursor | ASCII_CASE_BIT) || (*cursor == *nameCursor))) { ++cursor; ++nameCursor; }
        if (!*nameCursor) return 1;
        while (*line && *line != '\n') ++line;
        if (*line) ++line;
    }
    return 0;
}
static CRITICAL_SECTION g_Lock;             /* serialises all bus dispatch       */
static DWORD    g_LockOwner, g_LockDepth;
static LONGLONG g_LockSince;
static UINT32 QpcMicroseconds(LONGLONG ticks)
{
    if (!g_QpcFrequency.QuadPart || ticks <= 0) return 0;
    return (UINT32)((ticks * MICROSECONDS_PER_SECOND) / g_QpcFrequency.QuadPart);
}
/* 64-bit form, for the throttle's windowed wall total (a 32-bit us wraps at 71 min
   and the window can be up to CPUSPEED_MAX_WINDOW_US). d is bounded to one window, so
   d*1e6 stays far inside 64 bits. */
static UINT64 QpcMicroseconds64(LONGLONG ticks)
{
    if (!g_QpcFrequency.QuadPart || ticks <= 0) return 0ull;
    return (UINT64)((ticks * MICROSECONDS_PER_SECOND_LL) / g_QpcFrequency.QuadPart);
}

static VOID HostLockEnter(INT site)
{
    LARGE_INTEGER waitStart, acquired;
    DWORD threadId = GetCurrentThreadId();
    INT nested = (g_LockOwner == threadId && g_LockDepth != 0);
    QueryPerformanceCounter(&waitStart);
    EnterCriticalSection(&g_Lock);
    if (!nested) {
        UINT32 waitMicroseconds;
        QueryPerformanceCounter(&acquired);
        waitMicroseconds = QpcMicroseconds(acquired.QuadPart - waitStart.QuadPart);
        if (waitMicroseconds > g_LockWaitMicroseconds) { g_LockWaitMicroseconds = waitMicroseconds; g_LockWaitSite = site; }
        g_LockOwner = threadId; g_LockSince = acquired.QuadPart; g_LockSite = site; g_LockDepth = 0;
    }
    g_LockDepth++;
}
enum { PATCH_MAP_HASH_SHIFT = 8, PATCH_MAP_HEADROOM = 16 };   /* PatchMapHash / PatchMapSet */
static VOID HostLockLeave(VOID)
{
    if (g_LockDepth && --g_LockDepth == 0) {
        LARGE_INTEGER now;
        UINT32 holdMicroseconds;
        QueryPerformanceCounter(&now);
        holdMicroseconds = QpcMicroseconds(now.QuadPart - g_LockSince);
        if (holdMicroseconds > g_LockHoldMicroseconds) { g_LockHoldMicroseconds = holdMicroseconds; g_LockHoldSite = g_LockSite; }
        g_LockOwner = 0;                 /* clear BEFORE releasing: the next owner
                                           must not see us as the holder */
    }
    LeaveCriticalSection(&g_Lock);
}
/* ── A NON-BLOCKING ACQUIRE, for a caller that would rather SKIP than QUEUE. ──────
     The tick generator uses this for its delivery attempts: an attempt needs g_Lock
     (the never-suspend-a-lock-holder interlock), but the CLOCK must never stand in
     line behind the renderer to make one -- a skipped attempt costs nothing, because
     the tick is already latched and the cooperative path delivers it at the guest's
     next trap. Bookkeeping matches HostLockEnter minus the wait tracking, which is
     meaningless here: a try never waits. */
static INT HostLockTry(INT site)
{
    DWORD threadId = GetCurrentThreadId();
    INT nested = (g_LockOwner == threadId && g_LockDepth != 0);
    if (!TryEnterCriticalSection(&g_Lock)) return 0;
    if (!nested) {
        LARGE_INTEGER acquired; QueryPerformanceCounter(&acquired);
        g_LockOwner = threadId; g_LockSince = acquired.QuadPart; g_LockSite = site; g_LockDepth = 0;
    }
    g_LockDepth++;
    return 1;
}
#define DPMI_PMAP_MASK  (DPMI_PMAP_SLOTS - 1u)
static DWORD g_PatchMapCount;

static DWORD PatchMapHash(DWORD linear) { return ((linear * KNUTH_HASH_MULTIPLIER_U) >> PATCH_MAP_HASH_SHIFT) & DPMI_PMAP_MASK; }

static BYTE PatchMapGet(DWORD linear)
{
    DWORD start = PatchMapHash(linear), probe;
    if (!linear) return 0;
    for (probe = 0; probe < DPMI_PMAP_SLOTS; ++probe) {
        DWORD slot = (start + probe) & DPMI_PMAP_MASK;
        if (!g_PatchMapLinear[slot]) return 0;
        if (g_PatchMapLinear[slot] == linear) return g_PatchMapVector[slot];
    }
    return 0;
}

static VOID PatchMapSet(DWORD linear, BYTE vector)
{
    DWORD start = PatchMapHash(linear), probe;
    if (!linear || g_PatchMapCount >= DPMI_PMAP_SLOTS - PATCH_MAP_HEADROOM) return;   /* leave headroom, never fill */
    for (probe = 0; probe < DPMI_PMAP_SLOTS; ++probe) {
        DWORD slot = (start + probe) & DPMI_PMAP_MASK;
        if (!g_PatchMapLinear[slot]) { g_PatchMapLinear[slot] = linear; g_PatchMapVector[slot] = vector; ++g_PatchMapCount; return; }
        if (g_PatchMapLinear[slot] == linear) { g_PatchMapVector[slot] = vector; return; }
    }
}

/* Clearing leaves the key in place with vec=0: a tombstone, so probe chains that ran
   through this slot still find what is past it. Slots are never reused, which is fine
   at these counts and is the whole reason for the headroom check above. */
static VOID PatchMapClear(DWORD linear)
{
    DWORD start = PatchMapHash(linear), probe;
    for (probe = 0; probe < DPMI_PMAP_SLOTS; ++probe) {
        DWORD slot = (start + probe) & DPMI_PMAP_MASK;
        if (!g_PatchMapLinear[slot]) return;
        if (g_PatchMapLinear[slot] == linear) { g_PatchMapVector[slot] = 0; return; }
    }
}
/* Is [addr, addr+len) committed and readable RIGHT NOW? For probes that dereference
   an address derived from one guest's memory map: under that guest the page is there,
   under every other guest it is not, and an unguarded read takes the whole host down
   with an access violation. Ask, don't assume -- and don't reach for SEH to paper over
   it, because a fault we swallow is a fault we stop seeing. */
static INT MemoryReadable(ULONG_PTR address, SIZE_T length)
{
    MEMORY_BASIC_INFORMATION memoryInfo;
    if (VirtualQuery((LPCVOID)address, &memoryInfo, sizeof memoryInfo) != sizeof memoryInfo) return 0;
    if (memoryInfo.State != MEM_COMMIT) return 0;
    if (memoryInfo.Protect & PAGE_GUARD) return 0;
    if (!(memoryInfo.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
                      | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                      | PAGE_EXECUTE_WRITECOPY))) return 0;
    /* the region must also COVER the whole span, not merely start inside it */
    return (address + length) <= ((ULONG_PTR)memoryInfo.BaseAddress + memoryInfo.RegionSize);
}

/* Real/synthesised real-mode interrupt dispatch. The guest runs cooperatively
   (we only regain control at event boundaries), so when a hardware IRQ becomes
   pending and the guest's IF is set, we do here exactly what the CPU does on a
   hardware interrupt: push FLAGS/CS/IP, clear IF+TF, and vector CS:IP through the
   real-mode IVT. The guest's handler IRETs back normally. (`CD nn` software ints
   still vector natively via VME; this is only for asynchronous IRQ delivery.) */
static VOID PokeWord(DWORD linear, WORD value)
{ volatile BYTE *memory = (volatile BYTE *)0; memory[linear] = (BYTE)value; memory[linear + 1] = (BYTE)(value >> BYTE_SHIFT); }
static VOID PokeDword(DWORD linear, DWORD value)   /* dword store: 32-bit IRET frame slots (GH #18 run 83) */
{ volatile BYTE *memory = (volatile BYTE *)0;
  memory[linear] = (BYTE)value; memory[linear+1] = (BYTE)(value >> BYTE_SHIFT); memory[linear+2] = (BYTE)(value >> WORD_SHIFT); memory[linear+3] = (BYTE)(value >> TOP_BYTE_SHIFT); }
static WORD PeekWord(DWORD linear)
{ const volatile BYTE *memory = (const volatile BYTE *)0; return (WORD)(memory[linear] | (memory[linear + 1] << BYTE_SHIFT)); }
/* Width-selected guest memory access (1/2/4 bytes) for the string-I/O servicer. */
static DWORD PeekWidth(DWORD linear, INT width)
{ const volatile BYTE *memory = (const volatile BYTE *)0;
  if (width == 1) return memory[linear];
  if (width == X86_WORD_SIZE) return PeekWord(linear);
  return (DWORD)PeekWord(linear) | ((DWORD)PeekWord(linear + X86_WORD_SIZE) << WORD_SHIFT); }
static VOID PokeWidth(DWORD linear, DWORD value, INT width)
{ volatile BYTE *memory = (volatile BYTE *)0;
  if (width == 1) { memory[linear] = (BYTE)value; return; }
  if (width == X86_WORD_SIZE) { PokeWord(linear, (WORD)value); return; }
  PokeDword(linear, value); }

static LONGLONG QpcTicks(UINT32 microseconds)
{ return g_QpcFrequency.QuadPart ? (LONGLONG)((g_QpcFrequency.QuadPart / MILLISECONDS_PER_SECOND) * microseconds / MICROSECONDS_PER_MILLISECOND) : 0; }

static INT StringsEqual(PCSTR first, PCSTR second)
{
    while (*first && *first == *second) { ++first; ++second; }
    return *first == *second;
}

static INT HostReadable(PCVOID pointer, SIZE_T length)
{
    MEMORY_BASIC_INFORMATION memoryInfo;
    ULONG_PTR address = (ULONG_PTR)pointer;
    if (!address || length == 0) return 0;
    if (VirtualQuery((LPCVOID)address, &memoryInfo, sizeof(memoryInfo)) != sizeof(memoryInfo)) return 0;
    if (memoryInfo.State != MEM_COMMIT) return 0;
    if (memoryInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    if (!(memoryInfo.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return 0;
    /* the span must not run off the end of this region into an unmapped one */
    return (address + length) <= ((ULONG_PTR)memoryInfo.BaseAddress + memoryInfo.RegionSize);
}

/* The same question for a destination we are about to WRITE. Same reasoning and the
   same reason not to use IsBadWritePtr -- see the note above. Used before filling a
   guest buffer on the guest's behalf (WOW32 0x97), where getting the selector wrong
   would otherwise scribble on whatever the bad base happened to name. */
static INT HostWritable(PVOID pointer, SIZE_T length)
{
    MEMORY_BASIC_INFORMATION memoryInfo;
    ULONG_PTR address = (ULONG_PTR)pointer;
    if (!address || length == 0) return 0;
    if (VirtualQuery((LPCVOID)address, &memoryInfo, sizeof(memoryInfo)) != sizeof(memoryInfo)) return 0;
    if (memoryInfo.State != MEM_COMMIT) return 0;
    if (memoryInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    if (!(memoryInfo.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return 0;
    return (address + length) <= ((ULONG_PTR)memoryInfo.BaseAddress + memoryInfo.RegionSize);
}
