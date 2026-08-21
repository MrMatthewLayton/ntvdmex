/*
 * controld.c -- NTVDMEX bare-metal CONTROL daemon (session-9).
 *
 * A tiny, robust remote-control channel for the XP test box, independent of the
 * test watcher (runwatch.bat). It NEVER launches guest code, so it can never wedge
 * -- which is exactly why it can recover the test watcher when a run hangs it, and
 * gives a remote reboot so the box needs no physical access.
 *
 * Mechanism: it polls a command file on the SMB share (which the remote driver on
 * the Mac writes) and acts on it:
 *   reboot   -> ExitWindowsEx(EWX_REBOOT | EWX_FORCE)   (needs SeShutdownPrivilege)
 *   poweroff -> ExitWindowsEx(EWX_POWEROFF | EWX_FORCE)
 *   kill     -> taskkill /f /im ntvdmhost.exe   (unwedge a hung test host)
 *   exec CMD -> WinExec(CMD)                    (restart a dead watcher; see below)
 * It writes a heartbeat file every poll so the driver can confirm it's alive, and
 * is a singleton (a named mutex) so repeated launches from rt.bat are no-ops.
 *
 * XP-safe build (msvcrt, subsystem 5.01):
 *   i686-w64-mingw32-gcc -O2 -mwindows -o controld.exe controld.c \
 *     -lkernel32 -luser32 -ladvapi32 \
 *     -Wl,--major-os-version,5 -Wl,--minor-os-version,1 \
 *     -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1
 */
#include <windows.h>

#define SHARE "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex"
#define CTRL  SHARE "\\control.txt"
#define BEAT  SHARE "\\controld.txt"

/* Enable SeShutdownPrivilege in our token so ExitWindowsEx is allowed. */
static void enable_shutdown_priv(void)
{
    HANDLE tok;
    TOKEN_PRIVILEGES tp;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueA(NULL, "SeShutdownPrivilege", &tp.Privileges[0].Luid))
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
    CloseHandle(tok);
}

/* Read the command file into buf (NUL-terminated). Returns byte count, 0 if absent. */
static int read_cmd(char *buf, int cap)
{
    DWORD n = 0;
    HANDLE h = CreateFileA(CTRL, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ReadFile(h, buf, (DWORD)(cap - 1), &n, NULL);
    CloseHandle(h);
    buf[n] = 0;
    return (int)n;
}

static void write_beat(const char *s)
{
    DWORD wr;
    HANDLE h = CreateFileA(BEAT, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { WriteFile(h, s, lstrlenA(s), &wr, NULL); CloseHandle(h); }
}

/* case-insensitive "does s begin with p" */
static int starts_with(const char *s, const char *p)
{
    while (*p) {
        char a = *s++, b = *p++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    char buf[256];
    (void)inst; (void)prev; (void)cmd; (void)show;

    /* singleton: exit quietly if another controld is already running */
    CreateMutexA(NULL, FALSE, "ntvdmex_controld_singleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    enable_shutdown_priv();
    write_beat("controld: started\r\n");

    for (;;) {
        if (read_cmd(buf, (int)sizeof buf) > 0) {
            DeleteFileA(CTRL);                       /* consume: one-shot */
            if (starts_with(buf, "reboot")) {
                char m[96];
                /* Primary: shutdown.exe (handles the privilege path itself, the standard
                   XP reboot). Belt-and-suspenders: ExitWindowsEx too, reporting its error
                   so we can diagnose if the box still won't go (v1's ExitWindowsEx alone
                   reached here but never rebooted -- session-9). */
                write_beat("controld: REBOOT (shutdown.exe -r -f)\r\n");
                WinExec("shutdown.exe -r -f -t 00", SW_HIDE);
                if (!ExitWindowsEx(EWX_REBOOT | EWX_FORCE, SHTDN_REASON_MAJOR_OTHER)) {
                    wsprintfA(m, "controld: ExitWindowsEx err=%lu (shutdown.exe should still fire)\r\n",
                              GetLastError());
                    write_beat(m);
                }
            } else if (starts_with(buf, "poweroff")) {
                write_beat("controld: POWEROFF\r\n");
                WinExec("shutdown.exe -s -f -t 00", SW_HIDE);
                ExitWindowsEx(EWX_POWEROFF | EWX_FORCE, SHTDN_REASON_MAJOR_OTHER);
            } else if (starts_with(buf, "kill")) {
                write_beat("controld: kill ntvdmhost\r\n");
                WinExec("taskkill /f /im ntvdmhost.exe", SW_HIDE);
            } else if (starts_with(buf, "exec ")) {
                /* GENERIC EXEC -- the lever that makes a dead watcher recoverable
                   remotely. Session 16: runwatch.bat's Startup copy had LF-only line
                   endings, so cmd.exe could not resolve `goto loop` and the watcher
                   died one iteration in. controld was alive, but reboot/kill/quit
                   could not RESTART anything, so the only repair was physical access
                   to the box. `exec cmd /c "...\runwatch.bat"` closes that hole.
                   WinExec (not CreateProcess) deliberately: it is what the rest of
                   this file already uses, it returns immediately, and the child
                   outlives us -- which matters because runwatch.bat's first act is
                   to taskkill controld and start a fresh one. */
                char m[192];
                char *arg = buf + 5;
                char *p;
                while (*arg == ' ' || *arg == '\t') arg++;
                for (p = arg; *p; p++)                 /* trim the CR/LF the share adds */
                    if (*p == '\r' || *p == '\n') { *p = 0; break; }
                wsprintfA(m, "controld: exec [%s]\r\n", arg);
                write_beat(m);
                if (*arg) WinExec(arg, SW_SHOWNORMAL);
            } else if (starts_with(buf, "quit")) {
                /* let the driver stop controld remotely so a new build can be hot-swapped:
                   the singleton mutex is released on exit, so a fresh start takes over. */
                write_beat("controld: quit\r\n");
                return 0;
            } else {
                write_beat("controld: unknown cmd\r\n");
            }
        } else {
            write_beat("controld: idle\r\n");
        }
        Sleep(1500);
    }
    return 0;
}
